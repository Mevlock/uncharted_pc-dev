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

/* The widened constants, and the block the stubs read them from when it can be switched. */
static float wide_constants[4];
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
        float value = wide ? wide_constants[i] : 1.0f;
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

void runtime_patch_ultrawide(uint8_t *code, uint64_t size) {
    const char *mode = getenv("BB_ULTRAWIDE");
    if (mode && mode[0] == '0' && mode[1] == 0) return;
    unsigned width = 1920, height = 1080;
    if (!output_size(&width, &height)) return;
    const char *fov_env = getenv("BB_FOV");
    unsigned fov_pct = fov_env ? (unsigned)strtoul(fov_env, NULL, 10) : 100;
    if (fov_pct < 60 || fov_pct > 150) fov_pct = 100;
    const float aspect = (float)width / (float)height;
    const int wide = aspect > 16.0f / 9.0f + 0.01f;
    if (!wide && fov_pct == 100) return;
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
    wide_constants[0] = k;
    wide_constants[1] = 1.0f / k;
    wide_constants[2] = k / fov;
    wide_constants[3] = fov / k;
    float *constants = alloc_near(code, size);
    if (constants) {
        for (int i = 0; i < 4; ++i) constants[i] = 1.0f;
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
    printf("Ultrawide: %d camera reads widened for %ux%u (field %#x), field of view %u%%%s\n", ns,
           width, height, field, fov_pct, switch_constants ? ", gameplay only" : "");
}
