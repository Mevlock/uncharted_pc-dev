/* libScePad on SDL3 gamepads, keyboard and mouse. SDL events are pumped by the window thread
 * (gpu/shim/window.cpp); here state is only sampled. The controller, keyboard and mouse work
 * together; their bindings come from input.ini (BB_INPUT_CONFIG, written by the launcher):
 *
 *   key.<action>=<key or mouse name>[|<second>]   SDL key names ("Space", "Left Shift", "W"),
 *       "Mouse Left", "Mouse Right", "Mouse Middle", "Mouse 4", "Mouse 5", "Wheel Up",
 *       "Wheel Down"; actions: cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad
 *       up down left right, ls_up ls_down ls_left ls_right, rs_up rs_down rs_left rs_right.
 *   pad.<action>=<controller button>   SDL names (a b x y leftshoulder rightshoulder
 *       lefttrigger righttrigger leftstick rightstick back start dpup dpdown dpleft dpright
 *       touchpad misc1 paddle1..paddle4) or "none", for the button actions above.
 *   mouse_look=1  mouse_sensitivity=1.0  mouse_invert_y=0  pad_invert_y=0
 *
 * Defaults (no file): WASD move, mouse look, left button R2 (fire), right button L2 (aim),
 * Space Cross, C Circle, F Square, E Triangle, Q L1, G R1, Left Shift L3, V R3, Escape/Enter
 * Options, Tab touchpad, arrow keys and the wheel the d-pad, IJKL the right stick. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened, sdl_ready;
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;

uint64_t runtime_guest_monotonic_us(void);
static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
static uint8_t axis(int16_t v) { int x=(v+32768)>>8; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}
static void touch_click(PadData *d, int right) {
    d->buttons|=BTN_TOUCHPAD;
    d->touch_count=1;
    d->touches[0]=(PadTouch){.x=right ? 1440 : 480,.y=471,.id=0};
}

/* Opens the first gamepad SDL knows about; called under lock. */
/* The keyboard and mouse were used last (the prompts' device 0): the controller doesn't rumble
 * (antikmewzik's controls fork). */
static int keyboard_in_use;
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    if (!gamepad) {
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) {
            gamepad=SDL_OpenGamepad(ids[0]);
            if (gamepad) { ++connected_count; printf("Runtime: gamepad connected: %s\n",SDL_GetGamepadName(gamepad)); }
        }
        SDL_free(ids);
    }
    return gamepad;
}
/* Degrees of camera turn per mouse pixel at sensitivity 1 (0.08 felt "very slow", issue #18; the
 * launcher's slider and box go from 0.1 to 10 times this). */
#define MOUSE_DEGREES_PER_PIXEL 0.15f

/* ---- bindings (input.ini) ---------------------------------------------------------------- */
/* BIND_KEY: a physical key (SDL scancode, US position). BIND_LAYOUT: a character key the player
 * chose, found in the current keyboard layout (SDL keycode), so "Z" is the key labelled Z on an
 * AZERTY keyboard too. The default letters stay positions: WASD is ZQSD on AZERTY. */
enum { BIND_KEY=1, BIND_MOUSE, BIND_WHEEL, BIND_LAYOUT };
typedef struct { uint8_t kind; int32_t code; } Bind;
typedef struct { const char *name; uint32_t ps; const char *keys; const char *pad; } Action;
enum { A_LS_UP=16, A_LS_DOWN, A_LS_LEFT, A_LS_RIGHT, A_RS_UP, A_RS_DOWN, A_RS_LEFT, A_RS_RIGHT, ACTIONS };
static const Action actions[ACTIONS]={
    {"cross",BTN_CROSS,"Space","a"}, {"circle",BTN_CIRCLE,"C","b"}, {"square",BTN_SQUARE,"F","x"},
    {"triangle",BTN_TRIANGLE,"E","y"}, {"l1",BTN_L1,"Q","leftshoulder"}, {"r1",BTN_R1,"G","rightshoulder"},
    {"l2",BTN_L2,"Mouse Right","lefttrigger"}, {"r2",BTN_R2,"Mouse Left","righttrigger"},
    {"l3",BTN_L3,"Left Shift","leftstick"}, {"r3",BTN_R3,"V","rightstick"},
    {"options",BTN_OPTIONS,"Escape|Return","start"}, {"touchpad",BTN_TOUCHPAD,"Tab","back"},
    {"up",BTN_UP,"Up","dpup"}, {"down",BTN_DOWN,"Down","dpdown"},
    {"left",BTN_LEFT,"Left|Wheel Up","dpleft"}, {"right",BTN_RIGHT,"Right|Wheel Down","dpright"},
    {"ls_up",0,"W",NULL}, {"ls_down",0,"S",NULL}, {"ls_left",0,"A",NULL}, {"ls_right",0,"D",NULL},
    {"rs_up",0,"I",NULL}, {"rs_down",0,"K",NULL}, {"rs_left",0,"J",NULL}, {"rs_right",0,"L",NULL},
};
#define PAD_TRIGGER_LEFT (-2)
#define PAD_TRIGGER_RIGHT (-3)
static struct {
    Bind keys[ACTIONS][2];
    int pad[16];               /* SDL_GamepadButton, PAD_TRIGGER_*, or -1 */
    int mouse_look, mouse_invert_y, pad_invert_y;
    float mouse_sensitivity;
} input;

/* Whether `name` is one of the keys in `defaults` ("Escape|Return"). */
static int default_key(const char *name, const char *defaults) {
    const size_t length=strlen(name);
    for (const char *d=defaults; d && *d; ) {
        const char *end=strchr(d,'|');
        const size_t n=end ? (size_t)(end-d) : strlen(d);
        if (n==length && !SDL_strncasecmp(d,name,n)) return 1;
        d=end ? end+1 : NULL;
    }
    return 0;
}
static Bind parse_bind(const char *name, const char *defaults) {
    static const struct { const char *name; uint8_t kind; int16_t code; } mouse[]={
        {"Mouse Left",BIND_MOUSE,1}, {"Mouse Right",BIND_MOUSE,2}, {"Mouse Middle",BIND_MOUSE,4},
        {"Mouse 4",BIND_MOUSE,8}, {"Mouse 5",BIND_MOUSE,16}, {"Wheel Up",BIND_WHEEL,1}, {"Wheel Down",BIND_WHEEL,-1},
    };
    for (size_t i=0;i<sizeof(mouse)/sizeof(*mouse);++i)
        if (!SDL_strcasecmp(name,mouse[i].name)) return (Bind){mouse[i].kind,mouse[i].code};
    if (name[0]>' ' && name[0]<127 && !name[1] && !default_key(name,defaults)) {
        const SDL_Keycode key=SDL_GetKeyFromName(name);
        if (key!=SDLK_UNKNOWN) return (Bind){BIND_LAYOUT,(int32_t)key};
    }
    const SDL_Scancode code=SDL_GetScancodeFromName(name);
    return code==SDL_SCANCODE_UNKNOWN ? (Bind){0,0} : (Bind){BIND_KEY,(int32_t)code};
}
static void set_keys(int action, const char *value) {
    char text[128];
    snprintf(text,sizeof(text),"%s",value);
    input.keys[action][0]=input.keys[action][1]=(Bind){0,0};
    char *second=strchr(text,'|');
    if (second) *second++=0;
    if (*text) input.keys[action][0]=parse_bind(text,actions[action].keys);
    if (second && *second) input.keys[action][1]=parse_bind(second,actions[action].keys);
}
static int parse_pad(const char *value) {
    if (!SDL_strcasecmp(value,"lefttrigger")) return PAD_TRIGGER_LEFT;
    if (!SDL_strcasecmp(value,"righttrigger")) return PAD_TRIGGER_RIGHT;
    const SDL_GamepadButton button=SDL_GetGamepadButtonFromString(value);
    return button==SDL_GAMEPAD_BUTTON_INVALID ? -1 : (int)button;
}
static void load_input(void) {
    for (int a=0;a<ACTIONS;++a) {
        set_keys(a,actions[a].keys);
        if (a<16) input.pad[a]=parse_pad(actions[a].pad);
    }
    input.mouse_look=1; input.mouse_sensitivity=1.0f;
    const char *path=getenv("BB_INPUT_CONFIG");
    FILE *f=path && *path ? fopen(path,"r") : NULL;
    if (f) {
        char line[256];
        while (fgets(line,sizeof(line),f)) {
            line[strcspn(line,"\r\n")]=0;
            char *eq=strchr(line,'=');
            if (line[0]=='#' || !eq) continue;
            *eq=0;
            const char *key=line, *value=eq+1;
            if (!strcmp(key,"mouse_look")) input.mouse_look=atoi(value)!=0;
            else if (!strcmp(key,"mouse_sensitivity")) input.mouse_sensitivity=(float)atof(value);
            else if (!strcmp(key,"mouse_invert_y")) input.mouse_invert_y=atoi(value)!=0;
            else if (!strcmp(key,"pad_invert_y")) input.pad_invert_y=atoi(value)!=0;
            else for (int a=0;a<ACTIONS;++a) {
                if (!strncmp(key,"key.",4) && !strcmp(key+4,actions[a].name)) set_keys(a,value);
                else if (a<16 && !strncmp(key,"pad.",4) && !strcmp(key+4,actions[a].name)) input.pad[a]=parse_pad(value);
            }
        }
        fclose(f);
        printf("Runtime: controls from %s (mouse look %s, sensitivity %.2f)\n",path,
               input.mouse_look ? "on" : "off",input.mouse_sensitivity);
    }
    if (input.mouse_sensitivity<=0.0f) input.mouse_sensitivity=1.0f;
    bbgpu_mouse_configure(input.mouse_look);
}

/* Mouse motion as a right stick: the filtered speed (pixels per second) scaled to a deflection,
 * past the game's dead zone as soon as the mouse moves. */
static float mouse_vx, mouse_vy;
static uint64_t mouse_last_us;
static uint64_t wheel_until_us[2]; /* wheel up, wheel down: held briefly for the game to see */
/* A turn rate (degrees/s) as a stick byte, through the inverse of the U1 follow camera's response
 * measured below its speed ramp: about 240 x^2.5 degrees/s (x = 0.25: 7, 0.5: 37). */
static uint8_t mouse_axis(float rate) {
    const float r=rate<0 ? -rate : rate;
    if (r<0.5f) return 128;
    float out=SDL_powf(r/240.0f,0.4f);
    if (out>1.0f) out=1.0f;
    int x=(int)(128.0f+(rate<0 ? -out : out)*127.5f);
    return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x);
}
static int bind_down(const Bind *b, const bool *k, uint32_t mouse, uint64_t now) {
    switch (b->kind) {
    case BIND_KEY: return k && k[b->code];
    case BIND_LAYOUT: {
        const SDL_Scancode code=k ? SDL_GetScancodeFromKey((SDL_Keycode)b->code,NULL) : SDL_SCANCODE_UNKNOWN;
        return code!=SDL_SCANCODE_UNKNOWN && k[code];
    }
    case BIND_MOUSE: return (mouse & (uint32_t)b->code)!=0;
    case BIND_WHEEL: return now<wheel_until_us[b->code>0 ? 0 : 1];
    default: return 0;
    }
}
/* Once the keyboard is up: where the layout put each character key the player chose. */
static void log_layout_keys(void) {
    for (int a=0;a<ACTIONS;++a) for (int i=0;i<2;++i) {
        const Bind *b=&input.keys[a][i];
        if (b->kind!=BIND_LAYOUT) continue;
        const SDL_Scancode code=SDL_GetScancodeFromKey((SDL_Keycode)b->code,NULL);
        printf("Runtime: key.%s %s: the key labelled so in this keyboard layout (US position %s)\n",
               actions[a].name,SDL_GetKeyName((SDL_Keycode)b->code),
               code==SDL_SCANCODE_UNKNOWN ? "none" : SDL_GetScancodeName(code));
    }
}
static int action_down(int a, const bool *k, uint32_t mouse, uint64_t now) {
    return bind_down(&input.keys[a][0],k,mouse,now) || bind_down(&input.keys[a][1],k,mouse,now);
}

/* ---- mouse camera ------------------------------------------------------------------------
 * The games turn the camera from the right stick through their own curve, a speed ramp and a
 * top speed (U1: about 240 degrees/s, 45 while aiming), which no stick position can make feel like
 * a mouse. Where the camera's state is known, the mouse turns it directly instead: a callback at
 * the start of the camera's per-frame update (installed by the loader when the code matches)
 * rotates its direction by the mouse motion since the last frame. Elsewhere the mouse still
 * moves the right stick. */
static pthread_mutex_t camera_lock=PTHREAD_MUTEX_INITIALIZER;
/* Mouse motion (pixels) not yet given to the follow camera [0] and to the joypad state [1]: each
 * gets all of it (the follow camera does not read the joypad's right stick values, the other
 * cameras do not use the follow camera's direction). */
static float camera_dx[3], camera_dy[3]; /* [2]: the aiming camera (raw aim, EU) */
/* Raw aim: mouse pixels not yet given to the aim direction, and the last
 * time the default aim controller ran (the mouse then turns its direction, not the stick). */
static float aim_dx, aim_dy;
static volatile uint64_t aim_direct_us;
/* When aim was pressed. After a roll the game starts the aiming camera's controller up to
 * a second later: the mouse motion of that time waits for it (1.5 s at most) instead of being lost. */
static volatile uint64_t aim_press_us;
static uint64_t camera_hook_us;       /* last follow camera callback: it turns with the mouse */
static uint64_t cover_hook_us;        /* last cover camera callback: it turns with the mouse too */
static uint64_t sticks_hook_us;       /* last joypad callback: the mouse is a right stick value there */
static float camera_test_rate=-1.0f;  /* BB_MOUSE_CAMERA_TEST=<degrees per frame>: constant turn */
static int mouse_debug(void) {
    static int on=-1;
    if (on<0) on=getenv("BB_MOUSE_DEBUG")!=NULL;
    return on;
}
static int camera_take(int queue, float *dx, float *dy) {
    pthread_mutex_lock(&camera_lock);
    *dx=camera_dx[queue]; *dy=camera_dy[queue];
    camera_dx[queue]=camera_dy[queue]=0;
    pthread_mutex_unlock(&camera_lock);
    if (camera_test_rate<0.0f) {
        const char *test=getenv("BB_MOUSE_CAMERA_TEST");
        camera_test_rate=test ? (float)atof(test) : 0.0f;
    }
    return 1;
}
/* Paced mouse (BB_MOUSE_PACE_MS, default 50; 0: off). The game reads the pad once a frame but at a
 * point that drifts against its camera jobs, so "the movement since the last take" alternated
 * between about one and two reads' worth: a steady mouse turned the shown camera 0.15-0.85 degrees
 * per frame in a beat (measured 2026-10-24 with a scripted turn, BB_CAMERA_LOG + BB_PAD_TIMING).
 * A paced take gives the mouse's rate over the last window (window thread, event times) times the
 * camera's usual update period; the queue's own total still says whether this camera gets the
 * mouse, and 10% of the difference is fed back each take, so the angle turned stays exact. */
static volatile float scripted_rate[2]; /* scripted mx= my= (pixels/s): the rate in place of the window's */
static float mouse_pace_seconds(void) {
    static float v=-1.0f;
    if (v<0.0f) { const char *e=getenv("BB_MOUSE_PACE_MS"); v=(e && *e ? (float)atof(e) : 50.0f)*0.001f; if (v<0.0f) v=0.0f; }
    return v;
}
static void camera_take_paced(int queue, float *dx, float *dy, float *period_out) {
    static uint64_t last_us[4], active_us[4];
    static float period[4], debt_x[4], debt_y[4];
    float ax, ay;
    camera_take(queue,&ax,&ay);
    *dx=ax; *dy=ay;
    const float tau=mouse_pace_seconds();
    const uint64_t now=now_us();
    const float wall=last_us[queue] ? (float)(now-last_us[queue])*1e-6f : 0.0f;
    last_us[queue]=now;
    if (period_out) *period_out=wall>0.0005f && wall<0.05f ? wall : 1.0f/60.0f;
    if (tau<=0.0f || queue<0 || queue>=4) return;
    if (!(wall>0.0005f) || wall>0.1f) { debt_x[queue]=debt_y[queue]=0.0f; return; } /* a pause: as it is */
    period[queue]=period[queue]>0.0f ? period[queue]+(wall-period[queue])*0.02f : wall; /* ~1 s average */
    if (period_out) *period_out=period[queue];
    if (ax!=0.0f || ay!=0.0f) active_us[queue]=now;
    if (now-active_us[queue]>100000) { /* this camera has no mouse now: what is left, at once */
        *dx=ax+debt_x[queue]; *dy=ay+debt_y[queue]; debt_x[queue]=debt_y[queue]=0.0f;
        return;
    }
    float rx, ry;
    if (scripted_rate[0]!=0.0f || scripted_rate[1]!=0.0f) { rx=scripted_rate[0]; ry=scripted_rate[1]; }
    else bbgpu_mouse_rate(tau,&rx,&ry);
    float ox=rx*period[queue], oy=ry*period[queue];
    debt_x[queue]+=ax-ox; debt_y[queue]+=ay-oy;
    ox+=debt_x[queue]*0.1f; oy+=debt_y[queue]*0.1f;
    debt_x[queue]*=0.9f; debt_y[queue]*=0.9f;
    *dx=ox; *dy=oy;
}
/* The follow camera's collision/position update runs once a frame with the camera object (U1
 * CUSA02343 1.00 and others with the same code; U2 and U3 have the same function, found by its
 * instructions): its horizontal direction (x, y, z) at +0x1310 (U2 +0x9ec0, U3 +0x78c0) and the
 * vertical arc position at +0x12bc (U2 +0x9e60, U3 +0x7874: the right stick up lowers it). */
static float wrap_degrees(float a) {
    while (a>180.0f) a-=360.0f;
    while (a<-180.0f) a+=360.0f;
    return a;
}
static float heading_of(const float *dir) { return SDL_atan2f(-dir[0],-dir[2])*57.29578f; }
/* The follow camera keeps the turns of its direction while the right stick is held: with the stick
 * at rest the game sets the direction itself every few frames and most turns were lost (an uneven,
 * short turn). So while the mouse turns the direction, the joypad callback holds the stick a little
 * (0.3, about 13 degrees/s) the same way, and the mouse moves a wanted heading: each frame the
 * direction turns by the mouse's motion plus what the direction is still short of it (read from the
 * direction itself, which also takes out the stick's own turn; the shown camera follows the
 * direction with the game's smoothing, 1 to 8 frames late, too late to steer by). While the mouse
 * rests, the wanted heading follows the direction (the game's own camera moves stay free). Should
 * the camera keep no turns at all, the mouse goes to the joypad's right stick (the follow camera's
 * curve), with a small turn each frame to notice when turns are kept again. */
static volatile int follow_direct=1;
static float follow_kept=1.0f;            /* average fraction of a turn still there the next frame */
static volatile int aim_held;             /* L2 is held: the aiming camera (also while it blends in) */
static volatile int aim_stick_fallback; /* raw aim: the aiming camera puts its direction back (U1): the stick turns it */
static int raw_aim_armed(uint64_t now) {
    return aim_held && aim_direct_us && (now-aim_direct_us<1000000 || now-aim_press_us<1500000);
}
/* While the game blends (a roll, cover, a cut) it runs two follow cameras, a new one each time:
 * each keeps its own wanted heading and turn check (shared state made them pull against each other
 * and switch the mouse to the joypad), and every camera updated in a frame gets the frame's mouse
 * motion, so they turn together. */
typedef struct {
    uint64_t camera, last_us;
    uint64_t moved_us;  /* last frame the mouse moved */
    float target;       /* wanted heading (degrees) */
    float before, after; /* heading before and after the last turn */
    float shown, shown_v; /* where the game's smoothed camera is, estimated (degrees, degrees/s) */
    uint64_t shown_us;
    int pending;
} FollowState;
static FollowState follow_states[4];
static FollowState *follow_state(uint64_t camera, uint64_t now) {
    FollowState *slot=&follow_states[0];
    for (int i=0;i<4;++i) {
        if (follow_states[i].camera==camera) { slot=&follow_states[i]; slot->last_us=now; return slot; }
        if (follow_states[i].last_us<slot->last_us) slot=&follow_states[i];
    }
    *slot=(FollowState){.camera=camera,.last_us=now};
    return slot;
}
/* Every camera updated in the same frame (within 4 ms) gets the frame's mouse motion: the follow
 * cameras of a blend, and the cover camera blending with them. */
static void frame_motion(uint64_t now, float *dx, float *dy) {
    static uint64_t frame_take_us;
    static float frame_dx, frame_dy;
    if (now-frame_take_us>4000) {
        camera_take(0,&frame_dx,&frame_dy);
        frame_take_us=now;
    }
    *dx=frame_dx; *dy=frame_dy;
}
static volatile float follow_correction;  /* degrees the direction is short of the wanted heading */
static volatile float follow_push;        /* the right stick while the direction turns (-0.3, 0, 0.3) */
static volatile int follow_has_arc;       /* the follow camera's vertical arc is known */
/* arc_degrees: the camera pitch one unit of arc covers (U1 61; measured in U2 about 135, in U3 about 105, steeper upwards) */
static void follow_camera(uint64_t camera, uint32_t dir_offset, uint32_t arc_offset, float arc_degrees, float arc_max) {
    const uint64_t now=now_us();
    follow_has_arc=arc_offset!=0;
    if (camera_hook_us && now-camera_hook_us>100000) {
        float ignore_x, ignore_y;
        camera_take(0,&ignore_x,&ignore_y);
        for (int i=0;i<4;++i) follow_states[i].moved_us=0;
    }
    camera_hook_us=now;
    float *dir=(float *)(uintptr_t)(camera+dir_offset);
    const float heading=heading_of(dir);
    FollowState *st=follow_state(camera,now);
    float dx, dy;
    frame_motion(now,&dx,&dy);
    if (aim_held) { st->moved_us=0; st->pending=0; return; } /* the aiming camera takes the mouse */
    /* The shown camera follows the camera's position with the game's smoothing, a spring (fitted to a
     * U3 recording: critically damped, w about 24/s, plus about 50 ms of render delay). It moves the
     * position in a straight line, so a direction far ahead cut through the orbit and swung the
     * camera up over Drake (looking straight down). The direction stays within 60 degrees of the
     * spring (the camera dips about 4 degrees at most); faster turns than about 700 degrees/s
     * follow a few frames later through the wanted heading. */
    {
        float lag_dt=st->shown_us ? (float)(now-st->shown_us)*1e-6f : 0.0f;
        if (!st->shown_us || lag_dt>0.5f) { st->shown=heading; st->shown_v=0.0f; lag_dt=0.0f; }
        const float w=24.0f;
        const int steps=1+(int)(lag_dt/0.004f);
        for (int i=0;i<steps;++i) {
            const float h=lag_dt/(float)steps;
            st->shown_v+=(w*w*wrap_degrees(heading-st->shown)-2.0f*w*st->shown_v)*h;
            st->shown=wrap_degrees(st->shown+st->shown_v*h);
        }
        st->shown_us=now;
    }
    if (st->pending) {
        float kept=wrap_degrees(heading-st->before)/wrap_degrees(st->after-st->before);
        kept=kept<0.0f ? 0.0f : kept>1.0f ? 1.0f : kept;
        follow_kept+=0.2f*(kept-follow_kept);
        if (follow_kept<0.12f) follow_direct=0;
        else if (follow_kept>0.3f) follow_direct=1;
        st->pending=0;
    }
    float shown=heading; /* the shown camera's yaw (debug output) */
    double age=0.0;
    if (mouse_debug() && (!bbgpu_camera_yaw(&shown,&age) || age>0.25)) shown=heading;
    const float turn=camera_test_rate>0.0f ? camera_test_rate : dx*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
    /* the wanted heading and how far the direction is short of it (degrees still to turn). The
     * camera rests 0.15 s after the mouse stopped, once the direction has reached the wanted heading
     * (fast turns go beyond what the game keeps in a frame) or at the latest after 0.5 s. */
    float correction=wrap_degrees(heading-st->target);
    const uint64_t still=now-st->moved_us;
    const int resting=!st->moved_us || (still>150000 && (SDL_fabsf(correction)<0.5f || still>500000));
    if (turn!=0.0f) st->moved_us=now;
    if (resting) {
        st->target=heading;
        correction=0.0f;
    }
    if (correction>90.0f || correction<-90.0f) { /* at most a quarter turn behind */
        correction=correction>0.0f ? 90.0f : -90.0f;
        st->target=wrap_degrees(heading-correction);
    }
    st->target=wrap_degrees(st->target-turn); /* a positive turn lowers the heading */
    if (!follow_direct) {
        /* the joypad turns the camera (with the shortfall as extra speed); a small probe turn shows
         * whether direct turns are kept again */
        follow_correction=correction;
        follow_push=0.0f;
        if (turn!=0.0f) {
            const float probe=turn>0.0f ? 0.3f : -0.3f;
            const float a=probe*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
            const float x=dir[0], z=dir[2];
            dir[0]=x*c-z*s;
            dir[2]=x*s+z*c;
            st->before=heading;
            st->after=heading_of(dir);
            st->pending=1;
        }
        if (mouse_debug()) printf("Mouse: follow t=%.4f dx=%.1f heading=%.2f shown=%.2f target=%.2f kept=%.2f joypad\n",
                                  now*1e-6,dx,heading,shown,st->target,follow_kept);
        return;
    }
    follow_correction=0.0f;
    /* the shortfall is made up a few degrees a frame (no jump when the game keeps turns again) */
    if (correction<-4.0f) correction=-4.0f;
    if (correction>4.0f) correction=4.0f;
    float total=turn+correction;
    const float lead=wrap_degrees(heading-total-st->shown); /* a positive turn lowers the heading */
    if (lead>60.0f) total+=lead-60.0f;
    else if (lead<-60.0f) total+=lead+60.0f;
    /* the stick is held the way the mouse moves (until 0.1 s after it stops): the stick's own turn
     * is then steady and the correction takes it out evenly. Following the sign of the total flipped
     * the stick every frame on slow moves, and the camera shook by the stick's turn. */
    static float push_sign;
    static uint64_t push_us;
    if (turn!=0.0f) { push_sign=turn>0.0f ? 0.3f : -0.3f; push_us=now; }
    follow_push=push_us && now-push_us<100000 ? push_sign : 0.0f;
    if (mouse_debug()) printf("Mouse: follow t=%.4f dx=%.1f heading=%.2f shown=%.2f target=%.2f turn=%.2f total=%.2f arc=%.4f\n",
                              now*1e-6,dx,heading,shown,st->target,turn,total,
                              arc_offset ? *(const float *)(uintptr_t)(camera+arc_offset) : -1.0f);
    if (total!=0.0f) {
        const float a=total*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
        const float x=dir[0], z=dir[2];
        dir[0]=x*c-z*s;
        dir[2]=x*s+z*c;
        if (SDL_fabsf(total)>=0.15f) {
            st->before=heading;
            st->after=heading_of(dir);
            st->pending=1;
        }
    }
    if (dy!=0.0f && arc_offset) {
        /* The mouse stops below the top of the arc (arc_max): at the top the camera looks straight
         * down and a small sideways move spins the whole view around Drake. BB_MOUSE_ARC_MAX=<0..1>
         * sets it for every game. Where the game itself has the camera higher, the mouse only lowers it. */
        static float arc_env=-2.0f;
        if (arc_env<-1.0f) { const char *e=getenv("BB_MOUSE_ARC_MAX"); arc_env=e ? (float)atof(e) : -1.0f; }
        const float top_limit=arc_env>=0.0f ? arc_env : arc_max;
        float *arc=(float *)(uintptr_t)(camera+arc_offset);
        const float cur=*arc, top=cur>top_limit ? cur : top_limit;
        float v=cur+(input.mouse_invert_y ? -dy : dy)*(MOUSE_DEGREES_PER_PIXEL/arc_degrees)*input.mouse_sensitivity;
        *arc=v<0.0f ? 0.0f : v>top ? top : v;
    }
}
/* The cover camera (CameraControlCover) turns with the right stick in its own update: its
 * horizontal direction (x, 0, z) turns around the vertical, and its pitch (degrees) moves within
 * limits (up to limits[0], down to -limits[1]). The stick's top speed made the mouse slow there;
 * at the start of the update the mouse turns both directly, the same amount as the follow camera. */
static void cover_camera(float *dir, float *pitch, const float *limits) {
    const uint64_t now=now_us();
    if (cover_hook_us && now-cover_hook_us>100000 && !(camera_hook_us && now-camera_hook_us<100000)) {
        float ignore_x, ignore_y;
        camera_take(0,&ignore_x,&ignore_y);
    }
    cover_hook_us=now;
    follow_push=0.0f;
    float dx, dy;
    frame_motion(now,&dx,&dy);
    if (aim_held) return; /* the aiming camera takes the mouse */
    if (mouse_debug()) printf("Mouse: cover t=%.4f dx=%.1f dy=%.1f heading=%.2f pitch=%.2f limits=%.1f..%.1f\n",
                              now*1e-6,dx,dy,heading_of(dir),*pitch,limits ? -limits[1] : 0.0f,limits ? limits[0] : 0.0f);
    const float turn=dx*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
    if (turn!=0.0f) {
        const float a=turn*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
        const float x=dir[0], z=dir[2];
        dir[0]=x*c-z*s;
        dir[2]=x*s+z*c;
    }
    if (dy!=0.0f && limits) {
        float v=*pitch-(input.mouse_invert_y ? -dy : dy)*MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
        if (v>limits[0]) v=limits[0];
        if (v<-limits[1]) v=-limits[1];
        *pitch=v;
    }
}
/* Turns an aim direction (x, y, z) by yaw and pitch (degrees), keeping its length; pitch stays
 * within +-70 degrees. Same yaw sense as the follow camera: a positive yaw is a turn right. */
static void aim_turn(float *v, float yaw, float pitch) {
    const float x=v[0], y=v[1], z=v[2];
    const float len=SDL_sqrtf(x*x+y*y+z*z), h=SDL_sqrtf(x*x+z*z);
    if (!(len>0.5f && len<1.5f) || h<1e-4f) return;
    const float a=yaw*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
    const float hx=(x*c-z*s)/h, hz=(x*s+z*c)/h;
    float p=SDL_atan2f(y,h)+pitch*0.017453293f;
    if (p>1.2217f) p=1.2217f;
    if (p<-1.2217f) p=-1.2217f;
    v[0]=hx*SDL_cosf(p)*len; v[1]=SDL_sinf(p)*len; v[2]=hz*SDL_cosf(p)*len;
}
/* Raw aim, all three games (US 1.02). The aim controller (U1 0x276a10, U2 0x71a1b0, U3 0x9285c0;
 * rsi = the controller) gets the aiming camera's remembered aim direction as its 4th argument
 * (rcx): it turns it by the stick, the camera saves it back, and the shown direction adds aim
 * assist and recoil on top. Called from the aiming camera's own call site (return address
 * main_return), the mouse turns that direction directly; other callers (U3 also computes a
 * temporary direction elsewhere) are left alone and take no mouse motion. regs: the hook's saved
 * registers (rbx r11 r10 r9 r8 rdx rcx rax rflags, return address). */
static void raw_aim(const char *game, uint64_t controller, const uint64_t *regs, uint64_t image,
                    uint64_t main_return) {
    static float aim_scale=-1.0f; /* BB_AIM_SCALE: raw aim speed against the follow camera's (1.0) */
    static int aim_off=-1;        /* BB_AIM_DIRECT=0: the old stick aiming, for comparing */
    if (aim_scale<0.0f) { const char *e=getenv("BB_AIM_SCALE"); aim_scale=e ? (float)atof(e) : 1.0f; }
    if (aim_off<0) { const char *e=getenv("BB_AIM_DIRECT"); aim_off=e && e[0]=='0'; }
    if (aim_held) bbgpu_note_aiming(); /* BB_DOF=aim: no depth of field while aiming (our aim hooks did this) */
    const uint64_t ret=regs[9+16]-image; /* (this port's thunk: 128-byte red-zone step before the return address) */
    float *dir=(float *)(uintptr_t)regs[6];
    static uint64_t other_logged;
    if (ret!=main_return) {
        if (other_logged!=ret) {
            other_logged=ret;
            printf("Mouse: %s aim controller from another caller (return %#llx), left alone\n",game,
                   (unsigned long long)ret);
        }
        return;
    }
    static int first=1;
    if (first) {
        first=0;
        printf("Mouse: %s raw aim on: controller %#llx, remembered direction %#llx (controller %+lld)\n",
               game,(unsigned long long)controller,(unsigned long long)(uintptr_t)dir,
               (long long)((uintptr_t)dir-controller));
    }
    if (aim_off) return;
    const uint64_t now=now_us();
    pthread_mutex_lock(&camera_lock);
    const float dx=aim_dx, dy=aim_dy;
    aim_dx=aim_dy=0.0f;
    pthread_mutex_unlock(&camera_lock);
    aim_direct_us=now;
    const float k=MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity*aim_scale;
    if (mouse_debug()) printf("Mouse: aim t=%.4f ctl=%#llx yaw=%.3f pitch=%.3f dx=%.1f dy=%.1f\n",now*1e-6,(unsigned long long)controller,
                              SDL_atan2f(-dir[0],-dir[2])*57.29578f,SDL_atan2f(dir[1],SDL_sqrtf(dir[0]*dir[0]+dir[2]*dir[2]))*57.29578f,dx,dy);
    /* While aim is held the game may swap in new aiming cameras (after a roll: a new one about
     * every 100 ms while Drake gets up), which threw away the turns given to the one before. A new
     * controller in the same aim press gets the mouse's turns of this press on top of the game's
     * own direction: copying the last camera's direction kept the first camera of a roll, which
     * still faced the way Drake looked before the roll. On the same controller the game's own
     * changes (aim assist, recoil) stay. */
    /* Two aiming cameras can also take turns within one frame (blending): each controller keeps
     * how much of this press's turns it already has, so each gets every turn exactly once. */
    /* A turn counts as given once the controller keeps it. U1 sometimes starts an aim press with an
     * aiming camera whose direction the game puts back every frame until it swaps in the next one
     * (0.3-0.5 s, players: "the aim is locked for a second each time you aim"): turns given to it
     * were counted and lost. A direction that comes back exactly as it was before the turn marks
     * such a camera: while it is updated the mouse goes to the right stick instead (the old
     * aiming, aim_stick_fallback), and the turns it threw away are dropped (no jump later). It is
     * also the camera shown while the game blends aiming in (taking turns with one that keeps
     * them, every frame): giving the mouse to the keeping one only (the stick off) made the aim
     * follow 40-160 ms late. */
    static uint64_t session_press;
    static float turned_yaw, turned_pitch; /* the mouse's turns in this aim press (degrees) */
    static struct aim_given {
        uint64_t controller, last_us;
        float yaw, pitch;                  /* turns the controller's direction holds */
        float sent_yaw, sent_pitch;        /* turned last time, not yet known to be kept */
        float before[3];                   /* its direction before that turn */
        int puts_back;                     /* it threw a turn away: the stick turns it */
    } given[4];
    static int last_slot=-1;
    if (session_press!=aim_press_us) {
        session_press=aim_press_us;
        turned_yaw=turned_pitch=0.0f;
        memset(given,0,sizeof(given));
        last_slot=-1;
        aim_stick_fallback=0;
    }
    if (!aim_held) { aim_stick_fallback=0; return; }
    int slot=0;
    for (int i=0;i<4;++i) {
        if (given[i].controller==controller) { slot=i; break; }
        if (given[i].last_us<given[slot].last_us) slot=i;
    }
    #define AIM_DOT(a,b) ((a)[0]*(b)[0]+(a)[1]*(b)[1]+(a)[2]*(b)[2])
    #define AIM_UNCHANGED(a,b) (SDL_fabsf((a)[0]-(b)[0])+SDL_fabsf((a)[1]-(b)[1])+SDL_fabsf((a)[2]-(b)[2])<1e-4f)
    struct aim_given *g=&given[slot];
    if (g->controller==controller) {
        /* the last turn: kept unless the direction came back as it was before it */
        if (g->sent_yaw!=0.0f || g->sent_pitch!=0.0f) {
            /* U1 only: its joypad hook is where the stick fallback turns the camera */
            if (game[1]=='1' && AIM_UNCHANGED(dir,g->before)) {
                if (!g->puts_back && mouse_debug())
                    printf("Mouse: aim controller %#llx puts its direction back: the stick turns it\n",(unsigned long long)controller);
                g->puts_back=1;
                turned_yaw=g->yaw; turned_pitch=g->pitch;
            } else {
                g->yaw+=g->sent_yaw; g->pitch+=g->sent_pitch;
            }
        }
    } else {
        /* A new controller that starts where the last one ended (U1 swaps aiming cameras while
         * aiming) holds that one's kept turns, and its last turn too when it starts where that
         * turn pointed; one that starts elsewhere (after a roll: the game's own direction) holds none. */
        float yaw0=0.0f, pitch0=0.0f;
        if (last_slot>=0) {
            const struct aim_given *p=&given[last_slot];
            float turned_dir[3]={p->before[0],p->before[1],p->before[2]};
            if (p->sent_yaw!=0.0f || p->sent_pitch!=0.0f) aim_turn(turned_dir,p->sent_yaw,p->sent_pitch);
            const float to_before=AIM_DOT(dir,p->before), to_turned=AIM_DOT(dir,turned_dir);
            if (to_turned>0.9986f && to_turned>=to_before) { /* within 3 degrees */
                yaw0=p->yaw+p->sent_yaw; pitch0=p->pitch+p->sent_pitch;
            } else if (to_before>0.9986f) {
                yaw0=p->yaw; pitch0=p->pitch;
            }
        }
        memset(g,0,sizeof(*g));
        g->controller=controller;
        g->yaw=yaw0; g->pitch=pitch0;
    }
    #undef AIM_DOT
    #undef AIM_UNCHANGED
    g->last_us=now;
    g->before[0]=dir[0]; g->before[1]=dir[1]; g->before[2]=dir[2];
    last_slot=slot;
    /* U1 blends aiming in from the follow camera (shown mixed, the aiming camera's share growing):
     * a turn of the aiming camera alone showed 250 ms late (measured on screen), the stick
     * turns the shown view at once (130 ms, as a controller). The stick aims for the start of
     * each press (BB_AIM_STICK_MS, 400; 0: off), then the mouse turns 1:1. */
    static int stick_ms=-1;
    if (stick_ms<0) { const char *e=getenv("BB_AIM_STICK_MS"); stick_ms=e ? atoi(e) : 400; }
    const int blending=game[1]=='1' && stick_ms>0 && now-aim_press_us<(uint64_t)stick_ms*1000;
    aim_stick_fallback=g->puts_back || blending;
    if (g->puts_back || blending) { /* the stick has this motion */
        turned_yaw=g->yaw; turned_pitch=g->pitch;
        g->sent_yaw=g->sent_pitch=0.0f;
        return;
    }
    if (dx!=0.0f || dy!=0.0f) {
        turned_yaw+=dx*k;
        turned_pitch+=-(input.mouse_invert_y ? -dy : dy)*k;
    }
    const float yaw=turned_yaw-g->yaw, pitch=turned_pitch-g->pitch;
    g->sent_yaw=yaw; g->sent_pitch=pitch;
    if (yaw!=0.0f || pitch!=0.0f) aim_turn(dir,yaw,pitch);
}
static void __attribute__((ms_abi)) u1_102_aim_controller_c(uint64_t output, uint64_t controller, uint64_t image,
                                                            uint64_t r14, const uint64_t *regs) {
    (void)output; (void)r14;
    /* U1 1.02: called from the aiming camera's update (0xc43b0) at 0xc4c52 */
    raw_aim("U1",controller,regs,image,0xc4c57);
}
static void __attribute__((ms_abi)) u2_102_aim_controller(uint64_t output, uint64_t controller, uint64_t image,
                                                          uint64_t r14, const uint64_t *regs) {
    (void)output; (void)r14;
    /* U2 1.02: the aiming camera calls it at 0x45124b (controller camera+0x160, direction +0x1f30) */
    raw_aim("U2",controller,regs,image,0x451250);
}
static void __attribute__((ms_abi)) u3_102_aim_controller(uint64_t output, uint64_t controller, uint64_t image,
                                                          uint64_t r14, const uint64_t *regs) {
    (void)output; (void)r14;
    /* U3 1.02: the aiming camera calls it at 0x51188f (controller camera+0x1b0, direction +0x11e0);
     * 0x4d09ce computes a temporary direction and is left alone */
    raw_aim("U3",controller,regs,image,0x511894);
}
static void __attribute__((ms_abi)) u1_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14, const uint64_t *regs) {
    (void)regs;
    (void)unused; (void)image; (void)r14;
    follow_camera(camera,0x1310,0x12bc,61.0f,1.0f);
}
static void __attribute__((ms_abi)) u2_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14, const uint64_t *regs) {
    (void)regs;
    (void)unused; (void)image; (void)r14;
    follow_camera(camera,0x9ec0,0x9e60,135.0f,0.75f);
}
static void __attribute__((ms_abi)) u3_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14, const uint64_t *regs) {
    (void)regs;
    (void)unused; (void)image; (void)r14;
    follow_camera(camera,0x78c0,0x7874,105.0f,0.75f);
}
/* U1 and U2: the limits come from the camera's settings (+0x8 up, +0xc down). */
static void cover_camera_settings(uint64_t camera, uint32_t dir_offset, uint32_t pitch_offset,
                                  uint32_t settings_offset) {
    const float *settings=*(const float *const *)(uintptr_t)(camera+settings_offset);
    cover_camera((float *)(uintptr_t)(camera+dir_offset),(float *)(uintptr_t)(camera+pitch_offset),
                 settings ? settings+2 : NULL);
}
static void __attribute__((ms_abi)) u1_102_cover_camera(uint64_t output, uint64_t camera, uint64_t image,
                                                        uint64_t r14, const uint64_t *regs) {
    (void)output; (void)image; (void)r14; (void)regs;
    cover_camera_settings(camera,0x240,0x230,0x1300);
}
static void __attribute__((ms_abi)) u2_102_cover_camera(uint64_t output, uint64_t camera, uint64_t image,
                                                        uint64_t r14, const uint64_t *regs) {
    (void)output; (void)image; (void)r14; (void)regs;
    cover_camera_settings(camera,0x1c0,0x1b0,0x1290);
}
/* U3: the camera is the first argument and holds its limits (+0x218 up, +0x21c down). */
static void __attribute__((ms_abi)) u3_102_cover_camera(uint64_t camera, uint64_t unused, uint64_t image,
                                                        uint64_t r14, const uint64_t *regs) {
    (void)unused; (void)image; (void)r14; (void)regs;
    cover_camera((float *)(uintptr_t)(camera+0x2f0),(float *)(uintptr_t)(camera+0x2e0),
                 (const float *)(uintptr_t)(camera+0x218));
}
/* Raw aim (Uncharted 2 and 3). While aiming the follow camera is not the one shown, and the mouse
 * went to the right stick: through the aiming camera's curve, speed cap and dead zone. The games'
 * camera input step (every camera, aiming included; after BilalReloaded's Uncharted port, located
 * in the EU 1.02 builds by the fields it uses) gets the look direction it is about to turn from:
 * the mouse rotates it by its exact angle, and moves the camera's vertical arc the way the follow
 * camera does, so aiming turns 1:1 with the same sensitivity. BB_RAW_AIM=0: off (the stick). */
static uint64_t aim_hook_us; /* last camera input step while aiming: the mouse aims directly */
static int raw_aim_enabled(void) {
    static int on=-1;
    if (on<0) { const char *e=getenv("BB_RAW_AIM"); on=!(e && e[0]=='0'); }
    return on;
}
/* U3 1.02 (EU): camera input at image+0x11d4e0 (camera rdi, look direction rsi, stick rdx). */
/* U2 1.02 (EU): camera input at image+0x16d920 (camera rdi, look direction rsi, stick rdx); the
 * same in CUSA02344 1.02 at image+0x428f40 (BilalReloaded's build, untested here). */
static const unsigned char u1_follow_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x28,0x01,0x00,0x00,
    0x44,0x89,0x8d,0x00,0xff,0xff,0xff,0x49,0x89,0xcd,0x48,0x89,0xd3,0x49,0x89,0xf7,0x49,0x89,0xfc,
    0x4c,0x8b,0x35,0x92,0x7b,0x8d,0x00,0x49,0x8b,
};
static const unsigned char u2_follow_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x48,0x01,0x00,0x00,
    0x44,0x89,0x8d,0x18,0xff,0xff,0xff,0x48,0x89,0x8d,0xc8,0xfe,0xff,0xff,0x49,0x89,0xd5,0x4c,0x89,0xad,
    0x38,0xff,0xff,0xff,0x48,0x89,0xf3,0x49,
};
static const unsigned char u3_follow_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xd8,0x01,0x00,0x00,
    0x44,0x89,0x8d,0xa0,0xfe,0xff,0xff,0x48,0x89,0x8d,0x48,0xfe,0xff,0xff,0x49,0x89,0xd5,0x48,0x89,0xf3,
    0x49,0x89,0xff,0x4c,0x8b,0x65,0x20,0x4c,
};
/* U1 US 1.02 (CUSA02320) has the same code with other RIP-relative displacements. */
static const unsigned char u1_102_follow_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x28,0x01,0x00,0x00,
    0x44,0x89,0x8d,0x00,0xff,0xff,0xff,0x49,0x89,0xcd,0x48,0x89,0xd3,0x49,0x89,0xf7,0x49,0x89,0xfc,
    0x4c,0x8b,0x35,0x42,0xbd,0x89,0x00,0x49,0x8b,
};
static const unsigned char u1_102_pad_update_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xb8,0x00,0x00,0x00,
    0x49,0x89,0xfe,0x4c,0x8b,0x2d,0xe2,0x37,0x83,0x00,0x49,0x8b,
};
/* U2 and U3 US 1.02 (CUSA02320) follow camera: the same function, recompiled (found by its
 * instruction patterns; it uses the same direction and arc fields on rdi). */
static const unsigned char u2_102_follow_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x28,0x01,0x00,0x00,
    0x45,0x89,0xcc,0x48,0x89,0x8d,0xf8,0xfe,0xff,0xff,0x49,0x89,0xd6,0x49,0x89,0xfd,
};
static const unsigned char u3_102_follow_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xb8,0x01,0x00,0x00,
    0x45,0x89,0xcc,0x48,0x89,0x8d,0x68,0xfe,0xff,0xff,0x49,0x89,0xd6,0x49,0x89,0xfd,
};
static const unsigned char u2_102_aim_controller_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x18,0x01,0x00,0x00,
    0xc5,0xfa,0x11,0x45,0x90,0x44,0x89,0x85,0x60,0xff,
};
static const unsigned char u3_102_aim_controller_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xd8,0x00,0x00,0x00,
    0xc5,0xfa,0x11,0x85,0x40,0xff,0xff,0xff,0x4c,0x89,
};
/* 1.02 cover camera updates (CameraControlCover). U1 (vtable 0x927320) and U2 (vtable 0xfff0d0,
 * slot 28): rsi = the camera; U3 (vtable 0x1670210, slot 40): rdi = the camera. */
static const unsigned char u1_102_cover_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x88,0x01,0x00,0x00,
    0x49,0x89,0xd7,0x48,0x89,0xf3,0x49,0x89,0xfe,
};
static const unsigned char u2_102_cover_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x68,0x01,0x00,0x00,
    0x49,0x89,0xd7,0x48,0x89,0xf3,0x49,0x89,0xfe,
};
static const unsigned char u3_102_cover_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xf8,0x00,0x00,0x00,
    0x49,0x89,0xf6,0x49,0x89,0xff,
};
static const unsigned char u1_102_aim_controller_c_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x08,0x03,0x00,0x00,
    0x45,0x89,0xc4,0x48,0x89,0x8d,0x00,0xfd,0xff,0xff,
};
/* The same aim controllers and cover cameras in the EU version (CUSA02343), whose U2 and U3 are
 * compiled differently: found by the aiming camera's call (rcx = camera + 0x1f30 / + 0x11e0, rsi =
 * camera + 0x160 / + 0x1b0) and by the cover camera's vtable slot (U2 table 0xe063d0 slot 28, U3
 * 0x1365470 slot 40), matched slot by slot against the 1.02 tables. Same arguments and fields. */
static const unsigned char u1_eu_aim_controller_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x08,0x03,0x00,0x00,
    0x45,0x89,0xc4,0x48,
};
static const unsigned char u2_eu_aim_controller_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x28,0x03,0x00,0x00,
    0xc5,0xfa,0x11,0x85,
};
#define u3_eu_aim_controller_code u2_eu_aim_controller_code /* the same first 24 bytes (other game) */
static const unsigned char u1_eu_cover_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x88,0x01,0x00,0x00,
    0x49,0x89,0xd7,0x48,
};
static const unsigned char u2_eu_cover_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xa8,0x01,0x00,0x00,
    0x49,0x89,0xd7,0x49,
};
static const unsigned char u3_eu_cover_camera_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x81,0xec,0x90,0x00,0x00,0x00,
    0x49,0x89,0xf6,0x49,0x89,0xff,
};
static void __attribute__((ms_abi)) u1_eu_aim_controller(uint64_t output, uint64_t controller, uint64_t image,
                                                         uint64_t r14, const uint64_t *regs) {
    (void)output; (void)r14;
    raw_aim("U1",controller,regs,image,0xc4b67); /* called at 0xc4b62 in the aiming camera 0xc42c0 */
}
static void __attribute__((ms_abi)) u2_eu_aim_controller(uint64_t output, uint64_t controller, uint64_t image,
                                                         uint64_t r14, const uint64_t *regs) {
    (void)output; (void)r14;
    raw_aim("U2",controller,regs,image,0x18cc5f); /* called at 0x18cc5a in the aiming camera 0x18c9a0 */
}
static void __attribute__((ms_abi)) u3_eu_aim_controller(uint64_t output, uint64_t controller, uint64_t image,
                                                         uint64_t r14, const uint64_t *regs) {
    (void)output; (void)r14;
    raw_aim("U3",controller,regs,image,0x13c621); /* called at 0x13c61c in the aiming camera 0x13c230 */
}
/* U1: the game's pad update (image+0x117200) fills the caller's joypad state (its first argument):
 * the sticks as floats lx ly rx ry at +0 and again at +0x10, converted from the pad's bytes. Cameras
 * other than the follow camera (aiming: about 45 degrees/s * x|x|, no speed ramp) get the mouse as a
 * right stick value there, not limited to the stick's range. */
static uint64_t joypad_state;         /* the state the pad update fills (its first argument) */
static uint64_t sticks_last_us;
static float aim_test=-1.0f;          /* BB_MOUSE_AIM_TEST=<x>: right stick x while not following */
static float aim_stick(float rate) {
    const float r=rate<0 ? -rate : rate;
    const float x=r<=45.0f ? SDL_sqrtf(r/45.0f) : (r+9.0f)/54.0f;
    return rate<0 ? -x : x;
}
/* U2 and U3 1.02 (EU): the aiming camera doesn't run the camera input step (only while it blends
 * in); it turns from the right stick, capped at full deflection: ~46 degrees/s with the mouse as a
 * stick. The pad update ends by converting the pad's stick bytes into the caller's input (floats,
 * right stick x at +8, y at +0xc, a copy at +0x18, +0x1c); there the mouse's turn rate is added as
 * a stick value beyond 1 while L2 is held, through the inverse of the aiming curve, measured as in
 * U1 (45 x^2 degrees/s up to x = 1, then about 54 x - 9; vertically `pitch` times that).
 * BB_AIM_STICK=<x>[,<y>]: fixed values while aiming, n times them at the n-th aim press (y up and
 * down in turn; measuring the aiming curve). */
/* U2: the pad update at image+0x251420 ends at image+0x251be8 with its input in r14. */
/* U2 US 1.02 (CUSA02320, as CUSA02344): the pad update at image+0x565d30 fills the two pads in a
 * loop that moves its output pointer (rdi at the start), so its start notes the output and its end
 * (image+0x56668e) turns pad 0's right stick, as in U3. */
/* U3: the pad update at image+0x741a30 keeps its input (rdi) in r13; at its end (image+0x74231e)
 * the input is the one its start saw. */
/* U2 US 1.02: the pad update's start (push rbp; mov rbp, rsp; push r15 move) and its end (mov rdx,
 * [r14]; cmp rdx, [rbp-0x30] move; the jne after it still sees the compare). */
/* U1 US 1.02 (eboot.bin): the same code as EU, moved (follow camera image+0xaede0, pad update
 * image+0x117350); only their rip-relative operands differ. The joypad sticks match EU's bytes
 * exactly at image+0x11799e. */
/* U2 US 1.02: the aiming camera (UpdateCameraDirection, player-aiming-machine3.cpp, inlined) gives
 * each stick axis its response, sign(x) * |x|^0.65, and clamps it to [-1, 1] before storing it
 * (+0xc88, +0xc8c), where EU passes values beyond full deflection on: aiming stopped at stick speed
 * (~45 degrees/s). The clamps go (the x block's load of -1 stays: the y block uses it). A stick
 * never exceeds 1, so only the mouse's larger values (raw aim) change anything. */
/* U3 US 1.02: the pad update at image+0x10ddd90 (output in rdi, as U2 US's), its end at
 * image+0x10de707; the aiming clamps at image+0x9297b9/0x92981f. U1 US: the clamps at
 * image+0x277fcb/0x27802c. Same code shapes as U2 US (see there). */
/* U3 US 1.02: the follow camera at image+0x4e7970 (string id 0xf34acb0c; the camera in rdi and its
 * direction at +0x78c0, as in EU). */
/* U2 US 1.02: the follow camera at image+0x42c690 (the camera in rdi, as in EU). */
/* The follow camera's response to the right stick value (measured in U1): about 144 x^2 degrees/s up
 * to x = 0.5, then a speed that builds up over a second towards 240 degrees/s from x = 0.7 (and stays
 * there above 0.9): fast turns reach it late. */
static float follow_stick(float rate) {
    const float r=rate<0 ? -rate : rate;
    float x=r<=36.0f ? SDL_sqrtf(r/144.0f) : 0.5f+(r-36.0f)/(240.0f-36.0f)*0.4f;
    if (x>0.9f) x=0.9f;
    return rate<0 ? -x : x;
}
static void __attribute__((ms_abi)) u1_pad_update(uint64_t state, uint64_t unused, uint64_t image, uint64_t r14, const uint64_t *regs) {
    (void)regs;
    (void)unused; (void)r14;
    if (mouse_debug() && joypad_state!=state)
        printf("Mouse: joypad state %#llx (image %#llx, offset %#llx)\n",(unsigned long long)state,
               (unsigned long long)image,(unsigned long long)(state-image));
    joypad_state=state;
}
static int u1_us_curve; /* US 1.02: the aiming curve past full deflection is 45 * x^0.65 */
static void __attribute__((ms_abi)) u1_joypad_sticks(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t r14, const uint64_t *regs) {
    (void)regs;
    (void)unused0; (void)unused1; (void)image;
    if (!joypad_state || r14-0x62!=joypad_state) return; /* pad 0 only */
    const uint64_t now=now_us();
    sticks_hook_us=now;
    float dt=sticks_last_us ? (float)(now-sticks_last_us)*1e-6f : 1.0f/60.0f;
    sticks_last_us=now;
    if (dt<1.0f/480.0f) dt=1.0f/480.0f;
    if (dt>0.05f) dt=0.05f;
    static float follow_test=-1.0f;   /* BB_MOUSE_FOLLOW_TEST=<x>: right stick x everywhere (measuring) */
    if (follow_test<0.0f) { const char *t=getenv("BB_MOUSE_FOLLOW_TEST"); follow_test=t ? (float)atof(t) : 0.0f; }
    if (follow_test>0.0f) { ((float *)(uintptr_t)joypad_state)[2]+=follow_test; ((float *)(uintptr_t)joypad_state)[6]+=follow_test; return; }
    float dx, dy;
    float paced_period;
    camera_take_paced(1,&dx,&dy,&paced_period);
    if (mouse_pace_seconds()>0.0f) dt=paced_period; /* the stick is the paced movement over the usual period: the mouse's rate */
    if (aim_test<0.0f) { const char *t=getenv("BB_MOUSE_AIM_TEST"); aim_test=t ? (float)atof(t) : 0.0f; }
    float *pad=(float *)(uintptr_t)joypad_state;
    float x=0.0f, y=0.0f;
    if (aim_test>0.0f) x=aim_test;
    else {
        /* the turn rate (degrees/s) the mouse asks for, through the inverse of the aiming curve
         * measured in U1: 45 x^2 degrees/s up to x = 1, then about 54 x - 9 (x = 3: 152, x = 6: 324) */
        const float scale=MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity/dt;
        const int following=camera_hook_us && now-camera_hook_us<50000;
        if (raw_aim_armed(now) && !aim_stick_fallback) dx=dy=0.0f; /* raw aim turns it */
        float push=0.0f;
        if (!aim_held && following && follow_direct) { /* the follow camera turned itself */
            dx=dy=0.0f;
            push=follow_push;
        }
        if (!aim_held && cover_hook_us && now-cover_hook_us<50000) dx=dy=0.0f; /* the cover camera too */
        float (*curve)(float)=aim_held ? aim_stick : follow_stick;
        /* following through the joypad: the shortfall is made up within about a quarter second */
        const float extra=!aim_held && following && !follow_direct ? follow_correction*4.0f : 0.0f;
        x=curve(dx*scale+extra)+push;
        /* aiming turns vertically at about 0.45 times the horizontal rate for the same stick value */
        y=curve((input.mouse_invert_y ? -dy : dy)*scale/(aim_held ? 0.45f : 1.0f));
        /* US 1.02: past full deflection the aiming camera turns at 45 * x^0.65 (as U2's) */
        if (aim_held && u1_us_curve) {
            const float rx=dx*scale, ry=(input.mouse_invert_y ? -dy : dy)*scale/0.45f;
            if (rx>45.0f || rx<-45.0f) x=rx>0.0f ? SDL_powf(rx/45.0f,1.0f/0.65f) : -SDL_powf(-rx/45.0f,1.0f/0.65f);
            if (ry>45.0f || ry<-45.0f) y=ry>0.0f ? SDL_powf(ry/45.0f,1.0f/0.65f) : -SDL_powf(-ry/45.0f,1.0f/0.65f);
        }
    }
    if (mouse_debug()) printf("Mouse: aim t=%.4f dt=%.4f dx=%.1f x=%.3f game=%.3f\n",now*1e-6,dt,dx,x,pad[2]);
    if (x!=0.0f) { pad[2]+=x; pad[6]+=x; }
    if (y!=0.0f) { pad[3]+=y; pad[7]+=y; }
}
static const unsigned char u1_pad_update_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xb8,0x00,0x00,0x00,
    0x49,0x89,0xfe,0x4c,0x8b,0x2d,0x92,0xf6,0x86,0x00,0x49,0x8b,
};
static const unsigned char u1_joypad_sticks_code[]={
    0x80,0x7c,0x01,0x6d,0x00,0x74,0x3b,0xc5,0xe2,0x59,0x64,0x01,0x40,0xc4,0xc1,0x7a,0x11,0x66,0xbe,0xc5,
    0xe2,0x59,0x64,0x01,
};
/* U2 UpdateFocus (player-base.cpp:1101): the camera's look-at spring went NaN once at 240 Hz and
 * the game's assertion ended it. The assertion's call now restarts the spring at the player's
 * target ([rbp-0x60]): position (+0x2b0, y +0x2a4) and velocities (+0x2c0, +0x2a8); a NaN target
 * still stops at the trap. */
static const unsigned char u2_focus_assert_code[]={
    0xe8,0xa8,0xd7,0x1a,0x00,0x48,0x85,0xc0,0x74,0x27,0x48,0x8d,0x3d,0x4c,0x9c,0x83,0x00,0x48,0x8d,0x35,
    0x4b,0x99,0x83,0x00,0x48,0x8d,0x15,0x5b,0x9c,0x83,0x00,0x48,0x8d,0x0d,0x7e,0x99,0x83,0x00,0x41,0xb8,
    0x4d,0x04,0x00,0x00,0x45,0x31,0xc9,0xff,0xd0,0xcd,0x41,0xc5,0xf8,0x28,0x45,0xd0,
};
static const unsigned char u2_focus_reset[]={
    0xc5,0xf8,0x28,0x45,0xa0,                     /* vmovaps xmm0, [rbp-0x60] (the target) */
    0x0f,0x2e,0xc0,0x7a,0x2c,                     /* ucomiss xmm0, xmm0; jp trap */
    0xc5,0xf8,0x29,0x83,0xb0,0x02,0x00,0x00,      /* vmovaps [rbx+0x2b0], xmm0 */
    0xc4,0xe3,0x79,0x17,0x83,0xa4,0x02,0x00,0x00,0x01, /* vextractps [rbx+0x2a4], xmm0, 1 */
    0xc5,0xf8,0x29,0x45,0xd0,                     /* vmovaps [rbp-0x30], xmm0 */
    0xc5,0xf0,0x57,0xc9,                          /* vxorps xmm1, xmm1, xmm1 */
    0xc5,0xf8,0x29,0x8b,0xc0,0x02,0x00,0x00,      /* vmovaps [rbx+0x2c0], xmm1 */
    0x83,0xa3,0xa8,0x02,0x00,0x00,0x00,           /* and dword [rbx+0x2a8], 0 */
    0xeb,0x02,                                    /* jmp (store the focus) */
    0xcd,0x41,                                    /* trap: int 0x41 */
};
_Static_assert(sizeof(u2_focus_reset)==sizeof(u2_focus_assert_code),"same size");
/* U2 "pAction != NULL" (moveactioncontroller.cpp:40): the player's move actions come from a pool
 * of 64 slots of 0x300 bytes (the object at character+0x2e8, pool at +0xf0). Two move states
 * swapping every frame (landing into a fight) keep one action each swap; at 144 fps bursts took
 * 16-30 slots, and a longer one filled the pool and stopped the game. The slot count passed where
 * the character is set up becomes 256 (+144 KB from the character's heap). */
static const unsigned char u2_move_pool_code[]={
    0x49,0x8b,0xbd,0xe8,0x02,0x00,0x00,           /* mov rdi, [r13+0x2e8] */
    0xba,0x06,0x00,0x00,0x00,                     /* mov edx, 6 */
    0xb9,0x40,0x00,0x00,0x00,                     /* mov ecx, 64 (move action slots) */
    0x41,0xb8,0x20,0x00,0x00,0x00,                /* mov r8d, 32 */
};
static const unsigned char u2_move_pool_256[]={
    0x49,0x8b,0xbd,0xe8,0x02,0x00,0x00,
    0xba,0x06,0x00,0x00,0x00,
    0xb9,0x00,0x01,0x00,0x00,                     /* mov ecx, 256 */
    0x41,0xb8,0x20,0x00,0x00,0x00,
};
_Static_assert(sizeof(u2_move_pool_256)==sizeof(u2_move_pool_code),"same size");
/* US 1.02 (CUSA02320, = CUSA02344): the same setup at 0x721347 (the character in r15). */
static const unsigned char u2_move_pool_code_us[]={
    0x49,0x8b,0xbf,0xe8,0x02,0x00,0x00,           /* mov rdi, [r15+0x2e8] */
    0xba,0x06,0x00,0x00,0x00,                     /* mov edx, 6 */
    0xb9,0x40,0x00,0x00,0x00,                     /* mov ecx, 64 (move action slots) */
    0x41,0xb8,0x20,0x00,0x00,0x00,                /* mov r8d, 32 */
};
static const unsigned char u2_move_pool_256_us[]={
    0x49,0x8b,0xbf,0xe8,0x02,0x00,0x00,
    0xba,0x06,0x00,0x00,0x00,
    0xb9,0x00,0x01,0x00,0x00,                     /* mov ecx, 256 */
    0x41,0xb8,0x20,0x00,0x00,0x00,
};
_Static_assert(sizeof(u2_move_pool_256_us)==sizeof(u2_move_pool_code_us),"same size");
/* U1 has the same move action pool (64 slots of 0x300 bytes, at +0x100 of the object at
 * character+0x3a8) and the same "pAction != NULL" check: 256 slots as in U2. The setup's
 * arguments, identical in EU (0x2668fe) and US (0x266e6e). */
static const unsigned char u1_move_pool_code[]={
    0x49,0x8b,0xb6,0xf0,0x00,0x00,0x00,           /* mov rsi, [r14+0xf0] */
    0xba,0x06,0x00,0x00,0x00,                     /* mov edx, 6 */
    0xb9,0x40,0x00,0x00,0x00,                     /* mov ecx, 64 (move action slots) */
    0x41,0xb8,0x20,0x00,0x00,0x00,                /* mov r8d, 32 */
};
static const unsigned char u1_move_pool_256[]={
    0x49,0x8b,0xb6,0xf0,0x00,0x00,0x00,
    0xba,0x06,0x00,0x00,0x00,
    0xb9,0x00,0x01,0x00,0x00,                     /* mov ecx, 256 */
    0x41,0xb8,0x20,0x00,0x00,0x00,
};
_Static_assert(sizeof(u1_move_pool_256)==sizeof(u1_move_pool_code),"same size");
/* U3's heap free (corelib memory-new-malloc, image+0x494a70: rdi the heap, rsi the block's data;
 * its size, header included, at data-0xc). The game frees a cloth simulation's buffers right
 * after its last dispatch and reuses the memory at once (its free list goes into the block):
 * the free is noted (a few stores, no lock: a level change frees thousands of blocks) and the
 * GPU's read-backs leave the heap's own bytes in free blocks alone (BufferCache, FreedNotes). */
static const unsigned char u3_heap_free_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x28,0x49,0x89,0xf6,
    0x49,0x89,0xfc,0x48,0x8d,0x05,0xaa,0x3d,0xf6,0x00,
};
/* The same free in US 1.02 (CUSA02320, as CUSA02344): image+0xd9ce70, the block in rsi and its
 * header at -0xc/-0x10 as in EU. */
static const unsigned char u3_heap_free_us_code[]={
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18,0x49,0x89,0xf4,
    0x49,0x89,0xfe,0x48,0x8d,0x05,0x9a,0xa2,0x94,0x00,
};
static void __attribute__((ms_abi)) u3_heap_free(uint64_t heap, uint64_t data, uint64_t image, uint64_t r14, const uint64_t *regs) {
    (void)regs;
    (void)heap; (void)image; (void)r14;
    static int hook=-1; /* BB_U3_FREE_HOOK=0: off (A/B tests) */
    if (hook<0) { const char *e=getenv("BB_U3_FREE_HOOK"); hook=!(e && e[0]=='0'); }
    if (!hook || !data) return;
    int32_t size;
    memcpy(&size,(const void *)(uintptr_t)(data-0xc),4);
    if (size<0) size=-size;
    if (size>0x10 && size<(64<<20)) bbgpu_note_guest_free((uintptr_t)data,(uint64_t)size-0x10);
}
/* U2 (EU 1.00) game/character-leg-ik.cpp:391, assertion IsFinite(deltaTransform): the root delta
 * the leg IK gets from its caller can be NaN for a frame (train wreck, chapter 1: issues #88,
 * #253) and the game stopped. The leg IK only adds its y to two accumulators, so a non-finite
 * delta now counts as no movement: it is zeroed (also in the caller's vector) and the update
 * goes on. */
static const unsigned char u2_leg_ik_assert_code[]={
    0xe8,0xa3,0xcb,0xbf,0xff,0x48,0x85,0xc0,0x74,0x27,0x48,0x8d,0x3d,0x1b,0xfe,0x25,0x00,0x48,0x8d,
    0x35,0x92,0xfc,0x25,0x00,0x48,0x8d,0x15,0x06,0xfe,0x25,0x00,0x48,0x8d,0x0d,0xd1,0xfc,0x25,0x00,
    0x41,0xb8,0x87,0x01,0x00,0x00,0x45,0x31,0xc9,0xff,0xd0,0xcd,0x41,
};
static const unsigned char u2_leg_ik_zero[]={
    0xc5,0xf8,0x57,0xc0,                          /* vxorps xmm0, xmm0, xmm0 */
    0xc4,0xc1,0x78,0x29,0x07,                     /* vmovaps [r15], xmm0 */
    0xeb,0x2d,                                    /* jmp (the update, 0x8ec650) */
    0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,
    0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,
    0xcc,0xcc,
};
_Static_assert(sizeof(u2_leg_ik_zero)==sizeof(u2_leg_ik_assert_code),"same size");
/* The same two assertions in US 1.02 (CUSA02320, as CUSA02344), where the assert handler comes from
 * a global (mov rax, [rip + ...]: 2 bytes longer); the blocks run to the reload after the trap.
 * UpdateFocus keeps the player's target at [rbp-0x60] and the camera at the same offsets as EU. */
static const unsigned char u2_focus_assert_code_us[]={ /* image+0x72778f */
    0x48,0x8b,0x05,0xe2,0x67,0xe4,0x00,0x48,0x85,0xc0,0x74,0x27,0x48,0x8d,0x3d,0x7d,0x4c,0x76,0x00,0x48,
    0x8d,0x35,0x2c,0x4b,0x76,0x00,0x48,0x8d,0x15,0x8c,0x4c,0x76,0x00,0x48,0x8d,0x0d,0xbd,0xcb,0x6c,0x00,
    0x41,0xb8,0x4d,0x04,0x00,0x00,0x45,0x31,0xc9,0xff,0xd0,0xcd,0x41,0xc5,0xf8,0x28,0x45,0xd0,
};
static const unsigned char u2_focus_reset_us[]={
    0xc5,0xf8,0x28,0x45,0xa0,                     /* vmovaps xmm0, [rbp-0x60] (the target) */
    0x0f,0x2e,0xc0,0x7a,0x2e,                     /* ucomiss xmm0, xmm0; jp trap */
    0xc5,0xf8,0x29,0x83,0xb0,0x02,0x00,0x00,      /* vmovaps [rbx+0x2b0], xmm0 */
    0xc4,0xe3,0x79,0x17,0x83,0xa4,0x02,0x00,0x00,0x01, /* vextractps [rbx+0x2a4], xmm0, 1 */
    0xc5,0xf8,0x29,0x45,0xd0,                     /* vmovaps [rbp-0x30], xmm0 */
    0xc5,0xf0,0x57,0xc9,                          /* vxorps xmm1, xmm1, xmm1 */
    0xc5,0xf8,0x29,0x8b,0xc0,0x02,0x00,0x00,      /* vmovaps [rbx+0x2c0], xmm1 */
    0x83,0xa3,0xa8,0x02,0x00,0x00,0x00,           /* and dword [rbx+0x2a8], 0 */
    0xeb,0x04,                                    /* jmp (store the focus, image+0x7277c9) */
    0xcc,0xcc,
    0xcd,0x41,                                    /* trap: int 0x41 */
};
_Static_assert(sizeof(u2_focus_reset_us)==sizeof(u2_focus_assert_code_us),"same size");
static const unsigned char u2_leg_ik_assert_code_us[]={ /* image+0x4713b9 */
    0x48,0x8b,0x05,0xb8,0xcb,0x0f,0x01,0x48,0x85,0xc0,0x74,0x27,0x48,0x8d,0x3d,0xf8,0xa2,0xa0,0x00,0x48,
    0x8d,0x35,0xc3,0xa1,0xa0,0x00,0x48,0x8d,0x15,0xd4,0x86,0xa5,0x00,0x48,0x8d,0x0d,0x93,0x2f,0x98,0x00,
    0x41,0xb8,0x87,0x01,0x00,0x00,0x45,0x31,0xc9,0xff,0xd0,0xcd,0x41,0xc4,0xc1,0x78,0x28,0x07,
};
static const unsigned char u2_leg_ik_zero_us[]={
    0xc5,0xf8,0x57,0xc0,                          /* vxorps xmm0, xmm0, xmm0 */
    0xc4,0xc1,0x78,0x29,0x07,                     /* vmovaps [r15], xmm0 */
    0xeb,0x2f,                                    /* jmp (the update, image+0x4713f3) */
    0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,
    0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,
    0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,
};
_Static_assert(sizeof(u2_leg_ik_zero_us)==sizeof(u2_leg_ik_assert_code_us),"same size");
/* U3 Player::UpdateFocus (player.cpp:1897, assertion m_lookAt.X() == m_lookAt.X(): the crash
 * climbing the fort in chapter 8). The point Nate looks at ([player+0xef0], velocity +0xf00)
 * springs towards a target; once NaN it stays NaN, and the function's own reset (taken when the
 * point is far off) never runs for it: NaN compares false. The assertion's call now takes that
 * reset when the target is a number (the velocity register zeroed first, as on the reset's usual
 * way in); a NaN target still stops at the trap. BilalReloaded's fix for 1.02 (upstream 0.3.9),
 * and upstream's for 1.00: EU image+0x3908b8 (reset at 0x390779, target at rbp-0xa0, zero in
 * xmm4), US and EU/Russian 1.02 image+0xbcf3ba (reset at 0xbcf27a, target at rbp-0x80, xmm5). */
static const unsigned char u3_focus_assert_code[]={
    0xe8,0xc3,0xc1,0x10,0x00,0x48,0x85,0xc0,0x74,0x27,0x48,0x8d,0x3d,0xd6,0x98,0xc1,0x00,0x48,0x8d,0x35,
    0x05,0x95,0xc1,0x00,0x48,0x8d,0x15,0xe5,0x98,0xc1,0x00,0x48,0x8d,0x0d,0x3b,0x95,0xc1,0x00,0x41,0xb8,
    0x69,0x07,0x00,0x00,0x45,0x31,0xc9,0xff,0xd0,0xcd,0x41,
};
static const unsigned char u3_focus_reset[]={
    0xc5,0xd8,0x57,0xe4,                          /* vxorps xmm4, xmm4, xmm4 */
    0xc5,0xf8,0x28,0x85,0x60,0xff,0xff,0xff,      /* vmovaps xmm0, [rbp-0xa0] (the target) */
    0xc5,0xf8,0xc2,0xc8,0x00,                     /* vcmpeqps xmm1, xmm0, xmm0 */
    0xc5,0xf8,0x50,0xc1,                          /* vmovmskps eax, xmm1 */
    0x83,0xf8,0x0f,                               /* cmp eax, 15 */
    0x0f,0x84,0xa3,0xfe,0xff,0xff,                /* je the reset (0x390779) */
    0xcd,0x41,                                    /* the target is NaN too: trap as before */
    0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,
};
_Static_assert(sizeof(u3_focus_reset)==sizeof(u3_focus_assert_code),"same size");
static const unsigned char u3_focus_assert_code_us[]={
    0x48,0x8b,0x05,0xf7,0x33,0x02,0x01,0x48,0x85,0xc0,0x74,0x27,0x48,0x8d,0x3d,0x74,0xff,0x89,0x00,0x48,
    0x8d,0x35,0x46,0xfd,0x89,0x00,0x48,0x8d,0x15,0x83,0xff,0x89,0x00,0x48,0x8d,0x0d,0x6e,0xce,0x7d,0x00,
    0x41,0xb8,0x6a,0x07,0x00,0x00,0x45,0x31,0xc9,0xff,0xd0,0xcd,0x41,
};
static const unsigned char u3_focus_reset_us[]={
    0xc5,0xd0,0x57,0xed,                          /* vxorps xmm5, xmm5, xmm5 */
    0xc5,0xf8,0x28,0x45,0x80,                     /* vmovaps xmm0, [rbp-0x80] (the target) */
    0xc5,0xf8,0xc2,0xc8,0x00,                     /* vcmpeqps xmm1, xmm0, xmm0 */
    0xc5,0xf8,0x50,0xc1,                          /* vmovmskps eax, xmm1 */
    0x83,0xf8,0x0f,                               /* cmp eax, 15 */
    0x0f,0x84,0xa5,0xfe,0xff,0xff,                /* je the reset (0xbcf27a) */
    0xcd,0x41,                                    /* the target is NaN too: trap as before */
    0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,
    0xcc,0xcc,0xcc,0xcc,
};
_Static_assert(sizeof(u3_focus_reset_us)==sizeof(u3_focus_assert_code_us),"same size");
static const RuntimeGuestCallback guest_callbacks[]={
    {"U3 heap free (GPU data)",0x494a70,u3_heap_free_code,sizeof(u3_heap_free_code),6,u3_heap_free,NULL},
    {"U3 heap free (GPU data, US)",0xd9ce70,u3_heap_free_us_code,sizeof(u3_heap_free_us_code),6,u3_heap_free,NULL},
    {"U1 follow camera (mouse)",0xaecf0,u1_follow_camera_code,sizeof(u1_follow_camera_code),6,u1_follow_camera,NULL},
    {"U1 pad update (mouse)",0x117200,u1_pad_update_code,sizeof(u1_pad_update_code),6,u1_pad_update,NULL},
    {"U1 joypad sticks (mouse)",0x11784e,u1_joypad_sticks_code,sizeof(u1_joypad_sticks_code),5,u1_joypad_sticks,NULL},
    {"U1 1.02 follow camera (mouse)",0xaede0,u1_102_follow_camera_code,sizeof(u1_102_follow_camera_code),6,u1_follow_camera,NULL},
    {"U1 1.02 pad update (mouse)",0x117350,u1_102_pad_update_code,sizeof(u1_102_pad_update_code),6,u1_pad_update,NULL},
    {"U1 1.02 joypad sticks (mouse)",0x11799e,u1_joypad_sticks_code,sizeof(u1_joypad_sticks_code),5,u1_joypad_sticks,NULL},
    {"U1 1.02 aim controller (mouse)",0x276a10,u1_102_aim_controller_c_code,sizeof(u1_102_aim_controller_c_code),6,u1_102_aim_controller_c,NULL},
    {"U2 1.02 aim controller (mouse)",0x71a1b0,u2_102_aim_controller_code,sizeof(u2_102_aim_controller_code),6,u2_102_aim_controller,NULL},
    {"U3 1.02 aim controller (mouse)",0x9285c0,u3_102_aim_controller_code,sizeof(u3_102_aim_controller_code),6,u3_102_aim_controller,NULL},
    {"U2 follow camera (mouse)",0x171fe0,u2_follow_camera_code,sizeof(u2_follow_camera_code),6,u2_follow_camera,NULL},
    {"U3 follow camera (mouse)",0x121b90,u3_follow_camera_code,sizeof(u3_follow_camera_code),6,u3_follow_camera,NULL},
    {"U2 1.02 follow camera (mouse)",0x42c690,u2_102_follow_camera_code,sizeof(u2_102_follow_camera_code),6,u2_follow_camera,NULL},
    {"U1 1.02 cover camera (mouse)",0xa4670,u1_102_cover_camera_code,sizeof(u1_102_cover_camera_code),6,u1_102_cover_camera,NULL},
    {"U2 1.02 cover camera (mouse)",0x417450,u2_102_cover_camera_code,sizeof(u2_102_cover_camera_code),6,u2_102_cover_camera,NULL},
    {"U3 1.02 cover camera (mouse)",0x4cd030,u3_102_cover_camera_code,sizeof(u3_102_cover_camera_code),6,u3_102_cover_camera,NULL},
    {"U3 1.02 follow camera (mouse)",0x4e7970,u3_102_follow_camera_code,sizeof(u3_102_follow_camera_code),6,u3_follow_camera,NULL},
    {"U1 EU aim controller (mouse)",0x2764c0,u1_eu_aim_controller_code,sizeof(u1_eu_aim_controller_code),6,u1_eu_aim_controller,NULL},
    {"U2 EU aim controller (mouse)",0x334220,u2_eu_aim_controller_code,sizeof(u2_eu_aim_controller_code),6,u2_eu_aim_controller,NULL},
    {"U3 EU aim controller (mouse)",0x317740,u3_eu_aim_controller_code,sizeof(u3_eu_aim_controller_code),6,u3_eu_aim_controller,NULL},
    {"U1 EU cover camera (mouse)",0xa4590,u1_eu_cover_camera_code,sizeof(u1_eu_cover_camera_code),6,u1_102_cover_camera,NULL},
    {"U2 EU cover camera (mouse)",0x161070,u2_eu_cover_camera_code,sizeof(u2_eu_cover_camera_code),6,u2_102_cover_camera,NULL},
    {"U3 EU cover camera (mouse)",0x10e6d0,u3_eu_cover_camera_code,sizeof(u3_eu_cover_camera_code),6,u3_102_cover_camera,NULL},
    {"U2 camera focus NaN reset",0x33ba13,u2_focus_assert_code,sizeof(u2_focus_assert_code),
     sizeof(u2_focus_reset),NULL,u2_focus_reset},
    {"U2 leg IK NaN delta",0x8ec618,u2_leg_ik_assert_code,sizeof(u2_leg_ik_assert_code),
     sizeof(u2_leg_ik_zero),NULL,u2_leg_ik_zero},
    {"U2 camera focus NaN reset (US)",0x72778f,u2_focus_assert_code_us,sizeof(u2_focus_assert_code_us),
     sizeof(u2_focus_reset_us),NULL,u2_focus_reset_us},
    {"U2 leg IK NaN delta (US)",0x4713b9,u2_leg_ik_assert_code_us,sizeof(u2_leg_ik_assert_code_us),
     sizeof(u2_leg_ik_zero_us),NULL,u2_leg_ik_zero_us},
    {"U2 move action pool 256 slots",0x33872c,u2_move_pool_code,sizeof(u2_move_pool_code),
     sizeof(u2_move_pool_256),NULL,u2_move_pool_256},
    {"U1 move action pool 256 slots",0x2668fe,u1_move_pool_code,sizeof(u1_move_pool_code),
     sizeof(u1_move_pool_256),NULL,u1_move_pool_256},
    {"U1 move action pool 256 slots (US)",0x266e6e,u1_move_pool_code,sizeof(u1_move_pool_code),
     sizeof(u1_move_pool_256),NULL,u1_move_pool_256},
    {"U2 move action pool 256 slots (US)",0x721347,u2_move_pool_code_us,sizeof(u2_move_pool_code_us),
     sizeof(u2_move_pool_256_us),NULL,u2_move_pool_256_us},
    {"U3 player focus NaN reset",0x3908b8,u3_focus_assert_code,sizeof(u3_focus_assert_code),
     sizeof(u3_focus_reset),NULL,u3_focus_reset},
    {"U3 player focus NaN reset (US)",0xbcf3ba,u3_focus_assert_code_us,sizeof(u3_focus_assert_code_us),
     sizeof(u3_focus_reset_us),NULL,u3_focus_reset_us},
};
const RuntimeGuestCallback *runtime_guest_callbacks(size_t *count) {
    *count=sizeof(guest_callbacks)/sizeof(*guest_callbacks);
    return guest_callbacks;
}

/* The button pictures for this controller: 1 PlayStation (DualShock, DualSense: the game's own),
 * 2 Xbox (everything else; the buttons are named by position, so a Switch pad is shown as the
 * Xbox layout). */
static int prompts_pad_device(SDL_Gamepad *g) {
    const SDL_GamepadType type=SDL_GetGamepadType(g);
    if (type==SDL_GAMEPAD_TYPE_PS3 || type==SDL_GAMEPAD_TYPE_PS4 || type==SDL_GAMEPAD_TYPE_PS5) return 1;
    return SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_SOUTH)==SDL_GAMEPAD_BUTTON_LABEL_CROSS ? 1 : 2;
}

static void injected_mouse(float *dx, float *dy);
static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=runtime_guest_monotonic_us(); /* the game's clock (BB_TIME_SCALE) */
    static int loaded;
    if (!loaded) { load_input(); loaded=1; }
    SDL_Gamepad *g=current_gamepad();
    float mdx=0, mdy=0;
    uint32_t mouse=0;
    int wheel=0, captured=bbgpu_mouse_sample(&mdx,&mdy,&mouse,&wheel);
    if (bbgpu_overlay_captures_input()) return; /* settings menu open: neutral input */
    /* Scripted runs (BB_PAD_FILE) ignore the player's controller and keyboard: a pad in use on
     * the same PC reached the test window (BB_PAD_HOST=1 keeps them). */
    static int scripted=-1;
    if (scripted<0) {
        const char *file=getenv("BB_PAD_FILE"), *host=getenv("BB_PAD_HOST");
        scripted=file && *file && !(host && host[0]=='1');
    }
    if (scripted) { injected_mouse(&mdx,&mdy); captured=1; mouse=0; wheel=0; }
    const uint64_t now=now_us();
    int rx=0, ry=0; /* the keyboard's right stick */
    if (!scripted) { /* the player's controller and keyboard */
        if (wheel>0) wheel_until_us[0]=now+80000;
        if (wheel<0) wheel_until_us[1]=now+80000;
        const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    static int layout_logged;
    if (k && !layout_logged) { layout_logged=1; log_layout_keys(); }
        if (g) {
            for (int a=0;a<16;++a) {
                const int b=input.pad[a];
                if (b>=0 && SDL_GetGamepadButton(g,(SDL_GamepadButton)b)) d->buttons|=actions[a].ps;
                else if (b==PAD_TRIGGER_LEFT || b==PAD_TRIGGER_RIGHT) {
                    const uint8_t v=trigger(SDL_GetGamepadAxis(g,b==PAD_TRIGGER_LEFT ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                                                                  : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
                    if (v>30) d->buttons|=actions[a].ps;
                    if (actions[a].ps==BTN_L2 && v>d->l2) d->l2=v;
                    if (actions[a].ps==BTN_R2 && v>d->r2) d->r2=v;
                }
            }
            if (SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_TOUCHPAD)) d->buttons|=BTN_TOUCHPAD; /* DualSense/DS4 click */
            if ((d->buttons & BTN_L2) && !d->l2) d->l2=255;
            if ((d->buttons & BTN_R2) && !d->r2) d->r2=255;
            d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
            d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
            if (input.pad_invert_y) d->right_y=(uint8_t)(255-d->right_y);
            if (SDL_GetNumGamepadTouchpads(g)>0) {
                const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
                for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                    bool down=false;
                    float x=0, y=0;
                    if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                        d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                            .y=touch_axis(y,942),.id=(uint8_t)finger};
                    }
                }
            }
        }
    /* The device used last picks the button prompts: the game's PlayStation buttons or pictures
     * of the bound keys (bbgpu_prompts_controller). A stick counts past half way, the mouse when
     * it moves more than a few pixels. */
    const int pad_used=g && (d->buttons || d->l2>60 || d->r2>60 || abs(d->left_x-128)>64 ||
                             abs(d->left_y-128)>64 || abs(d->right_x-128)>64 || abs(d->right_y-128)>64);
    int keys_used=mouse || wheel || (captured && SDL_fabsf(mdx)+SDL_fabsf(mdy)>3.0f);
    for (int a=0;a<ACTIONS && !keys_used;++a) keys_used=action_down(a,k,mouse,now);
    static int prompts_device=-1;
    if (pad_used!=keys_used) {
        const int device=pad_used ? prompts_pad_device(g) : 0;
        if (device!=prompts_device) {
            prompts_device=device;
            bbgpu_prompts_controller(device);
            keyboard_in_use=device==0;
            if (keyboard_in_use && g) SDL_RumbleGamepad(g,0,0,0); /* the pad put down stops shaking */
        }
    }
        /* Keyboard and mouse, together with the controller. */
        for (int a=0;a<16;++a) if (action_down(a,k,mouse,now)) d->buttons|=actions[a].ps;
        if (d->buttons & BTN_L2) d->l2=255;
        if (d->buttons & BTN_R2) d->r2=255;
        // The touchpad button without a finger on a touch surface is a left-side click.
        if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
        const int lx=action_down(A_LS_RIGHT,k,mouse,now)-action_down(A_LS_LEFT,k,mouse,now);
        const int ly=action_down(A_LS_DOWN,k,mouse,now)-action_down(A_LS_UP,k,mouse,now);
        if (lx) d->left_x=(uint8_t)(lx<0 ? 0 : 255);
        if (ly) d->left_y=(uint8_t)(ly<0 ? 0 : 255);
        rx=action_down(A_RS_RIGHT,k,mouse,now)-action_down(A_RS_LEFT,k,mouse,now);
        ry=action_down(A_RS_DOWN,k,mouse,now)-action_down(A_RS_UP,k,mouse,now);
        if (rx) d->right_x=(uint8_t)(rx<0 ? 0 : 255);
        if (ry) d->right_y=(uint8_t)(ry<0 ? 0 : 255);
    }
    /* Where the game's camera or joypad state takes the mouse directly (their callbacks ran in the
     * last 100 ms), the motion goes there. U2 and U3 have no joypad callback: the follow camera turns
     * with the mouse (vertically too where its arc is known), with the right stick held a little the
     * same way (follow_push); aiming, other cameras and a follow camera that keeps no turns get the
     * mouse as a right stick here. */
    pthread_mutex_lock(&camera_lock);
    const int following=camera_hook_us && now-camera_hook_us<100000;
    const int joypad=sticks_hook_us && now-sticks_hook_us<100000;
    const int aiming=aim_held && aim_hook_us && now-aim_hook_us<100000; /* raw aim takes the mouse */
    const int cover=cover_hook_us && now-cover_hook_us<100000;
    for (int q=0;q<2;++q) {
        if (captured && (following || joypad || cover)) { camera_dx[q]+=mdx; camera_dy[q]+=mdy; }
        else { camera_dx[q]=camera_dy[q]=0; }
    }
    if (captured && aim_held && raw_aim_enabled()) { camera_dx[2]+=mdx; camera_dy[2]+=mdy; }
    else { camera_dx[2]=camera_dy[2]=0; }
    /* kept while aiming: at low frame rates the aim controller runs only every 100-200 ms */
    if (captured && raw_aim_armed(now)) { aim_dx+=mdx; aim_dy+=mdy; }
    else { aim_dx=aim_dy=0.0f; }
    pthread_mutex_unlock(&camera_lock);
    const int raw_aiming=raw_aim_armed(now);
    const int direct_x=joypad || aiming || raw_aiming || (cover && !aim_held) || (following && !aim_held && follow_direct);
    const int direct_y=joypad || aiming || raw_aiming || (cover && !aim_held) ||
                       (following && !aim_held && follow_direct && follow_has_arc);
    if (captured && direct_x && direct_y) {
        mouse_vx=mouse_vy=0; mouse_last_us=0;
    } else if (captured) {
        float dt=mouse_last_us ? (float)(now-mouse_last_us)*1e-6f : 0.0f;
        mouse_last_us=now;
        if (dt>0.0f) {
            if (dt>0.1f) dt=0.1f;
            /* turn rate in degrees/s, smoothed over 12 ms (mouse reports arrive in bursts) */
            const float alpha=1.0f-SDL_expf(-dt/0.012f), scale=MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
            mouse_vx+=alpha*(mdx/dt*scale-mouse_vx);
            mouse_vy+=alpha*((input.mouse_invert_y ? -mdy : mdy)/dt*scale-mouse_vy);
            if (mdx==0 && mouse_vx>-1.0f && mouse_vx<1.0f) mouse_vx=0;
            if (mdy==0 && mouse_vy>-1.0f && mouse_vy<1.0f) mouse_vy=0;
        }
        if (mouse_vx!=0 && !rx && !direct_x) d->right_x=mouse_axis(mouse_vx);
        if (mouse_vy!=0 && !ry && !direct_y) d->right_y=mouse_axis(mouse_vy);
    } else {
        mouse_vx=mouse_vy=0; mouse_last_us=0;
    }
}

/* BB_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad defaults to a left-side click; lx= ly= rx= ry= (0..255) override
 * the sticks. An empty file releases everything. */
static struct { uint32_t buttons; int stick[4]; int touch_side; float mouse[2]; } injected={0,{-1,-1,-1,-1},-1,{0,0}};
static int replay_armed;      /* 1 while a BB_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
/* Scripted mouse motion (mx= my=, pixels/s) as this sample's mouse movement (sample_host). */
static void injected_mouse(float *dx, float *dy) {
    static uint64_t last;
    const uint64_t now=now_us();
    float dt=last ? (float)(now-last)*1e-6f : 0.0f;
    if (dt>0.1f) dt=0.1f;
    last=now;
    *dx=injected.mouse[0]*dt; *dy=injected.mouse[1]*dt;
    scripted_rate[0]=injected.mouse[0]; scripted_rate[1]=injected.mouse[1];
}
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("BB_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const struct { const char *name; uint32_t ps; } names[]={
        {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
        {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
        {"options",BTN_OPTIONS}, {"touchpad",BTN_TOUCHPAD},
        {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    };
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    injected.touch_side=-1;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    injected.mouse[0]=injected.mouse[1]=0.0f;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* BB_PAD_REPLAY */
        if (!strcmp(token,"touchpad_left") || !strcmp(token,"touchpad_right")) {
            injected.buttons|=BTN_TOUCHPAD;
            injected.touch_side=!strcmp(token,"touchpad_right");
        }
        for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) if (!strcmp(token,names[i].name)) injected.buttons|=names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
        if (!strncmp(token,"mx=",3)) injected.mouse[0]=(float)atof(token+3); /* mouse motion, pixels/s */
        if (!strncmp(token,"my=",3)) injected.mouse[1]=(float)atof(token+3);
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d mouse %g %g (t=%.4f)\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3],injected.mouse[0],
           injected.mouse[1],now*1e-6);
}
/* BB_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; BB_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in BB_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("BB_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("BB_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* BB_DEV_MENU=1 (experiment, Uncharted 3 1.02, EU and US): F11 opens and closes Naughty Dog's developer menu,
 * which the retail game builds but gives no way to open: the menu's "open" byte (the camera code checks it) is
 * flipped. Per build, the root pointers of both menus and where their names lie (the names tell which build
 * this is; any other game or release is left alone). US = CUSA02320 / CUSA02344 (one executable). */
static const struct { uintptr_t dev_name, quick_name, dev_root, quick_root; } dev_menu_builds[]={
    {0xf60a72,0xfacfe0,0x142ee88,0x196cdd0},   /* EU CUSA02343 */
    {0x1458d8a,0x1470aa3,0x17b4a38,0x1ae9ef8}, /* US CUSA02320, CUSA02344 */
};
static void dev_menu_key(void) {
    static int checked, on, f11_was_down, f8_was_down;
    if (!checked) { const char *e=getenv("BB_DEV_MENU"); on=e && e[0]=='1'; checked=1; }
    if (!on || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f11=k && k[SDL_SCANCODE_F11], f8=k && k[SDL_SCANCODE_F8];
    /* F8: the "Quick Menu" (player and actor tools: Invincible Player, Reset Actors, ...), the game's second
     * debug menu, built at the same start-up step (root pointers in dev_menu_builds). (Shift is a game
     * key, taken by the keyboard bindings, so it cannot pick the menu.) */
    const int quick=f8 && !f8_was_down;
    if ((f11 && !f11_was_down) || quick) {
        const char *name=quick ? "Quick menu" : "Dev menu";
        const uintptr_t base=runtime_image_base();
        int build=-1;
        for (int b=0;base && b<(int)(sizeof(dev_menu_builds)/sizeof(dev_menu_builds[0])) && build<0;++b)
            if (!memcmp((const void *)(base+dev_menu_builds[b].dev_name),"Dev Menu",9) &&
                !memcmp((const void *)(base+dev_menu_builds[b].quick_name),"Quick Menu",11)) build=b;
        if (build<0) {
            printf("%s: not this game or release (the offsets are for Uncharted 3 1.02, EU and US)\n",name);
        } else {
            unsigned char *menu=*(unsigned char **)(base+(quick ? dev_menu_builds[build].quick_root
                                                                  : dev_menu_builds[build].dev_root));
            if (!menu) printf("%s: not built yet\n",name);
            else {
                menu[0x90]^=1;
                printf("%s: %s (menu at %p)\n",name,menu[0x90] ? "opened" : "closed",(void *)menu);
            }
        }
        fflush(stdout);
    }
    f11_was_down=f11;
    f8_was_down=f8;
}
/* After the menu or the text dialog closes, buttons still held (the Cross that accepted a
 * name) stay hidden until released: the game would take them as a new press. */
static int hold_after_capture;
static void sample(PadData *d) {
    sample_host(d);
    if (bbgpu_overlay_captures_input()) { hold_after_capture=1; return; }
    dev_menu_key();
    record_sample(d);
    read_inject();
    replay_sample(d);
    d->buttons|=injected.buttons;
    {
        const int held=(d->buttons & BTN_L2)!=0;
        if (mouse_debug() && held!=aim_held) printf("Mouse: L2 %s (t=%.4f)\n",held ? "held" : "released",now_us()*1e-6);
        if (held && !aim_held) aim_press_us=now_us();
        aim_held=held;
    }
    if (injected.touch_side>=0) touch_click(d,injected.touch_side);
    else if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    /* U2, U3 (no joypad callback): the right stick held a little while the mouse turns the follow
     * camera's direction (follow_push, see follow_camera), unless the stick is in use */
    const uint64_t now=now_us();
    if (follow_push!=0.0f && camera_hook_us && now-camera_hook_us<100000 &&
        !(sticks_hook_us && now-sticks_hook_us<100000) && !aim_held && d->right_x>=118 && d->right_x<=138)
        d->right_x=(uint8_t)(128.0f+follow_push*127.5f);
    if (hold_after_capture) {
        if (d->buttons) d->buttons=0;
        else hold_after_capture=0;
    }
}

static ABI int32_t pad_init(void) { pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    int already=opened; opened=1;
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    opened=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    sample(data); ++reads;
    pthread_mutex_unlock(&lock);
    /* BB_PAD_TIMING=1 (diagnosis): every read with the same clock as BB_CAMERA_LOG (steady clock = QPC). */
    static int timing=-1;
    if (timing<0) timing=getenv("BB_PAD_TIMING")!=NULL;
    if (timing) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
        printf("PadRead: t=%.4f rx=%u\n",(double)ts.tv_sec+ts.tv_nsec*1e-9,data->right_x);
        runtime_wait_print((double)ts.tv_sec+ts.tv_nsec*1e-9);
    }
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    pthread_mutex_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    if (g && !keyboard_in_use) SDL_RumbleGamepad(g,(uint16_t)(param[0]*257),(uint16_t)(param[1]*257),1000);
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Light bar {r, g, b}: shown on controllers that have one (DualShock 4, DualSense). */
static void set_led(uint8_t r, uint8_t g, uint8_t b) {
    pthread_mutex_lock(&lock);
    SDL_Gamepad *pad=current_gamepad();
    if (pad) SDL_SetGamepadLED(pad,r,g,b);
    pthread_mutex_unlock(&lock);
}
static ABI int32_t pad_light_bar(int32_t handle, const uint8_t *rgb) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!rgb) return ERR_INVALID_ARG;
    set_led(rgb[0],rgb[1],rgb[2]);
    return 0;
}
static ABI int32_t pad_reset_light_bar(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    set_led(0,0,255); /* user 1 colour */
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

static const RuntimeExport exports[]={
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
    {"scePadSetLightBar",pad_light_bar}, {"scePadResetLightBar",pad_reset_light_bar},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none"); }
