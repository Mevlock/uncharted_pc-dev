#ifndef BB_RUNTIME_H
#define BB_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#include "compat_win.h"
/* No signals on Windows: the exception handler resumes the thread in bb_longjmp
 * (gpu/shim/bbport_toggles.h uses the same buffer type). */
typedef bb_jmp_buf sigjmp_buf;
#define sigsetjmp(buffer, save) bb_setjmp(buffer)
#else
#include <setjmp.h>
#endif
/* Recovery point for speculative guest memory reads on this thread (probe.c fault handler). */
extern __thread sigjmp_buf *runtime_fault_recover;
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
/* Ends the process: log flushed, shader cache saved, no destructors and (Windows) no DLL detach
 * code (TerminateProcess; ExitProcess could hang in libsrt's detach, and exit()'s destructors
 * waited for the GPU). Every exit of the game and every STOP go through it. */
__attribute__((noreturn)) void runtime_quit(int code);
/* Fault handler: a fault inside a partial remap in progress waits for it; returns 1 to retry. */
int runtime_memory_wait_carve(uintptr_t address);
const char *runtime_import_name(const char *name);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
void **runtime_application_heap_api(void);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
int32_t *runtime_errno(void);
uintptr_t runtime_sema_resolve(const char *name);
void runtime_sema_report(void);
unsigned runtime_sema_waiters(uint32_t id);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
void runtime_file_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
int runtime_file_translate(const char *guest, char *out, size_t size);
int runtime_file_replace(const char *source, const char *target);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* The PS4's floating-point state for guest code on this thread (MXCSR, x87 control word). */
void runtime_set_guest_fpu(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
void *runtime_image_map(size_t size, int prot);
/* The loaded game's image (guest offset 0), or 0 before it is mapped. */
uintptr_t runtime_image_base(void);
/* Its size in bytes (0 before it is mapped). */
size_t runtime_image_size(void);
/* 1 once the image is loaded, patched and protected (its code readable). */
int runtime_image_ready(void);
/* Two bytes of the game's code swapped in one locked write (expect -> value); 0 done. */
int runtime_code_swap16(void *at, uint16_t expect, uint16_t value);
/* Developer mode's debug text: 1 shown, 0 hidden, -1 no developer mode (runtime_pad.c). */
int runtime_dev_text_shown(void);
/* Windows: reserves the PS4 user address range as a placeholder (no-op elsewhere). */
void runtime_memory_reserve(void);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);
int runtime_pad_tool(int test, int strength);
uintptr_t runtime_rtc_resolve(const char *name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *title);
uintptr_t runtime_savedata_resolve(const char *name);
/* Trophies (runtime_trophy.c): the folder run.py prepared for the running game (scripts/trophies.py). */
void runtime_trophy_configure(const char *dir);
uintptr_t runtime_trophy_resolve(const char *name);
void runtime_savedata_report(void);
int runtime_savedata_mounted(void);
/* "Quit" in the games' main menus (runtime_exit_menu.c). */
void __attribute__((ms_abi)) runtime_exit_menu_u1_eu_add(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                         const uint64_t *regs);
void __attribute__((ms_abi)) runtime_exit_menu_u1_us_add(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                         const uint64_t *regs);
void __attribute__((ms_abi)) runtime_exit_menu_u1_eu_handler(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                             const uint64_t *regs);
void __attribute__((ms_abi)) runtime_exit_menu_u1_us_handler(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                             const uint64_t *regs);
void runtime_menumouse_u2_host(uint64_t host);   /* runtime_menumouse.c: U2's menu host, from the menu builder */
/* The "Keyboard / Mouse" row of Options > Controls (antikmewzik's; runtime_exit_menu.c adds it, runtime_menumouse.c gives it its
 * label): on unless BB_KBM_ROW=0. */
int runtime_kbm_row_on(void);
/* The row's label in the game's language (BB_LANGUAGE) that fits `room` bytes, written over `host` (the game text it borrows:
 * its current text, to tell whether the bank has the label's letters); NULL: none fits. */
const char *runtime_kbm_label(const char *host, size_t room);
/* U2/U3: the text lookup hook that gives the row its label is installed (the row is added only then). */
int runtime_menumouse_kbm_label_hook(void);
/* Writes guest memory that may not be writable (a game text): 0 when it could not. */
int runtime_menumouse_write(uint64_t dst, const void *src, size_t size);
/* The row was picked: the overlay's Controls page opens (gpu/shim/bbport_overlay.cpp). */
void bbgpu_open_controls(void);
void __attribute__((ms_abi)) runtime_exit_menu_u2_data(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
    const uint64_t *regs);
void __attribute__((ms_abi)) runtime_exit_menu_u3_data(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
    const uint64_t *regs);
void __attribute__((ms_abi)) runtime_exit_menu_u2_handler(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
    const uint64_t *regs);
void __attribute__((ms_abi)) runtime_exit_menu_u3_handler(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
    const uint64_t *regs);
/* Checkpoint jumps (runtime_checkpoint.c): the overlay asks for a task, the frame hook runs the game's own jump;
 * practice mode sends the campaign save to a practice copy until the game restarts. */
void runtime_checkpoint_request(uint32_t task);
int runtime_checkpoint_practice(void);
void runtime_checkpoint_set_practice(int on);
int runtime_checkpoint_result(void);
int runtime_checkpoint_available(void);
void __attribute__((ms_abi)) runtime_checkpoint_frame_eu(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                        const uint64_t *regs);
void __attribute__((ms_abi)) runtime_checkpoint_frame_us(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                        const uint64_t *regs);
void __attribute__((ms_abi)) runtime_checkpoint_frame_u1_eu(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                           const uint64_t *regs);
void __attribute__((ms_abi)) runtime_checkpoint_frame_u1_us(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                           const uint64_t *regs);
void __attribute__((ms_abi)) runtime_checkpoint_frame_u2_eu(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                           const uint64_t *regs);
void __attribute__((ms_abi)) runtime_checkpoint_frame_u2_us(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                           const uint64_t *regs);
/* runtime_onehit.c: Uncharted 2 Npc::ComputeArmorDamage (One Hit Kills; BB_ONEHIT_LOG=1 prints each attack). */
void __attribute__((ms_abi)) runtime_u2_npc_damage(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                          const uint64_t *regs);
void __attribute__((ms_abi)) runtime_u1_jetski_skill(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                     const uint64_t *regs);
void __attribute__((ms_abi)) runtime_u1_respond_to_attack(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                          const uint64_t *regs);
int runtime_savedata_pick(int32_t user, const char *title, const char (*names)[32], uint32_t count,
                          int which, const char *focus, char out[32], void *param_out);
/* The save list (runtime_services.c): a save as the game would mount it exists; its picture (thumb.png, else
 * icon0.png; -1 none); its label (title, subtitle, detail, date); moved to <user dir>/savedata-deleted (0 moved). */
int runtime_savedata_exists(int32_t user, const char *title, const char *dir);
int runtime_savedata_icon(int32_t user, const char *title, const char *dir, char *out, size_t size);
void runtime_savedata_label(int32_t user, const char *title, const char *dir, char *out, size_t size);
int runtime_savedata_trash(int32_t user, const char *title, const char *dir);
void runtime_thread_attach_host(const char *name);
uintptr_t runtime_fiber_resolve(const char *name);
void runtime_fiber_report(void);
/* For fault reports (no allocation, any thread): the guest thread and fiber running here, or NULL. */
const char *runtime_thread_name_here(void);
const char *runtime_fiber_name_here(void);
/* Host functions the loader runs at the start of matching guest functions (probe.c): the code
 * at `offset` must equal `signature`; `moved` bytes of whole position-independent instructions
 * move to the thunk. The callback gets the guest's rdi, rsi and r14 there and the image base.
 * Without a callback, `patch` replaces the first `moved` bytes instead (a fix in the game's code). */
typedef struct {
    const char *name;
    uint64_t offset;
    const unsigned char *signature;
    size_t signature_size, moved;
    /* regs: the registers the hook saved (rbx r11 r10 r9 r8 rdx rcx rax rflags, then at a
     * function's start the return address) */
    void (__attribute__((ms_abi)) *callback)(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                             const uint64_t *regs);
    const unsigned char *patch;
} RuntimeGuestCallback;
const RuntimeGuestCallback *runtime_guest_callbacks(size_t *count);
/* BB_PAD_TIMING=1 (diagnosis): blocking waits noted per thread (name, start, duration); the pad
 * read prints the ones since the last read. runtime_wait_begin returns 0 when off. */
double runtime_wait_begin(void);
void runtime_wait_end(const char *what, double start);
/* Shader preparation page (on by default; BB_PREP_FREEZE=0: off, the game runs behind the page): while it shows, the game's threads stop at their next wait,
 * lock, yield or sound output (runtime_freeze_point; cooperative: never inside a host lock) and all go on together
 * when it closes, so the game starts afterwards as if the page took no time. runtime_freeze_guest: the page's switch. */
extern volatile int runtime_freeze_flag;
void runtime_freeze_wait(void);
static inline void runtime_freeze_point(void) {
    if (__builtin_expect(runtime_freeze_flag, 0)) runtime_freeze_wait();
}
void runtime_freeze_guest(int on);
void runtime_wait_print(double now);
void runtime_wait_caller(uint64_t caller, uint64_t arg);
#endif
