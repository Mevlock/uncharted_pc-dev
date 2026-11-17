/* One Hit Kills for Uncharted 2 (Uncharted 3 has its own debug switch). Npc::ComputeArmorDamage (npc code) works out
 * what an attack does to the NPC hit: rdi = the NPC, rsi = the attack record {+4 kind, +8 attacker handle (slot
 * pointer, +0x10 its id), +0x20 "does damage" (bullets: 0 = none), +0x24 damage, +0x30 hit points}. The armour step
 * runs the damage through the NPC's armour pieces (npc+0x4098, count +0x40a0): each piece hit takes what it still
 * holds out of the damage (a table by attack and armour type can replace the damage first) and lets only a share of
 * the rest through (the piece's own factor), so the heavies in body armour and the helmeted shotgunners took next to
 * nothing until their armour was off, whatever the attack's damage was. The damage after armour then goes (scaled by
 * the difficulty's player damage when the attacker is the player) into the record the caller hands on to the
 * health code (Npc::RespondToAttack only reacts).
 * The hooks sit where the damage after armour is final, so armour can no longer absorb it:
 * - 1.00 (EU and US alike): in the caller, right after both helpers (kind 3: 0x300f0, the rest: 0x30430) return it in
 *   eax: mov r14d, eax at 0x2fff0, r14 = the attack.
 * - 1.02 (EU, US and RU alike; both helpers inlined into 0x244ef0, which also writes the record): where the final
 *   damage at [rbp-0x4040] is copied into the record, 0x245b3e (after the player's difficulty scale: 100000 stays
 *   lethal). No register holds the attack there, so the hook at the function's start (0x244ef0) notes it per thread.
 * The attacker comes from the handle as the game itself reads it (slot -> object, valid while object+0x44 == the id);
 * its +0x30 is its type id, +0x150 its faction (the friend/enemy table's row).
 * The player's type id is 0xa5a63e9b (faction 1; measured 2026-11-11: its bullets did 13-54, an ally, type
 * 3c5648dd faction 7, always 9, enemies fd2c9c09 faction 2). With the cheat on (BB_CHEATS holds "onehit", run.py: this
 * game's cheats) an attack of the player's with damage does 100000 after armour: the NPC dies at once; allies' and
 * enemies' attacks stay as they are (an ally shot by an enemy is not a one-hit kill), and so do the player's attacks
 * the game gives no damage (no damage before armour, or a bullet with +0x20 clear).
 * BB_ONEHIT_LOG=1: each attack is printed (kind, damage before and after armour, attacker type and faction). */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define U2_PLAYER_TYPE 0xa5a63e9bu
#define ONE_HIT_DAMAGE 100000

static int onehit_on(void) {
    static int on=-1;
    if (on<0) {
        const char *e=getenv("BB_CHEATS");
        char list[512];
        snprintf(list,sizeof(list),",%s,",e ? e : "");
        on=strstr(list,",onehit,")!=NULL;
        if (on) printf("Cheats: One Hit Kills on (the player's attacks on NPCs)\n");
    }
    return on;
}

static int onehit_log(void) {
    static int on=-1;
    if (on<0) { const char *e=getenv("BB_ONEHIT_LOG"); on=e && e[0]=='1'; }
    return on;
}

/* The attacker object of an attack record, or NULL (no handle, or a stale one). */
static const unsigned char *attacker_of(const unsigned char *attack) {
    uint64_t slot; uint32_t id;
    memcpy(&slot,attack+8,8); memcpy(&id,attack+0x10,4);
    if (!slot || !id) return NULL;
    const unsigned char *object; memcpy(&object,(const void *)(uintptr_t)slot,8);
    if (!object) return NULL;
    uint32_t object_id; memcpy(&object_id,object+0x44,4);
    return object_id==id ? object : NULL;
}

/* The damage after armour (*damage, the low 32 bits) of `attack`: the player's attacks with damage do 100000. */
static void u2_armor_damage(const unsigned char *attack, uint32_t *damage) {
    const unsigned char *attacker=attacker_of(attack);
    uint32_t kind, type=0, faction=0xffffffff; int32_t raw; unsigned char does_damage;
    memcpy(&kind,attack+4,4); memcpy(&does_damage,attack+0x20,1); memcpy(&raw,attack+0x24,4);
    if (attacker) { memcpy(&type,attacker+0x30,4); memcpy(&faction,attacker+0x150,4); }
    const int32_t after=(int32_t)*damage;
    /* kind 3 does not look at +0x20; for the rest, 0 there with nothing after armour = an attack without damage */
    const int harmless=after==0 && kind!=3 && !does_damage;
    if (onehit_on() && type==U2_PLAYER_TYPE && raw>0 && !harmless && after<ONE_HIT_DAMAGE)
        *damage=(uint32_t)ONE_HIT_DAMAGE;
    if (onehit_log())
        printf("OneHit: attack kind %u, damage %d before armour, %d after, attacker %p type %08x faction %u -> %d\n",
               kind,raw,after,(const void *)attacker,type,faction,(int32_t)*damage);
}

/* 1.00: the caller after both helpers, r14 = the attack, rax = the damage after armour (regs[7]). */
void __attribute__((ms_abi)) runtime_u2_armor_damage_100(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                         const uint64_t *regs) {
    (void)rdi; (void)rsi; (void)image;
    if ((!onehit_on() && !onehit_log()) || !r14) return;
    uint64_t *rax=&((uint64_t *)(uintptr_t)regs)[7];
    uint32_t damage=(uint32_t)*rax;
    u2_armor_damage((const unsigned char *)(uintptr_t)r14,&damage);
    *rax=(*rax&~(uint64_t)0xffffffffu)|damage;
}

/* 1.02: the attack of the Npc::ComputeArmorDamage running on this thread (its start notes it; nothing it calls
 * computes another NPC's damage), taken by the copy into the record below. */
static _Thread_local const unsigned char *u2_attack_102;
void __attribute__((ms_abi)) runtime_u2_npc_damage(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                   const uint64_t *regs) {
    (void)rdi; (void)image; (void)r14; (void)regs;
    u2_attack_102=(const unsigned char *)(uintptr_t)rsi;
}
/* 1.02: mov rax, [rbp-0x4040] (the final damage) at 0x245b3e; rsp = rbp-0x4120 there (fixed frame: sub rsp,0x40f8
 * after six pushes), so the damage is at the guest's rsp + 0xe0. */
void __attribute__((ms_abi)) runtime_u2_armor_damage_102(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                         const uint64_t *regs) {
    (void)rdi; (void)rsi; (void)image; (void)r14;
    const unsigned char *attack=u2_attack_102;
    u2_attack_102=NULL;
    if ((!onehit_on() && !onehit_log()) || !attack) return;
    const uint64_t rsp=(uint64_t)(uintptr_t)&regs[25];
    uint32_t damage;
    memcpy(&damage,(const void *)(uintptr_t)(rsp+0xe0),4);
    u2_armor_damage(attack,&damage);
    memcpy((void *)(uintptr_t)(rsp+0xe0),&damage,4);
}

/* Uncharted 1's jet ski (chapters 8 and 12-13): Nate drives, Elena shoots where the player aims, and the game counts
 * her shots as hers (attacker side 5, an ally), so One Hit Kills did nothing there. Her BuddyJetskiSkill update
 * (runtime_u1_jetski_skill, 0x4bf80 in all four code versions, El Dorado found it) runs every frame while she rides;
 * while it ran less than half a second ago, an ally's attack counts as the player's. Enemies (side 2) hitting her or
 * Nate stay as they are, and allies outside the jet ski too. */
static volatile uint64_t jetski_seen_ns;
static uint64_t onehit_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static int u1_jetski_riding(void) {
    const uint64_t seen = __atomic_load_n(&jetski_seen_ns, __ATOMIC_RELAXED);
    return seen && onehit_now_ns() - seen < 500000000u;
}

/* Uncharted 1 Npc::RespondToAttack (npc.cpp; EU and 1.02 both at 0x13ed0): rdi = the NPC, rsi = the attack record
 * {+8 the attacker's record, +0x14 damage as a share of the NPC's health (float: the player's body shots 0.375, its
 * headshots 375)}; the attacker record's +0x60 is 1 for the player (2 and 5 for the others measured 2026-11-11). With
 * the cheat on the player's attacks do 1000 (any NPC dies at once), the others stay as they are. RespondToAttack reads
 * the damage itself and applies it (no armor step before it, unlike Uncharted 2). BB_ONEHIT_LOG=1: the record's first
 * 24 words and the attacker's 32. */
void __attribute__((ms_abi)) runtime_u1_respond_to_attack(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                          const uint64_t *regs) {
    (void)image; (void)r14; (void)regs;
    if ((!onehit_on() && !onehit_log()) || !rsi || !rdi) return;
    if (onehit_on()) {
        uint64_t who; memcpy(&who,(const void *)(uintptr_t)(rsi+8),8);
        uint32_t side=0; if (who) memcpy(&side,(const void *)(uintptr_t)(who+0x60),4);
        float damage; memcpy(&damage,(const void *)(uintptr_t)(rsi+0x14),4);
        if ((side==1 || (side==5 && u1_jetski_riding())) && damage>0.0f && damage<1000.0f) {
            const float lethal=1000.0f;
            memcpy((void *)(uintptr_t)(rsi+0x14),&lethal,4);
        }
    }
    if (!onehit_log()) return;
    uint32_t w[24];
    memcpy(w,(const void *)(uintptr_t)rsi,sizeof(w));
    printf("OneHit1: npc %p attack",(const void *)(uintptr_t)rdi);
    for (int i=0;i<24;++i) printf(" %08x",w[i]);
    uint64_t attacker; memcpy(&attacker,(const void *)(uintptr_t)(rsi+8),8);
    if (attacker) {
        uint32_t o[32];
        memcpy(o,(const void *)(uintptr_t)attacker,sizeof(o));
        printf(" | attacker");
        for (int i=0;i<32;++i) printf(" %08x",o[i]);
    }
    printf("\n");
}

/* Uncharted 1 BuddyJetskiSkill update (0x4bf80, all four code versions): Elena rides the jet ski this frame. */
void __attribute__((ms_abi)) runtime_u1_jetski_skill(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                     const uint64_t *regs) {
    (void)rdi; (void)rsi; (void)image; (void)r14; (void)regs;
    if (!onehit_on() && !onehit_log()) return;
    const uint64_t now = onehit_now_ns();
    const uint64_t seen = __atomic_load_n(&jetski_seen_ns, __ATOMIC_RELAXED);
    if (onehit_log() && (!seen || now - seen >= 500000000u)) printf("OneHit1: Elena rides the jet ski: her shots count as the player's\n");
    __atomic_store_n(&jetski_seen_ns, now, __ATOMIC_RELAXED);
}
