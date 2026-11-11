/* One Hit Kills for Uncharted 2 (Uncharted 3 has its own debug switch): a hook at the start of the enemies'
 * Npc::RespondToAttack (npc code, " - received attack for %d damage"): rdi = the NPC hit, rsi = the attack record
 * {+4 kind, +8 attacker handle (slot pointer, +0x10 its id), +0x24 damage}. The attacker comes from the handle as the
 * game itself reads it (slot -> object, valid while object+0x44 == the id); its +0x30 is its type id, +0x150 its
 * faction (the friend/enemy table's row).
 * BB_ONEHIT_LOG=1: each attack is printed (kind, damage, attacker type and faction, the NPC's faction), to tell the
 * player's attacks from the others. */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void __attribute__((ms_abi)) runtime_u2_respond_to_attack(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                          const uint64_t *regs) {
    (void)image; (void)r14; (void)regs;
    if (!onehit_log() || !rsi || !rdi) return;
    const unsigned char *attack=(const unsigned char *)(uintptr_t)rsi, *npc=(const unsigned char *)(uintptr_t)rdi;
    const unsigned char *attacker=attacker_of(attack);
    uint32_t kind, type=0, faction=0xffffffff, npc_faction, npc_type; int32_t damage;
    memcpy(&kind,attack+4,4); memcpy(&damage,attack+0x24,4);
    memcpy(&npc_faction,npc+0x150,4); memcpy(&npc_type,npc+0x30,4);
    if (attacker) { memcpy(&type,attacker+0x30,4); memcpy(&faction,attacker+0x150,4); }
    printf("OneHit: attack kind %u, damage %d, attacker %p type %08x faction %u -> npc %p type %08x faction %u\n",
           kind,damage,(const void *)attacker,type,faction,(const void *)npc,npc_type,npc_faction);
}
