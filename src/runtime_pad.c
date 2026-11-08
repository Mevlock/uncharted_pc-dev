/* libScePad on SDL3 gamepads, keyboard and mouse. SDL events are pumped by the window thread
 * (gpu/shim/window.cpp); here state is only sampled. The controller, keyboard and mouse work
 * together; their bindings come from input.ini (BB_INPUT_CONFIG, written by the launcher):
 *
 *   key.<action>=<key or mouse name>[|<second>]   SDL key names ("Space", "Left Shift", "W"),
 *       "Mouse Left", "Mouse Right", "Mouse Middle", "Mouse 4", "Mouse 5", "Wheel Up",
 *       "Wheel Down"; actions: cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad
 *       up down left right, ls_up ls_down ls_left ls_right, rs_up rs_down rs_left rs_right,
 *       rotate_ccw rotate_cw (held: the left stick circles counter-clockwise / clockwise, for
 *       the valves and wheels that ask for the stick turned in circles), walk (held: the
 *       movement keys push the left stick only part of the way, so the character walks).
 *   pad.<action>=<controller button>   SDL names (a b x y leftshoulder rightshoulder
 *       lefttrigger righttrigger leftstick rightstick back start dpup dpdown dpleft dpright
 *       touchpad misc1 paddle1..paddle4) or "none", for the button actions above.
 *   mouse_look=1  mouse_sensitivity=1.0  mouse_invert_y=0  pad_invert_y=0
 *   mouse_aim_sensitivity=1.0 (times the sensitivity while aiming)  mouse_vertical=1.0 (times it for up/down)
 *   pad_vibration=1  pad_vibration_strength=100  walk_speed=45 (walk key deflection, percent)
 *
 * Defaults (no file): WASD move, mouse look, left button R2 (fire), right button L2 (aim),
 * Space Cross, C Circle, F Square, E Triangle, Q L1, G R1, Left Shift L3, V R3, Escape/Enter
 * Options, Tab touchpad, arrow keys and the wheel the d-pad, IJKL the right stick, R and T
 * turn the left stick round counter-clockwise and clockwise, Left Ctrl walks (the rotate and
 * walk keys by drakolordx7, from his fork: 46aec0d3, 6455d6bb). */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
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

/* The keyboard and mouse were used last (the prompts' device 0): the controller doesn't rumble
 * (antikmewzik's controls fork). */
static int keyboard_in_use;
/* The pad is sampled when the game reads it, so a press and release between two reads was never
 * seen (BilalReloaded's port #309: the Treasure Vault lever in Uncharted 1 asks to mash Triangle).
 * Every button that went down since the last read counts as pressed in that read (SDL event watch
 * on the window thread; read and cleared in sample_host). BB_PAD_LATCH=0: off. */
static unsigned long long pad_taps; /* bit = SDL_GamepadButton, set on BUTTON_DOWN */
static bool SDLCALL pad_tap_watch(void *userdata, SDL_Event *event) {
    (void)userdata;
    /* (any open pad: a press on another one makes it the pad in use, pad_pressed) */
    if (event->type==SDL_EVENT_GAMEPAD_BUTTON_DOWN && event->gbutton.button<64)
        __atomic_or_fetch(&pad_taps,1ull<<event->gbutton.button,__ATOMIC_RELAXED);
    return true;
}
static int pad_latch_enabled(void) {
    static int value=-1;
    if (value<0) { const char *e=getenv("BB_PAD_LATCH"); value=!(e && e[0]=='0'); }
    return value;
}
/* A controller switched off during play stuttered the game: every pad read (on the game's thread) looked for one
 * and tried to open it, and a Bluetooth pad that is going away can still be listed for a while, with each open or
 * rumble to it blocking for milliseconds. Now a pad is looked for when SDL reports one added, or at most once a
 * second; a removed one is closed at once; rumble and the light bar back off for 2 s after a slow write. Calls
 * slower than 2 ms are logged ("Runtime: pad ... took"). */
static volatile int pad_added=1, pad_removed;
static volatile SDL_JoystickID pad_pressed; /* the pad that had the last button press */
static uint64_t next_pad_scan_us, pad_writes_after_us;
static bool SDLCALL pad_device_watch(void *userdata, SDL_Event *event) {
    (void)userdata;
    if (event->type==SDL_EVENT_GAMEPAD_ADDED) __atomic_store_n(&pad_added,1,__ATOMIC_RELEASE);
    else if (event->type==SDL_EVENT_GAMEPAD_REMOVED) __atomic_store_n(&pad_removed,1,__ATOMIC_RELEASE);
    else if (event->type==SDL_EVENT_GAMEPAD_BUTTON_DOWN) __atomic_store_n(&pad_pressed,event->gbutton.which,__ATOMIC_RELEASE);
    return true;
}
static void pad_slow(const char *what, uint64_t start) {
    const uint64_t took=now_us()-start;
    if (took>2000) {
        static unsigned noted;
        if (noted++<20) printf("Runtime: pad %s took %.1f ms\n",what,took/1000.0);
        if (took>8000 && (what[0]=='r' || what[0]=='l')) pad_writes_after_us=now_us()+2000000; /* rumble, light */
    }
}
/* Every connected controller stays open and the game reads the one in use: the last that had a button pressed or a
 * stick or trigger pushed past half way (from spacepepperoni's Linux port, 3329bf97). Only the first one SDL listed
 * was read before, so a virtual or hidden pad listed first (Steam Input, DS4Windows, a handheld's built-in
 * controller) won and the controller in the player's hands gave no input (upstream #629, #682, #653, #765, #762).
 * A pad becomes the one in use on a press or a push (an edge), so a pad that reports a stick held all the time cannot
 * keep taking over. With one controller nothing changes. Rumble and the light bar go to the pad in use.
 * A virtual mirror of the pad in the player's hands (Steam Input, DS4Windows) gets the same presses a few ms apart:
 * another pad takes over only when the one in use has had no press or push for PAD_QUIET_US, and none in the
 * PAD_CONFIRM_US after the other pad's (so the same input on both keeps the pad in use; before, every press could
 * swap them, each swap stopping the old pad's rumble and moving the light bar). */
#define MAX_GAMEPADS 8
#define PAD_QUIET_US 400000u
#define PAD_CONFIRM_US 60000u
static SDL_Gamepad *gamepads[MAX_GAMEPADS];
static int gamepad_count;
static unsigned gamepad_pushed[MAX_GAMEPADS]; /* bit = axis past half way at the last look */
static unsigned long long gamepad_held[MAX_GAMEPADS]; /* bit = SDL_GamepadButton held at the last look */
static uint64_t gamepad_edge_us[MAX_GAMEPADS]; /* the last press or push (an edge) */
static SDL_Gamepad *rumbled_pad;  /* the pad last given a rumble that may still run, until rumbled_until_us */
static uint64_t rumbled_until_us;
static unsigned long long pad_buttons_held(SDL_Gamepad *g) {
    unsigned long long mask=0;
    for (int b=0;b<SDL_GAMEPAD_BUTTON_COUNT && b<64;++b)
        if (SDL_GetGamepadButton(g,(SDL_GamepadButton)b)) mask|=1ull<<b;
    return mask;
}
static unsigned pad_axes_pushed(SDL_Gamepad *g) {
    unsigned mask=0;
    for (int a=0;a<SDL_GAMEPAD_AXIS_COUNT;++a) {
        const int v=SDL_GetGamepadAxis(g,(SDL_GamepadAxis)a);
        if (v>16384 || v<-16384) mask|=1u<<a;
    }
    return mask;
}
/* Closes pads that went away and (open_new) opens new ones. */
static void list_gamepads(int open_new) {
    for (int i=0;i<gamepad_count;) {
        if (SDL_GamepadConnected(gamepads[i])) { ++i; continue; }
        printf("Runtime: gamepad disconnected\n");
        if (gamepads[i]==gamepad) gamepad=NULL;
        if (gamepads[i]==rumbled_pad) rumbled_pad=NULL;
        SDL_CloseGamepad(gamepads[i]);
        --gamepad_count;
        gamepads[i]=gamepads[gamepad_count];
        gamepad_pushed[i]=gamepad_pushed[gamepad_count];
        gamepad_held[i]=gamepad_held[gamepad_count];
        gamepad_edge_us[i]=gamepad_edge_us[gamepad_count];
    }
    if (!open_new) return;
    int count=0;
    SDL_JoystickID *ids=SDL_GetGamepads(&count);
    for (int k=0;ids && k<count && gamepad_count<MAX_GAMEPADS;++k) {
        int known=0;
        for (int i=0;i<gamepad_count && !known;++i) known=SDL_GetGamepadID(gamepads[i])==ids[k];
        if (known) continue;
        SDL_Gamepad *g=SDL_OpenGamepad(ids[k]);
        if (!g) continue;
        gamepad_pushed[gamepad_count]=pad_axes_pushed(g);
        gamepad_held[gamepad_count]=pad_buttons_held(g);
        gamepad_edge_us[gamepad_count]=0;
        gamepads[gamepad_count++]=g;
        ++connected_count;
        printf("Runtime: gamepad connected: %s\n",SDL_GetGamepadName(g));
    }
    SDL_free(ids);
}
static void use_gamepad(SDL_Gamepad *g) {
    if (g==gamepad) return;
    const uint64_t now=now_us();
    /* the old one stops, if it may still be rumbling */
    if (gamepad && gamepad==rumbled_pad && now<rumbled_until_us && now>=pad_writes_after_us) {
        SDL_RumbleGamepad(gamepad,0,0,0);
        rumbled_pad=NULL;
    }
    gamepad=g;
    /* at most one line every 10 s, with the switches not logged in between */
    static uint64_t logged_us;
    static unsigned noted, unlogged;
    if (noted<50 && (!logged_us || now-logged_us>=10000000u)) {
        ++noted;
        logged_us=now;
        if (unlogged) printf("Runtime: gamepad in use: %s (%u switches not logged)\n",SDL_GetGamepadName(g),unlogged);
        else printf("Runtime: gamepad in use: %s\n",SDL_GetGamepadName(g));
        unlogged=0;
    } else ++unlogged;
}
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) {
        sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
        if (sdl_ready>0 && pad_latch_enabled()) SDL_AddEventWatch(pad_tap_watch,NULL);
        if (sdl_ready>0) SDL_AddEventWatch(pad_device_watch,NULL);
    }
    if (sdl_ready<0) return NULL;
    const uint64_t now=now_us();
    const int removed=__atomic_exchange_n(&pad_removed,0,__ATOMIC_ACQ_REL);
    const int added=__atomic_exchange_n(&pad_added,0,__ATOMIC_ACQ_REL);
    if (removed || (gamepad && !SDL_GamepadConnected(gamepad))) {
        /* (a pad going away can still be listed for a while: new ones are looked for on an added event or a second later) */
        list_gamepads(0);
        next_pad_scan_us=now+1000000;
    }
    if (added || (!gamepad_count && now>=next_pad_scan_us)) {
        next_pad_scan_us=now+1000000;
        list_gamepads(1);
        pad_slow("open",now);
    }
    if (!gamepad && gamepad_count) use_gamepad(gamepads[0]);
    if (gamepad_count>1) {
        /* candidate: another pad that had an edge while the one in use was quiet; it takes over once the one in use
         * stays quiet for PAD_CONFIRM_US more (a mirror's copy of the same press arrives in that time) */
        static SDL_Gamepad *candidate;
        static uint64_t candidate_us;
        const SDL_JoystickID pressed=__atomic_exchange_n(&pad_pressed,0,__ATOMIC_ACQ_REL);
        uint64_t current_edge=0;
        int candidate_open=0;
        SDL_Gamepad *next=NULL;
        for (int i=0;i<gamepad_count;++i) {
            const unsigned pushed=pad_axes_pushed(gamepads[i]);
            const unsigned long long held=pad_buttons_held(gamepads[i]);
            if ((pushed & ~gamepad_pushed[i]) || (held & ~gamepad_held[i]) ||
                (pressed && SDL_GetGamepadID(gamepads[i])==pressed))
                gamepad_edge_us[i]=now;
            gamepad_pushed[i]=pushed;
            gamepad_held[i]=held;
            if (gamepads[i]==gamepad) current_edge=gamepad_edge_us[i];
            if (gamepads[i]==candidate) candidate_open=1;
        }
        if (!candidate_open) candidate=NULL;
        for (int i=0;i<gamepad_count && !candidate;++i)
            if (gamepads[i]!=gamepad && gamepad_edge_us[i]==now && (!current_edge || now-current_edge>=PAD_QUIET_US)) {
                candidate=gamepads[i];
                candidate_us=now;
            }
        if (candidate) {
            if (candidate==gamepad || (current_edge && current_edge+PAD_QUIET_US>candidate_us)) candidate=NULL; /* the one in use had the same input */
            else if (now-candidate_us>=PAD_CONFIRM_US) { next=candidate; candidate=NULL; }
        }
        if (next) use_gamepad(next);
    }
    return gamepad;
}
/* Rumble and the light bar: skipped for 2 s after a write that blocked (a pad going away). */
static void pad_rumble(SDL_Gamepad *g, uint16_t low, uint16_t high, uint32_t ms) {
    if (!g || now_us()<pad_writes_after_us) return;
    const uint64_t start=now_us();
    SDL_RumbleGamepad(g,low,high,ms);
    pad_slow("rumble",start);
    if (low || high) { rumbled_pad=g; rumbled_until_us=start+(uint64_t)ms*1000u; }
    else if (g==rumbled_pad) rumbled_pad=NULL;
}
/* ---- the pad thread (BB_PAD_THREAD=0: off) ------------------------------------------------------------------------
 * Switching a controller off or on makes SDL rescan the input devices for 100-300 ms with its joystick lock held.
 * Every SDL controller call waits for that lock: the window's event pump (which updated the controllers) and then the
 * game's pad reads stalled, one hitch per switch (0.3.6.44, measured 190-260 ms). SDL's controller update, opening and
 * closing the pad, reading it, rumble and the light bar now all run on a thread of their own; the game reads a copy of
 * the pad taken after each update (at most ~2 ms old), and rumble and the light bar are requests the thread carries
 * out. A rescan then holds up only this thread: the game keeps the last state it saw. */
typedef struct {
    int present, device;           /* device: the button pictures (prompts_pad_device) */
    unsigned long long buttons;    /* bit = SDL_GamepadButton */
    int16_t axes[SDL_GAMEPAD_AXIS_COUNT];
    int touch_count;
    struct { float x, y; uint8_t id; } touch[2];
} PadSnap;
static int pad_thread_on(void) {
    static int value=-1;
    if (value<0) { const char *e=getenv("BB_PAD_THREAD"); value=!(e && e[0]=='0'); }
    return value;
}
static int prompts_pad_device(SDL_Gamepad *g);
static SDL_Gamepad *current_gamepad(void);
/* The pad's state through SDL (on the pad thread, or on the game's thread with the thread off). */
static void pad_fill(SDL_Gamepad *g, PadSnap *s) {
    memset(s,0,sizeof(*s));
    if (!g) return;
    s->present=1;
    for (int b=0;b<SDL_GAMEPAD_BUTTON_COUNT && b<64;++b)
        if (SDL_GetGamepadButton(g,(SDL_GamepadButton)b)) s->buttons|=1ull<<b;
    for (int a=0;a<SDL_GAMEPAD_AXIS_COUNT;++a) s->axes[a]=SDL_GetGamepadAxis(g,(SDL_GamepadAxis)a);
    if (SDL_GetNumGamepadTouchpads(g)>0) {
        const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
        for (int finger=0;finger<fingers && s->touch_count<2;++finger) {
            bool down=false;
            float x=0, y=0;
            if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                s->touch[s->touch_count].x=x; s->touch[s->touch_count].y=y; s->touch[s->touch_count].id=(uint8_t)finger;
                ++s->touch_count;
            }
        }
    }
    s->device=prompts_pad_device(g);
}
static pthread_mutex_t snap_lock=PTHREAD_MUTEX_INITIALIZER;
static PadSnap snap_shared;
static char snap_name[96];          /* the open pad's name (runtime_pad_report) */
/* Requests for the thread: rumble and the light bar (a sequence number each: the latest wins). */
static pthread_mutex_t request_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned rumble_seq, led_seq;
static uint16_t rumble_low, rumble_high;
static uint32_t rumble_ms;
static uint8_t led_rgb[3];
static void *pad_thread_main(void *arg) {
    (void)arg;
    unsigned rumble_done=0, led_done=0;
    SDL_Gamepad *led_pad=NULL; /* the light bar's colour goes to a pad that becomes the one in use too */
    for (;;) {
        const uint64_t start=now_us();
        SDL_UpdateJoysticks();
        const uint64_t took=now_us()-start;
        if (took>20000) {
            static unsigned noted;
            if (noted++<20) printf("Runtime: controller update took %.0f ms (on the pad thread; the game went on)\n",took/1000.0);
        }
        SDL_Gamepad *g=current_gamepad();
        PadSnap snap;
        pad_fill(g,&snap);
        pthread_mutex_lock(&snap_lock);
        snap_shared=snap;
        if (g) { const char *name=SDL_GetGamepadName(g); snprintf(snap_name,sizeof(snap_name),"%s",name ? name : "Game controller"); }
        else snap_name[0]=0;
        pthread_mutex_unlock(&snap_lock);
        pthread_mutex_lock(&request_lock);
        const unsigned rs=rumble_seq, ls=led_seq;
        const uint16_t low=rumble_low, high=rumble_high;
        const uint32_t ms=rumble_ms;
        const uint8_t r=led_rgb[0], gg=led_rgb[1], b=led_rgb[2];
        pthread_mutex_unlock(&request_lock);
        if (rs!=rumble_done) { rumble_done=rs; pad_rumble(g,low,high,ms); }
        if (g!=led_pad) { led_pad=g; if (g && ls) led_done=ls-1; }
        if (ls!=led_done && g && now_us()>=pad_writes_after_us) {
            led_done=ls;
            const uint64_t led_start=now_us();
            SDL_SetGamepadLED(g,r,gg,b);
            pad_slow("light bar",led_start);
        } else if (ls!=led_done && !g) led_done=ls;
        SDL_Delay(2);
    }
    return NULL;
}
/* Started on the first pad read once SDL's gamepad part is up; SDL's own update in the event pump is turned off
 * (window.cpp sets the same hint before SDL starts). */
static int pad_thread_started;
static void pad_thread_start(void) {
    if (pad_thread_started) return;
    pad_thread_started=1;
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS,"0");
    pthread_t thread;
    if (pthread_create(&thread,NULL,pad_thread_main,NULL)==0) {
        pthread_detach(thread);
        printf("Runtime: controllers are updated and read on a thread of their own (BB_PAD_THREAD=0: off)\n");
    } else {
        SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS,"1");
        pad_thread_started=-1;
        printf("Runtime: the pad thread could not start: controllers are read on the game's thread\n");
    }
}
/* The pad as the game sees it this read. */
static void pad_snapshot(PadSnap *s) {
    if (pad_thread_on() && pad_thread_started>=0) {
        if (!pad_thread_started) {
            /* (sdl_ready and SDL's event watches are set up by current_gamepad, on the pad thread) */
            if (!SDL_WasInit(SDL_INIT_GAMEPAD) && !SDL_InitSubSystem(SDL_INIT_GAMEPAD)) { memset(s,0,sizeof(*s)); return; }
            pad_thread_start();
        }
        if (pad_thread_started>0) {
            pthread_mutex_lock(&snap_lock);
            *s=snap_shared;
            pthread_mutex_unlock(&snap_lock);
            return;
        }
    }
    pad_fill(current_gamepad(),s);
}
/* Rumble and the light bar: through the thread, or at once with it off. */
static void pad_request_rumble(uint16_t low, uint16_t high, uint32_t ms) {
    if (pad_thread_on() && pad_thread_started>0) {
        pthread_mutex_lock(&request_lock);
        rumble_low=low; rumble_high=high; rumble_ms=ms; ++rumble_seq;
        pthread_mutex_unlock(&request_lock);
        return;
    }
    pad_rumble(current_gamepad(),low,high,ms);
}
static void pad_request_led(uint8_t r, uint8_t g, uint8_t b) {
    if (pad_thread_on() && pad_thread_started>0) {
        pthread_mutex_lock(&request_lock);
        led_rgb[0]=r; led_rgb[1]=g; led_rgb[2]=b; ++led_seq;
        pthread_mutex_unlock(&request_lock);
        return;
    }
    SDL_Gamepad *pad=current_gamepad();
    if (pad && now_us()>=pad_writes_after_us) {
        const uint64_t start=now_us();
        SDL_SetGamepadLED(pad,r,g,b);
        pad_slow("light bar",start);
    }
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
enum { A_LS_UP=16, A_LS_DOWN, A_LS_LEFT, A_LS_RIGHT, A_RS_UP, A_RS_DOWN, A_RS_LEFT, A_RS_RIGHT,
       A_ROT_CCW, A_ROT_CW, A_WALK, ACTIONS };
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
    {"rotate_ccw",0,"R",NULL}, {"rotate_cw",0,"T",NULL}, {"walk",0,"Left Ctrl",NULL},
};
#define PAD_TRIGGER_LEFT (-2)
#define PAD_TRIGGER_RIGHT (-3)
static struct {
    Bind keys[ACTIONS][2];
    int pad[16];               /* SDL_GamepadButton, PAD_TRIGGER_*, or -1 */
    int mouse_look, mouse_invert_y, pad_invert_y, pad_vibration, pad_vibration_strength, walk_speed;
    volatile float mouse_sensitivity;
    /* Times the sensitivity while aiming (L2), and for up and down (looking and aiming). The in-game
     * menu changes all three while the game runs (runtime_mouse_set, another thread: plain floats). */
    volatile float mouse_aim_sensitivity, mouse_vertical;
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
    input.mouse_aim_sensitivity=1.0f; input.mouse_vertical=1.0f;
    input.pad_vibration=1; input.pad_vibration_strength=100; input.walk_speed=45;
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
            else if (!strcmp(key,"mouse_aim_sensitivity")) input.mouse_aim_sensitivity=(float)atof(value);
            else if (!strcmp(key,"mouse_vertical")) input.mouse_vertical=(float)atof(value);
            else if (!strcmp(key,"mouse_invert_y")) input.mouse_invert_y=atoi(value)!=0;
            else if (!strcmp(key,"pad_invert_y")) input.pad_invert_y=atoi(value)!=0;
            else if (!strcmp(key,"pad_vibration")) input.pad_vibration=atoi(value)!=0;
            else if (!strcmp(key,"pad_vibration_strength")) input.pad_vibration_strength=atoi(value);
            else if (!strcmp(key,"walk_speed")) {
                const int v=atoi(value);
                input.walk_speed=v<10 ? 10 : v>100 ? 100 : v;
            }
            else for (int a=0;a<ACTIONS;++a) {
                if (!strncmp(key,"key.",4) && !strcmp(key+4,actions[a].name)) set_keys(a,value);
                else if (a<16 && !strncmp(key,"pad.",4) && !strcmp(key+4,actions[a].name)) input.pad[a]=parse_pad(value);
            }
        }
        fclose(f);
        printf("Runtime: controls from %s (mouse look %s, sensitivity %.2f, aiming x%.2f, vertical x%.2f)\n",path,
               input.mouse_look ? "on" : "off",input.mouse_sensitivity,input.mouse_aim_sensitivity,input.mouse_vertical);
    }
    if (input.mouse_sensitivity<=0.0f) input.mouse_sensitivity=1.0f;
    if (input.mouse_aim_sensitivity<=0.0f) input.mouse_aim_sensitivity=1.0f;
    if (input.mouse_vertical<=0.0f) input.mouse_vertical=1.0f;
    if (input.pad_vibration_strength<0) input.pad_vibration_strength=0;
    if (input.pad_vibration_strength>100) input.pad_vibration_strength=100;
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
    case BIND_KEY:
        /* Alt+Enter switches window <-> fullscreen (window.cpp): that Enter is not a game key. */
        if (k && (b->code==SDL_SCANCODE_RETURN || b->code==SDL_SCANCODE_KP_ENTER) &&
            (k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT] || bbgpu_window_enter_swallowed())) return 0;
        return k && k[b->code];
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
/* Degrees of turn per mouse pixel: the sensitivity, times the aiming one while aim is held (also
 * while aiming blends in, and on the stick paths), times the vertical one for up and down. */
static float mouse_scale(int vertical) {
    float k=MOUSE_DEGREES_PER_PIXEL*input.mouse_sensitivity;
    if (aim_held) k*=input.mouse_aim_sensitivity;
    if (vertical) k*=input.mouse_vertical;
    return k;
}
/* The in-game menu: the current values (any pointer may be NULL) and changes (applied at once, for
 * this session; values <= 0 are left as they are). */
void runtime_mouse_get(float *look, float *aim, float *vertical) {
    if (look) *look=input.mouse_sensitivity;
    if (aim) *aim=input.mouse_aim_sensitivity;
    if (vertical) *vertical=input.mouse_vertical;
}
void runtime_mouse_set(float look, float aim, float vertical) {
    if (look>0.0f) input.mouse_sensitivity=look;
    if (aim>0.0f) input.mouse_aim_sensitivity=aim;
    if (vertical>0.0f) input.mouse_vertical=vertical;
}
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
    const float turn=camera_test_rate>0.0f ? camera_test_rate : dx*mouse_scale(0);
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
        float v=cur+(input.mouse_invert_y ? -dy : dy)*mouse_scale(1)/arc_degrees;
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
    const float turn=dx*mouse_scale(0);
    if (turn!=0.0f) {
        const float a=turn*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
        const float x=dir[0], z=dir[2];
        dir[0]=x*c-z*s;
        dir[2]=x*s+z*c;
    }
    if (dy!=0.0f && limits) {
        float v=*pitch-(input.mouse_invert_y ? -dy : dy)*mouse_scale(1);
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
    const float k=mouse_scale(0)*aim_scale, k_vertical=mouse_scale(1)*aim_scale;
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
        turned_pitch+=-(input.mouse_invert_y ? -dy : dy)*k_vertical;
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
        const float scale=mouse_scale(0)/dt, scale_y=mouse_scale(1)/dt;
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
        y=curve((input.mouse_invert_y ? -dy : dy)*scale_y/(aim_held ? 0.45f : 1.0f));
        /* US 1.02: past full deflection the aiming camera turns at 45 * x^0.65 (as U2's) */
        if (aim_held && u1_us_curve) {
            const float rx=dx*scale, ry=(input.mouse_invert_y ? -dy : dy)*scale_y/0.45f;
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
/* U2 Chapter Select weapons (issue #16, 2026-11-06): the menu's Chapter Select plays the chapter's
 * task group (PlayTask, mode 1). PlayTask first resets the player's settings to the default set
 * (459d0e4e: only the .45 Defender, 40 rounds) and then applies the weapon set of the task it played
 * or its parents; a group has none. The chapter's first checkpoint applies its own set when it
 * activates only if its tasks.bin flags carry 0x20, and the first checkpoints of chapters 8 and 13
 * (war-temple-1-inside-temple, valley-start) do not: Nate starts with the Defender, whose model
 * those levels never load, so it is invisible and silent (the overlay's checkpoint jump plays the
 * checkpoint itself and gets its Beretta, rifle and grenade). Here, after PlayTask(mode 1) of a task
 * without data (a group), the first playable task that activates gets its set as flag 0x20 would
 * give it (when the set has a gun); once. Continue (mode 2), checkpoint restarts and jumps to a
 * checkpoint do not arm it. Chapters it changes: 6-10, 13, 14, 18, 20, 23, 24, 26.
 * BB_CHAPTER_WEAPONS=0: off. */
typedef void (__attribute__((sysv_abi)) *U2ApplySettings)(uint32_t set);
static _Atomic int u2_chapter_start; /* armed by a group's PlayTask(mode 1) */
static int u2_chapter_weapons_on(void) {
    static int on=-1;
    if (on<0) { const char *e=getenv("BB_CHAPTER_WEAPONS"); on=!(e && e[0]=='0'); }
    return on;
}
static void u2_chapter_play(uint64_t task, uint64_t mode) {
    if (!u2_chapter_weapons_on() || task<0x10000) return;
    const int group=*(const uint64_t *)(uintptr_t)(task+0x50)==0;
    atomic_store(&u2_chapter_start,(uint32_t)mode==1 && group);
}
static const unsigned char u2_apply_settings_eu[]={0x55,0x48,0x89,0xe5,0x31,0xf6,0xe8,0x25,0x2c,0xfd,0xff,0x48,0x85,
                                                   0xc0,0x74,0x12,0x48,0x8d,0x3d,0xf1,0xd9,0xb9,0x00,0xb9};
static const unsigned char u2_apply_settings_us[]={0x55,0x48,0x89,0xe5,0xe8,0x97,0x4e,0xfb,0xff,0x48,0x85,0xc0,0x74,
                                                   0x17,0x48,0x8b,0x30,0x48,0x85,0xf6,0x74,0x0f,0x48,0x8d};
static void u2_chapter_activate(uint64_t task, uint64_t image, uint64_t apply, const unsigned char *code, size_t size) {
    if (!atomic_load(&u2_chapter_start) || task<0x10000) return;
    if (memcmp((const void *)(uintptr_t)(image+apply),code,size)) {
        atomic_store(&u2_chapter_start,0);
        printf("Chapter weapons: the settings function is not where expected; nothing applied\n");
        return;
    }
    const unsigned char *t=(const unsigned char *)(uintptr_t)task;
    const unsigned char *data=*(const unsigned char *const *)(t+0x50);
    if (!data || !(*(const uint32_t *)(data+0xd0)&6)) return; /* not playable (a group): wait */
    atomic_store(&u2_chapter_start,0);
    const uint32_t flags=*(const uint32_t *)(t+0x48), set=*(const uint32_t *)(data+0xc8);
    if (flags&0x20 || !set) return; /* the game applies it itself */
    /* Only a set with a gun: some first checkpoints name the empty set (65759d0e, the unarmed openings'),
     * where the Defender is better than nothing (chapter 12, war-escape-watertower-fight). The set: five
     * slots {name, weapon id +8, ammo +0x10} of 0x18 bytes; guns in the first three, f114b39d = none.
     * The lookup is the one the settings function calls (EU: (id, 0) -> set; 1.02: (id) -> &set). */
    const unsigned char *found=NULL;
    if (apply==0x43b7e0) {
        found=((const unsigned char *(__attribute__((sysv_abi)) *)(uint32_t, int))(uintptr_t)(image+0x40e410))(set,0);
    } else {
        const unsigned char *const *p=
            ((const unsigned char *const *(__attribute__((sysv_abi)) *)(uint32_t))(uintptr_t)(image+0xa15300))(set);
        found=p ? *p : NULL;
    }
    int gun=0;
    for (int i=0;found && i<3;++i) {
        const uint32_t weapon=*(const uint32_t *)(found+8+0x18*i);
        if (weapon && weapon!=0xf114b39du) gun=1;
    }
    if (!gun) {
        printf("Chapter weapons: Chapter Select start at task %08x: its set %08x has no gun; the game's "
               "default kept\n",*(const uint32_t *)(t+8),set);
        return;
    }
    printf("Chapter weapons: Chapter Select start at task %08x: its weapon set %08x applied (the game "
           "leaves the default .45 Defender there)\n",*(const uint32_t *)(t+8),set);
    fflush(stdout);
    ((U2ApplySettings)(uintptr_t)(image+apply))(set);
}
static void __attribute__((ms_abi)) u2_chapter_play_hook(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                       const uint64_t *regs) {
    (void)image; (void)r14; (void)regs;
    u2_chapter_play(rdi,rsi);
}
static void __attribute__((ms_abi)) u2_chapter_activate_eu(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                           const uint64_t *regs) {
    (void)rsi; (void)r14; (void)regs;
    u2_chapter_activate(rdi,image,0x43b7e0,u2_apply_settings_eu,sizeof(u2_apply_settings_eu));
}
static void __attribute__((ms_abi)) u2_chapter_activate_us(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                           const uint64_t *regs) {
    (void)rsi; (void)r14; (void)regs;
    u2_chapter_activate(rdi,image,0xa60460,u2_apply_settings_us,sizeof(u2_apply_settings_us));
}
/* PlayTask and TaskActivate: their first 40 bytes in each release (the settings function is checked
 * before it is called). */
static const unsigned char u2_play_task_eu[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,
    0x81,0xe4,0xe0,0xff,0xff,0xff,0x48,0x81,0xec,0x60,0x01,0x00,0x00,0x89,0xf3,0x49,0x89,0xfd,0x4c,0x8b,0x3d,0x11,
    0xc0,0xa2,0x00,0x49};
static const unsigned char u2_play_task_us[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,
    0x81,0xe4,0xe0,0xff,0xff,0xff,0x48,0x81,0xec,0x80,0x01,0x00,0x00,0x41,0x89,0xf6,0x48,0x89,0xfb,0x4c,0x8b,0x3d,
    0x18,0x2e,0x5f,0x00};
static const unsigned char u2_task_activate_eu[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
    0x48,0x81,0xe4,0xe0,0xff,0xff,0xff,0x48,0x81,0xec,0xa0,0x02,0x00,0x00,0x49,0x89,0xfd,0x48,0x8b,0x05,0x73,0x81,
    0xa2,0x00,0x48,0x8b,0x00};
static const unsigned char u2_task_activate_us[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
    0x48,0x81,0xe4,0xe0,0xff,0xff,0xff,0x48,0x81,0xec,0xa0,0x02,0x00,0x00,0x48,0x89,0xfb,0x48,0x8b,0x05,0xbb,0xe8,
    0x5e,0x00,0x48,0x8b,0x00};
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
/* The engine's job runner copies a descriptor it reaches through the job's record
 * (mov rax, [r10+8]; mov rcx, [rax+0x20]); a job run before its descriptor was set read address 0x20
 * and the game stopped (BilalReloaded's port 0.8.0, its #205 #304 #326, all three games). Before the
 * two loads a jump goes to a stub in the function's dead assert path (it keeps its int 0x41): a null
 * pointer ends the job at the function's own exit, otherwise the loads run and it jumps back.
 * 1.02 code (US CUSA02320, EU/Russian CUSA02344, EU 1.02): the same bytes in all three dumps. The
 * jump's signature runs from the job record's setup through the loads, so it only goes in where the
 * stub's site matches as well. */
static const unsigned char u1_job_loads_code[]={0x49,0xc7,0x42,0x48,0x00,0x00,0x00,0x00,0x48,0x8b,0x05,0xa4,0xb6,0xb8,0x00,0x49,0x89,0x42,0x10,0x49,0x8b,0x42,0x08,0x48,0x8b,0x48,0x20};
static const unsigned char u1_job_loads_jump[]={0x49,0xc7,0x42,0x48,0x00,0x00,0x00,0x00,0x48,0x8b,0x05,0xa4,0xb6,0xb8,0x00,0x49,0x89,0x42,0x10,0xe9,0xff,0x0a,0x00,0x00,0x90,0x90,0x90};
static const unsigned char u1_job_cave_code[]={0x48,0x8b,0x05,0xef,0xe6,0xa9,0x00,0x48,0x85,0xc0,0x74,0x27,0x48,0x8d,0x3d,0x2c,0xe9,0x1e,0x00,0x48,0x8d,0x35,0xc4,0xe7};
static const unsigned char u1_job_cave_stub[]={0xcd,0x41,0x49,0x8b,0x42,0x08,0x48,0x85,0xc0,0x0f,0x84,0x9a,0xff,0xff,0xff,0x48,0x8b,0x48,0x20,0xe9,0xee,0xf4,0xff,0xff};
static const unsigned char u2_job_loads_code[]={0x49,0xc7,0x42,0x48,0x00,0x00,0x00,0x00,0x48,0x8b,0x05,0x14,0xe4,0x94,0x00,0x49,0x89,0x42,0x10,0x49,0x8b,0x42,0x08,0x48,0x8b,0x48,0x20};
static const unsigned char u2_job_loads_jump[]={0x49,0xc7,0x42,0x48,0x00,0x00,0x00,0x00,0x48,0x8b,0x05,0x14,0xe4,0x94,0x00,0x49,0x89,0x42,0x10,0xe9,0xfd,0x0a,0x00,0x00,0x90,0x90,0x90};
static const unsigned char u2_job_cave_code[]={0x48,0x8b,0x05,0x61,0x84,0x84,0x00,0x48,0x85,0xc0,0x74,0x27,0x48,0x8d,0x3d,0xfa,0x73,0x18,0x00,0x48,0x8d,0x35,0xef,0x71};
static const unsigned char u2_job_cave_stub[]={0xcd,0x41,0x49,0x8b,0x42,0x08,0x48,0x85,0xc0,0x0f,0x84,0x9a,0xff,0xff,0xff,0x48,0x8b,0x48,0x20,0xe9,0xf0,0xf4,0xff,0xff};
static const unsigned char u3_job_loads_code[]={0x49,0xc7,0x42,0x48,0x00,0x00,0x00,0x00,0x48,0x8b,0x05,0xc4,0xc3,0xd6,0x00,0x49,0x89,0x42,0x10,0x49,0x8b,0x42,0x08,0x48,0x8b,0x48,0x20};
static const unsigned char u3_job_loads_jump[]={0x49,0xc7,0x42,0x48,0x00,0x00,0x00,0x00,0x48,0x8b,0x05,0xc4,0xc3,0xd6,0x00,0x49,0x89,0x42,0x10,0xe9,0xfd,0x0a,0x00,0x00,0x90,0x90,0x90};
static const unsigned char u3_job_cave_code[]={0x48,0x8b,0x05,0x41,0x8d,0xc6,0x00,0x48,0x85,0xc0,0x74,0x27,0x48,0x8d,0x3d,0x08,0x41,0x4f,0x00,0x48,0x8d,0x35,0xfd,0x3e};
static const unsigned char u3_job_cave_stub[]={0xcd,0x41,0x49,0x8b,0x42,0x08,0x48,0x85,0xc0,0x0f,0x84,0x9a,0xff,0xff,0xff,0x48,0x8b,0x48,0x20,0xe9,0xf0,0xf4,0xff,0xff};
/* 1.00 code (EU CUSA02343 and US CUSA02320 1.00, the same bytes): the job function is not inlined there (U1 0x4ed260,
 * U2 0x67b570, U3 0x629fd0: it opens the profiler scope, copies the descriptor with rbx = the record, then calls the
 * job's work) and has the same two loads (mov rax, [rbx+8]; mov rcx, [rax+0x20]). They jump to a stub in the dead
 * assert path of a nearby job type check ("cmp dword [rsi], 0x610ba10"; U1 0x4ed843, U2 0x67bb63, U3 0x62a5b3, kept
 * int 0x41 first): a null pointer goes to the function's profiler scope pop and exit (U1 0x4ed41a, U2 0x67b72a,
 * U3 0x62a18a), skipping the copy and the work as on 1.02. (Each 1.00 executable has a second, unreferenced copy of
 * the descriptor copy, U3 0x629950: no call, jump, lea or pointer reaches it, so it is left alone.) */
static const unsigned char u1_job_loads_code_100[]={0x48,0x89,0x53,0x48,0xe8,0x70,0xdc,0xf0,0xff,0x48,0x89,0x43,0x10,0x48,0x8b,0x43,0x08,0x48,0x8b,0x48,0x20};
static const unsigned char u1_job_loads_jump_100[]={0x48,0x89,0x53,0x48,0xe8,0x70,0xdc,0xf0,0xff,0x48,0x89,0x43,0x10,0xe9,0x8c,0x05,0x00,0x00,0x90,0x90,0x90};
static const unsigned char u1_job_cave_code_100[]={0xe8,0x58,0x66,0xe7,0xff,0x48,0x85,0xc0,0x74,0x27,0x48,0x8d,0x3d,0xd8,0xd0,0x32,0x00,0x48,0x8d,0x35,0xfa,0xd0,0x32,0x00};
static const unsigned char u1_job_cave_stub_100[]={0xcd,0x41,0x48,0x8b,0x43,0x08,0x48,0x85,0xc0,0x0f,0x84,0xc8,0xfb,0xff,0xff,0x48,0x8b,0x48,0x20,0xe9,0x61,0xfa,0xff,0xff};
static const unsigned char u2_job_loads_code_100[]={0x48,0x89,0x53,0x48,0xe8,0x10,0x9e,0xf0,0xff,0x48,0x89,0x43,0x10,0x48,0x8b,0x43,0x08,0x48,0x8b,0x48,0x20};
static const unsigned char u2_job_loads_jump_100[]={0x48,0x89,0x53,0x48,0xe8,0x10,0x9e,0xf0,0xff,0x48,0x89,0x43,0x10,0xe9,0x9c,0x05,0x00,0x00,0x90,0x90,0x90};
static const unsigned char u2_job_cave_code_100[]={0xe8,0x58,0xd6,0xe6,0xff,0x48,0x85,0xc0,0x74,0x27,0x48,0x8d,0x3d,0xf8,0xfd,0x56,0x00,0x48,0x8d,0x35,0x1a,0xfe,0x56,0x00};
static const unsigned char u2_job_cave_stub_100[]={0xcd,0x41,0x48,0x8b,0x43,0x08,0x48,0x85,0xc0,0x0f,0x84,0xb8,0xfb,0xff,0xff,0x48,0x8b,0x48,0x20,0xe9,0x51,0xfa,0xff,0xff};
static const unsigned char u3_job_loads_code_100[]={0x48,0x89,0x53,0x48,0xe8,0x30,0x6c,0xf0,0xff,0x48,0x89,0x43,0x10,0x48,0x8b,0x43,0x08,0x48,0x8b,0x48,0x20};
static const unsigned char u3_job_loads_jump_100[]={0x48,0x89,0x53,0x48,0xe8,0x30,0x6c,0xf0,0xff,0x48,0x89,0x43,0x10,0xe9,0x8c,0x05,0x00,0x00,0x90,0x90,0x90};
static const unsigned char u3_job_cave_code_100[]={0xe8,0xc8,0x24,0xe7,0xff,0x48,0x85,0xc0,0x74,0x27,0x48,0x8d,0x3d,0xd8,0xdf,0x9c,0x00,0x48,0x8d,0x35,0xfa,0xdf,0x9c,0x00};
static const unsigned char u3_job_cave_stub_100[]={0xcd,0x41,0x48,0x8b,0x43,0x08,0x48,0x85,0xc0,0x0f,0x84,0xc8,0xfb,0xff,0xff,0x48,0x8b,0x48,0x20,0xe9,0x61,0xfa,0xff,0xff};
/* U3 light camera NaN (BB_LIGHT_ORTHO_GUARD=0: off; from BilalReloaded's port 0.7.2, his #284: "Stay in the Light",
 * the game stopped on "zscale*n+zoffset" in CreateOrthographicProjection, render-camera.h:441). The scene view's light
 * camera (1.02 0xf3c7e0, 1.00 0x5dcc70) normalises the light direction and builds a light basis against (1,0,0) with
 * rsqrt and no guard: a zero direction, or one exactly along +-X, makes both light matrices NaN (rbp-0x1b0 and
 * rbp-0x170, 8 rows; a NaN or infinite far, xmm1, does the same to the box), and the ortho builder (1.02 0xf768d0,
 * 1.00 0x618650) asserts. Its call (1.02 0xf3cad8, 1.00 0x5dcf24) now goes to a stub that checks the rows and the far
 * value: finite -> on to the builder as before; not -> it returns past the follow-up call too (mov rdi, ...; call the
 * camera update, 1.02 0x11c0d30, 1.00 0x80a2f0), so that light camera keeps the last good frame's matrices. The stub
 * sits in the builder's dead far-plane assert path (1.02 0xf770b7, 171 bytes; 1.00 0x618e2e, 147 bytes), which still
 * starts with int 0x41; the near-plane assert is unchanged. 1.02 is his stub; 1.00 has the same call site, frame
 * offsets and follow-up (another compiler layout of the builder: a shorter path, so the rows are read through
 * lea rax, [rbp-0x1b0]). */
static const unsigned char u3_light_ortho_site_102[]={0xc5,0xfa,0x10,0x05,0x24,0xed,0x50,0x00,0xc5,0xfa,0x10,0x8d,0x0c,0xfe,0xff,0xff,0xe8,0xf3,0x9d,0x03,0x00,0x4c,0x89,0xe7,0xe8,0x4b,0x42,0x28,0x00};
static const unsigned char u3_light_ortho_call_102[]={0xc5,0xfa,0x10,0x05,0x24,0xed,0x50,0x00,0xc5,0xfa,0x10,0x8d,0x0c,0xfe,0xff,0xff,0xe8,0xdc,0xa5,0x03,0x00};
static const unsigned char u3_light_ortho_cave_102[]={0x48,0x8b,0x05,0xfa,0xb6,0xc7,0x00,0x48,0x85,0xc0,0x0f,0x84,0x99,0x00,0x00,0x00,0x48,0x8d,0x3d,0xcf,0x4c,0x53,0x00,0x48,0x8d,0x35,0x2a,0x22,0x52,0x00,0x49,0x89,0xd6,0x48,0x8d,0x15,0xa1,0x4c,0x53,0x00,0x49,0x89,0xdc,0x48,0x89,0xcb,0x48,0x8d,0x0d,0x64,0x51,0x43,0x00,0x41,0xb8,0xba,0x01,0x00,0x00,0x45,0x31,0xc9,0xc5,0x78,0x29,0xad,0xf0,0xfe,0xff,0xff,0xc5,0x78,0x29,0xb5,0xe0,0xfe,0xff,0xff,0xc5,0x78,0x29,0xbd,0xd0,0xfe,0xff,0xff,0xc5,0xf8,0x29,0x95,0xc0,0xfe,0xff,0xff,0xc5,0xf8,0x29,0xa5,0xb0,0xfe,0xff,0xff,0xc5,0xf8,0x29,0xad,0xa0,0xfe,0xff,0xff,0xff,0xd0,0xc5,0xf8,0x28,0xad,0xa0,0xfe,0xff,0xff,0xc5,0xf8,0x28,0xa5,0xb0,0xfe,0xff,0xff,0xc5,0xf8,0x28,0x95,0xc0,0xfe,0xff,0xff,0xc5,0x78,0x28,0xbd,0xd0,0xfe,0xff,0xff,0xc5,0x78,0x28,0xb5,0xe0,0xfe,0xff,0xff,0xc5,0x78,0x28,0xad,0xf0,0xfe,0xff,0xff,0x48,0x89,0xd9,0x4c,0x89,0xe3,0x4c,0x89,0xf2,0xcd,0x41};
static const unsigned char u3_light_ortho_stub_102[]={0xcd,0x41,0xc5,0xf8,0x28,0x9d,0x50,0xfe,0xff,0xff,0xc5,0xe0,0x5c,0xdb,0xc5,0xf8,0x28,0xa5,0x60,0xfe,0xff,0xff,0xc5,0xd8,0x5c,0xe4,0xc5,0xe0,0x58,0xdc,0xc5,0xf8,0x28,0xa5,0x70,0xfe,0xff,0xff,0xc5,0xd8,0x5c,0xe4,0xc5,0xe0,0x58,0xdc,0xc5,0xf8,0x28,0xa5,0x80,0xfe,0xff,0xff,0xc5,0xd8,0x5c,0xe4,0xc5,0xe0,0x58,0xdc,0xc5,0xf8,0x28,0xa5,0x90,0xfe,0xff,0xff,0xc5,0xd8,0x5c,0xe4,0xc5,0xe0,0x58,0xdc,0xc5,0xf8,0x28,0xa5,0xa0,0xfe,0xff,0xff,0xc5,0xd8,0x5c,0xe4,0xc5,0xe0,0x58,0xdc,0xc5,0xf8,0x28,0xa5,0xb0,0xfe,0xff,0xff,0xc5,0xd8,0x5c,0xe4,0xc5,0xe0,0x58,0xdc,0xc5,0xf8,0x28,0xa5,0xc0,0xfe,0xff,0xff,0xc5,0xd8,0x5c,0xe4,0xc5,0xe0,0x58,0xdc,0xc5,0xf2,0x5c,0xe1,0xc5,0xe2,0x58,0xdc,0xc5,0xe0,0xc2,0xdb,0x03,0xc5,0xf8,0x50,0xc3,0x85,0xc0,0x75,0x05,0xe9,0x81,0xf7,0xff,0xff,0x48,0x83,0x04,0x24,0x08,0xc3,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc};
static const unsigned char u3_light_ortho_site_100[]={0xc5,0xfa,0x10,0x05,0x28,0x49,0xa1,0x00,0xc5,0xfa,0x10,0x8d,0x04,0xfe,0xff,0xff,0xe8,0x27,0xb7,0x03,0x00,0x48,0x89,0xdf,0xe8,0xbf,0xd3,0x22,0x00};
static const unsigned char u3_light_ortho_call_100[]={0xc5,0xfa,0x10,0x05,0x28,0x49,0xa1,0x00,0xc5,0xfa,0x10,0x8d,0x04,0xfe,0xff,0xff,0xe8,0x07,0xbf,0x03,0x00};
static const unsigned char u3_light_ortho_cave_100[]={0xc5,0xf8,0x29,0xad,0xa0,0xfe,0xff,0xff,0xc5,0xf8,0x29,0xa5,0xb0,0xfe,0xff,0xff,0xc5,0xf8,0x29,0x95,0xc0,0xfe,0xff,0xff,0xc5,0x78,0x29,0xbd,0xd0,0xfe,0xff,0xff,0xc5,0x78,0x29,0xb5,0xe0,0xfe,0xff,0xff,0xc5,0x78,0x29,0xad,0xf0,0xfe,0xff,0xff,0xe8,0x1d,0x3c,0xe8,0xff,0x48,0x85,0xc0,0x74,0x27,0x48,0x8d,0x3d,0x5f,0xd1,0x9d,0x00,0x48,0x8d,0x35,0xf0,0xd0,0x9d,0x00,0x48,0x8d,0x15,0x34,0xd1,0x9d,0x00,0x48,0x8d,0x0d,0xed,0xcf,0x9d,0x00,0x41,0xb8,0xba,0x01,0x00,0x00,0x45,0x31,0xc9,0xff,0xd0,0xcd,0x41,0xc5,0x78,0x28,0xad,0xf0,0xfe,0xff,0xff,0xc5,0x78,0x28,0xb5,0xe0,0xfe,0xff,0xff,0xc5,0x78,0x28,0xbd,0xd0,0xfe,0xff,0xff,0xc5,0xf8,0x28,0x95,0xc0,0xfe,0xff,0xff,0xc5,0xf8,0x28,0xa5,0xb0,0xfe,0xff,0xff,0xc5,0xf8,0x28,0xad,0xa0,0xfe,0xff,0xff};
static const unsigned char u3_light_ortho_stub_100[]={0xcd,0x41,0x48,0x8d,0x85,0x50,0xfe,0xff,0xff,0xc5,0xf8,0x28,0x18,0xc5,0xe0,0x5c,0xdb,0xc5,0xf8,0x28,0x60,0x10,0xc5,0xd8,0x5c,0xe4,0xc5,0xe0,0x58,0xdc,0xc5,0xf8,0x28,0x60,0x20,0xc5,0xd8,0x5c,0xe4,0xc5,0xe0,0x58,0xdc,0xc5,0xf8,0x28,0x60,0x30,0xc5,0xd8,0x5c,0xe4,0xc5,0xe0,0x58,0xdc,0xc5,0xf8,0x28,0x60,0x40,0xc5,0xd8,0x5c,0xe4,0xc5,0xe0,0x58,0xdc,0xc5,0xf8,0x28,0x60,0x50,0xc5,0xd8,0x5c,0xe4,0xc5,0xe0,0x58,0xdc,0xc5,0xf8,0x28,0x60,0x60,0xc5,0xd8,0x5c,0xe4,0xc5,0xe0,0x58,0xdc,0xc5,0xf8,0x28,0x60,0x70,0xc5,0xd8,0x5c,0xe4,0xc5,0xe0,0x58,0xdc,0xc5,0xf2,0x5c,0xe1,0xc5,0xe2,0x58,0xdc,0xc5,0xe0,0xc2,0xdb,0x03,0xc5,0xf8,0x50,0xc3,0x85,0xc0,0x75,0x05,0xe9,0x9c,0xf7,0xff,0xff,0x48,0x83,0x04,0x24,0x08,0xc3,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc};
/* U3 menu manager update (the Dev Menu's per-frame update, where its Tasks jump runs): the checkpoint jump's
 * frame point (runtime_checkpoint.c). The moved bytes are the push/sub/mov prologue before the first
 * rip-relative lea. */
static const unsigned char u3_menu_update_eu_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
                                                     0x48,0x83,0xec,0x78,0x49,0x89,0xff,0x48,0x8d,0x1d};
static const unsigned char u3_menu_update_us_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
                                                     0x48,0x81,0xec,0xc8,0x00,0x00,0x00,0x49,0x89,0xfe,0x48,0x8d,0x05};
/* U1 and U2 menu manager updates (same role; U1 EU 0xe8f60 and 1.02 0xe9050 share their first bytes). */
static const unsigned char u1_menu_update_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x81,0xec,
                                                  0x80,0x00,0x00,0x00,0x49,0x89,0xfe,0x4c,0x8b,0x3d};
static const unsigned char u2_menu_update_eu_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
                                                     0x48,0x83,0xec,0x78,0x49,0x89,0xfe,0x4c,0x8b,0x25};
static const unsigned char u2_menu_update_us_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
                                                     0x48,0x81,0xec,0xd8,0x00,0x00,0x00,0x49,0x89,0xfd,0x4c,0x8b,0x25};
/* "Quit" in Uncharted 1's main menu (runtime_exit_menu.c): AddToPage (the same bytes in both code versions; the
 * 13 moved bytes are its push/mov prologue) and the menu's item handler (EU sub rsp 0xd8, 1.02 0xc8). */
static const unsigned char u1_add_to_page_code[]={0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x49,0x89,0xf6,0x48,0x89,0xfb,
                                                  0x48,0x63,0x83,0x98,0x00,0x00,0x00};
static const unsigned char u1_menu_handler_eu_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
                                                      0x48,0x81,0xec,0xd8,0x00,0x00,0x00,0x89,0xf3,0x49,0x89,0xfe};
static const unsigned char u1_menu_handler_us_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
                                                      0x48,0x81,0xec,0xc8,0x00,0x00,0x00,0x89,0xf3,0x49,0x89,0xfe};
/* U3 restart fall (BB_U3_PLATFORM_DELTA=0 = off). The character controller (1.02 0x1194ed0, 1.00 EU+US 0x7e0ce0;
 * rbx = the controller) stores the motion of the platform its owner stands on for this frame: +0x90 = the platform's
 * displacement / dt, +0xa0 its rotation (from the owner's attach struct). On a frame where the owner stands on a
 * different platform than on the last frame, that "motion" is the jump between the two platforms' poses: at a cruise
 * ship checkpoint restart Drake touches two spawn platforms in his first frames above ~70 fps and the controller reports
 * ~3000 m/s and a 40 degree turn. The follow camera rides that delta and swings 16 m off; the level scripts test the
 * camera against the level regions, drop the deck cargo for ~0.1 s, and Drake loses his floor and falls through the
 * ship. Stepping onto another platform is not platform motion: the delta of such a frame is zero here (+0x90 xyz 0,
 * +0xa0 identity). Hooked after the stores (1.02: lea r14, [rbx+0x10]; 1.00: lea r12, [rbx+0x10]; then lea r13,
 * [rbx+0x20]). */
static const unsigned char u3_platdelta_code[]={0x4c,0x8d,0x73,0x10,0x4c,0x8d,0x6b,0x20};
static const unsigned char u3_platdelta_code_100[]={0x4c,0x8d,0x63,0x10,0x4c,0x8d,0x6b,0x20};
static void __attribute__((ms_abi)) u3_platdelta(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14, const uint64_t *regs) {
    (void)rdi; (void)rsi; (void)r14; (void)image;
    static int on=-1;
    if (on<0) { const char *e=getenv("BB_U3_PLATFORM_DELTA"); on=!(e && e[0]=='0'); }
    if (!on) return;
    const uint64_t ctrl=regs[0];
    if (ctrl<0x10000) return;
    /* the owner (handle +0xc0 slot / +0xc8 id) and its platform handle (owner +0xb8 slot, +0xc0 id) */
    const uint64_t oslot=*(const uint64_t *)(uintptr_t)(ctrl+0xc0); const uint32_t oid=*(const uint32_t *)(uintptr_t)(ctrl+0xc8);
    const uint64_t owner=oslot>0x10000 ? *(const uint64_t *)(uintptr_t)oslot : 0;
    if (owner<0x10000 || !oid || *(const uint32_t *)(uintptr_t)(owner+0x7c)!=oid) return;
    const uint64_t pslot=*(const uint64_t *)(uintptr_t)(owner+0xb8); const uint32_t pid=*(const uint32_t *)(uintptr_t)(owner+0xc0);
    /* the last platform per owner id (owners move in memory; their id does not) */
    static struct { uint32_t oid, pid; uint64_t pslot; } last[64];
    int i=0, free_i=-1;
    for (;i<64;++i) { if (last[i].oid==oid) break; if (!last[i].oid && free_i<0) free_i=i; }
    if (i==64) { if (free_i<0) free_i=(int)(oid%64u); i=free_i; last[i].oid=oid; last[i].pid=pid; last[i].pslot=pslot; return; }
    const int changed=last[i].pid!=pid || last[i].pslot!=pslot;
    last[i].pid=pid; last[i].pslot=pslot;
    if (!changed) return;
    float *d=(float *)(uintptr_t)(ctrl+0x90), *q=(float *)(uintptr_t)(ctrl+0xa0);
    d[0]=d[1]=d[2]=0.0f; q[0]=q[1]=q[2]=0.0f; q[3]=1.0f;
}
/* "Quit" in Uncharted 2's and 3's main menus (runtime_exit_menu.c): the store of the menu data's pointer after its
 * lookup (7 bytes moved) and the menu handler (13 push bytes moved), per game and code version. */
static const unsigned char u2_eu_menu_store[]={0x49,0x89,0x86,0xa8,0x17,0x00,0x00,0x83,0x38,0x00,0x0f,0x8e};
static const unsigned char u2_us_menu_store[]={0x48,0x89,0x83,0xa8,0x17,0x00,0x00,0x83,0x38,0x00,0x0f,0x8e};
static const unsigned char u3_eu_menu_store[]={0x49,0x89,0x87,0x00,0x14,0x00,0x00,0x83,0x38,0x00,0x0f,0x8e};
static const unsigned char u3_us_menu_store[]={0x49,0x89,0x86,0x00,0x14,0x00,0x00,0x83,0x38,0x00,0x0f,0x8e};
static const unsigned char u2_eu_menu_handler[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
                                                 0x48,0x81,0xec,0x68,0x01,0x00,0x00,0x89,0x95,0x8c,0xfe};
static const unsigned char u2_us_menu_handler[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
                                                 0x48,0x81,0xe4,0xe0,0xff,0xff,0xff,0x48,0x81,0xec,0x40,0x02,0x00,0x00,0x89};
static const unsigned char u3_eu_menu_handler[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
                                                 0x48,0x81,0xec,0x08,0x03,0x00,0x00,0x89,0x95,0xfc,0xfc};
static const unsigned char u3_us_menu_handler[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
                                                 0x48,0x81,0xec,0xe8,0x02,0x00,0x00,0x89,0x95,0x24,0xfd};
static const RuntimeGuestCallback guest_callbacks[]={
    {"U2 main menu Quit (EU)",0x29d7da,u2_eu_menu_store,sizeof(u2_eu_menu_store),7,runtime_exit_menu_u2_data,NULL},
    {"U2 main menu Quit (1.02)",0x5c5433,u2_us_menu_store,sizeof(u2_us_menu_store),7,runtime_exit_menu_u2_data,NULL},
    {"U3 main menu Quit (EU)",0x1e408a,u3_eu_menu_store,sizeof(u3_eu_menu_store),7,runtime_exit_menu_u3_data,NULL},
    {"U3 main menu Quit (1.02)",0x6496f1,u3_us_menu_store,sizeof(u3_us_menu_store),7,runtime_exit_menu_u3_data,NULL},
    {"U2 main menu Quit handler (EU)",0x29e980,u2_eu_menu_handler,sizeof(u2_eu_menu_handler),13,
     runtime_exit_menu_u2_handler,NULL},
    {"U2 main menu Quit handler (1.02)",0x5c8840,u2_us_menu_handler,sizeof(u2_us_menu_handler),13,
     runtime_exit_menu_u2_handler,NULL},
    {"U3 main menu Quit handler (EU)",0x1e54b0,u3_eu_menu_handler,sizeof(u3_eu_menu_handler),13,
     runtime_exit_menu_u3_handler,NULL},
    {"U3 main menu Quit handler (1.02)",0x64ce90,u3_us_menu_handler,sizeof(u3_us_menu_handler),13,
     runtime_exit_menu_u3_handler,NULL},
    {"U1 main menu Quit (EU)",0x132d70,u1_add_to_page_code,sizeof(u1_add_to_page_code),13,
     runtime_exit_menu_u1_eu_add,NULL},
    {"U1 main menu Quit (1.02)",0x132f00,u1_add_to_page_code,sizeof(u1_add_to_page_code),13,
     runtime_exit_menu_u1_us_add,NULL},
    {"U1 menu Quit handler (EU)",0x148a30,u1_menu_handler_eu_code,sizeof(u1_menu_handler_eu_code),20,
     runtime_exit_menu_u1_eu_handler,NULL},
    {"U1 menu Quit handler (1.02)",0x148e80,u1_menu_handler_us_code,sizeof(u1_menu_handler_us_code),20,
     runtime_exit_menu_u1_us_handler,NULL},
    {"U1 checkpoint jumps (EU)",0xe8f60,u1_menu_update_code,sizeof(u1_menu_update_code),21,
     runtime_checkpoint_frame_u1_eu,NULL},
    {"U1 checkpoint jumps (1.02)",0xe9050,u1_menu_update_code,sizeof(u1_menu_update_code),21,
     runtime_checkpoint_frame_u1_us,NULL},
    {"U2 checkpoint jumps (EU)",0x1cd800,u2_menu_update_eu_code,sizeof(u2_menu_update_eu_code),20,
     runtime_checkpoint_frame_u2_eu,NULL},
    {"U2 checkpoint jumps (1.02)",0x4b43f0,u2_menu_update_us_code,sizeof(u2_menu_update_us_code),23,
     runtime_checkpoint_frame_u2_us,NULL},
    {"U3 checkpoint jumps (EU)",0x6cbdb0,u3_menu_update_eu_code,sizeof(u3_menu_update_eu_code),20,
     runtime_checkpoint_frame_eu,NULL},
    {"U3 checkpoint jumps (1.02)",0x103ff10,u3_menu_update_us_code,sizeof(u3_menu_update_us_code),23,
     runtime_checkpoint_frame_us,NULL},
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
    {"U2 Chapter Select weapons: PlayTask (EU)",0x4354f0,u2_play_task_eu,sizeof(u2_play_task_eu),6,
     u2_chapter_play_hook,NULL},
    {"U2 Chapter Select weapons: TaskActivate (EU)",0x439390,u2_task_activate_eu,sizeof(u2_task_activate_eu),6,
     u2_chapter_activate_eu,NULL},
    {"U2 Chapter Select weapons: PlayTask (1.02)",0xa57dc0,u2_play_task_us,sizeof(u2_play_task_us),6,
     u2_chapter_play_hook,NULL},
    {"U2 Chapter Select weapons: TaskActivate (1.02)",0xa5c320,u2_task_activate_us,sizeof(u2_task_activate_us),6,
     u2_chapter_activate_us,NULL},
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
    {"U1 job descriptor guard (1.02, stub)",0x617602,u1_job_cave_code,sizeof(u1_job_cave_code),
     sizeof(u1_job_cave_stub),NULL,u1_job_cave_stub},
    {"U1 job descriptor guard (1.02, jump)",0x616aed,u1_job_loads_code,sizeof(u1_job_loads_code),
     sizeof(u1_job_loads_jump),NULL,u1_job_loads_jump},
    {"U2 job descriptor guard (1.02, stub)",0xd25b10,u2_job_cave_code,sizeof(u2_job_cave_code),
     sizeof(u2_job_cave_stub),NULL,u2_job_cave_stub},
    {"U2 job descriptor guard (1.02, jump)",0xd24ffd,u2_job_loads_code,sizeof(u2_job_loads_code),
     sizeof(u2_job_loads_jump),NULL,u2_job_loads_jump},
    {"U3 job descriptor guard (1.02, stub)",0xf89a70,u3_job_cave_code,sizeof(u3_job_cave_code),
     sizeof(u3_job_cave_stub),NULL,u3_job_cave_stub},
    {"U3 job descriptor guard (1.02, jump)",0xf88f5d,u3_job_loads_code,sizeof(u3_job_loads_code),
     sizeof(u3_job_loads_jump),NULL,u3_job_loads_jump},
    {"U1 job descriptor guard (1.00, stub)",0x4ed843,u1_job_cave_code_100,sizeof(u1_job_cave_code_100),
     sizeof(u1_job_cave_stub_100),NULL,u1_job_cave_stub_100},
    {"U1 job descriptor guard (1.00, jump)",0x4ed2a7,u1_job_loads_code_100,sizeof(u1_job_loads_code_100),
     sizeof(u1_job_loads_jump_100),NULL,u1_job_loads_jump_100},
    {"U2 job descriptor guard (1.00, stub)",0x67bb63,u2_job_cave_code_100,sizeof(u2_job_cave_code_100),
     sizeof(u2_job_cave_stub_100),NULL,u2_job_cave_stub_100},
    {"U2 job descriptor guard (1.00, jump)",0x67b5b7,u2_job_loads_code_100,sizeof(u2_job_loads_code_100),
     sizeof(u2_job_loads_jump_100),NULL,u2_job_loads_jump_100},
    {"U3 job descriptor guard (1.00, stub)",0x62a5b3,u3_job_cave_code_100,sizeof(u3_job_cave_code_100),
     sizeof(u3_job_cave_stub_100),NULL,u3_job_cave_stub_100},
    {"U3 job descriptor guard (1.00, jump)",0x62a017,u3_job_loads_code_100,sizeof(u3_job_loads_code_100),
     sizeof(u3_job_loads_jump_100),NULL,u3_job_loads_jump_100},
    {"U3 light ortho guard (1.02, stub)",0xf770b7,u3_light_ortho_cave_102,sizeof(u3_light_ortho_cave_102),
     sizeof(u3_light_ortho_stub_102),NULL,u3_light_ortho_stub_102},
    {"U3 light ortho guard (1.02, call)",0xf3cac8,u3_light_ortho_site_102,sizeof(u3_light_ortho_site_102),
     sizeof(u3_light_ortho_call_102),NULL,u3_light_ortho_call_102},
    {"U3 light ortho guard (1.00, stub)",0x618e2e,u3_light_ortho_cave_100,sizeof(u3_light_ortho_cave_100),
     sizeof(u3_light_ortho_stub_100),NULL,u3_light_ortho_stub_100},
    {"U3 light ortho guard (1.00, call)",0x5dcf14,u3_light_ortho_site_100,sizeof(u3_light_ortho_site_100),
     sizeof(u3_light_ortho_call_100),NULL,u3_light_ortho_call_100},
    {"U3 platform change is not platform motion (1.02)",0x1195038,u3_platdelta_code,8,8,u3_platdelta,NULL},
    {"U3 platform change is not platform motion (1.00)",0x7e0df8,u3_platdelta_code_100,8,8,u3_platdelta,NULL},
};
const RuntimeGuestCallback *runtime_guest_callbacks(size_t *count) {
    *count=sizeof(guest_callbacks)/sizeof(*guest_callbacks);
    return guest_callbacks;
}

/* The button pictures for this controller: 1 PlayStation (DualShock, DualSense: the game's own),
 * 3 Nintendo (Switch Pro, Joy-Cons, pads labelled B at the bottom: B A Y X by position), 2 Xbox
 * (everything else). */
static int prompts_pad_device(SDL_Gamepad *g) {
    const SDL_GamepadType type=SDL_GetGamepadType(g);
    if (type==SDL_GAMEPAD_TYPE_PS3 || type==SDL_GAMEPAD_TYPE_PS4 || type==SDL_GAMEPAD_TYPE_PS5) return 1;
    if (type==SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || type==SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
        type==SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT || type==SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) return 3;
    const SDL_GamepadButtonLabel south=SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_SOUTH);
    return south==SDL_GAMEPAD_BUTTON_LABEL_CROSS ? 1 : south==SDL_GAMEPAD_BUTTON_LABEL_B ? 3 : 2;
}

/* Turning the left stick in circles (valves, wheels, cranks: the games detect the turn from the
 * stick passing the four directions up, left, down, right in order, each at most 0.31 s after the
 * last, and rolling the eight movement-key directions never does). While a rotate key is held the
 * left stick circles at full deflection, 1.25 turns a second, a smooth sweep that starts where the
 * stick points (the movement keys held, or up). `rotate_dir`: -1 counter-clockwise, 1 clockwise.
 * (By drakolordx7, from his fork, 46aec0d3.) */
#define ROTATE_TURNS_PER_SECOND 1.25f
static int host_rotate;               /* the rotate keys held right now (set in sample_host) */
static float rotate_angle;            /* radians clockwise from up */
static uint64_t rotate_last_us;
static void rotate_stick(PadData *d, int rotate_dir, uint64_t now) {
    if (!rotate_dir) { rotate_last_us=0; return; }
    if (!rotate_last_us || now-rotate_last_us>100000) {
        const float x=((float)d->left_x-127.5f)/127.5f, y=((float)d->left_y-127.5f)/127.5f;
        rotate_angle=x*x+y*y>0.25f ? SDL_atan2f(x,-y) : 0.0f;
    } else {
        float dt=(float)(now-rotate_last_us)*1e-6f;
        if (dt>0.05f) dt=0.05f;
        rotate_angle+=(float)rotate_dir*dt*ROTATE_TURNS_PER_SECOND*6.2831853f;
        if (rotate_angle>6.2831853f) rotate_angle-=6.2831853f;
        if (rotate_angle<0.0f) rotate_angle+=6.2831853f;
    }
    rotate_last_us=now;
    const float x=127.5f+127.5f*SDL_sinf(rotate_angle), y=127.5f-127.5f*SDL_cosf(rotate_angle);
    d->left_x=(uint8_t)(x<0.0f ? 0 : x>255.0f ? 255 : (int)(x+0.5f));
    d->left_y=(uint8_t)(y<0.0f ? 0 : y>255.0f ? 255 : (int)(y+0.5f));
}

static void injected_mouse(float *dx, float *dy);
static void sample_host(PadData *d) {
    host_rotate=0;
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=runtime_guest_monotonic_us(); /* the game's clock (BB_TIME_SCALE) */
    static int loaded;
    if (!loaded) { load_input(); loaded=1; }
    PadSnap pad;
    pad_snapshot(&pad);
    int g=pad.present;   /* a controller is there (its state: pad, read on the pad thread) */
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
        /* SDL delivers gamepad events in the background too (window.cpp): the game takes them
         * only while its window is in front, unless the player chose the controller to keep working
         * in the background (BB_PAD_BACKGROUND=1, launcher Controls page; the game on one screen,
         * other programs on the other). The keyboard and mouse are only ever read in front. */
        const unsigned long long taps=__atomic_exchange_n(&pad_taps,0ull,__ATOMIC_RELAXED);
        static int pad_background=-1;
        if (pad_background<0) {
            const char *e=getenv("BB_PAD_BACKGROUND");
            pad_background=e && e[0]=='1';
            if (pad_background) printf("Runtime: the controller keeps working while the game window is in the background\n");
        }
        if (!bbgpu_window_foreground()) {
            if (!pad_background) g=0;
            k=NULL; mouse=0; wheel=0; captured=0; wheel_until_us[0]=wheel_until_us[1]=0;
        }
        if (g) {
            for (int a=0;a<16;++a) {
                const int b=input.pad[a];
                if (b>=0 && ((b<64 && (pad.buttons>>b & 1ull)) || (b<64 && (taps>>b & 1ull))))
                    d->buttons|=actions[a].ps;
                else if (b==PAD_TRIGGER_LEFT || b==PAD_TRIGGER_RIGHT) {
                    const uint8_t v=trigger(pad.axes[b==PAD_TRIGGER_LEFT ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                                                         : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER]);
                    if (v>30) d->buttons|=actions[a].ps;
                    if (actions[a].ps==BTN_L2 && v>d->l2) d->l2=v;
                    if (actions[a].ps==BTN_R2 && v>d->r2) d->r2=v;
                }
            }
            if ((pad.buttons>>SDL_GAMEPAD_BUTTON_TOUCHPAD & 1ull) || (taps>>SDL_GAMEPAD_BUTTON_TOUCHPAD & 1ull))
                d->buttons|=BTN_TOUCHPAD; /* DualSense/DS4 click */
            if ((d->buttons & BTN_L2) && !d->l2) d->l2=255;
            if ((d->buttons & BTN_R2) && !d->r2) d->r2=255;
            d->left_x=axis(pad.axes[SDL_GAMEPAD_AXIS_LEFTX]); d->left_y=axis(pad.axes[SDL_GAMEPAD_AXIS_LEFTY]);
            d->right_x=axis(pad.axes[SDL_GAMEPAD_AXIS_RIGHTX]); d->right_y=axis(pad.axes[SDL_GAMEPAD_AXIS_RIGHTY]);
            if (input.pad_invert_y) d->right_y=(uint8_t)(255-d->right_y);
            for (int t=0;t<pad.touch_count && d->touch_count<2;++t)
                d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(pad.touch[t].x,1919),
                    .y=touch_axis(pad.touch[t].y,942),.id=pad.touch[t].id};
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
        const int device=pad_used ? pad.device : 0;
        if (device!=prompts_device) {
            prompts_device=device;
            bbgpu_prompts_controller(device);
            keyboard_in_use=device==0;
            if (keyboard_in_use && g) pad_request_rumble(0,0,0); /* the pad put down stops shaking */
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
        if ((lx || ly) && action_down(A_WALK,k,mouse,now)) {
            /* Walk: the key direction at walk_speed percent of full deflection (diagonals too; by
             * drakolordx7, from his fork, 6455d6bb). */
            const float scale=127.5f*(float)input.walk_speed/100.0f/((lx && ly) ? 1.41421356f : 1.0f);
            if (lx) d->left_x=(uint8_t)(int)(127.5f+(float)lx*scale+0.5f);
            if (ly) d->left_y=(uint8_t)(int)(127.5f+(float)ly*scale+0.5f);
        } else {
            if (lx) d->left_x=(uint8_t)(lx<0 ? 0 : 255);
            if (ly) d->left_y=(uint8_t)(ly<0 ? 0 : 255);
        }
        host_rotate=action_down(A_ROT_CW,k,mouse,now)-action_down(A_ROT_CCW,k,mouse,now);
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
            const float alpha=1.0f-SDL_expf(-dt/0.012f), scale=mouse_scale(0), scale_y=mouse_scale(1);
            mouse_vx+=alpha*(mdx/dt*scale-mouse_vx);
            mouse_vy+=alpha*((input.mouse_invert_y ? -mdy : mdy)/dt*scale_y-mouse_vy);
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
 * the sticks; rot_ccw rot_cw turn the left stick round (the rotate keys). An empty file releases everything. */
static struct { uint32_t buttons; int stick[4]; int touch_side; float mouse[2]; int rotate; } injected={0,{-1,-1,-1,-1},-1,{0,0},0};
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
    injected.rotate=0;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"rot_ccw")) injected.rotate=-1; /* the rotate keys, scripted */
        if (!strcmp(token,"rot_cw")) injected.rotate=1;
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
/* BB_DEV_MENU=1 (experiment, all three games 1.02, EU and US): F11 opens and closes Naughty Dog's developer menu,
 * which the retail games build but give no way to open: the menu's "open" byte (the game's code checks it) is
 * flipped. Per build, the root pointers of the menus, where their names lie (the names tell which build this
 * is; any other game or release is left alone) and the open byte. Uncharted 1 has no Quick Menu (quick_name 0).
 * US = CUSA02320 / CUSA02344 (one executable per game). */
static const struct { uintptr_t dev_name, quick_name, dev_root, quick_root; unsigned open; } dev_menu_builds[]={
    {0xf60a72,0xfacfe0,0x142ee88,0x196cdd0,0x90},   /* Uncharted 3 EU CUSA02343 */
    {0x1458d8a,0x1470aa3,0x17b4a38,0x1ae9ef8,0x90}, /* Uncharted 3 US */
    {0xb4eeab,0xb9700b,0xebbdc8,0xfaf1c0,0x90},     /* Uncharted 2 EU */
    {0xe7cf5c,0xe96390,0x11160c8,0x1161ef8,0x90},   /* Uncharted 2 US */
    {0x79eaf5,0,0xa8dbf0,0,0x88},                   /* Uncharted 1 EU */
    {0x7962c5,0,0xa51af0,0,0x88},                   /* Uncharted 1 US */
};
static int dev_menu_name_at(uintptr_t base, size_t size, uintptr_t at, const char *name) {
    const size_t n=strlen(name)+1;
    return at && at+n<=size && !memcmp((const void *)(base+at),name,n);
}
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
        const size_t size=runtime_image_size();
        int build=-1;
        for (int b=0;base && b<(int)(sizeof(dev_menu_builds)/sizeof(dev_menu_builds[0])) && build<0;++b)
            if (dev_menu_name_at(base,size,dev_menu_builds[b].dev_name,"Dev Menu") &&
                (!dev_menu_builds[b].quick_name ||
                 dev_menu_name_at(base,size,dev_menu_builds[b].quick_name,"Quick Menu"))) build=b;
        if (build<0) {
            printf("%s: not this game or release (the offsets are for the 1.02 games, EU and US)\n",name);
        } else if (quick && !dev_menu_builds[build].quick_root) {
            printf("%s: this game has none\n",name);
        } else {
            unsigned char *menu=*(unsigned char **)(base+(quick ? dev_menu_builds[build].quick_root
                                                                  : dev_menu_builds[build].dev_root));
            const unsigned open=dev_menu_builds[build].open;
            if (!menu) printf("%s: not built yet\n",name);
            else {
                menu[open]^=1;
                printf("%s: %s (menu at %p)\n",name,menu[open] ? "opened" : "closed",(void *)menu);
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
    rotate_stick(d,injected.rotate ? injected.rotate : host_rotate,now_us());
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
    bbgpu_input_sample(); /* low latency mode: a new frame starts here and may wait (outside the lock) */
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
    if (!(pad_thread_on() && pad_thread_started>0)) current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    if (input.pad_vibration && !keyboard_in_use) {
        const unsigned strength=(unsigned)input.pad_vibration_strength;
        const uint16_t low=(uint16_t)((param[0]*257u*strength+50u)/100u);
        const uint16_t high=(uint16_t)((param[1]*257u*strength+50u)/100u);
        pad_request_rumble(low,high,1000);
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Light bar {r, g, b}: shown on controllers that have one (DualShock 4, DualSense). */
static void set_led(uint8_t r, uint8_t g, uint8_t b) {
    pthread_mutex_lock(&lock);
    pad_request_led(r,g,b);
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
void runtime_pad_report(void) {
    if (pad_thread_on() && pad_thread_started>0) {
        char name[sizeof(snap_name)];
        pthread_mutex_lock(&snap_lock);
        snprintf(name,sizeof(name),"%s",snap_name[0] ? snap_name : "none");
        pthread_mutex_unlock(&snap_lock);
        printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,name);
        return;
    }
    printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none");
}

/* A loader command used by the launcher before the game starts: no Vulkan or guest memory. */
int runtime_pad_tool(int test, int strength) {
    const int already_ready=SDL_WasInit(SDL_INIT_GAMEPAD)!=0;
    if (!already_ready && !SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        fprintf(stderr,"Controller detection failed: %s\n",SDL_GetError());
        return 1;
    }
    int count=0;
    SDL_JoystickID *ids=SDL_GetGamepads(&count);
    SDL_Gamepad *pad=ids && count>0 ? SDL_OpenGamepad(ids[0]) : NULL;
    SDL_free(ids);
    if (!pad) {
        puts("connected=0\nrumble=0");
        if (!already_ready) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return 0;
    }
    const char *name=SDL_GetGamepadName(pad);
    const bool rumble=SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad),
                                             SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN,false);
    puts("connected=1");
    printf("name=%s\n",name ? name : "Game controller");
    printf("rumble=%d\n",rumble ? 1 : 0);
    if (test) {
        if (strength<0) strength=0;
        if (strength>100) strength=100;
        const uint16_t amount=(uint16_t)((65535u*(unsigned)strength+50u)/100u);
        const bool started=rumble && SDL_RumbleGamepad(pad,amount,amount,500);
        if (started) SDL_Delay(500);
        SDL_RumbleGamepad(pad,0,0,0);
        printf("tested=%d\n",started ? 1 : 0);
    }
    SDL_CloseGamepad(pad);
    if (!already_ready) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    return 0;
}
