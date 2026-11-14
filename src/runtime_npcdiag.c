/* Diagnosis (BB_NPC_DUMP=<file>, Uncharted 2 1.02 code only): while <file>.on exists, each frame (the menu manager's
 * update, runtime_checkpoint.c) every live NPC of the NPC manager (1.02 0x110dcb8: +0 count, entries at +0x10 of 16
 * bytes {slot pointer, id}; the object is *slot while its +0x44 equals the id) is written raw to <file>: a record
 * {"NPCD", frame, index, id, object address, size} and `size` bytes (BB_NPC_DUMP_SIZE, default 0x6000). At most
 * BB_NPC_DUMP_FRAMES frames (default 1800). For offline analysis of AI/animation state that flips. */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>

void runtime_npcdiag_frame(uint64_t image, int code) {
    static int state=-1; static FILE *out; static char trig[600]; static uint32_t size=0x6000, max_frames=1800, frames;
    static uint32_t frame;
    if (state==0) return;
    if (state<0) {
        const char *p=getenv("BB_NPC_DUMP");
        if (!p || !*p) { state=0; return; }
        snprintf(trig,sizeof(trig),"%s.on",p);
        const char *s=getenv("BB_NPC_DUMP_SIZE"); if (s) size=(uint32_t)strtoul(s,NULL,0);
        const char *f=getenv("BB_NPC_DUMP_FRAMES"); if (f) max_frames=(uint32_t)strtoul(f,NULL,0);
        out=fopen(p,"wb");
        state=out ? 1 : 0;
        printf("NPC dump: %s (%s), %u bytes per NPC\n",p,out ? "open" : "cannot open",size);
        fflush(stdout);
        if (!out) return;
    }
    ++frame;
    if (code!=3 || frames>=max_frames) return;
    struct stat st;
    if (stat(trig,&st)!=0) return;
    ++frames;
    const unsigned char *mgr=(const unsigned char *)(uintptr_t)(image+0x110dcb8);
    uint32_t count; memcpy(&count,mgr,4);
    if (count>32) count=32;
    for (uint32_t i=0;i<count;++i) {
        uint64_t slot; uint32_t id; memcpy(&slot,mgr+0x10+16*i,8); memcpy(&id,mgr+0x18+16*i,4);
        if (slot<0x10000 || !id) continue;
        uint64_t obj; memcpy(&obj,(const void *)(uintptr_t)slot,8);
        if (obj<0x10000) continue;
        uint32_t oid; memcpy(&oid,(const void *)(uintptr_t)(obj+0x44),4);
        if (oid!=id) continue;
        if (!runtime_memory_is_mapped((uintptr_t)obj,size)) continue;
        struct { char magic[4]; uint32_t frame, index, id; uint64_t obj; uint32_t size, pad; } h={{'N','P','C','D'},frame,i,id,obj,size,0};
        fwrite(&h,sizeof(h),1,out);
        fwrite((const void *)(uintptr_t)obj,1,size,out);
    }
    if (frames==max_frames) { fflush(out); printf("NPC dump: %u frames written\n",frames); fflush(stdout); }
    else if ((frames&63)==0) fflush(out);
}

/* BB_NPC_LOG=1 (diagnosis, 1.02 code): the NPCs' own debug log. The AI writes it through NpcLog (1.02 0x214b60:
 * rdi = the NPC, rsi = printf format; it prints to the TTY when 0x1057d60+0x121 is set and to the NPC's log stream
 * +0x21b0) only while the AI debug switch 0x1057d60+0x118 (byte 0; bits 32-39 = categories) is on, which the frame hook
 * sets here. The hook inside NpcLog (after its va_list is built: rbx = the NPC, r14 = the format, va_list at rsp+0xb0)
 * formats the line itself and prints "NpcLog f<frame> <npc id>: <text>". BB_NPC_LOG_ID=<hex id>: only that NPC. */
static int npclog_on=-1; static uint32_t npclog_id; static volatile uint32_t npclog_frame;
static void npclog_init(void) {
    const char *e=getenv("BB_NPC_LOG"); npclog_on=e && e[0]=='1';
    const char *i=getenv("BB_NPC_LOG_ID"); if (i) npclog_id=(uint32_t)strtoul(i,NULL,16);
}
void runtime_npclog_frame(uint64_t image) {
    if (npclog_on<0) npclog_init();
    if (!npclog_on) return;
    ++npclog_frame;
    unsigned char *b=(unsigned char *)(uintptr_t)(image+0x1057d60);
    if (b[0x118]!=1 || (b[0x11c]&0xff)!=0xff) {
        b[0x118]=1; b[0x11c]=0xff;
        static int told; if (!told++) { printf("NpcLog: AI debug log switch on\n"); fflush(stdout); }
    }
}
typedef struct { uint32_t gp, fp; uint64_t overflow, save; } SysvVaList;
static uint64_t va_gp(SysvVaList *v) {
    uint64_t x;
    if (v->gp<48) { memcpy(&x,(const void *)(uintptr_t)(v->save+v->gp),8); v->gp+=8; }
    else { memcpy(&x,(const void *)(uintptr_t)v->overflow,8); v->overflow+=8; }
    return x;
}
static double va_fp(SysvVaList *v) {
    double x;
    if (v->fp<176) { memcpy(&x,(const void *)(uintptr_t)(v->save+v->fp),8); v->fp+=16; }
    else { memcpy(&x,(const void *)(uintptr_t)v->overflow,8); v->overflow+=8; }
    return x;
}
static int readable_str(uint64_t p) {
    const uint64_t base=runtime_image_base(), size=runtime_image_size();
    if (base && p>=base && p<base+size) return 1;
    if (p<0x10000 || p>=0x7fffffffffffull) return 0;
    if (runtime_memory_is_mapped((uintptr_t)p,1)) return 1;
    /* a guest stack buffer (formatted lines): a guarded read */
    sigjmp_buf jb; volatile int ok=0;
    if (sigsetjmp(jb,0)==0) { runtime_fault_recover=&jb; volatile char c=*(volatile const char *)(uintptr_t)p; (void)c; ok=1; }
    runtime_fault_recover=NULL;
    return ok;
}
void __attribute__((ms_abi)) runtime_npclog_line(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                 const uint64_t *regs) {
    (void)rdi; (void)rsi; (void)image;
    if (npclog_on<=0) return;
    const uint64_t npc=regs[0];
    if (npc<0x10000 || !readable_str(r14)) return;
    uint32_t id; memcpy(&id,(const void *)(uintptr_t)(npc+0x44),4);
    if (npclog_id && id!=npclog_id) return;
    const uint64_t rsp=(uint64_t)(uintptr_t)&regs[25];
    SysvVaList v; memcpy(&v,(const void *)(uintptr_t)(rsp+0xb0),sizeof(v));
    char out[1024]; size_t n=0; const char *f=(const char *)(uintptr_t)r14;
    for (int guard=0; *f && n+64<sizeof(out) && guard<1000; ++guard) {
        if (*f!='%') { out[n++]=*f++; continue; }
        if (f[1]=='%') { out[n++]='%'; f+=2; continue; }
        char spec[32]; size_t k=0; spec[k++]=*f++;
        while (*f && strchr("-+ #0123456789.*",*f) && k<20) spec[k++]=*f++;
        int lng=0; while (*f && strchr("hlLqjzt",*f) && k<24) { if (*f=='l'||*f=='q'||*f=='j'||*f=='z') lng=1; spec[k++]=*f++; }
        if (!*f) break;
        const char c=*f++;
        spec[k++]=c; spec[k]=0;
        int w=0;
        if (strchr("dic",c)) { long long x=(long long)va_gp(&v);
            char t[40]; snprintf(t,sizeof(t),"%lld",lng ? x : (long long)(int)x); w=snprintf(out+n,sizeof(out)-n,"%s",c=='c' ? (char[2]){(char)x,0} : t); }
        else if (strchr("uxXo",c)) { unsigned long long x=va_gp(&v); if (!lng) x&=0xffffffffu;
            w=snprintf(out+n,sizeof(out)-n,c=='x' ? "%llx" : c=='X' ? "%llX" : c=='o' ? "%llo" : "%llu",x); }
        else if (c=='p') { w=snprintf(out+n,sizeof(out)-n,"%llx",(unsigned long long)va_gp(&v)); }
        else if (c=='s') { uint64_t p=va_gp(&v); w=snprintf(out+n,sizeof(out)-n,"%.200s",readable_str(p) ? (const char *)(uintptr_t)p : "(?)"); }
        else if (strchr("fFeEgGaA",c)) { double x=va_fp(&v); char fs[40]; snprintf(fs,sizeof(fs),"%s",spec); w=snprintf(out+n,sizeof(out)-n,fs,x); }
        else { w=snprintf(out+n,sizeof(out)-n,"%s",spec); }
        if (w>0) n+=(size_t)w < sizeof(out)-n ? (size_t)w : sizeof(out)-n-1;
    }
    while (n && (out[n-1]=='\n' || out[n-1]=='\r')) --n;
    out[n]=0;
    printf("NpcLog f%u %08x: %s\n",npclog_frame,id,out);
}

/* BB_NPC_LOG=1 too: each navigation command an NPC gets (1.02 NavCommandProcessor send, 0x383ef0: rdi = the processor,
 * its +0x118 the NPC; rsi = the command, +0x64 its type (1 StopAndStand, 2 MoveToPoint...), +0x40 the target point):
 * the calling site, the target, the NPC's position (+0x110), its skill (+0x4020, name at +8; state at skill +0x18), its
 * point list (+0x4028: count, current index, points at +0x10) and zone (+0x3fb0 index, +0x3fc0 + 32 i: center, radius,
 * height). */
static int fin(float x) { return x==x && x<1e30f && x>-1e30f; }
void __attribute__((ms_abi)) runtime_npclog_command(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                    const uint64_t *regs) {
    (void)r14;
    if (npclog_on<=0 || rdi<0x10000 || rsi<0x10000) return;
    const unsigned char *npc=*(const unsigned char *const *)(uintptr_t)(rdi+0x118);
    if ((uintptr_t)npc<0x10000) return;
    uint32_t id; memcpy(&id,npc+0x44,4);
    if (npclog_id && id!=npclog_id) return;
    const unsigned char *cmd=(const unsigned char *)(uintptr_t)rsi;
    uint32_t type; memcpy(&type,cmd+0x64,4);
    float t[4], p[4]; memcpy(t,cmd+0x40,16); memcpy(p,npc+0x110,16);
    const uint64_t ret=regs[25];
    const unsigned char *skill=*(const unsigned char *const *)(npc+0x4020);
    const char *sname="-"; int32_t sstate=-1;
    if ((uintptr_t)skill>0x10000) { const char *n=*(const char *const *)(skill+8); if (readable_str((uint64_t)(uintptr_t)n)) sname=n; memcpy(&sstate,skill+0x18,4); }
    char pts[160]="";
    const unsigned char *list=*(const unsigned char *const *)(npc+0x4028);
    if ((uintptr_t)list>0x10000) {
        int32_t count, idx; memcpy(&count,list,4); memcpy(&idx,list+4,4);
        float q[4]={0}; if (idx>=0 && idx<64) memcpy(q,list+0x10+16*idx,16);
        snprintf(pts,sizeof(pts)," list %d/%d (%.3f %.3f %.3f)",idx,count,(double)q[0],(double)q[1],(double)q[2]);
    }
    uint32_t zi; memcpy(&zi,npc+0x3fb0,4);
    float z[6]={0}; if (zi<8) memcpy(z,npc+0x3fc0+32*zi,24);
    const float dx=t[0]-p[0], dy=t[1]-p[1], dz=t[2]-p[2];
    printf("NpcCmd f%u %08x: type %u from +%#llx target (%.3f %.3f %.3f) pos (%.3f %.3f %.3f) d %.3f (xz %.3f) skill %s state %d%s zone %u (%.3f %.3f %.3f r %.3f h %.3f)\n",
           npclog_frame,id,type,(unsigned long long)(ret-image),(double)t[0],(double)t[1],(double)t[2],(double)p[0],(double)p[1],
           (double)p[2],fin(dx) ? (double)sqrtf(dx*dx+dy*dy+dz*dz) : -1.0,fin(dx) ? (double)sqrtf(dx*dx+dz*dz) : -1.0,sname,sstate,pts,zi,
           (double)z[0],(double)z[1],(double)z[2],(double)z[4],(double)z[5]);
}
