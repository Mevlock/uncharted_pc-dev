/* "Quit" in the games' main menus: one more item, last, that closes the game (like any PC game). Its label is the
 * games' own "Quit" text (text id 0xa0be106d, in every language the games ship), so it follows the game's language.
 *
 * Uncharted 1 (EU 1.00 and 1.02 code) builds its menu pages in code, each page with its own item list
 * (page+0xa0, count at page+0x98): the hook at the start of AddToPage notes the main menu's last item, Load Game
 * (id 0x18, label text 0x7c894f27), and at the next AddToPage call (the game goes on building its other pages)
 * appends a Quit item (AddItem with id 0x7e00, which no menu uses) to that page. The menu's item handler
 * (a switch on the item id) is hooked at its start: id 0x7e00 ends the game through runtime_quit, after a save
 * that is being written has finished.
 *
 * Also here: antikmewzik's "Keyboard / Mouse" row of Options > Controls (after the game's Controller Layout row), which opens
 * the overlay's Controls page (the keyboard and mouse bindings). U1: added by the same AddToPage hook, between the Controller
 * Layout row and Reset to Defaults; U2, U3: a copy of the Controller Layout record in the menu data, going to the menu handler.
 * Its label: runtime_menumouse.c (runtime_kbm_label). BB_KBM_ROW=0: no row. */
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
    uint64_t text_lookup, text_table;   /* the game's text lookup (table, id) -> string, and its text table (the row's label) */
} U1Build;
static const U1Build u1_builds[]={
    {"Uncharted 1 EU",0x129ed0,0x35dbb0,0x132d70,0x14a480,0xabbaf4,0x49cea0,0x99efc8},
    {"Uncharted 1 1.02",0x12a020,0x4542b0,0x132f00,0x14a940,0xa7f9f8,0x5caac0,0x9687d8},
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

/* The Keyboard / Mouse row (U1): the options builder adds the Controls page's Controller Layout row (id 0x412, label text
 * 0xdd40dc7b) and right after it Reset to Defaults (0x426, 0xb69a538a) to the same page, in 1.00 and 1.02 (checked in both
 * builders, 1.02 0x145876 / 1.00 0x145471). At that second AddToPage our row (id 0x7e02, which no menu uses: the item handler
 * gets it) is added first, its label a copy of the Controller Layout's with the text 0x1b124bb7 ("Left/Right Sticks", shown
 * nowhere), written over in the game's text bank (runtime_kbm_label; each time the page is built, so a language change keeps it).
 * A page holds 20 items (+0xa0, the page's own fields from +0x140): never more. */
#define U1_LAYOUT_ID 0x412u
#define U1_LAYOUT_TEXT 0xdd40dc7bu
#define U1_RESET_ID 0x426u
#define U1_RESET_TEXT 0xb69a538au
#define U1_KBM_ID 0x7e02u
#define U1_KBM_TEXT 0x1b124bb7u
static unsigned char *u1_kbm_page, u1_kbm_label[0x18];
static void *u1_kbm_owner;
typedef const char *(__attribute__((sysv_abi)) *TextLookup)(uint64_t table, uint64_t id);
static void u1_kbm_label_text(const U1Build *b, uint64_t image) {
    static const unsigned char lookup_code[]={0x55,0x48,0x89,0xe5,0x53,0x50,0x48,0x89,0xfb,0xe8,0x92,0xff,0xff,0xff,0x48,0x85};
    static int told;
    if (memcmp((const void *)(uintptr_t)(image+b->text_lookup),lookup_code,sizeof(lookup_code))) {
        if (!told++) printf("Exit menu: %s: the text lookup code differs: the Keyboard / Mouse row keeps the game's text\n",b->name);
        return;
    }
    const char *s=((TextLookup)(uintptr_t)(image+b->text_lookup))(image+b->text_table,U1_KBM_TEXT);
    const char *standard;   /* (the table's default text: the id is not in the bank) */
    memcpy(&standard,(const void *)(uintptr_t)(image+b->text_table+0x10),sizeof(standard));
    if (!s || s==standard) return;
    char old[96];
    const size_t len=strnlen(s,sizeof(old));
    if (len<4 || len>=sizeof(old)) return;
    memcpy(old,s,len+1);
    const char *label=runtime_kbm_label(old,len);
    if (!label) return;
    char padded[96];   /* the same length: the rest blank (antikmewzik's keys_label_here) */
    memset(padded,' ',len);
    memcpy(padded,label,strlen(label));
    if (memcmp(padded,old,len) && runtime_menumouse_write((uint64_t)(uintptr_t)s,padded,len)) {
        printf("Exit menu: %s: Keyboard / Mouse row label \"%s\" (over \"%s\")\n",b->name,label,old);
    }
}
static void u1_kbm_row(const U1Build *b, uint64_t image, unsigned char *page, unsigned char *item) {
    uint32_t id, text;
    unsigned char *label;
    memcpy(&id,item+0xa8,4);
    memcpy(&label,item+8,sizeof(label));
    if (!label) { u1_kbm_page=NULL; return; }
    memcpy(&text,label+8,4);
    if (id==U1_LAYOUT_ID && text==U1_LAYOUT_TEXT) {
        u1_kbm_page=page;
        memcpy(u1_kbm_label,label,0x18);
        memcpy(&u1_kbm_owner,item+0xa0,sizeof(u1_kbm_owner));
        return;
    }
    unsigned char *const after=u1_kbm_page;
    u1_kbm_page=NULL;
    if (page!=after || id!=U1_RESET_ID || text!=U1_RESET_TEXT) return;
    int32_t count;
    memcpy(&count,page+0x98,4);
    if (count<1 || count>=19) { printf("Exit menu: %s: the Controls page holds %d items: no Keyboard / Mouse row\n",b->name,count); return; }
    for (int32_t i=0;i<count;++i) {
        unsigned char *other;
        uint32_t other_id;
        memcpy(&other,page+0xa0+8*(size_t)i,sizeof(other));
        if (!other) continue;
        memcpy(&other_id,other+0xa8,4);
        if (other_id==U1_KBM_ID) return;   /* (built again into the same page) */
    }
    u1_kbm_label_text(b,image);
    unsigned char *row=((Alloc)(uintptr_t)(image+b->alloc))(0x108);
    unsigned char *row_label=((Alloc)(uintptr_t)(image+b->alloc))(0x18);
    if (!row || !row_label) return;
    memcpy(row_label,u1_kbm_label,0x18);
    const uint64_t row_text=U1_KBM_TEXT;
    memcpy(row_label+8,&row_text,sizeof(row_text));
    ((AddItem)(uintptr_t)(image+b->add_item))(row,u1_kbm_owner,U1_KBM_ID,row_label,0);
    adding=1;
    ((AddToPage)(uintptr_t)(image+b->add_to_page))(page,row);
    adding=0;
    static int told;
    if (told++<3) printf("Exit menu: %s: Keyboard / Mouse row added to Options > Controls (%d items)\n",b->name,count+1);
}

static void u1_add_to_page(const U1Build *b, uint64_t image, unsigned char *page, unsigned char *item) {
    if (adding) return;  /* our own AddToPage call below */
    if (item && runtime_kbm_row_on()) u1_kbm_row(b,image,page,item);
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
        if (spot->added && id==spot->after_id && text==spot->after_text) {
            static int told[2];
            if (told[s]++<5) printf("Exit menu: Uncharted 1: the %s is built again (page %p): no Quit added this time\n",
                                    spot->where,(void *)page);
        }
        if (spot->added || spot->page || id!=spot->after_id || text!=spot->after_text) continue;
        spot->page=page;
        memcpy(&spot->owner,item+0xa0,sizeof(spot->owner));
        memcpy(spot->label,label,0x18);
    }
}

/* The pause menu's Save Game was picked (U2/U3: BUTTON_SAVEGAME 0x62ca5c41; U1: U1_SAVE_GAME_ID): the game's save
 * dialog that follows is the pause menu's "Save game" list (runtime_services.c), a save by hand. Chapter Select, New
 * Game and the like ask the same dialog where to save without this ("Where to save"). Taken once, only within 3 s
 * (the list follows at once), and any other item picked meanwhile drops it (backed out of Save Game, then Chapter
 * Select). BB_EXIT_MENU_IDS=1 (diagnosis): every item id the menus' handlers get. */
#define DC_SAVEGAME_ID 0x62ca5c41u
/* U1 1.02, seen (BB_EXIT_MENU_IDS=1): Save Game 0x0b, Load Game 0x0c (the Load list follows), Chapter Select 0x17
 * (then the chapter and its part, 0x4xx, then where to save). */
#define U1_SAVE_GAME_ID 0x0bu
static struct timespec save_requested_at;
static void note_item(const char *game, uint32_t id, int save_game) {
    static int log = -1;
    if (log < 0) { const char *e=getenv("BB_EXIT_MENU_IDS"); log = e && e[0]=='1'; }
    if (log) printf("Exit menu: %s: menu item %08x picked\n", game, id);
    if (save_game) clock_gettime(CLOCK_MONOTONIC, &save_requested_at);
    else save_requested_at.tv_sec = 0;
}
int runtime_exit_menu_take_save_request(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int fresh = save_requested_at.tv_sec && now.tv_sec - save_requested_at.tv_sec <= 3;
    save_requested_at.tv_sec = 0;
    return fresh;
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
    note_item(b->name, id, id == U1_SAVE_GAME_ID);
    if (id==QUIT_ID) quit_from_menu("Uncharted 1");
    if (id==U1_KBM_ID) { printf("Exit menu: %s: Keyboard / Mouse picked: the overlay's Controls page\n",b->name); bbgpu_open_controls(); }
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
#define DC_SPEEDRUN_QUIT_ID 0x44ff3436u
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

/* The photo mode row of the Options' Game pages (antikmewzik; label text 0xa3da9f13, "Photo Mode Enable", the last item of
 * its page). Photo mode works in the port (d-pad down in play once the row is on; on keyboard and mouse the d-pad is the
 * arrows and the wheel, which is why antikmewzik took it out), and the game's own row keeps it off unless the player turns it
 * on, so the row stays (2026-11-13). BB_PHOTO_ROW=0 leaves it out of every page that holds it (the count one less when it
 * is the last item, else a copy of the items without it). */
#define DC_PHOTO_TEXT 0xa3da9f13u
static void dc_drop_photo_row(const DcLayout *l, unsigned char *menu) {
    const char *keep=getenv("BB_PHOTO_ROW");
    if (!keep || keep[0]!='0') return;
    uint32_t pages;
    unsigned char *page_array;
    memcpy(&pages,menu,4);
    memcpy(&page_array,menu+8,sizeof(page_array));
    if (!page_array || pages>4096) return;
    int dropped=0;
    for (uint32_t p=0;p<pages;++p) {
        unsigned char *page=page_array+(size_t)p*l->page_stride, *header;
        memcpy(&header,page+l->page_items,sizeof(header));
        if (!header) continue;
        uint32_t count;
        unsigned char *items;
        memcpy(&count,header,4);
        memcpy(&items,header+8,sizeof(items));
        if (!items || count<2 || count>256) continue;
        for (uint32_t i=0;i<count;++i) {
            uint32_t label;
            memcpy(&label,items+(size_t)i*l->item_stride+0x28,4);
            if (label!=DC_PHOTO_TEXT) continue;
            const uint32_t left=count-1;
            if (i==count-1) memcpy(header,&left,4);
            else {   /* kept for the life of the process, as the Quit items */
                unsigned char *copy=calloc(count,l->item_stride), *new_header=calloc(1,16);
                if (!copy || !new_header) { free(copy); free(new_header); break; }
                memcpy(copy,items,(size_t)i*l->item_stride);
                memcpy(copy+(size_t)i*l->item_stride,items+(size_t)(i+1)*l->item_stride,(size_t)(count-i-1)*l->item_stride);
                memcpy(new_header,&left,4);
                memcpy(new_header+8,&copy,sizeof(copy));
                memcpy(page+l->page_items,&new_header,sizeof(new_header));
            }
            uint32_t sid;
            memcpy(&sid,page+8,4);
            printf("Exit menu: %s: photo mode row left out of page %08x (item %u of %u)\n",l->name,sid,i+1,count);
            ++dropped;
            break;
        }
    }
    static int told;   /* (the data looked up again: the row is gone already) */
    if (!dropped && !told++) printf("Exit menu: %s: no photo mode row found in the menus\n",l->name);
}

/* The Keyboard / Mouse row (U2, U3; antikmewzik's u2_menu_def_patch / u3_row_walk): in every page that holds the Controller
 * Layout record (label text 0xdd40dc7b, the row of the controller picture; the page's title and the Options entry have other
 * texts), a copy of it right after it, with an id of its own (no target page: it goes to the menu handler) and the borrowed
 * label text 0x62501513 (runtime_menumouse.c writes the label over it). Only when that label can be written (the text lookup
 * hook of the menu mouse is in). */
#define DC_LAYOUT_TEXT 0xdd40dc7bu
#define DC_KBM_ID 0x3a1a91d1u
#define DC_KBM_TEXT 0x62501513u
#define DC_CONTROLS_PAGE 0x5c717aadu
static void dc_add_kbm_row(const DcLayout *l, unsigned char *menu) {
    if (!runtime_kbm_row_on()) return;
    static int told_hook;
    if (!runtime_menumouse_kbm_label_hook()) {
        if (!told_hook++) printf("Exit menu: %s: no Keyboard / Mouse row (the menu mouse's text lookup hook is not in)\n",l->name);
        return;
    }
    uint32_t pages;
    unsigned char *page_array;
    memcpy(&pages,menu,4);
    memcpy(&page_array,menu+8,sizeof(page_array));
    if (!page_array || pages>4096) return;
    int found=0;
    for (uint32_t p=0;p<pages;++p) {
        unsigned char *page=page_array+(size_t)p*l->page_stride, *header;
        memcpy(&header,page+l->page_items,sizeof(header));
        if (!header) continue;
        /* Only Options > Controls (page 5c717aad in U2 and U3, 1.00 and 1.02). U2 also has a 3-row page 9400ffbb with
         * Controller Layout that players could not find in any menu (2026-11-13): it stays as it is. */
        uint32_t page_id;
        memcpy(&page_id,page+8,4);
        if (page_id!=DC_CONTROLS_PAGE) continue;
        uint32_t count;
        unsigned char *items;
        memcpy(&count,header,4);
        memcpy(&items,header+8,sizeof(items));
        if (!items || count==0 || count>256) continue;
        int at=-1, have=0;
        for (uint32_t i=0;i<count;++i) {
            uint32_t id, label;
            memcpy(&id,items+(size_t)i*l->item_stride+8,4);
            memcpy(&label,items+(size_t)i*l->item_stride+0x28,4);
            if (id==DC_KBM_ID) have=1;
            if (label==DC_LAYOUT_TEXT && at<0) at=(int)i;
        }
        if (at<0) continue;
        ++found;
        if (have) continue;   /* (the data looked up again) */
        /* Kept for the life of the process, as the Quit items. */
        unsigned char *copy=calloc((size_t)count+1,l->item_stride), *new_header=calloc(1,16);
        if (!copy || !new_header) { free(copy); free(new_header); return; }
        const size_t head=(size_t)(at+1)*l->item_stride;
        memcpy(copy,items,head);
        memcpy(copy+head,items+(size_t)at*l->item_stride,l->item_stride);
        memcpy(copy+head+l->item_stride,items+head,(size_t)count*l->item_stride-head);
        unsigned char *row=copy+head;
        static const char name[]="BUTTON_KEYBOARD_MOUSE", label_name[]="BP_MENU_KEYBOARD_MOUSE";
        const char *name_ptr=name, *label_ptr=label_name;
        const uint32_t id=DC_KBM_ID, text=DC_KBM_TEXT, target=0;
        uint32_t type;
        memcpy(&type,row+0x10,4);
        memcpy(row,&name_ptr,sizeof(name_ptr));
        memcpy(row+8,&id,4);
        memcpy(row+0x20,&label_ptr,sizeof(label_ptr));
        memcpy(row+0x28,&text,4);
        memcpy(row+0x50,&target,4);
        const uint32_t new_count=count+1;
        memcpy(new_header,&new_count,4);
        memcpy(new_header+8,&copy,sizeof(copy));
        memcpy(page+l->page_items,&new_header,sizeof(new_header));
        uint32_t sid;
        memcpy(&sid,page+8,4);
        printf("Exit menu: %s: Keyboard / Mouse row added to page %08x after item %d (type %u; %u items)\n",l->name,sid,at+1,
               type,new_count);
    }
    static int told;
    if (!found && !told++) printf("Exit menu: %s: no Controller Layout row in the menus: no Keyboard / Mouse row\n",l->name);
}

/* BB_EXIT_MENU_PAGES=1 (diagnosis): every page of the menu data that holds the game's own "Quit to Main Menu"
 * (0xfdcd24df) or Load Game (0xf7a5b490), with its items' ids, once per lookup. */
static void dc_list_pages(const DcLayout *l, unsigned char *menu) {
    const char *e=getenv("BB_EXIT_MENU_PAGES");
    if (!e || e[0]!='1') return;
    uint32_t pages;
    unsigned char *page_array;
    memcpy(&pages,menu,4);
    memcpy(&page_array,menu+8,sizeof(page_array));
    if (!page_array || pages>4096) return;
    printf("Exit menu: %s: menu data %p, %u pages\n",l->name,(void *)menu,pages);
    for (uint32_t p=0;p<pages;++p) {
        unsigned char *page=page_array+(size_t)p*l->page_stride, *header;
        uint32_t sid, count;
        unsigned char *items;
        memcpy(&sid,page+8,4);
        memcpy(&header,page+l->page_items,sizeof(header));
        if (!header) continue;
        memcpy(&count,header,4);
        memcpy(&items,header+8,sizeof(items));
        if (!items || count==0 || count>256) continue;
        int interesting=0;
        for (uint32_t i=0;i<count;++i) {
            uint32_t id;
            memcpy(&id,items+(size_t)i*l->item_stride+8,4);
            interesting|=id==DC_GAMEROOT_QUIT_ID || id==DC_LOADGAME_ID || id==DC_QUIT_ID || id==DC_PAUSE_QUIT_ID;
        }
        if (!interesting) continue;
        char line[1200];
        int n=snprintf(line,sizeof(line),"Exit menu: %s: page %08x (%u items):",l->name,sid,count);
        for (uint32_t i=0;i<count && n<(int)sizeof(line)-40;++i) {
            uint32_t id, label;
            const char *name;
            memcpy(&id,items+(size_t)i*l->item_stride+8,4);
            memcpy(&label,items+(size_t)i*l->item_stride+0x28,4);
            memcpy(&name,items+(size_t)i*l->item_stride,sizeof(name));
            n+=snprintf(line+n,sizeof(line)-n," %08x(%.24s)",id,name ? name : "?");
            (void)label;
        }
        printf("%s\n",line);
    }
}

static void dc_add_quit(const DcLayout *l, unsigned char *menu) {
    if (!menu) return;
    dc_list_pages(l,menu);
    dc_drop_photo_row(l,menu);
    dc_add_kbm_row(l,menu);
    /* Main menu: closes the game at once (nothing to lose there). */
    dc_add_item(l,menu,DC_PROFILE_PAGE,DC_LOADGAME_ID,"BUTTON_QUIT_GAME",DC_QUIT_ID,0,"main menu");
    /* Pause menus: opens the game's Yes/No page, set up by the handler hook below. Every page that ends in one of the
     * game's two "Quit to Main Menu" items gets it, not only the usual pause page (f05cd578): the games also have a
     * pause page with the speed run's Quit to Main Menu (50a120f8, BP_BUTTON_SPEEDRUN_QUIT), and U3 a short one
     * (b8c303dc: Options, Restart, Quit to Main Menu). On those our Quit was missing, until the game showed the usual
     * page again (a player's U2: gone, back after a checkpoint restart). */
    uint32_t pages;
    unsigned char *page_array;
    memcpy(&pages,menu,4);
    memcpy(&page_array,menu+8,sizeof(page_array));
    if (!page_array || pages>4096) return;
    for (uint32_t p=0;p<pages;++p) {
        unsigned char *page=page_array+(size_t)p*l->page_stride, *header;
        uint32_t sid, count;
        unsigned char *items;
        memcpy(&sid,page+8,4);
        memcpy(&header,page+l->page_items,sizeof(header));
        if (!header || sid==DC_PROFILE_PAGE) continue;
        memcpy(&count,header,4);
        memcpy(&items,header+8,sizeof(items));
        if (!items || count==0 || count>256) continue;
        uint32_t template_id=0;
        for (uint32_t i=0;i<count;++i) {
            uint32_t id;
            memcpy(&id,items+(size_t)i*l->item_stride+8,4);
            /* the usual pause page's item as the template when the page has both (the speed run's only where it is alone) */
            if (id==DC_GAMEROOT_QUIT_ID) template_id=id;
            else if (id==DC_SPEEDRUN_QUIT_ID && !template_id) template_id=id;
        }
        if (!template_id) continue;
        char where[40];
        snprintf(where,sizeof(where),"pause menu %08x",sid);
        dc_add_item(l,menu,sid,template_id,"BUTTON_QUIT_DESKTOP",DC_PAUSE_QUIT_ID,DC_YESNO_PAGE,where);
    }
}

/* The pause menu's Quit was picked: the handler goes on to open the Yes/No page (the item's target page); its
 * record gets the title "Quit", the game's Yes and No, and Yes sends the main menu's Quit id. */
static void dc_handler(const DcLayout *l, unsigned char *host, uint32_t id) {
    note_item(l->name, id, id == DC_SAVEGAME_ID);
    if (id==DC_QUIT_ID) quit_from_menu(l->name);
    if (id==DC_KBM_ID) { printf("Exit menu: %s: Keyboard / Mouse picked: the overlay's Controls page\n",l->name); bbgpu_open_controls(); }
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
    (void)rdi; (void)rsi;
    dc_add_quit(&dc_u2,(unsigned char *)(uintptr_t)regs[7]);
    /* the menu builder's host (its first argument; the store here is host+0x17a8): the builder keeps it in rbx on 1.02
     * (0x5c53b4 mov rbx,rdi) and in r14 on 1.00 (0x29d764 mov r14,rdi); told apart by those instructions (the hook site itself
     * is patched). The menu mouse finds the page on screen through it (runtime_menumouse.c, U2 direct jump). */
    static const unsigned char keep_102[]={0x48,0x89,0xfb,0x48,0x89,0x9d,0xa8,0xfe,0xff,0xff};   /* mov rbx,rdi; mov [rbp-0x158],rbx */
    static const unsigned char keep_100[]={0x48,0x81,0xec,0x88,0x01,0x00,0x00,0x49,0x89,0xfe};   /* sub rsp,0x188; mov r14,rdi */
    const unsigned char *code=(const unsigned char *)(uintptr_t)image;
    if (!memcmp(code+0x5c53b4,keep_102,sizeof(keep_102))) runtime_menumouse_u2_host(regs[0]);
    else if (!memcmp(code+0x29d75d,keep_100,sizeof(keep_100))) runtime_menumouse_u2_host(r14);
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
