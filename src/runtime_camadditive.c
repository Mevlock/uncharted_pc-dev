/* Uncharted 3: animated camera additives end at full weight: a one-frame camera snap (BB_CAMERA_ADDITIVE_FADE=0: off).
 * The game's own bug, on PS4 too (the U3 desert horse ride: the user's "full-screen hitches"; a PS4 capture shows it).
 *
 * The camera additives (biglib camera-shake.cpp: the animated ones, "dummy-camera-object" camera animations played on
 * top of the gameplay camera: the horse ride's sway, boats, vehicles; per player a table of 50 x 0x50) carry a weight
 * (+0x24) that their Evaluate (1.02 0x10233b0, 1.00 0xe64000) moves toward fade-in x fade-out x intensity: when the
 * additive has a fade-in time (+0x34, ticks of 1/300 s) the weight moves at most 300/fade-in per second, dt-correct
 * (up and down). The intensity spring can be 2.0, and a state change stops the additive with a 0.2 s fade-out
 * (0x1022f60: end = now + 60 ticks), so when the end time (+0x30) passes the weight is still ~1-2 (it can only fall
 * 1/s), and IsDone (now > end, or a one-shot camera animation at its last frame) removes it right there: the camera
 * jumps by the whole remaining additive in one frame (ride: 1.2-1.3 deg yaw and 0.2 units, at 30/60/144 fps alike).
 *
 * The fix, one shared rule for every animated additive: an additive the game takes out (IsDone true in the per-frame
 * removal loop, 1.02 0x10217f6, 1.00 0x6b4d76) while its weight is not yet 0 stays in until its weight reaches 0. Its
 * end time is moved to now (a one-shot animation at its last frame holds that frame), so its target weight is 0 and
 * the game's own limiter brings it down at the game's own rate (300/fade-in per second); with no fade-in time it uses
 * its fade-out time (else 0.2 s, the game's stop fade). Its key (+8) is cleared, so starting the same additive again
 * makes a new one that fades in (a crossfade) instead of restarting this one mid-fade (another snap). Real camera cuts
 * do not go through additives; nothing else changes. Safety: after 3 s past its end it goes regardless. */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W_EPS 1e-4f
#define STOP_FADE 60      /* ticks: the game's own stop fade (0x1022f60's caller passes 60) */
#define SAFETY_TICKS 900  /* 3 s past the end: removed whatever its weight */

typedef struct { uint64_t table_lea, clock_lea; const char *name; } Version;
static const Version versions[2]={{0x10217ce,0x102332d,"1.02"},{0x6b4d4e,0xe6461c,"1.00"}};

/* rip-relative target of a 7-byte lea (bytes checked: 4c 8d 3d = lea r15 / 48 8d 05 = lea rax) */
static unsigned char *lea_target(unsigned char *image, uint64_t at, unsigned char rex, unsigned char modrm) {
    if (image[at]!=rex || image[at+1]!=0x8d || image[at+2]!=modrm) return NULL;
    int32_t d; memcpy(&d,image+at+3,4);
    return image+at+7+d;
}

static void site(int v, uint64_t r14, const uint64_t *regs) {
    static unsigned char *table[2], *clock_ptr[2];
    static int ready[2];
    unsigned char *image=(unsigned char *)runtime_image_base();
    if (!ready[v]) {
        ready[v]=1;
        table[v]=lea_target(image,versions[v].table_lea,0x4c,0x3d);
        clock_ptr[v]=lea_target(image,versions[v].clock_lea,0x48,0x05);
        printf("U3 camera additives: fade-out fix (%s) %s\n",versions[v].name,table[v] && clock_ptr[v] ? "on" : "unavailable");
        fflush(stdout);
    }
    if (!(regs[7]&0xff) || !table[v] || !clock_ptr[v] || r14>3) return;   /* not done, or no globals */
    unsigned char *const *clk=(unsigned char *const *)(clock_ptr[v]+0x18);
    if (!*clk) return;
    const int32_t now=*(const int32_t *)(*clk+0x94);
    unsigned char *e=table[v]+r14*0xfa0+regs[0];   /* r15 + rbx: the entry IsDone was asked about */
    float w; int32_t end, fin, fout;
    memcpy(&w,e+0x24,4); memcpy(&end,e+0x30,4); memcpy(&fin,e+0x34,4); memcpy(&fout,e+0x38,4);
    if (!(w>W_EPS) || now-end>SAFETY_TICKS) return;   /* faded out (or stuck): the game removes it */
    if (end>now) { end=now; memcpy(e+0x30,&end,4); }   /* a one-shot at its last frame: fade from now */
    if (fin<=0) { fin=fout>0 ? fout : STOP_FADE; memcpy(e+0x34,&fin,4); }   /* the weight limiter's rate */
    memset(e+8,0,4);                                     /* retired: a new start of it is a new additive */
    ((uint64_t *)regs)[7]&=~(uint64_t)0xff;              /* not done yet */
    static int said;
    if (!said) { said=1; printf("U3 camera additives: one ended at weight %.2f: fading it out first (logged once)\n",(double)w); fflush(stdout); }
}

/* At the removal loop, after IsDone returned (al) and before "mov ecx,[r13+r14*4]; test al,al": rbx = the entry's
 * offset in the player's table, r14 = the player. */
void __attribute__((ms_abi)) runtime_u3_camera_additive_102(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                            const uint64_t *regs) {
    (void)rdi; (void)rsi; (void)image; site(0,r14,regs);
}
void __attribute__((ms_abi)) runtime_u3_camera_additive_100(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                            const uint64_t *regs) {
    (void)rdi; (void)rsi; (void)image; site(1,r14,regs);
}
