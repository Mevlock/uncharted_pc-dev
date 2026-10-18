/* Ultrawide (Hor+) and field of view for the three games, applied to the loaded game code in
 * memory (the files are not changed). Technique from the Uncharted Ultrawide Patcher 1.2.
 *
 * The engine's perspective projections (CreatePerspectiveProjectionZoom / ...InfiniteFar in
 * rendercamera.h), view frustum planes and corners (culling) and view-ray constants are built
 * from two adjacent camera fields read back to back: the inverse aspect ratio (height / width,
 * field F) and the zoom (1 / tan of half the horizontal field of view, F + 4). Every place that
 * reads both of them gets them scaled: inverse aspect by k = (16/9) / (width/height), zoom by
 * k / fov. The horizontal view widens to the screen's aspect while the vertical one stays as on
 * 16:9 (Hor+), culling widens with it, and fov > 1 widens both directions. Code reading only one
 * field (level of detail by on-screen size) keeps the original. Each read jumps to a stub in the
 * zero padding at the end of the code segment: the original instruction, a vmulss by the
 * constant, a jump back.
 *
 * BB_ULTRAWIDE: auto (default: the window, or the screen in fullscreen), 0 (off), or WIDTHxHEIGHT.
 * BB_FOV: percent of the game's field of view, 100 (default) to 150. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* Read by the presenter: stretch the frame over the whole window instead of letterboxing it. */
int runtime_ultrawide_fill;
/* The horizontal factor k (<= 1): the UI drawn over a widened frame is squeezed by it back to
 * its 16:9 layout (the pipeline cache's ultrawide UI vertex shaders). */
float runtime_ultrawide_scale = 1.0f;
/* Set when BB_RENDER_RES patched the display mode: the presenter shows frames that do not fill
 * the window at 16:9 (the game's layout stretched over the larger buffer). */
int runtime_render_resolution;
unsigned runtime_render_width, runtime_render_height;

/* The widened constants, and the block the stubs read them from when it can be switched. */
static float wide_constants[4];
/* The constants for the other frames: all 1 (the game's own view), unless BB_RENDER_RES renders
 * into a buffer wider than 16:9 (the game then builds its camera from the buffer's shape). */
static float narrow_constants[4] = {1.0f, 1.0f, 1.0f, 1.0f};
static float *volatile switch_constants;

/* Gameplay frames get the widened view, others (menus, title screens, loading) the game's own:
 * called by the presenter for each frame it prepares. The game builds its camera a frame or
 * two ahead, so a switch shows up that much later. */
void runtime_ultrawide_select(int wide) {
    float *constants = switch_constants;
    static int current = -1;
    if (!constants || wide == current) return;
    current = wide;
    for (int i = 0; i < 4; ++i) {
        float value = wide ? wide_constants[i] : narrow_constants[i];
        __atomic_store(&constants[i], &value, __ATOMIC_RELAXED);
    }
}

/* A writable page within 2 GiB of the code (rel32 from the stubs). */
static float *alloc_near(uint8_t *code, uint64_t size) {
#ifdef _WIN32
    const uintptr_t base = ((uintptr_t)code + size + 0xffff) & ~(uintptr_t)0xffff;
    for (uintptr_t step = 0; step < (1800ull << 20); step += 1u << 16) {
        void *p = VirtualAlloc((void *)(base + step), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p) return (float *)p;
        if ((uintptr_t)code > step + (2u << 20)) {
            p = VirtualAlloc((void *)((((uintptr_t)code - step) & ~(uintptr_t)0xffff) - 0x10000), 4096,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (p) return (float *)p;
        }
    }
#else
    (void)code;
    (void)size;
#endif
    return NULL;
}

typedef struct {
    uint8_t op;  /* 0x10 vmovss, 0x59 vmulss, 0x5e vdivss */
    uint8_t reg; /* destination xmm */
    uint8_t base;
    uint8_t len;
    int32_t disp;
} ScalarOp;

/* A VEX scalar-single op (pp = F3, map 0F) with a [base + disp32] operand, no index. */
static int decode_scalar(const uint8_t *code, uint64_t size, uint64_t at, ScalarOp *op) {
    if (at + 10 > size) return 0;
    const uint8_t *c = code + at;
    int r, b = 0, vvvv, q;
    if (c[0] == 0xc5) {
        if ((c[1] & 3) != 2) return 0;
        r = !(c[1] & 0x80);
        vvvv = (~c[1] >> 3) & 15;
        q = 2;
    } else if (c[0] == 0xc4) {
        if ((c[1] & 31) != 1 || (c[2] & 3) != 2 || !(c[1] & 0x40)) return 0; /* map 0F, no VEX.X */
        r = !(c[1] & 0x80);
        b = !(c[1] & 0x20);
        vvvv = (~c[2] >> 3) & 15;
        q = 3;
    } else
        return 0;
    const uint8_t opcode = c[q], modrm = c[q + 1];
    if (opcode != 0x10 && opcode != 0x59 && opcode != 0x5e) return 0;
    if ((modrm >> 6) != 2) return 0;
    q += 2;
    int base = modrm & 7;
    if (base == 4) {
        const uint8_t sib = c[q++];
        if (((sib >> 3) & 7) != 4) return 0;
        base = sib & 7;
    }
    if (opcode == 0x10 && vvvv) return 0;
    op->op = opcode;
    op->reg = (uint8_t)(((modrm >> 3) & 7) | (r << 3));
    op->base = (uint8_t)(base | (b << 3));
    memcpy(&op->disp, c + q, 4);
    op->len = (uint8_t)(q + 4);
    return 1;
}

static int64_t find_bytes(const uint8_t *hay, uint64_t size, const void *needle, uint64_t n) {
    for (uint64_t i = 0; i + n <= size; ++i)
        if (hay[i] == *(const uint8_t *)needle && !memcmp(hay + i, needle, n)) return (int64_t)i;
    return -1;
}

/* Places that load the address of a zero-terminated string: lea r64, [rip + rel32]. */
static int string_refs(const uint8_t *code, uint64_t size, const char *name, uint64_t *out, int max) {
    const int64_t at = find_bytes(code, size, name, strlen(name) + 1);
    if (at < 0) return 0;
    int count = 0;
    for (uint64_t p = 0; p + 7 <= size && count < max; ++p) {
        if ((code[p] != 0x48 && code[p] != 0x4c) || code[p + 1] != 0x8d || (code[p + 2] & 0xc7) != 0x05)
            continue;
        int32_t rel;
        memcpy(&rel, code + p + 3, 4);
        if ((int64_t)p + 7 + rel == at) out[count++] = p;
    }
    return count;
}

enum { MAX_ANCHORS = 32, MAX_READS = 512, MAX_SITES = 64 };
typedef struct { uint64_t at; ScalarOp op; } Read;

static uint64_t distance(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

/* The camera's inverse-aspect field: loaded right before the zoom, from the same register, in
 * the projection builders. The most frequent such offset near their assertions. */
static int camera_field(const uint8_t *code, uint64_t size, const uint64_t *anchors, int count, uint32_t *field) {
    uint32_t seen[MAX_SITES];
    int n = 0;
    for (int a = 0; a < count; ++a)
        for (uint64_t q = anchors[a] > 0x200 ? anchors[a] - 0x200 : 0; q <= anchors[a]; ++q) {
            ScalarOp first, second;
            if (!decode_scalar(code, size, q, &first) || first.op != 0x10) continue;
            if (!decode_scalar(code, size, q + first.len, &second) || second.op != 0x10) continue;
            if (second.base != first.base || second.disp != first.disp + 4) continue;
            if (first.disp < 0x100 || first.disp >= 0x1000 || (first.disp & 3)) continue;
            if (n < MAX_SITES) seen[n++] = (uint32_t)first.disp;
        }
    int best = 0, tie = 0;
    for (int i = 0; i < n; ++i) {
        int votes = 0;
        for (int j = 0; j < n; ++j) votes += seen[j] == seen[i];
        if (votes > best) { best = votes; *field = seen[i]; tie = 0; }
        else if (votes == best && seen[i] != *field) tie = 1;
    }
    return best >= 2 && !tie;
}

static int parse_size(const char *s, unsigned *w, unsigned *h) {
    char *end;
    unsigned long a = strtoul(s, &end, 10);
    if (end == s || (*end != 'x' && *end != 'X')) return 0;
    unsigned long b = strtoul(end + 1, &end, 10);
    if (!a || !b || a > 32768 || b > 32768) return 0;
    *w = (unsigned)a;
    *h = (unsigned)b;
    return 1;
}

/* The output size the frame is shown at: BB_ULTRAWIDE=WxH, else the screen in fullscreen, else
 * the window (BB_WINDOW_SIZE, as the launcher sets them). */
static int output_size(unsigned *w, unsigned *h) {
    const char *mode = getenv("BB_ULTRAWIDE");
    if (mode && parse_size(mode, w, h)) return 1;
    const char *fullscreen = getenv("BB_FULLSCREEN");
    if (fullscreen && fullscreen[0] == '1') {
#ifdef _WIN32
        DEVMODEW dm;
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth && dm.dmPelsHeight) {
            *w = dm.dmPelsWidth;
            *h = dm.dmPelsHeight;
            return 1;
        }
#endif
    }
    const char *window = getenv("BB_WINDOW_SIZE");
    return window && parse_size(window, w, h);
}

static int render_resolution(const uint8_t *code, uint64_t size, uint64_t image_end, unsigned *width,
                             unsigned *height);

void runtime_patch_ultrawide(uint8_t *code, uint64_t size, uint64_t image_end) {
    const char *mode = getenv("BB_ULTRAWIDE");
    const int off = mode && mode[0] == '0' && mode[1] == 0;
    /* BB_RENDER_RES (runtime_patch_resolution): the game takes its camera's aspect from the render
     * buffer, which keeps the horizontal view and crops the vertical one when the buffer is wider
     * than 16:9. The inverse aspect is scaled back by c = buffer aspect / (16/9) on every frame,
     * so the view is the 16:9 one stretched over the buffer like the UI (the presenter shows
     * those frames at 16:9) before any widening. */
    float c = 1.0f;
    unsigned render_w, render_h;
    if (render_resolution(code, size, image_end, &render_w, &render_h)) {
        c = (float)render_w / (float)render_h / (16.0f / 9.0f);
        if (c < 1.0f) c = 1.0f; /* narrower buffers: not handled (letterboxed by the game?) */
    }
    unsigned width = 1920, height = 1080;
    if (off || !output_size(&width, &height)) {
        if (c == 1.0f) return;
        width = 1920;
        height = 1080;
    }
    const char *fov_env = getenv("BB_FOV");
    unsigned fov_pct = fov_env ? (unsigned)strtoul(fov_env, NULL, 10) : 100;
    if (fov_pct < 60 || fov_pct > 150) fov_pct = 100;
    const float aspect = (float)width / (float)height;
    const int wide = aspect > 16.0f / 9.0f + 0.01f;
    if (!wide && fov_pct == 100 && c == 1.0f) return;
    const float k = wide ? (16.0f / 9.0f) / aspect : 1.0f, fov = fov_pct / 100.0f;

    uint64_t persp[MAX_ANCHORS], ortho[MAX_ANCHORS];
    int np = string_refs(code, size, "CreatePerspectiveProjectionZoom", persp, MAX_ANCHORS / 2);
    np += string_refs(code, size, "CreatePerspectiveProjectionZoomInfiniteFar", persp + np, MAX_ANCHORS / 2);
    const int no = string_refs(code, size, "CreateOrthographicProjection", ortho, MAX_ANCHORS);
    uint32_t field;
    if (!np || !camera_field(code, size, persp, np, &field)) {
        printf("Ultrawide: camera projection not found, the view stays 16:9\n");
        return;
    }

    /* Every [reg + disp32] scalar read of F - 8, F and F + 4. */
    static Read reads[MAX_READS];
    int nr = 0;
    for (uint64_t e = 4; e + 4 <= size; ++e) {
        uint32_t v;
        memcpy(&v, code + e, 4);
        if (v != field - 8 && v != field && v != field + 4) continue;
        for (unsigned len = 7; len <= 10; ++len) {
            ScalarOp op;
            if (e + 4 < len) continue;
            if (decode_scalar(code, size, e + 4 - len, &op) && op.len == len && (uint32_t)op.disp == v) {
                if (nr == MAX_READS) { printf("Ultrawide: too many camera reads, not applied\n"); return; }
                reads[nr].at = e + 4 - len;
                reads[nr].op = op;
                ++nr;
            }
        }
    }
    /* Camera reads only: not the stack frame, not a getter (read then jmp/ret), not next to a
     * read of F - 8 from the same register (another structure), not in the orthographic builder. */
    static Read kept[MAX_READS];
    int nk = 0;
    for (int i = 0; i < nr; ++i) {
        const Read *r = &reads[i];
        if ((uint32_t)r->op.disp == field - 8 || r->op.base == 4 || r->op.base == 5) continue;
        const uint8_t next = code[r->at + r->op.len];
        if (next == 0xeb || next == 0xe9 || next == 0xc3) continue;
        int other = 0;
        for (int j = 0; j < nr && !other; ++j)
            other = (uint32_t)reads[j].op.disp == field - 8 && reads[j].op.op == 0x10 &&
                    reads[j].op.base == r->op.base && distance(reads[j].at, r->at) < 0x40;
        if (other) continue;
        uint64_t nearest = UINT64_MAX;
        int in_ortho = 0;
        for (int a = 0; a < np; ++a)
            if (persp[a] >= r->at && persp[a] - r->at < 0x200 && persp[a] - r->at < nearest) { nearest = persp[a] - r->at; in_ortho = 0; }
        for (int a = 0; a < no; ++a)
            if (ortho[a] >= r->at && ortho[a] - r->at < 0x200 && ortho[a] - r->at < nearest) { nearest = ortho[a] - r->at; in_ortho = 1; }
        if (!in_ortho) kept[nk++] = *r;
    }
    /* Sites: reads with a read of the other field from the same register close by. */
    static Read sites[MAX_SITES];
    int ns = 0;
    for (int i = 0; i < nk; ++i) {
        const uint32_t other = (uint32_t)kept[i].op.disp == field ? field + 4 : field;
        int paired = 0;
        for (int j = 0; j < nk && !paired; ++j)
            paired = j != i && (uint32_t)kept[j].op.disp == other && kept[j].op.base == kept[i].op.base &&
                     distance(kept[j].at, kept[i].at) < 0x130;
        if (!paired) continue;
        if (ns == MAX_SITES || (ns && sites[ns - 1].at + sites[ns - 1].op.len > kept[i].at)) {
            printf("Ultrawide: unexpected camera code, not applied\n");
            return;
        }
        sites[ns++] = kept[i];
    }
    if (ns < 4) {
        printf("Ultrawide: only %d camera reads found, not applied\n", ns);
        return;
    }
    /* Room for the stubs: the zero padding at the end of the code segment. */
    uint64_t pad = size;
    while (pad > 0 && code[pad - 1] == 0) --pad;
    const uint64_t cave = (pad + 0x100 + 0xff) & ~(uint64_t)0xff;
    if (size - pad < 8192 || cave + 0x20 + (uint64_t)ns * 0x20 > size) {
        printf("Ultrawide: no room at the end of the game code, not applied\n");
        return;
    }
    /* Constants: k, 1/k (after a division), k/fov, fov/k. They live in a writable block near the
     * code so the presenter can switch them per frame (runtime_ultrawide_select): menus, title
     * screens and loading get the game's own view (all 1), gameplay the widened one. Without such
     * a block they stay in the padding, always widened. */
    wide_constants[0] = k * c;
    wide_constants[1] = 1.0f / (k * c);
    narrow_constants[0] = c;
    narrow_constants[1] = 1.0f / c;
    wide_constants[2] = k / fov;
    wide_constants[3] = fov / k;
    float *constants = alloc_near(code, size);
    if (constants) {
        for (int i = 0; i < 4; ++i) constants[i] = narrow_constants[i];
    } else {
        constants = (float *)(code + cave);
        memcpy(constants, wide_constants, sizeof(wide_constants));
    }
    static const uint8_t nops[6][5] = {{0}, {0x90}, {0x66, 0x90}, {0x0f, 0x1f, 0x00}, {0x0f, 0x1f, 0x40, 0x00},
                                       {0x0f, 0x1f, 0x44, 0x00, 0x00}};
    for (int s = 0; s < ns; ++s) {
        const uint64_t at = sites[s].at, stub = cave + 0x20 + (uint64_t)s * 0x20;
        const uint8_t len = sites[s].op.len, reg = sites[s].op.reg;
        uint8_t *c = code + stub;
        memcpy(c, code + at, len);
        c += len;
        const float *constant =
            constants + ((uint32_t)sites[s].op.disp == field ? 0 : 2) + (sites[s].op.op == 0x5e ? 1 : 0);
        /* vmulss xmm(reg), xmm(reg), [rip + constant] */
        c[0] = 0xc5;
        c[1] = (uint8_t)((reg < 8 ? 0x80 : 0) | ((~reg & 15) << 3) | 2);
        c[2] = 0x59;
        c[3] = (uint8_t)(((reg & 7) << 3) | 5);
        int32_t rel = (int32_t)((intptr_t)constant - (intptr_t)(c + 8));
        memcpy(c + 4, &rel, 4);
        c += 8;
        c[0] = 0xe9; /* jmp back */
        rel = (int32_t)((int64_t)(at + len) - (int64_t)(c - code + 5));
        memcpy(c + 1, &rel, 4);
        uint8_t *site = code + at;
        site[0] = 0xe9; /* jmp stub */
        rel = (int32_t)((int64_t)stub - (int64_t)(at + 5));
        memcpy(site + 1, &rel, 4);
        memcpy(site + 5, nops[len - 5], len - 5u);
    }
    runtime_ultrawide_fill = wide;
    runtime_ultrawide_scale = k;
    if (constants != (float *)(code + cave)) switch_constants = constants;
    printf("Ultrawide: %d camera reads widened for %ux%u (field %#x), field of view %u%%%s, render aspect "
           "correction %.3f\n",
           ns, width, height, field, fov_pct, switch_constants ? ", gameplay only" : "", c);
}

/* Render resolution (BB_RENDER_RES=WIDTHxHEIGHT, U3 for now): the engine's display mode table
 * (one setter per mode: 1280x720, 1920x1080, 1024x600, 1168x657) fills a global display config:
 * the render size three times (+0x08, +0x18, +0x24, with the 16:9 aspect at +0x20/+0x2c), the
 * output size (+0x38), an eighth/quarter size (+0x40/+0x44), and passes the size as floats to the
 * view setup. The 1920x1080 mode and the config's constructors get the new size; the aspect
 * stays 16:9, so the image is widened by the ultrawide camera patch like the 1080p one was and
 * the menus and the UI keep their layout. */
static int match_mode_block(const uint8_t *p, uint8_t m) {
    static const uint8_t fields[12] = {0x08, 0x0c, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x10, 0x40, 0x44};
    static const uint32_t values[12] = {1920, 1080, 0, 1920, 1080, 0x3fe38e39, 1920, 1080, 0x3fe38e39,
                                        0x3f800000, 0xf0, 0x10e};
    for (int i = 0; i < 12; ++i) {
        const uint8_t *q = p + 7 * i;
        if (q[0] != 0xc7 || q[1] != m || q[2] != fields[i] || memcmp(q + 3, &values[i], 4)) return 0;
    }
    return 1;
}

/* The game's memory table entries ({id, parent, size, alignment}): the render target heaps
 * (0x84, 0x85 under the GPU context), the GPU context's total (entry 3, 3052 MiB) and its free
 * entry (0x0e, ALLOCATION_APP_GPU_FREE, 208.7 MiB, alignment 1). */
static const uint8_t heap_a[24] = {0x84, 0, 0, 0, 6, 0, 0, 0, 0x00, 0x40, 0xbc, 0x08, 0, 0, 0, 0,
                                   0x80, 0,    0, 0, 0, 0, 0, 0};
static const uint8_t heap_b[24] = {0x85, 0, 0, 0, 6, 0, 0, 0, 0x00, 0xbc, 0x10, 0x23, 0, 0, 0, 0,
                                   0x80, 0,    0, 0, 0, 0, 0, 0};
static const uint8_t gpu_total[24] = {3, 0, 0, 0, 2, 0, 0, 0, 0x00, 0x00, 0xc0, 0xbe, 0, 0, 0, 0,
                                      0x00, 0x00, 0x10, 0, 0, 0, 0, 0};
static const uint8_t gpu_free[24] = {0x0e, 0, 0, 0, 6, 0, 0, 0, 0x00, 0x04, 0x0b, 0x0d, 0, 0, 0, 0,
                                     1,    0, 0, 0, 0, 0, 0, 0};
/* The direct memory context's check: movabs rdx, 4608 MiB; xor ecx, ecx; mov rdi, rbx; call. */
static const uint8_t direct_check[16] = {0x48, 0xba, 0, 0, 0, 0x20, 1, 0, 0, 0, 0x31, 0xc9, 0x48, 0x89, 0xdf, 0xe8};
/* The movie textures' sizes in the render target setup and the movie's chroma offset (below). */
static const uint8_t movie_first[7] = {0x44, 0x89, 0xf9, 0x4c, 0x89, 0xc3, 0xe8};
static const uint8_t movie_second[15] = {0x48, 0x8b, 0x8d, 0x18, 0xfd, 0xff, 0xff, 0x4c,
                                         0x8b, 0x85, 0x10, 0xfd, 0xff, 0xff, 0xe8};
static const uint8_t movie_chroma[11] = {0x41, 0x0f, 0xaf, 0xde, 0x83, 0xe3, 0xc0, 0x48, 0x03, 0x5d, 0x80};

/* The size BB_RENDER_RES asks for, when the loaded game is one the patch knows (U3: every piece it
 * changes is found, once where it must be unique); otherwise nothing is changed at all. */
static int render_resolution(const uint8_t *code, uint64_t size, uint64_t image_end, unsigned *width,
                             unsigned *height) {
    static int known = -1;
    static unsigned w, h;
    if (known < 0) {
        known = 0;
        const char *env = getenv("BB_RENDER_RES");
        if (!env || !parse_size(env, &w, &h) || w < 640 || h < 360 || w > 7680 || h > 4320) return 0;
        w &= ~15u; /* the game asserts displaySize.m_w % 16 == 0 */
        h &= ~3u;
        if (w == 1920 && h == 1080) return 0;
        int blocks = 0, direct = 0, first = 0, second = 0, chroma = 0, entries = 0;
        for (uint64_t i = 0; i + 200 < size; ++i) {
            const uint8_t *q = code + i;
            if (q[0] == 0xc7 && q[2] == 0x08 && (q[1] == 0x47 || q[1] == 0x40) && match_mode_block(q, q[1])) ++blocks;
            if (q[0] == 0x48 && !memcmp(q, direct_check, 16)) ++direct;
            if (q[0] == 0x44 && !memcmp(q, movie_first, 7)) ++first;
            if (q[0] == 0x48 && !memcmp(q, movie_second, 15)) ++second;
            if (q[0] == 0x41 && !memcmp(q, movie_chroma, 11)) ++chroma;
        }
        const uint8_t *wanted[4] = {heap_a, heap_b, gpu_total, gpu_free};
        for (uint64_t i = size & ~(uint64_t)7; i + 24 <= image_end; i += 8)
            for (int e = 0; e < 4; ++e)
                if (!memcmp(code + i, wanted[e], 24)) entries |= 1 << e;
        if (!blocks || direct != 1 || first != 1 || second != 1 || chroma != 1 || entries != 15) {
            printf("Render resolution: BB_RENDER_RES is for Uncharted 3, this game is left at 1920x1080 "
                   "(%d %d %d %d %d %#x)\n", blocks, direct, first, second, chroma, entries);
            return 0;
        }
        known = 1;
    }
    if (known && width) {
        *width = w;
        *height = h;
    }
    return known;
}

void runtime_patch_resolution(uint8_t *code, uint64_t size, uint64_t image_end) {
    unsigned width, height;
    if (!render_resolution(code, size, image_end, &width, &height)) return;
    float *floats = NULL;
    int blocks = 0, sizes = 0, loads = 0, limits = 0;
    for (uint64_t i = 0; i + 200 < size; ++i) {
        if (code[i] != 0xc7 || code[i + 2] != 0x08) continue;
        const uint8_t m = code[i + 1];
        if ((m != 0x47 && m != 0x40) || !match_mode_block(code + i, m)) continue;
        uint8_t *p = code + i;
        const uint32_t w8 = width / 8, h4 = height / 4;
        memcpy(p + 7 * 0 + 3, &width, 4);
        memcpy(p + 7 * 1 + 3, &height, 4);
        memcpy(p + 7 * 3 + 3, &width, 4);
        memcpy(p + 7 * 4 + 3, &height, 4);
        memcpy(p + 7 * 6 + 3, &width, 4);
        memcpy(p + 7 * 7 + 3, &height, 4);
        memcpy(p + 7 * 10 + 3, &w8, 4);
        memcpy(p + 7 * 11 + 3, &h4, 4);
        ++blocks;
        /* The output size after it: movabs rax, (1080 << 32) | 1920, or two dword stores. */
        static const uint8_t movabs[10] = {0x48, 0xb8, 0x80, 0x07, 0, 0, 0x38, 0x04, 0, 0};
        const uint64_t packed = ((uint64_t)height << 32) | width;
        for (int j = 84; j < 140; ++j) {
            uint8_t *q = p + j;
            if (!memcmp(q, movabs, 10)) {
                memcpy(q + 2, &packed, 8);
                ++sizes;
                break;
            }
            if (q[0] == 0xc7 && q[1] == m && q[2] == 0x38 && q[7] == 0xc7 && q[8] == m && q[9] == 0x3c) {
                uint32_t a, b;
                memcpy(&a, q + 3, 4);
                memcpy(&b, q + 10, 4);
                if (a == 1920 && b == 1080) {
                    memcpy(q + 3, &width, 4);
                    memcpy(q + 10, &height, 4);
                    ++sizes;
                    break;
                }
            }
        }
        /* The view setup's float arguments: vmovss xmm0/xmm1/xmm2, [rip + ...] then jmp. */
        for (int j = 84; j < 200; ++j) {
            uint8_t *q = p + j;
            if (q[0] == 0xc5 && q[1] == 0xfa && q[2] == 0x10 && q[3] == 0x05 && !memcmp(q + 8, "\xc5\xfa\x10\x0d", 4) &&
                !memcmp(q + 16, "\xc5\xfa\x10\x15", 4) && q[24] == 0xe9) {
                int32_t d;
                memcpy(&d, q + 4, 4);
                const float *was_w = (const float *)(q + 8 + d);
                memcpy(&d, q + 12, 4);
                const float *was_h = (const float *)(q + 16 + d);
                if (*was_w != 1920.0f || *was_h != 1080.0f) break;
                if (!floats && !(floats = alloc_near(code, size))) break;
                floats[0] = (float)width;
                floats[1] = (float)height;
                int32_t rel = (int32_t)((intptr_t)&floats[0] - (intptr_t)(q + 8));
                memcpy(q + 4, &rel, 4);
                rel = (int32_t)((intptr_t)&floats[1] - (intptr_t)(q + 16));
                memcpy(q + 12, &rel, 4);
                ++loads;
                break;
            }
        }
    }
    /* Movies: the decoder writes 1920x1080 NV12 (pitch 1920, chroma after 1088 rows), which a
     * compute pass converts over the whole screen. The render target setup creates the movie's
     * luma and chroma textures (two sets) from the render size instead, and the movie renderer
     * puts the chroma at width * align64(height) of it; both get the movie's size:
     *  - mov ecx, r15d; mov rbx, r8 (first luma; r15/rbx keep the render size, halved after it
     *    for the chroma and kept for other half-resolution targets: particles) jumps to a stub
     *    passing ecx = 1920, r8d = 1080;
     *  - mov ecx, r15d; mov r8d, ebx (the two chroma textures, 0x57 and 0xe5 bytes further) jump
     *    to stubs passing ecx = 960, r8d = 540;
     *  - mov rcx, [rbp - 0x2e8]; mov r8, [rbp - 0x2f0] (second luma) become mov ecx, 1920;
     *    mov r8d, 1080;
     *  - imul ebx, r14d; and ebx, -64 (chroma offset) becomes mov ebx, 1920 * 1088. */
    int movies = 0;
    {
        int64_t at_first = -1, at_second = -1, at_chroma = -1;
        int n_first = 0, n_second = 0, n_chroma = 0;
        for (uint64_t i = 0; i + 16 < size; ++i) {
            if (code[i] == 0x44 && !memcmp(code + i, movie_first, 7)) at_first = (int64_t)i, ++n_first;
            if (code[i] == 0x48 && !memcmp(code + i, movie_second, 15)) at_second = (int64_t)i, ++n_second;
            if (code[i] == 0x41 && !memcmp(code + i, movie_chroma, 11)) at_chroma = (int64_t)i, ++n_chroma;
        }
        /* The stubs: the last 0x80 bytes of the code segment's zero padding, 0x20 each. */
        const uint64_t stubs = (size - 0x80) & ~(uint64_t)0xf;
        int room = 1;
        for (uint64_t i = stubs; i < stubs + 0x60; ++i) room &= code[i] == 0;
        static const uint8_t chroma_size[6] = {0x44, 0x89, 0xf9, 0x41, 0x89, 0xd8}; /* mov ecx, r15d; mov r8d, ebx */
        const int64_t at_chroma1 = at_first + 0x57, at_chroma2 = at_first + 0xe5;
        if (n_first == 1 && n_second == 1 && n_chroma == 1 && room && at_first + 0xf0 < (int64_t)size &&
            !memcmp(code + at_chroma1, chroma_size, 6) && !memcmp(code + at_chroma2, chroma_size, 6)) {
            /* A stub at `stub` running `body`, then jumping back to after the 6 bytes at `site`. */
            static const uint8_t luma[14] = {0xb9, 0x80, 0x07, 0, 0,       /* mov ecx, 1920 */
                                             0x4c, 0x89, 0xc3,             /* mov rbx, r8 */
                                             0x41, 0xb8, 0x38, 0x04, 0, 0}; /* mov r8d, 1080 */
            static const uint8_t chroma_args[11] = {0xb9, 0xc0, 0x03, 0, 0,       /* mov ecx, 960 */
                                                    0x41, 0xb8, 0x1c, 0x02, 0, 0}; /* mov r8d, 540 */
            const struct {
                int64_t site;
                const uint8_t *body;
                int len;
            } detours[3] = {{at_first, luma, 14}, {at_chroma1, chroma_args, 11}, {at_chroma2, chroma_args, 11}};
            for (int d = 0; d < 3; ++d) {
                const uint64_t stub = stubs + 0x20 * (uint64_t)d;
                uint8_t *c = code + stub;
                memcpy(c, detours[d].body, (size_t)detours[d].len);
                c[detours[d].len] = 0xe9;
                int32_t rel = (int32_t)((detours[d].site + 6) - (int64_t)(stub + detours[d].len + 5));
                memcpy(c + detours[d].len + 1, &rel, 4);
                uint8_t *site = code + detours[d].site;
                site[0] = 0xe9;
                rel = (int32_t)((int64_t)stub - (detours[d].site + 5));
                memcpy(site + 1, &rel, 4);
                site[5] = 0x90;
            }
            static const uint8_t second_fixed[14] = {0xb9, 0x80, 0x07, 0, 0, 0x66, 0x90,
                                                     0x41, 0xb8, 0x38, 0x04, 0, 0, 0x90};
            memcpy(code + at_second, second_fixed, 14);
            static const uint8_t chroma_fixed[7] = {0xbb, 0x00, 0xe0, 0x1f, 0x00, 0x66, 0x90};
            memcpy(code + at_chroma, chroma_fixed, 7);
            movies = 1;
        }
    }
    /* A size limit set next to it (2560x1216): raised if the new size is larger. */
    for (uint64_t i = 0; i + 14 < size; ++i) {
        uint8_t *q = code + i;
        if (q[0] != 0xc7 || q[2] != 0x30 || q[7] != 0xc7 || q[8] != q[1] || q[9] != 0x34) continue;
        uint32_t a, b;
        memcpy(&a, q + 3, 4);
        memcpy(&b, q + 10, 4);
        if (a != 2560 || b != 1216) continue;
        if (width > a) memcpy(q + 3, &width, 4);
        if (height > b) memcpy(q + 10, &height, 4);
        ++limits;
    }
    /* The render target heaps in the game's memory table ({id, parent, size, alignment}: entries
     * 0x84 and 0x85 under the GPU context, 139.8 and 561.1 MiB), sized for 1080p: grown with the
     * pixel count. The context's entries must add up to its total (3052 MiB, entry 3 of the
     * table above it), which grows by as much, and the direct memory the runtime reports
     * (BB_DMEM_MB, 5056 MiB) with it unless it is set. */
    uint8_t *entries[4] = {NULL, NULL, NULL, NULL};
    const uint8_t *wanted[4] = {heap_a, heap_b, gpu_total, gpu_free};
    for (uint64_t i = size & ~(uint64_t)7; i + 24 <= image_end; i += 8)
        for (int e = 0; e < 4; ++e)
            if (!entries[e] && !memcmp(code + i, wanted[e], 24)) entries[e] = code + i;
    uint64_t grown = 0, heaps[2] = {0, 0}, total = 0, direct_total = 0, free_size = 0;
    const double ratio = (double)width * height / (1920.0 * 1080.0);
    if (entries[0] && entries[1] && entries[2] && entries[3] && ratio > 1.0) {
        /* The engine keeps the GPU context's size in 32 bits (its arena wraps around past 4 GiB),
         * so the total stays below 4096 MiB: the 10% margin shrinks to fit, and growth beyond that
         * comes out of the free entry. Sizes in whole 2 MiB, the unit the areas are allocated in. */
        const uint64_t mib2 = 0x1fffff, cap = 4094ull << 20;
        memcpy(&heaps[0], entries[0] + 8, 8);
        memcpy(&heaps[1], entries[1] + 8, 8);
        memcpy(&total, entries[2] + 8, 8);
        memcpy(&free_size, entries[3] + 8, 8);
        const double need = (heaps[0] + heaps[1]) * (ratio - 1.0);
        const uint64_t room = cap > total ? cap - total : 0;
        double margin = 1.1;
        if (need * margin > (double)room) margin = need > 0 ? (double)room / need : 1.0;
        if (margin < 1.0) margin = 1.0;
        uint64_t grow[2];
        for (int e = 0; e < 2; ++e) {
            grow[e] = ((uint64_t)(heaps[e] * (ratio - 1.0) * margin) + mib2) & ~mib2;
            grown += grow[e];
        }
        uint64_t borrowed = 0;
        if (grown > room) {
            borrowed = grown - room;
            if (borrowed + (4ull << 20) > free_size) { /* does not fit: grow as far as it does */
                const uint64_t short_by = borrowed + (4ull << 20) - free_size;
                borrowed -= short_by < borrowed ? short_by : borrowed;
                grown = room + borrowed;
                const uint64_t a = ((uint64_t)((double)grown * heaps[0] / (heaps[0] + heaps[1]))) & ~mib2;
                grow[0] = a;
                grow[1] = (grown - a) & ~mib2;
                grown = grow[0] + grow[1];
                borrowed = grown > room ? grown - room : 0;
                printf("Render resolution: not enough GPU memory under 4 GiB for this size, the render "
                       "target heaps grow %.0f MiB short\n", (need - (double)grown) / 1048576.0);
            }
            free_size -= borrowed;
            memcpy(entries[3] + 8, &free_size, 8);
        }
        for (int e = 0; e < 2; ++e) {
            heaps[e] += grow[e];
            memcpy(entries[e] + 8, &heaps[e], 8);
        }
        grown -= borrowed; /* the part that enlarges the context */
        total += grown;
        memcpy(entries[2] + 8, &total, 8);
        /* The direct memory context holding it is checked against a constant in the memory map
         * setup: movabs rdx, 4608 MiB; xor ecx, ecx; mov rdi, rbx; call ValidateContext, followed
         * by the next context's mov edx, 512 MiB. */
        for (uint64_t i = 0; i + 64 < size; ++i) {
            if (memcmp(code + i, direct_check, 16)) continue;
            uint64_t expected;
            memcpy(&expected, code + i + 2, 8);
            expected += grown;
            memcpy(code + i + 2, &expected, 8);
            direct_total = expected;
            break;
        }
        if (!getenv("BB_DMEM_MB")) {
            char dmem[48];
            snprintf(dmem, sizeof(dmem), "BB_DMEM_MB=%llu", 5056ull + (grown >> 20) + 256);
#ifdef _WIN32
            _putenv(dmem);
#else
            putenv(strdup(dmem));
#endif
        }
    }
    runtime_render_resolution = blocks > 0;
    if (runtime_render_resolution) {
        runtime_render_width = width;
        runtime_render_height = height;
    }
    printf("Render resolution: %ux%u (%d display modes, %d output sizes, %d view setups, %d limits patched, movies %s, "
           "render target heaps %.1f + %.1f MiB, GPU memory %.1f MiB (%.1f free), direct %.1f MiB, BB_DMEM_MB %s)\n",
           width, height, blocks, sizes, loads, limits, movies ? "1920x1080" : "not patched", heaps[0] / 1048576.0, heaps[1] / 1048576.0,
           total / 1048576.0, free_size / 1048576.0, direct_total / 1048576.0, getenv("BB_DMEM_MB") ? getenv("BB_DMEM_MB") : "unset");
}
