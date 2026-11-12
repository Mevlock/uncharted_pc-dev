#define _GNU_SOURCE
/* Mouse in the menus of Uncharted 1, 2 and 3 (hover follows the cursor, click, wheel, sliders, scroll bars, lists,
 * Yes/No confirmations, the side or right button goes back) by antikmewzik
 * (github.com/antikmewzik/uncharted_pc_controls_qol, branch u2-mkb-qol 65522a6, src/runtime_pad.c: "the mouse in the
 * menus (U1)" and his U2/U3 menu rows, GPL-2.0-or-later as this port; Uncharted 2 and 3: see their section). His code runs here nearly as he wrote it (marked "port:" where it
 * differs): the loader's callbacks (probe.c) and this port's pad read (runtime_pad.c); both game codes, 1.02 (his
 * addresses; US CUSA02320 1.02 = EU CUSA02343 1.02) and 1.00 (EU CUSA02343 1.00 = US CUSA02320 1.00: the same code
 * moved, located by masked signatures). Every hook site and every game function it calls is checked against the
 * original bytes first; a game code that does not match keeps the menus as they are (logged). Not taken: his menu
 * keys (Enter, Esc, Alt, Backspace; the port keeps its own bindings), the keyboard row of the Controls menu (the port has no
 * bindings window to open), the probes (F7/F8/F9). His compare-with-friends removal, the photo mode row, the Treasures model
 * turned with the mouse, U3's chapter carousel and Yes/No by value and U2's controller picture are in (both codes, see
 * "antikmewzik's other menu changes" below). BB_MENU_MOUSE=0: off. */
#include "runtime_menumouse.h"
#include "gpu/bbgpu.h"
int runtime_mouselook_camera_recent(void);   /* runtime_mouselook.c: a follow / cover camera ran in the last 250 ms (gameplay) */
extern float runtime_ultrawide_scale;   /* runtime_ultrawide.c: the UI squeeze (< 0: Vert+, the vertical share) */
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define MS_ABI __attribute__((ms_abi))
enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000,
};
typedef uint64_t (__attribute__((sysv_abi)) *guest_fn2)(uint64_t,uint64_t);
static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
static int mouse_debug(void) {
    static int on=-1;
    if (on<0) on=getenv("BB_MOUSE_DEBUG")!=NULL;
    return on;
}
static int safe_read(void *dst, uint64_t src, size_t size) {
    if (src<0x10000 || src>0x00007fffffffffffull) return 0;
#ifdef _WIN32
    SIZE_T got=0;
    return ReadProcessMemory(GetCurrentProcess(),(LPCVOID)(uintptr_t)src,dst,size,&got) && got==size;
#else
    memcpy(dst,(const void *)(uintptr_t)src,size); return 1;
#endif
}
static int safe_write(uint64_t dst, const void *src, size_t size) {
    if (dst<0x10000 || dst>0x00007fffffffffffull) return 0;
#ifdef _WIN32
    SIZE_T put=0;
    return WriteProcessMemory(GetCurrentProcess(),(LPVOID)(uintptr_t)dst,src,size,&put) && put==size;
#else
    memcpy((void *)(uintptr_t)dst,src,size); return 1;
#endif
}

/* ---- port: the game code (U1): hook sites, the game functions called, the globals read ---------- */
typedef struct { uint64_t at; unsigned char op[3]; } CodeRef;   /* "lea reg, [rip + disp32]" (7 bytes) */
typedef struct {
    const char *version;
    uint64_t item_begin1, item_end1, item_begin2, item_end2, text_draw, menu_event, credits, select_index;
    CodeRef frame_time;   /* the game's frame time (0 while paused) */
    CodeRef audio;        /* the lea of the audio object: its volume floats are at +0x3db20, in the order of the rows */
    uint64_t volume_set;  /* the volume setter's store "vmovss [rdi+rax*4+0x3db20], xmm0" (checked) */
} U1MenuCode;
static const U1MenuCode u1_menu_codes[]={
    {"1.02",0x133fbc,0x133fc8,0x133feb,0x133ff7,0x13acf0,0x1336c0,0x3b41e0,0x1335a0,{0x860b,{0x48,0x8d,0x05}},{0x3583df,{0x48,0x8d,0x05}},0x3584ee},   /* (volumes 0xc86298+0x3db20 = 0xcc3db8, antikmewzik's) */
    {"1.00",0x133e2c,0x133e38,0x133e5b,0x133e67,0x13aa70,0x133530,0x576a30,0x133410,{0x860b,{0x48,0x8d,0x05}},{0x51c66f,{0x48,0x8d,0x05}},0x51c77e},   /* (located 2026-11-10: 0x10fddf8+0x3db20 = 0x113b918) */
};
static const U1MenuCode *u1m;           /* the code found, NULL: none (the menus stay as they are) */
static uint64_t u1_image;               /* port: the image base once a code matched (his name) */
static const volatile float *u1_frame_time;
static uint64_t u1_volumes;   /* the image offset of the four volume floats (0: not known, those sliders go by steps) */
static unsigned char *mm_image;
static uint64_t mm_image_size;
/* The original bytes at each site (the moved instructions first). */
static const unsigned char u1_menu_item_begin_code[]={0x48,0x8b,0x07,0x4c,0x89,0xf6,0x4c,0x89,0xfa,0xff,0x50,0x18};
static const unsigned char u1_menu_item_end1_code[]={0x41,0x8b,0x8d,0x98,0x00,0x00,0x00,0x41,0x8b,0x85,0x50,0x01,0x00,0x00};
static const unsigned char u1_menu_item_end2_code[]={0x49,0x8b,0x04,0x24,0x48,0x3b,0x45,0xd0,0x75,0x12};
static const unsigned char u1_menu_event_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18,0x49,0x89,0xf6};
static const unsigned char u1_select_index_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50,0x89,0xf3,0x49,0x89,0xfe,0x4d};
static int code_is(uint64_t at, const unsigned char *bytes, size_t n) {
    return mm_image && at && at+n<=mm_image_size && !memcmp(mm_image+at,bytes,n);
}
static const unsigned char *code_ref(const CodeRef *r) {
    if (!r->at || r->at+7>mm_image_size || memcmp(mm_image+r->at,r->op,3)) return NULL;
    int32_t disp;
    memcpy(&disp,mm_image+r->at+3,4);
    const int64_t target=(int64_t)r->at+7+disp;
    return target>0 && (uint64_t)target<mm_image_size ? mm_image+target : NULL;
}
static int camera_stopped(void) { return u1_frame_time && !(*u1_frame_time>0.0f); }
static uint64_t wheel_until_us[2];   /* port: set by the pad read (runtime_menumouse_buttons) */
static volatile int u1_rail_wheel;   /* U1: a list with a scrollbar is on screen: the wheel scrolls its view (the bar's thumb) instead of moving the highlight */
static volatile uint64_t u1_hide_select_us;   /* U1 Statistics: a statistic is selected (his list code): the prompt bar drops its SELECT (u1_bar_flags) */
static int scripted_cursor;          /* port: BB_PAD_FILE runs: the cursor and buttons from the pad file */
static float scripted_ux, scripted_uy;

typedef struct { uint64_t item; float x, y, w, h, rx, ry, rw, rh; uint64_t us; int ordinal; float style[8]; } MenuText;
static int menu_text_ordinal;             /* the n-th text of the item being drawn */
#define MENU_TEXTS 256
static MenuText menu_texts[MENU_TEXTS];
static int menu_text_count;
static uint64_t menu_text_item;           /* the item whose draw is running, 0 outside */
static pthread_mutex_t menu_text_lock=PTHREAD_MUTEX_INITIALIZER;
static volatile uint64_t menu_text_frame_us;       /* the clock of the last text of the first item: a new frame starts */
static void MS_ABI u1_menu_item_begin(uint64_t item, uint64_t unused, uint64_t image, uint64_t r14, const uint64_t *regs) {
    (void)unused; (void)image; (void)r14; (void)regs;
    menu_text_item=item;
    menu_text_ordinal=0;
}
static void MS_ABI u1_menu_item_end(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t r14, const uint64_t *regs) {
    (void)unused0; (void)unused1; (void)image; (void)r14; (void)regs;
    menu_text_item=0;
}
static int menu_mode;   /* port: a U1 menu is open (runtime_menumouse_active) */
/* Texts drawn in a menu outside of any menu item (the scrolling lists, the carousels, titles, footers, arrows): the
 * rectangle of each, kept by position, to find out what such a screen is made of (F9) and, for the lists, which line is
 * highlighted (its style differs). */
#define FREE_TEXTS 160
static MenuText free_texts[FREE_TEXTS];
static int free_text_count;
/* The credits (U1 1.02): the credits task updates and draws itself in image+0x3b41e0 (strings "CREDITS:: ..." and "credits-roll"), called every
 * frame while they are on screen; they draw no menu text the port sees, so the menu keys and the mouse would otherwise be off there. */
static volatile uint64_t u1_credits_us;
static int u1_credits_recent(uint64_t now) { return u1_credits_us && now-u1_credits_us<300000 && u1_credits_us<=now+1000000; }
static void MS_ABI u1_credits_tick(uint64_t task, uint64_t unused1, uint64_t image, uint64_t r14, const uint64_t *regs) {   /* task = rdi: its first word is the state, 7..11 while the credits roll */
    (void)unused1; (void)image; (void)r14; (void)regs;
    uint32_t state=0;
    static uint32_t last_state=0xffffffffu;
    if (!safe_read(&state,task,4)) return;
    if (mouse_debug() && state!=last_state) { last_state=state; printf("Runtime: U1 credits task state %u\n",state); }
    if (state-7u<5u) u1_credits_us=now_us();
}
static const unsigned char u1_credits_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x98,0x01,0x00,0x00};
static void MS_ABI u1_text_draw(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t r14, const uint64_t *regs) {
    (void)unused0; (void)unused1; (void)image; (void)r14;
    const uint64_t item=menu_text_item;
    if (!item && !menu_mode) return;
    /* port: rdx (the style), rcx (the laid-out text), r8 (the rectangle), r9d (the horizontal alignment) from the
     * registers this loader's thunk saved, the 7th argument (vertical alignment) above the return address */
    const uint64_t style=regs[5], layout=regs[6], rect=regs[4];
    const uint32_t halign=(uint32_t)regs[3];
    uint32_t valign=(uint32_t)regs[26];
    float r[4], size[2], offset[2];
    if (!safe_read(r,rect,16) || !safe_read(&size[0],layout+0x14,4) || !safe_read(&size[1],layout+0x18,4) ||
        !safe_read(&offset[0],style+0x20,4) || !safe_read(&offset[1],style+0x24,4)) return;
    const float x=halign==2 ? r[0]+r[2]-size[0] : halign==0 ? r[0] : r[0]+(r[2]-size[0])*0.5f;
    const float y=valign==2 ? r[1]+r[3]-size[1] : valign==0 ? r[1] : r[1]+(r[3]-size[1])*0.5f;
    pthread_mutex_lock(&menu_text_lock);
    const uint64_t now=now_us();
    if (!item) {
        MenuText free_text={0,x+offset[0],y+offset[1],size[0],size[1],r[0],r[1],r[2],r[3],now,0,{0}};
        if (!safe_read(free_text.style,style,sizeof(free_text.style))) memset(free_text.style,0,sizeof(free_text.style));
        int fslot=-1;
        for (int i=0;i<free_text_count;++i) {
            const MenuText *o=&free_texts[i];
            if (fabsf(o->rx-r[0])<0.5f && fabsf(o->ry-r[1])<0.5f && fabsf(o->rw-r[2])<0.5f && fabsf(o->rh-r[3])<0.5f &&
                fabsf(o->w-free_text.w)<3.0f) { fslot=i; break; }
        }
        if (fslot<0) for (int i=0;i<free_text_count;++i) if (now-free_texts[i].us>300000) { fslot=i; break; }
        if (fslot<0 && free_text_count<FREE_TEXTS) fslot=free_text_count++;
        if (fslot>=0) free_texts[fslot]=free_text;
        pthread_mutex_unlock(&menu_text_lock);
        return;
    }
    menu_text_frame_us=now;
    const int ordinal=menu_text_ordinal++;
    MenuText text={item,x+offset[0],y+offset[1],size[0],size[1],r[0],r[1],r[2],r[3],now,ordinal,{0}};
    if (!safe_read(text.style,style,sizeof(text.style))) memset(text.style,0,sizeof(text.style));
    int slot=-1;
    for (int i=0;i<menu_text_count;++i)  /* the same text of the same item: the newest draw replaces the old one */
        if (menu_texts[i].item==item && menu_texts[i].ordinal==ordinal) { slot=i; break; }
    if (slot<0) { /* an old entry (not drawn for 200 ms: a closed menu) makes room first */
        for (int i=0;i<menu_text_count;++i) if (now-menu_texts[i].us>200000) { slot=i; break; }
    }
    if (slot<0 && menu_text_count<MENU_TEXTS) slot=menu_text_count++;
    if (slot>=0) menu_texts[slot]=text;
    pthread_mutex_unlock(&menu_text_lock);
}
static const unsigned char u1_text_draw_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x58,0x44,0x89,0xcb};
/* U1 1.02: the menu's event handler (0x1336c0: rdi the menu, rsi the event {context ptr, type (1 up, 2 down), float}). Up and down
 * reach a scrolling list through it, one row per event. To take the highlight of a list to the row under the cursor in one go, the
 * pad thread arms a number of extra steps and presses the direction once; this hook, on the game's own thread and with the real
 * event, then replays the handler that many more times with a copy of the event (the context pointer is only good during the
 * call, so it cannot be kept for later). */
static volatile uint64_t u1_jump_menu, u1_jump_us, u1_event_last_menu;
static volatile int u1_jump_steps, u1_jump_type, u1_jump_busy, u1_event_last_type, u1_event_count;
static volatile uint32_t u1_jump_done, u1_measure_done;
static volatile uint64_t u1_measure_item;
static volatile int u1_measure_n;
static int list_top_read(uint64_t item);
static void list_top_write(uint64_t item, int top);
static int list_index_raw(uint64_t item) {   /* the highlighted row of a list: the u32 at model+0x14, model = *(item+8) */
    uint64_t model=0; uint32_t idx=0;
    if (!safe_read(&model,item+8,8) || !safe_read(&idx,model+0x14,4) || idx>4096) return -1;
    return (int)idx;
}
static void MS_ABI u1_menu_event(uint64_t menu, uint64_t ev, uint64_t image, uint64_t r14, const uint64_t *regs) {
    (void)r14; (void)regs;
    u1_event_last_menu=menu;
    { uint32_t ty=0; if (safe_read(&ty,ev+8,4) && ty) { u1_event_last_type=(int)ty; ++u1_event_count; } }
    if (u1_jump_busy || (u1_jump_steps<=0 && !u1_measure_item) || menu!=u1_jump_menu) return;
    if (now_us()-u1_jump_us>250000) { u1_jump_steps=0; u1_measure_item=0; return; }
    uint64_t copy[4];
    if (!safe_read(copy,ev,sizeof(copy))) return;
    if ((int32_t)(copy[1]&0xffffffffu)!=u1_jump_type) {
        if ((copy[1]&0xffffffffu) && mouse_debug()) printf("Runtime: list jump: event type %u ignored (wanted %d), value %08x, ctx %#llx\n",(unsigned)(copy[1]&0xffffffffu),u1_jump_type,(unsigned)(copy[1]>>32),(unsigned long long)copy[0]);
        return;
    }
    const guest_fn2 handler=(guest_fn2)(uintptr_t)(image+u1m->menu_event);   /* port: by game code */
    if (u1_measure_item) {
        /* The length of a list, found without the player seeing the highlight run: all inside this one call, press down until the
         * highlighted row stops growing or wraps to the top, then go back to just before where it was (the real press that is
         * still to come moves it one down) and put the view back. */
        const uint64_t item=u1_measure_item;
        u1_measure_item=0;
        u1_jump_busy=1;
        const int idx0=list_index_raw(item), top0=list_top_read(item);
        uint64_t down[4]={copy[0],copy[1],copy[2],copy[3]}, up[4]={copy[0],copy[1],copy[2],copy[3]};
        down[1]=(copy[1]&~0xffffffffull)|2u; up[1]=(copy[1]&~0xffffffffull)|1u;
        int prev=idx0, cur=idx0, wrapped=0, total=0;
        for (int i=0;i<400 && idx0>=0;++i) {
            handler(menu,(uint64_t)(uintptr_t)down);
            cur=list_index_raw(item);
            if (cur<0) break;
            if (cur<=prev) { total=prev+1; wrapped=cur<prev; break; }
            prev=cur;
        }
        if (total>0) {
            const int back=idx0>=1 ? idx0-1 : idx0;
            if (wrapped) for (int i=0;i<back;++i) handler(menu,(uint64_t)(uintptr_t)down);
            else for (int i=0;i<cur-back;++i) handler(menu,(uint64_t)(uintptr_t)up);
        }
        if (top0>=0) list_top_write(item,top0);
        u1_jump_busy=0;
        u1_measure_n=total;
        ++u1_measure_done;
        return;
    }
    const int n=u1_jump_steps;
    u1_jump_steps=0;
    u1_jump_busy=1;
    for (int i=0;i<n;++i) handler(menu,(uint64_t)(uintptr_t)copy);
    u1_jump_busy=0;
    ++u1_jump_done;
}
/* A confirmation (return to the checkpoint, to the main menu): a line of exactly two short texts (Oui / Non) near the middle of the
 * screen, the bottom prompts (three on their line) and the list rows (many pairs) being told apart by those counts. The texts may be
 * the row's own or free ones, so both tables are read. Returns the left and the right text. */
typedef struct { float lx, ly, lw, lh, rx, ry, rw, rh; } DialogInfo;
static int menu_dialog_find(uint64_t now, DialogInfo *d) {
    float cx[48], cy[48], cw[48], ch[48], titx[48], tity[48];
    int n=0, nt=0;
    pthread_mutex_lock(&menu_text_lock);
    for (int pass=0;pass<2;++pass) {
        const int total=pass ? free_text_count : menu_text_count;
        for (int i=0;i<total;++i) {
            const MenuText *t=pass ? &free_texts[i] : &menu_texts[i];
            if (now-t->us>100000 || t->w<=1.0f) continue;
            /* port: any text can be the question (the pause menu's Quit asks only "Quit", 55 wide); only a wide one is never a choice */
            if (nt<48) { titx[nt]=t->x+t->w*0.5f; tity[nt]=t->y+t->h*0.5f; ++nt; }
            if (t->w>150.0f) continue;
            if (t->w>90.0f || t->h<18.0f || t->h>40.0f || n>=48) continue;
            cx[n]=t->x; cy[n]=t->y; cw[n]=t->w; ch[n]=t->h; ++n;
        }
    }
    pthread_mutex_unlock(&menu_text_lock);
    int found=0;
    for (int a=0;a<n;++a) for (int b=0;b<n;++b) {
        if (cx[a]>=cx[b]-20.0f) continue;
        const float ya=cy[a]+ch[a]*0.5f;
        if (fabsf(ya-(cy[b]+ch[b]*0.5f))>=4.0f) continue;
        int on_line=0;
        for (int c=0;c<n;++c) if (fabsf(ya-(cy[c]+ch[c]*0.5f))<4.0f) ++on_line;
        const float mid=((cx[a]+cw[a]*0.5f)+(cx[b]+cw[b]*0.5f))*0.5f;
        if (on_line!=2 || fabsf(mid-640.0f)>=120.0f || cx[b]-(cx[a]+cw[a])>120.0f) continue;
        int title=0;   /* the question: a wide text centred over the pair, 40..130 above it */
        for (int t=0;t<nt;++t) if (fabsf(titx[t]-mid)<50.0f && ya-tity[t]>40.0f && ya-tity[t]<130.0f) title=1;
        if (!title) continue;
        if (found) return 0;   /* a second pair: a list, not a confirmation */
        *d=(DialogInfo){cx[a],cy[a],cw[a],ch[a],cx[b],cy[b],cw[b],ch[b]};
        found=1;
    }
    return found;
}
/* A carousel screen (cinematics, chapters): its item draws a 13x13 arrow text. There the wheel and the left/right keys move
 * sideways, not up and down. A confirmation has the same wheel. */
static int menu_carousel_recent(uint64_t now) {
    DialogInfo dialog;
    if (menu_dialog_find(now,&dialog)) return 1;
    int found=0;
    pthread_mutex_lock(&menu_text_lock);
    for (int i=0;i<menu_text_count && !found;++i) {
        const MenuText *t=&menu_texts[i];
        if (t->ordinal>=1 && now-t->us<100000 && t->w>=12.0f && t->w<=14.0f && t->h>=12.0f && t->h<=14.0f && t->style[0]>0.0f) found=1;
    }
    pthread_mutex_unlock(&menu_text_lock);
    return found;
}
/* A scrolling list (treasures, medals, statistics): ONE menu item that draws the rows itself, a label column of texts 25.2 tall
 * (27.7 for the highlighted row, whose style[5] is 3.0) with a pitch of 29.2. Returns the highlighted row's centre, the pitch,
 * the label column and the vertical extent of the rows. */
typedef struct { float sel_cy, pitch, colx, top, bottom; uint64_t item; } ListInfo;
static int menu_list_find_ex(uint64_t now, ListInfo *info, int need_highlight) {
    int found=0;
    pthread_mutex_lock(&menu_text_lock);
    float best_x=0.0f; int best=0; uint64_t best_item=0;
    for (int i=0;i<menu_text_count;++i) {
        const MenuText *a=&menu_texts[i];
        if (a->ordinal<1 || now-a->us>100000 || a->w<=1.0f || a->h<22.0f || a->h>29.0f) continue;
        int n=0;
        for (int j=0;j<menu_text_count;++j) {
            const MenuText *b=&menu_texts[j];
            if (b->item==a->item && b->ordinal>=1 && now-b->us<=100000 && b->w>1.0f && b->h>=22.0f && b->h<=29.0f && fabsf(b->x-a->x)<1.0f) ++n;
        }
        if (n>best) { best=n; best_x=a->x; best_item=a->item; }
    }
    if (best>=2) {
        float ys[64]; int nys=0; float sel=-1.0f, top=1e9f, bottom=-1e9f;
        for (int j=0;j<menu_text_count && nys<64;++j) {
            const MenuText *b=&menu_texts[j];
            if (b->ordinal<1 || now-b->us>100000 || b->w<=1.0f || b->h<22.0f || b->h>29.0f || fabsf(b->x-best_x)>=1.0f) continue;
            ys[nys++]=b->y+b->h*0.5f;
            if (b->y<top) top=b->y;
            if (b->y+b->h>bottom) bottom=b->y+b->h;
            if (b->style[5]>2.5f) sel=b->y+b->h*0.5f;
        }
        float pitch=1e9f;
        for (int a=0;a<nys;++a) for (int b=0;b<nys;++b) { const float d=ys[a]-ys[b]; if (d>5.0f && d<pitch) pitch=d; }
        if ((sel>=0.0f || !need_highlight) && pitch>=25.0f && pitch<=35.0f) { info->sel_cy=sel; info->pitch=pitch; info->colx=best_x; info->top=top; info->bottom=bottom; info->item=best_item; found=1; }
    }
    pthread_mutex_unlock(&menu_text_lock);
    return found;
}
/* a value text (right of the label column) on the line of label t (menu_text_lock held) */
static int line_has_value(const MenuText *t, uint64_t item, float colx, uint64_t now) {
    for (int i=0;i<menu_text_count;++i) { const MenuText *v=&menu_texts[i]; if (v->item==item && now-v->us<=100000 && v->ordinal>=1 && v->x>colx+300.0f && v->h<30.0f && v->w>0.5f && fabsf(v->y-t->y)<2.0f) return 1; }
    return 0;
}
/* ---- the mouse in the menus (U1) ------------------------------------------------------------
 * The game's menu texts are laid out in a 1280x720 space (measured: a 5-row menu has its rows at x 367.5, width
 * 600, height 33.6 and a pitch of 33.6, which matches the screenshots at 1125 lines). The cursor, put in that
 * space, tells which row of which menu it is on (u1_text_draw records the row rectangles). Hovering a row
 * presses the d-pad up or down, one step every 100 ms, until the menu's selected index (menu+0x150) is that
 * row; a left click on a row selects it the same way and then presses cross. With no menu text on screen (the
 * "press a key to start" screen) a left click presses cross. The wheel and the keys stop the hover until the
 * cursor moves again. */
#define UI_WIDTH 1280.0f
#define UI_HEIGHT 720.0f
/* Rows the game never selects (the section titles, the text over a picture): a hover that cannot reach one gives up and the
 * row is ignored for 30 s, instead of pressing up and down for ever. */
typedef struct { uint64_t menu; int index; uint64_t until; } MenuSkip;
static MenuSkip menu_skips[16];
static int menu_skipped(uint64_t menu, int index, uint64_t now) {
    for (int i=0;i<16;++i) if (menu_skips[i].menu==menu && menu_skips[i].index==index && now<menu_skips[i].until) return 1;
    return 0;
}
static void menu_skip(uint64_t menu, int index, uint64_t now) {
    int slot=0;
    for (int i=0;i<16;++i) { if (now>=menu_skips[i].until) { slot=i; break; } if (menu_skips[i].until<menu_skips[slot].until) slot=i; }
    menu_skips[slot]=(MenuSkip){menu,index,now+30000000u};
    printf("Runtime: menu row %d of %#llx cannot be selected: ignored\n",index,(unsigned long long)menu);
}
static int menu_row_at(float ux, float uy, uint64_t now, uint64_t *menu_out, int *index_out, int *count_out) {
    int found=0;
    pthread_mutex_lock(&menu_text_lock);
    for (int i=0;i<menu_text_count && !found;++i) {
        const MenuText *t=&menu_texts[i];
        if (t->ordinal!=0 || now-t->us>100000) continue;
        if (ux<t->rx || ux>t->rx+t->rw || uy<t->ry || uy>t->ry+t->rh) continue;
        uint64_t menu=0; int32_t count=0;
        if (!safe_read(&menu,t->item+0x80,8) || !safe_read(&count,menu+0x98,4)) continue;
        for (int j=0;j<count && j<64;++j) {
            uint64_t p=0;
            if (safe_read(&p,menu+0xa0+8u*(uint64_t)j,8) && p==t->item) {
                if (menu_skipped(menu,j,now)) break;
                *menu_out=menu; *index_out=j; *count_out=count; found=1; break;
            }
        }
    }
    pthread_mutex_unlock(&menu_text_lock);
    return found;
}
/* How many texts were drawn lately, and how many of those belong to a real menu item (a text drawn while the
 * hooks think an item is being drawn may be a stale tag: the screens that are not menus). */
static void menu_texts_visible(uint64_t now, int *texts, int *rows) {
    *texts=*rows=0;
    pthread_mutex_lock(&menu_text_lock);
    for (int i=0;i<menu_text_count;++i) {
        const MenuText *t=&menu_texts[i];
        if (now-t->us>=100000) continue;
        ++*texts;
        uint64_t menu=0; int32_t count=0;
        if (t->ordinal==0 && safe_read(&menu,t->item+0x80,8) && safe_read(&count,menu+0x98,4) && count>0 && count<64) ++*rows;
    }
    pthread_mutex_unlock(&menu_text_lock);
}
/* The controls inside a row (U1 1.02, from the recorded texts of the row): the two small 10x10 draws are the left
 * and right arrows (the smaller x is the left one), small value texts (height 20..30, width up to 100, from the
 * 4th text on) are the choices of an Oui/Non style row (the left choice is a left press, the right one a right
 * press), a thin wide bar (height up to 12, width from 100) is a slider. kind: 1 left, 2 right, 3 slider. */
typedef struct { int kind; uint64_t menu; int index; uint64_t item; float tx, tw; } MenuControl;
static int menu_control_at(float ux, float uy, uint64_t now, MenuControl *c) {
    int found=0;
    pthread_mutex_lock(&menu_text_lock);
    for (int i=0;i<menu_text_count && !found;++i) {
        const MenuText *t=&menu_texts[i];
        if (t->ordinal<1 || now-t->us>100000) continue;
        int kind=0;
        if (t->w<=14.0f && t->h<=14.0f) {                       /* an arrow (10x10 in the rows, 13x13 in the carousels) */
            if (ux<t->x-8 || ux>t->x+t->w+8 || uy<t->y-8 || uy>t->y+t->h+8) continue;
            int partners=0;
            kind=1;   /* the left arrow, unless another arrow of the row lies further left */
            for (int j=0;j<menu_text_count;++j) {
                const MenuText *o=&menu_texts[j];
                if (j!=i && o->item==t->item && o->ordinal>=1 && o->w<=14.0f && o->h<=14.0f) { ++partners; if (o->x<t->x) kind=2; }
            }
            if (!partners) kind=t->x+t->w*0.5f<640.0f ? 1 : 2;   /* one arrow alone (a carousel end): left half or right half */
            if (t->style[0]<=0.0f) continue;
        } else if (t->ordinal>=3 && t->w<=100.0f && t->h>=20.0f && t->h<=30.0f) {   /* a choice */
            if (ux<t->x || ux>t->x+t->w || uy<t->y || uy>t->y+t->h) continue;
            int left_of_me=0, right_of_me=0;
            for (int j=0;j<menu_text_count;++j) {
                const MenuText *o=&menu_texts[j];
                if (j==i || o->item!=t->item || o->ordinal<3 || o->w>100.0f || o->h<20.0f || o->h>30.0f) continue;
                if (o->x<t->x-1.0f) ++left_of_me; else if (o->x>t->x+1.0f) ++right_of_me;
            }
            kind=right_of_me>0 && left_of_me==0 ? 1 : left_of_me>0 && right_of_me==0 ? 2 : 0;
            if (!kind) continue;
        } else if (t->ordinal>=3 && t->h<12.0f && t->w>=100.0f) { /* a slider */
            if (ux<t->x-6 || ux>t->x+t->w+6 || uy<t->y-12 || uy>t->y+t->h+12) continue;
            kind=3;
        } else continue;
        uint64_t menu=0; int32_t count=0;
        if (!safe_read(&menu,t->item+0x80,8) || !safe_read(&count,menu+0x98,4)) continue;
        for (int j=0;j<count && j<64;++j) {
            uint64_t p=0;
            if (safe_read(&p,menu+0xa0+8u*(uint64_t)j,8) && p==t->item) { c->kind=kind; c->menu=menu; c->index=j; c->item=t->item; c->tx=t->x; c->tw=t->w; found=1; break; }
        }
    }
    pthread_mutex_unlock(&menu_text_lock);
    return found;
}
/* The scrollbar rail of a list: a text draw 8 wide and ~438 tall (the first draws of the list item); returns its rectangle and the
 * list item. The thumb is not readable, so the bar is followed by the list's selected row: the item's model (+8) keeps it at +0x14
 * (counted over all the rows, the section titles too). The number of rows is measured once by scrolling to the end. */
static int menu_rail_find(uint64_t now, float *rx, float *ry, float *rw, float *rh, uint64_t *item) {
    int found=0;
    pthread_mutex_lock(&menu_text_lock);
    for (int i=0;i<menu_text_count && !found;++i) {
        const MenuText *t=&menu_texts[i];
        if (t->ordinal<1 || now-t->us>100000 || t->w<6.0f || t->w>10.0f || t->h<200.0f) continue;
        *rx=t->x; *ry=t->y; *rw=t->w; *rh=t->h; *item=t->item; found=1;
    }
    pthread_mutex_unlock(&menu_text_lock);
    return found;
}
/* The first visible row of a list (found by watching the list item in game): the int at item+0x238 (0 at the top, one more each
 * time the highlight goes past the last visible row), with a scroll animation (item+0x23c a flag, item+0x240 a float that runs
 * from 0 and rests at 1.0). Writing the row and resting the animation moves the list at once without touching the highlight. */
static int list_top_read(uint64_t item) {
    int32_t v=0;
    return safe_read(&v,item+0x238,4) && v>=0 && v<4096 ? (int)v : -1;
}
static void list_top_write(uint64_t item, int top) {
    const int32_t v=top, flag=0; const float rest=1.0f;
    safe_write(item+0x238,&v,4); safe_write(item+0x23c,&flag,4); safe_write(item+0x240,&rest,4);
}
static int menu_list_index(uint64_t item) {
    uint64_t model=0; uint32_t idx=0;
    if (!safe_read(&model,item+8,8) || !safe_read(&idx,model+0x14,4) || idx>4096) return -1;
    return (int)idx;
}
/* The menu's own "select this row" (0x1335a0: menu, index): it asks the row if it can be selected, deselects the old one and selects
 * the new, like a press of up or down but at any distance. Nothing happens for a row that cannot be selected. */
static void menu_select_index(uint64_t menu, int index) {
    if (!u1_image || !menu || index<0) return;
    ((guest_fn2)(uintptr_t)(u1_image+u1m->select_index))(menu,(uint64_t)(uint32_t)index);   /* port: by game code */
    if (mouse_debug()) {   /* port: BB_MOUSE_DEBUG=1: each row the cursor selects (scripted checks) */
        int32_t sel=-1;
        safe_read(&sel,menu+0x150,4);
        printf("Menu mouse: row %d of menu %#llx asked, the menu's selected row is now %d\n",index,(unsigned long long)menu,sel);
    }
}
static int menu_hover;   /* the cursor is over something that answers a click (the golden arrow lights up) */
static uint32_t menu_mouse_buttons(const bool *k, uint32_t mouse, uint64_t now) {
    menu_hover=0;
    static struct {
        int active, target, presses, click_pending, click_action, last_left, drag, queue, last_dir, last_sel, last_target, last_press, list_active, list_click, list_presses,
            sb_drag, sb_measure, sb_same, sb_last, sb_n, sb_cmp, direct_tries, list_armed, jump_fail, dlg_sel, sb_by_wheel, sb_pending;
        uint64_t wseen[2];
        uint32_t jump_done_at_arm;
        int press_dir, freeze_on, freeze_top, sb_m_armed, sb_m_fallback; uint32_t sb_m_done; uint64_t press_until, freeze_item, freeze_end, sb_m_until, sb_m_deadline;
        uint64_t sb_item, sb_cached_item, list_hold, dlg_seen, dlg_cross_at;
        float sb_y, sb_h;
        float last_x, last_y;
        /* the slider being dragged: track x and width (UI space), number of steps, pointer to its value when the
         * game keeps it as a float (0..100 or 0..1), the position we put it at (-1: not known yet) */
        float sl_x, sl_w; int sl_steps, sl_est, sl_extra_max; uint64_t sl_vp;
        uint64_t menu, click_until, click_deadline, step_until, next_step, list_deadline, list_last_step;
        int step_dir;
    } h;
    uint32_t out=0;
    float mx=0, my=0, ux=0, uy=0;
    if (scripted_cursor) {   /* port: BB_PAD_FILE cx= cy= (fractions of the 16:9 menu picture) */
        ux=scripted_ux*UI_WIDTH; uy=scripted_uy*UI_HEIGHT; mx=ux; my=uy;
    } else {
        SDL_Window *w=SDL_GetMouseFocus();
        int ww=0, wh=0;
        if (!w) return 0;
        SDL_GetMouseState(&mx,&my);
        SDL_GetWindowSize(w,&ww,&wh);
        if (ww<=0 || wh<=0) return 0;
        if (bbgpu_frame_fills_window()) {   /* a play frame over the whole window */
            ux=mx/(float)ww*UI_WIDTH; uy=my/(float)wh*UI_HEIGHT;
            /* (16:10, Vert+: its UI is squeezed back to 16:9 vertically, runtime_ultrawide_scale < 0: the UI's height is that share of the window's) */
            if (runtime_ultrawide_scale<0.0f) { const float sv=-runtime_ultrawide_scale; uy=(my-(float)wh*(1.0f-sv)*0.5f)/((float)wh*sv)*UI_HEIGHT; }
            /* (ultrawide, Hor+: the UI is squeezed back to 16:9 sideways, 0 < scale < 1: the UI's width is that share of the window's; the pause menus over
             * gameplay were read as stretched over the whole width: off by 60 at the Controls page's values on 21:9) */
            else if (runtime_ultrawide_scale>0.0f && runtime_ultrawide_scale<0.999f) { const float sh=runtime_ultrawide_scale; ux=(mx-(float)ww*(1.0f-sh)*0.5f)/((float)ww*sh)*UI_WIDTH; }
        }
        else {
            const float scale=fminf((float)ww/UI_WIDTH,(float)wh/UI_HEIGHT);
            ux=(mx-((float)ww-UI_WIDTH*scale)*0.5f)/scale; uy=(my-((float)wh-UI_HEIGHT*scale)*0.5f)/scale;
        }
    }
    const int moved=fabsf(mx-h.last_x)>=1.0f || fabsf(my-h.last_y)>=1.0f;
    h.last_x=mx; h.last_y=my;
    if (moved && !h.drag) { h.active=1; h.presses=0; }
    if (mouse_debug() && moved) { static uint64_t shown; if (now-shown>300000) { shown=now; printf("Menu mouse: cursor at %.0f %.0f (menu space)\n",ux,uy); } }   /* port: debug */
    if (now<wheel_until_us[0] || now<wheel_until_us[1] ||
        (k && (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_S] || k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_DOWN]))) h.active=0;
    const int left=(mouse & 1u)!=0, clicked=left && !h.last_left;
    h.last_left=left;
    if (!left) { h.drag=0; h.queue=0; }
    /* A confirmation (Oui / Non): the cursor over a choice selects it (once, from where the selection is believed to be: "Non" when
     * the box opens), a click selects it and then presses cross, the wheel and the keys go left and right. Nothing else on the
     * screen reacts to the mouse there (a click elsewhere in the box must not validate the selected choice). */
    {
        DialogInfo dlg;
        if (menu_dialog_find(now,&dlg)) {
            if (now-h.dlg_seen>300000) { h.dlg_sel=2; h.dlg_cross_at=0; h.step_until=0; h.click_until=0; h.click_pending=0; h.queue=0; }
            h.dlg_seen=now;
            int which=0, pressed=0;
            if (ux>=dlg.lx-12.0f && ux<=dlg.lx+dlg.lw+12.0f && uy>=dlg.ly-12.0f && uy<=dlg.ly+dlg.lh+12.0f) which=1;
            else if (ux>=dlg.rx-12.0f && ux<=dlg.rx+dlg.rw+12.0f && uy>=dlg.ry-12.0f && uy<=dlg.ry+dlg.rh+12.0f) which=2;
            if (k) {
                if (k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT]) h.dlg_sel=1;
                if (k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) h.dlg_sel=2;
            }
            if (now<wheel_until_us[0]) h.dlg_sel=1; else if (now<wheel_until_us[1]) h.dlg_sel=2;
            if (which) menu_hover=1;
            if (which && (moved || clicked) && which!=h.dlg_sel) { h.step_dir=which; h.step_until=now+100000; h.dlg_sel=which; pressed=1; }
            if (clicked && which) {
                h.dlg_cross_at=now+(pressed ? 260000u : 30000u);
                printf("Runtime: menu confirmation click on the %s choice\n",which==1 ? "left" : "right");
            }
            if (h.dlg_cross_at && now>=h.dlg_cross_at) { h.dlg_cross_at=0; h.click_until=now+120000; }
            uint32_t o=0;
            if (now<h.step_until) o|=h.step_dir==1 ? BTN_LEFT : BTN_RIGHT;
            if (now<h.click_until) o|=BTN_CROSS;
            return o;
        }
    }
    /* The scrollbar of a list: press on the rail and drag. The first time on a list its length is measured (down to the end:
     * the selected row stops growing or wraps), then the selected row follows the cursor along the rail. */
    int sb_out=0;
    {
        float rx, ry, rw, rh; uint64_t ritem=0;
        const int have_rail=menu_rail_find(now,&rx,&ry,&rw,&rh,&ritem);
        if (!left && !h.sb_by_wheel) h.sb_drag=0;
        u1_rail_wheel=have_rail;
        if (h.sb_by_wheel && !have_rail) { h.sb_drag=0; h.sb_by_wheel=0; h.sb_pending=0; }
        {   /* the wheel scrolls the list's view (the bar's thumb) like a drag of the bar would: a notch is one row; the highlight stays where it is */
            int notch=0;
            for (int i=0;i<2;++i) if (wheel_until_us[i]!=h.wseen[i]) { h.wseen[i]=wheel_until_us[i]; notch+=i==0 ? -1 : 1; }
            if (have_rail && notch && !left && menu_list_index(ritem)>=0) {
                if (!h.sb_drag) {
                    h.sb_drag=1; h.sb_by_wheel=1; h.sb_pending=0; h.sb_item=ritem; h.sb_y=ry; h.sb_h=rh; h.sb_cmp=0; h.sb_same=0; h.sb_last=menu_list_index(ritem);
                    h.sb_measure=(h.sb_cached_item!=ritem);
                    if (h.sb_measure) h.sb_n=0;
                    h.sb_m_armed=0; h.sb_m_fallback=0;
                }
                if (h.sb_by_wheel) h.sb_pending+=notch;
            }
        }
        if (have_rail && ux>=rx-10.0f && ux<=rx+rw+10.0f && uy>=ry-4.0f && uy<=ry+rh+4.0f) menu_hover=1;
        if (have_rail && clicked && ux>=rx-10.0f && ux<=rx+rw+10.0f && uy>=ry-4.0f && uy<=ry+rh+4.0f && menu_list_index(ritem)>=0) {
            h.sb_drag=1; h.sb_item=ritem; h.sb_y=ry; h.sb_h=rh; h.sb_cmp=0; h.sb_same=0; h.sb_last=menu_list_index(ritem);
            h.sb_measure=(h.sb_cached_item!=ritem);
            if (h.sb_measure) h.sb_n=0;
            h.sb_m_armed=0; h.sb_m_fallback=0;
            printf("Runtime: scrollbar drag on list item %#llx, rows %s\n",(unsigned long long)ritem,h.sb_measure ? "to measure" : "known");
        }
        if (h.sb_drag) {
            const int idx=menu_list_index(h.sb_item);
            if (idx<0) h.sb_drag=0;
            else if (h.sb_measure && !h.sb_m_fallback) {
                /* one press carries the whole measure (see u1_menu_event); no result in half a second: the slow way below */
                uint64_t lmenu=0;
                if (!h.sb_m_armed) {
                    if (safe_read(&lmenu,h.sb_item+0x80,8) && lmenu) {
                        u1_measure_n=0; u1_jump_menu=lmenu; u1_jump_type=2; u1_jump_us=now; u1_measure_item=h.sb_item;
                        h.sb_m_armed=1; h.sb_m_done=u1_measure_done; h.sb_m_until=now+50000; h.sb_m_deadline=now+500000;
                    } else h.sb_m_fallback=1;
                } else if (u1_measure_done!=h.sb_m_done) {
                    if (u1_measure_n>0) { h.sb_n=u1_measure_n; h.sb_measure=0; h.sb_cached_item=h.sb_item; printf("Runtime: scrollbar: the list has %d rows (measured in one go)\n",h.sb_n); }
                    else h.sb_m_fallback=1;
                } else if (now>h.sb_m_deadline) { u1_measure_item=0; h.sb_m_fallback=1; }
                if (h.sb_measure && !h.sb_m_fallback && now<h.sb_m_until) sb_out=BTN_DOWN;
            }
            else if (h.sb_measure) {
                /* down until the row stops growing or wraps round to the top */
                if (idx>h.sb_last && idx!=h.sb_last) { h.sb_same=0; h.sb_last=idx; }
                else if (idx<h.sb_last) { h.sb_n=h.sb_last+1; h.sb_measure=0; h.sb_cached_item=h.sb_item; }
                else if (++h.sb_same>24) { h.sb_n=idx+1; h.sb_measure=0; h.sb_cached_item=h.sb_item; }
                if (!h.sb_measure) printf("Runtime: scrollbar: the list has %d rows\n",h.sb_n);
                else if ((now/16000u)&1u) sb_out=BTN_DOWN;
            }
            if (!h.sb_measure && h.sb_drag && h.sb_by_wheel) {
                ListInfo li; const int top=list_top_read(h.sb_item);
                if (menu_list_find_ex(now,&li,0) && li.pitch>1.0f && top>=0) {
                    const int visible=(int)floorf((li.bottom-li.top)/li.pitch+0.5f);
                    const int max_top=h.sb_n-visible>0 ? h.sb_n-visible : 0;
                    int want=top+h.sb_pending;
                    want=want<0 ? 0 : want>max_top ? max_top : want;
                    if (want!=top) list_top_write(h.sb_item,want);
                }
                h.sb_pending=0; h.sb_drag=0; h.sb_by_wheel=0;
            } else if (!h.sb_measure && h.sb_drag) {
                float f=(uy-h.sb_y)/h.sb_h; f=f<0.0f ? 0.0f : f>1.0f ? 1.0f : f;
                ListInfo li;
                if (menu_list_find_ex(now,&li,0) && li.pitch>1.0f && list_top_read(h.sb_item)>=0) {   /* the highlighted row may be out of view: not needed here */
                    /* the bar scrolls the view (first visible row), the highlight stays where it is */
                    const int visible=(int)floorf((li.bottom-li.top)/li.pitch+0.5f);
                    const int max_top=h.sb_n-visible>0 ? h.sb_n-visible : 0;
                    const int want=(int)floorf(f*(float)max_top+0.5f);
                    if (list_top_read(h.sb_item)!=want) list_top_write(h.sb_item,want);
                } else {
                const int target=(int)floorf(f*(float)(h.sb_n>1 ? h.sb_n-1 : 1)+0.5f);
                if (idx!=target) {
                    const int dir=idx<target ? 1 : -1;
                    if (h.sb_cmp!=0 && dir!=h.sb_cmp) h.sb_cmp=0;   /* the cursor moved to the other side: go the other way */
                    h.sb_cmp=dir;
                    if ((now/16000u)&1u) sb_out=dir>0 ? BTN_DOWN : BTN_UP;
                }
                }
            }
        }
    }
    /* A scrolling list: the cursor over its rows moves the highlight there (up or down presses until the highlighted row
     * is at the cursor), a click then validates. */
    ListInfo list={0};
    int list_ctl=0, list_delta=0, list_title=0, list_noclick=0;
    int list_found=menu_list_find_ex(now,&list,0);
    if (list_found && list.sel_cy<0.0f) {   /* the highlighted row is out of view (scrolled away with the bar): where it would be */
        const int top=list_top_read(list.item), idx=menu_list_index(list.item);
        if (top>=0 && idx>=0) list.sel_cy=list.top+12.6f+(float)(idx-top)*list.pitch; else list_found=0;
    }
    if (list_found && ux>=list.colx-20.0f && ux<=list.colx+560.0f && uy>=list.top-4.0f && uy<=list.bottom+4.0f) {
        list_ctl=1;
        list_delta=(int)floorf((uy-list.sel_cy)/list.pitch+0.5f);
        /* Statistiques (a list with a column of values on the right): only the difficulties of Contre-la-montre (rows with an empty value) open
         * something. A click on a statistic does nothing, and the section titles (dimmer: style[1] 0.5) are never selected by the game, so
         * the cursor over one must not chase it (the highlight shook between the rows around it). */
        int stats=0, label=-1, value_w=0, bright_label=0, nvals=0, titles=0;
        pthread_mutex_lock(&menu_text_lock);
        for (int i=0;i<menu_text_count;++i) {
            const MenuText *t=&menu_texts[i];
            if (t->item!=list.item || now-t->us>100000 || t->ordinal<1) continue;
            if (t->x>list.colx+300.0f && t->w>0.5f && t->h<30.0f) ++nvals;
            if (t->h>=22.0f && t->h<=29.0f && fabsf(t->x-list.colx)<1.0f && t->style[1]<0.7f && t->style[5]<2.5f && !line_has_value(t,list.item,list.colx,now)) ++titles;   /* (a section title: dim, no value on its line) */
            if (t->h>=22.0f && t->h<=29.0f && fabsf(t->x-list.colx)<1.0f && t->style[1]>=0.7f) bright_label=1;   /* (Récompenses: every label, the highlighted one too, is at style[1] 0.5) */
            if (fabsf(t->x-list.colx)<1.0f && t->h>=22.0f && t->h<=29.0f && uy>=t->y-2.0f && uy<t->y+list.pitch-2.0f) label=i;
        }
        /* (the pause menu's costume list has a value column too (???) but no dim section titles: its rows can be chosen) */
        stats=nvals>=3;   /* (Médailles has one lone icon on the right, not a column of values) */
        if (!bright_label || !titles) stats=0;  /* Récompenses: every label is dim (style[1] 0.5) next to a value column; only Statistiques mixes dim titles with normal rows */
        if (label>=0) {
            const MenuText *l=&menu_texts[label];
            for (int i=0;i<menu_text_count;++i) {
                const MenuText *t=&menu_texts[i];
                if (t->item==list.item && now-t->us<=100000 && t->ordinal>=1 && t->x>list.colx+300.0f && t->h<30.0f && fabsf(t->y-l->y)<2.0f && t->w>0.5f) value_w=1;
            }
            if (stats) {
                if (l->style[1]<0.7f && l->style[5]<2.5f) { list_title=1; list_noclick=1; list_delta=0; }
                else if (value_w) list_noclick=1;
            }
        }
        pthread_mutex_unlock(&menu_text_lock);
        if (clicked && mouse_debug())
            printf("Runtime: list click ui=(%.0f %.0f): stats %d (values %d, bright %d, titles %d), label %d value %d title %d noclick %d, delta %d\n",
                   ux,uy,stats,nvals,bright_label,titles,label,value_w,list_title,list_noclick,list_delta);
    }
    if (list_found) {   /* Statistiques, highlight on a statistic (a row with a value): no "SÉLECTIONNER" in the prompt bar, see u1_bar_flags */
        int stats=0, sel_value=0, bright_label=0, nvals=0, titles=0;
        pthread_mutex_lock(&menu_text_lock);
        const MenuText *sl=NULL;
        for (int i=0;i<menu_text_count;++i) {
            const MenuText *t=&menu_texts[i];
            if (t->item!=list.item || now-t->us>100000 || t->ordinal<1) continue;
            if (t->x>list.colx+300.0f && t->w>0.5f && t->h<30.0f) ++nvals;
            if (t->h>=22.0f && t->h<=29.0f && fabsf(t->x-list.colx)<1.0f && t->style[1]>=0.7f) bright_label=1;
            if (fabsf(t->x-list.colx)<1.0f && t->h>=22.0f && t->h<=29.0f && t->style[5]>2.5f) sl=t;
            if (t->h>=22.0f && t->h<=29.0f && fabsf(t->x-list.colx)<1.0f && t->style[1]<0.7f && t->style[5]<2.5f && !line_has_value(t,list.item,list.colx,now)) ++titles;
        }
        stats=nvals>=3 && bright_label && titles;
        if (sl) for (int i=0;i<menu_text_count;++i) {
            const MenuText *t=&menu_texts[i];
            if (t->item==list.item && now-t->us<=100000 && t->ordinal>=1 && t->x>list.colx+300.0f && t->h<30.0f && fabsf(t->y-sl->y)<2.0f && t->w>0.5f) sel_value=1;
        }
        pthread_mutex_unlock(&menu_text_lock);
        if (stats && sel_value) u1_hide_select_us=now;
    }
    uint64_t menu=0; int index=-1, count=0;
    const int on_row=list_ctl ? 0 : menu_row_at(ux,uy,now,&menu,&index,&count);
    MenuControl ctl={0,0,-1,0,0,0};
    const int on_control=!h.drag && !list_ctl && menu_control_at(ux,uy,now,&ctl);
    if (list_ctl || on_row || on_control) menu_hover=1;
    if (list_ctl) {
        if (moved) { h.list_active=1; h.list_presses=0; }
        if (now<wheel_until_us[0] || now<wheel_until_us[1] ||
            (k && (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_S] || k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_DOWN]))) h.list_active=0;
        if (clicked && !list_noclick) { h.list_click=1; h.list_deadline=now+1500000; h.list_active=1; h.list_presses=0; }
        if (list_title) h.list_active=0;
    } else h.list_click=0;
    /* Hovering a row at the edge of a list must not scroll it: the first row shown is noted when the cursor starts to move the
     * highlight, and put back whenever the game moves it meanwhile (until a moment after the highlight is under the cursor, or the
     * wheel or the keys take over). */
    if (list.item) {
        const int top=list_top_read(list.item);
        const int user_scroll=now<wheel_until_us[0] || now<wheel_until_us[1] ||
            (k && (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_S] || k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_DOWN]));
        if (user_scroll || h.sb_drag || (h.freeze_on && h.freeze_item!=list.item)) h.freeze_on=0;
        if (!h.freeze_on && list_ctl && (h.list_active || h.list_click) && list_delta!=0 && top>=0 && !user_scroll && !h.sb_drag) {
            h.freeze_on=1; h.freeze_top=top; h.freeze_item=list.item; h.freeze_end=0;
        }
        if (h.freeze_on) {
            if (top>=0 && top!=h.freeze_top) {
                list_top_write(list.item,h.freeze_top);
                printf("Runtime: list view kept at row %d (the game moved it to %d)\n",h.freeze_top,top);
            }
            if (list_delta==0) { if (!h.freeze_end) h.freeze_end=now+400000; } else h.freeze_end=0;
            if (h.freeze_end && now>h.freeze_end) h.freeze_on=0;
        }
    } else h.freeze_on=0;
    if (clicked) {
        int texts, rows;
        menu_texts_visible(now,&texts,&rows);
        if (mouse_debug()) printf("Runtime: menu click ui=(%.0f %.0f) on_row=%d control=%d texts=%d rows=%d\n",ux,uy,on_row,on_control ? ctl.kind : 0,texts,rows);
        if (mouse_debug() && (texts<=4 || (!on_row && !on_control && !list_ctl))) {   /* (also a click on nothing known: what is drawn there) */ {   /* a confirmation box has few texts: list them (to tell it from the other screens) */
            pthread_mutex_lock(&menu_text_lock);
            for (int i=0;i<menu_text_count;++i) { const MenuText *t=&menu_texts[i]; if (now-t->us<100000) printf("Runtime:   text item ord=%d x=%.1f y=%.1f w=%.1f h=%.1f\n",t->ordinal,t->x,t->y,t->w,t->h); }
            for (int i=0;i<free_text_count;++i) { const MenuText *t=&free_texts[i]; if (now-t->us<100000) printf("Runtime:   text free x=%.1f y=%.1f w=%.1f h=%.1f\n",t->x,t->y,t->w,t->h); }
            pthread_mutex_unlock(&menu_text_lock); }
        }
        if (h.sb_drag) {
            /* the press is on a scrollbar: nothing else takes it (the selected row's sub-menu must not open) */
        } else if (on_control) {
            h.target=ctl.index; h.menu=ctl.menu; h.click_pending=1; h.click_deadline=now+1500000; h.active=1; h.presses=0;
            h.click_action=ctl.kind;
            if (ctl.kind==3) {
                uint64_t model=0, vp=0, steps=0;
                h.drag=1; h.sl_x=ctl.tx; h.sl_w=ctl.tw; h.sl_steps=10; h.sl_est=-1; h.sl_vp=0; h.queue=0;
                if (safe_read(&model,ctl.item+8,8) && safe_read(&vp,model+0x18,8)) {
                    if (vp>=0x1000000ull) h.sl_vp=vp;               /* a float behind a pointer (brightness, sensitivity) */
                    else if (vp>=2 && vp<=64) {                       /* the volumes: the number of steps; the four values are floats 0..1 */
                        uint32_t slot=99;                               /* in an array of the data segment (1.02), in the order of the rows */
                        h.sl_steps=(int)vp;
                        if (u1_image && u1_volumes && safe_read(&slot,model+0x14,4) && slot<4) h.sl_vp=u1_image+u1_volumes+4u*slot;   /* port: by game code */
                    }
                }
                (void)steps;
                printf("Runtime: menu slider start steps=%d value ptr=%#llx track=(%.1f %.1f)\n",h.sl_steps,(unsigned long long)h.sl_vp,h.sl_x,h.sl_w);
            }
        } else if (on_row) {
            h.target=index; h.menu=menu; h.click_pending=1; h.click_action=0; h.click_deadline=now+1500000; h.active=1; h.presses=0;
        } else if (rows==0 && !list_ctl) h.click_until=now+150000;   /* no menu rows: "press a key to start", the game selection */
    }
    if (on_row && h.active && !h.click_pending && !h.drag) { h.target=index; h.menu=menu; }
    if (!on_row && !on_control && !h.click_pending && !h.drag) h.menu=0;
    int arrived=0;
    if (h.target!=h.last_target) { h.last_target=h.target; h.last_dir=0; h.last_press=0; h.last_sel=-1; h.presses=0; h.direct_tries=0; }
    if (h.menu && (h.active || h.click_pending || h.drag)) {
        int32_t sel=-1;
        if (safe_read(&sel,h.menu+0x150,4) && sel>=0) {
            if (sel==h.target) {
                h.presses=0; arrived=1; h.last_dir=0; h.last_press=0; h.direct_tries=0;
                if (h.click_pending) {
                    h.click_pending=0;
                    if (h.click_action==0) h.click_until=now+120000;
                    else if (h.click_action==1 || h.click_action==2) { h.step_dir=h.click_action; h.step_until=now+120000; }
                }
            } else if ((h.last_dir!=0 && ((sel<h.target) ? 1 : -1)!=h.last_dir && sel!=h.last_sel) ||
                       (h.last_press!=0 && sel!=h.last_sel && (sel-h.last_sel)*h.last_press<0)) {
                /* the selection went past the row without stopping on it, or wrapped round the other way (a row at the
                 * start or the end of the menu): the game does not select this row */
                menu_skip(h.menu,h.target,now);
                h.active=0; h.click_pending=0; h.drag=0; h.presses=0; h.last_dir=0; h.menu=0;
            } else if (h.presses<24 && u1_image && h.direct_tries<4) {
                menu_select_index(h.menu,h.target);   /* straight to the row; a row that does not take it falls back to the presses below */
                ++h.direct_tries;
            } else if (h.presses<24 && ((now/50000u)&1u)) {
                h.last_dir=(sel<h.target) ? 1 : -1; h.last_sel=sel; h.last_press=h.last_dir;
                out|=(sel<h.target) ? BTN_DOWN : BTN_UP;
                static uint64_t last_step;
                if (now-last_step>90000) { last_step=now; ++h.presses; }
            } else if (h.presses>=24) { h.active=0; h.click_pending=0; h.drag=0; }
        }
        if (h.click_pending && now>h.click_deadline) h.click_pending=0;
    }
    /* A slider dragged with the left button held: the bar goes where the cursor is. The cursor's place on the track
     * gives the wanted step (0..steps). When the game keeps the value as a float (brightness 0..100, sensitivity
     * 0..1) it is read and the difference is pressed; otherwise (the volumes) the bar is first taken to the left end
     * with steps+1 presses, then the wanted number of right presses is added, and later moves press the
     * difference from where we put it. */
    if (h.drag && arrived && h.sl_w>1.0f) {
        float fraction=(ux-h.sl_x)/h.sl_w;
        fraction=fraction<0.0f ? 0.0f : fraction>1.0f ? 1.0f : fraction;
        const int want=(int)floorf(fraction*(float)h.sl_steps+0.5f);
        int est;
        if (h.sl_vp) {
            float v=0.0f;
            if (safe_read(&v,h.sl_vp,4)) {
                const float range=v>1.0001f ? 100.0f : 1.0f;
                est=(int)floorf(v/range*(float)h.sl_steps+0.5f)+h.queue;
            } else est=want;
        } else {
            if (h.sl_est<0) { h.queue-=h.sl_steps+1; h.sl_est=0; }
            est=h.sl_est+h.queue+(h.sl_steps+1 > 0 ? 0 : 0);
            est=h.sl_est;
        }
        if (h.sl_vp) h.queue+=want-est;
        else {
            h.queue+=want-h.sl_est; h.sl_est=want;
            if (want==h.sl_steps && h.sl_extra_max==0) { h.queue+=4; h.sl_extra_max=1; }   /* a press may be lost: be sure of the end */
            if (want!=h.sl_steps) h.sl_extra_max=0;
        }
        if (h.queue>60) h.queue=60;
        if (h.queue<-60) h.queue=-60;
    }    if (h.queue!=0 && now>=h.step_until && now>=h.next_step) {
        h.step_dir=h.queue>0 ? 2 : 1;
        h.step_until=now+35000; h.next_step=now+70000;
        const int n=h.queue>0 ? h.queue : -h.queue;
        if (h.drag && n>1 && h.menu && !u1_jump_steps && !u1_jump_busy) {   /* a slider far from the cursor: one real press (event 3 left, 4 right) carries the other steps, see u1_menu_event */
            u1_jump_menu=h.menu; u1_jump_type=h.queue>0 ? 4 : 3; u1_jump_us=now; u1_jump_steps=n-1;
            h.queue=h.queue>0 ? 1 : -1;
            h.next_step=now+45000;
        }
        h.queue+=h.queue>0 ? -1 : 1;
    }
    if (now<h.step_until) out|=h.step_dir==1 ? BTN_LEFT : BTN_RIGHT;
    out|=(uint32_t)sb_out;
    if (list_ctl && list_delta!=0 && !h.sb_drag) {
        /* never press past either end of the list: Up on the first row or Down on the last wraps round to the other end and the scroll bar's
         * thumb leaps across (the cursor moving fast while the highlight lags behind a frame or two asks for exactly that) */
        const int idx=menu_list_index(list.item);
        if (idx>=0) {
            if (list_delta<0 && -list_delta>idx) list_delta=-idx;
            else if (list_delta>0 && h.sb_cached_item==list.item && h.sb_n>0 && list_delta>h.sb_n-1-idx) list_delta=h.sb_n-1-idx>0 ? h.sb_n-1-idx : 0;
        }
    }
    if (!h.sb_drag && list_ctl && (h.list_active || h.list_click)) {
        if (list_delta==0) {
            h.list_presses=0; u1_jump_steps=0; h.list_armed=0;
            if (h.list_click) { h.list_click=0; h.click_until=now+120000; }
        } else if (h.list_presses<40 && ((now/16000u)&1u) && now>=h.list_hold) {
            /* more than one row away: the first press carries the other steps (see u1_menu_event); until the highlight is seen
             * to have moved, no further press is sent. A jump that never happens (3 times) leaves the plain presses. */
            const int distance=list_delta<0 ? -list_delta : list_delta;
            uint64_t lmenu=0;
            if (h.jump_fail>=3 && now-h.list_hold>3000000) h.jump_fail=0;   /* give the jump another chance later */
            if (distance>1 && h.jump_fail<3 && list.item && safe_read(&lmenu,list.item+0x80,8) && lmenu) {
                if (h.list_armed) {
                    if (u1_jump_done==h.jump_done_at_arm) {
                        ++h.jump_fail;
                        if (mouse_debug()) printf("Runtime: list jump not taken (%d): armed menu %#llx, last event menu %#llx, last non-zero event type %d (%d seen), wanted %d, steps left %d\n",h.jump_fail,(unsigned long long)u1_jump_menu,(unsigned long long)u1_event_last_menu,u1_event_last_type,u1_event_count,u1_jump_type,u1_jump_steps);
                    } else h.jump_fail=0;
                }
                h.jump_done_at_arm=u1_jump_done; h.list_armed=1;
                u1_jump_menu=lmenu; u1_jump_type=list_delta>0 ? 2 : 1; u1_jump_us=now;
                u1_jump_steps=(distance>24 ? 24 : distance)-1;
                h.list_hold=now+160000;
                h.press_until=now+50000; h.press_dir=list_delta>0 ? 1 : -1;   /* the press is held long enough for the game to see it */
            }
            out|=list_delta>0 ? BTN_DOWN : BTN_UP;
            if (now-h.list_last_step>90000) { h.list_last_step=now; ++h.list_presses; }
        } else if (h.list_presses>=40) { h.list_active=0; h.list_click=0; }
        if (h.list_click && now>h.list_deadline) h.list_click=0;
    }
    if (now<h.press_until && list_ctl && list_delta!=0) out|=h.press_dir>0 ? BTN_DOWN : BTN_UP;
    if (now<h.click_until) out|=BTN_CROSS;
    return out;
}

/* ---- Uncharted 3 ------------------------------------------------------------------------------
 * After antikmewzik's U2/U3 menu code (u2_text_probe, u2_row_note, u3_menu_buttons) and this port's own U3 page jump (the test
 * port of his menus, 2026-10-28, test/antik-u3-menus): the menu texts are drawn by one routine (rdx the style, rcx the laid-out
 * text, r8 the rectangle, 1280x720 space): the rows are 24-42 tall, the selected one has style[6] at 1. A menu page (a container
 * updated every frame on the game's thread) keeps its rows at +0xb0 (count +0xac); a row has its focus byte at +8, its rectangle
 * at +0x90 and in its vtable +0x30 "set the focus" (0 lose, 1 take), +0x40 "can take the focus". The cursor over a row asks the
 * page to move its focus there (on the game's thread, at the page's own update), a click on the focused row presses cross, the
 * wheel goes up and down, the right or side button goes back. Both game codes: 1.02 (US/EU 1.02) and 1.00 (EU/US 1.00, compiled
 * differently: the two functions located by their code and the page layout; their arguments are the same). Not taken yet: his
 * sliders by position, the long lists' scroll bars, the chapter carousel and the treasure models (their 1.00 code is not located). */
typedef struct { float x, y, w, h, sel; uint64_t us; int front; } U3Row;   /* front: U2 text drawn emphasised (style[5] 3: the lit row, a Yes/No's choices) */
#define U3_ROWS 192
static U3Row u3_rows[U3_ROWS];
/* U3: the < > of a settings row are two texts 10-20 wide and 33.6 tall side by side (antikmewzik: x 378 and 394 on the audio
 * volume rows), narrower than a row; kept here (u3_lock) */
static U3Row u3_arrows[16];
/* U2/U3: the rails of the sliders (sensitivity, volumes): a text whose picture is 100+ (U3: 60+) wide and at most 12 tall, laid out
 * in a row's rectangle (antikmewzik's u2_slider_note / u3_slider_note); x, y and h of the row, w = the rail's width (u3_lock) */
static U3Row u3_sliders[8];
/* U3's ghost rows (antikmewzik): a row whose label text is empty (its picture 0 wide) in the left column (x 96), drawn but not meant to
 * be chosen (Audio output, under "Front"): the hover skips them. And the credits (U2 / U3): text 700-800 tall, never highlighted:
 * the menu mode stays on while they show (Esc / back still work). */
static float u3_ghost_y[8];
static uint64_t u3_ghost_us[8];
static volatile uint64_t menu_credits_us;
/* U3's art gallery viewer (Bonus > Rewards > art galleries; antikmewzik): its "< >" line is a text 1080 x 60 at 100, 600; a click on
 * the arrow zones (x 92-146 / 1134-1188, y 596-664) pushes the left stick that way for 150 ms (the viewer reads the stick) */
static volatile uint64_t u3_gallery_us, gallery_stick_until;
static volatile int gallery_stick_dir;
int runtime_menumouse_stick_x(void) { return now_us()<gallery_stick_until ? gallery_stick_dir : 0; }
static pthread_mutex_t u3_lock=PTHREAD_MUTEX_INITIALIZER;
static volatile uint64_t u3_page_us;     /* the last update of a focused menu page */
static volatile uint64_t u3_info_us;     /* an information box (a locked reward) is over the menu */
/* U2: the big pictures of a page (Chapter select's picture: 520 x 205 at 380, 210): their big < > are drawn just outside them, not as
 * texts (u3_lock) */
static U3Row u2_panels[8];
static volatile uint64_t u2_popup_us;    /* U2's chapter carousel (Chapter select): its panel is a "text" 683.8 x 318.8 at 298.1, 156.2 (antikmewzik) */
/* U3's Yes/No (the pause menu's Quit, return to checkpoint): both choices are drawn lit (style[6] 1); the chosen one has the
 * highlight bar drawn around it, an unlit text box a few units larger on each side (520 x 40 around 512 x 32). u3_lock held. */
static int u3_in_bar(const U3Row *t, uint64_t now) {
    for (int i=0;i<U3_ROWS;++i) {
        const U3Row *b=&u3_rows[i];
        if (!b->us || now-b->us>=100000 || b->sel>=0.5f || b->w<=t->w || b->w-t->w>16.0f || b->h-t->h>16.0f) continue;
        if (b->x<=t->x+0.5f && b->y<=t->y+0.5f && b->x+b->w>=t->x+t->w-0.5f && b->y+b->h>=t->y+t->h-0.5f) return 1;
    }
    return 0;
}
static volatile int u3_jump_req, u3_jump_done;   /* the hover asks (pad thread), a page answers (game thread): 1 done, -1 not possible, -2 nothing selectable */
static volatile float u3_jump_cx, u3_jump_cy;
static uint64_t u3_jump_page, u3_jump_us; static int u3_jump_row=-1;
typedef struct {
    const char *version;
    uint64_t text, page;
} U3MenuCode;
static const U3MenuCode u3_menu_codes[]={{"1.02",0x63d6f0,0x6253f0},{"1.00",0x1d6060,0x1c5000}};
static const unsigned char u3_text_102_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x28,0x49,0x89,0xcc,
                                               0x48,0x89,0xd3,0x49,0x89,0xf7,0x49,0x89,0xfe,0x48,0x8d,0x0d};
static const unsigned char u3_text_100_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x38,0x45,0x89,0xce,
                                               0x4d,0x89,0xc5,0x49,0x89,0xcf,0x48,0x89,0xd3,0x48,0x89,0x75};
static const unsigned char u3_page_102_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,
                                               0x48,0x49,0x89,0xf5,0x49,0x89,0xff,0x48,0x8d,0x05};
static const unsigned char u3_page_100_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,
                                               0x48,0x49,0x89,0xf7,0x49,0x89,0xfd,0x48,0x8d,0x05};
static const U3MenuCode *u3m;
/* Uncharted 2: the same menu text routine (U3's was made from it; 1.00 compiled like U3's 1.00, located by its code), the selected row
 * marked by style[5] at 3 (U3: style[6] at 1). No page hook here yet: the hover steps the highlight with the d-pad onto the row under
 * the cursor (as antikmewzik's first U3 menus did), two columns (the main menu) by left and right. */
typedef struct { const char *version; uint64_t text; } U2MenuCode;
static const U2MenuCode u2_menu_codes[]={{"1.02",0x5b56d0},{"1.00",0x28ddf0}};
static const unsigned char u2_text_102_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x28,0x49,0x89,0xcc,
                                               0x48,0x89,0xd3,0x49,0x89,0xf7,0x49,0x89,0xfe,0x48,0x8b,0x0d};
static const unsigned char u2_text_100_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x38,0x45,0x89,0xce,
                                               0x4d,0x89,0xc5,0x49,0x89,0xcf,0x48,0x89,0xd3,0x48,0x89,0x75};
static const U2MenuCode *u2m;
typedef int64_t (__attribute__((sysv_abi)) *guest_vcall_fn)(uint64_t);
typedef void (__attribute__((sysv_abi)) *guest_set_fn)(uint64_t, int);
/* U2 1.02's list control update (0x57e410, antikmewzik's u2_widget_hook): FUN_0057e410(widget, frame) runs every frame for each
 * selector / list. The widget's value is in a data source at widget+0x18 (vtable +0 get, +8 set(value), +0x30 minimum, +0x38
 * maximum); +0x248 is the first row seen, +0x244 the rows seen, +0x22c / +0x228 the value it followed last. A hover on a list row
 * sets the value by the game's own call here, on its thread (EU 1.00: the same function at 0x267f60). */
typedef struct { uint64_t w, ds; int cur, lo, hi, top, vis; uint64_t us; float rx, ry, rw, rh; int focused; } U2List;   /* focused: the control's flag at +8 (as a page's children) */   /* r*: the control's rectangle (+0x90) */
static U2List u2_lists[16];
static uint64_t u2_dropdown_w;   /* the open drop-down list control (see u2_list_update), 0: none */
static volatile uint64_t u2_set_ds;
static volatile int u2_set_val, u2_set_pending, u2_set_keep;
static volatile uint64_t u2_set_us;   /* (a request no list took: dropped after 300 ms) */
/* the scroll bar (antikmewzik's): it moves the view only (the first row seen, +0x248), written here on the game's thread */
static volatile uint64_t u2_scroll_w;
static volatile int u2_scroll_top, u2_scroll_pending;
static int __attribute__((unused)) u2_list_pick(uint64_t now, float ux, float uy, U2List *out) {   /* the list under the cursor: a fresh control holding it, the smallest */
    int best=-1;
    pthread_mutex_lock(&u3_lock);
    for (int i=0;i<16;++i) {
        const U2List *l=&u2_lists[i];
        if (!l->us || now-l->us>150000 || l->hi-l->lo<1 || l->cur<l->lo || l->cur>l->hi) continue;   /* (short lists too: all their rows seen) */
        if (ux<l->rx || ux>l->rx+l->rw || uy<l->ry || uy>l->ry+l->rh) continue;
        if (best<0 || l->rw*l->rh<u2_lists[best].rw*u2_lists[best].rh) best=i;
    }
    if (best>=0) *out=u2_lists[best];
    pthread_mutex_unlock(&u3_lock);
    return best>=0;
}
static void u2_list_update(uint64_t self, uint64_t image) {
    uint64_t ds=0, vt=0, f0=0, f8=0, f30=0, f38=0;
    if (self<0x10000 || !safe_read(&ds,self+0x18,8) || ds<0x10000 || !safe_read(&vt,ds,8) || vt<0x10000) return;
    if (!safe_read(&f0,vt,8) || !safe_read(&f8,vt+8,8) || !safe_read(&f30,vt+0x30,8) || !safe_read(&f38,vt+0x38,8)) return;
    const uint64_t lo_code=image, hi_code=image+0x2000000;
    if (f0<lo_code || f0>=hi_code || f8<lo_code || f8>=hi_code || f30<lo_code || f30>=hi_code || f38<lo_code || f38>=hi_code) return;
    int cur=(int)(int32_t)((guest_vcall_fn)(uintptr_t)f0)(ds);
    const int lo=(int)(int32_t)((guest_vcall_fn)(uintptr_t)f30)(ds), hi=(int)(int32_t)((guest_vcall_fn)(uintptr_t)f38)(ds);
    int32_t top=0, vis=0;
    safe_read(&top,self+0x248,4); safe_read(&vis,self+0x244,4);
    if (u2_set_pending && u2_set_ds==ds) {
        int v=u2_set_val; if (v<lo) v=lo; if (v>hi) v=hi;
        /* (antik) a value at the first or last row seen makes the list scroll (the widget keeps the selection a row from its edges, when
         * the value differs from the one it followed last): for a hover the followed value is set too, the list stays put */
        const int edge=u2_set_keep && vis>=4 && vis<=16 && ((v==top && top>lo) || (v==top+vis-1 && v<hi));
        ((guest_set_fn)(uintptr_t)f8)(ds,v);
        if (edge) { const int32_t w=v; safe_write(self+0x22c,&w,4); safe_write(self+0x228,&w,4); }
        u2_set_pending=0;
        if (mouse_debug()) printf("Menu mouse: U2 list set(%d) (was %d, range %d..%d, rows %d..%d seen)\n",v,cur,lo,hi,top,top+vis-1);
        cur=v;
    }
    if (u2_scroll_pending && u2_scroll_w==self) {
        int t=u2_scroll_top;
        if (vis>=1 && vis<=32) { if (t>hi+1-vis) t=hi+1-vis; if (t<lo) t=lo; const int32_t w=t; safe_write(self+0x248,&w,4); top=t; }
        u2_scroll_pending=0;
        if (mouse_debug()) printf("Menu mouse: U2 list view scrolled to row %d\n",t);
    }
    const uint64_t now=now_us();
    float rc[4]={0,0,0,0};
    safe_read(rc,self+0x90,16);
    pthread_mutex_lock(&u3_lock);
    int slot=-1, oldest=0;
    for (int i=0;i<16;++i) { if (u2_lists[i].w==self) { slot=i; break; } if (u2_lists[i].us<u2_lists[oldest].us) oldest=i; }
    if (slot<0) { slot=oldest; if (mouse_debug()) printf("Menu mouse: U2 list control %#llx: values %d..%d (now %d), rows %d..%d seen\n",(unsigned long long)self,lo,hi,cur,top,top+vis-1); }
    uint8_t fl=0;
    safe_read(&fl,self+8,1);
    if (mouse_debug() && u2_lists[slot].w==self && (u2_lists[slot].focused!=0)!=(fl!=0))
        printf("Menu mouse: U2 list control %#llx (values %d..%d, top %d, %d shown, rect %.0f %.0f %.0fx%.0f): focus flag %d -> %d\n",(unsigned long long)self,lo,hi,top,vis,rc[0],rc[1],rc[2],rc[3],u2_lists[slot].focused,fl);
    /* a drop-down (U2's difficulty choice): a list that gains the focus with no rectangle yet (it animates open from 0x0) */
    if (fl && !(u2_lists[slot].w==self && u2_lists[slot].focused) && rc[2]<1.0f && rc[3]<1.0f) u2_dropdown_w=self;
    else if (!fl && u2_dropdown_w==self) u2_dropdown_w=0;
    u2_lists[slot]=(U2List){self,ds,cur,lo,hi,top,vis,now,rc[0],rc[1],rc[2],rc[3],fl};
    pthread_mutex_unlock(&u3_lock);
}
/* U3's list (antikmewzik's u3_list_hook, 1.02 0x5e3e60; Bonus > Cutscenes, Treasures, Statistics): one control draws all the rows and keeps the
 * selected row in its data source (+0x18; vtable +0 get, +8 set, +0x38 minimum, +0x40 maximum); +0x184 rows seen, +0x188 first row seen,
 * +0x16c the selection the view last followed (told the new one for a hover: the view stays). Noted in u2_lists (top = first row seen).
 * which: 0 (the 1.02 site; EU 1.00's list function is not located yet: 0x1a3050 / 0x1b03f0 were not it). */
/* Treasures, the 3D model (antikmewzik's u2_model_hook / u3_model_hook): the scene (menutreasurescene) keeps two angular speeds at
 * +0x70 (around the vertical) and +0x74 that the right stick adds to and the game clamps to +-2 rad/s and lets die down; each frame
 * the model's matrix turns by speed x dt. A hook after the second clamp's store, before the turn, sets the speeds from the mouse while
 * the left button is held on the model (pressed right of the list), then lets them coast down after the release. U3's list hover
 * also tells the scene which model to show (u3_note_*: the call the list makes itself when it follows a selection). */
static volatile uint64_t model_us;            /* the scene's update ran lately: the model is on screen */
static volatile int model_drag;               /* the left button went down on the model and is still held */
static volatile float model_ax, model_ay;     /* the cursor's motion since the scene's last frame, radians */
static volatile uint64_t u3_scene;            /* U3: the scene (rdi at its update's start; r15 at the hook, a register the thunk does not keep) */
static volatile int u3_note_pending; static volatile int32_t u3_note_value; static volatile uint64_t u3_note_ds, u3_note_fn, u3_note_queue;
/* U3 Chapter select's carousel (antikmewzik's u3_carousel_hook; 1.02 0x5eccb0, 1.00 0x1a0490): a control with a data source (+0x18:
 * get +0, set +8, minimum +0x38, maximum +0x40, as the sliders); opened it is 289 tall with the pictures. The wheel over it and a click
 * on its arrows (drawn outside its rectangle) set the chapter by value (u2_set_*, applied in its draw on the game's thread). */
static struct { uint64_t w, ds, us; int cur, lo, hi; float rc[4]; } u3_car;
/* U3's Yes/No boxes (antikmewzik's u3_dialog_hook; 1.02 0x5e0de0, 1.00 0x197fc0): the choices are drawn by a control with a data
 * source (as the sliders); the choice under the cursor is set by value, a click sets it, then presses the cross once it is there.
 * BB_U3_DIALOG_JUMP=0: the d-pad steps to the choice instead (as before). */
static struct { uint64_t w, ds, us; int cur, lo, hi; float rc[4]; } u3_dlg;
static int u3_dialog_jump_on(void) { static int on=-1; if (on<0) { const char *e=getenv("BB_U3_DIALOG_JUMP"); on=!(e && e[0]=='0'); } return on; }
/* U2's controller picture (Options > Controls > Controller, antikmewzik): it has no menu rows, so the menu mode would go off there and
 * the right / side button would not go back. It draws its labels through the game's text lookup (1.02 0xcddc00, 1.00 0x633460)
 * every frame: the last time one of its own texts was asked for says it is up. */
static volatile uint64_t u2_layout_us;
static void u3_list_update(uint64_t self, uint64_t image, int which) {
    uint64_t ds=0, vt=0, f0=0, f8=0, fmin=0, fmax=0; float rc[4]={0,0,0,0};
    if (self<0x10000 || !safe_read(rc,self+0x90,16) || rc[3]<100.0f) return;
    if (!safe_read(&ds,self+0x18,8) || ds<0x10000 || !safe_read(&vt,ds,8) || vt<0x10000) return;
    if (!safe_read(&f0,vt,8) || !safe_read(&f8,vt+8,8) || !safe_read(&fmin,vt+0x38,8) || !safe_read(&fmax,vt+0x40,8)) return;
    const uint64_t lo_code=image, hi_code=image+0x2000000;
    if (f0<lo_code || f0>=hi_code || f8<lo_code || f8>=hi_code || fmin<lo_code || fmin>=hi_code || fmax<lo_code || fmax>=hi_code) return;
    int cur=(int)(int32_t)((guest_vcall_fn)(uintptr_t)f0)(ds);
    const int lo=(int)(int32_t)((guest_vcall_fn)(uintptr_t)fmin)(ds), hi=(int)(int32_t)((guest_vcall_fn)(uintptr_t)fmax)(ds);
    int32_t vis=0, first=-1;
    safe_read(&vis,self+0x184,4); safe_read(&first,self+0x188,4);
    if (which) {   /* (EU 1.00 candidates: logged only) */
        if (mouse_debug()) { static uint64_t seen[2][16]; int known=0; for (int i=0;i<16;++i) if (seen[which-1][i]==self) known=1;
            static uint64_t last_us[2]; static int last_cur[2]={-99,-99};
            if (!known || cur!=last_cur[which-1]) { for (int i=0;i<16 && !known;++i) if (!seen[which-1][i]) { seen[which-1][i]=self; break; }
                if (now_us()-last_us[which-1]>100000) { last_us[which-1]=now_us(); last_cur[which-1]=cur;
                    printf("Menu mouse: U3 1.00 list candidate %d: control %#llx values %d..%d now %d, +0x184 %d +0x188 %d, rect %.0f %.0f %.0fx%.0f\n",which,(unsigned long long)self,lo,hi,cur,vis,first,rc[0],rc[1],rc[2],rc[3]); } } }
        return;
    }
    if (hi-lo<1) return;
    if (u2_set_pending && u2_set_ds==ds) {
        int v=u2_set_val; if (v<lo) v=lo; if (v>hi) v=hi;
        const int32_t first_pre=first;
        ((guest_set_fn)(uintptr_t)f8)(ds,v);
        if (u2_set_keep) { const int32_t w=v; safe_write(self+0x16c,&w,4); safe_write(self+0x188,&first_pre,4); }   /* the view stays */
        if (u2_set_keep && model_us && now_us()-model_us<200000) {   /* Treasures: the model follows only when the list tells the scene (it does so when it follows a selection itself): the same call, from the scene's update */
            uint64_t vt2=0, fn48=0, q=0; uint8_t on=0, flags=0;
            if (safe_read(&on,self+8,1) && on && safe_read(&flags,self+0xa8,1) && (flags & 0x40) && safe_read(&vt2,ds,8) && safe_read(&fn48,vt2+0x48,8) &&
                safe_read(&q,self+0x178,8) && q>0x10000 && fn48>=lo_code && fn48<hi_code) { u3_note_ds=ds; u3_note_fn=fn48; u3_note_queue=q+0x10; u3_note_value=v; u3_note_pending=1; }
        }
        u2_set_pending=0;
        if (mouse_debug()) printf("Menu mouse: U3 list set(%d) (was %d, range %d..%d, rows %d..%d seen)\n",v,cur,lo,hi,first,first+vis-1);
        cur=v;
    }
    if (u2_scroll_pending && u2_scroll_w==self && vis>=3 && vis<=32) {
        int t=u2_scroll_top; if (t>hi+1-vis) t=hi+1-vis; if (t<lo) t=lo;
        const int32_t w=t; safe_write(self+0x188,&w,4); first=t;
        u2_scroll_pending=0;
        if (mouse_debug()) printf("Menu mouse: U3 list view scrolled to row %d\n",t);
    }
    uint8_t fl=0;
    safe_read(&fl,self+8,1);
    const uint64_t now=now_us();
    pthread_mutex_lock(&u3_lock);
    int slot=-1, oldest=0;
    for (int i=0;i<16;++i) { if (u2_lists[i].w==self) { slot=i; break; } if (u2_lists[i].us<u2_lists[oldest].us) oldest=i; }
    if (slot<0) { slot=oldest; if (mouse_debug()) printf("Menu mouse: U3 list control %#llx: values %d..%d (now %d), rows %d..%d seen, rect %.0f %.0f %.0fx%.0f\n",(unsigned long long)self,lo,hi,cur,first,first+vis-1,rc[0],rc[1],rc[2],rc[3]); }
    u2_lists[slot]=(U2List){self,ds,cur,lo,hi,first>=lo ? first : lo,vis,now,rc[0],rc[1],rc[2],rc[3],fl};
    pthread_mutex_unlock(&u3_lock);
}

static void u3_text(const uint64_t *regs) {
    float r[4], st[9];
    if (!safe_read(r,regs[4],16)) return;
    const uint64_t now=now_us();
    if (fabsf(r[0]-358.0f)<2.0f && fabsf(r[2]-564.0f)<3.0f && r[3]>=150.0f) u3_info_us=now;   /* (his: the box of a locked reward) */
    if (u2m && fabsf(r[0]-298.1f)<1.5f && fabsf(r[1]-156.2f)<1.5f && fabsf(r[2]-683.8f)<2.0f && r[3]>=280.0f && r[3]<=330.0f) u2_popup_us=now;   /* (318.8 tall, 293.6 on some chapters) */
    if ((u2m || u3m) && r[2]>=400.0f && r[2]<=800.0f && r[3]>=150.0f && r[3]<=400.0f) {
        pthread_mutex_lock(&u3_lock);
        int slot=0;
        for (int i=0;i<8;++i) {
            if (fabsf(u2_panels[i].x-r[0])<0.5f && fabsf(u2_panels[i].y-r[1])<0.5f && fabsf(u2_panels[i].w-r[2])<0.5f) { slot=i; break; }
            if (u2_panels[i].us<u2_panels[slot].us) slot=i;
        }
        u2_panels[slot]=(U3Row){r[0],r[1],r[2],r[3],0.0f,now,0};
        pthread_mutex_unlock(&u3_lock);
    }
    if (mouse_debug() && r[3]>=150.0f && r[2]>=300.0f) {   /* BB_MOUSE_DEBUG=1: each big panel once (to the unit; pictures, information boxes) */
        static float seen[512][4]; static int nseen;
        int known=0;
        for (int i=0;i<nseen && !known;++i) known=fabsf(seen[i][0]-r[0])<1.0f && fabsf(seen[i][1]-r[1])<1.0f && fabsf(seen[i][2]-r[2])<1.0f && fabsf(seen[i][3]-r[3])<1.0f;
        if (!known && nseen<512) { memcpy(seen[nseen++],r,sizeof(r)); printf("Menu mouse: big panel %.1f %.1f %.1f x %.1f\n",r[0],r[1],r[2],r[3]); }
    }
    if (mouse_debug() && r[3]>=24.0f && r[3]<=42.0f) {   /* BB_MOUSE_DEBUG=1: each row-sized text box and each change of its style (to tell the highlight) */
        static struct { float r[4]; int q[9]; } seen[3000];
        static int nseen;
        float s9[9]={0};
        const int ok=safe_read(s9,regs[5],sizeof(s9));
        int q[9], known=0;
        for (int k=0;k<9;++k) q[k]=(int)lroundf(s9[k]*10.0f);
        for (int i=0;i<nseen && !known;++i)
            known=!memcmp(seen[i].q,q,sizeof(q)) && fabsf(seen[i].r[0]-r[0])<0.5f && fabsf(seen[i].r[1]-r[1])<0.5f && fabsf(seen[i].r[2]-r[2])<0.5f && fabsf(seen[i].r[3]-r[3])<0.5f;
        if (!known && nseen<3000) {
            memcpy(seen[nseen].r,r,sizeof(r)); memcpy(seen[nseen].q,q,sizeof(q)); ++nseen;
            printf("Menu mouse: text box %.1f %.1f %.1f x %.1f style%s %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f\n",r[0],r[1],r[2],r[3],ok ? "" : " (unreadable)",
                   s9[0],s9[1],s9[2],s9[3],s9[4],s9[5],s9[6],s9[7],s9[8]);
        }
    }
    if (r[3]>=700.0f && r[3]<=800.0f && r[2]>=40.0f) menu_credits_us=now;
    if (u3m && fabsf(r[0]-100.0f)<2.0f && fabsf(r[1]-600.0f)<3.0f && fabsf(r[2]-1080.0f)<3.0f && fabsf(r[3]-60.0f)<3.0f &&
        !runtime_mouselook_camera_recent()) u3_gallery_us=now;   /* (not a box of that size in play: subtitles, hints) */
    {   /* a slider's rail (the size of its picture: the text object, rcx, +0x3c) */
        float sz[2];
        if (u3m && safe_read(sz,regs[6]+0x3c,8) && sz[0]<0.5f && fabsf(r[0]-96.0f)<3.0f && r[3]>=24.0f && r[3]<=42.0f) {
            pthread_mutex_lock(&u3_lock);
            int slot=-1, oldest=0;
            for (int i=0;i<8;++i) { if (fabsf(u3_ghost_y[i]-r[1])<0.5f) { slot=i; break; } if (u3_ghost_us[i]<u3_ghost_us[oldest]) oldest=i; }
            if (slot<0) slot=oldest;
            u3_ghost_y[slot]=r[1]; u3_ghost_us[slot]=now;
            pthread_mutex_unlock(&u3_lock);
        }
        if (safe_read(sz,regs[6]+0x3c,8) && sz[1]>0.0f && sz[1]<=12.0f &&
            ((u2m && sz[0]>=100.0f && r[3]>=24.0f && r[3]<=42.0f) || (u3m && sz[0]>=60.0f && r[3]>=20.0f && r[3]<=44.0f))) {
            pthread_mutex_lock(&u3_lock);
            int slot=0;
            for (int i=0;i<8;++i) {
                if (fabsf(u3_sliders[i].x-r[0])<0.5f && fabsf(u3_sliders[i].y-r[1])<0.5f) { slot=i; break; }
                if (u3_sliders[i].us<u3_sliders[slot].us) slot=i;
            }
            u3_sliders[slot]=(U3Row){r[0],r[1],sz[0],r[3],0.0f,now,0};
            pthread_mutex_unlock(&u3_lock);
        }
    }
    if (u3m && r[2]>=10.0f && r[2]<=20.0f && r[3]>=33.0f && r[3]<=34.2f) {
        pthread_mutex_lock(&u3_lock);
        int slot=0;
        for (int i=0;i<16;++i) {
            if (fabsf(u3_arrows[i].x-r[0])<0.5f && fabsf(u3_arrows[i].y-r[1])<0.5f) { slot=i; break; }
            if (u3_arrows[i].us<u3_arrows[slot].us) slot=i;
        }
        u3_arrows[slot]=(U3Row){r[0],r[1],r[2],r[3],0.0f,now,0};
        pthread_mutex_unlock(&u3_lock);
        return;
    }
    if (!(r[3]>=24.0f && r[3]<=42.0f && r[2]>=20.0f) || !safe_read(st,regs[5],sizeof(st))) return;
    /* only a colour (0..2) followed by the flags is a style (other calls pass something else in rdx) */
    for (int k=0;k<4;++k) if (!(st[k]>=0.0f && st[k]<=2.0f)) return;
    /* the selection mark, kept as 0..1: U3 style[6] (0..1), U2 style[5] (0..3) times style[7] (0..1): on U2's rows style[7] is 1 and
     * style[5] the mark; on its Yes/No (the pause menu's Quit, return to checkpoint) style[5] is 3 on both choices and style[7] the mark */
    const float mark=u2m ? st[5]/3.0f*st[7] : st[6];
    if (!(mark>=0.0f && mark<=1.01f)) return;
    pthread_mutex_lock(&u3_lock);
    int slot=-1, oldest=0;
    for (int i=0;i<U3_ROWS;++i) {
        const U3Row *o=&u3_rows[i];
        if (o->us && fabsf(o->x-r[0])<0.5f && fabsf(o->y-r[1])<0.5f && fabsf(o->w-r[2])<0.5f && fabsf(o->h-r[3])<0.5f) { slot=i; break; }
        if (u3_rows[i].us<u3_rows[oldest].us) oldest=i;
    }
    if (slot<0) slot=oldest;
    u3_rows[slot]=(U3Row){r[0],r[1],r[2],r[3],mark,now,u2m && st[5]>=2.9f};
    pthread_mutex_unlock(&u3_lock);
}
/* A focused page (rows or not: the controller picture has none) keeps the menu mode on; the page holding the row under the cursor moves
 * its focus there when asked (our port of 2026-10-28, play-tested on US 1.02). */
static void u3_page(uint64_t self, uint64_t image) {
    uint8_t focused=0;
    if (self>0x10000 && safe_read(&focused,self+8,1) && focused) u3_page_us=now_us();
    if (!u3_jump_req) return;
    /* BB_MOUSE_DEBUG=1: why a page did not take the jump (each page and reason once a second) */
#define U3_PAGE_WHY(why) do { if (mouse_debug()) { static uint64_t shown_self[16], shown_us[16]; static int k; int f=-1; const uint64_t t=now_us();         for (int z=0;z<16;++z) if (shown_self[z]==self) f=z; if (f<0) { f=k; k=(k+1)%16; shown_self[f]=self; shown_us[f]=0; }         if (t-shown_us[f]>1000000) { shown_us[f]=t; printf("Menu mouse: U3 page %#llx: no jump to y %.0f: %s\n",(unsigned long long)self,u3_jump_cy,why); } } } while (0)
    int32_t cnt=0; uint64_t arr=0, ch[64], vt[64];
    if (self<0x10000 || !safe_read(&cnt,self+0xac,4) || cnt<2 || cnt>64 || !safe_read(&arr,self+0xb0,8) || arr<0x10000 ||
        !safe_read(ch,arr,(size_t)cnt*8)) { U3_PAGE_WHY("no children"); return; }
    int focus=-1, nfocus=0;
    for (int i=0;i<cnt;++i) {
        uint8_t f=0;
        if (ch[i]<0x10000 || !safe_read(&vt[i],ch[i],8) || vt[i]<image || vt[i]>=image+0x2000000 || !safe_read(&f,ch[i]+8,1)) return;
        if (f) { if (focus<0) focus=i; ++nfocus; }
    }
    if (nfocus!=1) { U3_PAGE_WHY(nfocus ? "several focused" : "none focused"); return; }
    const float cy=u3_jump_cy, cx=u3_jump_cx;
    int target=-1; float best=1e9f;
    for (int i=0;i<cnt;++i) {
        float rc[4];
        /* a row (24-42 tall), or a big control holding the cursor (Chapter select: the chapter's name and picture are one 290 tall
         * control); a row on that line wins over a big control */
        if (!safe_read(rc,ch[i]+0x90,16)) continue;
        const int box=rc[3]>42.0f && rc[3]<450.0f;
        if (!box && (rc[3]<24.0f || rc[3]>42.0f)) continue;
        const float d=box ? (cy>=rc[1] && cy<=rc[1]+rc[3] && cx>=rc[0]-8.0f && cx<=rc[0]+rc[2]+8.0f ? 9.0f : 1e3f) : fabsf(rc[1]+rc[3]*0.5f-cy);
        const float score=d+0.02f*fabsf(rc[0]+rc[2]*0.5f-cx);
        if (d<10.0f && score<best) { best=score; target=i; }
    }
    if (target<0) {   /* not this page (a parent page, hidden, is updated too) */
        if (mouse_debug()) {
            char buf[512]; int o=snprintf(buf,sizeof(buf),"no child at that height (%d children:",cnt);
            for (int i=0;i<cnt && o<(int)sizeof(buf)-24;++i) { float rc[4]={0}; safe_read(rc,ch[i]+0x90,16); o+=snprintf(buf+o,sizeof(buf)-o," %.0f/%.0f",rc[1]+rc[3]*0.5f,rc[3]); }
            snprintf(buf+o,sizeof(buf)-o,")");
            U3_PAGE_WHY(buf);
        }
        return;
    }
    int selectable=0;
    {   /* vtable +0x40: can the row take the focus? (a section title cannot) */
        uint64_t fs=0;
        if (!safe_read(&fs,vt[target]+0x40,8) || fs<image || fs>=image+0x2000000) return;
        selectable=(uint8_t)((guest_vcall_fn)(uintptr_t)fs)(ch[target])!=0;
    }
    {   /* the page on screen: its focused row is the row drawn highlighted now, the target a row drawn now */
        float fr[4], tr[4];
        if (!safe_read(fr,ch[focus]+0x90,16) || !safe_read(tr,ch[target]+0x90,16)) return;
        const uint64_t now=now_us();
        int lit=0, drawn=0;
        pthread_mutex_lock(&u3_lock);
        for (int i=0;i<U3_ROWS;++i) {
            const U3Row *r=&u3_rows[i];
            if (!r->us || now-r->us>=100000) continue;
            const float rcy=r->y+r->h*0.5f;   /* by the middle: a text is centred on its row */
            /* (a big control: a text inside it counts, its name is not drawn at its middle) */
            const int in_f=fr[3]>42.0f ? r->y>=fr[1]-1.0f && r->y+r->h<=fr[1]+fr[3]+1.0f && r->x>=fr[0]-1.0f && r->x<=fr[0]+fr[2]
                                       : fabsf(r->x-fr[0])<1.5f && fabsf(rcy-(fr[1]+fr[3]*0.5f))<1.5f;
            const int in_t=tr[3]>42.0f ? r->y>=tr[1]-1.0f && r->y+r->h<=tr[1]+tr[3]+1.0f && r->x>=tr[0]-1.0f && r->x<=tr[0]+tr[2]
                                       : fabsf(r->x-tr[0])<1.5f && fabsf(rcy-(tr[1]+tr[3]*0.5f))<1.5f;
            if (r->sel>=0.83f && in_f) lit=1;
            if (in_t) drawn=1;
        }
        pthread_mutex_unlock(&u3_lock);
        if (self==u3_jump_page && focus==u3_jump_row && now-u3_jump_us<1000000) lit=1;   /* the page of the last jump, before its row is drawn lit */
        if (!lit || !drawn) { U3_PAGE_WHY(!lit ? "its focused row is not drawn lit" : "the row is not drawn"); return; }
    }
    u3_jump_req=0;
    if (!selectable) { u3_jump_done=-2; return; }
    if (target!=focus) {
        uint64_t f0=0, f1=0;
        if (!safe_read(&f0,vt[focus]+0x30,8) || !safe_read(&f1,vt[target]+0x30,8) || f0<image || f0>=image+0x2000000 || f1<image || f1>=image+0x2000000) {
            u3_jump_done=-1;
            return;
        }
        ((guest_set_fn)(uintptr_t)f0)(ch[focus],0);
        ((guest_set_fn)(uintptr_t)f1)(ch[target],1);
        if (mouse_debug()) printf("Menu mouse: U3 row %d -> %d on page %#llx (y %.0f)\n",focus,target,(unsigned long long)self,cy);
    }
    u3_jump_done=1;
    u3_jump_page=self; u3_jump_row=target; u3_jump_us=now_us();
}
/* U2's direct jump (after antikmewzik's u2_menu_jump_service, US 1.02; here on both codes): the menu builder's host (from the
 * Quit hook in it, runtime_exit_menu.c) points to its pages; a page is {.., u32 children at +0xb4, children at +0xb8}, a child
 * {vtable, u8 focused at +8, u32 id at +0x10, rect x y w h at +0x90}, vtable +0x30 sets its focus. Of the pages with exactly one
 * focused child, the child drawn under the cursor (its middle within 10 of the cursor's row, the cursor's x inside it) takes the
 * focus. Hidden pages are updated too: requiring the cursor inside the child keeps a page behind a dialog from taking it. */
static volatile uint64_t u2_host;
static volatile uint64_t u2_nopage_us;   /* the last jump found no focused page (a Yes/No over the menu): the d-pad steps for a second */
void runtime_menumouse_u2_host(uint64_t host) {
    if (host!=u2_host) printf("Menu mouse: Uncharted 2 menu host %#llx (direct jump to the row under the cursor)\n",(unsigned long long)host);
    u2_host=host;
}
static void u2_jump(uint64_t image) {
    if (!u3_jump_req) return;
    const float cy=u3_jump_cy, cx=u3_jump_cx;
    const uint64_t host=u2_host;
    uint64_t q[0x220];
    u3_jump_req=0;
    if (host<0x10000 || !safe_read(q,host,sizeof(q))) { if (mouse_debug()) printf("Menu mouse: U2 jump: no menu host (%#llx)\n",(unsigned long long)host); u3_jump_done=-1; return; }
    int pages=0;
    uint64_t best_ch[2]={0}, best_vt[2]={0};
    float best=1e9f;
    int found=0;
    for (int i=0;i<0x220;++i) {
        uint64_t arr=0, ch[64], vt[64];
        int32_t n=0;
        if (q[i]<0x10000 || !safe_read(&n,q[i]+0xb4,4) || n<2 || n>64 || !safe_read(&arr,q[i]+0xb8,8) || arr<0x10000 || !safe_read(ch,arr,(size_t)n*8)) continue;
        int focus=-1, nfocus=0, valid=1;
        for (int k=0;k<n && valid;++k) {
            uint8_t f=0;
            if (ch[k]<0x10000 || !safe_read(&vt[k],ch[k],8) || vt[k]<image || vt[k]>=image+0x2000000 || !safe_read(&f,ch[k]+8,1)) valid=0;
            else if (f) { if (focus<0) focus=k; ++nfocus; }
        }
        if (!valid || nfocus!=1) continue;
        ++pages;
        if (mouse_debug()) {
            static uint64_t shown[32]; int known=0, slot=-1;
            for (int z=0;z<32;++z) { if (shown[z]==q[i]) known=1; if (!shown[z] && slot<0) slot=z; }
            if (!known && slot>=0) {
                shown[slot]=q[i];
                printf("Menu mouse: U2 page host+%#x %#llx, %d children (focus %d):",i*8,(unsigned long long)q[i],n,focus);
                for (int k=0;k<n;++k) { float rc[4]={0}; safe_read(rc,ch[k]+0x90,16); printf(" [%.0f %.0f %.0f %.0f]",rc[0],rc[1],rc[2],rc[3]); }
                printf("\n");
            }
        }
        for (int k=0;k<n;++k) {
            float rc[4];
            uint32_t id=0;
            if (!safe_read(rc,ch[k]+0x90,16)) continue;
            const int row=rc[3]>=28.0f && rc[3]<=46.0f, box=rc[3]>200.0f && rc[3]<400.0f;   /* rows 28-36, buttons ~42, the chapter picker 238 */
            if (!row && !box) continue;
            safe_read(&id,ch[k]+0x10,4);
            if (id==0x905ec3adu || id==0xbea3cef1u) continue;   /* (antik) the stereo page's two hidden rows */
            /* by the row's line; of two rows on one line (the main menu's columns) the nearer in x. A value drawn right of its row (Off / On)
             * is outside the child's rectangle: it still selects its row */
            const float d=box ? (cy>=rc[1] && cy<=rc[1]+rc[3] ? 0.0f : 1e3f) : fabsf(rc[1]+rc[3]*0.5f-cy);
            const float dx=cx<rc[0] ? rc[0]-cx : cx>rc[0]+rc[2] ? cx-(rc[0]+rc[2]) : 0.0f;
            const float score=d+0.02f*dx;
            if (d<10.0f && score<best) { best=score; best_ch[0]=ch[focus]; best_vt[0]=vt[focus]; best_ch[1]=ch[k]; best_vt[1]=vt[k]; found=1; }
        }
    }
    if (!found) { if (!pages) u2_nopage_us=now_us(); if (mouse_debug()) printf("Menu mouse: U2 jump: no row at %.0f %.0f (%d pages)\n",cx,cy,pages); u3_jump_done=-2; return; }
    if (best_ch[0]!=best_ch[1]) {
        uint64_t f0=0, f1=0;
        if (!safe_read(&f0,best_vt[0]+0x30,8) || !safe_read(&f1,best_vt[1]+0x30,8) || f0<image || f0>=image+0x2000000 || f1<image || f1>=image+0x2000000) {
            u3_jump_done=-1;
            return;
        }
        ((guest_set_fn)(uintptr_t)f0)(best_ch[0],0);
        ((guest_set_fn)(uintptr_t)f1)(best_ch[1],1);
        if (mouse_debug()) printf("Menu mouse: U2 focus moved to the row at y %.0f\n",cy);
    }
    u3_jump_done=1;
}
static int u3_menu_open(uint64_t now) {
    static uint64_t seen;   /* (the highlight fades from row to row: the menu stays on for a moment without a lit row) */
    int lit=u3_page_us && now-u3_page_us<150000 && u3_page_us<=now+1000000;
    if (u2_popup_us && now-u2_popup_us<300000 && u2_popup_us<=now+1000000) lit=1;   /* (U2's chapter carousel has no lit row) */
    if (u2_layout_us && now-u2_layout_us<300000 && u2_layout_us<=now+1000000) lit=1;   /* (U2's controller picture: no rows) */
    if (u3_gallery_us && now-u3_gallery_us<300000 && u3_gallery_us<=now+1000000) lit=1;   /* (U3's art gallery viewer: no lit row) */
    if (menu_credits_us && now-menu_credits_us<300000 && menu_credits_us<=now+1000000) lit=1;   /* (the credits: no lit row either) */
    /* (a list on screen is a menu: dragging its scroll bar can take every lit row out of view, US 1.02's Skins) */
    for (int i=0;i<16 && !lit && (u2m || u3m);++i) { const U2List *l=&u2_lists[i]; if (l->us && now-l->us<300000 && l->vis>=3 && l->rh>=70.0f && l->focused) lit=1; }
    pthread_mutex_lock(&u3_lock);
    for (int i=0;i<U3_ROWS && !lit;++i) if (u3_rows[i].us && now-u3_rows[i].us<300000 && u3_rows[i].sel>=0.5f) lit=1;
    pthread_mutex_unlock(&u3_lock);
    if (lit) seen=now;
    return seen && now-seen<800000;
}
static uint32_t u3_menu_buttons(uint32_t mouse, uint64_t now) {
    static struct { float last_x, last_y, req_cy, step_cy, step_cx; int last_left, active, click, steps, step_dir; uint64_t click_until, click_deadline, req_us, step_until, next_step; float nojump_cy, nojump_cx; int nojump; float step_sel_cy, step_sel_cx; } h={-1e9f,-1e9f,-1.0f,-1.0f,-1.0f,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    uint32_t out=0;
    float ux=0, uy=0;
    if (scripted_cursor) { ux=scripted_ux*UI_WIDTH; uy=scripted_uy*UI_HEIGHT; }
    else {
        SDL_Window *w=SDL_GetMouseFocus();
        float mx=0, my=0; int ww=0, wh=0;
        if (!w) return 0;
        SDL_GetMouseState(&mx,&my);
        SDL_GetWindowSize(w,&ww,&wh);
        if (ww<=0 || wh<=0) return 0;
        if (bbgpu_frame_fills_window()) {   /* a play frame over the whole window */
            ux=mx/(float)ww*UI_WIDTH; uy=my/(float)wh*UI_HEIGHT;
            /* (16:10, Vert+: its UI is squeezed back to 16:9 vertically, runtime_ultrawide_scale < 0: the UI's height is that share of the window's) */
            if (runtime_ultrawide_scale<0.0f) { const float sv=-runtime_ultrawide_scale; uy=(my-(float)wh*(1.0f-sv)*0.5f)/((float)wh*sv)*UI_HEIGHT; }
            /* (ultrawide, Hor+: the UI is squeezed back to 16:9 sideways, 0 < scale < 1: the UI's width is that share of the window's; the pause menus over
             * gameplay were read as stretched over the whole width: off by 60 at the Controls page's values on 21:9) */
            else if (runtime_ultrawide_scale>0.0f && runtime_ultrawide_scale<0.999f) { const float sh=runtime_ultrawide_scale; ux=(mx-(float)ww*(1.0f-sh)*0.5f)/((float)ww*sh)*UI_WIDTH; }
        }
        else {
            const float scale=fminf((float)ww/UI_WIDTH,(float)wh/UI_HEIGHT);
            ux=(mx-((float)ww-UI_WIDTH*scale)*0.5f)/scale; uy=(my-((float)wh-UI_HEIGHT*scale)*0.5f)/scale;
        }
    }
    const int moved=fabsf(ux-h.last_x)>=1.0f || fabsf(uy-h.last_y)>=1.0f;
    h.last_x=ux; h.last_y=uy;
    const int left=(mouse & 1u)!=0, clicked=left && !h.last_left;
    h.last_left=left;
    const int info=u3_info_us && now-u3_info_us<300000;
    /* the rows drawn now: the one under the cursor (the smallest that holds it: a row's label and value are texts of their own) and the lit one */
    U3Row hov={0}, sel={0};
    int have_hov=0, have_sel=0, sel_bar=0, hov_pri=0;
    pthread_mutex_lock(&u3_lock);
    /* an open list in front (U2's difficulty choice: a focused list of 5 over the Game page): its rows win over the page's texts
     * under it (the hidden "On" of Tutorial Hints, smaller than "Hard", took the click: the list chose the current value) */
    float fl[4]={0}; int have_fl=0;
    for (int k=0;k<16 && (u2m || u3m);++k) {
        const U2List *l=&u2_lists[k];
        if (!l->us || now-l->us>=150000 || !l->focused || l->vis<2 || l->rh<40.0f || l->rw<40.0f) continue;
        if (ux<l->rx || ux>l->rx+l->rw || uy<l->ry || uy>l->ry+l->rh) continue;
        if (!have_fl || l->rw*l->rh<fl[2]*fl[3]) { fl[0]=l->rx; fl[1]=l->ry; fl[2]=l->rw; fl[3]=l->rh; have_fl=1; }
    }
    for (int i=0;i<U3_ROWS;++i) {
        const U3Row *r=&u3_rows[i];
        if (!r->us || now-r->us>=100000) continue;
        /* the most lit (a fading highlight is two rows); U3's Yes/No: the lit one inside the highlight bar. U2's section titles
         * (Aiming, Camera: 34.2 tall, the rows 33.6) are drawn lit all the time: never the selection */
        if (r->sel>=0.5f && !(u2m && fabsf(r->h-34.2f)<0.15f)) {
            const int bar=!u2m && u3_in_bar(r,now);
            if (!have_sel || bar>sel_bar || (bar==sel_bar && (r->sel>sel.sel+0.01f || (fabsf(r->sel-sel.sel)<=0.01f && r->w>sel.w)))) { sel=*r; have_sel=1; sel_bar=bar; }
        }
        if (u3m && r->w>750.0f && !(fabsf(r->x-380.0f)<1.0f && fabsf(r->w-520.0f)<1.0f)) {   /* (antik) a wide banner above the menu is not a row */
            int in_list=0;   /* (but a row of a list on screen is: Treasures' rows are 796 wide) */
            for (int k=0;k<16 && !in_list;++k) {
                const U2List *l=&u2_lists[k];
                in_list=l->us && now-l->us<300000 && l->vis>=3 && l->rh>=70.0f && r->y>=l->ry-1.0f && r->y+r->h<=l->ry+l->rh+1.0f && r->x>=l->rx-4.0f && r->x+r->w<=l->rx+l->rw+4.0f;
            }
            if (!in_list) continue;
        }
        const int pri=have_fl && r->x>=fl[0]-2.0f && r->x+r->w<=fl[0]+fl[2]+2.0f && r->y>=fl[1]-2.0f && r->y+r->h<=fl[1]+fl[3]+2.0f;
        if (ux>=r->x && ux<=r->x+r->w && uy>=r->y && uy<=r->y+r->h && 
            (!have_hov || pri>hov_pri || (pri==hov_pri && (r->front>hov.front || (r->front==hov.front && r->w*r->h<hov.w*hov.h))))) { hov=*r; have_hov=1; hov_pri=pri; }   /* U2: the rows faded behind a Yes/No lose to its choices */
    }
    const U3Row raw_hov=hov; const int have_raw=have_hov;   /* (the text under the cursor, before the rules below) */
    if (have_hov && u3m)
        for (int i=0;i<8;++i)
            if (u3_ghost_us[i] && now-u3_ghost_us[i]<300000 && fabsf(u3_ghost_y[i]-hov.y)<1.0f) { have_hov=0; break; }   /* a ghost row: not a target */
    /* the cursor over no text but level with a row (between a setting's name and its value: U3 draws its < > there, on the
     * selected row only): the nearest text of that line, within 300 of the cursor */
    if (!have_hov) {
        float best_dx=300.0f;
        for (int i=0;i<U3_ROWS;++i) {
            const U3Row *r=&u3_rows[i];
            if (!r->us || now-r->us>=100000 || uy<r->y || uy>r->y+r->h || r->h>42.0f || (u3m && r->w>750.0f)) continue;
            int ghost=0;
            for (int g=0;g<8 && u3m;++g) if (u3_ghost_us[g] && now-u3_ghost_us[g]<300000 && fabsf(u3_ghost_y[g]-r->y)<1.0f) ghost=1;
            if (ghost) continue;
            const float dx=ux<r->x ? r->x-ux : ux>r->x+r->w ? ux-(r->x+r->w) : 0.0f;
            if (dx<best_dx) { best_dx=dx; hov=*r; have_hov=1; }
        }
    }
    /* U2's Yes/No: a box spanning both choices (520 wide, behind them) holds the cursor in the gap and at the choices' edges; it is
     * not a row of its own: a choice on its line within 20 of the cursor counts (when it holds two or more emphasised texts there);
     * the middle of the gap is no row */
    if (have_hov && !hov.front) {
        const float hcy=hov.y+hov.h*0.5f;
        U3Row closest={0};
        int inner=0;
        float best=1e9f;
        for (int i=0;i<U3_ROWS;++i) {
            const U3Row *r=&u3_rows[i];
            if (!r->us || now-r->us>=100000 || !r->front || fabsf(r->y+r->h*0.5f-hcy)>2.0f || r->x<hov.x-0.5f || r->x+r->w>hov.x+hov.w+0.5f) continue;
            ++inner;
            const float d=ux<r->x ? r->x-ux : ux>r->x+r->w ? ux-(r->x+r->w) : 0.0f;   /* to the choice's box */
            if (d<best) { best=d; closest=*r; }
        }
        if (inner>=2) { if (best<=20.0f) hov=closest; else have_hov=0; }
    }
    /* U3's Yes/No: the lit choice in the bar and another lit choice of its size in the same column. The jump cannot move it (it
     * would move the pause page behind it): the d-pad steps to the choice under the cursor (its bar's size counts), as on U2. */
    int dialog=0;
    if (sel_bar) {
        U3Row choice={0};
        int have_choice=0;
        for (int i=0;i<U3_ROWS;++i) {
            const U3Row *r=&u3_rows[i];
            if (!r->us || now-r->us>=100000 || r->sel<0.5f || fabsf(r->x-sel.x)>0.5f || fabsf(r->w-sel.w)>0.5f || fabsf(r->h-sel.h)>0.5f) continue;
            if (fabsf(r->y-sel.y)>1.0f) dialog=1;
            if (ux>=r->x-4.0f && ux<=r->x+r->w+4.0f && uy>=r->y-4.0f && uy<=r->y+r->h+4.0f) { choice=*r; have_choice=1; }
        }
        if (dialog) { hov=choice; have_hov=have_choice; }
    }
    pthread_mutex_unlock(&u3_lock);
    /* U2's Yes/No over the chapter carousel (Chapter Select's "Player stats, trophies ... are disabled in this mode. Continue?"
     * for Explorer): the cursor on one of two emphasised choices of the same size side by side (210x40 each, 100 apart). Its
     * box has the size of a locked reward's information box, the carousel stays drawn behind it and its lists lose the focus to
     * it as to a window: the choice is kept through those three rules. */
    int u2_choice_pair=0;
    if (u2m && have_hov && hov.front && hov.w<=300.0f) {
        pthread_mutex_lock(&u3_lock);
        for (int i=0;i<U3_ROWS && !u2_choice_pair;++i) {
            const U3Row *r=&u3_rows[i];
            if (!r->us || now-r->us>=100000 || !r->front || fabsf(r->x-hov.x)<1.0f) continue;
            u2_choice_pair=fabsf(r->y-hov.y)<2.0f && fabsf(r->w-hov.w)<1.0f && fabsf(r->h-hov.h)<1.0f;
        }
        pthread_mutex_unlock(&u3_lock);
    }
    if (info && !u2_choice_pair) have_hov=0;   /* an information box: the menu below is left alone */
    /* U2's drop-down choice (Options > Game > Difficulty: Easy .. Brutal over the page): a focused list of a few values with no
     * rectangle of its own whose texts the menu hooks do not see. It opens level with the lit row, one value per 29.2 below,
     * right of that row's label. The cursor there picks the value (the page behind is left alone: its hidden "On" took the click) */
    static struct { int on, want, cur; uint64_t ds; } dd;
    dd.on=0;
    /* (U3's difficulty choice too: a focused list showing all of its 2-8 values, 424 116 208x143 for five; its last rows are no texts the
     * hooks see, and its two lit items in the bar read as a Yes/No) */
    if ((u2m && have_sel && !dialog) || u3m) {
        pthread_mutex_lock(&u3_lock);
        for (int k=0;k<16 && !dd.on;++k) {
            const U2List *l=&u2_lists[k];
            if (!l->us || now-l->us>=150000 || !l->focused || l->vis<2 || l->vis>8) continue;
            if (u2m ? l->w!=u2_dropdown_w : (l->hi-l->lo+1!=l->vis || l->rw<40.0f || l->rh<20.0f*(float)l->vis)) continue;
            /* its rectangle once it is open (650 278 250x146: 5 rows of 29.2); while it opens, level with the lit row */
            const int rect=l->rw>40.0f && l->rh>=20.0f*(float)l->vis;
            const float pitch=rect ? l->rh/(float)l->vis : 29.2f, top=rect ? l->ry : sel.y+sel.h*0.5f-pitch*0.5f;
            const float left=rect ? l->rx-10.0f : sel.x+sel.w-20.0f, right=rect ? l->rx+l->rw+10.0f : sel.x+sel.w+420.0f;
            if (ux<left || ux>right || uy<top || uy>=top+pitch*(float)l->vis) continue;
            int v=l->top+(int)((uy-top)/pitch); v=v<l->lo ? l->lo : v>l->hi ? l->hi : v;
            dd.on=1; dd.want=v; dd.ds=l->ds; dd.cur=l->cur;
        }
        pthread_mutex_unlock(&u3_lock);
        if (dd.on) { have_hov=0; dialog=0; }
    }
    if (u2m) {   /* a window over U2's lists (a locked reward or skin): the lists on screen (controls with 3+ rows seen) have all lost the focus
                  * to it (their flag at +8): the lists below are left alone until it closes (the game's own state, any size or language) */
        int lists=0, focused=0;
        pthread_mutex_lock(&u3_lock);
        for (int i=0;i<16;++i) { const U2List *l=&u2_lists[i]; if (l->us && now-l->us<150000 && l->vis>=3 && l->rh>=70.0f) { ++lists; if (l->focused) ++focused; } }   /* (a settings row's selector is a control of one row, 34 tall: not a list) */
        if (lists && !focused && !u2_choice_pair) {
            have_hov=0;
            /* the window's own button (an unlocked skin's Enable): its lit row, under the cursor anywhere on the window's panel (a panel
             * drawn in the last 150 ms holding the lit row's centre) -> the click presses it */
            const float scx=sel.x+sel.w*0.5f, scy=sel.y+sel.h*0.5f;
            for (int i=0;i<8 && have_sel;++i) {
                const U3Row *p=&u2_panels[i];
                if (!p->us || now-p->us>=150000) continue;
                if (scx>=p->x && scx<=p->x+p->w && scy>=p->y && scy<=p->y+p->h && ux>=p->x && ux<=p->x+p->w && uy>=p->y && uy<=p->y+p->h) {
                    /* (one of the window's own buttons under the cursor, emphasised as its choices are: that one, OK in the
                     * difficulty window's OK / Cancel; elsewhere on the window the lit one) */
                    const int own=have_raw && raw_hov.front && raw_hov.x>=p->x && raw_hov.x+raw_hov.w<=p->x+p->w &&
                                  raw_hov.y>=p->y && raw_hov.y+raw_hov.h<=p->y+p->h;
                    hov=own ? raw_hov : sel; have_hov=1; break;
                }
            }
        }
        pthread_mutex_unlock(&u3_lock);
    }
    static uint64_t u3_win_cross_until;
    int win_hover=0;
    if (u3m && !have_hov && have_raw && !dialog) {   /* U3: a window (an unlocked skin's Enable, drawn unlit: the title above is the lit text): the
                                                   * text under the cursor inside a window panel drawn in the last 150 ms -> a click presses cross */
        int in_win=0;
        pthread_mutex_lock(&u3_lock);
        for (int k=0;k<8 && !in_win;++k) {
            const U3Row *p=&u2_panels[k];
            if (!p->us || now-p->us>=150000 || p->h>300.0f) continue;
            in_win=raw_hov.x>=p->x-1.0f && raw_hov.x+raw_hov.w<=p->x+p->w+1.0f && raw_hov.y>=p->y-1.0f && raw_hov.y+raw_hov.h<=p->y+p->h+1.0f &&
                   ux>=raw_hov.x-8.0f && ux<=raw_hov.x+raw_hov.w+8.0f && uy>=raw_hov.y-8.0f && uy<=raw_hov.y+raw_hov.h+8.0f;
        }
        pthread_mutex_unlock(&u3_lock);
        win_hover=in_win;
        if (in_win && clicked) { u3_win_cross_until=now+120000; if (mouse_debug()) puts("Menu mouse: U3 window button click: cross"); }
    }
    {   /* Treasures: the model's panel (the list's rectangle runs under it): no hover there; a press on it turns the model while held */
        static float mlast_x, mlast_y;
        const int model=model_us && now-model_us<200000 && model_us<=now+1000000;
        const int over=model && !dialog && (u3m ? ux>=736.0f && ux<=1046.0f && uy>=102.0f && uy<=490.0f : ux>=712.0f && uy>=130.0f && uy<=600.0f);
        if (!left || !model) model_drag=0;
        else if (clicked && over) { model_drag=1; if (mouse_debug()) puts("Menu mouse: Treasures model grabbed"); }
        if (model_drag) { model_ax+=(ux-mlast_x)*0.006f; model_ay+=(uy-mlast_y)*0.006f; }
        mlast_x=ux; mlast_y=uy;
        if (over || model_drag) have_hov=0;
    }
    /* U2's chapter carousel (antikmewzik's): the rows behind it are left alone; a click on its panel left of the pictures presses
     * left, right of them right, on the picture in the middle cross (choose) */
    static struct { uint32_t dir; uint64_t until; } car;
    /* (a Yes/No over it, Explorer's warning: its choices are the targets, not the carousel) */
    const int carousel=u2m && u2_popup_us && now-u2_popup_us<700000 && u2_popup_us<=now+1000000 && !u2_choice_pair;
    if (u2m && mouse_debug()) {   /* (debug) the rules that leave the rows alone, when they change */
        static int last=-1;
        const int state=u2_choice_pair | info<<1 | (u2_popup_us && now-u2_popup_us<700000)<<2 | have_hov<<3;
        if (state!=last) {
            last=state;
            printf("Menu mouse: U2 rules: Yes/No pair %d, information box %d, carousel %d -> cursor row %s\n",u2_choice_pair,info,
                   (state>>2)&1,have_hov ? "kept" : "none");
        }
    }
    if (carousel) {
        have_hov=0;
        if (clicked && ux>=298.0f && ux<=980.0f && uy>=156.0f && uy<=475.0f) {
            car.dir=ux<=430.0f ? BTN_LEFT : ux>=850.0f ? BTN_RIGHT : BTN_CROSS; car.until=now+(car.dir==BTN_CROSS ? 120000 : 70000);
            if (mouse_debug()) printf("Menu mouse: U2 chapter carousel click: %s\n",car.dir==BTN_LEFT ? "left" : car.dir==BTN_RIGHT ? "right" : "choose");
        }
    }
    if (now<car.until) out|=car.dir;
    {   /* the hand also over what answers a click without being a row: a window's button, the gallery viewers' < >, U2's chapter carousel */
        const int arrows=uy>=596.0f && uy<=664.0f && ((ux>=92.0f && ux<=146.0f) || (ux>=1134.0f && ux<=1188.0f));
        const int gal=arrows && ((u3m && u3_gallery_us && now-u3_gallery_us<4000000) ||
                                 (u2m && !have_sel && !have_hov && !dialog && !(u2_layout_us && now-u2_layout_us<1000000) && !carousel));
        const int car_hover=carousel && ux>=298.0f && ux<=980.0f && uy>=156.0f && uy<=475.0f;
        const float *cr=u3_car.rc;
        const int car3=u3m && u3_car.us && now-u3_car.us<150000 && cr[3]>200.0f && u3_car.hi>u3_car.lo && !dialog &&
                       ((ux>=cr[0]-55.0f && ux<=cr[0]-4.0f) || (ux>=cr[0]+cr[2]+4.0f && ux<=cr[0]+cr[2]+55.0f)) && uy>=cr[1]+60.0f && uy<=cr[1]+cr[3]-60.0f;
        menu_hover=have_hov || win_hover || gal || car_hover || car3;
    }
    const float hov_cy=hov.y+hov.h*0.5f, sel_cy=sel.y+sel.h*0.5f;
    int dialog_side=0;   /* U2: a Yes/No side by side on screen (two emphasised choices on one line): the wheel goes left / right */
    if (u2m && have_sel && sel.front) {
        pthread_mutex_lock(&u3_lock);
        for (int i=0;i<U3_ROWS && !dialog_side;++i) { const U3Row *r=&u3_rows[i]; if (r->us && now-r->us<100000 && r->front && fabsf(r->y+r->h*0.5f-sel_cy)<2.0f && fabsf(r->x-sel.x)>20.0f && r->h<=42.0f) dialog_side=1; }
        pthread_mutex_unlock(&u3_lock);
    }
    /* the same row: the same line, and (two columns: U2's main menu) overlapping in x */
    const int same=have_hov && have_sel && fabsf(hov_cy-sel_cy)<1.5f && hov.x<sel.x+sel.w && sel.x<hov.x+hov.w;
    if (moved) h.active=1;
    if (now<wheel_until_us[0] || now<wheel_until_us[1]) h.active=0;   /* the wheel moves the highlight: the cursor waits until it moves again */
    /* U2 settings rows (after antikmewzik's u2_menu_buttons): a click on the < > arrows of the selected row (two 33.6 tall texts 20
     * apart, 100+ wide, the glyph 15 into each: the left half of the 50 from the first is "left", the rest "right"), or on the first or
     * last of the values written in a row (Off / On: texts up to 130 wide right of x 600, two or more on a line that has its label left
     * of them) selects the row, then presses left or right once. Cross does nothing there; a middle value only selects the row. */
    static struct { uint32_t dir, press_dir; float cy; uint64_t until, seen, press_until; } ch;
    uint32_t picked=0;
    float picked_cy=0.0f;
    {   /* U3's Yes/No by value (see u3_dlg): its choices are texts 512 x 32 at x 384, 42 apart (the bar behind the chosen one 40 tall);
         * the one under the cursor is set, a click sets it and presses the cross once it is the choice. The d-pad steps do not run then. */
        static struct { int want; uint64_t t, cross_until; } dj={-1,0,0};
        if (u3m && u3_dialog_jump_on() && u3_dlg.us && now-u3_dlg.us<150000 && u3_dlg.hi>u3_dlg.lo && u3_dlg.hi-u3_dlg.lo<=8) {
            float ys[8], cx[16], cw[16], cy[16]; int n=0, nc=0, hovi=-1;
            pthread_mutex_lock(&u3_lock);
            for (int i=0;i<U3_ROWS;++i) {
                const U3Row *r=&u3_rows[i];
                /* (the choices: texts 32 tall and at least 200 wide inside the control's rectangle: Quit's at x 384, 512 wide; the
                 * difficulty window's OK / Cancel at x 344, 592 wide; any layout) */
                const float *q=u3_dlg.rc;
                if (!r->us || now-r->us>=150000 || fabsf(r->h-32.0f)>2.0f || r->w<200.0f) continue;
                if (q[2]>40.0f ? (r->x<q[0]-6.0f || r->x+r->w>q[0]+q[2]+6.0f || r->y<q[1]-6.0f || r->y+r->h>q[1]+q[3]+6.0f)
                               : (fabsf(r->x-384.0f)>2.0f || fabsf(r->w-512.0f)>2.0f)) continue;
                int dup=0; for (int k=0;k<nc;++k) if (fabsf(cy[k]-r->y)<1.0f && fabsf(cx[k]-r->x)<1.0f) dup=1;
                if (!dup && nc<16) { cx[nc]=r->x; cw[nc]=r->w; cy[nc]=r->y; ++nc; }
            }
            pthread_mutex_unlock(&u3_lock);
            /* (the choices share their x and width: the message lines inside the window do not) */
            const int need=u3_dlg.hi-u3_dlg.lo+1;
            for (int a=0;a<nc && n==0;++a) {
                int same=0; for (int b=0;b<nc;++b) if (fabsf(cx[b]-cx[a])<1.0f && fabsf(cw[b]-cw[a])<1.0f) ++same;
                if (same!=need) continue;
                for (int b=0;b<nc && n<8;++b) if (fabsf(cx[b]-cx[a])<1.0f && fabsf(cw[b]-cw[a])<1.0f) ys[n++]=cy[b];
            }
            for (int a=0;a<n;++a) for (int b=a+1;b<n;++b) if (ys[b]<ys[a]) { const float t=ys[a]; ys[a]=ys[b]; ys[b]=t; }
            const float qx0=u3_dlg.rc[2]>40.0f ? u3_dlg.rc[0] : 380.0f, qx1=u3_dlg.rc[2]>40.0f ? u3_dlg.rc[0]+u3_dlg.rc[2] : 900.0f;
            for (int i=0;i<n;++i) if (ux>=qx0 && ux<=qx1 && uy>=ys[i]-5.0f && uy<ys[i]+37.0f) hovi=i;
            if (n>=2 && n==u3_dlg.hi-u3_dlg.lo+1) {
                if (hovi>=0) {
                    const int want=u3_dlg.lo+hovi;
                    if (u2_set_pending && now-u2_set_us>300000) u2_set_pending=0;
                    if (want!=u3_dlg.cur && !u2_set_pending && (moved || clicked)) {
                        u2_set_ds=u3_dlg.ds; u2_set_val=want; u2_set_keep=0; u2_set_us=now; u2_set_pending=1;
                        if (mouse_debug()) printf("Menu mouse: U3 Yes/No choice %d -> %d\n",u3_dlg.cur,want);
                    }
                    if (clicked) { dj.want=want; dj.t=now; }
                }
                if (clicked) picked=1;
                menu_hover=hovi>=0;
                have_hov=0; h.click=0; h.active=0;   /* (no steps, no page jump) */
            }
        }
        if (dj.want>=0) {
            if (u3_dlg.us && now-u3_dlg.us<150000 && u3_dlg.cur==dj.want) { dj.cross_until=now+120000; dj.want=-1; if (mouse_debug()) puts("Menu mouse: U3 Yes/No click: cross"); }
            else if (now-dj.t>600000) dj.want=-1;
        }
        if (now<dj.cross_until) out|=BTN_CROSS;
    }
    if (clicked && mouse_debug()) printf("Menu mouse: click at %.0f %.0f: cursor row %s %.0f %.0f %.0fx%.0f, dialog %d\n",ux,uy,have_hov ? "yes" : "no",hov.x,hov.y,hov.w,hov.h,dialog);
    if (clicked && mouse_debug()) {   /* (debug) the texts drawn near the click */
        pthread_mutex_lock(&u3_lock);
        for (int i=0;i<U3_ROWS;++i) {
            const U3Row *r=&u3_rows[i];
            if (!r->us || now-r->us>=100000 || uy<r->y-40.0f || uy>r->y+r->h+40.0f || ux<r->x-200.0f || ux>r->x+r->w+200.0f) continue;
            printf("Menu mouse:   near: %.1f %.1f %.1fx%.1f sel %.2f front %d\n",r->x,r->y,r->w,r->h,r->sel,r->front);
        }
        pthread_mutex_unlock(&u3_lock);
    }
    if (clicked && u3m && !dialog) {   /* U3: a click on a row's < > (antikmewzik's U3 arrow click): left of the second text "left" */
        pthread_mutex_lock(&u3_lock);
        for (int i=0;i<16 && !picked;++i) {
            const U3Row *a=&u3_arrows[i];
            if (!a->us || now-a->us>=150000) continue;
            for (int j=0;j<16;++j) {
                const U3Row *b=&u3_arrows[j];
                if (i==j || !b->us || now-b->us>=150000 || fabsf(a->y-b->y)>=1.0f || b->x<=a->x || b->x>a->x+a->w+8.0f) continue;
                if (ux>=a->x-4.0f && ux<=b->x+b->w+4.0f && uy>=a->y && uy<a->y+a->h) { picked=ux<b->x ? BTN_LEFT : BTN_RIGHT; picked_cy=a->y+a->h*0.5f; break; }
            }
        }
        pthread_mutex_unlock(&u3_lock);
        if (picked==BTN_LEFT || picked==BTN_RIGHT) {   /* (picked 1: the Yes/No by value took the click) */
            ch.dir=picked; ch.cy=picked_cy; ch.until=now+1500000; ch.seen=0; h.active=1;
            if (mouse_debug()) printf("Menu mouse: U3 click on an arrow (%s) of the row at y %.0f\n",picked==BTN_LEFT ? "left" : "right",picked_cy);
        }
    }
    if (clicked && u2m && !dialog) {
        pthread_mutex_lock(&u3_lock);
        for (int i=0;i<U3_ROWS && !picked;++i) {
            const U3Row *a=&u3_rows[i];
            if (!a->us || now-a->us>=150000 || a->sel>=0.5f || a->h<33.0f || a->h>34.2f || a->w<100.0f) continue;
            for (int j=0;j<U3_ROWS;++j) {
                const U3Row *b=&u3_rows[j];
                if (i==j || !b->us || now-b->us>=150000 || b->sel>=0.5f || b->h<33.0f || b->h>34.2f || b->w<100.0f) continue;
                if (b->x>a->x+14.0f && b->x<a->x+26.0f && fabsf(a->y-b->y)<1.0f && uy>=a->y && uy<a->y+a->h && ux>=a->x && ux<a->x+50.0f) {
                    picked=ux<a->x+25.0f ? BTN_LEFT : BTN_RIGHT; picked_cy=a->y+a->h*0.5f;
                    break;
                }
            }
        }
        if (!picked && have_hov && hov.w<=130.0f && hov.x>=600.0f) {
            const float cy=hov.y+hov.h*0.5f;
            float minx=1e9f, maxx=-1e9f;
            int n=0, label=0;
            for (int i=0;i<U3_ROWS;++i) {
                const U3Row *r=&u3_rows[i];
                if (!r->us || now-r->us>=100000 || fabsf(r->y+r->h*0.5f-cy)>=4.0f) continue;
                if (r->x<600.0f) { label=1; continue; }
                if (r->w>130.0f) continue;
                ++n; if (r->x<minx) minx=r->x; if (r->x>maxx) maxx=r->x;
            }
            if (n>=2 && label) { picked=hov.x<=minx+1.0f ? BTN_LEFT : hov.x>=maxx-1.0f ? BTN_RIGHT : 0; picked_cy=cy; }
        }
        pthread_mutex_unlock(&u3_lock);
        if (picked) {
            ch.dir=picked; ch.cy=picked_cy; ch.until=now+1500000; ch.seen=0; h.active=1;
            if (mouse_debug()) printf("Menu mouse: U2 click on a value or arrow (%s) of the row at y %.0f\n",picked==BTN_LEFT ? "left" : "right",picked_cy);
        }
    }
    if (ch.dir && now>=ch.until) {
        if (mouse_debug()) printf("Menu mouse: value click dropped: the row at y %.0f never came (lit row %s at y %.0f)\n",ch.cy,have_sel ? "yes" : "no",sel_cy);
        ch.dir=0;
    }
    if (ch.dir) {   /* the clicked row is the selected one for a moment: press */
        if (have_sel && fabsf(sel_cy-ch.cy)<8.0f) {
            if (!ch.seen) ch.seen=now;
            if (now-ch.seen>=100000) {
                ch.press_dir=ch.dir; ch.press_until=now+70000; ch.dir=0;
                if (mouse_debug()) printf("Menu mouse: value press %s on the row at y %.0f\n",ch.press_dir==BTN_LEFT ? "left" : "right",ch.cy);
            }
        } else ch.seen=0;
    }
    /* U2, a page with < > rows (Chapter select): a click just left or right of a big picture (within 70 of its edge, at its height) is
     * its big < >: left or right is pressed at once. Any language: only the page's own picture and arrows are read. */
    /* (U3 with its carousel control seen: the chapter is set exactly below, no press the game may drop while the pictures slide) */
    const int u3_car_live=u3m && u3_car.us && now-u3_car.us<150000 && u3_car.rc[3]>200.0f && u3_car.hi>u3_car.lo;
    if (clicked && (u2m || u3m) && !picked && !dialog && !have_hov && !carousel && !u3_car_live) {
        pthread_mutex_lock(&u3_lock);
        int arrows=0;
        for (int i=0;i<16 && u3m && !arrows;++i) {   /* U3: its < > texts (10-20 wide) */
            const U3Row *a=&u3_arrows[i];
            if (!a->us || now-a->us>=150000) continue;
            for (int j=0;j<16;++j) {
                const U3Row *b=&u3_arrows[j];
                if (i!=j && b->us && now-b->us<150000 && fabsf(a->y-b->y)<1.0f && b->x>a->x && b->x<=a->x+a->w+8.0f) { arrows=1; break; }
            }
        }
        for (int i=0;i<U3_ROWS && !arrows;++i) {
            const U3Row *a=&u3_rows[i];
            if (!a->us || now-a->us>=150000 || a->sel>=0.5f || a->h<33.0f || a->h>34.2f || a->w<100.0f) continue;
            for (int j=0;j<U3_ROWS;++j) {
                const U3Row *b=&u3_rows[j];
                if (i!=j && b->us && now-b->us<150000 && b->h>=33.0f && b->h<=34.2f && b->w>=100.0f && b->x>a->x+14.0f && b->x<a->x+26.0f && fabsf(a->y-b->y)<1.0f) { arrows=1; break; }
            }
        }
        for (int i=0;i<8 && arrows && !picked;++i) {
            const U3Row *p=&u2_panels[i];
            if (!p->us || now-p->us>=150000 || uy<p->y || uy>p->y+p->h) continue;
            if (ux>=p->x-70.0f && ux<p->x) picked=BTN_LEFT;
            else if (ux>p->x+p->w && ux<=p->x+p->w+70.0f) picked=BTN_RIGHT;
        }
        pthread_mutex_unlock(&u3_lock);
        if (picked) {
            ch.press_dir=picked; ch.press_until=now+70000;
            if (mouse_debug()) printf("Menu mouse: picture arrow click: %s\n",picked==BTN_LEFT ? "left" : "right");
        }
    }
    if (now<ch.press_until) out|=ch.press_dir;
    /* dragging a slider (antikmewzik's fallback, from the drawn rail on both codes): a click on a rail selects its row (no cross);
     * while the button is held, one left / right press per tenth of the rail the cursor moves, once the row is the selected one */
    static struct { int on; uint32_t dir; float last, acc, w, cy; uint64_t next, press_until; } dr;
    /* U2 sliders, exact (antikmewzik's "slider direct"): the slider is a control with a data source like the lists (u2_list_update notes
     * it): a click on its rail sets the value where the rail is clicked, a held button follows the cursor. The control is the fresh one
     * with 3+ values laid out in the rail's row (its middle within 10, 20-45 tall). */
    static struct { int on; uint64_t ds; int lo, hi, sent; float x, w; } ds_sl;
    if ((u2m || u3m) && clicked && !picked && !dialog) {
        pthread_mutex_lock(&u3_lock);
        for (int i=0;i<8 && !picked;++i) {
            const U3Row *sl=&u3_sliders[i];
            if (!sl->us || now-sl->us>=100000 || ux<sl->x-10.0f || ux>sl->x+sl->w+10.0f || uy<sl->y || uy>=sl->y+sl->h) continue;
            const float cy=sl->y+sl->h*0.5f;
            float bd=10.0f; int best=-1;
            for (int k=0;k<16;++k) {
                const U2List *c=&u2_lists[k];
                if (!c->us || now-c->us>150000 || c->hi-c->lo<2 || c->rh<20.0f || c->rh>45.0f) continue;
                const float d=fabsf(c->ry+c->rh*0.5f-cy);
                if (d<bd) { bd=d; best=k; }
            }
            if (best>=0) { ds_sl.on=1; ds_sl.ds=u2_lists[best].ds; ds_sl.lo=u2_lists[best].lo; ds_sl.hi=u2_lists[best].hi; ds_sl.sent=-99999; ds_sl.x=sl->x; ds_sl.w=sl->w; picked=1; }
        }
        pthread_mutex_unlock(&u3_lock);
        if (picked && ds_sl.on && mouse_debug()) printf("Menu mouse: U2 slider grabbed (values %d..%d)\n",ds_sl.lo,ds_sl.hi);
    }
    if (ds_sl.on && !left) ds_sl.on=0;
    if (ds_sl.on && ds_sl.w>0.0f) {
        float f=(ux-ds_sl.x)/ds_sl.w; f=f<0.0f ? 0.0f : f>1.0f ? 1.0f : f;
        const int want=ds_sl.lo+(int)floorf(f*(float)(ds_sl.hi-ds_sl.lo)+0.5f);
        if (u2_set_pending && now-u2_set_us>300000) u2_set_pending=0;
        if (want!=ds_sl.sent && !u2_set_pending) { u2_set_ds=ds_sl.ds; u2_set_val=want; u2_set_keep=0; u2_set_us=now; u2_set_pending=1; ds_sl.sent=want; }
        h.active=0;
    }
    static int drag_on=-1;   /* off until it is as good as the arrows (user 2026-11-10): BB_MENU_SLIDER_DRAG=1 turns it on */
    if (drag_on<0) { const char *e=getenv("BB_MENU_SLIDER_DRAG"); drag_on=e && e[0]=='1'; }
    if (clicked && !picked && !dialog && drag_on) {
        pthread_mutex_lock(&u3_lock);
        for (int i=0;i<8;++i) {
            const U3Row *sl=&u3_sliders[i];
            if (!sl->us || now-sl->us>=100000 || ux<sl->x-10.0f || ux>sl->x+sl->w+10.0f || uy<sl->y || uy>=sl->y+sl->h) continue;
            dr.on=1; dr.last=ux; dr.acc=0.0f; dr.w=sl->w; dr.cy=sl->y+sl->h*0.5f; dr.next=0;
            picked=1;   /* (no cross: it does nothing on a slider) */
            if (mouse_debug()) printf("Menu mouse: slider grabbed (row at y %.0f, rail %.0f..%.0f)\n",dr.cy,sl->x,sl->x+sl->w);
            break;
        }
        pthread_mutex_unlock(&u3_lock);
    }
    if (dr.on && !left) dr.on=0;
    if (dr.on) {
        dr.acc+=ux-dr.last; dr.last=ux;
        const float step=dr.w/10.0f;
        if (dr.acc>2.0f*step) dr.acc=2.0f*step; else if (dr.acc<-2.0f*step) dr.acc=-2.0f*step;   /* no backlog: the knob stops with the mouse */
        if (have_sel && fabsf(sel_cy-dr.cy)<8.0f && fabsf(dr.acc)>=step && now>=dr.next) {
            dr.dir=dr.acc>0.0f ? BTN_RIGHT : BTN_LEFT;
            dr.acc-=dr.acc>0.0f ? step : -step;
            dr.press_until=now+60000; dr.next=now+100000;
        }
    }
    if (now<dr.press_until) out|=dr.dir;
    if (clicked && !picked && u3m && u3_gallery_us && now-u3_gallery_us<4000000 && uy>=596.0f && uy<=664.0f &&
        ((ux>=92.0f && ux<=146.0f) || (ux>=1134.0f && ux<=1188.0f))) {
        gallery_stick_dir=ux<640.0f ? -1 : 1; gallery_stick_until=now+150000; picked=1;
        if (mouse_debug()) printf("Menu mouse: U3 gallery arrow: %s\n",gallery_stick_dir<0 ? "left" : "right");
    }
    /* U2's art gallery viewer: no rows at all (the picture and its < > are images), the < > where U3 has them; a click there presses
     * left / right on the d-pad (the arrow keys page there). Not on the controller picture or the chapter carousel (the credits
     * flag is no test: the gallery's picture is as tall as the credits) */
    static uint64_t u2_gal_until; static int u2_gal_dir;
    if (clicked && !picked && u2m && !have_sel && !have_hov && !dialog && uy>=596.0f && uy<=664.0f &&
        ((ux>=92.0f && ux<=146.0f) || (ux>=1134.0f && ux<=1188.0f)) &&
        !(u2_layout_us && now-u2_layout_us<1000000) && !(u2_popup_us && now-u2_popup_us<1000000)) {
        u2_gal_dir=ux<640.0f ? BTN_LEFT : BTN_RIGHT; u2_gal_until=now+80000; picked=1;
        if (mouse_debug()) printf("Menu mouse: U2 gallery arrow: %s\n",ux<640.0f ? "left" : "right");
    } else if (clicked && u2m && mouse_debug() && uy>=596.0f && uy<=664.0f && ((ux>=92.0f && ux<=146.0f) || (ux>=1134.0f && ux<=1188.0f)))
        printf("Menu mouse: U2 gallery arrow spot, not taken: picked %d sel %d hov %d dialog %d layout %d popup %d\n",picked,have_sel,have_hov,dialog,
               u2_layout_us && now-u2_layout_us<1000000,u2_popup_us && now-u2_popup_us<1000000);
    if (now<u2_gal_until) out|=u2_gal_dir;
    /* U3 Chapter select, the opened carousel: its < > sit outside its rectangle (4 to 55 left / right of it, away from its top and bottom
     * 60): a click there sets the chapter one down / up (antikmewzik's) */
    const int car_on=u3m && u3_car.us && now-u3_car.us<150000 && u3_car.rc[3]>200.0f && u3_car.hi>u3_car.lo;
    if (clicked && car_on && !picked && !dialog) {
        const float *r=u3_car.rc;
        const int left_a=ux>=r[0]-55.0f && ux<=r[0]-4.0f, right_a=ux>=r[0]+r[2]+4.0f && ux<=r[0]+r[2]+55.0f;
        if ((left_a || right_a) && uy>=r[1]+60.0f && uy<=r[1]+r[3]-60.0f) {
            int v=(u2_set_pending && u2_set_ds==u3_car.ds ? u2_set_val : u3_car.cur)+(left_a ? -1 : 1);
            v=v<u3_car.lo ? u3_car.lo : v>u3_car.hi ? u3_car.hi : v;
            u2_set_ds=u3_car.ds; u2_set_val=v; u2_set_keep=0; u2_set_us=now; u2_set_pending=1; picked=1;
            if (mouse_debug()) printf("Menu mouse: U3 carousel arrow %s -> %d\n",left_a ? "left" : "right",v);
        }
    }
    if (clicked && u3m && !picked && mouse_debug() && u3_car.us)
        printf("Menu mouse: U3 carousel not taken: seen %lld ms ago, rect %.0f %.0f %.0fx%.0f, values %d..%d (now %d), dialog %d\n",
               (long long)((now-u3_car.us)/1000),u3_car.rc[0],u3_car.rc[1],u3_car.rc[2],u3_car.rc[3],u3_car.lo,u3_car.hi,u3_car.cur,dialog);
    if (clicked && have_hov && !picked) { h.click=1; h.click_deadline=now+1500000; h.active=1; }
    else if (clicked && !have_sel && !picked) h.click_until=now+150000;   /* no rows (the title's "press a button"): a click presses cross */
    if (h.click && now>h.click_deadline) h.click=0;
    /* U2: the page's focus jumps to the row under the cursor (u2_jump, on the game thread); a row no page holds (U2's Yes/No, whose
     * choices are not page rows) is stepped to with the d-pad instead, until the cursor goes to another row */
    const float hov_cx0=hov.x+hov.w*0.5f;
    if (h.nojump && (fabsf(hov_cy-h.nojump_cy)>1.0f || fabsf(hov_cx0-h.nojump_cx)>1.0f)) h.nojump=0;
    static int u3_timeouts;   /* U3: jump requests for this row no page answered (a page the jump does not know: Chapter select's) */
    if (((u2m && u3_jump_done<0) || (u3m && u3_jump_done==-3)) && !u3_jump_req && have_hov && fabsf(hov_cy-h.req_cy)<=1.0f) { h.nojump=1; h.nojump_cy=hov_cy; h.nojump_cx=hov_cx0; u3_jump_done=0; }
    /* the cursor over a list (a focused list control holding it: U2 Rewards, U3 Cutscenes / Treasures / Statistics): the list's own
     * value jumps (below), no page jump requests and no d-pad steps (U3 only got there after two unanswered page requests, then
     * stepped as well) */
    int in_list=0;
    U2List over_list={0};
    if ((u2m || u3m) && !dialog) {
        pthread_mutex_lock(&u3_lock);
        for (int i=0;i<16 && !in_list;++i) {
            const U2List *l=&u2_lists[i];
            if (l->us && now-l->us<150000 && l->focused && l->vis>=3 && l->rh>=70.0f && ux>=l->rx-30.0f && ux<=l->rx+l->rw && uy>=l->ry && uy<=l->ry+l->rh) { in_list=1; over_list=*l; }
        }
        pthread_mutex_unlock(&u3_lock);
    }
    const int step_mode=in_list || dialog || (u3m && h.nojump) || (u2m && (h.nojump || !u2_host || (u2_nopage_us && now-u2_nopage_us<1000000)));
    /* U2 1.02, a list (no page holds its rows): the value of the list is set to the row under the cursor at once (rows counted from the lit
     * one, by the rows' pitch) instead of d-pad steps */
    /* U2 1.02, the scroll bar of a long list (antikmewzik's): a drawing left of the list's rows (70 to 12 left of them), as tall as the
     * rows seen. A click on it, then a drag, puts the first row seen where the cursor is along it (the selection stays). The list and its
     * column come from the drawn rows: the column holding most rows 24-31 tall, the fresh list control with more values than rows seen. */
    static struct { int drag; uint64_t w; } sb;
    if ((u2m || u3m) && !dialog) {
        float xs[16]; int cnt[16], nx=0;
        pthread_mutex_lock(&u3_lock);
        for (int i=0;i<U3_ROWS;++i) {
            const U3Row *a=&u3_rows[i];
            if (!a->us || now-a->us>=100000 || a->h<24.0f || a->h>31.0f) continue;
            int k=0; while (k<nx && fabsf(xs[k]-a->x)>=1.0f) ++k;
            if (k==nx) { if (nx>=16) continue; xs[nx]=a->x; cnt[nx]=0; ++nx; }
            ++cnt[k];
        }
        int col=-1; for (int k=0;k<nx;++k) if (col<0 || cnt[k]>cnt[col] || (cnt[k]==cnt[col] && xs[k]<xs[col])) col=k;   /* (a tie: the leftmost, the bar is left of the list) */
        float top_cy=1e9f, bot_cy=-1e9f;
        if (col>=0) for (int i=0;i<U3_ROWS;++i) {
            const U3Row *a=&u3_rows[i];
            if (!a->us || now-a->us>=100000 || a->h<24.0f || a->h>31.0f || fabsf(a->x-xs[col])>=1.0f) continue;
            const float c=a->y+a->h*0.5f; if (c<top_cy) top_cy=c; if (c>bot_cy) bot_cy=c;
        }
        U2List l={0}; int have=0;
        for (int i=0;i<16 && col>=0;++i) {
            const U2List *c=&u2_lists[i];
            if (!c->us || now-c->us>150000 || c->vis<2 || c->hi-c->lo+1<=c->vis || abs(c->vis-cnt[col])>1 || !c->focused) continue;
            if (!have || c->hi-c->lo>l.hi-l.lo) { l=*c; have=1; }
        }
        pthread_mutex_unlock(&u3_lock);
        if (clicked && mouse_debug()) printf("Menu mouse: scroll bar check: column x %.0f (%d rows, %.0f..%.0f), list %s (%d seen)\n",col>=0 ? xs[col] : -1.0f,col>=0 ? cnt[col] : 0,top_cy,bot_cy,have ? "yes" : "no",have ? l.vis : 0);
        if (have && cnt[col]>=2) {
            const float pitch=(bot_cy-top_cy)/(float)(cnt[col]-1), top=top_cy-pitch*0.5f, rail_h=pitch*(float)l.vis, rowx=xs[col];
            /* (U2: a drawing 70 to 12 left of the rows; U3: the list's own left edge, antikmewzik's lx-8 .. lx+22) */
            const int in_rail=(u2m ? ux>=rowx-70.0f && ux<=rowx-12.0f : ux>=l.rx-8.0f && ux<=l.rx+22.0f) && uy>=top && uy<=top+rail_h;
            if (clicked && in_rail && !picked) {
                sb.drag=1; sb.w=l.w; picked=1;
                if (mouse_debug()) printf("Menu mouse: U2 scroll bar grabbed (list of %d, %d seen)\n",l.hi-l.lo+1,l.vis);
            }
            if (sb.drag && left && sb.w==l.w && !u2_scroll_pending) {
                const int n=l.hi-l.lo+1;
                float th=rail_h*(float)l.vis/(float)n; if (th<12.0f) th=12.0f;
                float f=(uy-th*0.5f-top)/(rail_h-th); f=f<0.0f ? 0.0f : f>1.0f ? 1.0f : f;
                const int t=l.lo+(int)(f*(float)(n-l.vis)+0.5f);
                if (t!=l.top) { u2_scroll_w=l.w; u2_scroll_top=t; u2_scroll_pending=1; }
            }
        }
    }
    static uint64_t sb_released;
    if (!left && sb.drag) { sb.drag=0; sb_released=now; }
    /* dragging the bar, and a moment after: no hover jumps or steps (the cursor past the list's end hovered the page's title, and the
     * d-pad steps towards it wrapped the list round to its other end) */
    const int sb_quiet=sb.drag || (sb_released && now-sb_released<400000);
    if (sb_quiet) { h.active=0; h.click=0; }
    int list_jump=0;
    static struct { int on, want; uint64_t ds, until, landed, cross_until; } lc;   /* a click on a list row (antikmewzik's u2_ls_cross) */
    if (lc.on) {
        U2List cl={0}; int found=0;
        pthread_mutex_lock(&u3_lock);
        for (int i=0;i<16 && !found;++i) if (u2_lists[i].us && u2_lists[i].ds==lc.ds && now-u2_lists[i].us<150000) { cl=u2_lists[i]; found=1; }
        pthread_mutex_unlock(&u3_lock);
        if (now>lc.until || !found) lc.on=0;
        else if (cl.cur==lc.want) {
            if (!lc.landed) lc.landed=now;
            else if (now-lc.landed>=120000) { lc.on=0; lc.cross_until=now+120000; if (mouse_debug()) printf("Menu mouse: U2 list click: cross on value %d\n",lc.want); }
        } else lc.landed=0;
    }
    if (now<lc.cross_until) out|=BTN_CROSS;
    if (dd.on) {   /* (the drop-down, above) its value follows the cursor; a click presses cross once it is there */
        menu_hover=1; list_jump=1; h.click=0; h.click_until=0;
        if (dd.want!=dd.cur && (!u2_set_pending || u2_set_val!=dd.want)) { u2_set_ds=dd.ds; u2_set_val=dd.want; u2_set_keep=0; u2_set_us=now; u2_set_pending=1; }
        if (clicked) {
            lc.on=1; lc.ds=dd.ds; lc.want=dd.want; lc.until=now+1500000; lc.landed=0;
            if (mouse_debug()) printf("Menu mouse: U2 drop-down click on value %d\n",dd.want);
        }
    }
    /* U2's Yes/No and OK / Cancel by value (no d-pad steps): their two emphasised choices side by side are a focused list control of
     * the values 0..1 (Quit, the difficulty window); the choice under the cursor left of the lit one is one value down, right of it one
     * up; it is set at once, a click presses cross once it is the value */
    if (u2m && !dd.on && have_hov && have_sel && hov.front && sel.front && hov.h>=36.0f && sel.h>=36.0f && hov.w>=120.0f &&
        fabsf((hov.y+hov.h*0.5f)-(sel.y+sel.h*0.5f))<4.0f) {   /* (the choices' boxes: 210 x 40; a setting's Off / On, 25 tall, are no Yes/No) */
        U2List yn={0}; int have_yn=0;
        pthread_mutex_lock(&u3_lock);
        for (int k=0;k<16 && !have_yn;++k) {
            const U2List *l=&u2_lists[k];
            if (l->us && now-l->us<150000 && l->focused && l->lo==0 && l->hi==1 && l->cur>=0 && l->cur<=1) { yn=*l; have_yn=1; }
        }
        pthread_mutex_unlock(&u3_lock);
        if (have_yn) {
            const float hcx=hov.x+hov.w*0.5f, scx=sel.x+sel.w*0.5f;
            const int want=fabsf(hcx-scx)<2.0f ? yn.cur : hcx<scx ? yn.cur-1 : yn.cur+1;
            if (want>=0 && want<=1) {
                list_jump=1; h.click=0; h.click_until=0; menu_hover=1;
                if (want!=yn.cur && (!u2_set_pending || u2_set_val!=want)) {
                    u2_set_ds=yn.ds; u2_set_val=want; u2_set_keep=0; u2_set_us=now; u2_set_pending=1;
                    if (mouse_debug()) printf("Menu mouse: U2 Yes/No choice %d -> %d\n",yn.cur,want);
                }
                if (clicked) { lc.on=1; lc.ds=yn.ds; lc.want=want; lc.until=now+1500000; lc.landed=0; }
            }
        }
    }
    /* U2 1.02, a list (no page holds its rows): its value is set to the row under the cursor at once. The list's rows are the texts in
     * the hovered row's column (24-31 tall); the list control is the fresh one whose rows seen match their count; the row's place counts
     * from its first row seen (the top of the column). No drawn highlight is read: it fades after a jump (counting from it overshot), and
     * some lists draw other lit texts (Skins). A hover never scrolls the list. */
    if ((u2m || u3m) && step_mode && !dialog && !sb_quiet && have_hov && hov.h>=24.0f && hov.h<=31.0f && (h.active || h.click || clicked)) {
        float pitch=1e9f, top_cy=1e9f, ys[32];
        int n=0;
        pthread_mutex_lock(&u3_lock);
        for (int i=0;i<U3_ROWS;++i) {
            const U3Row *a=&u3_rows[i];
            if (!a->us || now-a->us>=100000 || fabsf(a->x-hov.x)>=1.0f || a->h<24.0f || a->h>31.0f) continue;
            const float acy=a->y+a->h*0.5f;
            int dup=0;
            for (int k=0;k<n;++k) if (fabsf(ys[k]-acy)<2.0f) dup=1;
            if (dup || n>=32) continue;
            ys[n++]=acy;
            if (acy<top_cy) top_cy=acy;
        }
        pthread_mutex_unlock(&u3_lock);
        for (int a=0;a<n;++a) for (int b=0;b<n;++b) { const float d=ys[b]-ys[a]; if (d>12.0f && d<pitch) pitch=d; }
        U2List l, best={0};
        int have=0, bestd=1000;
        pthread_mutex_lock(&u3_lock);
        for (int i=0;i<16;++i) {
            l=u2_lists[i];
            if (!l.us || now-l.us>150000 || l.hi-l.lo<1 || l.cur<l.lo || l.cur>l.hi || l.vis<1 || !l.focused) continue;
            const int rows=l.hi-l.lo+1<l.vis ? l.hi-l.lo+1 : l.vis;   /* (a short list: all its values seen) */
            const int d=abs(rows-n);
            if (d<bestd || (d==bestd && l.hi-l.lo>best.hi-best.lo)) { bestd=d; best=l; have=1; }
        }
        pthread_mutex_unlock(&u3_lock);
        if (have && bestd<=1 && n>=2 && pitch<100.0f) {
            l=best;
            list_jump=1;
            int want=l.top+(int)lroundf((hov_cy-top_cy)/pitch);
            if (want<l.top) want=l.top;
            if (want>l.top+l.vis-1) want=l.top+l.vis-1;
            if (want>l.hi) want=l.hi;
            if (want==l.cur) { h.req_cy=hov_cy; h.req_us=now; }   /* (there already: nothing to ask) */
            /* U3 Statistics (antikmewzik): a line with its value in a second text right of the label (28.6 tall) opens the comparison
             * with friends, which does not work offline: a click only selects it (sub-menu lines, no value, keep the cross) */
            int stat_value=0;
            if (u3m && clicked && hov.h>=26.0f && hov.h<=30.0f && (hov.x<288.0f || hov.x>350.0f)) {
                pthread_mutex_lock(&u3_lock);
                for (int i=0;i<U3_ROWS && !stat_value;++i) { const U3Row *r=&u3_rows[i]; if (r->us && now-r->us<100000 && r->w>=5.0f && fabsf(r->y-hov.y)<2.0f && r->x>=hov.x+hov.w-1.0f) stat_value=1; }
                pthread_mutex_unlock(&u3_lock);
            }
            if (clicked && !stat_value) { lc.on=1; lc.ds=l.ds; lc.want=want; lc.until=now+1500000; lc.landed=0; h.click=0; }   /* cross once the list is there */
            else if (clicked) h.click=0;
            if (u2_set_pending && now-u2_set_us>300000) u2_set_pending=0;
            if (!u2_set_pending && (fabsf(hov_cy-h.req_cy)>1.0f || now-h.req_us>150000)) {
                u2_set_ds=l.ds; u2_set_val=want; u2_set_keep=1; u2_set_us=now; u2_set_pending=1;
                h.req_cy=hov_cy; h.req_us=now;
            }
        }
    }
    /* U2 steps only between a Yes/No's two emphasised choices: its pages and lists jump directly, and steps towards anything else (a
     * list's title, a heading) pressed Up past the first row and wrapped the list round to its end */
    const int u2_choice=hov.front && sel.front;
    if (step_mode && !list_jump && !in_list && !sb_quiet && (!u2m || u2_choice) && have_hov && have_sel && !same && (h.active || h.click)) {
        /* U2: one d-pad press every 110 ms towards the row under the cursor; a row the presses never reach (a section title) is given up
         * after as many presses as rows between them and two more, until the cursor moves to another row */
        const float hov_cx=hov.x+hov.w*0.5f, sel_cx=sel.x+sel.w*0.5f;
        if (fabsf(hov_cy-h.step_cy)>1.0f || fabsf(hov_cx-h.step_cx)>1.0f) { h.step_cy=hov_cy; h.step_cx=hov_cx; h.steps=0; }
        const int budget=(int)ceilf(fabsf(hov_cy-sel_cy)/fmaxf(hov.h,20.0f))+2;
        /* the next press once the highlight has left the row of the last one (its fade takes a while: pressing on a timer overshot), or
         * after 250 ms if it has not moved */
        const int moved_on=h.steps>0 && now>=h.step_until+20000 && (fabsf(sel_cy-h.step_sel_cy)>1.0f || fabsf(sel_cx-h.step_sel_cx)>1.0f);
        /* sideways only between two choices drawn emphasised (a Yes/No): on a settings row left/right change its value */
        const int sideways=fabsf(hov_cy-sel_cy)<=1.5f;
        if (sideways && !(hov.front && sel.front) && !dialog) h.steps=budget;
        /* two presses that did not move the highlight: the row under the cursor is not one the presses reach (a title; the lit row
         * read is not this list's): given up, pressing on would wrap the list round */
        if (h.steps>=2 && now>=h.next_step && !moved_on && fabsf(sel_cy-h.step_sel_cy)<=1.0f && fabsf(sel_cx-h.step_sel_cx)<=1.0f) h.steps=budget;
        if ((now>=h.next_step || moved_on) && h.steps<budget) {
            h.step_dir=hov_cy>sel_cy+1.5f ? BTN_DOWN : hov_cy<sel_cy-1.5f ? BTN_UP : hov_cx>sel_cx ? BTN_RIGHT : BTN_LEFT;
            h.step_until=now+50000; h.next_step=now+250000; ++h.steps;
            h.step_sel_cy=sel_cy; h.step_sel_cx=sel_cx;
            if (mouse_debug()) printf("Menu mouse: step %d/%d dir %#x: cursor row %.0f %.0f, lit row %.0f %.0f (%.2f)%s\n",h.steps,budget,(unsigned)h.step_dir,hov_cx,hov_cy,sel_cx,sel_cy,sel.sel,moved_on ? " (the highlight moved)" : "");
        }
        if (h.steps>=budget && now>=h.next_step) { h.active=0; h.click=0; if (mouse_debug()) printf("Menu mouse: steps given up: cursor row %.0f %.0f, lit row %.0f %.0f (%.2f)\n",hov_cx,hov_cy,sel_cx,sel_cy,sel.sel); }
    }
    if (now<h.step_until) out|=(uint32_t)h.step_dir;
    if (!step_mode && have_hov && !same && (h.active || h.click) && !u3_jump_req) {
        /* the page answers within a frame: ask for a new row at once, again every 150 ms while the highlight has not come */
        if (fabsf(hov_cy-h.req_cy)>1.0f || now-h.req_us>150000) {
            if (fabsf(hov_cy-h.req_cy)>1.0f) u3_timeouts=0;
            u3_jump_cx=hov.x+hov.w*0.5f; u3_jump_cy=hov_cy; u3_jump_done=0; u3_jump_req=1;
            h.req_cy=hov_cy; h.req_us=now;
        }
    }
    if (u3_jump_req && now-h.req_us>300000) {   /* no page took it (the page was changing): asked again later; twice in a row on U3: the d-pad steps */
        u3_jump_req=0;
        if (u3m && ++u3_timeouts>=2) { u3_jump_done=-3; u3_timeouts=0; if (mouse_debug()) printf("Menu mouse: U3 jump not taken twice: the d-pad steps to the row at y %.0f\n",hov_cy); }
    }
    /* stepping (a Yes/No): the cursor's choice counts as reached once the highlight has settled there; mid-fade the old choice can
     * still look the most lit, and stopping then left the highlight on the other choice until the mouse moved again */
    if (mouse_debug()) {   /* the hover's state while the cursor moves (5 a second at most) */
        static uint64_t shown;
        if (moved && now-shown>200000) {
            shown=now;
            printf("Menu mouse: at %.0f %.0f: cursor row %s %.0f %.0f %.0fx%.0f front %d, lit row %s %.0f %.0f (%.2f), same %d, active %d click %d, step mode %d (dialog %d nojump %d), steps %d\n",
                   ux,uy,have_hov ? "yes" : "no",hov.x,hov.y,hov.w,hov.h,hov.front,have_sel ? "yes" : "no",sel.x+sel.w*0.5f,sel_cy,sel.sel,same,h.active,h.click,
                   step_mode,dialog,h.nojump,h.steps);
        }
    }
    if (same && (!step_mode || sel.sel>=0.95f)) {
        h.active=0;
        if (h.click) { h.click=0; h.click_until=now+120000; }
    }
    if (now<h.click_until) out|=BTN_CROSS;
    if (u3m && now<u3_win_cross_until) out|=BTN_CROSS;
    /* the wheel (antikmewzik's per-screen wheel): over a list longer than its rows seen it scrolls the view three rows a notch (the
     * selection stays); on a Yes/No side by side (U2) it goes left / right; elsewhere a d-pad row */
    static uint64_t wheel_seen[2];
    int wheel_done=0;
    const int over_car=car_on && !dialog && ux>=u3_car.rc[0]-55.0f && ux<=u3_car.rc[0]+u3_car.rc[2]+55.0f && uy>=u3_car.rc[1] && uy<=u3_car.rc[1]+u3_car.rc[3];
    if (over_car) {   /* U3 Chapter select: the wheel over the opened carousel changes the chapter (antikmewzik's) */
        for (int i=0;i<2;++i) {
            if (wheel_until_us[i]==wheel_seen[i]) continue;
            wheel_seen[i]=wheel_until_us[i];
            if (now>=wheel_until_us[i]) continue;
            int v=(u2_set_pending && u2_set_ds==u3_car.ds ? u2_set_val : u3_car.cur)+(i==0 ? -1 : 1);
            v=v<u3_car.lo ? u3_car.lo : v>u3_car.hi ? u3_car.hi : v;
            u2_set_ds=u3_car.ds; u2_set_val=v; u2_set_keep=0; u2_set_us=now; u2_set_pending=1;
            if (mouse_debug()) printf("Menu mouse: U3 carousel wheel -> %d\n",v);
        }
        wheel_done=1;
    } else if (in_list && over_list.hi-over_list.lo+1>over_list.vis) {
        for (int i=0;i<2;++i) {
            if (wheel_until_us[i]==wheel_seen[i]) continue;
            wheel_seen[i]=wheel_until_us[i];
            int t=(u2_scroll_pending && u2_scroll_w==over_list.w ? u2_scroll_top : over_list.top)+(i==0 ? -3 : 3);
            if (t>over_list.hi+1-over_list.vis) t=over_list.hi+1-over_list.vis;
            if (t<over_list.lo) t=over_list.lo;
            u2_scroll_w=over_list.w; u2_scroll_top=t; u2_scroll_pending=1;
        }
        wheel_done=1;
    } else { wheel_seen[0]=wheel_until_us[0]; wheel_seen[1]=wheel_until_us[1]; }
    if (!info && !wheel_done) {
        const int across=u2m && dialog_side;
        if (now<wheel_until_us[0]) out|=across ? BTN_LEFT : BTN_UP;
        if (now<wheel_until_us[1]) out|=across ? BTN_RIGHT : BTN_DOWN;
    }
    return out;
}

/* ---- port: the loader and the pad read -------------------------------------------------------- */
static int menu_mouse_on(void) {
    static int on=-1;
    if (on<0) { const char *e=getenv("BB_MENU_MOUSE"); on=!(e && e[0]=='0'); }
    return on;
}
void runtime_menumouse_prepare(unsigned char *image, uint64_t image_size) {
    mm_image=image; mm_image_size=image_size;
    if (!menu_mouse_on()) { puts("Menu mouse: off (BB_MENU_MOUSE=0)"); return; }
    for (size_t i=0;i<sizeof(u1_menu_codes)/sizeof(*u1_menu_codes) && !u1m;++i) {
        const U1MenuCode *c=&u1_menu_codes[i];
        /* the text drawing routine first: it tells Uncharted 1 of this code from the other games */
        if (!code_is(c->text_draw,u1_text_draw_code,sizeof(u1_text_draw_code))) continue;
        const struct { const char *what; uint64_t at; const unsigned char *code; size_t n; } checks[]={
            {"menu item draw (1)",c->item_begin1,u1_menu_item_begin_code,sizeof(u1_menu_item_begin_code)},
            {"menu item draw end (1)",c->item_end1,u1_menu_item_end1_code,sizeof(u1_menu_item_end1_code)},
            {"menu item draw (2)",c->item_begin2,u1_menu_item_begin_code,sizeof(u1_menu_item_begin_code)},
            {"menu item draw end (2)",c->item_end2,u1_menu_item_end2_code,sizeof(u1_menu_item_end2_code)},
            {"menu event",c->menu_event,u1_menu_event_code,sizeof(u1_menu_event_code)},
            {"credits",c->credits,u1_credits_code,sizeof(u1_credits_code)},
            {"select a row",c->select_index,u1_select_index_code,sizeof(u1_select_index_code)},
        };
        const char *bad=NULL;
        for (size_t k=0;k<sizeof(checks)/sizeof(*checks) && !bad;++k) if (!code_is(checks[k].at,checks[k].code,checks[k].n)) bad=checks[k].what;
        const unsigned char *ft=code_ref(&c->frame_time);
        if (!bad && !ft) bad="frame time";
        if (bad) { printf("Menu mouse: Uncharted 1 %s: the %s code differs: menus without the mouse\n",c->version,bad); break; }
        u1_frame_time=(const volatile float *)ft;
        {   /* the volumes: the audio object's floats, where its setter stores them */
            static const unsigned char set[]={0x48,0x63,0xc6,0xc5,0xfa,0x11,0x84,0x87,0x20,0xdb,0x03,0x00,0xc3};
            const unsigned char *audio=code_ref(&c->audio);
            if (audio && code_is(c->volume_set-3,set,sizeof(set))) u1_volumes=(uint64_t)(audio-image)+0x3db20u;
            else printf("Menu mouse: Uncharted 1 %s: the volume code differs: the volume sliders go by steps\n",c->version);
        }
        u1_image=(uint64_t)(uintptr_t)image;
        u1m=c;
        printf("Menu mouse: Uncharted 1 (game code %s): menus follow the mouse\n",c->version);
    }
    for (size_t i=0;i<sizeof(u3_menu_codes)/sizeof(*u3_menu_codes) && !u1m && !u3m;++i) {
        const U3MenuCode *c=&u3_menu_codes[i];
        const int v102=i==0;
        const int text=code_is(c->text,v102 ? u3_text_102_code : u3_text_100_code,v102 ? sizeof(u3_text_102_code) : sizeof(u3_text_100_code));
        const int page=code_is(c->page,v102 ? u3_page_102_code : u3_page_100_code,v102 ? sizeof(u3_page_102_code) : sizeof(u3_page_100_code));
        if (!text && !page) continue;
        if (!text || !page) { printf("Menu mouse: Uncharted 3 %s: the %s code differs: menus without the mouse\n",c->version,text ? "menu page" : "menu text"); break; }
        u3m=c;
        printf("Menu mouse: Uncharted 3 (game code %s): menus follow the mouse\n",c->version);
    }
    for (size_t i=0;i<sizeof(u2_menu_codes)/sizeof(*u2_menu_codes) && !u1m && !u3m && !u2m;++i) {
        const U2MenuCode *c=&u2_menu_codes[i];
        const int v102=i==0;
        if (!code_is(c->text,v102 ? u2_text_102_code : u2_text_100_code,v102 ? sizeof(u2_text_102_code) : sizeof(u2_text_100_code))) continue;
        u2m=c;
        printf("Menu mouse: Uncharted 2 (game code %s): menus follow the mouse\n",c->version);
    }
    if (!u1m && !u3m && !u2m) puts("Menu mouse: no known menu code in this executable (the menus keep the keys and the controller)");
    fflush(stdout);
}
#define CB(name) static void MS_ABI name(uint64_t rdi, uint64_t rsi, uint64_t base, uint64_t r14, const uint64_t *regs)
CB(u1_item_begin_102) { if (u1m==&u1_menu_codes[0]) u1_menu_item_begin(rdi,rsi,base,r14,regs); }
CB(u1_item_begin_100) { if (u1m==&u1_menu_codes[1]) u1_menu_item_begin(rdi,rsi,base,r14,regs); }
CB(u1_item_end_102) { if (u1m==&u1_menu_codes[0]) u1_menu_item_end(rdi,rsi,base,r14,regs); }
CB(u1_item_end_100) { if (u1m==&u1_menu_codes[1]) u1_menu_item_end(rdi,rsi,base,r14,regs); }
CB(u1_text_102) { if (u1m==&u1_menu_codes[0]) u1_text_draw(rdi,rsi,base,r14,regs); }
CB(u1_text_100) { if (u1m==&u1_menu_codes[1]) u1_text_draw(rdi,rsi,base,r14,regs); }
CB(u1_event_102) { if (u1m==&u1_menu_codes[0]) u1_menu_event(rdi,rsi,base,r14,regs); }
CB(u1_event_100) { if (u1m==&u1_menu_codes[1]) u1_menu_event(rdi,rsi,base,r14,regs); }
CB(u1_credits_102) { if (u1m==&u1_menu_codes[0]) u1_credits_tick(rdi,rsi,base,r14,regs); }
CB(u1_credits_100) { if (u1m==&u1_menu_codes[1]) u1_credits_tick(rdi,rsi,base,r14,regs); }
CB(u3_text_cb) { (void)rdi; (void)rsi; (void)r14; if (u3m || u2m) u3_text(regs); if (u2m) u2_jump(base); }
CB(u3_page_cb) { (void)rsi; (void)r14; (void)regs; if (u3m) u3_page(rdi,base); }
#define SITE(name,at,sig,moved,cb) {name,at,sig,sizeof(sig),moved,cb,NULL}
static const RuntimeGuestCallback u1_sites[]={
    SITE("U1 1.02 menu item draw (1) (menu mouse)",0x133fbc,u1_menu_item_begin_code,9,u1_item_begin_102),
    SITE("U1 1.02 menu item draw end (1) (menu mouse)",0x133fc8,u1_menu_item_end1_code,7,u1_item_end_102),
    SITE("U1 1.02 menu item draw (2) (menu mouse)",0x133feb,u1_menu_item_begin_code,9,u1_item_begin_102),
    SITE("U1 1.02 menu item draw end (2) (menu mouse)",0x133ff7,u1_menu_item_end2_code,8,u1_item_end_102),
    SITE("U1 1.02 menu text draw (menu mouse)",0x13acf0,u1_text_draw_code,6,u1_text_102),
    SITE("U1 1.02 menu event (menu mouse)",0x1336c0,u1_menu_event_code,6,u1_event_102),
    SITE("U1 1.02 credits (menu mouse)",0x3b41e0,u1_credits_code,6,u1_credits_102),
    SITE("U1 1.00 menu item draw (1) (menu mouse)",0x133e2c,u1_menu_item_begin_code,9,u1_item_begin_100),
    SITE("U1 1.00 menu item draw end (1) (menu mouse)",0x133e38,u1_menu_item_end1_code,7,u1_item_end_100),
    SITE("U1 1.00 menu item draw (2) (menu mouse)",0x133e5b,u1_menu_item_begin_code,9,u1_item_begin_100),
    SITE("U1 1.00 menu item draw end (2) (menu mouse)",0x133e67,u1_menu_item_end2_code,8,u1_item_end_100),
    SITE("U1 1.00 menu text draw (menu mouse)",0x13aa70,u1_text_draw_code,6,u1_text_100),
    SITE("U1 1.00 menu event (menu mouse)",0x133530,u1_menu_event_code,6,u1_event_100),
    SITE("U1 1.00 credits (menu mouse)",0x576a30,u1_credits_code,6,u1_credits_100),
};
/* (6 bytes moved: push rbp; mov rbp, rsp; push r15) */

/* U3's slider draw (antikmewzik's u3_widget_hook; 1.02 0x5e2190, EU 1.00 0x198cf0 located 2026-11-10 by the same data-source calls:
 * +0x30, get, minimum +0x38, maximum +0x40): the slider is noted in u2_lists like U2's sliders (its rectangle +0x90) and takes a pending
 * set(value): a click on the rail sets the value where it is clicked (u3_menu_buttons, the U2 slider code). */
static void u3_slider_update(uint64_t self, uint64_t image) {
    uint64_t ds=0, vt=0, f0=0, f8=0, fmin=0, fmax=0; float rc[4]={0,0,0,0};
    if (self<0x10000 || !safe_read(rc,self+0x90,16) || !safe_read(&ds,self+0x18,8) || ds<0x10000 || !safe_read(&vt,ds,8) || vt<0x10000) return;
    if (!safe_read(&f0,vt,8) || !safe_read(&f8,vt+8,8) || !safe_read(&fmin,vt+0x38,8) || !safe_read(&fmax,vt+0x40,8)) return;
    const uint64_t lo_code=image, hi_code=image+0x2000000;
    if (f0<lo_code || f0>=hi_code || f8<lo_code || f8>=hi_code || fmin<lo_code || fmin>=hi_code || fmax<lo_code || fmax>=hi_code) return;
    int cur=(int)(int32_t)((guest_vcall_fn)(uintptr_t)f0)(ds);
    const int lo=(int)(int32_t)((guest_vcall_fn)(uintptr_t)fmin)(ds), hi=(int)(int32_t)((guest_vcall_fn)(uintptr_t)fmax)(ds);
    if (hi-lo<2) return;
    if (u2_set_pending && u2_set_ds==ds) {
        int v=u2_set_val; if (v<lo) v=lo; if (v>hi) v=hi;
        ((guest_set_fn)(uintptr_t)f8)(ds,v);
        u2_set_pending=0;
        if (mouse_debug()) printf("Menu mouse: U3 slider set(%d) (was %d, range %d..%d)\n",v,cur,lo,hi);
        cur=v;
    }
    uint8_t fl=0;
    safe_read(&fl,self+8,1);
    const uint64_t now=now_us();
    pthread_mutex_lock(&u3_lock);
    int slot=-1, oldest=0;
    for (int i=0;i<16;++i) { if (u2_lists[i].w==self) { slot=i; break; } if (u2_lists[i].us<u2_lists[oldest].us) oldest=i; }
    if (slot<0) { slot=oldest; if (mouse_debug()) printf("Menu mouse: U3 slider control %#llx: values %d..%d (now %d), rect %.0f %.0f %.0fx%.0f\n",(unsigned long long)self,lo,hi,cur,rc[0],rc[1],rc[2],rc[3]); }
    u2_lists[slot]=(U2List){self,ds,cur,lo,hi,0,0,now,rc[0],rc[1],rc[2],rc[3],fl};
    pthread_mutex_unlock(&u3_lock);
}
static const unsigned char u3_slider_102_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x28,0x01,0x00,0x00,0x49,0x89,0xd7};
static const unsigned char u3_slider_100_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xa8,0x00,0x00,0x00,0x49,0x89,0xd7};
CB(u3_slider_cb) { (void)rsi; (void)r14; (void)regs; if (u3m) u3_slider_update(rdi,base); }
/* (EU 1.00 = US 1.00: 0x199e40, located 2026-11-10 like U2's: the code reading +0x16c / +0x184 / +0x188 together sits just before
 * it, as before 1.02's 0x5e3e60) */
static const unsigned char u3_list_100_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x98,0x0f,0x00,0x00,0x49,0x89,0xd7};
static const unsigned char u3_list_102_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x68,0x0b,0x00,0x00};
CB(u3_list_cb) { (void)rsi; (void)r14; (void)regs; if (u3m) u3_list_update(rdi,base,0); }
static const RuntimeGuestCallback u3_sites[]={
    SITE("U3 1.02 menu text (menu mouse)",0x63d6f0,u3_text_102_code,6,u3_text_cb),
    SITE("U3 1.02 menu page update (menu mouse)",0x6253f0,u3_page_102_code,6,u3_page_cb),
    SITE("U3 1.02 list control draw (menu mouse)",0x5e3e60,u3_list_102_code,8,u3_list_cb),
    SITE("U3 1.02 slider draw (menu mouse)",0x5e2190,u3_slider_102_code,8,u3_slider_cb),
    SITE("U3 1.00 menu text (menu mouse)",0x1d6060,u3_text_100_code,6,u3_text_cb),
    SITE("U3 1.00 menu page update (menu mouse)",0x1c5000,u3_page_100_code,6,u3_page_cb),
    SITE("U3 1.00 list control draw (menu mouse)",0x199e40,u3_list_100_code,8,u3_list_cb),
    SITE("U3 1.00 slider draw (menu mouse)",0x198cf0,u3_slider_100_code,8,u3_slider_cb),
};
/* (EU 1.00 = US 1.00: the same function at 0x267f60, located 2026-11-10 by its scroll call: the value stored to +0x228 and +0x22c, then
 * vtable +0x68 with it in esi and edx; the same widget fields, other registers) */
static const unsigned char u2_list_100_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50,0x49,0x89,0xf6,0x49,0x89,0xfc,
                                                0x41,0x80,0xbc,0x24,0x30,0x02,0x00,0x00,0x00};
static const unsigned char u2_list_102_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18,0x49,0x89,0xf7,0x49,0x89,0xfe,
                                                0x41,0x80,0xbe,0x30,0x02,0x00,0x00,0x00};
CB(u2_list_cb) { (void)rsi; (void)r14; (void)regs; if (u2m) u2_list_update(rdi,base); }
static const RuntimeGuestCallback u2_sites[]={
    SITE("U2 1.02 menu text (menu mouse)",0x5b56d0,u2_text_102_code,6,u3_text_cb),
    SITE("U2 1.02 list control update (menu mouse)",0x57e410,u2_list_102_code,8,u2_list_cb),
    SITE("U2 1.00 menu text (menu mouse)",0x28ddf0,u2_text_100_code,6,u3_text_cb),
    SITE("U2 1.00 list control update (menu mouse)",0x267f60,u2_list_100_code,8,u2_list_cb),
};
/* ---- antikmewzik's other menu changes (his runtime_pad.c; 1.02 his addresses, 1.00 located 2026-11-10 by the same code) ---- */
/* Statistics, "compare with friends" (text 0x9a8d0b3e): the online comparison does not work here. The prompt bar draws that prompt by
 * kind (U1 kind 4, U2 kind 7, U3 kind 0x80): U1 and U2 note the moment their branch runs (the hook on its first test) and the branch's
 * jump goes past the drawing; U3's kind test compares with -1 (never taken). While it is up, the cross (Enter, a click) does nothing
 * (U1 always; U2 except on the rows that open a sub menu; U3 on a line with its value on the right). The SELECT prompt of the same bar
 * goes on a statistic (U1, U3: bit 0 of the bar's flags cleared). */
static volatile uint64_t u1_compare_us, u2_compare_us, u3_hide_select_us;
static volatile int u2_sel_plain_row, u3_sel_value;
CB(u1_compare_cb) { (void)rdi; (void)rsi; (void)base; (void)r14; (void)regs; if (u1m) u1_compare_us=now_us(); }
/* (the hook moves "mov r15d, r9d; mov [rbp-0x258], r15d": the flags are still in r9, regs[3], restored by the thunk) */
CB(u1_bar_flags_cb) { (void)rdi; (void)rsi; (void)base; (void)r14; if (u1m && now_us()-u1_hide_select_us<150000) ((uint64_t *)(uintptr_t)regs)[3]&=~(uint64_t)1; }
CB(u2_compare_cb) { (void)rdi; (void)rsi; (void)base; (void)r14; (void)regs; if (u2m) u2_compare_us=now_us(); }
/* U3 1.02: at "mov edx, r9d; and edx, 1" (r9 = regs[3]). 1.00 keeps the flags in [rbp-0x2b0] and reloads them into edi there
 * ("mov edi, [rbp-0x2b0]" is the moved instruction, run after this): the frame is rbp = rsp+0x310 (push rbp, five pushes, sub rsp
 * 0x2e8; no other change of rsp before), so the dword is at the guest's rsp+0x60; the guest's rsp is just above the saved registers
 * and the 128-byte red-zone step (regs+25). The later reloads (0x1c7066, 0x1c7172) see the cleared bit as well, as 1.02's spill. */
CB(u3_bar_flags_102_cb) { (void)rdi; (void)rsi; (void)base; (void)r14; if (u3m && now_us()-u3_hide_select_us<150000) ((uint64_t *)(uintptr_t)regs)[3]&=~(uint64_t)1; }
CB(u3_bar_flags_100_cb) {
    (void)rdi; (void)rsi; (void)base; (void)r14;
    if (!u3m || now_us()-u3_hide_select_us>=150000) return;
    uint32_t *flags=(uint32_t *)(uintptr_t)((uint64_t)(uintptr_t)(regs+25)+0x60);
    *flags&=~1u;
}
static const unsigned char u1_compare_code[]={0x41,0x83,0x7d,0x48,0x00,0x0f,0x84,0x34,0x02,0x00,0x00,0x4c,0x8d,0x3d};   /* cmp dword [r13+0x48],0; je (both codes) */
static const unsigned char u1_compare_je_code[]={0x0f,0x84,0x34,0x02,0x00,0x00,0x4c,0x8d,0x3d};
static const unsigned char u1_compare_je_patch[]={0x0f,0x84,0xf8,0x02,0x00,0x00};   /* past the prompt (1.02 0x129267, 1.00 0x129117) */
static const unsigned char u1_compare_jmp_code[]={0xe9,0x2f,0x02,0x00,0x00,0x4c,0x8d,0x35};
static const unsigned char u1_compare_jmp_patch[]={0xe9,0xec,0x02,0x00,0x00};
static const unsigned char u1_bar_flags_code[]={0x45,0x89,0xcf,0x44,0x89,0xbd,0xa8,0xfd,0xff,0xff,0x4c,0x89,0x85,0xa0,0xfd,0xff,0xff};   /* mov r15d, r9d; mov [rbp-0x258], r15d */
static const unsigned char u2_compare_102_code[]={0x41,0x83,0x7f,0x48,0x00,0x0f,0x84,0x8b,0x04,0x00,0x00,0x4d,0x89,0xfd};   /* cmp dword [r15+0x48],0; jz 0x5a595d */
static const unsigned char u2_compare_jz_102_code[]={0x0f,0x84,0x8b,0x04,0x00,0x00,0x4d,0x89,0xfd};
static const unsigned char u2_compare_jz_102_patch[]={0xe9,0x11,0x06,0x00,0x00,0x90};   /* jmp 0x5a5ae2, the next prompt (where kind != 7 goes) */
static const unsigned char u2_compare_100_code[]={0x41,0x83,0x7d,0x48,0x00,0x0f,0x84,0xb4,0x02,0x00,0x00,0x4c,0x8d,0x3d};   /* cmp dword [r13+0x48],0; jz 0x2822c0 */
static const unsigned char u2_compare_jz_100_code[]={0x0f,0x84,0xb4,0x02,0x00,0x00,0x4c,0x8d,0x3d};
static const unsigned char u2_compare_jz_100_patch[]={0xe9,0xac,0x03,0x00,0x00,0x90};   /* jmp 0x2823b7, the next prompt (where kind != 7 goes) */
static const unsigned char u3_compare_102_code[]={0x41,0x81,0xf9,0x80,0x00,0x00,0x00,0x0f,0x85,0x71,0x07,0x00,0x00};   /* cmp r9d,0x80; jne 0x6284e3 */
static const unsigned char u3_compare_102_patch[]={0x41,0x81,0xf9,0xff,0xff,0xff,0xff};
static const unsigned char u3_compare_100_code[]={0x81,0xff,0x80,0x00,0x00,0x00,0x0f,0x85,0x63,0x03,0x00,0x00};   /* cmp edi,0x80; jne 0x1c7500 */
static const unsigned char u3_compare_100_patch[]={0x81,0xff,0xff,0xff,0xff,0xff};
static const unsigned char u3_bar_flags_102_code[]={0x44,0x89,0xca,0x83,0xe2,0x01,0x0f,0x94,0xc3,0x44,0x89,0xc9};   /* mov edx, r9d; and edx, 1; sete bl */
static const unsigned char u3_bar_flags_100_code[]={0x8b,0xbd,0x50,0xfd,0xff,0xff,0xc4,0xe3,0x79,0x21,0x05};   /* mov edi, [rbp-0x2b0]; vinsertps */
static const unsigned char u3_bar_100_frame[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xe8,0x02,0x00,0x00};   /* 0x1c6c30: the frame the 1.00 hook relies on */
#define PATCH(name,at,sig,p) {name,at,sig,sizeof(sig),sizeof(p),NULL,p}
/* U1 Options > Game: the photo mode row (text 0xa3da9f13) is made by three blocks of the options builder (one per menu that holds it);
 * each ends with the add-to-menu call (1.02 0x132f00, 1.00 0x132d70), here made a 5-byte nop: the row is built and never added. */
static const unsigned char u1_photo_102_1_code[]={0x4c,0x89,0xf7,0x4c,0x89,0xee,0xe8,0xba,0xe0,0xfe,0xff};   /* mov rdi,r14; mov rsi,r13; call 0x132f00 */
static const unsigned char u1_photo_102_1_patch[]={0x4c,0x89,0xf7,0x4c,0x89,0xee,0x0f,0x1f,0x44,0x00,0x00};
static const unsigned char u1_photo_102_2_code[]={0x48,0x8b,0xbd,0x10,0xff,0xff,0xff,0x48,0x89,0xde,0xe8,0x1d,0xde,0xfe,0xff};
static const unsigned char u1_photo_102_2_patch[]={0x48,0x8b,0xbd,0x10,0xff,0xff,0xff,0x48,0x89,0xde,0x0f,0x1f,0x44,0x00,0x00};
static const unsigned char u1_photo_102_3_code[]={0x48,0x8b,0xbd,0x08,0xff,0xff,0xff,0x48,0x89,0xde,0xe8,0x06,0xdc,0xfe,0xff};
static const unsigned char u1_photo_102_3_patch[]={0x48,0x8b,0xbd,0x08,0xff,0xff,0xff,0x48,0x89,0xde,0x0f,0x1f,0x44,0x00,0x00};
static const unsigned char u1_photo_100_1_code[]={0x4c,0x89,0xff,0x4c,0x89,0xee,0xe8,0x2f,0xe3,0xfe,0xff};   /* mov rdi,r15; mov rsi,r13; call 0x132d70 */
static const unsigned char u1_photo_100_1_patch[]={0x4c,0x89,0xff,0x4c,0x89,0xee,0x0f,0x1f,0x44,0x00,0x00};
static const unsigned char u1_photo_100_2_code[]={0x48,0x8b,0xbd,0x18,0xff,0xff,0xff,0x48,0x89,0xde,0xe8,0x92,0xe0,0xfe,0xff};
static const unsigned char u1_photo_100_2_patch[]={0x48,0x8b,0xbd,0x18,0xff,0xff,0xff,0x48,0x89,0xde,0x0f,0x1f,0x44,0x00,0x00};
static const unsigned char u1_photo_100_3_code[]={0x48,0x8b,0xbd,0x10,0xff,0xff,0xff,0x48,0x89,0xde,0xe8,0x7b,0xde,0xfe,0xff};
static const unsigned char u1_photo_100_3_patch[]={0x48,0x8b,0xbd,0x10,0xff,0xff,0xff,0x48,0x89,0xde,0x0f,0x1f,0x44,0x00,0x00};
/* the cross held back on a statistic (the pad read, every input; see runtime_menumouse_filter) */
static int u3_stat_selected(uint64_t now) {
    U3Row sel={0}; int have=0;
    pthread_mutex_lock(&u3_lock);
    for (int i=0;i<U3_ROWS;++i) { const U3Row *r=&u3_rows[i]; if (r->us && now-r->us<100000 && r->sel>=0.5f && r->h>=24.0f && r->h<=42.0f && (!have || r->sel>sel.sel+0.01f)) { sel=*r; have=1; } }
    int value=0;
    /* a statistics line (28.6 tall) has its value in a second text to its right; the Rewards categories (labels at x 292..347) can be chosen */
    if (have && sel.h>=26.0f && sel.h<=30.0f && (sel.x<288.0f || sel.x>350.0f))
        for (int i=0;i<U3_ROWS && !value;++i) { const U3Row *r=&u3_rows[i]; if (r->us && now-r->us<100000 && r->w>=5.0f && fabsf(r->y-sel.y)<2.0f && r->x>=sel.x+sel.w-1.0f) value=1; }
    /* U2: the selected row is a 768-wide row of a list (no value column: it opens a sub menu) */
    u2_sel_plain_row=have && fabsf(sel.h-29.2f)<0.3f && fabsf(sel.x-270.0f)<1.0f && fabsf(sel.w-768.0f)<1.5f;
    pthread_mutex_unlock(&u3_lock);
    return value;
}
uint32_t runtime_menumouse_filter(uint32_t buttons, int keyboard) {
    const uint64_t now=now_us();
    if (u1m && menu_mode && u1_compare_us && now-u1_compare_us<250000) buttons&=~(uint32_t)BTN_CROSS;
    if (u3m || u2m) {
        const int stat=u3_stat_selected(now);
        if (u3m) { u3_sel_value=stat; if (stat) u3_hide_select_us=now; }
        if (keyboard && u2m && u2_compare_us && now-u2_compare_us<300000 && !u2_sel_plain_row) buttons&=~(uint32_t)BTN_CROSS;
        if (keyboard && u3m && u3_sel_value) buttons&=~(uint32_t)BTN_CROSS;
    }
    return buttons;
}

/* Treasures model (see model_us): the speeds written after the game's clamp are not limited, so the model follows the mouse one to
 * one; after the release they decay (BB_MODEL_COAST seconds, default 0.5; 0: stop at once). dt: the game's frame time global. */
static void model_turn(uint64_t image, uint64_t scene, uint64_t dt_at) {
    model_us=now_us();
    static float sx, sy;
    static float coast=-1.0f;
    if (coast<0.0f) { const char *e=getenv("BB_MODEL_COAST"); coast=e && atof(e)>=0.0 ? (float)atof(e) : 0.5f; }
    if (scene<0x10000) return;
    float dt=0.0f;
    if (!safe_read(&dt,image+dt_at,4) || !(dt>0.0005f) || dt>0.2f) dt=1.0f/30.0f;
    float vx, vy;
    if (model_drag) {
        const float ax=model_ax, ay=model_ay;
        model_ax-=ax; model_ay-=ay;
        vx=ax/dt; vy=-ay/dt;
        vx=vx>60.0f ? 60.0f : vx<-60.0f ? -60.0f : vx;   /* (a sanity limit only) */
        vy=vy>60.0f ? 60.0f : vy<-60.0f ? -60.0f : vy;
        const float a=1.0f-expf(-dt/0.06f);   /* the speed of the last 60 ms of the gesture, for the coast */
        sx+=a*(vx-sx); sy+=a*(vy-sy);
    } else {
        model_ax=model_ay=0.0f;
        if (coast<=0.0f || fabsf(sx)+fabsf(sy)<0.05f) { sx=sy=0.0f; return; }
        const float k=expf(-dt/coast);
        sx*=k; sy*=k; vx=sx; vy=sy;
    }
    const float v[2]={vx,vy};
    safe_write(scene+0x70,v,8);
}
typedef uint64_t (__attribute__((sysv_abi)) *guest_fn3)(uint64_t,uint64_t,uint64_t);
typedef uint64_t (__attribute__((sysv_abi)) *guest_fn6)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
static const unsigned char u3_notify_102_code[]={0x55,0x48,0x89,0xe5,0x53,0x50,0x48,0x85,0xf6,0x74,0x17,0x80,0x3e,0x00,0x74,0x12};
static const unsigned char u3_notify_100_code[]={0x48,0x85,0xf6,0x74,0x11,0x80,0x3e,0x00,0x74,0x0c,0x48,0x8b,0x7f,0x60,0x0f,0xb6};
static void u3_model(uint64_t image, uint64_t notify, const unsigned char *notify_code, size_t notify_n, uint64_t dt_at) {
    if (u3_note_pending) {   /* the hover's new selection: the scene is told which model to show (the item's text, queued) */
        u3_note_pending=0;
        if (code_is(notify,notify_code,notify_n)) {
            const uint64_t str=((guest_fn3)(uintptr_t)u3_note_fn)(u3_note_ds,(uint64_t)(uint32_t)u3_note_value,7);
            if (str) ((guest_fn6)(uintptr_t)(image+notify))(u3_note_queue,str,1,0,0xffffffffu,0xffffffffu);
            if (mouse_debug()) printf("Menu mouse: U3 Treasures scene told: item %d\n",(int)u3_note_value);
        }
    }
    model_turn(image,u3_scene,dt_at);
}
CB(u2_model_102_cb) { (void)rdi; (void)rsi; (void)regs; if (u2m) model_turn(base,r14,0x111628cu); }   /* (r14 is the scene) */
CB(u2_model_100_cb) { (void)rdi; (void)rsi; (void)regs; if (u2m) model_turn(base,r14,0xebd5ccu); }
CB(u3_scene_cb) { (void)rsi; (void)base; (void)r14; (void)regs; u3_scene=rdi; }
CB(u3_model_102_cb) { (void)rdi; (void)rsi; (void)r14; (void)regs; if (u3m) u3_model(base,0x63c5c0,u3_notify_102_code,sizeof(u3_notify_102_code),0x17b4bacu); }
CB(u3_model_100_cb) { (void)rdi; (void)rsi; (void)r14; (void)regs; if (u3m) u3_model(base,0x1d4fd0,u3_notify_100_code,sizeof(u3_notify_100_code),0x142ffc4u); }
/* after the second clamp: vmovss xmm0, [rbp-x] (the dt kept by the update); vmulss xmm0, xmm0, [scene+0x70] */
static const unsigned char u2_model_102_code[]={0xc5,0xfa,0x10,0x85,0x70,0xfd,0xff,0xff,0xc4,0xc1,0x7a,0x59,0x46,0x70,0xc5,0xfa};
static const unsigned char u2_model_100_code[]={0xc5,0xfa,0x10,0x85,0x10,0xfe,0xff,0xff,0xc4,0xc1,0x7a,0x59,0x46,0x70,0xc5,0xfa};
static const unsigned char u3_model_102_code[]={0xc5,0xfa,0x10,0x85,0x80,0xfc,0xff,0xff,0xc4,0xc1,0x7a,0x59,0x47,0x70,0xc5,0xfa};
static const unsigned char u3_model_100_code[]={0xc5,0xfa,0x10,0x85,0xcc,0xfd,0xff,0xff,0xc4,0xc1,0x7a,0x59,0x47,0x70,0xc5,0xfa};
/* the U3 scene update's start (push rbp; mov rbp, rsp; push r15 moved; then r15 = rdi) */
static const unsigned char u3_scene_102_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x98,0x03,0x00,0x00,0x49,0x89,0xf6,0x49,0x89,0xff};
static const unsigned char u3_scene_100_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x48,0x02,0x00,0x00,0x48,0x89,0xf3,0x49,0x89,0xff};

/* U3 Chapter select's carousel draw (see u3_car): the chapter set by a pending request, the control noted */
CB(u3_carousel_cb) {
    (void)rsi; (void)r14; (void)regs;
    if (!u3m || rdi<0x10000) return;
    uint64_t ds=0, vt=0, f0=0, f8=0, fmin=0, fmax=0; float rc[4]={0,0,0,0};
    if (!safe_read(rc,rdi+0x90,16) || !safe_read(&ds,rdi+0x18,8) || ds<0x10000 || !safe_read(&vt,ds,8) || vt<0x10000) return;
    if (!safe_read(&f0,vt,8) || !safe_read(&f8,vt+8,8) || !safe_read(&fmin,vt+0x38,8) || !safe_read(&fmax,vt+0x40,8)) return;
    const uint64_t lo_code=base, hi_code=base+0x2000000;
    if (f0<lo_code || f0>=hi_code || f8<lo_code || f8>=hi_code || fmin<lo_code || fmin>=hi_code || fmax<lo_code || fmax>=hi_code) return;
    int cur=(int)(int32_t)((guest_vcall_fn)(uintptr_t)f0)(ds);
    const int lo=(int)(int32_t)((guest_vcall_fn)(uintptr_t)fmin)(ds), hi=(int)(int32_t)((guest_vcall_fn)(uintptr_t)fmax)(ds);
    if (u2_set_pending && u2_set_ds==ds) {
        int v=u2_set_val; if (v<lo) v=lo; if (v>hi) v=hi;
        ((guest_set_fn)(uintptr_t)f8)(ds,v);
        u2_set_pending=0;
        if (mouse_debug()) printf("Menu mouse: U3 carousel set(%d) (was %d, range %d..%d)\n",v,cur,lo,hi);
        cur=v;
    }
    if (mouse_debug() && (u3_car.w!=rdi || now_us()-u3_car.us>1000000)) printf("Menu mouse: U3 carousel %#llx: chapter %d of %d..%d, rect %.0f %.0f %.0fx%.0f\n",(unsigned long long)rdi,cur,lo,hi,rc[0],rc[1],rc[2],rc[3]);
    u3_car.w=rdi; u3_car.ds=ds; u3_car.cur=cur; u3_car.lo=lo; u3_car.hi=hi; memcpy(u3_car.rc,rc,sizeof(rc)); u3_car.us=now_us();
}
static const unsigned char u3_carousel_102_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xa8,0x07,0x00,0x00,0x49,0x89,0xd7,0x49,0x89,0xf4,0x48,0x89,0xf9};
static const unsigned char u3_carousel_100_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x18,0x0a,0x00,0x00,0x49,0x89,0xd4,0x49,0x89,0xf7,0x49,0x89,0xfd};

/* U3 Yes/No choices draw (see u3_dlg): a pending choice set, the control noted */
CB(u3_dialog_cb) {
    (void)rsi; (void)r14; (void)regs;
    if (!u3m || rdi<0x10000) return;
    uint64_t ds=0, vt=0, f0=0, f8=0, fmin=0, fmax=0; float rc[4]={0,0,0,0};
    if (!safe_read(&ds,rdi+0x18,8) || ds<0x10000 || !safe_read(&vt,ds,8) || vt<0x10000) return;
    if (!safe_read(&f0,vt,8) || !safe_read(&f8,vt+8,8) || !safe_read(&fmin,vt+0x38,8) || !safe_read(&fmax,vt+0x40,8)) return;
    const uint64_t lo_code=base, hi_code=base+0x2000000;
    if (f0<lo_code || f0>=hi_code || f8<lo_code || f8>=hi_code || fmin<lo_code || fmin>=hi_code || fmax<lo_code || fmax>=hi_code) return;
    int cur=(int)(int32_t)((guest_vcall_fn)(uintptr_t)f0)(ds);
    const int lo=(int)(int32_t)((guest_vcall_fn)(uintptr_t)fmin)(ds), hi=(int)(int32_t)((guest_vcall_fn)(uintptr_t)fmax)(ds);
    if (u2_set_pending && u2_set_ds==ds) {
        int v=u2_set_val; if (v<lo) v=lo; if (v>hi) v=hi;
        ((guest_set_fn)(uintptr_t)f8)(ds,v);
        u2_set_pending=0;
        if (mouse_debug()) printf("Menu mouse: U3 Yes/No set(%d) (was %d, range %d..%d)\n",v,cur,lo,hi);
        cur=v;
    }
    safe_read(rc,rdi+0x90,16);
    if (mouse_debug() && (u3_dlg.w!=rdi || now_us()-u3_dlg.us>1000000)) printf("Menu mouse: U3 Yes/No control %#llx: choice %d of %d..%d, rect %.0f %.0f %.0fx%.0f\n",(unsigned long long)rdi,cur,lo,hi,rc[0],rc[1],rc[2],rc[3]);
    u3_dlg.w=rdi; u3_dlg.ds=ds; u3_dlg.cur=cur; u3_dlg.lo=lo; u3_dlg.hi=hi; memcpy(u3_dlg.rc,rc,sizeof(rc)); u3_dlg.us=now_us();
}
static const unsigned char u3_dialog_102_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xf8,0x04,0x00,0x00,0x48,0x89,0x95,0x38,0xfb,0xff,0xff,0x49,0x89,0xf4};
static const unsigned char u3_dialog_100_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x68,0x05,0x00,0x00,0x48,0x89,0xd3,0x48,0x89,0x9d,0xc8,0xfa,0xff,0xff};

/* U2's text lookup (table, text id): "Reset camera", "Photo mode", "Swap shoulder when aiming" are only on the controller picture */
CB(u2_text_lookup_cb) {
    (void)rdi; (void)base; (void)r14; (void)regs;
    const uint32_t k=(uint32_t)rsi;
    if (u2m && (k==0x4158a30du || k==0x9ef63173u || k==0x36fc7ed5u) && !runtime_mouselook_camera_recent()) u2_layout_us=now_us();   /* (not a HUD hint in play) */
}
static const unsigned char u2_text_lookup_code[]={0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x89,0xf3,0x49,0x89,0xfe,0x49,0x8b,0x7e,0x58,0x48,0x85,0xff,0x74};   /* (both codes) */

#define U1_SITES_102 \
    SITE("U1 1.02 compare-with-friends prompt (hook) (menu mouse)",0x128f64,u1_compare_code,5,u1_compare_cb), \
    PATCH("U1 1.02 compare-with-friends prompt (skip 1) (menu mouse)",0x128f69,u1_compare_je_code,u1_compare_je_patch), \
    PATCH("U1 1.02 compare-with-friends prompt (skip 2) (menu mouse)",0x128f76,u1_compare_jmp_code,u1_compare_jmp_patch), \
    SITE("U1 1.02 prompt bar flags (menu mouse)",0x128c8a,u1_bar_flags_code,10,u1_bar_flags_cb),     PATCH("U1 1.02 options without the photo mode row (1) (menu mouse)",0x144e3b,u1_photo_102_1_code,u1_photo_102_1_patch),     PATCH("U1 1.02 options without the photo mode row (2) (menu mouse)",0x1450d4,u1_photo_102_2_code,u1_photo_102_2_patch),     PATCH("U1 1.02 options without the photo mode row (3) (menu mouse)",0x1452eb,u1_photo_102_3_code,u1_photo_102_3_patch)
#define U1_SITES_100 \
    SITE("U1 1.00 compare-with-friends prompt (hook) (menu mouse)",0x128e14,u1_compare_code,5,u1_compare_cb), \
    PATCH("U1 1.00 compare-with-friends prompt (skip 1) (menu mouse)",0x128e19,u1_compare_je_code,u1_compare_je_patch), \
    PATCH("U1 1.00 compare-with-friends prompt (skip 2) (menu mouse)",0x128e26,u1_compare_jmp_code,u1_compare_jmp_patch), \
    SITE("U1 1.00 prompt bar flags (menu mouse)",0x128b3a,u1_bar_flags_code,10,u1_bar_flags_cb),     PATCH("U1 1.00 options without the photo mode row (1) (menu mouse)",0x144a36,u1_photo_100_1_code,u1_photo_100_1_patch),     PATCH("U1 1.00 options without the photo mode row (2) (menu mouse)",0x144ccf,u1_photo_100_2_code,u1_photo_100_2_patch),     PATCH("U1 1.00 options without the photo mode row (3) (menu mouse)",0x144ee6,u1_photo_100_3_code,u1_photo_100_3_patch)
#define U2_SITES_102 \
    SITE("U2 1.02 compare-with-friends prompt (hook) (menu mouse)",0x5a54c7,u2_compare_102_code,5,u2_compare_cb), \
    PATCH("U2 1.02 compare-with-friends prompt (skip) (menu mouse)",0x5a54cc,u2_compare_jz_102_code,u2_compare_jz_102_patch), \
    SITE("U2 1.02 Treasures model (menu mouse)",0x5d7c05,u2_model_102_code,8,u2_model_102_cb), \
    SITE("U2 1.02 text lookup (controller picture) (menu mouse)",0xcddc00,u2_text_lookup_code,6,u2_text_lookup_cb)
#define U2_SITES_100 \
    SITE("U2 1.00 compare-with-friends prompt (hook) (menu mouse)",0x282001,u2_compare_100_code,5,u2_compare_cb), \
    PATCH("U2 1.00 compare-with-friends prompt (skip) (menu mouse)",0x282006,u2_compare_jz_100_code,u2_compare_jz_100_patch), \
    SITE("U2 1.00 Treasures model (menu mouse)",0x2ad8ea,u2_model_100_code,8,u2_model_100_cb), \
    SITE("U2 1.00 text lookup (controller picture) (menu mouse)",0x633460,u2_text_lookup_code,6,u2_text_lookup_cb)
#define U3_SITES_102 \
    PATCH("U3 1.02 compare-with-friends prompt (skip) (menu mouse)",0x627d65,u3_compare_102_code,u3_compare_102_patch), \
    SITE("U3 1.02 prompt bar flags (menu mouse)",0x6277e8,u3_bar_flags_102_code,6,u3_bar_flags_102_cb), \
    SITE("U3 1.02 Treasures scene (menu mouse)",0x65f010,u3_scene_102_code,6,u3_scene_cb), \
    SITE("U3 1.02 Treasures model (menu mouse)",0x65f6b0,u3_model_102_code,8,u3_model_102_cb), \
    SITE("U3 1.02 chapter carousel draw (menu mouse)",0x5eccb0,u3_carousel_102_code,8,u3_carousel_cb), \
    SITE("U3 1.02 Yes/No choices draw (menu mouse)",0x5e0de0,u3_dialog_102_code,8,u3_dialog_cb)
#define U3_SITES_100 \
    PATCH("U3 1.00 compare-with-friends prompt (skip) (menu mouse)",0x1c7191,u3_compare_100_code,u3_compare_100_patch), \
    SITE("U3 1.00 prompt bar flags (menu mouse)",0x1c6ecb,u3_bar_flags_100_code,6,u3_bar_flags_100_cb), \
    SITE("U3 1.00 Treasures scene (menu mouse)",0x1f6e10,u3_scene_100_code,6,u3_scene_cb), \
    SITE("U3 1.00 Treasures model (menu mouse)",0x1f74f8,u3_model_100_code,8,u3_model_100_cb), \
    SITE("U3 1.00 chapter carousel draw (menu mouse)",0x1a0490,u3_carousel_100_code,8,u3_carousel_cb), \
    SITE("U3 1.00 Yes/No choices draw (menu mouse)",0x197fc0,u3_dialog_100_code,8,u3_dialog_cb)

static const RuntimeGuestCallback u1_more_102[]={U1_SITES_102}, u1_more_100[]={U1_SITES_100};
static const RuntimeGuestCallback u2_more_102[]={U2_SITES_102}, u2_more_100[]={U2_SITES_100};
static const RuntimeGuestCallback u3_more_102[]={U3_SITES_102}, u3_more_100[]={U3_SITES_100};
/* the sites of the code found: the menu mouse's, then these (the 1.00 U3 flags hook only on its known frame) */
static RuntimeGuestCallback mm_sites[64];
const RuntimeGuestCallback *runtime_menumouse_callbacks(size_t *count) {
    const RuntimeGuestCallback *a=NULL, *b=NULL;
    size_t na=0, nb=0;
#define TAKE(x,n,y) do { a=(x); na=(n); b=(y); nb=sizeof(y)/sizeof(*(y)); } while (0)
    if (u1m) { const size_t n=sizeof(u1_sites)/sizeof(*u1_sites)/2; if (u1m==&u1_menu_codes[0]) TAKE(u1_sites,n,u1_more_102); else TAKE(u1_sites+n,n,u1_more_100); }
    if (u3m) { if (u3m==&u3_menu_codes[0]) TAKE(u3_sites,4,u3_more_102); else TAKE(u3_sites+4,4,u3_more_100); }
    if (u2m) { if (u2m==&u2_menu_codes[0]) TAKE(u2_sites,2,u2_more_102); else TAKE(u2_sites+2,2,u2_more_100); }
#undef TAKE
    size_t n=0;
    for (size_t i=0;i<na && n<64;++i) mm_sites[n++]=a[i];
    for (size_t i=0;i<nb && n<64;++i) {
        if (b[i].callback==u3_bar_flags_100_cb && !code_is(0x1c6c30,u3_bar_100_frame,sizeof(u3_bar_100_frame))) {
            puts("Menu mouse: U3 1.00 prompt bar frame differs: its SELECT stays");
            continue;
        }
        mm_sites[n++]=b[i];
    }
    *count=n;
    return mm_sites;
}

/* A menu is open. U1: the pause (the game's frame time is 0), the credits, or menu rows drawn lately (title, main menu, game
 * selection); in play no menu row is drawn (hints are free texts). U3: a focused menu page, or a lit menu row. */
int runtime_menumouse_active(void) {
    if (!u1m && !u3m && !u2m) return 0;
    const uint64_t now=now_us();
    int open=0;
    if (u1m) {
        int rows=0;
        pthread_mutex_lock(&menu_text_lock);
        for (int i=0;i<menu_text_count && !rows;++i) if (now-menu_texts[i].us<300000) rows=1;
        pthread_mutex_unlock(&menu_text_lock);
        open=camera_stopped() || u1_credits_recent(now) || rows;
    } else open=u3_menu_open(now);
    if (open!=menu_mode) {
        menu_mode=open;
        printf("Menu mouse: %s\n",open ? "menu on screen: the cursor is free" : "menu gone");
    }
    return open;
}

uint32_t runtime_menumouse_buttons(const bool *k, uint32_t mouse, int wheel, int scripted, float sx, float sy) {
    const uint64_t now=now_us();
    scripted_cursor=scripted; scripted_ux=sx; scripted_uy=sy;
    if (mouse_debug()) { static uint32_t last=~0u; if (mouse!=last) { last=mouse; printf("Menu mouse: buttons %#x\n",mouse); } }
    if (wheel>0) wheel_until_us[0]=now+80000;
    if (wheel<0) wheel_until_us[1]=now+80000;
    uint32_t out=0;
    if (u3m || u2m) out=u3_menu_buttons(mouse,now);
    else {
        out=menu_mouse_buttons(k,mouse,now);
        /* the wheel moves the highlight (a carousel or a Yes/No: sideways), unless a list's scroll bar takes it */
        if (menu_carousel_recent(now)) {
            if (now<wheel_until_us[0]) out|=BTN_LEFT;
            if (now<wheel_until_us[1]) out|=BTN_RIGHT;
        } else if (!u1_rail_wheel) {
            if (now<wheel_until_us[0]) out|=BTN_UP;
            if (now<wheel_until_us[1]) out|=BTN_DOWN;
        }
    }
    /* the side button ("back") and the right button go back */
    if (mouse & (2u|8u)) out|=BTN_CIRCLE;
    bbgpu_set_cursor((mouse & 1u) ? 2 : menu_hover ? 1 : 0);
    return out;
}
