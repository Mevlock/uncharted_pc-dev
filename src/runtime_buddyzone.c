/* Uncharted 2: companions jitter in place after reaching their combat spot (BB_U2_BUDDY_ZONE=0: off). The game's own bug,
 * on PS4 too: chapter 19 "Siege", checkpoint #257 (village-2-battle-rooftops-climb-up), Tenzin through the double door.
 *
 * BuddyOpenCombatSkill (the companions' open-combat AI; 1.02 0x30d220, 1.00 0xa3f10) holds the companion in a zone (the
 * NPC's zone table: +0x3fb0 the current index, +0x3fc0 + 32 i: center xyz, +0x10 radius, +0x14 height). While the
 * companion is not in the zone (|dy| > height + 0.5, or the xz distance to the center > radius) it sends a MoveToPoint to
 * its spot. The navigation drives there and reports "GOAL REACHED" (its own arrival tolerance), but at #257 Tenzin stops
 * 0.55-0.82 m from the center of a zone of radius 0.5: still "not in the zone", so the skill sends the same MoveToPoint
 * again, the navigation starts walking (WalkStrafe), is there again on the next frame, plays the stop animation and
 * reports success: one start/stop cycle every 5 frames, each moving him ~5 mm, for ~200 frames (~3.5 s at 60 fps) until
 * he has crept inside 0.5 m. Every cycle blends a start and a stop transition: the pose jerks frame to frame (measured
 * 2026-11-14 with BB_NPC_LOG, runtime_npcdiag.c).
 *
 * The fix is hysteresis on the skill's in-zone test, applied ONLY to Tenzin's zone at #257 (siege_tenzin; the user's
 * call: the same logic for every companion could change fights elsewhere). The rule: once a MoveToPoint of the skill to that zone has
 * finished (the skill is in its move state, 4, when it tests again: it only tests again after the navigation is done)
 * or the companion was inside, the zone counts as reached up to 0.5 m beyond its radius, until the companion leaves that
 * band or the zone changes. A normal arrival (inside the radius) and every other caller of the zone test are unchanged.
 * Each time it decides "reached" for a zone it prints one line (NPC, zone, distance).
 * 1.02: the skill's inlined test (vmulss xmm1,xmm1,xmm1 / vpermilps xmm1,xmm1,0 / vcmpltps / vmovmskps; cmp eax,0xf;
 *       jne in-zone): xmm0 = the xz distance squared, xmm1[0] = the radius; the radius is raised.
 * 1.00: Npc::IsInZone (0x2c310) after Zone::Contains returned: al raised to 1, only when it returns to the skill
 *       (0xa4052; the zone test's other callers stay as they are). */
#include "runtime.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZONE_SLACK 0.5f   /* metres beyond the zone radius that still count, once reached */
#define MOVE_STATE 4      /* BuddyOpenCombatSkill +0x18: moving to its spot */

static volatile int lock_word;
static void zlock(void) { while (__atomic_exchange_n(&lock_word,1,__ATOMIC_ACQUIRE)) __builtin_ia32_pause(); }
static void zunlock(void) { __atomic_store_n(&lock_word,0,__ATOMIC_RELEASE); }

enum { SLOTS=64 };
static struct { uint32_t id; float cx, cz, r; int reached; uint64_t seen; } slots[SLOTS];
static uint64_t stamp;
static unsigned long long kicks;

/* The NPC's level name ("npc-..."), when one of its first pointers leads to one (for the log line only). */
static const char *npc_name(const unsigned char *npc) {
    const uint64_t base=runtime_image_base(), size=runtime_image_size();
    for (int off=0x8;off<0x400;off+=8) {
        uint64_t p; memcpy(&p,npc+off,8);
        if (p<0x10000 || p>=0x7fffffffffffull) continue;
        if (!(base && p>=base && p<base+size) && !runtime_memory_is_mapped((uintptr_t)p,8)) continue;
        const char *s=(const char *)(uintptr_t)p;
        if (s[0]=='n' && s[1]=='p' && s[2]=='c' && s[3]=='-') return s;
    }
    return "?";
}

/* The one place it is applied (the user's choice, 2026-11-14: nothing else changes): Tenzin (type 0x6c1c4709) in his
 * zone at chapter 19 "Siege" #257, center (-111.556 11.229 -60.625) radius 0.5. */
static int siege_tenzin(const unsigned char *npc, const float *center, float r) {
    uint32_t type; memcpy(&type,npc+0x30,4);
    return type==0x6c1c4709u && fabsf(center[0]+111.556f)<0.05f && fabsf(center[1]-11.229f)<0.10f &&
           fabsf(center[2]+60.625f)<0.05f && fabsf(r-0.5f)<0.01f;
}

/* d: the xz distance to the zone center (the game's own number), r the radius. 1: count the companion as in the zone.
 * The hysteresis is followed for every companion (it decides nothing by itself); only the #257 case gets the answer.
 * BB_BUDDY_ZONE_LOG=1 prints each place where it would decide "reached" (it changes nothing there). */
static int zone_reached(const unsigned char *npc, const float *center, float r, float d) {
    static int on=-1, log_all=-1;
    if (on<0) { const char *e=getenv("BB_U2_BUDDY_ZONE"); on=!(e && e[0]=='0'); }
    if (log_all<0) { const char *e=getenv("BB_BUDDY_ZONE_LOG"); log_all=e && e[0]=='1'; }
    if ((!on && !log_all) || !(r>0.0f) || !(d==d)) return 0;
    uint32_t id; memcpy(&id,npc+0x44,4);
    int32_t state=-1;
    const unsigned char *skill; memcpy(&skill,npc+0x4020,8);
    if ((uintptr_t)skill>0x10000) memcpy(&state,skill+0x18,4);
    int would=0, kick=0;
    zlock();
    ++stamp;
    int i=0, oldest=0;
    for (;i<SLOTS;++i) { if (slots[i].id==id) break; if (slots[i].seen<slots[oldest].seen) oldest=i; }
    if (i==SLOTS) { i=oldest; slots[i].id=id; slots[i].reached=0; slots[i].cx=center[0]; slots[i].cz=center[2]; slots[i].r=r; }
    slots[i].seen=stamp;
    if (fabsf(slots[i].cx-center[0])>0.01f || fabsf(slots[i].cz-center[2])>0.01f || fabsf(slots[i].r-r)>0.01f) {
        slots[i].cx=center[0]; slots[i].cz=center[2]; slots[i].r=r; slots[i].reached=0;   /* another zone */
    }
    if (d<=r) slots[i].reached=1;                      /* inside: the game says so too */
    else if (d>r+ZONE_SLACK) slots[i].reached=0;       /* left the band: the zone must be reached again */
    else if (slots[i].reached) would=1;
    else if (state==MOVE_STATE) { slots[i].reached=1; would=1; kick=1; ++kicks; }
    const unsigned long long n=kicks;
    zunlock();
    const int applied=would && on && siege_tenzin(npc,center,r);
    if (kick && (applied || log_all)) {
        uint32_t type; memcpy(&type,npc+0x30,4);
        printf("U2 buddy zone: %s (%08x type %08x) stopped %.2f m from its zone (%.3f %.3f %.3f, radius %.2f) after its "
               "move: %s (%llu so far)\n",npc_name(npc),id,type,(double)d,(double)center[0],(double)center[1],
               (double)center[2],(double)r,applied ? "counts as there (Siege #257 fix)" : "would count as there (log only)",n);
        fflush(stdout);
    }
    return applied;
}

/* 1.02, at 0x30d4a3 in BuddyOpenCombatSkill: rbx = the NPC, rax = its zone index * 32. */
void __attribute__((ms_abi)) runtime_u2_buddy_zone_102(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                       const uint64_t *regs) {
    (void)rdi; (void)rsi; (void)image; (void)r14;
    const unsigned char *npc=(const unsigned char *)(uintptr_t)regs[0];
    const uint64_t zi=regs[7];
    if ((uintptr_t)npc<0x10000 || zi>=32*8) return;
    float *x0=runtime_callback_xmm(regs,0), *x1=runtime_callback_xmm(regs,1);
    float center[4]; memcpy(center,npc+0x3fc0+zi,16);
    const float r=x1[0], d=sqrtf(x0[0]);
    if (zone_reached(npc,center,r,d)) x1[0]=r+ZONE_SLACK;
}

/* 1.00, at 0x2c393 in Npc::IsInZone (rbx = the NPC; al = Zone::Contains' answer; the position tested at rsp+0x20,
 * the return address at rsp+0x58). */
void __attribute__((ms_abi)) runtime_u2_buddy_zone_100(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                       const uint64_t *regs) {
    (void)rdi; (void)rsi; (void)r14;
    if (regs[7]&0xff) return;   /* in the zone already */
    const uint64_t rsp=(uint64_t)(uintptr_t)&regs[25];
    uint64_t ret; memcpy(&ret,(const void *)(uintptr_t)(rsp+0x58),8);
    if (ret!=image+0xa4052) return;   /* not BuddyOpenCombatSkill's test */
    const unsigned char *npc=(const unsigned char *)(uintptr_t)regs[0];
    if ((uintptr_t)npc<0x10000) return;
    uint32_t zi; memcpy(&zi,npc+0x3fb0,4);
    if (zi>=8) return;
    float pos[4], zone[6];
    memcpy(pos,(const void *)(uintptr_t)(rsp+0x20),16); memcpy(zone,npc+0x3fc0+32*zi,24);
    const float dx=pos[0]-zone[0], dy=pos[1]-zone[1], dz=pos[2]-zone[2];
    if (fabsf(dy)>zone[5]+0.5f) return;   /* out by height: as the game decided */
    if (zone_reached(npc,zone,zone[4],sqrtf(dx*dx+dz*dz)))
        ((uint64_t *)regs)[7]=(regs[7]&~(uint64_t)0xff)|1;
}
