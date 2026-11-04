/* "Quit" in the games' main menus: one more item, last, that closes the game (like any PC game). Its label is the
 * games' own "Quit" text (text id 0xa0be106d, in every language the games ship), so it follows the game's language.
 *
 * Uncharted 1 (EU 1.00 and 1.02 code) builds its menu pages in code, each page with its own item list
 * (page+0xa0, count at page+0x98): the hook at the start of AddToPage notes the main menu's last item, Load Game
 * (id 0x18, label text 0x7c894f27), and at the next AddToPage call (the game goes on building its other pages)
 * appends a Quit item (AddItem with id 0x7e00, which no menu uses) to that page. The menu's item handler
 * (a switch on the item id) is hooked at its start: id 0x7e00 ends the game through runtime_quit, after a save
 * that is being written has finished. */
#include "runtime.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

#define QUIT_ID 0x7e00u
#define PAUSE_QUIT_ID 0x7e01u
#define QUIT_TEXT 0xa0be106du
#define LOAD_GAME_ID 0x18u
#define LOAD_GAME_TEXT 0x7c894f27u
#define MAIN_MENU_QUIT_ID 0x10u
#define MAIN_MENU_QUIT_TEXT 0xdc389381u
#define YES_TEXT 0x260a263u
#define NO_TEXT 0x75bc82du

/* The pause menu's Yes/No: the game takes one of two dialog records and pages in turn (a toggle in its data), fills
 * the record (title at +8, count 1 at +0x14, Yes text and id at +0x18, No text and id at +0x20) and opens the page
 * with GoToPage(host, page, 0). */
typedef struct {
    const char *name;
    uint64_t add_item, alloc, add_to_page, go_to_page, toggle;
} U1Build;
static const U1Build u1_builds[]={
    {"Uncharted 1 EU",0x129ed0,0x35dbb0,0x132d70,0x14a480,0xabbaf4},
    {"Uncharted 1 1.02",0x12a020,0x4542b0,0x132f00,0x14a940,0xa7f9f8},
};

typedef void *(__attribute__((sysv_abi)) *Alloc)(uint64_t size);
typedef void (__attribute__((sysv_abi)) *AddItem)(void *item, void *owner, uint32_t id, void *label, uint32_t flags);
typedef void (__attribute__((sysv_abi)) *AddToPage)(void *page, void *item);
typedef void (__attribute__((sysv_abi)) *GoToPage)(void *host, void *page, uint32_t flags);

/* BB_EXIT_TEXT=<hex text id> (tests): another label text for the Quit items. */
static uint32_t quit_text(void) {
    static uint32_t text;
    if (!text) {
        const char *e=getenv("BB_EXIT_TEXT");
        unsigned v=0;
        text=e && sscanf(e,"%x",&v)==1 && v ? v : QUIT_TEXT;
        if (text!=QUIT_TEXT) printf("Exit menu: label text %08x (BB_EXIT_TEXT)\n",text);
    }
    return text;
}

/* Uncharted 1 builds each menu page in code. The item a Quit goes after (main menu: Load Game; pause menu: Quit to
 * Main Menu) is noted when the game adds it; at the next AddToPage call for another page (the page is complete)
 * a Quit item is appended, its label a copy of that item's with the "Quit" text. */
typedef struct {
    uint32_t after_id, after_text, quit_id;
    const char *where;
    unsigned char *page, *owner, label[0x18];
    int added;
} U1Spot;
static U1Spot u1_spots[]={
    {LOAD_GAME_ID,LOAD_GAME_TEXT,QUIT_ID,"main menu",NULL,NULL,{0},0},
    {MAIN_MENU_QUIT_ID,MAIN_MENU_QUIT_TEXT,PAUSE_QUIT_ID,"pause menu",NULL,NULL,{0},0},
};
static int adding;

static void u1_add_to_page(const U1Build *b, uint64_t image, unsigned char *page, unsigned char *item) {
    if (adding) return;  /* our own AddToPage call below */
    for (size_t s=0;s<sizeof(u1_spots)/sizeof(*u1_spots);++s) {
        U1Spot *spot=&u1_spots[s];
        if (!spot->page || spot->added || page==spot->page) continue;
        unsigned char *quit=((Alloc)(uintptr_t)(image+b->alloc))(0x108);
        unsigned char *label=((Alloc)(uintptr_t)(image+b->alloc))(0x18);
        if (quit && label) {
            memcpy(label,spot->label,0x18);
            const uint64_t text=quit_text();
            memcpy(label+8,&text,4);
            ((AddItem)(uintptr_t)(image+b->add_item))(quit,spot->owner,spot->quit_id,label,0);
            adding=1;
            ((AddToPage)(uintptr_t)(image+b->add_to_page))(spot->page,quit);
            adding=0;
            spot->added=1;
            int32_t count;
            memcpy(&count,spot->page+0x98,4);
            printf("Exit menu: %s: Quit added to the %s (%d items)\n",b->name,spot->where,count);
        }
        spot->page=NULL;
    }
    if (!item) return;
    uint32_t id, text;
    unsigned char *label;
    memcpy(&id,item+0xa8,4);
    memcpy(&label,item+8,sizeof(label));
    if (!label) return;
    memcpy(&text,label+8,4);
    for (size_t s=0;s<sizeof(u1_spots)/sizeof(*u1_spots);++s) {
        U1Spot *spot=&u1_spots[s];
        if (spot->added || spot->page || id!=spot->after_id || text!=spot->after_text) continue;
        spot->page=page;
        memcpy(&spot->owner,item+0xa0,sizeof(spot->owner));
        memcpy(spot->label,label,0x18);
    }
}

/* Ends the game from its menu: a save being written finishes first (at most 5 s). */
static void quit_from_menu(const char *game) {
    printf("Exit menu: %s: Quit picked, closing the game\n",game);
    fflush(stdout);
    for (int i=0;i<100 && runtime_savedata_mounted();++i) {
        struct timespec pause={0,50*1000*1000};
        nanosleep(&pause,NULL);
    }
    runtime_quit(0);
}

/* The pause menu's Quit: the game's Yes/No (title "Quit"), Yes sends the main menu Quit's id. */
static void u1_handler(const U1Build *b, uint64_t image, unsigned char *host, uint32_t id) {
    if (id==QUIT_ID) quit_from_menu("Uncharted 1");
    if (id!=PAUSE_QUIT_ID || !host) return;
    int32_t toggle;
    memcpy(&toggle,(void *)(uintptr_t)(image+b->toggle),4);
    toggle=(toggle+1)%2;  /* the game's own (x+1) mod 2 */
    memcpy((void *)(uintptr_t)(image+b->toggle),&toggle,4);
    unsigned char *dialog, *page;
    memcpy(&dialog,host+(toggle ? 0x8f0 : 0x8e8),sizeof(dialog));
    memcpy(&page,host+(toggle ? 0x448 : 0x440),sizeof(page));
    if (!dialog || !page) return;
    const uint32_t title=quit_text(), one=1, fields[4]={YES_TEXT,QUIT_ID,NO_TEXT,0};
    memcpy(dialog+8,&title,4);
    memcpy(dialog+0x14,&one,4);
    memcpy(dialog+0x18,fields,sizeof(fields));
    printf("Exit menu: %s: Quit picked on the pause menu, asking Yes/No\n",b->name);
    ((GoToPage)(uintptr_t)(image+b->go_to_page))(host,page,0);
}

void __attribute__((ms_abi)) runtime_exit_menu_u1_eu_add(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                         const uint64_t *regs) {
    (void)r14; (void)regs;
    u1_add_to_page(&u1_builds[0],image,(unsigned char *)(uintptr_t)rdi,(unsigned char *)(uintptr_t)rsi);
}
void __attribute__((ms_abi)) runtime_exit_menu_u1_us_add(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                         const uint64_t *regs) {
    (void)r14; (void)regs;
    u1_add_to_page(&u1_builds[1],image,(unsigned char *)(uintptr_t)rdi,(unsigned char *)(uintptr_t)rsi);
}
void __attribute__((ms_abi)) runtime_exit_menu_u1_eu_handler(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                             const uint64_t *regs) {
    (void)r14; (void)regs;
    u1_handler(&u1_builds[0],image,(unsigned char *)(uintptr_t)rdi,(uint32_t)rsi);
}
void __attribute__((ms_abi)) runtime_exit_menu_u1_us_handler(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                             const uint64_t *regs) {
    (void)r14; (void)regs;
    u1_handler(&u1_builds[1],image,(unsigned char *)(uintptr_t)rdi,(uint32_t)rsi);
}

/* Uncharted 2 and 3 load their menus from data (dc1/ps4/menu.bin): pages {.., u32 sid at +8, item header at +0x38
 * (U2) / +0x48 (U3)}, item header {u32 count; Item *items}, items {name, u32 id at +8, type at +0x10, label name at
 * +0x20, u32 label sid at +0x28, target page sid at +0x50 (0: the item goes to the menu handler)}. Right after the
 * game looks up the "menu" data (the hook is on the store of its pointer, rax), the main menu page (PAGE_PROFILE,
 * sid 0x82d5c00b) gets a new item list: its items and, last, a copy of LOADGAME (0xf7a5b490) as BUTTON_QUIT_GAME
 * (id 0x3a1a91cf, no target page) labelled with the games' "Quit" (0xa0be106d). The game then builds the item
 * itself like its own. The menu handler (host, id, ...) is hooked at its start: that id closes the game. */
#define DC_QUIT_ID 0x3a1a91cfu
#define DC_PAUSE_QUIT_ID 0x3a1a91d0u
#define DC_PROFILE_PAGE 0x82d5c00bu
#define DC_PAUSE_PAGE 0xf05cd578u
#define DC_LOADGAME_ID 0xf7a5b490u
#define DC_GAMEROOT_QUIT_ID 0xfdcd24dfu
#define DC_YESNO_PAGE 0x960d2b9du
#define DC_YES_TEXT 0x260a263u
#define DC_NO_TEXT 0x75bc82du
#define DC_NO_ID 0xfc5ba05du

/* The Yes/No dialog the pause menu's Quit to Main Menu opens: the host's dialog record (host+dialog) holds the title
 * text at +8 and, from `buttons` on, {u32 count (1); Yes text, Yes id, No text, No id; zeros}. */
typedef struct {
    const char *name;
    size_t page_stride, page_items, item_stride, dialog, buttons, zeros;
} DcLayout;
static const DcLayout dc_u2={"Uncharted 2",0x70,0x38,0x70,0x17c8,0x40,2}, dc_u3={"Uncharted 3",0x80,0x48,0x68,0x1430,0x10,3};

/* Appends a copy of the page's item `template_id` as a new item (name, id, label "Quit", target page). */
static void dc_add_item(const DcLayout *l, unsigned char *menu, uint32_t page_sid, uint32_t template_id,
                        const char *name, uint32_t id, uint32_t target, const char *where) {
    static const char label[]="BP_MENU_SPEEDRUN_QUIT";
    uint32_t pages;
    unsigned char *page_array;
    memcpy(&pages,menu,4);
    memcpy(&page_array,menu+8,sizeof(page_array));
    if (!page_array || pages>4096) return;
    for (uint32_t p=0;p<pages;++p) {
        unsigned char *page=page_array+(size_t)p*l->page_stride, *header;
        uint32_t sid;
        memcpy(&sid,page+8,4);
        if (sid!=page_sid) continue;
        memcpy(&header,page+l->page_items,sizeof(header));
        if (!header) return;
        uint32_t count;
        unsigned char *items;
        memcpy(&count,header,4);
        memcpy(&items,header+8,sizeof(items));
        if (!items || count==0 || count>256) return;
        const unsigned char *template=NULL;
        for (uint32_t i=0;i<count;++i) {
            uint32_t item_id;
            memcpy(&item_id,items+(size_t)i*l->item_stride+8,4);
            if (item_id==id) return;  /* already there (the data looked up again) */
            if (item_id==template_id) template=items+(size_t)i*l->item_stride;
        }
        if (!template) { printf("Exit menu: %s: no item to copy on the %s; no Quit\n",l->name,where); return; }
        /* Kept for the life of the process: the game keeps pointers to its item records. */
        unsigned char *copy=calloc((size_t)count+1,l->item_stride), *new_header=calloc(1,16);
        if (!copy || !new_header) { free(copy); free(new_header); return; }
        memcpy(copy,items,(size_t)count*l->item_stride);
        unsigned char *quit=copy+(size_t)count*l->item_stride;
        memcpy(quit,template,l->item_stride);
        const char *label_name=label;
        const uint32_t label_sid=quit_text();
        memcpy(quit,&name,sizeof(name));
        memcpy(quit+8,&id,4);
        memcpy(quit+0x20,&label_name,sizeof(label_name));
        memcpy(quit+0x28,&label_sid,4);
        memcpy(quit+0x50,&target,4);
        const uint32_t new_count=count+1;
        memcpy(new_header,&new_count,4);
        memcpy(new_header+8,&copy,sizeof(copy));
        memcpy(page+l->page_items,&new_header,sizeof(new_header));
        printf("Exit menu: %s: Quit added to the %s (%u items)\n",l->name,where,new_count);
        return;
    }
}

static void dc_add_quit(const DcLayout *l, unsigned char *menu) {
    if (!menu) return;
    /* Main menu: closes the game at once (nothing to lose there). */
    dc_add_item(l,menu,DC_PROFILE_PAGE,DC_LOADGAME_ID,"BUTTON_QUIT_GAME",DC_QUIT_ID,0,"main menu");
    /* Pause menu: opens the game's Yes/No page, set up by the handler hook below. */
    dc_add_item(l,menu,DC_PAUSE_PAGE,DC_GAMEROOT_QUIT_ID,"BUTTON_QUIT_DESKTOP",DC_PAUSE_QUIT_ID,DC_YESNO_PAGE,
                "pause menu");
}

/* The pause menu's Quit was picked: the handler goes on to open the Yes/No page (the item's target page); its
 * record gets the title "Quit", the game's Yes and No, and Yes sends the main menu's Quit id. */
static void dc_handler(const DcLayout *l, unsigned char *host, uint32_t id) {
    if (id==DC_QUIT_ID) quit_from_menu(l->name);
    if (id!=DC_PAUSE_QUIT_ID || !host) return;
    unsigned char *dialog;
    memcpy(&dialog,host+l->dialog,sizeof(dialog));
    if (!dialog) return;
    const uint32_t title=quit_text(), fields[5]={1,DC_YES_TEXT,DC_QUIT_ID,DC_NO_TEXT,DC_NO_ID}, zero=0;
    memcpy(dialog+8,&title,4);
    memcpy(dialog+l->buttons,fields,sizeof(fields));
    for (size_t z=0;z<l->zeros;++z) memcpy(dialog+l->buttons+sizeof(fields)+4*z,&zero,4);
    printf("Exit menu: %s: Quit picked on the pause menu, asking Yes/No\n",l->name);
}

/* regs: rbx r11 r10 r9 r8 rdx rcx rax rflags: the menu data's pointer is in rax (regs[7]). */
void __attribute__((ms_abi)) runtime_exit_menu_u2_data(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                       const uint64_t *regs) {
    (void)rdi; (void)rsi; (void)image; (void)r14;
    dc_add_quit(&dc_u2,(unsigned char *)(uintptr_t)regs[7]);
}
void __attribute__((ms_abi)) runtime_exit_menu_u3_data(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                       const uint64_t *regs) {
    (void)rdi; (void)rsi; (void)image; (void)r14;
    dc_add_quit(&dc_u3,(unsigned char *)(uintptr_t)regs[7]);
}
void __attribute__((ms_abi)) runtime_exit_menu_u2_handler(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                          const uint64_t *regs) {
    (void)image; (void)r14; (void)regs;
    dc_handler(&dc_u2,(unsigned char *)(uintptr_t)rdi,(uint32_t)rsi);
}
void __attribute__((ms_abi)) runtime_exit_menu_u3_handler(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14,
                                                          const uint64_t *regs) {
    (void)image; (void)r14; (void)regs;
    dc_handler(&dc_u3,(unsigned char *)(uintptr_t)rdi,(uint32_t)rsi);
}
