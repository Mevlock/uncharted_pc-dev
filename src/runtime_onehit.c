/* One Hit Kills for Uncharted 2 (Uncharted 3 has its own debug switch): a hook at the start of the NPCs'
 * Npc::ComputeArmorDamage (npc code), which applies an attack's damage to the NPC's health (Npc::RespondToAttack, " -
 * received attack for %d damage", runs after it and only reacts: damage changed there killed nothing): rdi = the NPC hit, rsi = the attack record
 * {+4 kind, +8 attacker handle (slot pointer, +0x10 its id), +0x24 damage}. The attacker comes from the handle as the
 * game itself reads it (slot -> object, valid while object+0x44 == the id); its +0x30 is its type id, +0x150 its
 * faction (the friend/enemy table's row).
 * The player's type id is 0xa5a63e9b (faction 1; measured 2026-11-11: its bullets did 13-54, an ally, type
 * 3c5648dd faction 7, always 9, enemies fd2c9c09 faction 2). With the cheat on (BB_CHEATS holds "onehit", run.py: this
 * game's cheats) an attack of the player's does 100000 damage: the NPC dies at once; allies' and enemies' attacks
 * stay as they are (an ally shot by an enemy is not a one-hit kill).
 * BB_ONEHIT_LOG=1: each attack is printed (kind, damage, attacker type and faction, the NPC's faction). */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void __attribute__((ms_abi)) runtime_u2_npc_damage(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                          const uint64_t *regs) {
    (void)image; (void)r14; (void)regs;
    if ((!onehit_on() && !onehit_log()) || !rsi || !rdi) return;
    unsigned char *attack=(unsigned char *)(uintptr_t)rsi;
    const unsigned char *npc=(const unsigned char *)(uintptr_t)rdi;
    const unsigned char *attacker=attacker_of(attack);
    if (onehit_on() && attacker) {
        uint32_t attacker_type; int32_t current;
        memcpy(&attacker_type,attacker+0x30,4); memcpy(&current,attack+0x24,4);
        if (attacker_type==U2_PLAYER_TYPE && current>0 && current<ONE_HIT_DAMAGE) {
            const int32_t lethal=ONE_HIT_DAMAGE;
            memcpy(attack+0x24,&lethal,4);
        }
    }
    if (!onehit_log()) return;
    uint32_t kind, type=0, faction=0xffffffff, npc_faction, npc_type; int32_t damage;
    memcpy(&kind,attack+4,4); memcpy(&damage,attack+0x24,4);
    memcpy(&npc_faction,npc+0x150,4); memcpy(&npc_type,npc+0x30,4);
    if (attacker) { memcpy(&type,attacker+0x30,4); memcpy(&faction,attacker+0x150,4); }
    printf("OneHit: attack kind %u, damage %d, attacker %p type %08x faction %u -> npc %p type %08x faction %u\n",
           kind,damage,(const void *)attacker,type,faction,(const void *)npc,npc_type,npc_faction);
}
