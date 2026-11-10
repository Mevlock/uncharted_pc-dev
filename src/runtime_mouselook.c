/* Mouse look (see runtime_mouselook.h).
 *
 * Ported from BilalReloaded's Uncharted port, gow3_pc/src/runtime_mouselook.c and gpu/shim/gow3_mouse.cpp
 * (commit 06b77753, 2026-11-09; Copyright BilalReloaded and contributors, GPL-2.0-or-later, as this
 * project). The handlers, the turn math, the steering patches, the blend sharing, the "turn put back"
 * check, the aim and cover handlers and the U1 aim bookkeeping are his; this file adapts them to this
 * loader's callbacks (probe.c callback_thunk: rdi, rsi, the saved rdx/rcx in regs, the third argument
 * replaced by writing the saved rdx), adds the 1.00 game code (EU CUSA02343 1.00 and US CUSA02320 1.00:
 * every address located here, see the tables at the end) and reads every global from a checked
 * instruction of the game's code instead of a fixed address. Comments marked "(Bilal)" keep his notes.
 *
 * The handlers run on the game's thread inside its camera update, once per camera per frame, so they
 * take the mouse movement since the last update and apply it to the camera state the function is about
 * to use. Small stacks: the game runs this in job fibers with 64 KiB stacks. */
#define _GNU_SOURCE
#include "runtime_mouselook.h"
#include "gpu/bbgpu.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MS_ABI __attribute__((ms_abi))

static unsigned char *image;
static uint64_t image_size;

/* ---- switches ------------------------------------------------------------------------------- */
static int knob(const char *name, int fallback) {
    const char *env=getenv(name);
    return env && env[0] ? env[0]!='0' : fallback;
}
int runtime_mouselook_enabled(void) {
    static int on=-1;
    if (on<0) on=knob("BB_MOUSELOOK",1);
    return on;
}
#define KNOB(fn,env,def) static int fn(void) { static int v=-1; if (v<0) v=runtime_mouselook_enabled() && knob(env,def); return v; }
KNOB(share_enabled,"BB_MOUSELOOK_SHARE",1)   /* every camera of a blend gets the frame's turn */
KNOB(steer_enabled,"BB_MOUSELOOK_STEER",1)   /* the wall rules see mouse turns as steering */
KNOB(u1_enabled,"BB_MOUSELOOK_U1",1)
KNOB(u1_step_enabled,"BB_MOUSELOOK_U1_STEP",0) /* (Bilal) opt-in: the turn applied at the stick step */
KNOB(u1_cover_on,"BB_MOUSELOOK_U1_COVER",1)
KNOB(u1_rail_on,"BB_MOUSELOOK_U1_RAIL",1)
KNOB(u1_aim_on,"BB_MOUSELOOK_U1_AIM",1)
KNOB(u2_aim_on,"BB_MOUSELOOK_U2_AIM",1)
KNOB(u3_aim_on,"BB_MOUSELOOK_U3_AIM",1)
KNOB(u2_cover_on,"BB_MOUSELOOK_U2_COVER",1)
KNOB(u3_cover_on,"BB_MOUSELOOK_U3_COVER",1)
static int u1_cover_enabled(void) { return u1_enabled() && u1_cover_on(); }
static int u1_rail_enabled(void) { return u1_enabled() && u1_rail_on(); }
static int u1_steer_enabled(void) { return u1_enabled() && steer_enabled(); }
static int look_debug(void) {
    static int v=-1;
    if (v<0) { const char *e=getenv("BB_MOUSE_DEBUG"); v=e && e[0]=='1'; }
    return v;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;
}
static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000+(uint64_t)ts.tv_nsec/1000;
}

/* ---- the mouse ------------------------------------------------------------------------------ */
/* (Bilal, gow3_mouse.cpp) The hooks take all counts since the last take; the pad read pauses them while
 * it gives the mouse to the right stick (no camera hook at every recent read), so nothing turns twice. */
static pthread_mutex_t take_lock=PTHREAD_MUTEX_INITIALIZER;
static volatile unsigned look_calls;
static volatile int look_paused;
unsigned runtime_mouselook_calls(void) { return __atomic_load_n(&look_calls,__ATOMIC_RELAXED); }
void runtime_mouselook_pause(int paused) { __atomic_store_n(&look_paused,paused!=0,__ATOMIC_RELAXED); }
/* The counts since the last take (scripted runs: the scripted rate over the time since the last take, an
 * even 1000 Hz mouse); 0 when no mouse reaches the game (not captured, the settings menu open, paused). */
static int take_raw(float *dx, float *dy) {
    *dx=*dy=0.0f;
    if (!runtime_mouselook_enabled()) return 0;
    __atomic_add_fetch(&look_calls,1u,__ATOMIC_RELAXED);
    float rx=0.0f, ry=0.0f;
    const int scripted=runtime_pad_scripted_mouse(&rx,&ry);
    pthread_mutex_lock(&take_lock);
    static uint64_t scripted_last;
    float mx=0.0f, my=0.0f;
    int captured;
    if (scripted) {
        const uint64_t t=now_us();
        float dt=scripted_last ? (float)(t-scripted_last)*1e-6f : 0.0f;
        if (dt>0.25f) dt=0.0f;
        scripted_last=t;
        mx=rx*dt; my=ry*dt;
        captured=1;
    } else {
        captured=bbgpu_mouse_take_look(&mx,&my);
    }
    pthread_mutex_unlock(&take_lock);
    if (!captured || look_paused || bbgpu_overlay_captures_input()) return 0;
    *dx=mx; *dy=my;
    return 1;
}
static float settings_sensitivity, settings_aim, settings_vertical;
static int settings_invert_y;
static void read_settings(void) {
    runtime_pad_mouse_settings(&settings_sensitivity,&settings_aim,&settings_vertical,&settings_invert_y);
}
/* Radians per mouse count at sensitivity 1: (Bilal) 0.0011 rad, about 0.063 degrees, a 2,000-count swipe
 * turns about 126 degrees. The launcher's sensitivity scales it (1.00 = his default); while aiming the
 * aiming sensitivity goes on top. BB_MOUSELOOK_DEGREES=<degrees per count at 1.0> for tests. */
static float look_scale(void) {
    static float base=-1.0f;
    if (base<0.0f) {
        const char *e=getenv("BB_MOUSELOOK_DEGREES");
        const float d=e && *e ? (float)atof(e) : 0.0f;
        base=d>0.0f ? d*0.017453292f : 0.0011f;
    }
    read_settings();
    const float s=settings_sensitivity>0.0f ? settings_sensitivity : 1.0f;
    const float aim=runtime_pad_aim_held() && settings_aim>0.0f ? settings_aim : 1.0f;
    return base*s*aim;
}
static float look_invert(void) { return settings_invert_y ? -1.0f : 1.0f; }
float runtime_mouse_camera_vertical(void);   /* runtime_pad.c: "Camera up and down", the cameras that move along the game's arc */
/* The movement with the up/down speed applied. */
static int take_mouse(float *dx, float *dy) {
    if (!take_raw(dx,dy)) return 0;
    read_settings();
    *dy*=settings_vertical>0.0f ? settings_vertical : 1.0f;
    return 1;
}
/* (Bilal) While aiming the aim cameras take the mouse (their own hooks below); the follow camera keeps
 * still (a turn of it would only show after aiming). It neither takes the movement nor counts as a hook
 * call then, so the pad's direct mode while aiming means an aim hook took the mouse at every pad read. */
static int take_look(float *dx, float *dy) {
    if (runtime_pad_aim_held()) { *dx=*dy=0.0f; return 0; }
    return take_mouse(dx,dy);
}
/* (Bilal) The games' own invert options flip the stick when the camera reads it; the mouse-as-stick path
 * (climbing, ...) goes through them, so the direct turns do as well. */
static void game_invert(const int *flags, float *dx, float *dy) {
    if (!flags) return;
    if (flags[0]) *dx=-*dx;
    if (flags[1]) *dy=-*dy;
}

/* ---- trace and debug (test aids) ------------------------------------------------------------ */
/* BB_MOUSE_TRACE=<file> (Bilal's GOW3_MOUSE_TRACE): one line per camera hook call and per pad read.
 * Hooks: "<s> <hook> <camera> <newest> <heading> <pitch> <dx> <dy> <mode> <extra> <extra> <vtable>",
 * heading in degrees about +Y at the call's entry (before the turn), dx/dy the counts it turned by,
 * mode d (direct turn), s (shared: another camera's turn of this frame), - (nothing to turn), p (the pad
 * gave the mouse to the stick), f (fallback: the camera put turns back), u (unusable direction), r
 * (aim released), o (off). Pad reads: "<s> pad <direct> <stick x> <stick y>". */
static FILE *trace_file;
static pthread_mutex_t trace_lock=PTHREAD_MUTEX_INITIALIZER;
static void trace_init(void) {
    const char *path=getenv("BB_MOUSE_TRACE");
    trace_file=path && path[0] ? fopen(path,"w") : NULL;
    if (trace_file) setvbuf(trace_file,NULL,_IOFBF,1<<16);
}
static double trace_seconds(void) { return (double)now_us()*1e-6; }
static float dir_heading(const float *dir) { return dir ? atan2f(dir[0],dir[2])*57.29578f : 0.0f; }
static float dir_pitch(const float *dir) { return dir ? atan2f(dir[1],sqrtf(dir[0]*dir[0]+dir[2]*dir[2]))*57.29578f : 0.0f; }
static _Thread_local float trace_extra[2];
static void mouse_trace(const char *hook, const void *camera, int newest, float heading, float pitch, float dx, float dy,
                        char mode) {
    if (!trace_file) return;
    uintptr_t vtable=camera ? *(const uintptr_t *)camera : 0;
    vtable=vtable>(uintptr_t)image && vtable<(uintptr_t)image+image_size ? vtable-(uintptr_t)image : 0;
    pthread_mutex_lock(&trace_lock);
    fprintf(trace_file,"%.4f %s %p %d %.3f %.3f %.1f %.1f %c %.3f %.3f %#llx\n",trace_seconds(),hook,camera,newest,
            heading,pitch,dx,dy,mode,trace_extra[0],trace_extra[1],(unsigned long long)vtable);
    static unsigned lines;
    if (++lines%32==0) fflush(trace_file);
    pthread_mutex_unlock(&trace_lock);
    trace_extra[0]=trace_extra[1]=0.0f;
}
void runtime_mouselook_trace_pad(int direct, float stick_x, float stick_y) {
    if (!trace_file) return;
    pthread_mutex_lock(&trace_lock);
    fprintf(trace_file,"%.4f pad %d %.3f %.3f\n",trace_seconds(),direct,stick_x,stick_y);
    pthread_mutex_unlock(&trace_lock);
}
/* BB_MOUSE_DEBUG=1: once a second per game the camera's heading, how far it turned and the stick it read. */
static void stick_debug(const char *game, const float *look, const float *stick) {
    static uint64_t last;
    static float last_heading;
    static unsigned calls;
    ++calls;
    const uint64_t now=now_ms();
    if (now-last<1000 || !look || !stick) return;
    const float heading=atan2f(look[0],look[2])*57.29578f;
    float turned=heading-last_heading;
    while (turned>180.0f) turned-=360.0f;
    while (turned<-180.0f) turned+=360.0f;
    printf("Mouse look debug: %s heading %.1f, turned %.1f over %u calls, right stick %.2f %.2f\n",game,heading,
           last ? turned : 0.0f,calls,stick[2],stick[3]);
    fflush(stdout);
    last=now;
    last_heading=heading;
    calls=0;
}

/* ---- game code: the sites and globals of each version ----------------------------------------- */
/* A global the handlers read, found from an instruction of the game's code that loads its address
 * ("lea reg, [rip + disp32]", 7 bytes: the 3 opcode bytes are checked, then the address resolved). */
typedef struct { uint64_t at; unsigned char op[3]; } CodeRef;
static unsigned char *code_ref(const CodeRef *r) {
    if (!r->at || r->at+7>image_size || memcmp(image+r->at,r->op,3)) return NULL;
    int32_t disp;
    memcpy(&disp,image+r->at+3,4);
    const int64_t target=(int64_t)r->at+7+disp;
    return target>0 && (uint64_t)target<image_size ? image+target : NULL;
}
typedef struct {
    const char *version;
    uint64_t follow, step, arc, cover, aim, rail;          /* hook sites (and the arc step called) */
    CodeRef invert, frame_time, clock;                    /* invert base (+0x4818 ..), float, int* */
    uint64_t read_x, read_x_calls[2], after_step;         /* steering patch sites */
    unsigned char read_x_code[19];
} U1Code;
typedef struct {
    const char *version;
    uint64_t input, pitch, aim, cover;
    CodeRef clock, invert, aim_lo, aim_hi;   /* clock struct (+0x184 clock, +0x18c frame time), invert base (+0x485c ..) */
    uint64_t store;                          /* mov [reg + 0x271], al after the input */
    unsigned char store_code[8];
    unsigned store_len;
} U2Code;
typedef struct {
    const char *version;
    uint64_t input, pitch, aim, cover;
    CodeRef clock, options, aim_invert, aim_limits; /* clock.at 0: the clock struct is the input's 7th argument */
    uint64_t store;                                 /* mov [rbx + 0x2c1], al after the input */
} U3Code;
/* Uncharted 1: the same code in both versions, moved (only the rip-relative operands differ). */
static const U1Code u1_codes[]={
    {"1.02",0xaff80,0xaac90,0xaa7b0,0xa4670,0x276a10,0x2a9690,
     {0xb1b60,{0x48,0x8d,0x05}},{0xb0182,{0x4c,0x8d,0x35}},{0xb022e,{0x48,0x8d,0x05}},
     0xb1b60,{0xb0219,0xb08e5},0xb0204,
     {0x48,0x8d,0x05,0xa1,0x66,0x8a,0x00,0x83,0xb8,0x18,0x48,0x00,0x00,0x00,0xc5,0xfa,0x10,0x47,0x08}},
    {"1.00",0xafe90,0xaaba0,0xaa6c0,0xa4590,0x2764c0,0x2a90a0,
     {0xb1a70,{0x48,0x8d,0x05}},{0xb0092,{0x4c,0x8d,0x35}},{0xb013e,{0x48,0x8d,0x05}},
     0xb1a70,{0xb0129,0xb07f5},0xb0114,
     {0x48,0x8d,0x05,0x11,0x27,0x8e,0x00,0x83,0xb8,0x18,0x48,0x00,0x00,0x00,0xc5,0xfa,0x10,0x47,0x08}},
};
/* Uncharted 2 and 3 1.00 are compiled differently from 1.02: located by the camera fields each function
 * uses, its calls and its callers (the struct offsets are the same in both versions). */
static const U2Code u2_codes[]={
    {"1.02",0x428f40,0x4288d0,0x450e40,0x417450,
     {0x4290b6,{0x4c,0x8d,0x2d}},{0x71a867,{0x48,0x8d,0x05}},{0x71b39e,{0x48,0x8d,0x05}},{0x71b3b5,{0x48,0x8d,0x05}},
     0x430a96,{0x88,0x83,0x71,0x02,0x00,0x00},6},
    {"1.00",0x16d920,0x16d280,0x18c9a0,0x161070,
     {0x16d9de,{0x48,0x8d,0x15}},{0x3355d4,{0x48,0x8d,0x05}},{0x334ce9,{0x48,0x8d,0x05}},{0x334d00,{0x48,0x8d,0x05}},
     0x175915,{0x41,0x88,0x84,0x24,0x71,0x02,0x00,0x00},8},
};
static const U3Code u3_codes[]={
    {"1.02",0x4e42f0,0x100ef90,0x511370,0x4cd030,
     {0x4e446a,{0x48,0x8d,0x1d}},{0x4e43f1,{0x48,0x8d,0x0d}},{0x929324,{0x48,0x8d,0x0d}},{0x929951,{0x48,0x8d,0x0d}},
     0x4ec5c3},
    {"1.00",0x11d4e0,0x6a5600,0x13c230,0x10e6d0,
     {0,{0,0,0}},{0x6a99a2,{0x48,0x8d,0x0d}},{0x3194fd,{0x48,0x8d,0x0d}},{0x317bf5,{0x48,0x8d,0x0d}},
     0x125a73},
};
/* The original bytes at each hook site (24: whole instructions, the first 6 moved to the thunk). */
#define PROLOGUE 0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56
#define PUSHES5 PROLOGUE,0x41,0x55,0x41,0x54,0x53
static const unsigned char sig_u1_follow[]={PUSHES5,0x48,0x81,0xec,0x78,0x02,0x00,0x00,0x48,0x89,0xd3,0x49};
static const unsigned char sig_u1_step[]={PUSHES5,0x48,0x81,0xec,0xa8,0x00,0x00,0x00,0xc5,0xfa,0x11,0x85};
static const unsigned char sig_u1_arc[]={PUSHES5,0x48,0x81,0xec,0x88,0x00,0x00,0x00,0xc5,0xfa,0x11,0x45};
static const unsigned char sig_u1_cover[]={PUSHES5,0x48,0x81,0xec,0x88,0x01,0x00,0x00,0x49,0x89,0xd7,0x48};
static const unsigned char sig_u1_aim[]={PUSHES5,0x48,0x81,0xec,0x08,0x03,0x00,0x00,0x45,0x89,0xc4,0x48};
static const unsigned char sig_u1_rail[]={PUSHES5,0x50,0x49,0x89,0xd7,0x49,0x89,0xf4,0x49,0x89,0xfe,0x45};
static const unsigned char sig_u2_input_102[]={PUSHES5,0x48,0x83,0xec,0x78,0xc5,0xfa,0x11,0x85,0x70,0xff,0xff};
static const unsigned char sig_u2_input_100[]={PUSHES5,0x48,0x83,0xec,0x68,0xc5,0xfa,0x11,0x45,0x8c,0x4c,0x89};
static const unsigned char sig_u2_pitch[]={PUSHES5,0x48,0x81,0xec,0x88,0x00,0x00,0x00,0xc5,0xfa,0x11,0x45};
static const unsigned char sig_u2_aim_102[]={PUSHES5,0x48,0x81,0xec,0xd8,0x01,0x00,0x00,0x49,0x89,0xf6,0x49};
static const unsigned char sig_u2_aim_100[]={PUSHES5,0x48,0x81,0xec,0x38,0x01,0x00,0x00,0x49,0x89,0xf6,0x49};
static const unsigned char sig_u2_cover_102[]={PUSHES5,0x48,0x81,0xec,0x68,0x01,0x00,0x00,0x49,0x89,0xd7,0x48};
static const unsigned char sig_u2_cover_100[]={PUSHES5,0x48,0x81,0xec,0xa8,0x01,0x00,0x00,0x49,0x89,0xd7,0x49};
static const unsigned char sig_u3_input_102[]={PUSHES5,0x48,0x81,0xec,0xc8,0x00,0x00,0x00,0x45,0x89,0xc7,0x48};
static const unsigned char sig_u3_input_100[]={PUSHES5,0x48,0x81,0xec,0xc8,0x00,0x00,0x00,0xc5,0xfa,0x11,0x85};
static const unsigned char sig_u3_pitch_102[]={0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x81,0xec,0xa0,0x00,0x00,0x00,
                                               0xc5,0xf8,0x28,0xd1,0x48,0x89,0xfb,0x4c,0x8d,0x35};
static const unsigned char sig_u3_pitch_100[]={PROLOGUE,0x41,0x54,0x53,0x48,0x83,0xec,0x30,0xc5,0xf8,0x28,0xd1,0x49,0x89,
                                               0xfc,0x4c,0x8d};
static const unsigned char sig_u3_aim_102[]={PUSHES5,0x48,0x81,0xec,0xb8,0x06,0x00,0x00,0x48,0x89,0xf3,0x49};
static const unsigned char sig_u3_aim_100[]={PUSHES5,0x48,0x81,0xec,0x98,0x01,0x00,0x00,0x49,0x89,0xf6,0x48};
static const unsigned char sig_u3_cover_102[]={PUSHES5,0x48,0x81,0xec,0xf8,0x00,0x00,0x00,0x49,0x89,0xf6,0x49};
static const unsigned char sig_u3_cover_100[]={PROLOGUE,0x41,0x54,0x53,0x48,0x81,0xec,0x90,0x00,0x00,0x00,0x49,0x89,
                                               0xf6,0x49,0x89,0xff};
static int code_is(uint64_t at, const unsigned char *bytes, size_t n) {
    return at && at+n<=image_size && !memcmp(image+at,bytes,n);
}

/* The version this game runs (NULL: none matched, the mouse stays the right stick) and its globals. */
static const U1Code *u1;
static const U2Code *u2;
static const U3Code *u3;
static struct {
    const int *invert;        /* follow camera: left/right, up/down */
    const int *rail_invert;   /* the jeep's gun */
    const float *frame_time;
    const int *const *clock;  /* -> ints, [2] the frame counter */
} u1g;
static struct {
    const unsigned char *clock_base; /* +0x184 the clock (300 per second), +0x18c the frame time */
    const int *invert, *aim_invert;
    const float *aim_lo, *aim_hi;    /* degrees: down, up */
} u2g;
static struct {
    const unsigned char *clock_base; /* +0x194 the clock, +0x19c the frame time; NULL: the input's 7th argument */
    const unsigned char *const *options;
    const unsigned char *aim_invert, *aim_limits;
} u3g;

void runtime_mouselook_prepare(unsigned char *base, uint64_t size) {
    image=base;
    image_size=size;
    trace_init();
    if (!runtime_mouselook_enabled()) { puts("Mouse look: off (BB_MOUSELOOK=0): the mouse is the right stick"); return; }
    for (size_t i=0;i<sizeof(u1_codes)/sizeof(*u1_codes) && !u1;++i) {
        const U1Code *c=&u1_codes[i];
        if (!code_is(c->follow,sig_u1_follow,24) || !code_is(c->arc,sig_u1_arc,24)) continue;
        const unsigned char *inv=code_ref(&c->invert), *ft=code_ref(&c->frame_time), *clk=code_ref(&c->clock);
        if (!inv || !ft || !clk) { printf("Mouse look: Uncharted 1 %s: a global was not found: the mouse is the right stick\n",c->version); break; }
        u1g.invert=(const int *)(inv+0x4818);
        u1g.rail_invert=(const int *)(inv+0x4820);
        u1g.frame_time=(const float *)ft;
        u1g.clock=(const int *const *)clk;
        u1=c;
        printf("Mouse look: Uncharted 1 (game code %s): follow, cover, aim and jeep gun cameras\n",c->version);
    }
    for (size_t i=0;i<sizeof(u2_codes)/sizeof(*u2_codes) && !u2;++i) {
        const U2Code *c=&u2_codes[i];
        const int v102=i==0;
        if (!code_is(c->input,v102 ? sig_u2_input_102 : sig_u2_input_100,24) || !code_is(c->pitch,sig_u2_pitch,24)) continue;
        const unsigned char *clk=code_ref(&c->clock), *inv=code_ref(&c->invert), *lo=code_ref(&c->aim_lo), *hi=code_ref(&c->aim_hi);
        if (!clk || !inv || !lo || !hi) { printf("Mouse look: Uncharted 2 %s: a global was not found: the mouse is the right stick\n",c->version); break; }
        u2g.clock_base=clk;
        u2g.invert=(const int *)(inv+0x485c);
        u2g.aim_invert=(const int *)(inv+0x4864);
        u2g.aim_lo=(const float *)lo;
        u2g.aim_hi=(const float *)hi;
        u2=c;
        printf("Mouse look: Uncharted 2 (game code %s): follow, cover and aim cameras\n",c->version);
    }
    for (size_t i=0;i<sizeof(u3_codes)/sizeof(*u3_codes) && !u3;++i) {
        const U3Code *c=&u3_codes[i];
        const int v102=i==0;
        if (!code_is(c->input,v102 ? sig_u3_input_102 : sig_u3_input_100,24) ||
            !code_is(c->pitch,v102 ? sig_u3_pitch_102 : sig_u3_pitch_100,24)) continue;
        const unsigned char *clk=c->clock.at ? code_ref(&c->clock) : NULL;
        const unsigned char *opt=code_ref(&c->options), *ainv=code_ref(&c->aim_invert), *lim=code_ref(&c->aim_limits);
        if ((c->clock.at && !clk) || !opt || !ainv || !lim) {
            printf("Mouse look: Uncharted 3 %s: a global was not found: the mouse is the right stick\n",c->version);
            break;
        }
        u3g.clock_base=clk;
        u3g.options=(const unsigned char *const *)opt;
        u3g.aim_invert=ainv;
        u3g.aim_limits=lim;
        u3=c;
        printf("Mouse look: Uncharted 3 (game code %s): follow, cover and aim cameras\n",c->version);
    }
    if (!u1 && !u2 && !u3) puts("Mouse look: no known camera code in this executable (the mouse is the right stick)");
    fflush(stdout);
}

/* ---- shared turn math (Bilal) ---------------------------------------------------------------- */
/* Yaw about +Y: the same angle convention as the games' stick path (positive stick X = negative angle =
 * turning right). */
static void rotate_look(float *look, float dx, float per_count) {
    const float angle=-dx*per_count;
    const float c=cosf(angle), s=sinf(angle);
    const float x=look[0], z=look[2];
    look[0]=x*c+z*s;
    look[2]=z*c-x*s;
}
static float wrap_angle(float a) {
    while (a>3.14159265f) a-=6.28318531f;
    while (a<-3.14159265f) a+=6.28318531f;
    return a;
}
/* (Bilal) One mouse turn per frame for every camera whose input runs in that frame: while the games blend
 * from one follow camera to another (near walls, narrow spots, a new camera setting with another field of
 * view) the input function runs for each camera of the blend, and the stick turns all of them. A camera
 * seen again starts a new frame; the others of the frame get the same turn. */
typedef struct {
    const void *cameras[4];
    unsigned count;
    float dx, dy;
    int took;
    uint64_t at; /* ms of the frame's take */
} FrameTurn;
static pthread_mutex_t frame_lock=PTHREAD_MUTEX_INITIALIZER;
static int frame_take(FrameTurn *t, const void *camera, float *dx, float *dy, char *mode) {
    pthread_mutex_lock(&frame_lock);
    const uint64_t now=now_ms();
    int seen=0;
    for (unsigned i=0;i<t->count;++i) seen|=t->cameras[i]==camera;
    if (share_enabled() && !seen && t->count>0 && t->count<4 && now-t->at<100) {
        t->cameras[t->count++]=camera;
        *dx=t->dx; *dy=t->dy; *mode='s';
        const int took=t->took;
        pthread_mutex_unlock(&frame_lock);
        return took;
    }
    t->took=take_look(dx,dy);
    t->dx=t->took ? *dx : 0.0f;
    t->dy=t->took ? *dy : 0.0f;
    t->cameras[0]=camera;
    t->count=1;
    t->at=now;
    *mode=!t->took ? 'p' : (*dx!=0.0f || *dy!=0.0f) ? 'd' : '-';
    const int took=t->took;
    pthread_mutex_unlock(&frame_lock);
    return took;
}
/* (Bilal, near walls) The follow cameras keep the stick they read and look at it again after turning: the
 * update pulls the camera in front of a wall at once while the stick turns it (|right x| > 0.2), runs a
 * wall timer while it turns hard (|x| >= 0.7 for 0.4 s), and the input function notes the player's
 * steering (a flag, its time: a walking Drake's camera swings behind him only some time after the last
 * steering, the end of a scripted auto turn). The mouse turned the direction with the stick at 0, so near
 * walls the camera acted as if nobody steered it. A mouse turn also puts the stick deflection that turns
 * the camera as fast (the yaw rate over the stick's full rate, smoothed over ~50 ms) into the kept stick
 * for those readers and notes the steering the same way; the input function itself gets a copy of the real
 * stick (it would turn by the deflection otherwise). */
typedef struct {
    float rate;    /* smoothed mouse yaw rate, rad/s */
    float x;       /* the stick deflection that turns as fast, -1..1 */
    uint64_t at;   /* ms of the last update */
} Steer;
static void steer_update(Steer *s, float dx, float per_count, float dt, float full_rate) {
    const uint64_t now=now_ms();
    if (now-s->at>100) s->rate=0.0f;
    s->at=now;
    if (!(dt>1e-4f && dt<0.25f)) dt=1.0f/60.0f;
    const float k=dt/(dt+0.05f);
    s->rate+=(dx*per_count/dt-s->rate)*k;
    float x=full_rate>1e-4f && isfinite(full_rate) ? s->rate/full_rate : 0.0f;
    if (!isfinite(x) || fabsf(x)<0.01f) x=0.0f;
    s->x=x>1.0f ? 1.0f : x<-1.0f ? -1.0f : x;
}
typedef struct {
    unsigned kept;       /* the kept stick (left x, y, right x, y) */
    unsigned steered;    /* byte: 1 = the player steered */
    unsigned steer_time; /* int: the clock when the player last steered */
    unsigned timer;      /* int: the timer the stick X keeps up (within 0.5 s) */
    unsigned auto_turn;  /* byte: a scripted turn toward a direction, ended by steering */
} SteerFields;
static const SteerFields u2_steer_fields={0x344,0x9f30,0x9f00,0x9f08,0x9ed1};
static const SteerFields u3_steer_fields={0x3c4,0x7930,0x7900,0x7908,0x78d0};
static Steer u1_steer, u2_steer, u3_steer;
/* 1 while the mouse turns the camera whose input or stick step ran last: the games' "the player steers"
 * result after it gets this too (runtime_mouselook_install_patches). */
static volatile int mouse_steering;
static int steering_now(const Steer *s) {
    return steer_enabled() && s->x!=0.0f && now_ms()-s->at<100;
}
static _Thread_local float stick_copy[4];
/* The kept stick and the steering notes for one follow camera; returns the stick copy for the input (NULL:
 * nothing written into the kept stick). ticks: the game's clock now. */
static void *steer_camera(const SteerFields *g, const Steer *s, unsigned char *camera, void *stick, int ticks) {
    const float x=s->x;
    if (!steer_enabled() || x==0.0f || !camera) return NULL;
    camera[g->steered]=1;
    *(int *)(camera+g->steer_time)=ticks;
    if (fabsf(x)>0.3f) camera[g->auto_turn]=0;
    int *timer=(int *)(camera+g->timer);
    if ((float)(ticks-*timer)*(1.0f/300.0f)<0.5f) *timer=ticks;
    if ((unsigned char *)stick!=camera+g->kept) return NULL;
    float *kept=(float *)stick;
    memcpy(stick_copy,kept,sizeof(stick_copy));
    if (fabsf(kept[2])<fabsf(x)) kept[2]=x;
    return stick_copy;
}
/* "Turn put back" check (Bilal's U1Look): whether the game kept the last turn shows at the next call; a
 * camera that puts its direction back gets the stick for half a second after 3 such turns in a row. */
typedef struct {
    const char *name;
    const unsigned char *camera; /* the camera the last checked turn went to (NULL: nothing to check) */
    float before, after;         /* its heading (radians) before and after that turn */
    unsigned lost;               /* turns in a row the game did not keep */
    uint64_t stick_until;        /* ms: the mouse is the stick until then */
    unsigned turns, kept, dropped, fallbacks;
    double taken[2];
} LookCheck;
static int look_ready_check(LookCheck *s, const unsigned char *camera, float heading, uint64_t now) {
    if (s->camera==camera) {
        const float turn=fabsf(wrap_angle(s->after-s->before));
        const float from_before=fabsf(wrap_angle(heading-s->before));
        const float from_after=fabsf(wrap_angle(heading-s->after));
        if (from_before<0.2f*turn && from_after>0.8f*turn) {
            ++s->dropped;
            if (++s->lost>=3) {
                s->lost=0;
                s->stick_until=now+500;
                ++s->fallbacks;
                if (look_debug()) {
                    printf("Mouse look debug: %s: the game put the direction back 3 times: the mouse is the stick for 0.5 s\n",s->name);
                    fflush(stdout);
                }
            }
        } else {
            ++s->kept;
            s->lost=0;
        }
    }
    s->camera=NULL;
    return now>=s->stick_until;
}
/* (one camera at a time: a call for another camera, one blending out, leaves the pending check alone) */
static int look_ready(LookCheck *s, const unsigned char *camera, float heading, uint64_t now) {
    if (s->camera && s->camera!=camera) return now>=s->stick_until;
    return look_ready_check(s,camera,heading,now);
}
/* Turns a direction about +Y and keeps the turn for the check at the next call (only turns of 1 degree or
 * more are checked: smaller ones are too close to the game's own swing). */
static void checked_turn(LookCheck *s, const unsigned char *camera, float *dir, float dx, float per_count) {
    const float before=atan2f(dir[0],dir[2]);
    rotate_look(dir,dx,per_count);
    ++s->turns;
    const float after=atan2f(dir[0],dir[2]);
    if (fabsf(wrap_angle(after-before))>=0.01745f) {
        s->camera=camera;
        s->before=before;
        s->after=after;
    }
}
/* A direction the mouse can turn: finite, about unit length and not steeper than ~78 degrees. */
static int aim_usable(const float *dir) {
    const float horizontal=dir[0]*dir[0]+dir[2]*dir[2];
    const float length=horizontal+dir[1]*dir[1];
    return isfinite(length) && length>0.81f && length<1.21f && horizontal>=0.04f;
}
static void aim_limits(float lo, float hi, float *out_lo, float *out_hi) {
    *out_lo=isfinite(lo) && lo<=-1.0f && lo>=-89.0f ? lo : -80.0f;
    *out_hi=isfinite(hi) && hi>=1.0f && hi<=89.0f ? hi : 80.0f;
}
/* Pitches a direction (x, y, z, 0) by -dy counts (mouse back = down), kept inside lo..hi degrees (never
 * pushed further out when it already is). */
static void pitch_dir(float *dir, float dy, float per_count, float lo, float hi) {
    const float length=sqrtf(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
    const float horizontal=sqrtf(dir[0]*dir[0]+dir[2]*dir[2]);
    if (!(length>0.0f) || !(horizontal>1e-4f)) return;
    const float pitch=atan2f(dir[1],horizontal);
    float next=pitch-dy*per_count*look_invert();
    lo*=0.017453292f;
    hi*=0.017453292f;
    if (next>hi && next>pitch) next=pitch>hi ? pitch : hi;
    if (next<lo && next<pitch) next=pitch<lo ? pitch : lo;
    const float c=cosf(next)*length/horizontal;
    dir[0]*=c;
    dir[2]*=c;
    dir[1]=sinf(next)*length;
}

/* ---- Uncharted 3 -------------------------------------------------------------------------------- */
/* (Bilal) Camera input (camera, look direction, stick input, ...): yaw rotates the look direction about +Y
 * by -stickX * ramp * scale * dt degrees; pitch is a 0..1 position on the camera's vertical curve, stepped
 * by the game's own function (camera + 0x7750, pitch, delta) with delta = speed(0x7588) * dt * stickY *
 * scale(0x7608). The mouse goes through the same paths without the ramp, dead zone or speed cap; the
 * vertical step per count matches the horizontal one through the stick rates' ratio. 1.00: the stick is
 * read through two small functions that apply the invert options (same fields), and the clock struct comes
 * as the 7th argument instead of from a global. */
typedef ABI float (*U3PitchStep)(void *limits, float pitch, float delta);
static FrameTurn u3_frame, u2_frame;
static const unsigned char *u3_clock_struct(const uint64_t *regs) {
    if (u3g.clock_base) return u3g.clock_base;
    static const unsigned char *last;
    const unsigned char *p=(const unsigned char *)(uintptr_t)regs[26]; /* [rsp + 8] at the entry: regs[25] is the return address */
    if (p && (uintptr_t)p>0x10000) last=p;
    return last;
}
static const int *u3_invert_flags(const unsigned char *camera) {
    const unsigned char *options=*u3g.options;
    const int player=*(const int *)(camera+0xe4);
    if (!options || player<0 || player>3) return NULL;
    static _Thread_local int flags[2];
    flags[0]=*(const int *)(options+player*4+0x4c50);
    flags[1]=*(const int *)(options+player*4+0x4c58);
    return flags;
}
static void u3_camera_input(unsigned char *camera, float *look, uint64_t *regs) {
    void *stick=(void *)(uintptr_t)regs[5];
    if (look_debug()) stick_debug("U3",look,stick);
    if (!camera) return;
    const unsigned char *clock=u3_clock_struct(regs);
    const float frame_time=clock ? *(const float *)(clock+0x19c) : 1.0f/60.0f;
    float dx=0, dy=0;
    char mode='-';
    const int took=frame_take(&u3_frame,camera,&dx,&dy,&mode);
    const float *f=(const float *)camera;
    if (mode!='s') steer_update(&u3_steer,took ? dx : 0.0f,look_scale(),frame_time,f[0x7578/4]*f[0x7604/4]*0.017453292f);
    trace_extra[0]=(float)((const unsigned char *)stick-camera); /* the kept stick's offset: 0x3c4 */
    trace_extra[1]=u3_steer.x;
    mouse_trace("u3f",camera,1,dir_heading(look),*(const float *)(camera+0x7874),dx,dy,mode);
    void *const steered=clock ? steer_camera(&u3_steer_fields,&u3_steer,camera,stick,*(const int *)(clock+0x194)) : NULL;
    mouse_steering=steering_now(&u3_steer);
    if (steered) regs[5]=(uint64_t)(uintptr_t)steered;
    if (!took || (dx==0.0f && dy==0.0f) || !look) return;
    game_invert(u3_invert_flags(camera),&dx,&dy);
    const float per_count=look_scale();
    if (dx!=0.0f) rotate_look(look,dx,per_count);
    if (dy!=0.0f) {
        const float yaw_full=f[0x7578/4]*f[0x7604/4]*0.017453292f; /* rad/s at full stick */
        const float pitch_full=f[0x7588/4]*f[0x7608/4];             /* curve units/s */
        if (yaw_full>1e-4f && pitch_full>0.0f && isfinite(yaw_full) && isfinite(pitch_full)) {
            const float delta=dy*per_count*look_invert()*runtime_mouse_camera_vertical()*(pitch_full/yaw_full);
            float *pitch=(float *)(camera+0x7874);
            const float next=((U3PitchStep)(image+u3->pitch))(camera+0x7750,*pitch,delta);
            if (isfinite(next)) *pitch=next;
        }
    }
}

/* ---- Uncharted 2 -------------------------------------------------------------------------------- */
/* (Bilal) Camera input with the same arguments and math as Uncharted 3's, other offsets: tuning at
 * *(camera + 0x8d00) (yaw ramp max +0x70, pitch speed +0x80), yaw scale camera + 0x9fa8, pitch scale
 * + 0x9fac, pitch at + 0x9e60 stepped by the game's function (camera, delta), which returns the new pitch. */
typedef ABI float (*U2PitchStep)(void *camera, float delta);
static void u2_camera_input(unsigned char *camera, float *look, uint64_t *regs) {
    void *stick=(void *)(uintptr_t)regs[5];
    if (look_debug()) stick_debug("U2",look,stick);
    if (!camera) return;
    float dx=0, dy=0;
    char mode='-';
    const int took=frame_take(&u2_frame,camera,&dx,&dy,&mode);
    const float *tune=*(const float *const *)(camera+0x8d00);
    if (mode!='s' && tune) {
        const float full_rate=tune[0x70/4]**(const float *)(camera+0x9fa8)*0.017453292f;
        steer_update(&u2_steer,took ? dx : 0.0f,look_scale(),*(const float *)(u2g.clock_base+0x18c),full_rate);
    }
    trace_extra[0]=*(const float *)(camera+0x9fa0);
    trace_extra[1]=u2_steer.x;
    mouse_trace("u2f",camera,1,dir_heading(look),*(const float *)(camera+0x9e60),dx,dy,mode);
    void *const steered=steer_camera(&u2_steer_fields,&u2_steer,camera,stick,*(const int *)(u2g.clock_base+0x184));
    mouse_steering=steering_now(&u2_steer);
    if (steered) regs[5]=(uint64_t)(uintptr_t)steered;
    if (!took || (dx==0.0f && dy==0.0f) || !look) return;
    game_invert(u2g.invert,&dx,&dy);
    const float per_count=look_scale();
    if (dx!=0.0f) rotate_look(look,dx,per_count);
    if (dy!=0.0f && tune) {
        const float *f=(const float *)camera;
        const float yaw_full=tune[0x70/4]*f[0x9fa8/4]*0.017453292f;
        const float pitch_full=tune[0x80/4]*f[0x9fac/4];
        if (yaw_full>1e-4f && pitch_full>0.0f && isfinite(yaw_full) && isfinite(pitch_full)) {
            const float delta=dy*per_count*look_invert()*runtime_mouse_camera_vertical()*(pitch_full/yaw_full);
            const float next=((U2PitchStep)(image+u2->pitch))(camera,delta);
            if (isfinite(next)) *(float *)(camera+0x9e60)=next;
        }
    }
}

/* ---- aim and cover cameras, Uncharted 2 and 3 (Bilal) -------------------------------------------- */
/* While the aim button is held the games turn their aim (zoom) cameras by the right stick through a shared
 * aim function with a dead zone, a response curve and a turn rate that ramps up: the mouse as that stick
 * was slow and accelerated. The aim camera's update (rdi the camera, rsi the input) keeps its aim direction
 * (x, y, z, 0; U3 camera + 0x11e0, U2 + 0x1f30) from frame to frame and only adds the stick's turn to it,
 * so the mouse turns that direction before the update runs (with the aim sensitivity on top): yaw about +Y,
 * pitch kept inside the game's aim limits. The aim function flips the stick by the games' aim invert
 * options, so the mouse does too. The cover cameras turn their flat direction (U3 + 0x2f0, U2 + 0x1c0) and
 * pitch (degrees; U3 + 0x2e0 inside -[+0x21c]..[+0x218]; U2 + 0x1b0 inside -tune[0xc]..tune[8], tune =
 * *(camera + 0x1290)) by the stick with a ramp too; the mouse turns both the same way. */
static LookCheck u3_aim={.name="U3 aim"}, u2_aim={.name="U2 aim"};
static LookCheck u3_cover_look={.name="U3 cover"}, u2_cover_look={.name="U2 cover"};
static int aim_take(LookCheck *s, const int *flags, float *dx, float *dy) {
    if (!runtime_pad_aim_held() || !take_mouse(dx,dy) || (*dx==0.0f && *dy==0.0f)) return 0;
    s->taken[0]+=*dx;
    s->taken[1]+=*dy;
    game_invert(flags,dx,dy);
    return 1;
}
static void aim_input(const char *hook, LookCheck *s, int enabled, unsigned char *camera, float *dir, const int *flags,
                      float lo, float hi) {
    const float heading=dir_heading(dir), pitch=dir_pitch(dir);
    if (!enabled || !runtime_pad_aim_held()) {
        s->camera=NULL;
        mouse_trace(hook,camera,1,heading,pitch,0.0f,0.0f,!enabled ? 'o' : 'r');
        return;
    }
    bbgpu_note_aiming(); /* BB_DOF=aim: no depth of field while aiming */
    if (!aim_usable(dir)) {
        s->camera=NULL;
        mouse_trace(hook,camera,1,heading,pitch,0.0f,0.0f,'u');
        return;
    }
    float dx=0, dy=0;
    if (!look_ready(s,camera,atan2f(dir[0],dir[2]),now_ms())) {
        mouse_trace(hook,camera,1,heading,pitch,0.0f,0.0f,'f');
        return;
    }
    if (!aim_take(s,flags,&dx,&dy)) {
        mouse_trace(hook,camera,1,heading,pitch,0.0f,0.0f,'-');
        return;
    }
    mouse_trace(hook,camera,1,heading,pitch,dx,dy,'d');
    const float per_count=look_scale();
    if (dx!=0.0f) checked_turn(s,camera,dir,dx,per_count);
    if (dy!=0.0f) pitch_dir(dir,dy,per_count,lo,hi);
}
static const int *u3_aim_invert_flags(int player) {
    static _Thread_local int flags[2];
    flags[0]=*(const int *)(u3g.aim_invert+player*4+0x521c);
    flags[1]=*(const int *)(u3g.aim_invert+player*4+0x5224);
    return flags;
}
static void u3_aim_camera_input(unsigned char *camera) {
    if (!camera) return;
    const int player=*(const int *)(camera+0x1e0);
    if (player<0 || player>3) return;
    const float *table=(const float *)(u3g.aim_limits+player*64);
    float lo, hi;
    aim_limits(-table[2],table[3],&lo,&hi);
    aim_input("u3a",&u3_aim,u3_aim_on(),camera,(float *)(camera+0x11e0),u3_aim_invert_flags(player),lo,hi);
}
static void u2_aim_camera_input(unsigned char *camera) {
    if (!camera) return;
    float lo, hi;
    aim_limits(-*u2g.aim_lo,*u2g.aim_hi,&lo,&hi);
    aim_input("u2a",&u2_aim,u2_aim_on(),camera,(float *)(camera+0x1f30),u2g.aim_invert,lo,hi);
}
/* A cover camera: flat direction dir (x, 0, z, 0) and pitch in degrees inside lo..hi; follow-camera input
 * (not while aiming), the follow camera's invert options. */
static void cover_input(const char *hook, FrameTurn *frame, LookCheck *s, int enabled, unsigned char *camera, float *dir,
                        float *pitch, const int *flags, float lo, float hi) {
    const float heading=dir_heading(dir), pitch_in=*pitch;
    if (!enabled) {
        s->camera=NULL;
        mouse_trace(hook,camera,1,heading,pitch_in,0.0f,0.0f,'o');
        return;
    }
    const float horizontal=dir[0]*dir[0]+dir[2]*dir[2];
    if (!isfinite(horizontal) || horizontal<0.81f || horizontal>1.21f || !isfinite(*pitch)) {
        s->camera=NULL;
        mouse_trace(hook,camera,1,heading,pitch_in,0.0f,0.0f,'u');
        return;
    }
    float dx=0, dy=0;
    char mode='f';
    const int ready=look_ready(s,camera,atan2f(dir[0],dir[2]),now_ms());
    const int took=ready && frame_take(frame,camera,&dx,&dy,&mode);
    mouse_trace(hook,camera,1,heading,pitch_in,dx,dy,mode);
    if (!took || (dx==0.0f && dy==0.0f)) return;
    s->taken[0]+=dx;
    s->taken[1]+=dy;
    game_invert(flags,&dx,&dy);
    const float per_count=look_scale();
    if (dx!=0.0f) checked_turn(s,camera,dir,dx,per_count);
    if (dy!=0.0f) {
        float next=*pitch-dy*per_count*look_invert()*57.29578f;
        if (next>hi && next>*pitch) next=*pitch>hi ? *pitch : hi;
        if (next<lo && next<*pitch) next=*pitch<lo ? *pitch : lo;
        if (isfinite(next)) *pitch=next;
    }
}
static void u3_cover_camera_input(unsigned char *camera) {
    if (!camera) return;
    const float lo=-*(const float *)(camera+0x21c), hi=*(const float *)(camera+0x218);
    cover_input("u3c",&u3_frame,&u3_cover_look,u3_cover_on(),camera,(float *)(camera+0x2f0),(float *)(camera+0x2e0),
                u3_invert_flags(camera),isfinite(lo) && lo<0.0f && lo>-90.0f ? lo : -60.0f,
                isfinite(hi) && hi>0.0f && hi<90.0f ? hi : 60.0f);
}
static void u2_cover_camera_input(unsigned char *camera) {
    if (!camera) return;
    const float *tune=*(const float *const *)(camera+0x1290);
    if (!tune) return;
    const float lo=-tune[0xc/4], hi=tune[0x8/4];
    cover_input("u2c",&u2_frame,&u2_cover_look,u2_cover_on(),camera,(float *)(camera+0x1c0),(float *)(camera+0x1b0),
                u2g.invert,isfinite(lo) && lo<0.0f && lo>-90.0f ? lo : -60.0f,
                isfinite(hi) && hi>0.0f && hi<90.0f ? hi : 60.0f);
}

/* ---- Uncharted 1 ------------------------------------------------------------------------------- */
/* (Bilal) The cameras' updates (vtable +0x98, called per camera of the blend) get (rdi) the location they
 * return, (rsi) the camera and (rdx) the params: +0x08 a byte, 1 for the newest camera of the blend (the one
 * the player steers) and 0 for older ones still blending out, +0x10 the pad input, +0x18 the blend
 * progress. The mouse turns the camera's own direction and height before its stick step: no ramp, dead zone
 * or turn-speed cap, the same angle per count as in Uncharted 2 and 3, the vertical step matched to the
 * horizontal one through the stick rates' ratio, with the game's invert options. Whether the game kept a
 * turn shows at the next call (LookCheck). Not the newest camera, an unusable direction or that fallback: no
 * mouse taken, so the pad read makes the mouse the right stick. */
static LookCheck u1_follow={.name="U1 follow"}, u1_cover={.name="U1 cover"};
static int u1_frame(void) {
    const int *clock=*u1g.clock;
    return clock ? clock[2] : -1;
}
static int u1_take(LookCheck *s, float *dx, float *dy) {
    if (!take_look(dx,dy) || (*dx==0.0f && *dy==0.0f)) return 0;
    s->taken[0]+=*dx;
    s->taken[1]+=*dy;
    game_invert(u1g.invert,dx,dy);
    return 1;
}
/* The follow camera (CameraControlFollow2, the default third-person camera): it looks along camera + 0x1310
 * (x, y, z, 0) and sits at + 0x12bc on its vertical arc (0..1). Its stick step turns the direction about +Y
 * by -stickX * rate * scale(+0x13d0) * frame time degrees, the rate (+0x12c0) ramping from tune[0x58] up to
 * tune[0x5c] deg/s while the stick is held (tune = *(camera + 0x1e0)), and moves along the arc with the arc
 * step (camera, delta), delta = tune[0x6c] * scale(+0x13d4) * frame time * stickY, which keeps it in 0..1. */
typedef ABI float (*U1ArcStep)(void *camera, float delta);
static int u1_follow_usable(const float *dir) {
    const float horizontal=dir[0]*dir[0]+dir[2]*dir[2];
    const float length=horizontal+dir[1]*dir[1];
    return isfinite(length) && length>0.81f && length<1.21f && horizontal>=0.25f;
}
static struct {
    int frame; /* the game's frame counter when it was taken, -1: none */
    const unsigned char *camera;
    float dx, dy, per_count;
} u1_frame_turn={.frame=-1};
static void u1_steer_notes(unsigned char *camera) {
    const float x=u1_steer.x;
    if (x==0.0f || !camera || !u1_steer_enabled()) return;
    const int clock=u1_frame();
    if (clock<0) return;
    *(int *)(camera+0x1348)=clock;
    if (fabsf(x)>0.3f) camera[0x1321]=0;
}
static void u1_follow_turn(unsigned char *camera, float *dir, const float *tune, float dx, float dy, float per_count,
                           int newest) {
    if (dx!=0.0f) {
        if (newest) checked_turn(&u1_follow,camera,dir,dx,per_count);
        else rotate_look(dir,dx,per_count);
    }
    if (dy!=0.0f) {
        const float *f=(const float *)camera;
        const float yaw_full=tune[0x5c/4]*f[0x13d0/4]*0.017453292f;
        const float arc_full=tune[0x6c/4]*f[0x13d4/4];
        if (yaw_full>1e-4f && arc_full>0.0f && isfinite(yaw_full) && isfinite(arc_full)) {
            const float delta=dy*per_count*look_invert()*runtime_mouse_camera_vertical()*(arc_full/yaw_full);
            const float next=((U1ArcStep)(image+u1->arc))(camera,delta);
            if (isfinite(next) && next>=0.0f && next<=1.0f) *(float *)(camera+0x12bc)=next;
        }
    }
}
/* (Bilal, opt-in BB_MOUSELOOK_U1_STEP=1) The update's hook decides the turn and the stick step's hook
 * applies it, where the stick turns too; a turn the step did not take is applied at the camera's next update. */
static struct {
    unsigned char *camera;
    const float *tune;
    float dx, dy, per_count;
    int newest;
} u1_pending[4];
static void u1_apply_pending(unsigned char *camera) {
    for (unsigned i=0;i<4;++i) {
        if (u1_pending[i].camera!=camera) continue;
        u1_follow_turn(camera,(float *)(camera+0x1310),u1_pending[i].tune,u1_pending[i].dx,u1_pending[i].dy,
                       u1_pending[i].per_count,u1_pending[i].newest);
        u1_pending[i].camera=NULL;
    }
}
static void u1_queue_turn(unsigned char *camera, const float *tune, float dx, float dy, float per_count, int newest) {
    if (!u1_step_enabled()) {
        u1_follow_turn(camera,(float *)(camera+0x1310),tune,dx,dy,per_count,newest);
        return;
    }
    unsigned slot=0;
    for (unsigned i=0;i<4;++i)
        if (u1_pending[i].camera==camera || !u1_pending[i].camera) { slot=i; break; }
    if (u1_pending[slot].camera && u1_pending[slot].camera!=camera) u1_apply_pending(u1_pending[slot].camera);
    u1_pending[slot].camera=camera;
    u1_pending[slot].tune=tune;
    u1_pending[slot].dx=dx;
    u1_pending[slot].dy=dy;
    u1_pending[slot].per_count=per_count;
    u1_pending[slot].newest=newest;
}
/* (Bilal) The stick step returns whether the player steers (stick not 0); the update hands that to the wall
 * push step, which does nothing while the player steers and otherwise turns the camera away from a wall
 * (measured: 2..3.5 degrees a frame against the mouse at a doorway). The patch after the step's call
 * (runtime_mouselook_install_patches) ORs mouse_steering in. */
static void u1_step_input(unsigned char *camera) {
    if (camera) u1_apply_pending(camera);
    mouse_steering=u1_steer_enabled() && steering_now(&u1_steer);
}
static void u1_follow_debug(const unsigned char *camera, const unsigned char *params) {
    static uint64_t last;
    static float last_heading;
    static unsigned calls, top;
    ++calls;
    if (!params || !params[0x08]) return;
    ++top;
    const uint64_t now=now_ms();
    if (now-last<1000 || !camera) return;
    const float *d=(const float *)(camera+0x1310), *f=(const float *)camera;
    const float heading=atan2f(d[0],d[2])*57.29578f;
    const LookCheck *s=&u1_follow;
    printf("Mouse look debug: U1 follow %u calls (%u newest), heading %.1f, turned %.1f, arc %.3f, rate %.0f, mouse %s, "
           "turns %u kept %u dropped %u fallbacks %u, counts %.0f %.0f\n",
           calls,top,heading,last ? wrap_angle((heading-last_heading)*0.017453292f)*57.29578f : 0.0f,f[0x12bc/4],
           f[0x12c0/4],now<s->stick_until ? "stick (fallback)" : "direct",s->turns,s->kept,s->dropped,s->fallbacks,
           s->taken[0],s->taken[1]);
    fflush(stdout);
    last=now;
    last_heading=heading;
    calls=top=0;
}
static void u1_follow_camera_input(unsigned char *camera, unsigned char *params) {
    if (look_debug()) u1_follow_debug(camera,params);
    if (!u1_enabled() || !camera || !params) return;
    u1_apply_pending(camera); /* a turn its last stick step did not take */
    float *dir=(float *)(camera+0x1310);
    const float *tune=*(const float *const *)(camera+0x1e0);
    const int usable=tune && u1_follow_usable(dir);
    const float heading=dir_heading(dir), arc=*(const float *)(camera+0x12bc);
    if (!params[0x08]) {
        /* An older camera still blending out: the newest camera's turn of this frame (the stick turns every
         * camera of the blend, so the view does not jump back when the game returns to this one). */
        if (usable && u1_frame_turn.frame==u1_frame() && u1_frame_turn.camera!=camera) {
            u1_queue_turn(camera,tune,u1_frame_turn.dx,u1_frame_turn.dy,u1_frame_turn.per_count,0);
            u1_steer_notes(camera);
            mouse_trace("u1f",camera,0,heading,arc,u1_frame_turn.dx,u1_frame_turn.dy,'s');
        } else {
            mouse_trace("u1f",camera,0,heading,arc,0.0f,0.0f,'-');
        }
        return;
    }
    u1_frame_turn.frame=-1;
    if (!usable) {
        u1_follow.camera=NULL;
        mouse_trace("u1f",camera,1,heading,arc,0.0f,0.0f,'u');
        return;
    }
    float dx=0, dy=0;
    const float per_count=look_scale();
    const float yaw_full=tune[0x5c/4]**(const float *)(camera+0x13d0)*0.017453292f;
    const float frame_time=*u1g.frame_time;
    if (!look_ready_check(&u1_follow,camera,atan2f(dir[0],dir[2]),now_ms())) {
        steer_update(&u1_steer,0.0f,per_count,frame_time,yaw_full);
        mouse_trace("u1f",camera,1,heading,arc,0.0f,0.0f,'f');
        return;
    }
    const int took=u1_take(&u1_follow,&dx,&dy);
    steer_update(&u1_steer,took ? dx : 0.0f,per_count,frame_time,yaw_full);
    u1_steer_notes(camera);
    {
        const float *input=*(const float *const *)(params+0x10);
        trace_extra[0]=input ? input[2] : 0.0f; /* the right stick X the step will read */
    }
    trace_extra[1]=*(const float *)(camera+0x12c0);
    if (!took) {
        mouse_trace("u1f",camera,1,heading,arc,dx,dy,'-');
        return;
    }
    mouse_trace("u1f",camera,1,heading,arc,dx,dy,'d');
    u1_queue_turn(camera,tune,dx,dy,per_count,1);
    u1_frame_turn.frame=u1_frame();
    u1_frame_turn.camera=camera;
    u1_frame_turn.dx=dx;
    u1_frame_turn.dy=dy;
    u1_frame_turn.per_count=per_count;
}
/* (Bilal, near walls) After its stick step the follow camera's update reads the right stick X again (the
 * invert option applied), for a wall timer (|x| >= 0.7 held) and to pull the camera in front of a wall at
 * once (|x| > 0.2). Those two calls go here: the stick X or, when larger, the deflection that turns as fast
 * as the mouse; the stick step itself and every other reader keep the plain stick. */
static ABI float u1_stick_x_read(const float *input) {
    float x=input ? input[2] : 0.0f;
    if (*u1g.invert) x=-x;
    const float phantom=u1_steer.x;
    if (phantom!=0.0f && now_ms()-u1_steer.at<100 && fabsf(phantom)>fabsf(x)) x=phantom;
    return x;
}
/* (Bilal) The cover camera (CameraControlCover): active while camera + 0x1f4 is set; tune = *(camera +
 * 0x1300). Its stick step turns the flat direction camera + 0x240 (x, 0, z, 0) about +Y by -stickX *
 * rate(+0x270, ramping tune[0x1c]..tune[0x20] deg/s) * frame time once the frame counter passes camera +
 * 0x1e4, and lowers the pitch + 0x230 (degrees) by tune[0x30] * frame time * stickY, kept inside
 * -tune[0xc]..tune[8]. */
static void u1_cover_camera_input(unsigned char *camera, unsigned char *params) {
    if (!camera || !params || !camera[0x1f4]) return;
    float *dir=(float *)(camera+0x240);
    const float *tune=*(const float *const *)(camera+0x1300);
    const uint64_t now=now_ms();
    const float heading=dir_heading(dir), pitch_in=*(const float *)(camera+0x230);
    if (!u1_cover_enabled() || !params[0x08] || !tune) {
        mouse_trace("u1c",camera,params[0x08],heading,pitch_in,0.0f,0.0f,'o');
        return;
    }
    const float horizontal=dir[0]*dir[0]+dir[2]*dir[2];
    if (!isfinite(horizontal) || horizontal<0.81f || horizontal>1.21f) {
        u1_cover.camera=NULL;
        mouse_trace("u1c",camera,1,heading,pitch_in,0.0f,0.0f,'u');
        return;
    }
    float dx=0, dy=0;
    if (!look_ready_check(&u1_cover,camera,atan2f(dir[0],dir[2]),now)) {
        mouse_trace("u1c",camera,1,heading,pitch_in,0.0f,0.0f,'f');
        return;
    }
    if (!u1_take(&u1_cover,&dx,&dy)) {
        mouse_trace("u1c",camera,1,heading,pitch_in,0.0f,0.0f,'-');
        return;
    }
    /* Still entering cover (the game ignores the stick then): the movement is dropped. */
    const int *clock=*u1g.clock;
    if (clock && clock[2]<=*(const int *)(camera+0x1e4)) {
        mouse_trace("u1c",camera,1,heading,pitch_in,dx,dy,'e');
        return;
    }
    mouse_trace("u1c",camera,1,heading,pitch_in,dx,dy,'d');
    const float per_count=look_scale();
    if (dx!=0.0f) checked_turn(&u1_cover,camera,dir,dx,per_count);
    const float yaw_full=tune[0x20/4]*0.017453292f, pitch_full=tune[0x30/4];
    if (dy!=0.0f && yaw_full>1e-4f && pitch_full>0.0f && isfinite(yaw_full) && isfinite(pitch_full)) {
        float *pitch=(float *)(camera+0x230);
        float next=*pitch-dy*per_count*look_invert()*runtime_mouse_camera_vertical()*(pitch_full/yaw_full);
        if (next<-tune[0xc/4]) next=-tune[0xc/4];
        if (next>tune[0x8/4]) next=tune[0x8/4];
        if (isfinite(next)) *pitch=next;
    }
}
/* (Bilal) The jeep's mounted gun (CameraControlRailVehicle, upstream #250): its update calls the look step
 * (camera, look, pad input): look + 4 the yaw and + 8 the pitch (radians), each turned by -stick * rate * the
 * game's camera speed past a dead zone (right x: rate tune[0x20]; right y: tune[0x1c]; tune = *(camera +
 * 0x1e8 + facing * 8), facing camera + 0x204), with their own invert options, then kept inside the game's
 * limits. The mouse turns the yaw and pitch at the step's entry; the game's limits still apply after it.
 * (Not verified in game by Bilal; BB_MOUSELOOK_U1_RAIL=0: the stick there again.) */
static FrameTurn u1_rail_frame;
static void u1_rail_look_input(unsigned char *camera, unsigned char *look) {
    if (!camera || !look) return;
    float *yaw=(float *)(look+4), *pitch=(float *)(look+8);
    const unsigned facing=*(const unsigned *)(camera+0x204);
    const float *tune=facing<4 ? *(const float *const *)(camera+0x1e8+facing*8) : NULL;
    const float heading=*yaw*57.29578f, pitch_in=*pitch*57.29578f;
    if (!u1_rail_enabled() || !tune) {
        mouse_trace("u1r",camera,1,heading,pitch_in,0.0f,0.0f,'o');
        return;
    }
    if (!isfinite(*yaw) || !isfinite(*pitch) || fabsf(*yaw)>7.0f || fabsf(*pitch)>3.2f) {
        mouse_trace("u1r",camera,1,heading,pitch_in,0.0f,0.0f,'u');
        return;
    }
    float dx=0, dy=0;
    char mode='-';
    const int took=frame_take(&u1_rail_frame,camera,&dx,&dy,&mode);
    mouse_trace("u1r",camera,1,heading,pitch_in,dx,dy,mode);
    if (!took || (dx==0.0f && dy==0.0f)) return;
    game_invert(u1g.rail_invert,&dx,&dy);
    const float per_count=look_scale();
    const float yaw_rate=tune[0x20/4], pitch_rate=tune[0x1c/4];
    const float ratio=isfinite(yaw_rate) && isfinite(pitch_rate) && yaw_rate>1e-3f && pitch_rate>0.0f ? pitch_rate/yaw_rate : 1.0f;
    if (dx!=0.0f) *yaw-=dx*per_count;
    if (dy!=0.0f) *pitch-=dy*per_count*look_invert()*ratio;
}
/* (Bilal) Direct mouse aim in Uncharted 1. The aim camera's update calls the aim function with the controller
 * (rsi) and the camera's remembered aim direction (x, y, z; rcx, the 4th argument). The function turns that
 * direction by the right stick, the camera keeps it, and aim assist and recoil go on top of the shown
 * direction. The mouse turns the remembered direction before the function runs, pitch inside +-70 degrees.
 * The game makes new aim cameras during one press (after a roll, about every 100 ms while Drake gets up; two
 * can take turns within a frame), and each has its own direction: the press's mouse turn is summed, and each
 * controller remembers how much of it it already has, so every camera gets each turn once. A new controller
 * that starts where the last turned direction ended (within 3 degrees) already has the turns; one that
 * starts elsewhere (the game's own direction after a roll) gets them on top. */
static LookCheck u1_aim={.name="U1 aim"};
static struct {
    int held;
    float dx, dy;
    unsigned clock;
    struct {
        const unsigned char *controller;
        float dx, dy;
        unsigned used;
    } given[4];
    float last[3];
    int have_last;
} u1_press;
static void u1_aim_input(unsigned char *controller, float *dir) {
    if (!u1_aim_on() || !controller || !dir) return;
    if (!runtime_pad_aim_held()) { u1_press.held=0; return; }
    bbgpu_note_aiming();
    if (!u1_press.held) {
        memset(&u1_press,0,sizeof(u1_press));
        u1_press.held=1;
    }
    if (!aim_usable(dir)) return;
    float dx=0, dy=0;
    const float heading=dir_heading(dir), pitch=dir_pitch(dir);
    if (aim_take(&u1_aim,u1g.invert,&dx,&dy)) {
        u1_press.dx+=dx;
        u1_press.dy+=dy;
    }
    mouse_trace("u1a",controller,1,heading,pitch,dx,dy,dx!=0.0f || dy!=0.0f ? 'd' : '-');
    int slot=0, found=0;
    for (int i=0;i<4;++i) {
        if (u1_press.given[i].controller==controller) { slot=i; found=1; break; }
        if (u1_press.given[i].used<u1_press.given[slot].used) slot=i;
    }
    if (!found) {
        const float *l=u1_press.last;
        const float dot=dir[0]*l[0]+dir[1]*l[1]+dir[2]*l[2];
        const float lengths=sqrtf((dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2])*(l[0]*l[0]+l[1]*l[1]+l[2]*l[2]));
        const int continues=u1_press.have_last && lengths>0.0f && dot>0.9986f*lengths;
        u1_press.given[slot].controller=controller;
        u1_press.given[slot].dx=continues ? u1_press.dx : 0.0f;
        u1_press.given[slot].dy=continues ? u1_press.dy : 0.0f;
    }
    u1_press.given[slot].used=++u1_press.clock;
    const float turn_x=u1_press.dx-u1_press.given[slot].dx, turn_y=u1_press.dy-u1_press.given[slot].dy;
    const float per_count=look_scale();
    if (turn_x!=0.0f) rotate_look(dir,turn_x,per_count);
    if (turn_y!=0.0f) pitch_dir(dir,turn_y,per_count,-70.0f,70.0f);
    u1_press.given[slot].dx=u1_press.dx;
    u1_press.given[slot].dy=u1_press.dy;
    memcpy(u1_press.last,dir,sizeof(u1_press.last));
    u1_press.have_last=1;
}

/* ---- the callbacks ----------------------------------------------------------------------------- */
/* probe.c's thunk: (rdi, rsi, image, r14, regs), regs = the saved rbx r11 r10 r9 r8 rdx rcx rax rflags (the
 * thunk restores them from there: writing regs[5] gives the function another third argument), then the
 * 128-byte red-zone step, then the return address (regs[25]) and the caller's stack arguments. Each
 * callback runs only for the version the game was found to be (runtime_mouselook_prepare). */
#define CB(name) static void MS_ABI name(uint64_t rdi, uint64_t rsi, uint64_t base, uint64_t r14, const uint64_t *regs)
#define UNUSED (void)base; (void)r14
CB(u1_follow_102) { UNUSED; (void)rdi; if (u1==&u1_codes[0]) u1_follow_camera_input((unsigned char *)rsi,(unsigned char *)regs[5]); }
CB(u1_follow_100) { UNUSED; (void)rdi; if (u1==&u1_codes[1]) u1_follow_camera_input((unsigned char *)rsi,(unsigned char *)regs[5]); }
CB(u1_step_102) { UNUSED; (void)rsi; (void)regs; if (u1==&u1_codes[0]) u1_step_input((unsigned char *)rdi); }
CB(u1_step_100) { UNUSED; (void)rsi; (void)regs; if (u1==&u1_codes[1]) u1_step_input((unsigned char *)rdi); }
CB(u1_cover_102) { UNUSED; (void)rdi; if (u1==&u1_codes[0]) u1_cover_camera_input((unsigned char *)rsi,(unsigned char *)regs[5]); }
CB(u1_cover_100) { UNUSED; (void)rdi; if (u1==&u1_codes[1]) u1_cover_camera_input((unsigned char *)rsi,(unsigned char *)regs[5]); }
CB(u1_aim_102) { UNUSED; (void)rdi; if (u1==&u1_codes[0]) u1_aim_input((unsigned char *)rsi,(float *)regs[6]); }
CB(u1_aim_100) { UNUSED; (void)rdi; if (u1==&u1_codes[1]) u1_aim_input((unsigned char *)rsi,(float *)regs[6]); }
CB(u1_rail_102) { UNUSED; (void)regs; if (u1==&u1_codes[0]) u1_rail_look_input((unsigned char *)rdi,(unsigned char *)rsi); }
CB(u1_rail_100) { UNUSED; (void)regs; if (u1==&u1_codes[1]) u1_rail_look_input((unsigned char *)rdi,(unsigned char *)rsi); }
CB(u2_input_102) { UNUSED; if (u2==&u2_codes[0]) u2_camera_input((unsigned char *)rdi,(float *)rsi,(uint64_t *)regs); }
CB(u2_input_100) { UNUSED; if (u2==&u2_codes[1]) u2_camera_input((unsigned char *)rdi,(float *)rsi,(uint64_t *)regs); }
CB(u2_aim_102) { UNUSED; (void)rsi; (void)regs; if (u2==&u2_codes[0]) u2_aim_camera_input((unsigned char *)rdi); }
CB(u2_aim_100) { UNUSED; (void)rsi; (void)regs; if (u2==&u2_codes[1]) u2_aim_camera_input((unsigned char *)rdi); }
CB(u2_cover_102) { UNUSED; (void)rdi; (void)regs; if (u2==&u2_codes[0]) u2_cover_camera_input((unsigned char *)rsi); }
CB(u2_cover_100) { UNUSED; (void)rdi; (void)regs; if (u2==&u2_codes[1]) u2_cover_camera_input((unsigned char *)rsi); }
CB(u3_input_102) { UNUSED; if (u3==&u3_codes[0]) u3_camera_input((unsigned char *)rdi,(float *)rsi,(uint64_t *)regs); }
CB(u3_input_100) { UNUSED; if (u3==&u3_codes[1]) u3_camera_input((unsigned char *)rdi,(float *)rsi,(uint64_t *)regs); }
CB(u3_aim_102) { UNUSED; (void)rsi; (void)regs; if (u3==&u3_codes[0]) u3_aim_camera_input((unsigned char *)rdi); }
CB(u3_aim_100) { UNUSED; (void)rsi; (void)regs; if (u3==&u3_codes[1]) u3_aim_camera_input((unsigned char *)rdi); }
CB(u3_cover_102) { UNUSED; (void)rsi; (void)regs; if (u3==&u3_codes[0]) u3_cover_camera_input((unsigned char *)rdi); }
CB(u3_cover_100) { UNUSED; (void)rsi; (void)regs; if (u3==&u3_codes[1]) u3_cover_camera_input((unsigned char *)rdi); }
#define SITE(name,at,sig,cb) {name,at,sig,sizeof(sig),6,cb,NULL}
static const RuntimeGuestCallback sites[]={
    SITE("U1 1.02 follow camera (mouse look)",0xaff80,sig_u1_follow,u1_follow_102),
    SITE("U1 1.02 follow camera stick step (mouse look)",0xaac90,sig_u1_step,u1_step_102),
    SITE("U1 1.02 cover camera (mouse look)",0xa4670,sig_u1_cover,u1_cover_102),
    SITE("U1 1.02 aim (mouse look)",0x276a10,sig_u1_aim,u1_aim_102),
    SITE("U1 1.02 jeep gun (mouse look)",0x2a9690,sig_u1_rail,u1_rail_102),
    SITE("U1 1.00 follow camera (mouse look)",0xafe90,sig_u1_follow,u1_follow_100),
    SITE("U1 1.00 follow camera stick step (mouse look)",0xaaba0,sig_u1_step,u1_step_100),
    SITE("U1 1.00 cover camera (mouse look)",0xa4590,sig_u1_cover,u1_cover_100),
    SITE("U1 1.00 aim (mouse look)",0x2764c0,sig_u1_aim,u1_aim_100),
    SITE("U1 1.00 jeep gun (mouse look)",0x2a90a0,sig_u1_rail,u1_rail_100),
    SITE("U2 1.02 camera input (mouse look)",0x428f40,sig_u2_input_102,u2_input_102),
    SITE("U2 1.02 aim camera (mouse look)",0x450e40,sig_u2_aim_102,u2_aim_102),
    SITE("U2 1.02 cover camera (mouse look)",0x417450,sig_u2_cover_102,u2_cover_102),
    SITE("U2 1.00 camera input (mouse look)",0x16d920,sig_u2_input_100,u2_input_100),
    SITE("U2 1.00 aim camera (mouse look)",0x18c9a0,sig_u2_aim_100,u2_aim_100),
    SITE("U2 1.00 cover camera (mouse look)",0x161070,sig_u2_cover_100,u2_cover_100),
    SITE("U3 1.02 camera input (mouse look)",0x4e42f0,sig_u3_input_102,u3_input_102),
    SITE("U3 1.02 aim camera (mouse look)",0x511370,sig_u3_aim_102,u3_aim_102),
    SITE("U3 1.02 cover camera (mouse look)",0x4cd030,sig_u3_cover_102,u3_cover_102),
    SITE("U3 1.00 camera input (mouse look)",0x11d4e0,sig_u3_input_100,u3_input_100),
    SITE("U3 1.00 aim camera (mouse look)",0x13c230,sig_u3_aim_100,u3_aim_100),
    SITE("U3 1.00 cover camera (mouse look)",0x10e6d0,sig_u3_cover_100,u3_cover_100),
};
const RuntimeGuestCallback *runtime_mouselook_callbacks(size_t *count) {
    /* Nothing when off, or for a game whose version was not found (its sites would do nothing). */
    *count=runtime_mouselook_enabled() && (u1 || u2 || u3) ? sizeof(sites)/sizeof(*sites) : 0;
    return sites;
}

/* ---- the steering patches (Bilal) ---------------------------------------------------------------- */
/* A jump from image + at (length bytes, whole instructions) to code at p; the rest becomes nops. */
static int patch_jump(uint64_t at, unsigned length, const unsigned char *p) {
    const int64_t delta=(int64_t)(uintptr_t)p-(int64_t)(uintptr_t)(image+at+5);
    if (delta<INT32_MIN || delta>INT32_MAX || length<5) return 0;
    const int32_t rel=(int32_t)delta;
    image[at]=0xe9;
    memcpy(image+at+1,&rel,4);
    memset(image+at+5,0x90,length-5);
    return 1;
}
/* "jmp [rip]; dq image + resume" */
static unsigned emit_jump_back(unsigned char *p, uint64_t resume) {
    static const unsigned char jmp[]={0xff,0x25,0x00,0x00,0x00,0x00};
    memcpy(p,jmp,sizeof(jmp));
    const uint64_t target=(uint64_t)(uintptr_t)(image+resume);
    memcpy(p+sizeof(jmp),&target,8);
    return sizeof(jmp)+8;
}
/* Uncharted 2 and 3: the follow camera's update stores the input function's "the player steers" result (al)
 * in the camera (U2 + 0x271, read by its wall rules; U3 + 0x2c1). The store (its original bytes, checked)
 * gets "or al, [mouse_steering]" in front. */
static int patch_store_al(unsigned char *p, uint64_t at, const unsigned char *store, unsigned len) {
    if (!code_is(at,store,len)) return 0;
    unsigned n=0;
    p[n++]=0x51;                  /* push rcx */
    p[n++]=0x48; p[n++]=0xb9;     /* mov rcx, &mouse_steering */
    const uint64_t flag=(uint64_t)(uintptr_t)&mouse_steering;
    memcpy(p+n,&flag,8);
    n+=8;
    p[n++]=0x0a; p[n++]=0x01;     /* or al, [rcx] */
    p[n++]=0x59;                  /* pop rcx */
    memcpy(p+n,store,len);        /* the original store */
    n+=len;
    emit_jump_back(p+n,at+len);
    return patch_jump(at,len,p);
}
/* A call redirect stub: keeps every register the game's stick read keeps (all but rax and xmm0), calls the
 * handler (SysV, rdi as given) and returns its float in xmm0. */
static unsigned build_call_stub(unsigned char *p, void *handler) {
    unsigned n=0;
    static const unsigned char push[]={0x51,0x52,0x56,0x57,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53};
    memcpy(p+n,push,sizeof(push)); /* push rcx rdx rsi rdi r8 r9 r10 r11 */
    n+=sizeof(push);
    /* sub rsp, 0xf8 (entry rsp = 8 mod 16, + 8 pushes, + 0xf8 = 0 mod 16 at the call) */
    p[n++]=0x48; p[n++]=0x81; p[n++]=0xec; p[n++]=0xf8; p[n++]=0x00; p[n++]=0x00; p[n++]=0x00;
    for (unsigned r=1;r<16;++r) { /* movdqu [rsp + (r-1)*16], xmm r */
        p[n++]=0xf3;
        if (r>=8) p[n++]=0x44;
        p[n++]=0x0f; p[n++]=0x7f; p[n++]=(unsigned char)(0x84|((r&7)<<3)); p[n++]=0x24;
        const uint32_t off=(r-1)*16;
        memcpy(p+n,&off,4);
        n+=4;
    }
    p[n++]=0x48; p[n++]=0xb8; /* mov rax, handler */
    const uint64_t target=(uint64_t)(uintptr_t)handler;
    memcpy(p+n,&target,8);
    n+=8;
    p[n++]=0xff; p[n++]=0xd0; /* call rax */
    for (unsigned r=1;r<16;++r) { /* movdqu xmm r, [rsp + (r-1)*16] */
        p[n++]=0xf3;
        if (r>=8) p[n++]=0x44;
        p[n++]=0x0f; p[n++]=0x6f; p[n++]=(unsigned char)(0x84|((r&7)<<3)); p[n++]=0x24;
        const uint32_t off=(r-1)*16;
        memcpy(p+n,&off,4);
        n+=4;
    }
    p[n++]=0x48; p[n++]=0x81; p[n++]=0xc4; p[n++]=0xf8; p[n++]=0x00; p[n++]=0x00; p[n++]=0x00;
    static const unsigned char pop[]={0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5f,0x5e,0x5a,0x59};
    memcpy(p+n,pop,sizeof(pop)); /* pop r11 r10 r9 r8 rdi rsi rdx rcx */
    n+=sizeof(pop);
    p[n++]=0xc3;
    return n;
}
/* Uncharted 1: the two stick reads after the follow camera's stick step (u1_stick_x_read) and the step's
 * "steering" result handed on (movzx edx, al; mov rdi, r13) with mouse_steering ORed in. */
static void install_u1_patches(unsigned char *stub) {
    static const unsigned char after_step[]={0x0f,0xb6,0xd0,0x4c,0x89,0xef};
    const uint64_t at=u1->after_step;
    if (!code_is(u1->read_x,u1->read_x_code,sizeof(u1->read_x_code)) || !code_is(at,after_step,sizeof(after_step))) {
        puts("Mouse look: Uncharted 1 steering patches unavailable (other code)");
        return;
    }
    for (unsigned i=0;i<2;++i) {
        const uint64_t c=u1->read_x_calls[i];
        int32_t rel;
        if (c+5>image_size) return;
        memcpy(&rel,image+c+1,4);
        if (image[c]!=0xe8 || (int64_t)c+5+rel!=(int64_t)u1->read_x) {
            puts("Mouse look: Uncharted 1 steering patches unavailable (other code)");
            return;
        }
        const int64_t delta=(int64_t)(uintptr_t)stub-(int64_t)(uintptr_t)(image+c+5);
        if (delta<INT32_MIN || delta>INT32_MAX) return;
    }
    build_call_stub(stub,(void *)u1_stick_x_read);
    for (unsigned i=0;i<2;++i) {
        const uint64_t c=u1->read_x_calls[i];
        const int32_t rel=(int32_t)((int64_t)(uintptr_t)stub-(int64_t)(uintptr_t)(image+c+5));
        memcpy(image+c+1,&rel,4);
    }
    unsigned char *p=stub+1024;
    unsigned n=0;
    p[n++]=0x0f; p[n++]=0xb6; p[n++]=0xd0; /* movzx edx, al */
    p[n++]=0x50;                           /* push rax */
    p[n++]=0x48; p[n++]=0xb8;              /* mov rax, &mouse_steering */
    const uint64_t flag=(uint64_t)(uintptr_t)&mouse_steering;
    memcpy(p+n,&flag,8);
    n+=8;
    p[n++]=0x0b; p[n++]=0x10;              /* or edx, [rax] */
    p[n++]=0x58;                           /* pop rax */
    p[n++]=0x4c; p[n++]=0x89; p[n++]=0xef; /* mov rdi, r13 */
    emit_jump_back(p+n,at+sizeof(after_step));
    const int steer=patch_jump(at,sizeof(after_step),p);
    printf("Mouse look: Uncharted 1 follow camera sees mouse turns as steering (stick reads %#llx %#llx%s)\n",
           (unsigned long long)u1->read_x_calls[0],(unsigned long long)u1->read_x_calls[1],
           steer ? ", step result" : "; step result patch unavailable");
}
void runtime_mouselook_install_patches(unsigned char *(*alloc)(size_t size)) {
    if (!image || !steer_enabled() || (!u1 && !u2 && !u3)) return;
    if (u1 && !u1_steer_enabled()) return;
    unsigned char *stub=alloc ? alloc(4096) : NULL;
    if (!stub) { puts("Mouse look: steering patches unavailable (no stub memory near the image)"); return; }
    if (u1) install_u1_patches(stub);
    static const unsigned char u3_store[]={0x88,0x83,0xc1,0x02,0x00,0x00};
    if (u2)
        printf("Mouse look: Uncharted 2 follow cameras see mouse turns as steering (%#llx %s)\n",
               (unsigned long long)u2->store,patch_store_al(stub,u2->store,u2->store_code,u2->store_len) ? "patched" : "unavailable");
    if (u3)
        printf("Mouse look: Uncharted 3 follow cameras see mouse turns as steering (%#llx %s)\n",
               (unsigned long long)u3->store,patch_store_al(stub,u3->store,u3_store,sizeof(u3_store)) ? "patched" : "unavailable");
    fflush(stdout);
}
