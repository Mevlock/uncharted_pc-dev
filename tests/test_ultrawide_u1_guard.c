/* U1 ultrawide camera guard, using synthetic cameras and native executable stubs.
 * Windows x64 with AVX; skips elsewhere. No game files or GPU initialization.
 * Build and run from the repository in the MSYS2 CLANG64 shell:
 *   clang -std=c11 -O2 -Wall -Wextra -Werror tests/test_ultrawide_u1_guard.c -o out/ultrawide-u1-test.exe
 *   out/ultrawide-u1-test.exe
 */
#include <stdio.h>
#if !defined(_WIN32) || !defined(__x86_64__)
int main(void) { puts("SKIP (Windows x64 only)"); return 0; }
#else
#include <cpuid.h>
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include "../src/runtime_ultrawide.c"

typedef struct {
    uint64_t before[16], after[16], flags_before, flags_after;
    uint32_t mxcsr_before, mxcsr_after;
    uint8_t ymm_before[16][32], ymm_after[16][32];
    uint8_t redzone_before[128], redzone_after[128];
} State;
_Static_assert(offsetof(State, ymm_before) == 280, "assembly layout");
_Static_assert(offsetof(State, ymm_after) == 792, "assembly layout");
_Static_assert(offsetof(State, redzone_before) == 1304, "assembly layout");
_Static_assert(offsetof(State, redzone_after) == 1432, "assembly layout");
void run_guard_state(State *, void *, void *, uint32_t);
/* MS ABI wrapper: snapshot CPU state around the emitted guard, including the
 * 128 bytes below its entry stack pointer. The C caller's state is restored. */
__asm__(".intel_syntax noprefix\n"
        ".text\n"
        ".globl run_guard_state\n"
        "run_guard_state:\n"
        "    push rbx\n"
        "    push rbp\n"
        "    push rdi\n"
        "    push rsi\n"
        "    push r12\n"
        "    push r13\n"
        "    push r14\n"
        "    push r15\n"
        "    sub rsp, 0x248\n"
        "    .irp r,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n"
        "    vmovups [rsp + 32*\\r], ymm\\r\n"
        "    .endr\n"
        "    mov [rsp+0x200], rcx\n"
        "    mov [rsp+0x208], rdx\n"
        "    mov [rsp+0x210], r8\n"
        "    stmxcsr [rsp+0x218]\n"
        "    mov [rsp+0x220], r9d\n"
        "    mov rax, rcx\n"
        "    .irp off,0,32,64,96\n"
        "    vmovups ymm0, [rax + 1304 + \\off]\n"
        "    vmovups [rsp - 136 + \\off], ymm0\n"
        "    .endr\n"
        "    .irp r,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n"
        "    vmovups ymm\\r, [rax + 280 + 32*\\r]\n"
        "    .endr\n"
        "    ldmxcsr [rsp+0x220]\n"
        "    mov rbx, [rsp+0x210]\n"
        "    mov rsi, rbx\n"
        "    mov rdi, rbx\n"
        "    mov r12, rbx\n"
        "    mov r15, rbx\n"
        "    mov rcx, 0x1122334455667788\n"
        "    mov rdx, 0x2233445566778899\n"
        "    mov rbp, 0x33445566778899aa\n"
        "    mov r8,  0x445566778899aabb\n"
        "    mov r9,  0x5566778899aabbcc\n"
        "    mov r10, 0x66778899aabbccdd\n"
        "    mov r11, 0x778899aabbccddee\n"
        "    mov r13, 0x8899aabbccddeeff\n"
        "    mov r14, 0x99aabbccddeeff00\n"
        "    push 0x247\n"
        "    popfq\n"
        "    mov [rax+0], rax\n"
        "    mov [rax+8], rcx\n"
        "    mov [rax+16], rdx\n"
        "    mov [rax+24], rbx\n"
        "    mov [rax+32], rsp\n"
        "    mov [rax+40], rbp\n"
        "    mov [rax+48], rsi\n"
        "    mov [rax+56], rdi\n"
        "    mov [rax+64], r8\n"
        "    mov [rax+72], r9\n"
        "    mov [rax+80], r10\n"
        "    mov [rax+88], r11\n"
        "    mov [rax+96], r12\n"
        "    mov [rax+104], r13\n"
        "    mov [rax+112], r14\n"
        "    mov [rax+120], r15\n"
        "    pushfq\n"
        "    pop qword ptr [rax+256]\n"
        "    stmxcsr [rax+272]\n"
        "    call qword ptr [rsp+0x208]\n"
        "    mov [rax+128], rax\n"
        "    mov [rax+136], rcx\n"
        "    mov [rax+144], rdx\n"
        "    mov [rax+152], rbx\n"
        "    mov [rax+160], rsp\n"
        "    mov [rax+168], rbp\n"
        "    mov [rax+176], rsi\n"
        "    mov [rax+184], rdi\n"
        "    mov [rax+192], r8\n"
        "    mov [rax+200], r9\n"
        "    mov [rax+208], r10\n"
        "    mov [rax+216], r11\n"
        "    mov [rax+224], r12\n"
        "    mov [rax+232], r13\n"
        "    mov [rax+240], r14\n"
        "    mov [rax+248], r15\n"
        "    pushfq\n"
        "    pop qword ptr [rax+264]\n"
        "    stmxcsr [rax+276]\n"
        "    .irp r,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n"
        "    vmovups [rax + 792 + 32*\\r], ymm\\r\n"
        "    .endr\n"
        "    .irp off,0,32,64,96\n"
        "    vmovups ymm0, [rsp - 136 + \\off]\n"
        "    vmovups [rax + 1432 + \\off], ymm0\n"
        "    .endr\n"
        "    ldmxcsr [rsp+0x218]\n"
        "    .irp r,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n"
        "    vmovups ymm\\r, [rsp + 32*\\r]\n"
        "    .endr\n"
        "    add rsp, 0x248\n"
        "    pop r15\n"
        "    pop r14\n"
        "    pop r13\n"
        "    pop r12\n"
        "    pop rsi\n"
        "    pop rdi\n"
        "    pop rbp\n"
        "    pop rbx\n"
        "    ret\n"
        ".att_syntax prefix\n");

static unsigned tested, largest;
static uint8_t *memory;
static void basis(uint8_t *camera, const float r[3], const float u[3], const float f[3], float aspect) {
    memset(camera, 0, 0x300);
    memcpy(camera + 0x250, r, 12);
    memcpy(camera + 0x260, u, 12);
    memcpy(camera + 0x270, f, 12);
    memcpy(camera + 0x290, &aspect, 4);
    /* A non-90-degree light: zoom deliberately differs from 1. */
    const float zoom = 2.3f;
    memcpy(camera + 0x294, &zoom, 4);
}

static void check(const char *name, uint8_t *camera, unsigned base, unsigned dest, int widen) {
    memset(memory, 0xcc, 4096);
    uint8_t *end = emit_u1_camera_guard(memory, memory + 0x300, (uint8_t)base,
                                       0x290, 0x3f7eb852, 0x3f80a3d7);
    unsigned used = (unsigned)(end - memory);
    if (used > largest) largest = used;
    assert(used + 10 + 13 <= 0x1c0); /* longest original instruction plus vmulss/jmp */
    /* A legacy scalar multiply lets this harness also verify all YMM upper bits. */
    *end++ = 0xf3;
    if (dest >= 8) *end++ = 0x44;
    *end++ = 0x0f; *end++ = 0x59; *end++ = (uint8_t)(0x05 | ((dest & 7) << 3));
    int32_t rel = (int32_t)((memory + 0x380) - (end + 4));
    memcpy(end, &rel, 4); end += 4;
    *end = 0xc3;
    memory[0x300] = 0xc3;
    const float multiplier = 0.5f;
    memcpy(memory + 0x380, &multiplier, 4);
    assert(FlushInstructionCache(GetCurrentProcess(), memory, 4096));
    State state = {0};
    for (unsigned r = 0; r < 16; ++r) {
        for (unsigned lane = 0; lane < 8; ++lane) {
            float value = (float)(r * 8 + lane + 2);
            memcpy(state.ymm_before[r] + lane * 4, &value, 4);
        }
    }
    for (unsigned i = 0; i < 128; ++i) state.redzone_before[i] = (uint8_t)(i ^ 0xa5);
    run_guard_state(&state, memory, camera, 0x5f95);
    uint8_t expected[16][32];
    memcpy(expected, state.ymm_before, sizeof(expected));
    float value;
    memcpy(&value, expected[dest], 4);
    if (widen) value *= 0.5f;
    memcpy(expected[dest], &value, 4);
    if (memcmp(state.before, state.after, sizeof(state.before)) ||
        state.flags_before != state.flags_after || state.mxcsr_before != state.mxcsr_after ||
        memcmp(expected, state.ymm_after, sizeof(expected)) ||
        memcmp(state.redzone_before, state.redzone_after, 128)) {
        fprintf(stderr, "FAIL %s base=%u dest=%u widen=%d gpr=%d flags=%llx/%llx mxcsr=%x/%x ymm=%d redzone=%d\n",
                name, base, dest, widen, memcmp(state.before, state.after, sizeof(state.before)),
                (unsigned long long)state.flags_before, (unsigned long long)state.flags_after,
                state.mxcsr_before, state.mxcsr_after,
                memcmp(expected, state.ymm_after, sizeof(expected)),
                memcmp(state.redzone_before, state.redzone_after, 128));
        exit(1);
    }
    ++tested;
}

static void check_all(const char *name, uint8_t *camera, int widen) {
    const unsigned bases[] = {3, 7, 12}; /* RBX, RDI, and the R12 SIB encoding */
    const unsigned destinations[] = {0, 1, 2, 8}; /* scratch overlaps and a high XMM register */
    for (unsigned b = 0; b < sizeof(bases) / sizeof(bases[0]); ++b)
        for (unsigned d = 0; d < sizeof(destinations) / sizeof(destinations[0]); ++d)
            check(name, camera, bases[b], destinations[d], widen);
}

int main(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 28)) || !(c & (1u << 27))) {
        puts("SKIP (AVX and OSXSAVE required)"); return 0;
    }
    __asm__("xgetbv" : "=a"(a), "=d"(d) : "c"(0));
    if ((a & 6) != 6) { puts("SKIP (OS has not enabled AVX state)"); return 0; }
    memory = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(memory);
    uint8_t camera[0x300];
    const float r[3] = {1, 0, 0}, u[3] = {0, 1, 0}, f[3] = {0, 0, 1};
    basis(camera, r, u, f, 1.0f); check_all("square light", camera, 0);
    basis(camera, r, u, f, 0.995f); check_all("inclusive low", camera, 0);
    basis(camera, r, u, f, 1.005f); check_all("inclusive high", camera, 0);
    basis(camera, r, u, f, nextafterf(0.995f, 0)); check_all("outside low", camera, 1);
    basis(camera, r, u, f, nextafterf(1.005f, 2)); check_all("outside high", camera, 1);
    basis(camera, r, u, f, 0.5625f); check_all("main camera", camera, 1);
    basis(camera, r, u, f, NAN); check_all("invalid aspect", camera, 1);
    const float bad[][3] = {{0,0,0}, {-1,0,0}, {NAN,0,0}, {INFINITY,0,0}, {-INFINITY,0,0}};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        basis(camera, bad[i], u, f, 1.0f); check_all("invalid or mirrored", camera, 1);
        const float cube_zoom = 1.0f;
        memcpy(camera + 0x294, &cube_zoom, 4);
        check_all("cube unknown preserved, finite reflection widened", camera, i == 1);
    }
    const float zero[3] = {0,0,0};
    const float zooms[] = {0.995f, 1.005f, 0.99f, 1.01f, NAN, INFINITY};
    for (unsigned i = 0; i < sizeof(zooms)/sizeof(zooms[0]); ++i) {
        basis(camera, zero, u, f, 1.0f);
        memcpy(camera + 0x294, &zooms[i], 4);
        check_all("unknown zoom guard boundaries", camera, i >= 2);
    }
    /* A non-axis-aligned proper rotation and its Householder reflection. */
    {
        const float a = 0.371f, b = 0.229f;
        float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
        float rr[3] = {ca*cb, sa*cb, -sb};
        float uu[3] = {-sa, ca, 0};
        float ff[3] = {ca*sb, sa*sb, cb};
        basis(camera, rr, uu, ff, 1.0f); check_all("rotated light", camera, 0);
        float normal[3] = {cosf(a*0.73f)*sinf(b*0.61f), sinf(a*0.73f)*sinf(b*0.61f), cosf(b*0.61f)};
        float *vectors[] = {rr, uu, ff};
        for (int v = 0; v < 3; ++v) {
            float dot = 0;
            for (int axis = 0; axis < 3; ++axis) dot += vectors[v][axis]*normal[axis];
            for (int axis = 0; axis < 3; ++axis) vectors[v][axis] -= 2*dot*normal[axis];
        }
        basis(camera, rr, uu, ff, 1.0f); check_all("reflected view", camera, 1);
    }
    printf("PASS: %u executable guard cases; largest guard %u bytes; all 16 GPRs, XMM/YMM registers, flags, MXCSR and 128-byte red zone preserved.\n", tested, largest);
    VirtualFree(memory, 0, MEM_RELEASE);
    return 0;
}

#endif
