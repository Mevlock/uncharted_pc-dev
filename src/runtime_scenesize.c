/* PROTOTYPE (BB_SCENE_OWN_SIZE=1, off by default): the game draws its own scene at the render size.
 *
 * With the upscale before the game's post-processing (BB_SCALED_BEFORE_POST: FSR 3.1 / FSR 4) the game runs at the
 * OUTPUT size (display config, HUD, gameplay screen maths, post-processing) and so far the renderer swapped reduced
 * stand-ins under part of the scene's passes. Here the game itself creates the scene's render targets at the render
 * size, so every scene pass draws that many pixels (a pass takes its viewport from the target it binds).
 *
 * The engine (Uncharted 1, 2, 3 share it) has one function that creates every screen-sized render target (the
 * "target setup"): it reads the width and height once from the display config (+0x08 / +0x0c) and calls one creator
 * for colour targets and one for depth targets, with the target's id, width and height. The pieces, all found by the
 * code's structure (nothing happens in a program where one is missing):
 *  - a scene copy of the display config whose sizes are the render size, kept equal to the game's own otherwise
 *    (refreshed when the target setup starts and once a frame);
 *  - the target setup reads the scene copy: every size it derives is the scene's;
 *  - the two creators are hooked: a target of the output class (used in or after the post-processing: the post
 *    chain, the bloom mips, the velocity / motion blur targets, screenshots, movies) asked for by the target setup
 *    gets its size from the output size again;
 *  - the config's second size pair (+0x18 / +0x1c: the frame's screen-scale constants and the shadow resolves, scene
 *    only) is the render size (runtime_patch_resolution);
 *  - the scene passes that read the first or third pair (deferred lights, particles, the ambient pass, lens flares)
 *    read the scene copy instead (a per-program list of the config's load sites, each checked to be one).
 * BB_SCENE_OWN_PARTS=<letters> (tests): t target setup, c output class, 2 pair 2, r readers (default: all).
 * BB_SCENE_OWN_CLASS=+c4,-c73,+d2 (tests): ids added to / taken from the output class.
 * BB_SCENE_OWN_READERS=<hex,hex,...> (tests): the reader sites, instead of the program's list. */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_BYTES 0x98
#define MAX_TARGETS 256

void *runtime_near_image_alloc(size_t size); /* runtime_memory.c */

/* Read by the GPU side: the size of the game's full-size scene targets when it is not the game's display size. */
int runtime_scene_own_size;
unsigned runtime_scene_own_width, runtime_scene_own_height;

static unsigned char *image;
static uint8_t *config, *scene_config;
static uint64_t setup_start, setup_end, setup_lea, creators[2];
static unsigned scene_w, scene_h, output_w, output_h;
static const char *parts = "tc2r";
static uint8_t output_class[2][MAX_TARGETS]; /* [depth][id] */
static struct { uint8_t depth; uint16_t id; uint8_t *entry; } created[MAX_TARGETS];
static int created_count, entries_logged;

static int part(char c) { return strchr(parts, c) != NULL; }

/* The output class: targets the post-processing, the movie player, the screenshot code or the HUD use (the engine's
 * target ids, the same in all three games). Everything else the target setup makes screen-sized is the scene's. */
static void class_defaults(void) {
    static const uint8_t colour[] = {0,   1,   2,   3,                          /* screenshot (supersampled) */
                                     28,  35,  36,  37,  38,  39,               /* post chain */
                                     72,  73,  74,  75,                         /* quarter-size mips (bloom; 73 shared with particles) */
                                     96,  108, 138, 139, 159,                   /* screenshot, movie */
                                     140, 141, 142, 143, 144, 145, 146, 147,    /* velocity, motion blur */
                                     153};                                      /* depth histogram */
    for (size_t i = 0; i < sizeof(colour); ++i) output_class[0][colour[i]] = 1;
    output_class[1][15] = 1; /* the velocity pass's depth */
    output_class[1][13] = 1; /* bound together with colour 73 (a binding's targets share one class: its viewport is the colour's) */
    const char *env = getenv("BB_SCENE_OWN_CLASS");
    for (const char *p = env; p && *p;) {
        const int add = *p == '+';
        if ((*p == '+' || *p == '-') && (p[1] == 'c' || p[1] == 'd')) {
            const unsigned long id = strtoul(p + 2, NULL, 10);
            if (id < MAX_TARGETS) output_class[p[1] == 'd'][id] = (uint8_t)add;
        }
        p = strchr(p, ',');
        if (p) ++p;
    }
}

/* `mov r32, [reg + disp8]` with an optional REX prefix: its length, or 0. */
static int load_at(const uint8_t *p, uint8_t disp) {
    int k = 0;
    if (p[k] >= 0x40 && p[k] <= 0x4f) ++k;
    if (p[k] != 0x8b || (p[k + 1] & 0xc0) != 0x40 || (p[k + 1] & 7) == 4 || p[k + 2] != disp) return 0;
    return k + 3;
}

static int is_lea_rip(const uint8_t *p) { return (p[0] == 0x48 || p[0] == 0x4c) && p[1] == 0x8d && (p[2] & 0xc7) == 0x05; }
static const uint8_t *lea_target(const uint8_t *p) {
    int32_t d;
    memcpy(&d, p + 3, 4);
    return p + 7 + d;
}

/* The reader sites per program (offsets of `lea r64, [rip + display config]`), named by the config's own offset. */
typedef struct {
    const char *name;
    uint64_t config;
    uint64_t sites[16];
} Readers;
static const Readers readers[] = {
    {"Uncharted 3 1.02", 0x18c6b34,
     {0xf5931e,                        /* deferred runtime lights: pair 3 */
      0xf5ce76, 0xf5e719,              /* light tiles: pair 3 */
      0xeb84ee,                        /* particles: pair 3 */
      0xf560a6, 0xf57104,              /* ambient pass: pair 1 */
      0x105610b, 0x1056244, 0x1056581, 0x10566b1}}, /* lens flares: pair 1 */
};

static const unsigned char prologue[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}; /* push rbp; mov rbp, rsp; push r15 */

static void refresh(void) {
    uint8_t now[CONFIG_BYTES];
    memcpy(now, config, CONFIG_BYTES);
    const uint32_t w = scene_w, h = scene_h, w8 = scene_w / 8, h4 = scene_h / 4;
    static const uint8_t pairs[3] = {0x08, 0x18, 0x24};
    for (int i = 0; i < 3; ++i) {
        memcpy(now + pairs[i], &w, 4);
        memcpy(now + pairs[i] + 4, &h, 4);
    }
    memcpy(now + 0x40, &w8, 4); /* the eighth / quarter size stored with the pairs */
    memcpy(now + 0x44, &h4, 4);
    if (memcmp(scene_config, now, CONFIG_BYTES)) memcpy(scene_config, now, CONFIG_BYTES);
}

static void __attribute__((ms_abi)) setup_entry(uint64_t rdi, uint64_t rsi, uint64_t base, uint64_t r14,
                                               const uint64_t *regs) {
    (void)rdi; (void)rsi; (void)base; (void)r14; (void)regs;
    refresh();
    created_count = 0;
    entries_logged = 0;
    printf("Scene size: target setup starts: scene %ux%u, output %ux%u\n", scene_w, scene_h, output_w, output_h);
}

/* A creator's start: rdi = the target table, ecx = id, r8w = width, r9w = height; regs[9 + 16] = the caller. */
static void create(int depth, uint64_t rdi, const uint64_t *regs) {
    const uint64_t caller = regs[9 + 16] - (uint64_t)(uintptr_t)image;
    const unsigned id = (unsigned)(regs[6] & 0xffffffffu), w = (unsigned)(regs[4] & 0xffff), h = (unsigned)(regs[3] & 0xffff);
    if (caller < setup_start || caller >= setup_end) {
        printf("Scene size: %s %u %ux%u created outside the target setup (%#llx): untouched\n", depth ? "depth" : "colour",
               id, w, h, (unsigned long long)caller);
        return;
    }
    unsigned nw = w, nh = h;
    const int output = id < MAX_TARGETS && output_class[depth][id];
    const char *how = "scene";
    if (output) {
        how = "output, size kept (not a screen size)";
        for (unsigned k = 0; k < 4 && nw == w && nh == h; ++k)
            if (w == scene_w >> k && h == scene_h >> k) { nw = output_w >> k; nh = output_h >> k; how = "output"; }
        for (unsigned m = 2; m <= 4 && nw == w && nh == h; ++m)
            if (w == scene_w * m && h == scene_h * m) { nw = output_w * m; nh = output_h * m; how = "output"; }
        if (!part('c')) { nw = w; nh = h; how = "output class, left at the scene size (BB_SCENE_OWN_PARTS)"; }
    }
    if (nw != w || nh != h) {
        uint64_t *r = (uint64_t *)regs;
        r[4] = (r[4] & ~(uint64_t)0xffff) | nw;
        r[3] = (r[3] & ~(uint64_t)0xffff) | nh;
    }
    if (id < MAX_TARGETS && created_count < MAX_TARGETS) {
        created[created_count].depth = (uint8_t)depth;
        created[created_count].id = (uint16_t)id;
        created[created_count].entry = (uint8_t *)(uintptr_t)rdi + (depth ? 0x3700 + id * 0x60ull : id * 0x58ull);
        ++created_count;
    }
    printf("Scene size: %s%u %ux%u", depth ? "d" : "c", id, w, h);
    if (nw != w || nh != h) printf(" -> %ux%u", nw, nh);
    printf(" (%s) at %#llx\n", how, (unsigned long long)caller);
}
static void __attribute__((ms_abi)) create_colour(uint64_t rdi, uint64_t rsi, uint64_t base, uint64_t r14,
                                                 const uint64_t *regs) {
    (void)rsi; (void)base; (void)r14;
    create(0, rdi, regs);
}
static void __attribute__((ms_abi)) create_depth(uint64_t rdi, uint64_t rsi, uint64_t base, uint64_t r14,
                                                const uint64_t *regs) {
    (void)rsi; (void)base; (void)r14;
    create(1, rdi, regs);
}

static RuntimeGuestCallback sites[3];
static size_t site_count;

/* Finds the pieces (before the game callbacks go in). */
void runtime_scenesize_prepare(unsigned char *base, uint64_t size) {
    const char *env = getenv("BB_SCENE_OWN_SIZE"), *scene = getenv("BB_SCENE_SIZE"), *output = getenv("BB_RENDER_RES");
    if (!env || env[0] != '1') return;
    if (!scene || !output || sscanf(scene, "%ux%u", &scene_w, &scene_h) != 2 ||
        sscanf(output, "%ux%u", &output_w, &output_h) != 2) {
        printf("Scene size: BB_SCENE_OWN_SIZE needs the upscale before the post-processing (a render size below the "
               "output size with FSR 3.1 / FSR 4 or BB_SCALED_BEFORE_POST=1): off\n");
        return;
    }
    /* as runtime_patch_resolution rounds the game's size (16 wide: the engine's tiles; 8 high: the bloom chain) */
    scene_w &= ~15u; scene_h &= ~7u; output_w &= ~15u; output_h &= ~7u;
    if (getenv("BB_SCENE_OWN_PARTS")) parts = getenv("BB_SCENE_OWN_PARTS");
    image = base;
    /* The target setup's read of the display config: lea r, [config]; mov w, [r + 8]; mov h, [r + 0xc];
     * lea rax, [w + 0x3f]; shr rax, 6 (the light tile grid). */
    int found = 0;
    for (uint64_t i = 0; i + 32 < size; ++i) {
        const uint8_t *p = base + i;
        if (!is_lea_rip(p)) continue;
        const int a = load_at(p + 7, 0x08);
        if (!a) continue;
        const int b = load_at(p + 7 + a, 0x0c);
        if (!b) continue;
        const uint8_t *q = p + 7 + a + b;
        if ((q[0] != 0x48 && q[0] != 0x49) || q[1] != 0x8d || (q[2] & 0xf8) != 0x40) continue;
        const int sib = (q[2] & 7) == 4; /* [r12 + 0x3f] takes a SIB byte (the 1.00 programs) */
        if ((sib && q[3] != 0x24) || q[3 + sib] != 0x3f || memcmp(q + 4 + sib, "\x48\xc1\xe8\x06", 4)) continue;
        setup_lea = i;
        ++found;
    }
    if (found != 1) {
        printf("Scene size: the target setup was not found in this program (%d): off\n", found);
        return;
    }
    config = (uint8_t *)lea_target(base + setup_lea);
    for (uint64_t i = setup_lea; i > setup_lea - 0x200 && !setup_start; --i)
        if (!memcmp(base + i, prologue, sizeof(prologue))) setup_start = i;
    /* Its end: the next function's start (16-aligned, after padding or a ret). */
    for (uint64_t i = (setup_start + 0x1000 + 15) & ~(uint64_t)15; setup_start && i < setup_start + 0x8000 && i + 4 < size; i += 16) {
        const uint8_t before = base[i - 1];
        if (!memcmp(base + i, prologue, 4) && (before == 0x00 || before == 0x90 || before == 0xc3 || before == 0xcc)) {
            setup_end = i;
            break;
        }
    }
    /* The creators: the functions the setup calls most whose entry address is table + id * 0x58 (colour) or
     * table + 0x3700 + id * 0x60 (depth). */
    int colours = 0, depths = 0;
    for (uint64_t i = setup_start; setup_end && i + 5 < setup_end; ++i) {
        if (base[i] != 0xe8) continue;
        int32_t d;
        memcpy(&d, base + i + 1, 4);
        const uint64_t target = i + 5 + (int64_t)d;
        if (target >= size - 0x200 || memcmp(base + target, prologue, sizeof(prologue))) continue;
        if (target == creators[0] || target == creators[1]) continue;
        int calls = 0, by58 = 0, by3700 = 0;
        for (uint64_t j = setup_start; j + 5 < setup_end; ++j) {
            int32_t e;
            if (base[j] != 0xe8) continue;
            memcpy(&e, base + j + 1, 4);
            calls += j + 5 + (int64_t)e == target;
        }
        for (int k = 0; k < 0x200; ++k) {
            by58 |= k < 0x60 && base[target + k] == 0x6b && base[target + k + 2] == 0x58;
            by3700 |= !memcmp(base + target + k, "\x00\x37\x00\x00", 4);
        }
        if (calls >= 40 && by58) { creators[0] = target; ++colours; }
        else if (calls >= 8 && by3700) { creators[1] = target; ++depths; }
    }
    if (!setup_start || !setup_end || colours != 1 || depths != 1) {
        printf("Scene size: the target creators were not found (setup %#llx..%#llx, %d colour, %d depth): off\n",
               (unsigned long long)setup_start, (unsigned long long)setup_end, colours, depths);
        return;
    }
    if (!(scene_config = runtime_near_image_alloc(4096))) return;
    class_defaults();
    static const char *names[3] = {"scene size: target setup", "scene size: colour target creator",
                                   "scene size: depth target creator"};
    const uint64_t at[3] = {setup_start, creators[0], creators[1]};
    void (__attribute__((ms_abi)) *const fn[3])(uint64_t, uint64_t, uint64_t, uint64_t, const uint64_t *) = {
        setup_entry, create_colour, create_depth};
    for (int i = 0; i < 3; ++i)
        sites[i] = (RuntimeGuestCallback){names[i], at[i], prologue, sizeof(prologue), sizeof(prologue), fn[i], NULL};
    site_count = 3;
    runtime_scene_own_size = 1;
    printf("Scene size: the game draws its scene at %ux%u, output %ux%u (BB_SCENE_OWN_SIZE=1, parts %s): display config "
           "%#llx, target setup %#llx..%#llx (config read at %#llx), creators %#llx / %#llx\n",
           scene_w, scene_h, output_w, output_h, parts, (unsigned long long)(config - base),
           (unsigned long long)setup_start, (unsigned long long)setup_end, (unsigned long long)setup_lea,
           (unsigned long long)creators[0], (unsigned long long)creators[1]);
}

const RuntimeGuestCallback *runtime_scenesize_callbacks(size_t *count) {
    *count = site_count;
    return sites;
}

/* The config's second size pair (runtime_patch_resolution asks while it writes the display modes). */
int runtime_scenesize_pair2(unsigned *width, unsigned *height) {
    if (!runtime_scene_own_size || !part('2')) return 0;
    *width = scene_w;
    *height = scene_h;
    return 1;
}

static int redirect(uint8_t *code, uint64_t at) {
    uint8_t *p = code + at;
    if (!is_lea_rip(p) || lea_target(p) != config) return 0;
    const int32_t rel = (int32_t)((intptr_t)scene_config - (intptr_t)(p + 7));
    memcpy(p + 3, &rel, 4);
    return 1;
}

/* The code patches (from runtime_patch_resolution, after the display modes got their sizes). */
void runtime_scenesize_patch(uint8_t *code, uint64_t size) {
    if (!runtime_scene_own_size) return;
    int setup = 0, done = 0, wanted = 0;
    if (part('t')) {
        setup = redirect(code, setup_lea);
        runtime_scene_own_width = scene_w;
        runtime_scene_own_height = scene_h;
    }
    if (part('r')) {
        const char *env = getenv("BB_SCENE_OWN_READERS");
        if (env) {
            for (const char *p = env; *p;) {
                char *end;
                const uint64_t at = strtoull(p, &end, 16);
                if (end == p) break;
                ++wanted;
                if (at + 7 < size && redirect(code, at)) ++done;
                else printf("Scene size: %#llx does not load the display config: left\n", (unsigned long long)at);
                p = *end == ',' ? end + 1 : end;
            }
        } else {
            for (size_t r = 0; r < sizeof(readers) / sizeof(*readers); ++r) {
                if (readers[r].config != (uint64_t)(config - code)) continue;
                for (int i = 0; i < 16 && readers[r].sites[i]; ++i) {
                    ++wanted;
                    if (readers[r].sites[i] + 7 < size && redirect(code, readers[r].sites[i])) ++done;
                    else printf("Scene size: %s: %#llx does not load the display config: left\n", readers[r].name,
                                (unsigned long long)readers[r].sites[i]);
                }
            }
        }
    }
    refresh();
    printf("Scene size: target setup reads the scene copy %s, %d of %d scene readers redirected, pair 2 %s\n",
           setup ? "yes" : "no", done, wanted, part('2') ? "at the scene size" : "left");
}

/* Once a frame (the game's frame start): the scene copy follows the game's config. */
void runtime_scenesize_frame(void) {
    if (!runtime_scene_own_size) return;
    refresh();
    /* BB_SCENE_OWN_LOG=1 (diagnosis): the target table's entries once after the setup (which field holds what). */
    if (!entries_logged && created_count) {
        entries_logged = 1;
        const char *log = getenv("BB_SCENE_OWN_LOG");
        if (!log || log[0] != '1') return;
        for (int i = 0; i < created_count; ++i) {
            const int n = created[i].depth ? 0x60 : 0x58;
            printf("Scene size: entry %s%u:", created[i].depth ? "d" : "c", created[i].id);
            for (int k = 0; k < n; k += 8) {
                uint64_t v;
                memcpy(&v, created[i].entry + k, 8);
                printf(" %llx", (unsigned long long)v);
            }
            printf("\n");
        }
    }
}
