/* Checkpoint jumps (the overlay's Checkpoints page): the game's own task jump, PlayTask, as Naughty Dog's
 * Dev Menu > Tasks calls it, run at the start of the menu manager's per-frame update (the frame point where
 * the Dev Menu runs it: the game-frame update, GameFramePreRenderUpdate). A jump the overlay asks for waits
 * there for the next frame.
 *
 * All three games, 1.00 code (EU and US 1.00) and 1.02 code (EU, US and EU/Russian 1.02); U1 and U2 below.
 * Each function is checked against its first bytes before it is called; a release where they differ is
 * "not supported" (no call).
 *
 * Practice mode: from the first jump until the game restarts, the game's campaign save (SAVEDATA0<game>0)
 * mounts from <saves>.backups/u<game>/practice instead (runtime_savedata.c), a copy of the campaign save made
 * before the jump, so the jumps and everything played after them never reach the player's own save. */
#include "runtime.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    const char *name;
    uint64_t menu_update, play_task, find_task, task_mgr;
    const unsigned char *find_code;
    size_t find_size;
} Build;

/* PlayTask: the same bytes in both releases (up to the first rip-relative lea). */
static const unsigned char play_task_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,
                                             0x98,0x00,0x00,0x00,0x41,0x89,0xcf,0x48,0x89,0xd3,0x41,0x89,0xf6,0x49,0x89,0xfc};
static const unsigned char find_task_eu[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x41,0x89,0xf6,0x49,0x89,
                                           0xff,0x4d,0x8b,0x27,0x49,0x8b,0x47,0x30,0x49,0x39,0xc4,0x74,0x13};
static const unsigned char find_task_us[]={0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x83,0xec,0x10,0x4c,0x8d,0x05};
static const Build builds[]={
    {"Uncharted 3 EU",0x6cbdb0,0x876ea0,0x873270,0x1a220c0,find_task_eu,sizeof(find_task_eu)},
    {"Uncharted 3 1.02",0x103ff10,0x1266080,0x125eab0,0x1b3ee70,find_task_us,sizeof(find_task_us)},
};

static _Atomic uint32_t requested;   /* task id waiting for the next frame, 0: none */
static _Atomic int practice;          /* campaign saves go to the practice copy */
static _Atomic int result;            /* last jump: 1 done, -1 no such task, -2 release not supported */
static _Atomic int hooked;            /* the frame hook has run (this game has jumps) */

void runtime_checkpoint_request(uint32_t task) { atomic_store(&requested,task); }
int runtime_checkpoint_practice(void) { return atomic_load(&practice); }
void runtime_checkpoint_set_practice(int on) { atomic_store(&practice,on); }
int runtime_checkpoint_result(void) { return atomic_exchange(&result,0); }
int runtime_checkpoint_available(void) { return atomic_load(&hooked); }

typedef void *(__attribute__((sysv_abi)) *FindTaskById)(void *mgr, uint32_t id);
typedef void (__attribute__((sysv_abi)) *PlayTask)(void *task, int mode, int *fade_frames, int flag);

int bbport_saves_start_practice(void); /* gpu/shim/bbport_saves.cpp: the practice copy, then practice mode on */

/* BB_CHECKPOINT_TEST=<task id hex>@<seconds> (tests): one jump that many seconds after the first frame
 * this hook saw, as if picked in the overlay (practice mode too: the campaign save is not touched). */
static void test_request(void) {
    static int state;  /* 0 unread, 1 waiting, 2 done or off */
    static uint32_t task;
    static double due;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    const double t=now.tv_sec+now.tv_nsec*1e-9;
    if (state==0) {
        const char *e=getenv("BB_CHECKPOINT_TEST");
        unsigned seconds=60;
        state=e && sscanf(e,"%x@%u",&task,&seconds)>=1 && task ? 1 : 2;
        due=t+seconds;
        if (state==1) printf("Checkpoints: test jump to task %08x in %u s (BB_CHECKPOINT_TEST)\n",task,seconds);
    }
    if (state==1 && t>=due) {
        state=2;
        if (bbport_saves_start_practice()) atomic_store(&requested,task);
        else printf("Checkpoints: test jump not made (practice mode could not start)\n");
    }
}

static void frame(const Build *b, uint64_t image) {
    atomic_store(&hooked,1);
    test_request();
    const uint32_t task=atomic_exchange(&requested,0);
    if (!task) return;
    unsigned char *base=(unsigned char *)(uintptr_t)image;
    if (memcmp(base+b->play_task,play_task_code,sizeof(play_task_code)) ||
        memcmp(base+b->find_task,b->find_code,b->find_size)) {
        printf("Checkpoints: %s: the task functions are not where expected; no jump\n",b->name);
        atomic_store(&result,-2);
        return;
    }
    void *t=((FindTaskById)(uintptr_t)(image+b->find_task))(base+b->task_mgr,task);
    if (!t) {
        printf("Checkpoints: task %08x not found\n",task);
        atomic_store(&result,-1);
        return;
    }
    int fade=0;
    printf("Checkpoints: jumping to task %08x (%s)\n",task,b->name);
    fflush(stdout);
    ((PlayTask)(uintptr_t)(image+b->play_task))(t,2,&fade,1);
    atomic_store(&result,1);
}

void __attribute__((ms_abi)) runtime_checkpoint_frame_eu(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                        const uint64_t *regs) {
    (void)rdi; (void)rsi; (void)r14; (void)regs;
    frame(&builds[0],image);
}
void __attribute__((ms_abi)) runtime_checkpoint_frame_us(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                        const uint64_t *regs) {
    (void)rdi; (void)rsi; (void)r14; (void)regs;
    frame(&builds[1],image);
}

/* Uncharted 1 and 2: the same Dev Menu > Tasks item, but there the jump is the menu's whole sequence (its item
 * OnExecute: U1 EU 0x738ee0, U2 EU 0xa6cac0): the task found by its hash in the game's task map; when it is
 * already the active task only ResetTasks; otherwise StartAtTaskIndex(game flow, the task's index, 1, 0) and,
 * when that returns true, ResetTasks(1) and the PlayTask wrapper. When StartAtTaskIndex returns false a level
 * load has started and the menu does nothing more (and neither does the port). U2 first follows the task's
 * redirect (a group to its first playable task). U2 1.02 has no task-by-hash function: its map is walked here
 * (FNV-1a of the hash, buckets of nodes {next, ..., hash +0x10, task +0x18}, bucket[count] = end).
 * Every function the sequence calls is checked against its first bytes first. */
typedef struct { uint64_t at; const char *pattern; } Check;
typedef struct {
    const char *name;
    uint64_t find, map_buckets, map_count;  /* find 0: walk the map */
    uint64_t redirect, is_active;            /* 0: none (U1: the task's state word == 1) */
    uint64_t task_index, start, game_flow, reset, wrap;
    int reset_active;                        /* ResetTasks argument in the "already active" branch */
    Check checks[8];
} OldBuild;

#define FIND_BY_HASH "55 48 89 e5 53 48 83 ec 18 48 8b 1d ?? ?? ?? ?? 48 8b 03 48 89 45 f0 89 7d ec 48 8d 7d ec be 04 00 00 00 ba c5 9d 1c 81 e8 ?? ?? ?? ?? 48 63 0d ?? ?? ?? ?? 31 d2 f7 f1"
#define START_AT "55 48 89 e5 41 57 41 56 41 55 41 54 53 50 41 89 cf 41 89 d6 49 89 fc 85"
#define U1_TASK_INDEX "55 48 89 e5 41 56 53 48 81 e4 e0 ff ff ff 48 81 ec 60 01 00 00 48 89 fb 4c 8b 35"
#define U1_RESET "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28 41 89 fd 48 8b 05"
#define U1_WRAP "55 48 89 e5 53 48 83 ec 18 48 8b 1d ?? ?? ?? ?? 48 8b 03 48 89 45 f0 c7 45 e8 00 00 00 00 48 8d 75 e8 31 d2 e8"
#define U2_WRAP "55 48 89 e5 53 48 83 ec 18 48 8b 1d ?? ?? ?? ?? 48 8b 03 48 89 45 f0 c7 45 e8 00 00 00 00 48 8d 55 e8 be 01 00 00 00 b9 01 00 00 00 e8"
#define U2_REDIRECT "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28 48 8b 0d ?? ?? ?? ?? 48 8b 01 48 89 45 d0 31"
static const OldBuild old_builds[]={
    {"Uncharted 1 EU",0x322300,0,0,0,0,0x31df00,0x14abf0,0xabb148,0x3203e0,0x320f80,0,
     {{0x322300,FIND_BY_HASH},{0x31df00,U1_TASK_INDEX},{0x14abf0,START_AT},{0x3203e0,U1_RESET},{0x320f80,U1_WRAP}}},
    {"Uncharted 1 1.02",0x3229c0,0,0,0,0,0x31e5c0,0x14b0b0,0xa7f048,0x320aa0,0x321640,0,
     {{0x3229c0,FIND_BY_HASH},{0x31e5c0,U1_TASK_INDEX},{0x14b0b0,START_AT},{0x320aa0,U1_RESET},{0x321640,U1_WRAP}}},
    {"Uncharted 2 EU",0x4362b0,0,0,0x435830,0x43a060,0x435980,0x2a1f90,0xf2e350,0x437830,0x438fb0,1,
     {{0x4362b0,FIND_BY_HASH},{0x435830,U2_REDIRECT},
      {0x43a060,"55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28 4c 8b 3d ?? ?? ?? ?? 49 8b 07 48 89 45 d0 48"},
      {0x435980,"55 48 89 e5 41 57 41 56 53 48 81 e4 e0 ff ff ff 48 81 ec 60 01 00 00 48"},{0x2a1f90,START_AT},
      {0x437830,"55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 41 89 fd 48 8b 05"},{0x438fb0,U2_WRAP}}},
    {"Uncharted 2 1.02",0,0x153c000,0x153c03c,0xa58240,0xa5d6e0,0xa583d0,0x5cc7f0,0x1136640,0xa5a270,0xa5bcd0,1,
     {{0xa58240,U2_REDIRECT},{0xa5d6e0,"55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28 48 8b 15"},
      {0xa583d0,"55 48 89 e5 41 56 53 48 81 e4 e0 ff ff ff 48 81 ec 60 01 00 00 48 89 fb"},
      {0x5cc7f0,"55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 e4 e0 ff ff ff 48 81 ec 80 01 00 00 41 89 cf 41 89"},
      {0xa5a270,"55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec a8 00 00 00 89 fb 48 89"},{0xa5bcd0,U2_WRAP}}},
};

/* "55 48 ?? ..." against the code at `code` (?? matches any byte). */
static int matches(const unsigned char *code, const char *pattern) {
    for (const char *p=pattern; *p;) {
        while (*p==' ') ++p;
        if (!*p) break;
        if (p[0]!='?') {
            unsigned value;
            if (sscanf(p,"%2x",&value)!=1 || *code!=(unsigned char)value) return 0;
        }
        ++code;
        p+=2;
    }
    return 1;
}

typedef void *(__attribute__((sysv_abi)) *TaskByHash)(uint32_t hash);
typedef void *(__attribute__((sysv_abi)) *TaskFn)(void *task);
typedef _Bool (__attribute__((sysv_abi)) *TaskTest)(void *task);
typedef int (__attribute__((sysv_abi)) *TaskIndex)(void *task);
typedef _Bool (__attribute__((sysv_abi)) *StartAtTaskIndex)(void *game_flow, int index, int a, int b);
typedef void (__attribute__((sysv_abi)) *ResetTasks)(int flag);
typedef void (__attribute__((sysv_abi)) *PlayTaskWrap)(void *task);

static void *walk_task_map(unsigned char *base, const OldBuild *b, uint32_t hash) {
    uint32_t h=0x811c9dc5u;
    for (int i=0;i<4;++i) h=(h^((hash>>(8*i))&0xff))*0x01000193u;
    const int count=*(const int *)(base+b->map_count);
    void **buckets=*(void ***)(base+b->map_buckets);
    if (!buckets || count<=0) return NULL;
    for (void **node=(void **)buckets[h%(uint32_t)count]; node; node=(void **)*node)
        if (*(const uint32_t *)((unsigned char *)node+0x10)==hash)
            return (void *)node==buckets[count] ? NULL : *(void **)((unsigned char *)node+0x18);
    return NULL;
}

static void old_frame(const OldBuild *b, uint64_t image) {
    atomic_store(&hooked,1);
    test_request();
    const uint32_t task=atomic_exchange(&requested,0);
    if (!task) return;
    unsigned char *base=(unsigned char *)(uintptr_t)image;
    for (size_t i=0;i<sizeof(b->checks)/sizeof(b->checks[0]) && b->checks[i].pattern;++i)
        if (!matches(base+b->checks[i].at,b->checks[i].pattern)) {
            printf("Checkpoints: %s: the task functions are not where expected (%#" PRIx64 "); no jump\n",b->name,
                   b->checks[i].at);
            atomic_store(&result,-2);
            return;
        }
    void *t=b->find ? ((TaskByHash)(uintptr_t)(image+b->find))(task) : walk_task_map(base,b,task);
    if (!t) {
        printf("Checkpoints: task %08x not found\n",task);
        atomic_store(&result,-1);
        return;
    }
    if (b->redirect) {
        void *r=((TaskFn)(uintptr_t)(image+b->redirect))(t);
        if (r) t=r;
    }
    const int active=b->is_active ? ((TaskTest)(uintptr_t)(image+b->is_active))(t)
                                  : **(int *const *)((unsigned char *)t+0x40)==1;
    printf("Checkpoints: jumping to task %08x (%s%s)\n",task,b->name,active ? ", already the active task: reset" : "");
    fflush(stdout);
    if (active) {
        ((ResetTasks)(uintptr_t)(image+b->reset))(b->reset_active);
    } else {
        const int index=((TaskIndex)(uintptr_t)(image+b->task_index))(t);
        if (((StartAtTaskIndex)(uintptr_t)(image+b->start))(base+b->game_flow,index,1,0)) {
            ((ResetTasks)(uintptr_t)(image+b->reset))(1);
            ((PlayTaskWrap)(uintptr_t)(image+b->wrap))(t);
        } else {
            printf("Checkpoints: task %08x: the game loads its level first (index %d)\n",task,index);
        }
    }
    atomic_store(&result,1);
}

#define OLD_FRAME(fn, i) \
    void __attribute__((ms_abi)) fn(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14, const uint64_t *regs) { \
        (void)rdi; (void)rsi; (void)r14; (void)regs; old_frame(&old_builds[i],image); }
OLD_FRAME(runtime_checkpoint_frame_u1_eu,0)
OLD_FRAME(runtime_checkpoint_frame_u1_us,1)
OLD_FRAME(runtime_checkpoint_frame_u2_eu,2)
OLD_FRAME(runtime_checkpoint_frame_u2_us,3)
