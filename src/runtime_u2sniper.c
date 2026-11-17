/* Uncharted 2, chapter 22 "The Monastery", the sniper fight at the second tower (checkpoint #309, task 05a8cb07
 * gompa-tower-top-of-tower-2): an invisible sniper keeps the fight going and the walkway kills the player. The game's
 * own bug, on PS4 too (BB_U2_MONASTERY_SNIPER=0: off). The level scripts are the same in 1.00 and 1.02
 * (dc1/ps4/ss-gompa.bin byte-identical); the fix has its 1.02 and 1.00 code sites.
 *
 * The scripts (decoded from ss-gompa.bin, 2026-11-17):
 *  - The tower-2 sniper script spawns ai-gt02-sniper-1..3 (the RPG soldier ai-gt02-rpg-1 is in the same group: 4
 *    alive). On every npc-killed event, while fewer than 3 of the group are alive, it spawns the next reinforcement:
 *    sniper-4, -5, -6, then -7 (one per kill, in that order).
 *    sniper-6 also starts the bridge waves (4 s after he spawns) and goes to zone-sniper-6-1, then 6-2; sniper-7 goes
 *    to zone-sniper-7-1 on the bridge.
 *  - sniper-6 and sniper-7 spawn 2.4-2.6 m BEHIND entrance-tunnel-door-1 (the gate at the far end of the bridge), in
 *    the tunnel, and walk out through it.
 *  - The bridge controller spawns 3 waves (wave1-1..10); with the third it tells ss-gompa-entrance-tunnel-gate-1 to
 *    close: 4 s later the gate closes and its nav blocker goes on (enable-nav-blocker-object entrance-tunnel-door-1).
 *  - sniper-7 spawns only on a sniper death after sniper-6 has spawned. A player who clears the bridge waves first
 *    (with three snipers still up) gets the gate closed before sniper-7 exists; the next sniper kill spawns him
 *    behind the closed gate. He can't get out (nav blocker, closed gate), can't be seen or reached, but he is alive:
 *    the sniper group never empties, the fight never ends.
 *  - The walkway's anti-rush region (script 6ac870f5, on enter): while the task is not done and the bridge group or the
 *    sniper group has anyone alive, it raises the snipers' accuracy and damage x200, waits 1 s and calls kill-player.
 *    That kill ignores the health cheat. With sniper-7 sealed in, it fires every time.
 *
 * The fix (the user's choice, 2026-11-17: the developers meant the sniper to come out of the gate): when sniper-6 or
 * sniper-7 is spawned (c-spawn-npc) while entrance-tunnel-door-1's nav blocker is on (the gate has closed), his
 * spawner is moved to zone-sniper-6-1 for that spawn: the game's own spot for sniper-6, 2.8 m outside the gate on the
 * bridge side, at the start of both snipers' route to the bridge. He then takes his post as scripted and can be
 * killed. While the gate is open nothing changes; the spawner's position goes back on its next spawn with the gate
 * open (a checkpoint restart). One line in the log each time it acts.
 *
 * Code sites (DC natives, argc = edi, argv = rsi; 8-byte arguments, a symbol is its 32-bit string id):
 *   1.02: c-spawn-npc 0x2e57b0 (spawner lookup 0x56c7b0 on the table 0x1130390), enable-nav-blocker-object 0xa04f60,
 *         disable-nav-blocker-object 0xa04ef0, kill-player 0x90c790 (trace), npc-die 0x2e4680 and
 *         npc-group-alive-count 0x2db160 (BB_SNIPER_TEST), spawner world locator 0x4bdfe0 (trace).
 *   1.00: c-spawn-npc 0x82a80 (spawner lookup 0x257e90, table 0xf1ddb0), enable 0x3ff550, disable 0x3ff5b0,
 *         kill-player 0x384380, npc-die 0x83790, npc-group-alive-count 0x8a060.
 * A spawner (EntitySpawner): +0x00 position (local), +0x10 rotation, +0x30/+0x38 its process handle and id, +0x58 the
 * level, +0x60 a parent spawner.
 *
 * BB_SNIPER_TRACE=1: logs each spawn, gate nav blocker change and kill-player of this fight, with positions.
 * BB_SNIPER_TEST=1 (tests): from the first sniper spawn, kills NPCs through npc-die in the order that seals sniper-7
 * (snipers 1-4, which brings sniper-6 and the bridge waves; the bridge waves; sniper-5 after the gate has closed, which
 * brings sniper-7) and logs where sniper-7 goes. */
#include "runtime.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SYSV __attribute__((sysv_abi))
typedef void *(SYSV *SpawnerFind)(void *table, uint32_t sid);
typedef void (SYSV *WorldLocator)(float *out, const void *spawner);
typedef uint64_t (SYSV *DcNative)(int argc, uint64_t *argv);

enum { V102, V100 };
static const struct { uint64_t find, table, locator, npc_die, group_count; const char *name; } code[2]={
    {0x56c7b0,0x1130390,0x4bdfe0,0x2e4680,0x2db160,"1.02"},
    {0x257e90,0xf1ddb0,0,0x83790,0x8a060,"1.00"},
};

#define SID_SNIPER_6 0x6531040cu   /* ai-gt02-sniper-6 */
#define SID_SNIPER_7 0x61f019bbu   /* ai-gt02-sniper-7 */
#define SID_POST     0xaeb2facau   /* zone-sniper-6-1: sniper-6's first spot, outside the gate */
#define SID_GATE     0x818f051du   /* entrance-tunnel-door-1 */
#define SID_GROUP_SNIPERS 0x888ee2f0u
#define SID_GROUP_BRIDGE  0xf1df19f3u

static volatile int gate_closed;   /* entrance-tunnel-door-1's nav blocker is on (enable/disable seen) */
static volatile int version=-1;
static volatile int fight_seen;    /* ai-gt02-sniper-1 spawned (BB_SNIPER_TEST starts then) */
static uint64_t image_base;
static int trace_on(void) { static int t=-1; if (t<0) { const char *e=getenv("BB_SNIPER_TRACE"); t=e && e[0]=='1'; } return t; }
static int fix_on(void) { static int f=-1; if (f<0) { const char *e=getenv("BB_U2_MONASTERY_SNIPER"); f=!(e && e[0]=='0'); } return f; }

static const char *who(uint32_t sid) {
    switch (sid) {
    case 0x7b765409u: return "ai-gt02-sniper-1"; case 0x763572d0u: return "ai-gt02-sniper-2";
    case 0x72f46f67u: return "ai-gt02-sniper-3"; case 0x6cb33f62u: return "ai-gt02-sniper-4";
    case 0x687222d5u: return "ai-gt02-sniper-5"; case SID_SNIPER_6: return "ai-gt02-sniper-6";
    case SID_SNIPER_7: return "ai-gt02-sniper-7"; case 0xf56cdee1u: return "ai-gt02-rpg-1";
    default: return NULL;
    }
}

static void *find_spawner(uint32_t sid) {
    if (version<0) return NULL;
    return ((SpawnerFind)(uintptr_t)(image_base+code[version].find))((void *)(uintptr_t)(image_base+code[version].table),sid);
}
/* the spawner's world position (1.02: the game's locator; 1.00: the local one) */
static void spawner_pos(const unsigned char *s, float *p) {
    if (version==V102) {
        float out[8] __attribute__((aligned(16)));
        ((WorldLocator)(uintptr_t)(image_base+code[V102].locator))(out,s);
        memcpy(p,out,12);
    } else memcpy(p,s,12);
}
/* the live NPC of a spawner, or NULL */
static const unsigned char *spawner_npc(const unsigned char *s) {
    uint64_t h; uint32_t id; memcpy(&h,s+0x30,8); memcpy(&id,s+0x38,4);
    if (h<0x10000 || !id) return NULL;
    uint64_t obj; memcpy(&obj,(const void *)(uintptr_t)h,8);
    if (obj<0x10000) return NULL;
    uint32_t oid; memcpy(&oid,(const void *)(uintptr_t)(obj+0x44),4);
    return oid==id ? (const unsigned char *)(uintptr_t)obj : NULL;
}

/* one moved spawner per sniper: where it was, where it was put */
static struct { const unsigned char *spawner; float was[3], put[3]; } moved[2];

static void on_spawn(int v, uint64_t image, uint64_t argc, uint64_t argv) {
    version=v; image_base=image;
    if ((int)argc<1 || argv<0x10000) return;
    uint32_t sid; memcpy(&sid,(const void *)(uintptr_t)argv,4);
    const char *name=who(sid);
    if (!name) return;
    if (sid==0x7b765409u) fight_seen=1;
    unsigned char *s=(unsigned char *)find_spawner(sid);
    if (trace_on()) {
        float p[3]={0,0,0}; if (s) spawner_pos(s,p);
        printf("U2 monastery sniper trace: spawn %s (gate %s) at (%.2f %.2f %.2f)\n",name,gate_closed ? "closed" : "open",
               (double)p[0],(double)p[1],(double)p[2]);
        fflush(stdout);
    }
    if (sid!=SID_SNIPER_6 && sid!=SID_SNIPER_7) return;
    if (!fix_on()) {
        static int told;
        if (!told++) { printf("U2 monastery sniper: fix off (BB_U2_MONASTERY_SNIPER=0)\n"); fflush(stdout); }
        return;
    }
    if (!s) return;
    const int k=sid==SID_SNIPER_7;
    if (!gate_closed) {
        /* gate open: his own spawner position (put back if this fix moved it, e.g. before a checkpoint restart) */
        if (moved[k].spawner==s && !memcmp(s,moved[k].put,12)) {
            memcpy(s,moved[k].was,12);
            printf("U2 monastery sniper: %s's spawner back at its own position (the gate is open)\n",name);
            fflush(stdout);
        }
        moved[k].spawner=NULL;
        return;
    }
    const unsigned char *post=(const unsigned char *)find_spawner(SID_POST);
    if (!post) return;
    if (memcmp(s+0x58,post+0x58,16)) {   /* not in the same level frame (level +0x58, parent +0x60): leave it */
        printf("U2 monastery sniper: %s spawns behind the closed gate; its spot is in another frame: not moved\n",name);
        fflush(stdout);
        return;
    }
    if (moved[k].spawner!=s || memcmp(s,moved[k].put,12)) { moved[k].spawner=s; memcpy(moved[k].was,s,12); }
    memcpy(moved[k].put,post,12);
    memcpy(s,post,12);
    printf("U2 monastery sniper: %s spawns behind entrance-tunnel-door-1 after it closed: spawned outside it at "
           "zone-sniper-6-1 (%.2f %.2f %.2f) instead of (%.2f %.2f %.2f) (Monastery #309 fix)\n",name,(double)moved[k].put[0],
           (double)moved[k].put[1],(double)moved[k].put[2],(double)moved[k].was[0],(double)moved[k].was[1],
           (double)moved[k].was[2]);
    fflush(stdout);
}
void __attribute__((ms_abi)) runtime_u2_sniper_spawn_102(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                         const uint64_t *regs) {
    (void)r14; (void)regs; on_spawn(V102,image,rdi,rsi);
}
void __attribute__((ms_abi)) runtime_u2_sniper_spawn_100(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                         const uint64_t *regs) {
    (void)r14; (void)regs; on_spawn(V100,image,rdi,rsi);
}

static void on_blocker(int v, uint64_t image, uint64_t argc, uint64_t argv, int on) {
    version=v; image_base=image;
    if ((int)argc<1 || argv<0x10000) return;
    uint32_t sid; memcpy(&sid,(const void *)(uintptr_t)argv,4);
    if (sid!=SID_GATE) return;
    gate_closed=on;
    if (trace_on()) {
        printf("U2 monastery sniper trace: entrance-tunnel-door-1 nav blocker %s\n",on ? "on (gate closed)" : "off (gate open)");
        for (int k=0;k<2;++k) {
            const unsigned char *s=(const unsigned char *)find_spawner(k ? SID_SNIPER_7 : SID_SNIPER_6);
            const unsigned char *n=s ? spawner_npc(s) : NULL;
            if (n) printf("U2 monastery sniper trace:   %s alive at (%.2f %.2f %.2f)\n",k ? "ai-gt02-sniper-7" : "ai-gt02-sniper-6",
                          (double)((const float *)(n+0x110))[0],(double)((const float *)(n+0x110))[1],(double)((const float *)(n+0x110))[2]);
        }
        fflush(stdout);
    }
}
#define BLOCKER(fn, v, on) \
    void __attribute__((ms_abi)) fn(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14, const uint64_t *regs) { \
        (void)r14; (void)regs; on_blocker(v,image,rdi,rsi,on); }
BLOCKER(runtime_u2_sniper_gate_on_102,V102,1)
BLOCKER(runtime_u2_sniper_gate_off_102,V102,0)
BLOCKER(runtime_u2_sniper_gate_on_100,V100,1)
BLOCKER(runtime_u2_sniper_gate_off_100,V100,0)

static unsigned long long kill_players;
void __attribute__((ms_abi)) runtime_u2_sniper_kill_player(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                           const uint64_t *regs) {
    (void)rdi; (void)rsi; (void)image; (void)r14; (void)regs;
    printf("U2 monastery sniper trace: kill-player (%llu)\n",++kill_players);
    fflush(stdout);
}

/* BB_SNIPER_TEST=1: the scripted reproduction, run from the U2 frame hook (runtime_checkpoint.c). */
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static uint64_t native(uint64_t fn, uint32_t sid) {
    uint64_t argv[2]={sid,0};
    return ((DcNative)(uintptr_t)(image_base+fn))(1,argv);
}
static int alive_sid(uint32_t sid) {
    const unsigned char *s=(const unsigned char *)find_spawner(sid);
    return s && spawner_npc(s)!=NULL;
}
static void test_kill(uint32_t sid, const char *name) {
    printf("U2 monastery sniper test: npc-die %s\n",name);
    fflush(stdout);
    native(code[version].npc_die,sid);
}
static void test_status(const char *tag) {
    const unsigned char *s=(const unsigned char *)find_spawner(SID_SNIPER_7);
    const unsigned char *n=s ? spawner_npc(s) : NULL;
    const int snipers=(int)native(code[version].group_count,SID_GROUP_SNIPERS);
    const int bridge=(int)native(code[version].group_count,SID_GROUP_BRIDGE);
    if (n) {
        const float *p=(const float *)(n+0x110);
        /* the gate's plane: center (-181.38 20.11 -451.67) in the world (the level's (118.62 20.11 -151.67) + its origin
         * (-300 0 -300), as for the gompa-tower spawners), its forward (0.70 0 -0.71) points into the tunnel */
        const float side=(p[0]+181.38f)*0.703f+(p[2]+451.67f)*-0.711f;
        printf("U2 monastery sniper test: %s: snipers alive %d, bridge alive %d; sniper-7 at (%.2f %.2f %.2f), %.2f m %s the gate\n",
               tag,snipers,bridge,(double)p[0],(double)p[1],(double)p[2],(double)fabsf(side),side>0 ? "BEHIND (tunnel side of)" : "outside");
    } else printf("U2 monastery sniper test: %s: snipers alive %d, bridge alive %d; sniper-7 not alive\n",tag,snipers,bridge);
    fflush(stdout);
}
void runtime_u2_sniper_test_frame(uint64_t image, int v) {
    static int on=-1, step;
    static double t0, next;
    static int wave_i;
    if (on<0) { const char *e=getenv("BB_SNIPER_TEST"); on=e && e[0]=='1'; }
    if (!on || !fight_seen || version!=v) return;   /* starts once this fight's first sniper spawned */
    image_base=image;
    const double t=now_s();
    if (!t0) { t0=t; next=t+8; printf("U2 monastery sniper test: start\n"); fflush(stdout); }
    if (t<next) return;
    /* the RPG soldier counts in the sniper group (4 alive at the start): kills 2, 3, 4 bring sniper-4, -5, -6 */
    static const uint32_t first[4]={0x7b765409u,0x763572d0u,0x72f46f67u,0x6cb33f62u};
    static const char *const first_name[4]={"ai-gt02-sniper-1","ai-gt02-sniper-2","ai-gt02-sniper-3","ai-gt02-sniper-4"};
    static const uint32_t waves[10]={0x84a5dd27u,0x89e6fbfeu,0x8d27e649u,0x9360b64cu,0x97a1abfbu,0x9ae28d22u,0x9e239095u,
                                     0xa66c2d28u,0xa2ad309fu,0x0be7dca2u};
    if (step<4) { test_kill(first[step],first_name[step]); ++step; next=t+1.5; test_status("after a sniper kill"); return; }
    if (step==4) {   /* the bridge waves: kill whoever of them is alive, every 0.5 s, until the gate has closed */
        if (gate_closed) { step=5; next=t+2; test_status("gate closed"); return; }
        if (t-t0>240) { printf("U2 monastery sniper test: the gate did not close in 240 s\n"); test_status("timeout"); on=0; return; }
        for (int k=0;k<10;++k) {
            const int i=(wave_i+k)%10;
            if (alive_sid(waves[i])) {
                char nm[40]; snprintf(nm,sizeof(nm),"ai-gt02-bridge-wave1-%d",i+1);
                test_kill(waves[i],nm); wave_i=i+1; break;
            }
        }
        next=t+0.5;
        static double last_status;
        if (t-last_status>10) { last_status=t; test_status("bridge waves"); }
        return;
    }
    if (step==5) { test_kill(0x687222d5u,"ai-gt02-sniper-5"); step=6; next=t+1; return; }   /* brings sniper-7 */
    if (step>=6 && step<26) { test_status("after sniper-5"); ++step; next=t+1; return; }
    if (step==26) { test_kill(SID_SNIPER_6,"ai-gt02-sniper-6"); test_kill(SID_SNIPER_7,"ai-gt02-sniper-7"); ++step; next=t+2; return; }
    if (step==27) { test_status("final"); ++step; printf("U2 monastery sniper test: done\n"); fflush(stdout); on=0; }
}
