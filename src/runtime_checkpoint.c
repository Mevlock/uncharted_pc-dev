/* Checkpoint jumps (the overlay's Checkpoints page): the game's own task jump, PlayTask, as Naughty Dog's
 * Dev Menu > Tasks calls it, run at the start of the menu manager's per-frame update (the frame point where
 * the Dev Menu runs it: the game-frame update, GameFramePreRenderUpdate). A jump the overlay asks for waits
 * there for the next frame.
 *
 * Uncharted 3 only for now: EU (1.00 code) and US (1.02 code: US CUSA02320, EU/Russian CUSA02344, EU 1.02).
 * Each function is checked against its first bytes before it is called; a release where they differ is
 * "not supported" (no call).
 *
 * Practice mode: from the first jump until the game restarts, the game's campaign save (SAVEDATA0<game>0)
 * mounts from <saves>.backups/u<game>/practice instead (runtime_savedata.c), a copy of the campaign save made
 * before the jump, so the jumps and everything played after them never reach the player's own save. */
#include "runtime.h"
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
