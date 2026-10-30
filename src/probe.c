/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <cpuid.h>
#include "runtime.h"
#include "gpu/bbgpu.h"
/* BB_TIME_SCALE: rdtsc instructions of the game patched to ud2 (patch_rdtsc). */
static int is_patched_rdtsc(uintptr_t rip);
uint64_t runtime_guest_tsc(void);
#ifdef _WIN32
#include "red_zone.h"
#endif
#include "cpu_check.h"
#include "isa_rewrite.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
static size_t image_size;
uintptr_t runtime_image_base(void) { return (uintptr_t)image; }
size_t runtime_image_size(void) { return image_size; }
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
/* Load-time rewrite of the instructions the host CPU lacks (check_cpu, isa_rewrite.c). */
static IsaTable *isa_table;
static unsigned isa_sets;
int vulkan_smoke(void);

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
#ifdef _WIN32
/* BB_WATCH=offset:type,... (diagnosis): guest image variables printed every 5 seconds.
 * Types: f float, d double, b byte, i int32, q int64. */
/* BB_POKE=offset:type=value,... (experiments): written before each print. */
static void apply_pokes(void) {
    const char *p=getenv("BB_POKE");
    while (p && *p) {
        char *end; uint64_t offset=strtoull(p,&end,16);
        char type=*end==':' ? end[1] : 'f';
        const char *value=strchr(end,'=');
        if (!value) break;
        unsigned char *at=image+offset;
        if (type=='d') *(double *)at=strtod(value+1,NULL);
        else if (type=='b') *at=(unsigned char)strtoul(value+1,NULL,0);
        else if (type=='i') *(int32_t *)at=(int32_t)strtol(value+1,NULL,0);
        else *(float *)at=strtof(value+1,NULL);
        p=strchr(value,',');
        if (p) ++p;
    }
}
static DWORD WINAPI watch_thread(void *argument) {
    const char *list=argument;
    for (;;) {
        Sleep(5000);
        apply_pokes();
        char line[1024]; size_t used=0;
        for (const char *p=list; *p && used<sizeof(line)-64; ) {
            char *end; uint64_t offset=strtoull(p,&end,16);
            char type=*end==':' ? end[1] : 'f';
            const unsigned char *at=image+offset;
            if (type=='d') used+=snprintf(line+used,sizeof(line)-used," %llx=%g",(unsigned long long)offset,*(const double *)at);
            else if (type=='b') used+=snprintf(line+used,sizeof(line)-used," %llx=%u",(unsigned long long)offset,*at);
            else if (type=='i') used+=snprintf(line+used,sizeof(line)-used," %llx=%d",(unsigned long long)offset,*(const int32_t *)at);
            else if (type=='q') used+=snprintf(line+used,sizeof(line)-used," %llx=%lld",(unsigned long long)offset,*(const long long *)at);
            else used+=snprintf(line+used,sizeof(line)-used," %llx=%g",(unsigned long long)offset,*(const float *)at);
            p=strchr(end,',');
            if (!p) break;
            ++p;
        }
        printf("Watch:%s\n",line);
        fflush(stdout);
    }
    return 0;
}
#endif
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) return low;
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
#endif
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
#endif
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)image, argument);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)image;
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",caller-modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
#ifndef _WIN32
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack. */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
/* The game is built for the PS4's AMD Jaguar CPU: AVX throughout, and BMI1 (andn, bextr, blsi, blsr,
 * tzcnt), MOVBE, LZCNT, F16C and POPCNT in thousands of places, its libc's string functions among
 * them. AVX, SSE4.2 and POPCNT are required (Intel Sandy Bridge, 2011, or AMD FX and newer). A CPU
 * without BMI1, MOVBE, LZCNT or F16C (Sandy and Ivy Bridge, issues #197 and #515) runs the game with
 * those instructions rewritten at load time (isa_rewrite.c, from the table scripts/isa_rewrite.py
 * made; run.py passes it with --isa-rewrite). Without them the code stopped at the first one with an
 * illegal instruction (0xc000001d in libc.prx while module 0 initialized), except LZCNT and TZCNT,
 * which older CPUs run as BSR and BSF (other results), so they have to be rewritten before the game runs.
 * BB_FORCE_LEGACY_CPU=1 rewrites all of them on any CPU (tests); BB_SKIP_CPU_CHECK=1 starts without
 * any check. Returns the CPU_SET_* mask to rewrite; exit code 24 with a message when AVX, SSE4.2 or
 * POPCNT is missing. */
static unsigned check_cpu(char *names_out, size_t names_size) {
    if (names_size) names_out[0]=0;
    if (getenv("BB_SKIP_CPU_CHECK")) return 0;
    unsigned a=0, b=0, c=0, d=0, basic=0;
    if (__get_cpuid(1,&a,&b,&c,&d)) basic=c;
    int avx=0;
    if (((basic>>27)&1) && ((basic>>28)&1)) { /* OSXSAVE and AVX: the OS also has to save YMM state */
        unsigned low, high;
        __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
        avx=(low&6)==6;
    }
    const struct { int present; const char *name; } features[]={
        {avx,"AVX"}, {(basic>>23)&1,"POPCNT"}, {(basic>>20)&1,"SSE4.2"},
    };
    char missing[64]="";
    for (size_t i=0;i<sizeof(features)/sizeof(*features);++i) {
        if (features[i].present) continue;
        if (*missing) strcat(missing,", ");
        strcat(missing,features[i].name);
    }
    if (*missing) {
        unsigned words[12]={0};
        char brand[49]="";
        if (__get_cpuid_max(0x80000000,NULL)>=0x80000004)
            for (unsigned i=0;i<3;++i) __get_cpuid(0x80000002+i,&words[i*4],&words[i*4+1],&words[i*4+2],&words[i*4+3]);
        memcpy(brand,words,48);
        for (size_t n=strlen(brand); n && brand[n-1]==' '; --n) brand[n-1]=0;
        const char *name=brand;
        while (*name==' ') ++name;
        if (!*name) name="unknown processor";
        fprintf(stderr,"STOP: unsupported CPU %s: missing %s (needs AVX: Intel Core 2nd generation or newer, or AMD FX or Ryzen)\n",name,missing);
#ifdef _WIN32
        char text[512];
        snprintf(text,sizeof(text),"This processor cannot run the game:\n%s\n\nMissing instructions: %s\n\n"
                 "The game needs AVX: an Intel Core of the 2nd generation (Sandy Bridge, 2011) or newer, "
                 "an AMD FX or an AMD Ryzen.",name,missing);
        MessageBoxA(NULL,text,"Uncharted",MB_OK|MB_ICONERROR|MB_SETFOREGROUND);
#endif
        exit(24);
    }
    const CpuFeatureWords none={0,0,0};
    const int forced=getenv("BB_FORCE_LEGACY_CPU") && strcmp(getenv("BB_FORCE_LEGACY_CPU"),"0");
    return cpu_missing_rewritable(forced ? none : cpu_feature_words(),names_out,names_size);
}
#ifdef _WIN32
static void describe_address(char *out, size_t size, uintptr_t address) {
    HMODULE module=NULL;
    char name[MAX_PATH]="?";
    if (image && address-(uintptr_t)image<0x20000000) { snprintf(out,size,"guest offset 0x%" PRIxPTR,address-(uintptr_t)image); return; }
    uint64_t site;
    if (red_zone_site(address,&site)) { snprintf(out,size,"red-zone thunk for guest offset %#" PRIx64,site); return; }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)address,&module) && module) {
        GetModuleFileNameA(module,name,sizeof(name));
        const char *base=strrchr(name,'\\');
        snprintf(out,size,"%s+0x%" PRIxPTR,base ? base+1 : name,address-(uintptr_t)module);
        return;
    }
    snprintf(out,size,"%p",(void *)address);
}
/* For the hitch report (gpu/shim/bbport_sampler.cpp): guest offset, red-zone thunk or module+offset. */
void bb_describe_address(char *out, size_t size, uintptr_t address) { describe_address(out,size,address); }
/* Every thread's stack into the log, once a session: called when the GPU stall report fires (a
 * wait for a tick nobody submits is a deadlock between the port's threads; a player's log then
 * shows which threads wait where). Each thread is suspended only while its registers and frames
 * are collected, without allocating or printing (it may hold the heap or stdout lock); frames
 * with unwind data are unwound by RtlVirtualUnwind, guest code by its frame-pointer chain. */
enum { DUMP_THREADS=192, DUMP_FRAMES=40 };
typedef struct { DWORD tid; char name[64]; int frames; DWORD64 pc[DUMP_FRAMES]; } ThreadStack;
static int dump_readable(DWORD64 address, size_t size) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!address || !VirtualQuery((const void *)(uintptr_t)address,&mbi,sizeof(mbi))) return 0;
    if (mbi.State!=MEM_COMMIT || (mbi.Protect&(PAGE_NOACCESS|PAGE_GUARD))) return 0;
    return address+size<=(DWORD64)(uintptr_t)mbi.BaseAddress+mbi.RegionSize;
}
static void dump_walk(CONTEXT c, ThreadStack *t) {
    t->frames=0;
    while (t->frames<DUMP_FRAMES && c.Rip) {
        t->pc[t->frames++]=c.Rip;
        const int guest=image && c.Rip-(DWORD64)(uintptr_t)image<0x20000000;
        DWORD64 base=0;
        PRUNTIME_FUNCTION function=guest ? NULL : RtlLookupFunctionEntry(c.Rip,&base,NULL);
        if (function) {
            void *handler_data; DWORD64 frame;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,c.Rip,function,&c,&handler_data,&frame,NULL);
        } else if (guest) {
            if (!dump_readable(c.Rbp,16)) break;
            const DWORD64 *fp=(const DWORD64 *)(uintptr_t)c.Rbp;
            if (fp[0]<=c.Rbp) { if (t->frames<DUMP_FRAMES) t->pc[t->frames++]=fp[1]; break; }
            c.Rsp=c.Rbp+16; c.Rip=fp[1]; c.Rbp=fp[0];
        } else {                                       /* a leaf function: return address at rsp */
            if (!dump_readable(c.Rsp,8)) break;
            c.Rip=*(const DWORD64 *)(uintptr_t)c.Rsp; c.Rsp+=8;
        }
    }
}
void runtime_dump_thread_stacks(const char *reason) {
    static LONG done;
    if (InterlockedExchange(&done,1)) return;
    static ThreadStack stacks[DUMP_THREADS];
    int count=0;
    typedef HRESULT (WINAPI *GetDescription)(HANDLE,PWSTR *);
    const GetDescription describe=(GetDescription)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),"GetThreadDescription");
    const DWORD self=GetCurrentThreadId(), pid=GetCurrentProcessId();
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    THREADENTRY32 entry={.dwSize=sizeof(entry)};
    for (BOOL ok=Thread32First(snapshot,&entry); ok && count<DUMP_THREADS; ok=Thread32Next(snapshot,&entry)) {
        if (entry.th32OwnerProcessID!=pid) continue;
        ThreadStack *t=&stacks[count];
        t->tid=entry.th32ThreadID; t->name[0]=0; t->frames=0;
        if (t->tid==self) {
            CONTEXT c; RtlCaptureContext(&c);
            dump_walk(c,t);
            snprintf(t->name,sizeof(t->name),"(reporting thread)");
            ++count;
            continue;
        }
        HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_LIMITED_INFORMATION,FALSE,t->tid);
        if (!thread) continue;
        PWSTR description=NULL;                        /* before suspending: it allocates */
        if (describe && SUCCEEDED(describe(thread,&description)) && description) {
            WideCharToMultiByte(CP_UTF8,0,description,-1,t->name,sizeof(t->name),NULL,NULL);
            LocalFree(description);
        }
        if (SuspendThread(thread)!=(DWORD)-1) {
            CONTEXT c={.ContextFlags=CONTEXT_FULL};
            if (GetThreadContext(thread,&c)) dump_walk(c,t);
            ResumeThread(thread);
        }
        CloseHandle(thread);
        ++count;
    }
    CloseHandle(snapshot);
    printf("Thread stacks (%s): %d threads\n",reason,count);
    for (int i=0;i<count;++i) {
        const ThreadStack *t=&stacks[i];
        printf("  thread %lu '%s':",t->tid,t->name);
        for (int f=0;f<t->frames;++f) {
            char where[MAX_PATH+64];
            describe_address(where,sizeof(where),(uintptr_t)t->pc[f]);
            printf(" %s%s",where,f+1<t->frames ? " <" : "");
        }
        printf("\n");
    }
    fflush(stdout);
}
static DWORD WINAPI stack_dump_after(void *unused) {
    (void)unused;
    Sleep((DWORD)(strtoul(getenv("BB_STACK_DUMP_AFTER"),NULL,10)*1000u));
    runtime_dump_thread_stacks("BB_STACK_DUMP_AFTER");
    return 0;
}
static int fatal_reported;
static LONG fatal_exception(EXCEPTION_POINTERS *info) {
    if (__atomic_exchange_n(&fatal_reported,1,__ATOMIC_ACQ_REL)) { Sleep(INFINITE); }
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    const CONTEXT *c=info->ContextRecord;
    char where[MAX_PATH+64];
    describe_address(where,sizeof(where),(uintptr_t)c->Rip);
    if (record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2)
        fprintf(stderr,"Fault: %s access to %p at %s (thread %lu)\n",
                record->ExceptionInformation[0]==1 ? "write" : record->ExceptionInformation[0]==8 ? "execute" : "read",
                (void *)record->ExceptionInformation[1],where,GetCurrentThreadId());
    else
        fprintf(stderr,"Fault: exception 0x%08lx at %s (thread %lu)\n",record->ExceptionCode,where,GetCurrentThreadId());
    fprintf(stderr,"  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n"
            "  r8=%016llx  r9=%016llx  r10=%016llx r11=%016llx\n  r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n",
            c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->Rbp,c->Rsp,
            c->R8,c->R9,c->R10,c->R11,c->R12,c->R13,c->R14,c->R15);
    if (gpu_enabled) bbgpu_dump_guest_writes(info);
    if (isa_table) {
        int site=isa_rewrite_site_of_stub(isa_table,(uintptr_t)c->Rip);
        if (site>=0) fprintf(stderr,"  While emulating site 0x%llx (%s)\n",(unsigned long long)isa_table->sites[site].offset,
                             isa_kind_name(isa_table->sites[site].kind));
        isa_rewrite_report(isa_table,stderr);
    }
    /* The code bytes around rip: shows a jump into the middle of an instruction or a stub. */
    {
        unsigned char code[64];
        SIZE_T got=0;
        if (ReadProcessMemory(GetCurrentProcess(),(void *)(uintptr_t)(c->Rip-48),code,sizeof(code),&got) && got==sizeof(code)) {
            fprintf(stderr,"  code at rip-48:");
            for (int i=0;i<64;++i) fprintf(stderr,"%s%02x",i==48?" |":" ",code[i]);
            fprintf(stderr,"\n");
        }
    }
    /* BB_FAULT_TABLE=1 (diagnosis, U1 EU 0x33972d: a bit-array pointer read from a 48-byte table
     * entry was garbage under VRAM give-back stress, 2026-10-27): the chain the game followed
     * (rsi+0x600 -> index at +0x118; [rbp-0x68]+0x10 -> entries; entry + 0x20 -> pointer). */
    if (getenv("BB_FAULT_TABLE") && image && c->Rip-(uint64_t)(uintptr_t)image==0x33972d) {
        uint64_t d=0,p=0,e=0; uint32_t idx=0; unsigned char entry[48]; SIZE_T got=0;
        HANDLE self=GetCurrentProcess();
        ReadProcessMemory(self,(void *)(uintptr_t)(c->Rsi+0x600),&d,8,&got);
        ReadProcessMemory(self,(void *)(uintptr_t)(d+0x118),&idx,4,&got);
        ReadProcessMemory(self,(void *)(uintptr_t)(c->Rbp-0x68),&p,8,&got);
        ReadProcessMemory(self,(void *)(uintptr_t)(p+0x10),&e,8,&got);
        const uint64_t at=e+(uint64_t)idx*48;
        fprintf(stderr,"  Table: object %016llx index %u, table %016llx entries %016llx, entry %016llx:",
                (unsigned long long)d,idx,(unsigned long long)p,(unsigned long long)e,(unsigned long long)at);
        if (ReadProcessMemory(self,(void *)(uintptr_t)at,entry,sizeof(entry),&got) && got==sizeof(entry))
            for (int i=0;i<48;++i) fprintf(stderr,"%s%02x",i%8?"":" ",entry[i]);
        fprintf(stderr,"\n");
    }
    /* The stack words at rsp that point into code: a jump to a bad address leaves its caller's
     * return address there, which the rbp chain below skips. */
    for (int i=0;i<16;++i) {
        uint64_t word=0;
        SIZE_T got=0;
        if (!ReadProcessMemory(GetCurrentProcess(),(void *)(uintptr_t)(c->Rsp+8*i),&word,8,&got) || got!=8) break;
        if (image && word-(uint64_t)(uintptr_t)image<0x4000000) {
            describe_address(where,sizeof(where),(uintptr_t)word);
            fprintf(stderr,"  [rsp+%#x] %s\n",8*i,where);
        }
    }
    /* rbp chain (guest and host code keep frame pointers). */
    uintptr_t rbp=(uintptr_t)c->Rbp;
    for (int depth=0; depth<24 && rbp; ++depth) {
        uintptr_t frame[2];
        SIZE_T got=0;
        if (!ReadProcessMemory(GetCurrentProcess(),(void *)rbp,frame,sizeof(frame),&got) || got!=sizeof(frame) || frame[0]<=rbp) break;
        describe_address(where,sizeof(where),frame[1]);
        fprintf(stderr,"  #%d %s\n",depth,where);
        rbp=frame[0];
    }
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(),3);
    return EXCEPTION_CONTINUE_SEARCH;
}
/* AMD FX (Piledriver) has everything the PS4's code needs except MOVBE (byte-swapped loads and
 * stores, a few hundred places in each game). There it raises an illegal instruction: the load
 * or store is done here and the guest goes on after it (issue #251). Register operand forms do
 * not exist; segment and address-size prefixes are left to the fault report. */
static volatile LONG64 movbe_emulated;
/* Decodes a MOVBE with a memory operand at p (regs: Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi,
 * R8-R15, as in CONTEXT): its length, or 0 when it is not one. */
static int movbe_decode(const unsigned char *p, const uint64_t *regs, uint64_t rip, uint64_t *address,
                        int *reg, int *width, int *store) {
    int n=0, size16=0, rex=0;
    while (n<3 && p[n]==0x66) { size16=1; ++n; }
    if ((p[n]&0xf0)==0x40) rex=p[n++];
    if (p[n]!=0x0f || p[n+1]!=0x38 || (p[n+2]!=0xf0 && p[n+2]!=0xf1)) return 0;
    *store=p[n+2]==0xf1;
    n+=3;
    const unsigned char modrm=p[n++];
    const int mod=modrm>>6, rm=modrm&7;
    *reg=((modrm>>3)&7)|((rex&4)<<1);
    if (mod==3) return 0;
    uint64_t at=0;
    int rip_relative=0;
    if (rm==4) {
        const unsigned char sib=p[n++];
        const int index=((sib>>3)&7)|((rex&2)<<2), base=(sib&7)|((rex&1)<<3);
        if (index!=4) at+=regs[index]<<(sib>>6);
        if ((sib&7)==5 && mod==0) { int32_t d; memcpy(&d,p+n,4); n+=4; at+=(uint64_t)(int64_t)d; }
        else at+=regs[base];
    } else if (rm==5 && mod==0) {
        int32_t d; memcpy(&d,p+n,4); n+=4; at=(uint64_t)(int64_t)d; rip_relative=1;
    } else {
        at=regs[rm|((rex&1)<<3)];
    }
    if (mod==1) { at+=(uint64_t)(int64_t)(int8_t)p[n]; n+=1; }
    else if (mod==2) { int32_t d; memcpy(&d,p+n,4); n+=4; at+=(uint64_t)(int64_t)d; }
    if (rip_relative) at+=rip+(uint64_t)n;
    *address=at;
    *width=(rex&8) ? 8 : size16 ? 2 : 4;
    return n;
}
static int emulate_movbe(CONTEXT *c) {
    DWORD64 *regs=&c->Rax;
    uint64_t address;
    int reg, width, store;
    const int n=movbe_decode((const unsigned char *)(uintptr_t)c->Rip,(const uint64_t *)regs,c->Rip,
                             &address,&reg,&width,&store);
    if (!n) return 0;
    void *memory=(void *)(uintptr_t)address;
    if (store) {
        const uint64_t v=regs[reg];
        if (width==8) { uint64_t s=__builtin_bswap64(v); memcpy(memory,&s,8); }
        else if (width==4) { uint32_t s=__builtin_bswap32((uint32_t)v); memcpy(memory,&s,4); }
        else { uint16_t s=__builtin_bswap16((uint16_t)v); memcpy(memory,&s,2); }
    } else if (width==8) {
        uint64_t v; memcpy(&v,memory,8); regs[reg]=__builtin_bswap64(v);
    } else if (width==4) {
        uint32_t v; memcpy(&v,memory,4); regs[reg]=__builtin_bswap32(v);
    } else {
        uint16_t v; memcpy(&v,memory,2); regs[reg]=(regs[reg]&~(DWORD64)0xffff)|__builtin_bswap16(v);
    }
    c->Rip+=(DWORD64)n;
    InterlockedIncrement64(&movbe_emulated);
    return 1;
}
static LONG CALLBACK vectored_fault(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    DWORD code=record->ExceptionCode;
    if (code==EXCEPTION_ILLEGAL_INSTRUCTION && is_patched_rdtsc((uintptr_t)info->ContextRecord->Rip)) {
        const uint64_t tsc=runtime_guest_tsc();
        info->ContextRecord->Rax=(uint32_t)tsc;
        info->ContextRecord->Rdx=tsc>>32;
        info->ContextRecord->Rip+=2;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2) {
        /* GPU page tracking (write-protected guest pages) is resolved first. */
        if (gpu_enabled && bbgpu_handle_fault(info,(void *)record->ExceptionInformation[1])) {
            red_zone_fault_handled(info->ContextRecord->Rip);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (runtime_fault_recover) {
            /* A speculative guest memory read (GPU draw preparation) faulted: the thread resumes
             * in bb_longjmp, outside the exception dispatcher, back at its recovery point. */
            unsigned long long *buffer=*runtime_fault_recover;
            runtime_fault_recover=NULL;
            CONTEXT *c=info->ContextRecord;
            c->Rsp=((c->Rsp-128)&~(DWORD64)15)-40;
            c->Rcx=(DWORD64)(uintptr_t)buffer;
            c->Rdx=1;
            c->Rip=(DWORD64)(uintptr_t)bb_longjmp;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code==EXCEPTION_BREAKPOINT && isa_table && record->ExceptionAddress) {
        /* An int3 that isa_rewrite.c put over a 4-byte instruction it could not jump from: emulated here. */
        CONTEXT *c=info->ContextRecord;
        IsaRegs regs;
        regs.rflags=c->EFlags;
        memcpy(regs.gpr,&c->Rax,sizeof(regs.gpr));   /* CONTEXT holds rax..r15 in x86 order */
        uintptr_t next;
        if (isa_rewrite_trap(isa_table,(uintptr_t)record->ExceptionAddress,&regs,&next)) {
            memcpy(&c->Rax,regs.gpr,sizeof(regs.gpr));
            c->EFlags=(DWORD)regs.rflags;
            c->Rip=next;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code==EXCEPTION_ILLEGAL_INSTRUCTION && image &&
        (uintptr_t)info->ContextRecord->Rip-(uintptr_t)image<0x20000000 &&
        emulate_movbe(info->ContextRecord)) return EXCEPTION_CONTINUE_EXECUTION;
    if (code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_ILLEGAL_INSTRUCTION || code==EXCEPTION_PRIV_INSTRUCTION ||
        code==EXCEPTION_STACK_OVERFLOW || code==EXCEPTION_INT_DIVIDE_BY_ZERO || code==EXCEPTION_DATATYPE_MISALIGNMENT) {
        /* Faults in guest code, the loader or the runtime end the process with a report; drivers
         * and system libraries may handle their own exceptions. */
        uintptr_t rip=(uintptr_t)info->ContextRecord->Rip;
        HMODULE self=GetModuleHandleA(NULL), module=NULL;
        int ours=(image && rip-(uintptr_t)image<0x20000000) ||
                 (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)rip,&module) && module==self) ||
                 !module;
        if (ours) return fatal_exception(info);
    }
    /* BB_STRICT_HANDLES=1: a closed or invalid handle used anywhere is reported (diagnostics). */
    if (code==0xC0000008 /* STATUS_INVALID_HANDLE */) return fatal_exception(info);
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI unhandled_fault(EXCEPTION_POINTERS *info) { return fatal_exception(info); }
/* Guest code reads its TCB with `mov rax, gs:[0]` (link_*.py rewrote fs:[0]); on Windows
 * the displacement becomes the TEB TLS slot that holds the guest TCB (runtime_thread.c). */
uint32_t runtime_thread_tcb_offset(void);
static uint64_t patch_tcb_loads(const Segment *segments, uint64_t count) {
    static const unsigned char load[]={0x65,0x48,0x8b,0x04,0x25,0,0,0,0};
    uint32_t displacement=runtime_thread_tcb_offset();
    uint64_t patched=0;
    for (uint64_t s=0;s<count;++s) {
        if (!(segments[s].flags&1) || segments[s].size<sizeof(load)) continue;
        unsigned char *at=image+segments[s].address, *end=at+segments[s].size-sizeof(load);
        for (; at<=end; ++at)
            if (at[0]==0x65 && !memcmp(at,load,sizeof(load))) { memcpy(at+5,&displacement,4); ++patched; at+=sizeof(load)-1; }
    }
    return patched;
}
#endif
#ifndef _WIN32
static void fault(int sig, siginfo_t *info, void *context) {
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (gpu_enabled && sig == SIGSEGV && bbgpu_handle_fault(context, info->si_addr)) return;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    ucontext_t *uc = context;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    char line[512];
    Dl_info where;
    if (rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (gpu_enabled) bbgpu_dump_guest_writes(context);
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
    _exit(128 + sig);
}
#endif
#ifndef _WIN32
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
static uintptr_t exe_base;
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=(uintptr_t)uc->uc_mcontext.gregs[REG_RIP], rbp=(uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,(uint64_t)uc->uc_mcontext.gregs[REG_RDI]);
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; dump_frames(context); }
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    const char head[]="STOP: watchdog timeout; thread stacks:\n";
    { ssize_t written_=write(2,head,sizeof(head)-1); (void)written_; }
    dump_frames(context);
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self=gettid();
    while (dir>=0 && (n=syscall(SYS_getdents64,dir,buffer,sizeof(buffer)))>0)
        for (long at=0; at<n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d=(void *)(buffer+at);
            pid_t tid=(pid_t)strtol(d->name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=d->reclen;
        }
    usleep(100000);
    _exit(128 + sig);
}
#endif
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
/* BBPATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"BBPATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",count,bytes,rebased);
}
/* BB_GUEST_HOOKS="offset:bytes,...,START-END": diagnostic call counters on guest functions.
 * The first `bytes` (>= 5, whole position-independent instructions) at each image offset move
 * to a thunk near the image, which counts the call and keeps its first two arguments; START-END
 * hooks every 16-byte aligned function in that range that begins with a frame-pointer prologue.
 * The counts that changed are printed every 2 s. */
#include <pthread.h>
typedef struct { volatile uint64_t calls, rdi, rsi, rdx, rcx, r8, r15; uint64_t offset, printed; } GuestHook;
static GuestHook *guest_hooks;
static size_t guest_hook_count;
static uint64_t guest_image_size;
/* A pointer argument into the image (or guest memory the tracker knows) shown as text. */
static void print_hook_string(const char *name, uint64_t value) {
    const unsigned char *p=(const unsigned char *)(uintptr_t)value;
    if (value<(uint64_t)(uintptr_t)image || value>=(uint64_t)(uintptr_t)image+guest_image_size-64) return;
    char text[65]; size_t n=0;
    while (n<64 && p[n]>=32 && p[n]<127) { text[n]=(char)p[n]; ++n; }
    text[n]=0;
    if (n>=3) printf("    %s -> \"%s\"\n",name,text);
}
static void *report_guest_hooks(void *unused) {
    (void)unused;
    for (;;) {
#ifdef _WIN32
        Sleep(2000);
#else
        sleep(2);
#endif
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC,&now);
        printf("Hooks at %.1f s\n",(double)now.tv_sec+now.tv_nsec/1e9);
        for (size_t i=0;i<guest_hook_count;++i) {
            GuestHook *h=&guest_hooks[i];
            uint64_t calls=h->calls;
            if (calls==h->printed) continue;
            printf("Hook %#" PRIx64 ": %" PRIu64 " calls (+%" PRIu64 ", last rdi %#" PRIx64 " rsi %#" PRIx64 " rdx %#" PRIx64
                   " rcx %#" PRIx64 " r8 %#" PRIx64 " r15 %#" PRIx64 ")\n",h->offset,calls,calls-h->printed,(uint64_t)h->rdi,
                   (uint64_t)h->rsi,(uint64_t)h->rdx,(uint64_t)h->rcx,(uint64_t)h->r8,(uint64_t)h->r15);
            h->printed=calls;
            print_hook_string("rdi",h->rdi); print_hook_string("rsi",h->rsi);
            print_hook_string("rdx",h->rdx); print_hook_string("rcx",h->rcx);
        }
        fflush(stdout);
    }
    return NULL;
}
/* BB_HOOK_DUMP=<hook offset>:<bytes>:<file>: every 20 ms the memory at the hook's last rdi
 * (an object) is appended to <file> as [double time][bytes] records (finding fields that follow
 * a value, e.g. a camera angle). */
static void *dump_hook_object(void *unused) {
    (void)unused;
    const char *spec=getenv("BB_HOOK_DUMP");
    char *end;
    const uint64_t offset=strtoull(spec,&end,0);
    const size_t bytes=*end==':' ? (size_t)strtoull(end+1,&end,0) : 0;
    FILE *f=*end==':' ? fopen(end+1,"wb") : NULL;
    GuestHook *h=NULL;
    for (size_t i=0;i<guest_hook_count;++i) if (guest_hooks[i].offset==offset) h=&guest_hooks[i];
    if (!f || !h || !bytes) { printf("BB_HOOK_DUMP: bad specification\n"); return NULL; }
    unsigned char *copy=malloc(bytes);
    const uint64_t before=getenv("BB_HOOK_DUMP_BEFORE") ? strtoull(getenv("BB_HOOK_DUMP_BEFORE"),NULL,0) : 0;
    for (;;) {
#ifdef _WIN32
        Sleep(50);
#else
        usleep(50000);
#endif
        const uint64_t object=h->rdi ? h->rdi-before : 0;
        if (!object) continue;
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((const void *)(uintptr_t)object,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
            (uint64_t)(uintptr_t)info.BaseAddress+info.RegionSize<object+bytes) continue;
#endif
        memcpy(copy,(const void *)(uintptr_t)object,bytes);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC,&now);
        const double t=(double)now.tv_sec+now.tv_nsec/1e9;
        fwrite(&t,sizeof(t),1,f);
        fwrite(copy,1,bytes,f);
        fflush(f);
    }
    return NULL;
}
/* Bytes of whole instructions (>= 5) a frame-pointer prologue at `f` can move, 0 if unknown. */
static size_t prologue_bytes(const unsigned char *f) {
    if (memcmp(f,"\x55\x48\x89\xe5",4)) return 0;
    if (f[4]==0x41 && f[5]>=0x50 && f[5]<=0x57) return 6;
    if (f[4]>=0x50 && f[4]<=0x57) return 5;
    if (f[4]==0x48 && f[5]==0x83 && f[6]==0xec) return 8;
    if (f[4]==0x48 && f[5]==0x81 && f[6]==0xec) return 11;
    return 0;
}
static void install_guest_hooks(Segment *segments, uint64_t ns) {
    const char *spec=getenv("BB_GUEST_HOOKS");
    if (!spec || !*spec) return;
    enum { MAX_HOOKS=16384, THUNK=80 };
    guest_hooks=calloc(MAX_HOOKS,sizeof(*guest_hooks));
    uint64_t end_of_image=0;
    for (uint64_t s=0;s<ns;++s) if (segments[s].address+segments[s].size>end_of_image) end_of_image=segments[s].address+segments[s].size;
    guest_image_size=end_of_image;
    unsigned char *thunks=NULL;
#ifdef _WIN32
    /* rel32 jumps: the thunks within 2 GiB of the image. */
    uint64_t base=((uint64_t)(uintptr_t)image+end_of_image+0xffff)&~(uint64_t)0xffff;
    for (uint64_t step=0;!thunks && step<(1900ull<<20);step+=1u<<16) {
        thunks=VirtualAlloc((void *)(uintptr_t)(base+step),(size_t)MAX_HOOKS*THUNK,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
        if (!thunks && (uint64_t)(uintptr_t)image>step+(2u<<20))
            thunks=VirtualAlloc((void *)(uintptr_t)(((uint64_t)(uintptr_t)image-step-(2u<<20))&~(uint64_t)0xffff),
                                (size_t)MAX_HOOKS*THUNK,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    }
#endif
    if (!thunks) fail("BB_GUEST_HOOKS: no thunk memory near the image");
    for (const char *p=spec;*p;) {
        char *end;
        uint64_t first=strtoull(p,&end,0), last=first, n=0;
        if (*end==':') n=strtoull(end+1,&end,0);
        else if (*end=='-') last=strtoull(end+1,&end,0);
        else fail("bad BB_GUEST_HOOKS entry");
        if (*end && *end!=',') fail("bad BB_GUEST_HOOKS entry");
        p=*end==',' ? end+1 : end;
        for (uint64_t offset=first;offset<=last;offset+=first==last ? 1 : 16) {
            if (!mapped(segments,ns,offset,16)) { if (first==last) fail("bad BB_GUEST_HOOKS entry"); continue; }
            unsigned char *f=image+offset;
            size_t moved=first==last ? (size_t)n : prologue_bytes(f);
            if (first!=last && !moved) continue;
            if (moved<5 || moved>16 || guest_hook_count==MAX_HOOKS) fail("bad BB_GUEST_HOOKS entry");
            GuestHook *h=&guest_hooks[guest_hook_count];
            unsigned char *t=thunks+guest_hook_count*THUNK;
            ++guest_hook_count;
            h->offset=offset;
            uintptr_t slot=(uintptr_t)h;
            size_t k=0;
            t[k++]=0x50;                                                /* push rax */
            t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&slot,8); k+=8;        /* movabs rax, hook */
            t[k++]=0xf0; t[k++]=0x48; t[k++]=0xff; t[k++]=0x00;         /* lock inc qword [rax] */
            t[k++]=0x48; t[k++]=0x89; t[k++]=0x78; t[k++]=0x08;         /* mov [rax+8], rdi */
            t[k++]=0x48; t[k++]=0x89; t[k++]=0x70; t[k++]=0x10;         /* mov [rax+16], rsi */
            t[k++]=0x48; t[k++]=0x89; t[k++]=0x50; t[k++]=0x18;         /* mov [rax+24], rdx */
            t[k++]=0x48; t[k++]=0x89; t[k++]=0x48; t[k++]=0x20;         /* mov [rax+32], rcx */
            t[k++]=0x4c; t[k++]=0x89; t[k++]=0x40; t[k++]=0x28;         /* mov [rax+40], r8 */
            t[k++]=0x4c; t[k++]=0x89; t[k++]=0x78; t[k++]=0x30;         /* mov [rax+48], r15 */
            t[k++]=0x58;                                                /* pop rax */
            memcpy(t+k,f,moved); k+=moved;                              /* the moved instructions */
            int32_t back=(int32_t)((int64_t)(uintptr_t)(f+moved)-(int64_t)(uintptr_t)(t+k+5));
            t[k++]=0xe9; memcpy(t+k,&back,4); k+=4;                     /* jmp offset+moved */
            int32_t to=(int32_t)((int64_t)(uintptr_t)t-(int64_t)(uintptr_t)(f+5));
            f[0]=0xe9; memcpy(f+1,&to,4); memset(f+5,0x90,moved-5);
        }
    }
    printf("Hooks: %zu guest functions counted\n",guest_hook_count);
    pthread_t thread;
    pthread_create(&thread,NULL,report_guest_hooks,NULL);
    if (getenv("BB_HOOK_DUMP")) pthread_create(&thread,NULL,dump_hook_object,NULL);
}
/* Game callbacks (runtime_guest_callbacks: mouse camera): a guest function whose code matches
 * the expected bytes calls a host function first, with rdi, rsi, the image base and r14 (Microsoft
 * x64 convention on every host). The thunk keeps the guest's registers and flags, so it also
 * works inside a function. */
static unsigned char *near_image_memory(size_t size) {
    unsigned char *memory=NULL;
#ifdef _WIN32
    /* rel32 jumps: within 2 GiB of the image. */
    uint64_t end_of_image=round_page(guest_image_size ? guest_image_size : 1);
    uint64_t base=((uint64_t)(uintptr_t)image+end_of_image+(32u<<20)+0xffff)&~(uint64_t)0xffff;
    for (uint64_t step=0;!memory && step<(1900ull<<20);step+=1u<<16) {
        memory=VirtualAlloc((void *)(uintptr_t)(base+step),size,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
        if (!memory && (uint64_t)(uintptr_t)image>step+(2u<<20))
            memory=VirtualAlloc((void *)(uintptr_t)(((uint64_t)(uintptr_t)image-step-(2u<<20))&~(uint64_t)0xffff),
                                size,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    }
#else
    (void)size;
#endif
    return memory;
}
/* A thunk that calls `callback` (Microsoft x64: rdi, rsi, the image base, r14) with the guest's
 * registers and flags kept, runs the `moved` bytes of position-independent instructions from `f`
 * and jumps back; `f` jumps to it. 0 without thunk memory. */
#define CALLBACK_THUNK_BYTES 16384 /* ~170 bytes per thunk (saves, red-zone step, xmm spills): about 90 hooks */
static unsigned char *callback_thunks;
static size_t callback_thunks_used;
static int callback_thunk(unsigned char *f, size_t moved, const void *callback) {
    if (!callback_thunks && !(callback_thunks=near_image_memory(CALLBACK_THUNK_BYTES))) {
        printf("Callbacks: no thunk memory\n");
        return 0;
    }
    if (callback_thunks_used+256>CALLBACK_THUNK_BYTES) {
        printf("Callbacks: thunk memory full\n");
        return 0;
    }
    unsigned char *t=callback_thunks+callback_thunks_used;
    size_t k=0;
    /* lea rsp, [rsp-0x80] (a hook inside a leaf function must not touch the 128 bytes below rsp
     * it keeps locals in; after drakolordx7, upstream PR #396); pushfq; push rax rcx rdx r8-r11 rbx;
     * mov rbx, rsp; and rsp, -16; sub rsp, 0xb0; mov [rsp+0x20], rbx (works at a function's start
     * and inside it). The callback's 5th argument is rbx: the saved registers rbx r11 r10 r9 r8
     * rdx rcx rax rflags (Umar-Qayyum-dev's raw mouse), then the 128-byte red-zone step, then (at
     * a function's start) the return address: regs[9 + 16]. */
    static const unsigned char save[]={0x48,0x8d,0x64,0x24,0x80,0x9c,0x50,0x51,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x53,
                                       0x48,0x89,0xe3,0x48,0x83,0xe4,0xf0,0x48,0x81,0xec,0xb0,0x00,0x00,0x00,
                                       0x48,0x89,0x5c,0x24,0x20};
    memcpy(t+k,save,sizeof(save)); k+=sizeof(save);
    for (int x=0;x<8;++x) {                                     /* movdqu [rsp+0x30+16x], xmmx */
        const uint32_t disp=0x30u+16u*(uint32_t)x;
        t[k++]=0xf3; t[k++]=0x0f; t[k++]=0x7f; t[k++]=(unsigned char)(0x84|x<<3); t[k++]=0x24;
        memcpy(t+k,&disp,4); k+=4;
    }
    t[k++]=0x48; t[k++]=0x89; t[k++]=0xf9;                      /* mov rcx, rdi */
    t[k++]=0x48; t[k++]=0x89; t[k++]=0xf2;                      /* mov rdx, rsi */
    const uintptr_t base=(uintptr_t)image, host=(uintptr_t)callback;
    t[k++]=0x49; t[k++]=0xb8; memcpy(t+k,&base,8); k+=8;        /* movabs r8, image */
    t[k++]=0x4d; t[k++]=0x89; t[k++]=0xf1;                      /* mov r9, r14 */
    t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&host,8); k+=8;        /* movabs rax, callback */
    t[k++]=0xff; t[k++]=0xd0;                                   /* call rax */
    for (int x=0;x<8;++x) {                                     /* movdqu xmmx, [rsp+0x30+16x] */
        const uint32_t disp=0x30u+16u*(uint32_t)x;
        t[k++]=0xf3; t[k++]=0x0f; t[k++]=0x6f; t[k++]=(unsigned char)(0x84|x<<3); t[k++]=0x24;
        memcpy(t+k,&disp,4); k+=4;
    }
    /* mov rsp, rbx; pop rbx r11-r8 rdx rcx rax; popfq; lea rsp, [rsp+0x80] */
    static const unsigned char restore[]={0x48,0x89,0xdc,0x5b,0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,
                                          0x5a,0x59,0x58,0x9d,0x48,0x8d,0xa4,0x24,0x80,0x00,0x00,0x00};
    memcpy(t+k,restore,sizeof(restore)); k+=sizeof(restore);
    memcpy(t+k,f,moved); k+=moved;                              /* the moved instructions */
    int32_t back=(int32_t)((int64_t)(uintptr_t)(f+moved)-(int64_t)(uintptr_t)(t+k+5));
    t[k++]=0xe9; memcpy(t+k,&back,4); k+=4;
    int32_t to=(int32_t)((int64_t)(uintptr_t)t-(int64_t)(uintptr_t)(f+5));
    f[0]=0xe9; memcpy(f+1,&to,4); memset(f+5,0x90,moved-5);
    callback_thunks_used+=(k+15)&~(size_t)15;
    return 1;
}
static void install_game_callbacks(Segment *segments, uint64_t ns, uint64_t image_size) {
    size_t count=0;
    const RuntimeGuestCallback *callbacks=runtime_guest_callbacks(&count);
    guest_image_size=image_size;
    for (size_t i=0;i<count;++i) {
        const RuntimeGuestCallback *c=&callbacks[i];
        if (!mapped(segments,ns,c->offset,c->signature_size) ||
            memcmp(image+c->offset,c->signature,c->signature_size)) continue;
        if (!c->callback) {
            memcpy(image+c->offset,c->patch,c->moved);
            printf("Callbacks: %s at %#" PRIx64 " (patch)\n",c->name,c->offset);
            continue;
        }
        if (!callback_thunk(image+c->offset,c->moved,(const void *)c->callback)) return;
        printf("Callbacks: %s at %#" PRIx64 "\n",c->name,c->offset);
    }
}
/* Render command contexts (ndgi): Begin and End are hooked to keep a per-thread flag, set while a
 * job worker has a context open (runtime_ndgi_open, cleared too when a fiber leaves the thread,
 * runtime_fiber.c), used by the job wait patch below. A context records its worker at Begin
 * (+0xe810) and asserts the same one at End ("m_workerThreadIndex == GetCurrentWorkerThreadIndex()"). */
#ifdef _WIN32
extern _Thread_local int runtime_ndgi_open; /* runtime_fiber.c */
static void __attribute__((ms_abi)) ndgi_end(uint64_t rdi, uint64_t rsi, uint64_t base, uint64_t r14) {
    (void)rdi; (void)rsi; (void)base; (void)r14;
    runtime_ndgi_open=0;
}
static void __attribute__((ms_abi)) ndgi_begin(uint64_t rdi, uint64_t rsi, uint64_t base, uint64_t r14) {
    (void)rdi; (void)rsi; (void)base; (void)r14;
    runtime_ndgi_open=1;
}
static int64_t find_masked(const char *hex, uint64_t size);
static void install_ndgi_hooks(uint64_t image_size) {
    const char *wait=getenv("BB_JOB_LOCK_WAIT_MS");
    if (wait && wait[0]=='0' && !wait[1]) return; /* nothing uses the flag */
    static const unsigned char begin[]={0x49,0x89,0x84,0x24,0x10,0xe8,0x00,0x00}; /* mov [r12+0xe810], rax */
    int64_t begin_at=-1;
    for (uint64_t at=0;at+16<=image_size;++at) {
        if (image[at]!=0x49 || memcmp(image+at,begin,sizeof(begin))) continue;
        if (begin_at>=0) { begin_at=-2; break; }
        begin_at=(int64_t)at;
    }
    size_t begin_len=8;
    if (begin_at==-1) {
        /* US 1.02 (as CUSA02344): the worker path's mov [r15+0xe810], r12 */
        begin_at=find_masked("4d89a710e80000",image_size);
        begin_len=7;
    }
    if (begin_at>=0 && callback_thunk(image+begin_at,begin_len,(const void *)ndgi_begin))
        printf("Render contexts: begin hooked at %#" PRIx64 "\n",(uint64_t)begin_at);
    else
        printf("Render contexts: begin site %s\n",begin_at==-2 ? "not unique" : "not found");
    /* End: mov rbx,[r14+0xe810]; call GetCurrentWorkerThreadIndex; cmp rbx,rax; je */
    static const unsigned char head[]={0x49,0x8b,0x9e,0x10,0xe8,0x00,0x00,0xe8};
    static const unsigned char tail[]={0x48,0x39,0xc3,0x74};
    int64_t found=-1;
    for (uint64_t at=0;at+16<=image_size;++at) {
        if (image[at]!=0x49 || memcmp(image+at,head,sizeof(head)) || memcmp(image+at+12,tail,sizeof(tail))) continue;
        if (found>=0) { printf("Render contexts: end site not unique\n"); return; }
        found=(int64_t)at;
    }
    if (found<0) {
        /* US 1.02: cmp byte [r15+0xe7dc],0; je; mov r12,[r15+0xe810] (the index call is inlined) */
        const int64_t us=find_masked("4180bfdce70000 00 0f84???????? 4d8ba710e80000",image_size);
        if (us>=0) found=us+14;
    }
    if (found<0) { printf("Render contexts: end site not found\n"); return; }
    if (callback_thunk(image+found,7,(const void *)ndgi_end))
        printf("Render contexts: end hooked at %#" PRIx64 "\n",(uint64_t)found);
}
#endif
/* Naughty Dog's job system: a worker with no job checks the queues again after a pause and only
 * sleeps (scePthreadCondTimedwait) when a flag of the scheduler allows it, which it does not here:
 * U2's museum, standing, 4-5 workers at 75-93 % CPU spun there on Umar's 4-core/8-thread PC
 * (CPU at 98 %), and the game's main thread and the port's GPU threads got less of it. On the
 * PS4 every worker has a core of its own. At the point both idle paths meet (mov r8, r14; cmp
 * r13d, 3; ja; ...; rdtsc), found by its code in any version of the three games, the worker now
 * yields its CPU to a thread that wants one (SwitchToThread: no wait when none does).
 * BB_JOB_YIELD=0: off; BB_TOGGLE_FILE bit 61: off while running. */
extern uint64_t runtime_disabled_optimizations;
static void __attribute__((ms_abi)) job_idle_yield(uint64_t rdi, uint64_t rsi, uint64_t base,
                                                   uint64_t r14) {
    (void)rdi; (void)rsi; (void)base; (void)r14;
    if (__atomic_load_n(&runtime_disabled_optimizations,__ATOMIC_RELAXED) & (UINT64_C(1)<<61)) return;
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}
/* Windows' Meltdown mitigation (kernel VA shadow, on Intel CPUs before about the 8th generation)
 * makes every kernel call much dearer, and the idle workers call SwitchToThread thousands of times
 * a second: an i7-7700K player's U3 (CPU-bound, ~1900 draws) ran ~50 FPS with the yield and ~55
 * without it, while newer CPUs gain ~4 % from it (2026-10-26). */
static int kernel_va_shadow_on(void) {
#ifdef _WIN32
    typedef LONG (WINAPI *QuerySystem)(ULONG,void *,ULONG,ULONG *);
    const QuerySystem query=(QuerySystem)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtQuerySystemInformation");
    ULONG flags=0;
    /* SystemKernelVaShadowInformation (196): bit 0 = KvaShadowEnabled */
    return query && query(196,&flags,sizeof(flags),NULL)>=0 && (flags&1);
#else
    return 0;
#endif
}
static void install_job_idle_yield(uint64_t image_size) {
    const char *env=getenv("BB_JOB_YIELD");
    if (env && env[0]=='0') { printf("Job workers: idle ones do not yield (BB_JOB_YIELD=0)\n"); return; }
    if (!(env && env[0]=='1') && kernel_va_shadow_on()) {
        printf("Job workers: idle ones do not yield (kernel VA shadow on: kernel calls are slow; BB_JOB_YIELD=1 yields)\n");
        return;
    }
    /* US / EU-Russian 1.02: mov r8,r14; cmp r13d,3; ja; mov rax,[r15]; mov rcx,[rax+r12*8+d32]; rdtsc.
     * EU 1.02 (CUSA02343): the same with other registers: cmp r12d,3; mov rax,[r13]. */
    static const struct { unsigned char head[7]; unsigned char tail[8]; size_t tail_len; } forms[]={
        {{0x4d,0x89,0xf0,0x41,0x83,0xfd,0x03},{0x49,0x8b,0x07,0x4a,0x8b,0x8c,0xe0},7},
        {{0x4d,0x89,0xf0,0x41,0x83,0xfc,0x03},{0x49,0x8b,0x45,0x00,0x4a,0x8b,0x8c,0xe0},8},
    };
    int hooked=0;
    for (uint64_t at=0;at+32<=image_size;++at) {
        const unsigned char *c=image+at;
        if (c[0]!=0x4d) continue;
        int form=-1;
        for (int f=0;f<2 && form<0;++f) if (!memcmp(c,forms[f].head,7)) form=f;
        if (form<0) continue;
        size_t j=7;
        if (c[j]==0x0f && c[j+1]==0x87) j+=6;       /* ja rel32 */
        else if (c[j]==0x77) j+=2;                  /* ja rel8 */
        else continue;
        const size_t tl=forms[form].tail_len;
        if (memcmp(c+j,forms[form].tail,tl) || c[j+tl+4]!=0x0f || c[j+tl+5]!=0x31) continue;
        if (!callback_thunk(image+at,sizeof(forms[form].head),(const void *)job_idle_yield)) break;
        printf("Job workers: the idle check at %#" PRIx64 " yields the CPU\n",at);
        ++hooked;
    }
    if (!hooked) printf("Job workers: idle check not found\n");
}
/* Naughty Dog's FrameBegin (ndlib/render/fg-draw-mgr.cpp) asserts that the frame it recycles,
 * three back, has its GPU end-of-pipe tick (set by the game's EOP interrupt handler). It never
 * waits: on PS4 the GPU always keeps up. Here the GPU side can fall behind (shaders building on a
 * first run, an upscaler starting) and the game stopped right after U1's first cutscene. The
 * assertion's branch is found through its message, in any version of the three games, and the
 * failed check now waits (pause, check again) instead. */
static void patch_eop_tick_wait(uint64_t image_size) {
    static const char message[]="GetRenderFrameParams(frameToFreeInstancesFor)->m_gfxEopTick";
    unsigned char *text=NULL;
    for (uint64_t i=0;i+sizeof(message)<=image_size;++i)
        if (image[i]=='G' && !memcmp(image+i,message,sizeof(message))) { text=image+i; break; }
    if (!text) return;
    int patched=0;
    for (uint64_t at=0;at+7<=image_size;++at) {
        /* lea reg, [rip+rel32] -> the message (REX.W, any register) */
        if ((image[at]&0xfb)!=0x48 || image[at+1]!=0x8d || (image[at+2]&0xc7)!=0x05) continue;
        int32_t rel; memcpy(&rel,image+at+3,4);
        if (image+at+7+rel!=text) continue;
        /* The assertion path ends in the int 0x41 shortly after the message is loaded. */
        unsigned char *trap=NULL;
        for (unsigned char *q=image+at+7;q<image+at+0x80 && q+2<=image+image_size;++q)
            if (q[0]==0xcd && q[1]==0x41) { trap=q; break; }
        if (!trap) continue;
        /* Up to 0x100 bytes before the trap: cmp qword [base(+index*scale)+disp8/32], 0 (REX.W 83 /7,
         * mod 01/10, with or without SIB: 1.00 has [rax+0xa08], US 1.02 [rax+rcx+0xa08]), then a short or
         * near jne past the trap (the assertion is the fall-through) or je to the assertion. The assertion
         * path is dead code ending in the trap; its first 7 bytes become pause; jmp back to the cmp. */
        for (unsigned char *c=trap-2;c>image && trap-c<=0x100;--c) {
            if ((c[0]&0xf8)!=0x48 || c[1]!=0x83 || ((c[2]>>3)&7)!=7) continue;
            const int mod=c[2]>>6, sib=(c[2]&7)==4;
            if (mod!=1 && mod!=2) continue;
            const int length=3+sib+(mod==2 ? 4 : 1);
            if (c[length]!=0x00) continue;
            unsigned char *j=c+length+1, *target, *after;
            int jne;
            if (j[0]==0x75 || j[0]==0x74) { target=j+2+(int8_t)j[1]; after=j+2; jne=j[0]==0x75; }
            else if (j[0]==0x0f && (j[1]==0x85 || j[1]==0x84)) {
                int32_t d; memcpy(&d,j+2,4); target=j+6+d; after=j+6; jne=j[1]==0x85;
            } else continue;
            unsigned char *path=jne ? after : target;
            if (jne ? target<=trap : target<=c) continue;
            if (path<c || path+7>trap+2) continue;
            const int32_t back=(int32_t)(c-(path+7));
            path[0]=0xf3; path[1]=0x90; path[2]=0xe9; memcpy(path+3,&back,4);
            printf("Frame pacing: the EOP tick check at %#" PRIx64 " waits for the GPU\n",(uint64_t)(c-image));
            ++patched;
            break;
        }
    }
    if (!patched) printf("Frame pacing: EOP tick check not found\n");
}
/* U1 without the collection menu (BB_SKIP_COLLECTION_MENU=1, the launcher's Drake's Fortune card).
 * eboot.bin holds the collection's front end and U1 in one menu host (menuhost.cpp). After the
 * logos a menu callback (image+0x65d540) opens the collection's front end (host mode 0, its title
 * page: image+0x14b0b0); the carousel's U1 card later opens U1's (mode 1: image+0x14ae00). Its
 * final jump goes through a thunk that takes U1's front end the first time, so the collection's
 * title never shows; later calls open the collection as before. */
/* BB_TIME_SCALE (runtime_kernel.c) slows the clocks the guest reads through the kernel; U2 and U3
 * also read the TSC themselves. Their rdtsc instructions become ud2 (same length) and the fault
 * handler returns the scaled TSC (vectored_fault). All sites of a list must match, else nothing
 * is patched. */
#include "u2_rdtsc_sites.h"
#include "u3_rdtsc_sites.h"
#include "u2_us_rdtsc_sites.h"
#include "u3_us_rdtsc_sites.h"
static const uint32_t *rdtsc_sites;
static size_t rdtsc_count;
uint64_t runtime_guest_tsc(void);
int runtime_time_scaled(void);
static int rdtsc_patched;
void runtime_patch_ultrawide(unsigned char *code, uint64_t size, uint64_t image_end);
void runtime_patch_resolution(unsigned char *code, uint64_t size, uint64_t image_end);
static void patch_rdtsc(uint64_t image_size) {
#ifndef _WIN32
    (void)image_size;
    return; /* the ud2 handler is in the Windows exception handler only */
#endif
    if (!runtime_time_scaled()) return;
    static const struct { const uint32_t *sites; size_t count; } lists[]={
        {u3_rdtsc_sites,sizeof(u3_rdtsc_sites)/sizeof(u3_rdtsc_sites[0])},
        {u2_rdtsc_sites,sizeof(u2_rdtsc_sites)/sizeof(u2_rdtsc_sites[0])},
        {u2_us_rdtsc_sites,sizeof(u2_us_rdtsc_sites)/sizeof(u2_us_rdtsc_sites[0])},
        {u3_us_rdtsc_sites,sizeof(u3_us_rdtsc_sites)/sizeof(u3_us_rdtsc_sites[0])},
    };
    for (size_t l=0;l<sizeof(lists)/sizeof(lists[0]) && !rdtsc_sites;++l) {
        size_t i=0;
        while (i<lists[l].count) {
            const uint32_t at=lists[l].sites[i];
            if (at+2>image_size || image[at]!=0x0f || image[at+1]!=0x31) break;
            ++i;
        }
        if (i==lists[l].count) { rdtsc_sites=lists[l].sites; rdtsc_count=lists[l].count; }
    }
    if (!rdtsc_sites) {
        printf("Time scale: this game's rdtsc sites are not known, its own TSC reads stay unscaled\n");
        return;
    }
    for (size_t i=0;i<rdtsc_count;++i) image[rdtsc_sites[i]+1]=0x0b; /* 0f 31 rdtsc -> 0f 0b ud2 */
    rdtsc_patched=1;
    printf("Time scale: %zu rdtsc instructions in the game scaled\n",rdtsc_count);
}
static int is_patched_rdtsc(uintptr_t rip) {
    if (!rdtsc_patched || !image || rip<(uintptr_t)image) return 0;
    const uint64_t off=rip-(uintptr_t)image;
    size_t lo=0, hi=rdtsc_count;
    while (lo<hi) {
        size_t mid=(lo+hi)/2;
        if (rdtsc_sites[mid]<off) lo=mid+1; else hi=mid;
    }
    return lo<rdtsc_count && rdtsc_sites[lo]==off;
}

static int64_t find_masked(const char *hex, uint64_t size);
static void patch_collection_skip(uint64_t image_size) {
    const char *skip=getenv("BB_SKIP_COLLECTION_MENU");
    if (!skip || skip[0]!='1') return;
    /* The menu callback, found by its code: push rbp; mov rbp, rsp; lea rax, [rip + ...];
     * cmp byte [rax + 0x39c0], 0; je; mov rax, [rdi]; pop rbp; jmp [rax + 0x90]; call ...;
     * mov rdi, rax; pop rbp; jmp (the collection). EU 1.02: image+0x65d540 (collection 0x14b0b0,
     * U1 0x14ae00); US 1.02 (CUSA02320, as CUSA02344): image+0x6734a0 (0x14b570, 0x14b2c0). U1's
     * front end is 0x2b0 bytes before the collection's in both. */
    static const char decide[]="55 48 89 e5 48 8d 05 ?? ?? ?? ?? 80 b8 c0 39 00 00 00 74 0a 48 8b 07 5d ff a0 90 00 00 00 "
                               "e8 ?? ?? ?? ?? 48 89 c7 5d e9 ?? ?? ?? ??";
    static const unsigned char open_u1[]={
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x78,0x49,0x89,0xfd,
    };
    const int64_t at=find_masked(decide,image_size);
    uint64_t jump=0, collection=0, u1=0;
    if (at>=0) {
        int32_t to;
        jump=(uint64_t)at+44-5;
        memcpy(&to,image+jump+1,4);
        collection=jump+5+(uint64_t)(int64_t)to;
        u1=collection-0x2b0;
    }
    if (at<0 || collection<0x2b0 || collection>=image_size || memcmp(image+u1,open_u1,sizeof(open_u1))) {
        printf("Collection menu: not found, the menu stays\n");
        return;
    }
    unsigned char *t=near_image_memory(64);
    if (!t) { printf("Collection menu: no thunk memory, the menu stays\n"); return; }
    unsigned char *flag=t+32;
    size_t k=0;
    int32_t rel;
    t[k++]=0x80; t[k++]=0x3d; rel=(int32_t)(flag-(t+k+5)); memcpy(t+k,&rel,4); k+=4; t[k++]=0x00; /* cmp byte [flag], 0 */
    t[k++]=0x74; t[k++]=12;                                                                         /* je collection */
    t[k++]=0xc6; t[k++]=0x05; rel=(int32_t)(flag-(t+k+5)); memcpy(t+k,&rel,4); k+=4; t[k++]=0x00; /* mov byte [flag], 0 */
    t[k++]=0xe9; rel=(int32_t)((int64_t)(uintptr_t)(image+u1)-(int64_t)(uintptr_t)(t+k+4)); memcpy(t+k,&rel,4); k+=4;
    t[k++]=0xe9; rel=(int32_t)((int64_t)(uintptr_t)(image+collection)-(int64_t)(uintptr_t)(t+k+4)); memcpy(t+k,&rel,4); k+=4;
    *flag=1;
    rel=(int32_t)((int64_t)(uintptr_t)t-(int64_t)(uintptr_t)(image+jump+5));
    memcpy(image+jump+1,&rel,4);
    printf("Collection menu: skipped once, U1's front end opens first\n");
}
/* Every chapter in Chapter Select and Brutal from the start (BB_UNLOCK_CHAPTERS=1, all three
 * games). From BilalReloaded's Uncharted port (tools/probe_uncharted.py UNLOCK_PATCHES,
 * GPL-2.0-or-later, made for CUSA02344 1.02); the EU release keeps the same code with other field
 * offsets and two copies of it. The games keep Bluepoint's tweak flags "Crushing", "Brutal" and
 * "Chapter Select" unlocked by default as three bytes; a constructor clears them (mov byte
 * [rbx+F],1 / [rbx+F+1],0 / [rbx+F+2],0) and the tweak table read fills Crushing and Chapter
 * Select (U2/U3: setne from [rax] and [rax+4]; U1: bytes [rax] and [rax+1]). Chapter Select is
 * now set where the table is read, Brutal where it is cleared. Saves are not touched. */
static int patch_unlock_one(int64_t at, unsigned field) {
    /* Brutal: mov byte [rbx+field-1], 0 just after mov byte [rbx+field-2], 1 before the read. */
    for (int64_t b=at-0x100; b<at; ++b) {
        if (b<0 || image[b]!=0xc6 || image[b+1]!=0x83) continue;
        uint32_t f0, f1;
        memcpy(&f0,image+b+2,4); memcpy(&f1,image+b+9,4);
        if (f0==field-2 && image[b+6]==1 && image[b+7]==0xc6 && image[b+8]==0x83 && f1==field-1 && image[b+13]==0) {
            image[b+13]=1;
            return 1;
        }
    }
    return 0;
}
static void patch_unlock_chapters(uint64_t image_size) {
    const char *on=getenv("BB_UNLOCK_CHAPTERS");
    if (!on || on[0]!='1') return;
    int chapters=0, brutal=0;
    for (uint64_t i=0; i+27<=image_size; ++i) {
        const unsigned char *p=image+i;
        uint32_t field;
        /* U2/U3: cmp dword [rax],0; setne [rbx+F-2]; cmp dword [rax+4],0; setne [rbx+F] */
        if (p[0]==0x83 && p[1]==0x38 && p[2]==0 && p[3]==0x0f && p[4]==0x95 && p[5]==0x83 &&
            !memcmp(p+10,"\x83\x78\x04\x00\x0f\x95\x83",7)) {
            memcpy(&field,p+17,4);
            uint32_t crushing; memcpy(&crushing,p+6,4);
            if (crushing+2!=field) continue;
            static const unsigned char set[11]={0xc6,0x83,0,0,0,0,1,0x0f,0x1f,0x40,0x00};
            unsigned char *q=image+i+10;
            memcpy(q,set,11); memcpy(q+2,&field,4);
            ++chapters; brutal+=patch_unlock_one((int64_t)i,field);
            continue;
        }
        /* U1: test rax,rax; je +0x11; mov cl,[rax]; mov [rbx+F-2],cl; mov al,[rax+1]; mov [rbx+F],al */
        if (p[0]==0x48 && p[1]==0x85 && p[2]==0xc0 && p[3]==0x74 && p[4]==0x11 && p[5]==0x8a && p[6]==0x08 &&
            p[7]==0x88 && p[8]==0x8b && !memcmp(p+13,"\x8a\x40\x01\x88\x83",5)) {
            memcpy(&field,p+18,4);
            uint32_t crushing; memcpy(&crushing,p+9,4);
            if (crushing+2!=field) continue;
            unsigned char *q=image+i+13;
            q[0]=0xc6; q[1]=0x83; memcpy(q+2,&field,4); q[6]=1; q[7]=0x66; q[8]=0x90;
            ++chapters; brutal+=patch_unlock_one((int64_t)i,field);
        }
    }
    printf("Unlock all chapters: %d chapter select and %d Brutal flags set\n",chapters,brutal);
}
/* Idle job workers sleep (all three games). From BilalReloaded's Uncharted port
 * (github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port, GPL-2.0-or-later,
 * tools/probe_uncharted.py JOB_SLEEP_PATCHES, made for CUSA02344 1.02), with the EU release's
 * encodings (CUSA02343 1.02: other registers, short jumps) added. The engine's six job worker
 * threads spin on their queues unless the job manager's sleep flag (byte +0x59, the
 * engine's own SetSleepMode) is set; with it, an idle worker waits on the manager's condition
 * variable after 1000 spins (10 ms timeout) and each job submission wakes them. On the PS4 every
 * worker owns a core and the flag stays off; on a PC the spinning workers take cores from the
 * GPU command thread. The flag is treated as set in the worker loop, its re-check before
 * waiting and the submission path: all three or none. BB_JOB_SLEEP=0 keeps the original. */
struct masked_pattern { const char *hex; int at; int len; const unsigned char *with; };
static int64_t find_masked(const char *hex, uint64_t size) {
    unsigned char bytes[64], mask[64];
    int n=0;
    for (const char *p=hex; *p && n<64;) {
        if (*p==' ') { ++p; continue; }
        if (p[0]=='?') { bytes[n]=0; mask[n++]=0; p+=2; continue; }
        unsigned v; sscanf(p,"%2x",&v); bytes[n]=(unsigned char)v; mask[n++]=1; p+=2;
    }
    int64_t found=-1;
    for (uint64_t i=0; i+n<=size; ++i) {
        if (image[i]!=bytes[0]) continue;
        int k=1;
        while (k<n && (!mask[k] || image[i+k]==bytes[k])) ++k;
        if (k<n) continue;
        if (found>=0) return -2; /* not unique */
        found=(int64_t)i;
    }
    return found;
}
/* Sticky wake-ups (docs/investigations/sticky_wakeups.md). An idle worker checks the job queues,
 * then takes the job manager's mutex and waits on its condition variable; a job submitted between
 * the check and the wait signalled nobody, and the worker slept the whole timeout with work queued
 * (a frame's work a frame late: U3's flashlight light missing for a frame). Both the submission's
 * signal and the worker's wait run with the manager's mutex held: the signal now also sets a flag,
 * and a worker about to wait while it is set clears it and returns at once instead (a spurious
 * wake-up, which the worker loop allows: it looks at the queues again). */
static volatile unsigned char job_signal_pending;
static int install_job_sticky(int64_t recheck, int64_t submission) {
    /* The wait: the first call after the re-check (mov rsi, rax; call at +0x19). The signal: EU, the
     * call after "add rsi, 0x60" (rsi = the mutex at +0x60, rdi = the condition at +0x80), +0x22;
     * US 1.02 (as CUSA02344), the call after "sub rdi, -0x80" (rdi = the condition), +0x9c, past the
     * lock's inlined error checks. */
    int64_t wait_call=-1, signal_call=-1;
    for (int64_t o=recheck+6;o<recheck+48;++o) if (image[o]==0xe8) { wait_call=o; break; }
    for (int64_t o=submission+6;o<submission+0x100;++o)
        if (!memcmp(image+o,"\x48\x83\xc6\x60\xe8",5) || !memcmp(image+o,"\x48\x83\xef\x80\xe8",5)) { signal_call=o+4; break; }
    if (wait_call<0 || signal_call<0) {
        printf("Job workers: sticky wake-ups not installed (%s call not found)\n",wait_call<0 ? "wait" : "signal");
        return 0;
    }
    unsigned char *t=near_image_memory(64);
    if (!t) { printf("Job workers: sticky wake-ups not installed (no thunk memory)\n"); return 0; }
    int32_t rel;
    memcpy(&rel,image+wait_call+1,4);
    const uint64_t wait_fn=(uint64_t)(uintptr_t)(image+wait_call+5+rel);
    memcpy(&rel,image+signal_call+1,4);
    const uint64_t signal_fn=(uint64_t)(uintptr_t)(image+signal_call+5+rel);
    const uint64_t flag=(uint64_t)(uintptr_t)&job_signal_pending;
    size_t k=0;
    /* wait: mov rax, &flag; xor ecx, ecx; xchg [rax], cl; test cl, cl; jz +3; xor eax, eax; ret;
     * jmp [rip] wait */
    unsigned char *wait_stub=t+k;
    t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&flag,8); k+=8;
    t[k++]=0x31; t[k++]=0xc9; t[k++]=0x86; t[k++]=0x08; t[k++]=0x84; t[k++]=0xc9;
    t[k++]=0x74; t[k++]=0x03; t[k++]=0x31; t[k++]=0xc0; t[k++]=0xc3;
    t[k++]=0xff; t[k++]=0x25; memset(t+k,0,4); k+=4; memcpy(t+k,&wait_fn,8); k+=8;
    /* signal: mov rax, &flag; mov byte [rax], 1; jmp [rip] signal */
    unsigned char *signal_stub=t+k;
    t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&flag,8); k+=8;
    t[k++]=0xc6; t[k++]=0x00; t[k++]=0x01;
    t[k++]=0xff; t[k++]=0x25; memset(t+k,0,4); k+=4; memcpy(t+k,&signal_fn,8); k+=8;
    rel=(int32_t)((int64_t)(uintptr_t)wait_stub-(int64_t)(uintptr_t)(image+wait_call+5));
    memcpy(image+wait_call+1,&rel,4);
    rel=(int32_t)((int64_t)(uintptr_t)signal_stub-(int64_t)(uintptr_t)(image+signal_call+5));
    memcpy(image+signal_call+1,&rel,4);
    printf("Job workers: sticky wake-ups (wait call 0x%" PRIx64 ", signal call 0x%" PRIx64 ")\n",
           (uint64_t)wait_call,(uint64_t)signal_call);
    return 1;
}
static void patch_job_sleep(uint64_t image_size) {
    const char *env=getenv("BB_JOB_SLEEP");
    if (env && env[0]=='0') { printf("Job workers: idle ones spin (BB_JOB_SLEEP=0)\n"); return; }
    /* Sleeping workers make the camera judder: U1-U3 run their camera and pad work in jobs, and
     * the main thread waits for each job phase, so a frame's work starts late by a varying
     * amount and the game, which times frames with the real clock, turns the camera unevenly per
     * shown frame (a steady turn: every ~4th frame a third of the others; measured 2026-10-24
     * with BB_CAMERA_LOG; mouse or stick alike). They spin unless the CPU is small: BB_JOB_SLEEP
     * unset or "auto" sleeps up to 8 hardware threads (as SuperMedo's 0.3.7), 1 always. Spinning
     * costs about 5 cores and 3-5% FPS on 16 threads (U3 square 120 -> 114-117). */
    if (!env || !*env || !strcmp(env,"auto")) {
        long threads=0;
#ifdef _WIN32
        DWORD_PTR mask=0, system_mask=0;
        if (GetProcessAffinityMask(GetCurrentProcess(),&mask,&system_mask)) threads=__builtin_popcountll((unsigned long long)mask);
#else
        threads=sysconf(_SC_NPROCESSORS_ONLN);
#endif
        if (threads>8) {
            printf("Job workers: idle ones spin (%ld hardware threads; BB_JOB_SLEEP=1 sleeps)\n",threads);
            return;
        }
    }
    static const unsigned char nop2[]={0x66,0x90}, nop6[]={0x66,0x0f,0x1f,0x44,0x00,0x00};
    /* Each site: alternatives (EU CUSA02343, CUSA02344); the first one found exactly once is used. */
    static const struct masked_pattern sites[3][2]={
        {{"80 7f 59 00 74 ?? 49 81 f8 e8 03 00 00",4,2,nop2}, {0}},                   /* worker loop */
        {{"80 78 59 00 74 ?? 48 89 c7 48 83 ef 80",4,2,nop2}, {0}},                   /* re-check */
        {{"80 7f 59 00 74 ?? 48 83 c7 60 be ff ff ff ff e8",4,2,nop2},                 /* submission, EU */
         {"80 78 59 00 0f 84 ?? ?? 00 00 48 83 c0 60",4,6,nop6}},                      /* submission, CUSA02344 */
    };
    int64_t at[3];
    const struct masked_pattern *use[3];
    for (int s=0; s<3; ++s) {
        at[s]=-1;
        for (int a=0; a<2 && sites[s][a].hex && at[s]<0; ++a) {
            const int64_t f=find_masked(sites[s][a].hex,image_size);
            if (f>=0) { at[s]=f; use[s]=&sites[s][a]; }
        }
        if (at[s]<0) { printf("Job workers: site %d not found, they keep spinning\n",s); return; }
    }
    for (int s=0; s<3; ++s) memcpy(image+at[s]+use[s]->at,use[s]->with,(size_t)use[s]->len);
    /* The worker then waits up to 10 ms without looking at the queue again: a job submitted after
     * its last look (the wake-up already given) waited for the timeout, and a frame's work finished
     * a frame late (U3's mine: the flashlight's light missing for a frame). The wait after the
     * re-check (mov edx, 10) is shortened to BB_JOB_SLEEP_MS (default 1, 10 the game's). */
    /* The wake-up is not lost instead (install_job_sticky below), so the worker can keep the game's
     * 1000 spins and 10 ms wait. BB_JOB_STICKY=0: off (the 50000 spins and 1 ms waits). */
    const char *sticky_env=getenv("BB_JOB_STICKY");
    const int sticky=!(sticky_env && sticky_env[0]=='0') && install_job_sticky(at[1],at[2]);
    const char *ms_env=getenv("BB_JOB_SLEEP_MS");
    const int ms=ms_env ? atoi(ms_env) : sticky ? 10 : 1;
    /* EU: mov edx, 10 (milliseconds); US 1.02 (as CUSA02344): mov edx, 10000 (microseconds, another
     * timed wait). */
    unsigned char *wait=image+at[1]+17;
    int32_t wait_value=0;
    if (wait[0]==0xba) memcpy(&wait_value,wait+1,4);
    const int is_wait=wait_value==10, is_wait_us=wait_value==10000;
    if (is_wait && ms>0 && ms<128) wait[1]=(unsigned char)ms;
    if (is_wait_us && ms>0 && ms<128) { const int32_t us=ms*1000; memcpy(wait+1,&us,4); }
    /* A sleeping worker starts a job only once the host has woken it: work a frame does not wait for
     * could then miss it. BB_JOB_SLEEP_SPINS (default 50000, the game's 1000: cmp r8, 1000) keeps a
     * worker spinning through a frame's bursts of jobs; it sleeps when the game is idle. */
    const char *spin_env=getenv("BB_JOB_SLEEP_SPINS");
    const long spins=spin_env ? atol(spin_env) : sticky ? 1000 : 50000;
    unsigned char *spin=image+at[0]+6;
    int32_t old_spins=0;
    memcpy(&old_spins,spin+3,4);
    const int is_spin=spin[0]==0x49 && spin[1]==0x81 && spin[2]==0xf8 && old_spins==1000;
    if (is_spin && spins>0 && spins<=0x7fffffff) { const int32_t v=(int32_t)spins; memcpy(spin+3,&v,4); }
    int32_t now_spins=-1;
    if (is_spin) memcpy(&now_spins,spin+3,4);
    printf("Job workers: idle ones sleep (0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 "), wait %d ms after %d spins\n",
           at[0],at[1],at[2],is_wait ? wait[1] : is_wait_us ? (int)(((uint32_t)wait[1]|(uint32_t)wait[2]<<8)/1000) : -1,
           (int)now_spins);
}
/* The job system's counter wait (corelib job scheduling: until [counter+0x20] == or <= a target):
 * a job worker that finds the counter not there yet suspends its job (SuspendJob) and retries when
 * the job runs again, which may be on another worker; other threads sleep 1 ms and retry in place.
 * On the PS4 each worker owns a core and the counter is there in time. With 4 cores the jobs it
 * waits for lag: U2's water Draw waits with a render command context open, resumed on another
 * worker and failed
 * "m_workerThreadIndex == GetCurrentWorkerThreadIndex()" (End) and "cmdBuf.m_bInUse == false"
 * (Begin, on the worker it left) within 1-7 deaths of a 4-core death loop. A worker with a render
 * context open (the Begin/End hooks above) now first retries in place (SwitchToThread, then 1 ms
 * sleeps, up to BB_JOB_LOCK_WAIT_MS, default 250; 0: off) and suspends its job only after that, as
 * before (the jobs it waits for still get a free worker); other workers suspend at once. The
 * counter is read again on every try. */
static uint64_t job_lock_try;
static unsigned job_lock_wait_ms=250;
static volatile long job_lock_fallbacks;
static ABI int job_lock_wait(uint64_t counter, uint64_t a, uint32_t b) {
    typedef int (ABI *Try)(uint64_t, uint64_t, uint32_t);
    const Try try_value=(Try)(uintptr_t)job_lock_try;
    const volatile uint64_t *value=(const volatile uint64_t *)(uintptr_t)(counter+0x20);
#define try_lock(c,x,y) try_value(*value,(x),(y))
#ifdef _WIN32
    /* Without a render context open the job may move: suspend at once, as the game does (waiting
     * in place for every taken lock starved the job system: 3.6 fps loading with 4 cores). */
    if (!runtime_ndgi_open) return 0;
    for (unsigned i=0;i<16;++i) {
        if (try_lock(counter,a,b)&0xff) return 1;
        SwitchToThread();
    }
    for (unsigned ms=0;ms<job_lock_wait_ms;++ms) {
        if (try_lock(counter,a,b)&0xff) return 1;
        Sleep(1);
    }
#else
    for (unsigned i=0;i<job_lock_wait_ms*10;++i) {
        if (try_lock(counter,a,b)&0xff) return 1;
        usleep(100);
    }
#endif
    if (try_lock(counter,a,b)&0xff) return 1;
    const long n=__atomic_add_fetch(&job_lock_fallbacks,1,__ATOMIC_RELAXED);
    if (n<=20) printf("Job wait: a worker with a render context open waited %u ms in vain, its job suspends (%ld)\n",
                      job_lock_wait_ms,n);
    return 0;
#undef try_lock
}
/* Uncharted 1 (EU and US 1.02), diagnosis: BB_SLEEP_LOG=1 logs every rigid body the physics engine puts to sleep
 * (the 5-byte setter `or byte [rdi+0xb],8; ret` that follows the clearer `and byte [rdi+0xb],0xf7; ret`; EU
 * 0x1da900, US 0x1dae70). Each second: how many went to sleep, by caller (guest image offset of the return address),
 * and the bodies first put to sleep in that second. For the jet ski barrels that freeze in mid-air above 60 FPS. */
static volatile long sleep_log_calls;
static struct { uint64_t caller; long count; int kind; } sleep_log_callers[16];
static uint64_t sleep_log_bodies[64];
static long sleep_log_nbodies;
static SRWLOCK sleep_log_lock=SRWLOCK_INIT;
/* kind: 0 the body setter, 1 the island node helper (node and its parents), 2 the island subtree helper. */
static ABI void sleep_log(uint64_t body, uint64_t caller, uint64_t kind) {
    AcquireSRWLockExclusive(&sleep_log_lock);
    ++sleep_log_calls;
    const uint64_t at=caller-(uint64_t)(uintptr_t)image;
    int k=0;
    while (k<16 && sleep_log_callers[k].count && (sleep_log_callers[k].caller!=at || sleep_log_callers[k].kind!=(int)kind)) ++k;
    if (k<16) { sleep_log_callers[k].caller=at; sleep_log_callers[k].kind=(int)kind; ++sleep_log_callers[k].count; }
    if (sleep_log_nbodies<64) sleep_log_bodies[sleep_log_nbodies++]=body;
    static ULONGLONG window;
    const ULONGLONG now=GetTickCount64();
    if (!window) window=now;
    if (now-window>=1000) {
        printf("Sleep log: %ld bodies put to sleep in %.1f s; by caller:",sleep_log_calls,(now-window)/1000.0);
        for (int i=0; i<16 && sleep_log_callers[i].count; ++i)
            printf(" %s@%#" PRIx64 " x%ld",sleep_log_callers[i].kind==0 ? "body" : sleep_log_callers[i].kind==1 ? "node" : "subtree",
                   sleep_log_callers[i].caller,sleep_log_callers[i].count);
        printf("\nSleep log: bodies:");
        for (long i=0; i<sleep_log_nbodies; ++i) printf(" %#" PRIx64,sleep_log_bodies[i]);
        printf("\n");
        fflush(stdout);
        sleep_log_calls=0; sleep_log_nbodies=0; memset(sleep_log_callers,0,sizeof(sleep_log_callers));
        window=now;
    }
    ReleaseSRWLockExclusive(&sleep_log_lock);
}
/* Entry hook: the first 5 bytes at p (whole instructions, no rip-relative or branch) move into a thunk that logs
 * (rdi, the return address, kind) and then runs them and jumps back to p+5. */
static int sleep_log_hook(unsigned char *p, int kind) {
    unsigned char *t=near_image_memory(128);
    if (!t) return 0;
    size_t k=0;
    static const unsigned char save[]={0x57,0x56,0x50,0x51,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53}; /* push rdi rsi rax rcx rdx r8-r11 */
    memcpy(t+k,save,sizeof(save)); k+=sizeof(save);
    static const unsigned char ret_addr[]={0x48,0x8b,0x74,0x24,0x48}; /* mov rsi,[rsp+0x48]: the caller (9 pushes) */
    memcpy(t+k,ret_addr,sizeof(ret_addr)); k+=sizeof(ret_addr);
    t[k++]=0xba; memcpy(t+k,&kind,4); k+=4;              /* mov edx, kind */
    const uintptr_t host=(uintptr_t)sleep_log;
    t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&host,8); k+=8; /* movabs rax, sleep_log */
    t[k++]=0xff; t[k++]=0xd0;                            /* call rax (rsp is 16-byte aligned here) */
    static const unsigned char restore[]={0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5a,0x59,0x58,0x5e,0x5f};
    memcpy(t+k,restore,sizeof(restore)); k+=sizeof(restore);
    memcpy(t+k,p,5); k+=5;                               /* the moved instructions */
    const uint64_t back=(uint64_t)(uintptr_t)(p+5);
    t[k++]=0xff; t[k++]=0x25; memset(t+k,0,4); k+=4; memcpy(t+k,&back,8); k+=8; /* jmp [rip]: p+5 */
    const int32_t to=(int32_t)((int64_t)(uintptr_t)t-(int64_t)(uintptr_t)(p+5));
    p[0]=0xe9; memcpy(p+1,&to,4);
    return 1;
}
/* The physics world's body list (BB_SLEEP_LOG=1): AddBody(world, body, params) and RemoveBody(world, body). The list
 * at world+0x5b80 holds at most 512 bodies (count at world+0x5b74); AddBody leaves a body out when it is full (it marks
 * it with index 0x200 at body+0x134), and a body left out is never simulated (it hangs where it is). Each second:
 * adds, adds refused because the list was full, removes, and the count. */
static volatile long body_log_adds, body_log_full, body_log_removes;
static volatile uint64_t body_log_world;
static volatile long body_log_max;
/* kind 5: an object's "put all my bodies to sleep" (rdi = the object, not the world): only its caller is counted. */
static struct { uint64_t caller; long count; } sleepall_callers[16];
static SRWLOCK sleepall_lock=SRWLOCK_INIT;
/* kind 7: the floating controller (EU 0x22cad0, rsi = the controller, its body at +0x30): the bodies it handled. */
static uint64_t ctrl_bodies[128];
static long ctrl_nbodies;
/* kinds 8/9: a process creates / destroys its physics body (rdi = the process, e.g. a FloatingBarrel). */
static uint64_t pb_create[64], pb_destroy[64];
static long pb_ncreate, pb_ndestroy;
/* Per-process timeline: every process that creates a physics body, sampled each second; a line is printed when its
 * state changes (body, world list, asleep, held, controller, destroys) or it moves. */
typedef struct {
    uint64_t proc, body, destroy_caller, destroy_caller2, vt0;
    long vlog, dumped;
    ULONGLONG created_at, ctrl_at;
    long creates, destroys;
    char last[160];
} BarrelTrack;
static BarrelTrack barrels[128];
static long nbarrels;
static BarrelTrack *barrel_find(uint64_t proc, int add) {
    for (long i=0; i<nbarrels; ++i) if (barrels[i].proc==proc) return &barrels[i];
    if (!add || nbarrels>=128) return NULL;
    BarrelTrack *b=&barrels[nbarrels++];
    memset(b,0,sizeof(*b));
    b->proc=proc;
    return b;
}
static uint64_t safe_read64(uint64_t at) {
    uint64_t v=0;
    SIZE_T got=0;
    if (!ReadProcessMemory(GetCurrentProcess(),(LPCVOID)(uintptr_t)at,&v,8,&got) || got!=8) return 0;
    return v;
}
static ULONGLONG barrel_t0;
static const volatile float *barrel_dt; /* the physics step (EU 0x9887a4), found from the controller */
static void barrel_report(void) {
    static volatile LONG64 window;
    const LONG64 now=(LONG64)GetTickCount64(), w=window;
    if (!w) { InterlockedCompareExchange64(&window,now,0); return; }
    if (now-w<1000 || InterlockedCompareExchange64(&window,now,w)!=w) return;
    if (!barrel_t0) barrel_t0=(ULONGLONG)now;
    const uint64_t world=body_log_world;
    AcquireSRWLockExclusive(&sleepall_lock);
    for (long i=0; i<nbarrels; ++i) {
        BarrelTrack *b=&barrels[i];
        const uint64_t p=b->proc;
        const uint64_t body=*(volatile uint64_t *)(uintptr_t)(p+0x1e0), coll=*(volatile uint64_t *)(uintptr_t)(p+0x1e8);
        if (*(volatile uint64_t *)(uintptr_t)p!=b->vt0) continue; /* freed or reused: not this barrel any more */
        const uint64_t vptr=*(volatile uint64_t *)(uintptr_t)p-(uint64_t)(uintptr_t)image;
        const unsigned flags98=*(volatile unsigned *)(uintptr_t)(p+0x98);
        const float y=*(volatile float *)(uintptr_t)(p+0x1c4);
        int inworld=-1, asleep=-1;
        unsigned fc=0, still=0, still2=0, idx=0;
        float invm=0;
        if (body) {
            idx=*(volatile uint16_t *)(uintptr_t)(body+0x134);
            if (world) inworld=idx<*(volatile uint32_t *)(uintptr_t)(world+0x5b74) &&
                               *(volatile uint64_t *)(uintptr_t)(world+0x5b80+8*(uint64_t)idx)==body;
            asleep=(*(volatile uint8_t *)(uintptr_t)(body+0xb)&8)!=0;
            fc=*(volatile uint32_t *)(uintptr_t)(body+0xfc);
            invm=*(volatile float *)(uintptr_t)(body+0xec);
            still=*(volatile uint16_t *)(uintptr_t)(body+0x136);
            still2=*(volatile uint16_t *)(uintptr_t)(body+0x138);
        }
        const int ctrl_recent=b->ctrl_at && (ULONGLONG)now-b->ctrl_at<1500;
        char line[160];
        snprintf(line,sizeof(line),"vt %#" PRIx64 " body %#" PRIx64 " coll %s held %d world %d asleep %d fc %#x invm %.3g ctrl %s c%ld d%ld",
                 vptr,body,coll ? "yes" : "no",(flags98>>3)&1,inworld,asleep,fc,invm,
                 ctrl_recent ? "now" : b->ctrl_at ? "earlier" : "never",b->creates,b->destroys);
        static float lasty[128];
        if (strcmp(line,b->last) || (y-lasty[i]>0.5f || lasty[i]-y>0.5f)) {
            printf("Barrel %#" PRIx64 " t=%.1f y=%.2f %s still %u/%u idx %u",p,(now-(LONG64)barrel_t0)/1000.0,y,line,still,still2,idx);
            if (b->destroys) printf(" destroyed by %#" PRIx64 " <- %#" PRIx64,b->destroy_caller,b->destroy_caller2);
            printf("\n");
            memcpy(b->last,line,sizeof(line));
            lasty[i]=y;
        }
    }
    ReleaseSRWLockExclusive(&sleepall_lock);
    fflush(stdout);
}
static ABI void body_log(uint64_t world, uint64_t body, uint64_t caller, uint64_t kind, uint64_t frame) {
    if (kind==8 || kind==9) {
        AcquireSRWLockExclusive(&sleepall_lock);
        if (kind==8 && pb_ncreate<64) pb_create[pb_ncreate++]=world;
        if (kind==9 && pb_ndestroy<64) pb_destroy[pb_ndestroy++]=world;
        BarrelTrack *b=barrel_find(world,kind==8);
        if (b && kind==8) { ++b->creates; b->created_at=GetTickCount64(); b->vt0=*(volatile uint64_t *)(uintptr_t)world; b->vlog=0; b->dumped=0; }
        if (b && kind==9 && *(volatile uint64_t *)(uintptr_t)(world+0x1e0)) {
            ++b->destroys;
            b->destroy_caller=caller-(uint64_t)(uintptr_t)image;
            b->destroy_caller2=safe_read64(frame+8)-(uint64_t)(uintptr_t)image;
        }
        ReleaseSRWLockExclusive(&sleepall_lock);
        barrel_report();
        return;
    }
    if (kind==7) {
        const uint64_t b=*(volatile uint64_t *)(uintptr_t)(body+0x30);
        AcquireSRWLockExclusive(&sleepall_lock);
        long i=0;
        while (i<ctrl_nbodies && ctrl_bodies[i]!=b) ++i;
        if (i==ctrl_nbodies && ctrl_nbodies<128) ctrl_bodies[ctrl_nbodies++]=b;
        for (long j=0; j<nbarrels; ++j)
            if (*(volatile uint64_t *)(uintptr_t)(barrels[j].proc+0x1e0)==b) barrels[j].ctrl_at=GetTickCount64();
        static int dumped;
        if (!dumped) {
            dumped=1;
            printf("Body log: first floating controller %#" PRIx64 ":",body);
            for (int k=0; k<0x60; k+=8) printf(" %#" PRIx64,*(volatile uint64_t *)(uintptr_t)(body+k));
            printf("\n");
        }
        ReleaseSRWLockExclusive(&sleepall_lock);
        barrel_report();
    }
    if (kind==10) { /* a FloatingBarrel PrePhys state gets a message (rdi = the state, [rdi+8] = the barrel) */
        if (*(volatile uint32_t *)(uintptr_t)body!=0x971e51ca) return;
        const uint64_t proc=*(volatile uint64_t *)(uintptr_t)(world+8), base=(uint64_t)(uintptr_t)image;
        printf("Barrel release: %#" PRIx64 " y=%.2f t=%.1f from %#" PRIx64,proc,*(volatile float *)(uintptr_t)(proc+0x1c4),
               barrel_t0 ? (GetTickCount64()-barrel_t0)/1000.0 : 0.0,caller-base);
        uint64_t f=frame;
        for (int i=0; i<8 && f; ++i) {
            const uint64_t r=safe_read64(f+8);
            if (!r) break;
            printf(" <- %#" PRIx64,r-base);
            f=safe_read64(f);
        }
        printf("\n");
        fflush(stdout);
        return;
    }
    if (kind==11) { /* a barrel's motion update (rdi = its state, [rdi+8] = the barrel): its body's first 20 frames */
        const uint64_t proc=*(volatile uint64_t *)(uintptr_t)(world+8);
        const uint64_t bd=*(volatile uint64_t *)(uintptr_t)(proc+0x1e0);
        if (!bd) return;
        AcquireSRWLockExclusive(&sleepall_lock);
        BarrelTrack *b=barrel_find(proc,0);
        if (b && b->dumped<20 && *(volatile uint64_t *)(uintptr_t)proc==b->vt0) {
            const int n=b->dumped++;
            printf("Barrel frame: %#" PRIx64 " #%d +%llums y=%.4f dt=%.5f body",proc,n,
                   (unsigned long long)(GetTickCount64()-b->created_at),*(volatile float *)(uintptr_t)(proc+0x1c4),
                   barrel_dt ? *barrel_dt : 0.0f);
            for (int k=0; k<0x158; k+=4) printf(" %08x",*(volatile uint32_t *)(uintptr_t)(bd+k));
            printf("\n");
        }
        ReleaseSRWLockExclusive(&sleepall_lock);
        return;
    }
    if (kind==6) { /* velocity set on a barrel's body in its first 3 s: rsi = the vector */
        AcquireSRWLockExclusive(&sleepall_lock);
        const ULONGLONG now=GetTickCount64();
        for (long j=0; j<nbarrels; ++j) {
            BarrelTrack *b=&barrels[j];
            if (*(volatile uint64_t *)(uintptr_t)(b->proc+0x1e0)!=world || now-b->created_at>3000 || b->vlog>=60) continue;
            ++b->vlog;
            const volatile float *v=(const volatile float *)(uintptr_t)body;
            printf("Barrel vel: %#" PRIx64 " +%llums y=%.3f v=(%.3f %.3f %.3f) dt=%.5f from %#" PRIx64 "\n",b->proc,
                   (unsigned long long)(now-b->created_at),*(volatile float *)(uintptr_t)(b->proc+0x1c4),v[0],v[1],v[2],
                   barrel_dt ? *barrel_dt : 0.0f,caller-(uint64_t)(uintptr_t)image);
        }
        ReleaseSRWLockExclusive(&sleepall_lock);
    }
    if (kind>=5) { /* 5: SleepAll(object), 6: a body's velocity/force set (flags bit 1, wakes it), 7: the controller */
        AcquireSRWLockExclusive(&sleepall_lock);
        const uint64_t at=(caller-(uint64_t)(uintptr_t)image) | (kind<<60);
        int k=0;
        while (k<16 && sleepall_callers[k].count && sleepall_callers[k].caller!=at) ++k;
        if (k<16) { sleepall_callers[k].caller=at; ++sleepall_callers[k].count; }
        ReleaseSRWLockExclusive(&sleepall_lock);
        return;
    }
    body_log_world=world;
    const uint32_t count=*(volatile uint32_t *)(uintptr_t)(world+0x5b74);
    if (kind==3) { InterlockedIncrement(&body_log_adds); if (count>=0x200) InterlockedIncrement(&body_log_full); }
    else InterlockedIncrement(&body_log_removes);
    if ((long)count>body_log_max) body_log_max=(long)count;
    static volatile LONG64 window;
    const LONG64 now=(LONG64)GetTickCount64(), w=window;
    if (!w) { InterlockedCompareExchange64(&window,now,0); return; }
    if (now-w<1000 || InterlockedCompareExchange64(&window,now,w)!=w) return;
    printf("Body log: %ld adds (%ld refused: world full), %ld removes in %.1f s; bodies in the world now %u (max %ld of 512)\n",
           InterlockedExchange(&body_log_adds,0),InterlockedExchange(&body_log_full,0),InterlockedExchange(&body_log_removes,0),
           (now-w)/1000.0,count,body_log_max);
    body_log_max=0;
    AcquireSRWLockExclusive(&sleepall_lock);
    if (sleepall_callers[0].count) {
        printf("Body log: SleepAll / velocity set, by caller:");
        for (int i=0; i<16 && sleepall_callers[i].count; ++i)
            printf(" %s@%#" PRIx64 " x%ld",(sleepall_callers[i].caller>>60)==5 ? "sleepall" : "velocity",
                   sleepall_callers[i].caller & ((1ull<<60)-1),sleepall_callers[i].count);
        printf("\n");
        memset(sleepall_callers,0,sizeof(sleepall_callers));
    }
    if (ctrl_nbodies) {
        printf("Body log: floating controller bodies (%ld):",ctrl_nbodies);
        for (long i=0; i<ctrl_nbodies; ++i) printf(" %#" PRIx64,ctrl_bodies[i]);
        printf("\n");
        ctrl_nbodies=0;
    }
    if (pb_ncreate || pb_ndestroy) {
        printf("Body log: physics body created for:");
        for (long i=0; i<pb_ncreate; ++i) printf(" %#" PRIx64,pb_create[i]);
        printf("; destroyed for:");
        for (long i=0; i<pb_ndestroy; ++i) printf(" %#" PRIx64,pb_destroy[i]);
        printf("\n");
        pb_ncreate=pb_ndestroy=0;
    }
    ReleaseSRWLockExclusive(&sleepall_lock);
    fflush(stdout);
}
/* Entry hook moving n whole instructions (no rip-relative, no branch) that calls body_log(rdi, rsi, return, kind). */
static int body_log_hook(unsigned char *p, int n, int kind) {
    unsigned char *t=near_image_memory(128);
    if (!t) return 0;
    size_t k=0;
    static const unsigned char save[]={0x57,0x56,0x50,0x51,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53}; /* push rdi rsi rax rcx rdx r8-r11 */
    memcpy(t+k,save,sizeof(save)); k+=sizeof(save);
    static const unsigned char ret_addr[]={0x48,0x8b,0x54,0x24,0x48}; /* mov rdx,[rsp+0x48]: the caller (9 pushes) */
    memcpy(t+k,ret_addr,sizeof(ret_addr)); k+=sizeof(ret_addr);
    t[k++]=0xb9; memcpy(t+k,&kind,4); k+=4;              /* mov ecx, kind */
    t[k++]=0x49; t[k++]=0x89; t[k++]=0xe8;              /* mov r8, rbp: the caller's frame */
    const uintptr_t host=(uintptr_t)body_log;
    t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&host,8); k+=8; /* movabs rax, body_log */
    t[k++]=0xff; t[k++]=0xd0;                            /* call rax */
    static const unsigned char restore[]={0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5a,0x59,0x58,0x5e,0x5f};
    memcpy(t+k,restore,sizeof(restore)); k+=sizeof(restore);
    memcpy(t+k,p,(size_t)n); k+=(size_t)n;               /* the moved instructions */
    const uint64_t back=(uint64_t)(uintptr_t)(p+n);
    t[k++]=0xff; t[k++]=0x25; memset(t+k,0,4); k+=4; memcpy(t+k,&back,8); k+=8; /* jmp [rip]: p+n */
    const int32_t to=(int32_t)((int64_t)(uintptr_t)t-(int64_t)(uintptr_t)(p+5));
    p[0]=0xe9; memcpy(p+1,&to,4);
    for (int i=5; i<n; ++i) p[i]=0x90;
    return 1;
}
/* Uncharted 1 (EU and US 1.02), experiment: BB_U1_PHYS_HZ=60 steps the rigid-body world at a fixed rate whatever the
 * frame rate. On frames shorter than a step the world step (EU 0x235350, dt in xmm0) returns at once as if it had
 * stepped (its own dt <= 1e-6 path would still run the controllers, the river current among them, with the last dt)
 * and the time is carried, so the world and its controllers see 1/60 s steps as on the PS4. Above 60 FPS some
 * floating barrels (jet ski river) otherwise stay frozen in mid-air until they fall asleep. */
static float u1_phys_step, u1_phys_acc;
static unsigned char u1_phys_skip;
static ABI float u1_phys_dt(float dt) {
    u1_phys_skip=0;
    if (!(dt>0.0f) || dt>0.25f) return dt;
    u1_phys_acc+=dt;
    if (u1_phys_acc<u1_phys_step*0.999f) { u1_phys_skip=1; return dt; }
    const float out=u1_phys_acc>=2.0f*u1_phys_step ? u1_phys_acc : u1_phys_step;
    u1_phys_acc-=out;
    if (u1_phys_acc<0.0f) u1_phys_acc=0.0f;
    return out;
}
static void patch_u1_fixed_physics(uint64_t image_size) {
    const char *hz=getenv("BB_U1_PHYS_HZ");
    if (!hz || atoi(hz)<=0) return;
    /* push rbp; mov rbp,rsp; push r14 | push rbx; sub rsp,0x10; mov r14,rsi; vmovss [rbp-0x14],xmm0; mov rbx,rdi;
     * mov dword [rbx+0x14eb4],0 */
    const int64_t at=find_masked("554889e5 4156 53 4883ec10 4989f6 c5fa1145ec 4889fb c783b44e010000000000",image_size);
    if (at<0) { printf("U1 fixed physics step: world step not found\n"); return; }
    unsigned char *p=image+at, *t=near_image_memory(128);
    if (!t) return;
    u1_phys_step=1.0f/(float)atoi(hz);
    size_t k=0;
    static const unsigned char save[]={0x57,0x56,0x50,0x51,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53};
    memcpy(t+k,save,sizeof(save)); k+=sizeof(save);
    const uintptr_t host=(uintptr_t)u1_phys_dt;
    t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&host,8); k+=8; /* movabs rax, u1_phys_dt */
    t[k++]=0xff; t[k++]=0xd0;                            /* call rax: xmm0 = u1_phys_dt(xmm0) */
    static const unsigned char restore[]={0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5a,0x59,0x58,0x5e,0x5f};
    memcpy(t+k,restore,sizeof(restore)); k+=sizeof(restore);
    const uintptr_t skip=(uintptr_t)&u1_phys_skip;
    t[k++]=0x50;                                         /* push rax */
    t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&skip,8); k+=8; /* movabs rax, &u1_phys_skip */
    t[k++]=0x80; t[k++]=0x38; t[k++]=0x00;               /* cmp byte [rax],0 */
    t[k++]=0x58;                                         /* pop rax */
    t[k++]=0x74; t[k++]=0x06;                            /* je: step */
    t[k++]=0xb8; t[k++]=1; t[k++]=0; t[k++]=0; t[k++]=0; /* mov eax,1: "stepped" */
    t[k++]=0xc3;                                         /* ret */
    memcpy(t+k,p,6); k+=6;                               /* push rbp; mov rbp,rsp; push r14 */
    const uint64_t back=(uint64_t)(uintptr_t)(p+6);
    t[k++]=0xff; t[k++]=0x25; memset(t+k,0,4); k+=4; memcpy(t+k,&back,8); k+=8;
    const int32_t to=(int32_t)((int64_t)(uintptr_t)t-(int64_t)(uintptr_t)(p+5));
    p[0]=0xe9; memcpy(p+1,&to,4); p[5]=0x90;
    printf("U1 fixed physics step: %d Hz (world step %#" PRIx64 ")\n",atoi(hz),(uint64_t)at);
}
static void patch_u1_sleep_log(uint64_t image_size) {
    const char *on=getenv("BB_SLEEP_LOG");
    if (!on || on[0]!='1') return;
    /* AddBody: push rbp; mov rbp,rsp; push r15 | push r14; push rbx; push rax; mov r15,rdx; mov rbx,rsi; mov r14,rdi */
    const int64_t add=find_masked("554889e5 4157 4156 53 50 4989d7 4889f3 4989fe 4885db",image_size);
    /* RemoveBody: push rbp; mov rbp,rsp; push r15 | push r14; push r12; push rbx; mov r14,rsi; mov rbx,rdi */
    const int64_t rem=find_masked("554889e5 4157 4156 4154 53 4989f6 4889fb 4d85f6 74?? 66418b8634010000",image_size);
    /* an object's "all my bodies to sleep" (virtual): push rbp; mov rbp,rsp; push r14 | push rbx; mov r14,rdi;
     * mov eax,[r14+0x10]; test eax,eax; je; xor ebx,ebx (EU 0x1ec940, US 0x1eceb0) */
    const int64_t all=find_masked("554889e5 4156 53 4989fe 418b4610 85c0 74?? 31db",image_size);
    const int okA=add>=0 && body_log_hook(image+add,6,3), okR=rem>=0 && body_log_hook(image+rem,6,4),
              okS=all>=0 && body_log_hook(image+all,6,5);
    /* a body's velocity/force set: or byte [rdi+0xfc],1 (7 bytes) | mov eax,[rsi+8]; mov [rdi+0x108],eax (EU 0x1da9a0) */
    const int64_t vel=find_masked("808ffc00000001 8b4608 898708010000",image_size);
    const int okV=vel>=0 && body_log_hook(image+vel,7,6);
    /* the floating controller: push rbp; mov rbp,rsp; push r15 | push r14 r13 r12 rbx; sub rsp,0x438; mov r15,rsi; mov r13,rdi */
    const int64_t ctl=find_masked("554889e5 4157 4156 4155 4154 53 4881ec38040000 4989f7 4989fd",image_size);
    const int okC=ctl>=0 && body_log_hook(image+ctl,6,7);
    /* the controller's lea rax,[rip+dt] at +0x3e (EU 0x22cb0e -> 0x9887a4, the physics step) */
    if (ctl>=0 && image[ctl+0x3e]==0x48 && image[ctl+0x3f]==0x8d && image[ctl+0x40]==0x05) {
        int32_t d; memcpy(&d,image+ctl+0x41,4);
        barrel_dt=(const volatile float *)(image+ctl+0x45+d);
    }
    printf("Body log: floating controller hooked %s (%#" PRIx64 ")\n",okC ? "yes" : "NO",(uint64_t)ctl);
    /* a process's physics body: create (if [rdi+0x1e0] is empty) and the two destroy copies */
    const int64_t mk=find_masked("554889e5 4157 4156 53 50 4189d7 4989f6 4889fb 4883bbe001000000",image_size);
    const int okK=mk>=0 && body_log_hook(image+mk,6,8);
    static const unsigned char kill[]={0x55,0x48,0x89,0xe5,0x53,0x50,0x48,0x89,0xfb,0x48,0x8b,0xb3,0xe0,0x01,0x00,0x00,0x48,0x85,0xf6,0x74};
    int okD=0;
    for (uint64_t i=0; i+sizeof(kill)<=image_size && okD<2; ++i)
        if (!memcmp(image+i,kill,sizeof(kill))) okD+=body_log_hook(image+i,6,9);
    /* FloatingBarrel PrePhys message handler: cmp dword [rsi],0x971e51ca (6 bytes) | jne; mov rdi,[rdi+8]; mov rax,[rdi]; jmp [rax+0x18] */
    const int64_t msg=find_masked("813eca511e97 75?? 488b7f08 488b07 ff6018",image_size);
    const int okM=msg>=0 && body_log_hook(image+msg,6,10);
    printf("Body log: barrel release message hooked %s (%#" PRIx64 ")\n",okM ? "yes" : "NO",(uint64_t)msg);
    /* a physics process's motion update: push rbp; mov rbp,rsp; push r14 | push rbx; mov r14,rdi; mov rbx,[r14+8];
     * mov rsi,[rbx+0x1e0]; test rsi,rsi (EU 0x2a2d50) */
    static const unsigned char motion[]={0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x49,0x89,0xfe,0x49,0x8b,0x5e,0x08,0x48,0x8b,0xb3,0xe0,0x01,0x00,0x00,0x48,0x85,0xf6};
    int okF=0;
    const int64_t mot=0;
    for (uint64_t i=0; i+sizeof(motion)<=image_size && okF<4; ++i)
        if (!memcmp(image+i,motion,sizeof(motion))) okF+=body_log_hook(image+i,6,11);
    printf("Body log: motion update hooked %s (%#" PRIx64 ")\n",okF ? "yes" : "NO",(uint64_t)mot);
    printf("Body log: process body create hooked %s (%#" PRIx64 "), destroy hooked %d\n",okK ? "yes" : "NO",(uint64_t)mk,okD);
    printf("Body log: velocity setter hooked %s (%#" PRIx64 ")\n",okV ? "yes" : "NO",(uint64_t)vel);
    printf("Body log: hooked AddBody %s (%#" PRIx64 "), RemoveBody %s (%#" PRIx64 "), SleepAll %s (%#" PRIx64 ")\n",
           okA ? "yes" : "NO",(uint64_t)add,okR ? "yes" : "NO",(uint64_t)rem,okS ? "yes" : "NO",(uint64_t)all);
    const int64_t at=find_masked("80670bf7c3 6666666666662e0f1f840000000000 804f0b08c3",image_size);
    if (at<0) return; /* not Uncharted 1 (or not unique) */
    /* body setter: or byte [rdi+0xb],8; ret (5 bytes, moved whole: the thunk's copy returns) */
    const int ok0=sleep_log_hook(image+at+20,0);
    /* island node helper: mov al,[rdi+0xb]; test al,8 | jne ...; or al,8; mov [rdi+0xb],al */
    const int64_t node=find_masked("8a470b a808 75?? 0c08 88470b",image_size);
    const int ok1=node>=0 && sleep_log_hook(image+node,1);
    /* island subtree helper: push rbp; mov rbp,rsp; push rbx | push rax; mov rbx,rdi; or byte [rbx+0xb],8 */
    const int64_t sub=find_masked("554889e5 53 50 4889fb 804b0b08",image_size);
    const int ok2=sub>=0 && sleep_log_hook(image+sub,2);
    printf("Sleep log: hooked body setter %s (%#" PRIx64 "), node helper %s (%#" PRIx64 "), subtree helper %s (%#" PRIx64 ")\n",
           ok0 ? "yes" : "NO",(uint64_t)(at+20),ok1 ? "yes" : "NO",(uint64_t)node,ok2 ? "yes" : "NO",(uint64_t)sub);
}
/* US 1.02 (CUSA02320, as CUSA02344), all three games: the try is inlined (counter r15, target r14,
 * mode ebx) and the worker check is in al:
 *   mov rcx,[r15+0x20]; cmp ebx,1; jne; cmp rcx,r14; setle cl; jmp; test ebx,ebx; jne; cmp rcx,r14;
 *   sete cl; test cl,cl; jne done; test al,al; je not_a_worker; nop10; (suspend loop:) mov rdi,r15;
 *   mov rsi,r14; mov edx,ebx; call SuspendJob
 * The game keeps an uncalled copy of the try function, which job_lock_wait calls. */
static void patch_job_lock_wait_us(uint64_t image_size) {
    const int64_t at=find_masked("498b4f20 83fb01 75?? 4c39f1 0f9ec1 eb?? 85db 75?? 4c39f1 0f94c1 84c9 75?? 84c0 74?? "
                                 "662e0f1f840000000000 4c89ff 4c89f6 89da e8????????",image_size);
    const int64_t try_at=find_masked("83fa01 7507 4839f7 0f9ec0 c3 85d2 7507 4839f7 0f94c0 c3",image_size);
    if (at<0 || try_at<0) { printf("Job wait: pattern not found, not patched\n"); return; }
    unsigned char *p=image+at;
    job_lock_try=(uint64_t)(uintptr_t)(image+try_at);
    const uint64_t done=(uint64_t)(uintptr_t)(p+31+(int8_t)p[30]);
    const uint64_t not_worker=(uint64_t)(uintptr_t)(p+35+(int8_t)p[34]);
    const uint64_t suspend_loop=(uint64_t)(uintptr_t)(p+45);
    unsigned char *t=near_image_memory(4096);
    if (!t) { printf("Job wait: no thunk memory, not patched\n"); return; }
    size_t k=0;
    const uintptr_t host=(uintptr_t)job_lock_wait;
    t[k++]=0x84; t[k++]=0xc0; t[k++]=0x75; t[k++]=0x0e;                                   /* test al,al; jnz worker */
    t[k++]=0xff; t[k++]=0x25; memset(t+k,0,4); k+=4; memcpy(t+k,&not_worker,8); k+=8;    /* jmp [rip]: not a worker */
    static const unsigned char args[]={0x4c,0x89,0xff,0x4c,0x89,0xf6,0x89,0xda};        /* rdi=r15 (the counter), rsi=r14, edx=ebx */
    memcpy(t+k,args,sizeof(args)); k+=sizeof(args);
    t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&host,8); k+=8;   /* movabs rax, job_lock_wait */
    t[k++]=0xff; t[k++]=0xd0;                              /* call rax (rsp aligned as at the original call) */
    t[k++]=0x84; t[k++]=0xc0;                              /* test al,al */
    t[k++]=0x74; t[k++]=0x0e;                              /* jz suspend */
    t[k++]=0xff; t[k++]=0x25; memset(t+k,0,4); k+=4; memcpy(t+k,&done,8); k+=8;          /* jmp [rip]: there */
    t[k++]=0xff; t[k++]=0x25; memset(t+k,0,4); k+=4; memcpy(t+k,&suspend_loop,8); k+=8;  /* jmp [rip]: suspend as before */
    const int32_t to=(int32_t)((int64_t)(uintptr_t)t-(int64_t)(uintptr_t)(p+31+5));
    p[31]=0xe9; memcpy(p+32,&to,4);                        /* replaces test al,al; je */
    memset(p+36,0x90,9);
    printf("Job wait: workers with a render context open wait for a job counter in place for up to %u ms before suspending their job (0x%" PRIx64 ", US)\n",
           job_lock_wait_ms,(uint64_t)at);
}
static void patch_job_lock_wait(uint64_t image_size) {
    const char *env=getenv("BB_JOB_LOCK_WAIT_MS");
    if (env) job_lock_wait_ms=(unsigned)strtoul(env,NULL,10);
    if (!job_lock_wait_ms) { printf("Job wait: BB_JOB_LOCK_WAIT_MS=0, workers suspend at once\n"); return; }
    /* mov rdi,[rbx+0x20]; mov rsi,r15; mov edx,r14d; call try; test al,al; jne done;
     * test r12b,r12b; je not_a_worker; nop5; (suspend loop:) mov rdi,rbx; mov rsi,r15; mov edx,r14d; call SuspendJob */
    const int64_t at=find_masked("488b7b20 4c89fe 4489f2 e8???????? 84c0 75?? 4584e4 74?? 0f1f440000 4889df 4c89fe 4489f2 e8????????",
                                 image_size);
    if (at==-1) { patch_job_lock_wait_us(image_size); return; }
    if (at<0) { printf("Job wait: %s\n",at==-2 ? "pattern not unique, not patched" : "pattern not found, not patched"); return; }
    unsigned char *p=image+at;
    int32_t rel; memcpy(&rel,p+11,4);
    job_lock_try=(uint64_t)(uintptr_t)(p+15+rel);
    const uint64_t done=(uint64_t)(uintptr_t)(p+19+(int8_t)p[18]);
    const uint64_t not_worker=(uint64_t)(uintptr_t)(p+24+(int8_t)p[23]);
    const uint64_t suspend_loop=(uint64_t)(uintptr_t)(p+29);
    unsigned char *t=near_image_memory(4096);
    if (!t) { printf("Job wait: no thunk memory, not patched\n"); return; }
    size_t k=0;
    const uintptr_t host=(uintptr_t)job_lock_wait;
    static const unsigned char test_r12b[]={0x45,0x84,0xe4,0x75,0x0e};      /* test r12b,r12b; jnz worker */
    memcpy(t+k,test_r12b,sizeof(test_r12b)); k+=sizeof(test_r12b);
    t[k++]=0xff; t[k++]=0x25; memset(t+k,0,4); k+=4; memcpy(t+k,&not_worker,8); k+=8;  /* jmp [rip]: not a worker */
    /* worker: */
    static const unsigned char args[]={0x48,0x89,0xdf,0x90,0x4c,0x89,0xfe,0x44,0x89,0xf2}; /* rdi=rbx (the counter), rsi=r15, edx=r14d */
    memcpy(t+k,args,sizeof(args)); k+=sizeof(args);
    t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&host,8); k+=8;   /* movabs rax, job_lock_wait */
    t[k++]=0xff; t[k++]=0xd0;                              /* call rax (rsp aligned as at the original calls) */
    t[k++]=0x84; t[k++]=0xc0;                              /* test al,al */
    t[k++]=0x74; t[k++]=0x0e;                              /* jz suspend */
    t[k++]=0xff; t[k++]=0x25; memset(t+k,0,4); k+=4; memcpy(t+k,&done,8); k+=8;          /* jmp [rip]: acquired */
    t[k++]=0xff; t[k++]=0x25; memset(t+k,0,4); k+=4; memcpy(t+k,&suspend_loop,8); k+=8;  /* jmp [rip]: suspend as before */
    const int32_t to=(int32_t)((int64_t)(uintptr_t)t-(int64_t)(uintptr_t)(p+19+5));
    p[19]=0xe9; memcpy(p+20,&to,4);                        /* replaces test r12b,r12b; je */
    printf("Job wait: workers with a render context open wait for a job counter in place for up to %u ms before suspending their job (0x%" PRIx64 ")\n",
           job_lock_wait_ms,(uint64_t)at);
}
/* The shader resource binding loop (U1 0x63e260) calls a handler per bound input through a
 * pointer-to-member table indexed by the input's type; only a few types have handlers and the rest
 * are null. An input of an unhandled type (a zeroed entry) called address 0 and stopped the game in
 * the jungle chapters. The call now skips a null handler. Same code in all three games:
 *   test cl,1; je +8; mov rdx,[rdi]; mov rcx,[rcx+rdx-1]; movzx esi,byte [rax+2]; <set rdx>; call rcx
 * where <set rdx> is `mov rdx, r15` (1.02, 3 bytes) or `lea rdx,[rbp-0x60]` (1.00, 4 bytes).
 * By soni424, with drakolordx7's 1.00 variant (their community fork, cbeb43e6 + 9e2ab8fd). */
static void patch_null_binding_handler(uint64_t image_size);
/* BB_DEV_MENU=1 (experiment, Uncharted 3 EU 1.02): the debug camera manager (Manual Camera, ...) is created at start
 * only when a check at guest offset 0x160527 says a heap (0x152b310, apparently the debug memory pool) exists, which
 * retail lacks: the je at 0x16052e that skips it is removed. Its menu items are attached to a "Camera..." item the
 * retail Dev Menu does not have: the name looked up at 0x12c22b is pointed at "Player Menu..." (0xf60afe), an item
 * the root has and leaves empty. */
static void patch_dev_menu_camera(uint64_t image_size) {
    const char *env = getenv("BB_DEV_MENU");
    if (!env || env[0] != '1' || image_size < 0x1000000) return;
    static const unsigned char skip_je[] = {0x74, 0x2c};                               /* je 0x16055c */
    static const unsigned char camera_name[] = {0x48, 0x8d, 0x35, 0x5b, 0x0f, 0xe3, 0x00}; /* lea rsi, "Camera..." */
    if (memcmp(image + 0x16052e, skip_je, sizeof(skip_je)) || memcmp(image + 0x12c22b, camera_name, sizeof(camera_name)) ||
        memcmp(image + 0xf5d18d, "Camera...", 10) || memcmp(image + 0xf60afe, "Player Menu...", 15)) {
        printf("Dev menu: camera items not patched (not Uncharted 3 EU 1.02)\n");
        return;
    }
    /* It allocates in memory context 8 (the debug pool, absent in retail: a null pool at the first allocation);
     * the menus' own context 0x13 (the Debug DMENU pool, as the menu builder at 0x160046 uses) instead, at the
     * three context switches on its path (mov dword [rbp-..], 8). */
    static const unsigned int context_imm[] = {0x160536, 0x12be5c, 0x12c1e4};
    for (unsigned i = 0; i < 3; ++i) {
        if (memcmp(image + context_imm[i], "\x08\x00\x00\x00", 4)) {
            printf("Dev menu: camera items not patched (context switch %#x differs)\n", context_imm[i]);
            return;
        }
    }
    for (unsigned i = 0; i < 3; ++i) image[context_imm[i]] = 0x13;
    image[0x16052e] = 0x66; /* 2-byte nop: the camera manager is always created */
    image[0x16052f] = 0x90;
    const int32_t disp = (int32_t)(0xf60afe - (0x12c22b + 7));
    memcpy(image + 0x12c22b + 3, &disp, 4);
    printf("Dev menu: debug camera manager created (menu memory), its items under Player Menu...\n");
}
static void patch_null_binding_handler(uint64_t image_size) {
    static const unsigned char head[]={0xf6,0xc1,0x01,0x74,0x08,0x48,0x8b,0x17,0x48,0x8b,0x4c,0x11,0xff,
                                       0x0f,0xb6,0x70,0x02};      /* 17 bytes, common to both */
    struct Variant { unsigned char tail[6]; unsigned tail_len; unsigned char guard[16]; unsigned guard_len; };
    static const struct Variant variants[]={
        /* 1.02: mov rdx, r15; call rcx -> call thunk (thunk: mov rdx, r15; test rcx, rcx; je ret;
         * jmp rcx (returns to the game's call site); ret) */
        {{0x4c,0x89,0xfa,0xff,0xd1},5,{0x4c,0x89,0xfa,0x48,0x85,0xc9,0x74,0x02,0xff,0xe1,0xc3},11},
        /* 1.00: lea rdx,[rbp-0x60]; call rcx -> call thunk + nop */
        {{0x48,0x8d,0x55,0xa0,0xff,0xd1},6,{0x48,0x8d,0x55,0xa0,0x48,0x85,0xc9,0x74,0x02,0xff,0xe1,0xc3},12},
    };
    unsigned char *thunk=NULL;
    int patched=0;
    for (uint64_t at=0;at+sizeof(head)+6<=image_size;++at) {
        if (image[at]!=0xf6 || memcmp(image+at,head,sizeof(head))) continue;
        for (size_t v=0;v<sizeof(variants)/sizeof(variants[0]);++v) {
            const struct Variant *var=&variants[v];
            if (memcmp(image+at+sizeof(head),var->tail,var->tail_len)) continue;
            if (!thunk && !(thunk=near_image_memory(4096))) { printf("Shader bindings: no thunk memory\n"); return; }
            if (16*(patched+1)>4096) return;
            unsigned char *t=thunk+16*patched, *f=image+at+sizeof(head);
            memcpy(t,var->guard,var->guard_len);
            int32_t to=(int32_t)((int64_t)(uintptr_t)t-(int64_t)(uintptr_t)(f+5));
            f[0]=0xe8; memcpy(f+1,&to,4);                        /* call thunk */
            if (var->tail_len>5) f[5]=0x90;
            printf("Shader bindings: null handler guard at %#" PRIx64 "\n",(uint64_t)(f-image));
            ++patched;
            break;
        }
    }
    if (!patched) printf("Shader bindings: null handler call site not found\n");
}
/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. */
void runtime_restart(void) {
    fflush(NULL);
#ifdef _WIN32
    /* run.py sets BB_RESTART_COMMAND: the launch command line, started again (new patches). */
    const char *command=getenv("BB_RESTART_COMMAND");
    puts("Runtime: restarting through run.py");
    if (!command || !*command) { fputs("runtime_restart: BB_RESTART_COMMAND is not set\n",stderr); _exit(1); }
    char *line=_strdup(command);
    /* Same output handles: a launcher reading the game's log keeps reading the new launch. */
    STARTUPINFOA startup={.cb=sizeof(startup),.dwFlags=STARTF_USESTDHANDLES,
                          .hStdInput=GetStdHandle(STD_INPUT_HANDLE),.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE),
                          .hStdError=GetStdHandle(STD_ERROR_HANDLE)};
    PROCESS_INFORMATION process;
    if (!line || !CreateProcessA(NULL,line,NULL,NULL,TRUE,0,NULL,NULL,&startup,&process)) {
        fprintf(stderr,"runtime_restart: CreateProcess failed (%lu)\n",GetLastError()); _exit(1);
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    /* The new launch waits for this process's GPU device to go away (run.py --after). */
    TerminateProcess(GetCurrentProcess(),0);
#else
    puts("Runtime: restarting through run.sh");
    syscall(SYS_close_range, 3u, ~0u, 0u);
    execlp("bash", "bash", "run.sh", (char *)NULL);
    perror("runtime_restart: exec");
    _exit(1);
#endif
}

#ifdef _WIN32
/* BB_ISA_STATS=1: the most executed rewritten sites every 30 seconds (how much the rewrite costs). */
static DWORD WINAPI isa_stats_thread(void *unused) {
    (void)unused;
    for (;;) { Sleep(30000); if (isa_table) isa_rewrite_report(isa_table, stdout); }
}
#endif
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifndef _WIN32
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits. */
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
#endif
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
    char isa_names[64];
    isa_sets=check_cpu(isa_names,sizeof(isa_names));
    if (argc == 2 && !strcmp(argv[1], "--cpu-features")) {   /* run.py: does the game need the rewrite table? */
        if (isa_sets) printf("CPU: missing %s\n",isa_names);
        else puts("CPU: complete");
        return 0;
    }
#ifdef _WIN32
    /* The PS4 user range must stay free of host allocations (GPU driver, heaps), and the guest
     * TCB needs one of the 64 TEB TLS slots before DLLs (Vulkan drivers) take them. */
    runtime_memory_reserve();
    runtime_thread_tcb_offset();
#endif
    int cpu_only = 0, strict_imports = 0, list_missing = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL, *guest_argv_file=NULL, *trophy_dir=NULL, *isa_file=NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--list-missing")) list_missing = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches") && i+1<argc) patch_file=argv[++i];
        else if (!strcmp(argv[i], "--guest-argv") && i+1<argc) guest_argv_file=argv[++i];
        else if (!strcmp(argv[i], "--trophies") && i+1<argc) trophy_dir=argv[++i];
        else if (!strcmp(argv[i], "--isa-rewrite") && i+1<argc) isa_file=argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--list-missing] [--content-profile file] [--app0 dir] [--user dir] [--patches file] [--guest-argv file] [--trophies dir] [--isa-rewrite table] [--timeout seconds] | --vulkan-only | --controller-info | --controller-test 0..100\n", argv[0]);
        return 1;
    }
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"BBCONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        _putenv_s("BB_APP0_DIR", app0); /* the GPU side reads shader code from the game files (variant replay) */
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
        char serial[16]="", title[128]="", version[16]="";
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"APP_VER",version,sizeof(version),NULL);
        bbnet_configure(app0,serial,title,version);
    }
    if (trophy_dir) runtime_trophy_configure(trophy_dir);
    bbgpu_register_kernel();
#ifdef _WIN32
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
    AddVectoredExceptionHandler(1, vectored_fault);
    SetUnhandledExceptionFilter(unhandled_fault);
    if (getenv("BB_STRICT_HANDLES")) {
        PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY strict={0};
        strict.RaiseExceptionOnInvalidHandleReference=1;
        strict.HandleExceptionsPermanentlyEnabled=1;
        if (!SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy,&strict,sizeof(strict)))
            fprintf(stderr,"BB_STRICT_HANDLES: policy not set (%lu)\n",GetLastError());
    }
    (void)timeout_seconds; /* no watchdog on Windows */
#else
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(timeout_seconds); /* 0 disables the watchdog */
#endif
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "BBPROBE1", 8) && memcmp(magic, "BBPROBE2", 8) && memcmp(magic,"BBPROBE3",8) && memcmp(magic,"BBPROBE4",8) && memcmp(magic,"BBPROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "BBPROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"BBPROBE5",8);
    int linked=!memcmp(magic,"BBPROBE3",8) || !memcmp(magic,"BBPROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* BBPROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"BBPROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    if (list_missing) {
        unsigned char *kinds=calloc(import_count ? import_count : 1,1);
        if (!kinds) fail("allocation failed");
        for (uint64_t i=0;i<nr;++i) if (relocs[i].kind) kinds[relocs[i].value]=(unsigned char)relocs[i].kind;
        uint64_t missing=0;
        for (uint64_t i=0;i<import_count;++i) {
            if (!kinds[i] || runtime_resolve(names[i],kinds[i]==2) || (native_libc && bindings[i])) continue;
            const char *symbol=runtime_symbol(names[i]);
            printf("missing %s %s%s\n",names[i],symbol ? symbol : "?",kinds[i]==2 ? " (data)" : "");
            ++missing;
        }
        printf("%" PRIu64 " of %" PRIu64 " imports missing\n",missing,import_count);
        return missing ? 20 : 0;
    }
    /* The rewrite stubs sit right behind the image, so every jmp between the two reaches. */
    size_t isa_area = 0;
    if (isa_sets) {
        const int forced = getenv("BB_FORCE_LEGACY_CPU") && strcmp(getenv("BB_FORCE_LEGACY_CPU"), "0");
#ifdef _WIN32
        if (isa_file) {
            if (!(isa_table = isa_table_load(isa_file))) {
                fprintf(stderr, "STOP: this CPU lacks %s and the rewrite table cannot be used\n", isa_names);
                exit(24);
            }
            isa_area = round_page(isa_stub_area_size(isa_table));
        } else if (isa_sets != CPU_SET_MOVBE || forced) {
            fprintf(stderr, "STOP: this CPU lacks %s (or BB_FORCE_LEGACY_CPU is set) and no rewrite table was given "
                    "(start the game through run.py or the launcher, which make it)\n", isa_names);
            exit(24);
        } else {
            printf("CPU: no MOVBE, the game's MOVBE instructions are emulated (slower)\n");
        }
#else
        (void)forced;
        fprintf(stderr, "STOP: this CPU lacks %s (the instruction rewrite is Windows only)\n", isa_names);
        exit(24);
#endif
    }
    image = runtime_image_map(round_page(size) + isa_area, PROT_READ | PROT_WRITE);
    if (!image) image = allocate(round_page(size) + isa_area);
    image_size = size;
#ifdef _WIN32
    red_zone_reserve(image, size);
#endif
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (isa_table && isa_table_matches(isa_table, image, size)) {
        fprintf(stderr, "STOP: the rewrite table %s was made for another image (delete it; run.py makes it again)\n", isa_file);
        exit(24);
    }
    if (!cpu_only) {
        char title[128]="Bloodborne", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        BbGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
#ifdef _WIN32
        bbgpu_thread_stack_dump=runtime_dump_thread_stacks;
        /* Never in Windows' efficiency mode (EcoQoS): it slows and moves the threads of processes
         * it judges to be in the background, onto efficiency cores on hybrid CPUs (BB_PCORES=0:
         * left to Windows; bbport_threads.h keeps the render threads on performance cores). */
        if (!getenv("BB_PCORES") || strcmp(getenv("BB_PCORES"),"0")) {
            PROCESS_POWER_THROTTLING_STATE power={PROCESS_POWER_THROTTLING_CURRENT_VERSION,PROCESS_POWER_THROTTLING_EXECUTION_SPEED,0};
            SetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&power,sizeof(power));
        }
        /* BB_TIMER_RES=<100 ns units> (test, e.g. 5000 = 0.5 ms): the Windows timer resolution, so
         * timed waits (the job workers' idle waits) end closer to when they should. */
        if (getenv("BB_TIMER_RES")) {
            typedef LONG (NTAPI *SetRes)(ULONG, BOOLEAN, PULONG);
            const SetRes set=(SetRes)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtSetTimerResolution");
            ULONG actual=0;
            if (set) set((ULONG)strtoul(getenv("BB_TIMER_RES"),NULL,10),TRUE,&actual);
            printf("Timer resolution: %.3f ms (BB_TIMER_RES)\n",actual/10000.0);
        }
        /* BB_STACK_DUMP_AFTER=<seconds> (diagnosis): the thread stacks once, that long after start. */
        if (getenv("BB_STACK_DUMP_AFTER")) CloseHandle(CreateThread(NULL,0,stack_dump_after,NULL,0,NULL));
#endif
        if (bbgpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,bbgpu_symbol_count());
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        /* SysV: mov edi, index; movabs rax, handler; jmp rax. No fake return values. */
        uint32_t index = (uint32_t)i;
        uintptr_t handler = (uintptr_t)unresolved;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; /* mov rsi,rdi: preserve arg0 */
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
            if (resolved) value = resolved + relocs[i].addend;
            else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
            }
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
    install_guest_hooks(segments, ns);
    {
        uint64_t image_end=0;
        for (uint64_t s=0;s<ns;++s) if (segments[s].address+segments[s].size>image_end) image_end=segments[s].address+segments[s].size;
        install_game_callbacks(segments, ns, image_end);
#ifdef _WIN32
        install_ndgi_hooks(image_end);
#endif
        install_job_idle_yield(image_end);
        patch_eop_tick_wait(image_end);
        patch_collection_skip(image_end);
        patch_unlock_chapters(image_end);
        patch_u1_sleep_log(image_end);
        patch_u1_fixed_physics(image_end);
        patch_job_sleep(image_end);
        patch_job_lock_wait(image_end);
        patch_null_binding_handler(image_end);
        patch_dev_menu_camera(image_end);
        patch_rdtsc(image_end);
        /* Ultrawide and field of view (runtime_ultrawide.c): the code segment at address 0. */
        for (uint64_t s=0;s<ns;++s)
            if ((segments[s].flags&1) && segments[s].address==0) { runtime_patch_ultrawide(image, segments[s].size, image_end); runtime_patch_resolution(image, segments[s].size, image_end); break; }
    }
    /* Before the red-zone patch (red_zone.c moves code into thunks; the table holds the original
     * places). The stubs step 256 bytes below rsp before saving anything, past the red zone. */
    if (isa_table) {
        unsigned char *stubs = image + round_page(size);
        IsaReport r = isa_rewrite_apply(isa_table, image, stubs, isa_sets);
        if (r.unsupported_needed) {
            fprintf(stderr, "STOP: this CPU lacks %s and %u instructions of the game cannot be rewritten\n", isa_names, r.unsupported_needed);
            exit(24);
        }
        printf("ISA: CPU lacks %s: %u sites rewritten (%u jmp, %u relocated, %u int3), %u skipped\n", isa_names,
               r.rewritten, r.jmp, r.relocated, r.int3, r.skipped);
        protect(stubs, isa_area, 5);
#ifdef _WIN32
        if (getenv("BB_ISA_STATS")) CloseHandle(CreateThread(NULL, 0, isa_stats_thread, NULL, 0, NULL));
#endif
    }
#ifdef _WIN32
    printf("Guest TCB loads redirected to the TEB TLS slot: %" PRIu64 "\n", patch_tcb_loads(segments, ns));
    /* Last code patch: Windows exception frames and the red zone of leaf functions (red_zone.c). */
    {
        RedZoneSegment *loaded=calloc(ns ? ns : 1,sizeof(*loaded));
        if (!loaded) fail("allocation failed");
        for (uint64_t s=0;s<ns;++s) loaded[s]=(RedZoneSegment){segments[s].address,segments[s].size,segments[s].flags};
        red_zone_patch(image, size, loaded, ns);
        free(loaded);
    }
#endif
    protect(traps, round_page((import_count + 1) * 32), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    printf("Guest image at %p\n", (void *)image);
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        runtime_set_guest_fpu();
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            typedef int (ABI *ModuleInit)(uint64_t,void *,void *);
            int result=((ModuleInit)(image+modules[m].init))(0,NULL,NULL);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
#ifdef _WIN32
    SetThreadDescription(GetCurrentThread(),L"bb:GuestMain");   /* found by name by the sampler (BB_HITCH_MS) */
    if (getenv("BB_WATCH")) CloseHandle(CreateThread(NULL,0,watch_thread,getenv("BB_WATCH"),0,NULL));
#endif
    /* argv: the executable and, after sceSystemServiceLoadExec, the arguments it was given
     * as its argv (--guest-argv: one per line, argv[0] first). */
    struct { uint64_t argc; const char *argv[34]; } params = {1, {"/app0/eboot.bin", NULL}};
    if (guest_argv_file) {
        FILE *list=fopen(guest_argv_file,"rb");
        static char lines[33][1024];
        uint64_t n=0;
        while (list && n<33 && fgets(lines[n],sizeof(lines[n]),list)) {
            lines[n][strcspn(lines[n],"\r\n")]=0;
            params.argv[n]=lines[n]; ++n;
        }
        if (list) fclose(list);
        if (n) { params.argc=n; params.argv[n]=NULL; }
        printf("Guest argv: %s (+%" PRIu64 " arguments)\n",params.argv[0],params.argc-1);
    }
#ifdef _WIN32
    typedef void (ABI *Entry)(void *, void (ABI *)(void));
    ((Entry)(image + entry))(&params, guest_exit);
#else
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,PROT_READ|PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
    enter_on_stack(image+entry,&params,(void *)guest_exit,stack+MAIN_STACK-64);
#endif
    fail("entry unexpectedly returned");
}
