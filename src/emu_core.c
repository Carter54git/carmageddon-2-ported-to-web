/* carma2web: interpreter glue - guest heap, traps, guest calls, PE loader, paths */
#define _GNU_SOURCE
#include "emu.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <sys/mman.h>
#endif

int gEmu_depth;
int gEmu_trace;
int gEmu_profile;

void emu_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void emu_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, " (eip=%08x esp=%08x)\n", gX86.eip, gX86.reg[X86_ESP]);
    gX86.halted = 1;
}

double emu_now_ms(void) {
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

/* ------------------------------------------------------------------ heap */
#ifdef __EMSCRIPTEN__
#include <malloc.h>
void* gmalloc(size_t n) { return malloc(n ? n : 1); }
void gfree(void* p) { free(p); }
void* grealloc(void* p, size_t n) { return realloc(p, n ? n : 1); }
size_t gsize(void* p) { return p ? malloc_usable_size(p) : 0; }
#else
/* Native test host: guest pointers must be < 4GB, so serve guest memory from a
 * fixed low arena with a simple segregated free-list allocator. */
#define ARENA_BASE 0x10000000ul
#define ARENA_SIZE 0xC0000000ul
typedef struct Blk {
    uint32_t size; /* usable size */
    uint32_t magic;
    struct Blk* next_free;
} Blk;
#define HDR 16
static uint8_t* arena_top;
static Blk* small_free[257]; /* by size/16 up to 4096 */
static Blk* big_free;
static void arena_init(void) {
    if (arena_top) return;
    void* p = mmap((void*)ARENA_BASE, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (p != (void*)ARENA_BASE) {
        perror("arena mmap");
        exit(1);
    }
    arena_top = p;
}
void* gmalloc(size_t n) {
    arena_init();
    if (n == 0) n = 1;
    size_t sz = (n + 15) & ~(size_t)15;
    Blk* b = NULL;
    if (sz <= 4096) {
        Blk** l = &small_free[sz / 16];
        if (*l) { b = *l; *l = b->next_free; }
    } else {
        sz = (sz + 4095) & ~(size_t)4095;
        for (Blk** l = &big_free; *l; l = &(*l)->next_free) {
            if ((*l)->size >= sz && (*l)->size <= sz * 2) { b = *l; *l = b->next_free; break; }
        }
    }
    if (b == NULL) {
        b = (Blk*)arena_top;
        arena_top += HDR + sz;
        if ((uintptr_t)arena_top > ARENA_BASE + ARENA_SIZE) { fprintf(stderr, "guest heap exhausted\n"); exit(1); }
        b->size = (uint32_t)sz;
    }
    b->magic = 0xB10CB10C;
    { extern uint32_t gX86_watch; uint32_t p_ = H2G((uint8_t*)b + HDR); if (gX86_watch && p_ <= gX86_watch && gX86_watch < p_ + b->size) fprintf(stderr, "ALLOC %08x size %x (req %zx) eip=%08x ret=%08x\n", p_, b->size, n, gX86.eip, *(uint32_t*)G2H(gX86.reg[4])); }
    return (uint8_t*)b + HDR;
}
void gfree(void* p) {
    if (!p) return;
    Blk* b = (Blk*)((uint8_t*)p - HDR);
    if (b->magic != 0xB10CB10C) { fprintf(stderr, "gfree: bad block %p\n", p); return; }
    b->magic = 0xF4EEF4EE;
    { extern uint32_t gX86_watch; uint32_t p_ = H2G(p); if (gX86_watch && p_ <= gX86_watch && gX86_watch < p_ + b->size) fprintf(stderr, "FREE %08x size %x eip=%08x ret=%08x\n", p_, b->size, gX86.eip, *(uint32_t*)G2H(gX86.reg[4])); }
    if (b->size <= 4096) { b->next_free = small_free[b->size / 16]; small_free[b->size / 16] = b; }
    else { b->next_free = big_free; big_free = b; }
}
size_t gsize(void* p) { return p ? ((Blk*)((uint8_t*)p - HDR))->size : 0; }
void* grealloc(void* p, size_t n) {
    if (!p) return gmalloc(n);
    size_t old = gsize(p);
    if (n <= old) return p;
    void* q = gmalloc(n);
    memcpy(q, p, old);
    gfree(p);
    return q;
}
#endif
void* gcalloc(size_t n) {
    void* p = gmalloc(n);
    memset(p, 0, n);
    return p;
}
char* gstrdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = gmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------------ traps */
typedef struct {
    char dll[32];
    char name[64];
    EmuApiFn fn;
    int nargs;
    uint32_t calls;
    double ms;
} Trap;
#define MAX_TRAPS 4096
static Trap gTraps[MAX_TRAPS];
static int gNum_traps;

typedef struct ApiNode {
    const EmuApiDef* def;
    struct ApiNode* next;
} ApiNode;
static ApiNode* gApis;

void emu_register_apis(const EmuApiDef* defs) {
    for (; defs->name != NULL; defs++) {
        ApiNode* n = malloc(sizeof(*n));
        n->def = defs;
        n->next = gApis;
        gApis = n;
    }
}

uint32_t emu_trap(const char* dll, const char* name, EmuApiFn fn, int nargs) {
    for (int i = 0; i < gNum_traps; i++) {
        if (strcasecmp(gTraps[i].dll, dll) == 0 && strcmp(gTraps[i].name, name) == 0) return EMU_TRAP_BASE + i * EMU_TRAP_STRIDE;
    }
    if (gNum_traps >= MAX_TRAPS) emu_fatal("too many traps");
    Trap* t = &gTraps[gNum_traps];
    snprintf(t->dll, sizeof(t->dll), "%s", dll);
    snprintf(t->name, sizeof(t->name), "%s", name);
    t->fn = fn;
    t->nargs = nargs;
    return EMU_TRAP_BASE + (gNum_traps++) * EMU_TRAP_STRIDE;
}

static int dll_eq(const char* a, const char* b) {
    /* compare ignoring case and a trailing .dll */
    size_t la = strlen(a), lb = strlen(b);
    if (la > 4 && strcasecmp(a + la - 4, ".dll") == 0) la -= 4;
    if (lb > 4 && strcasecmp(b + lb - 4, ".dll") == 0) lb -= 4;
    return la == lb && strncasecmp(a, b, la) == 0;
}

uint32_t emu_resolve_import(const char* dll, const char* name) {
    for (ApiNode* n = gApis; n; n = n->next) {
        if (strcmp(n->def->name, name) == 0 && dll_eq(n->def->dll, dll)) {
            return emu_trap(n->def->dll, name, n->def->fn, n->def->nargs);
        }
    }
    return emu_trap(dll, name, NULL, 0);
}

static void trap_hook(X86Cpu* c, uint32_t target) {
    uint32_t idx = (target - EMU_TRAP_BASE) / EMU_TRAP_STRIDE;
    if (idx >= (uint32_t)gNum_traps) {
        emu_fatal("call to bad trap %08x", target);
        return;
    }
    Trap* t = &gTraps[idx];
    uint32_t ret = x86_pop32(c);
    t->calls++;
    if (t->fn == NULL) {
        emu_fatal("unimplemented API %s!%s (called from %08x)", t->dll, t->name, ret);
        c->eip = ret;
        return;
    }
    if (gEmu_trace) emu_log("API %s(%08x,%08x,%08x,%08x) from %08x", t->name, ((uint32_t*)G2H(c->reg[X86_ESP]))[0],
        ((uint32_t*)G2H(c->reg[X86_ESP]))[1], ((uint32_t*)G2H(c->reg[X86_ESP]))[2], ((uint32_t*)G2H(c->reg[X86_ESP]))[3], ret);
    { extern uint32_t gX86_watch; static uint32_t lastw = 0xdeadbeef;
      if (gX86_watch) { uint32_t v = *(uint32_t*)G2H(gX86_watch); if (v != lastw) { fprintf(stderr, "WATCHAPI %08x=%08x before %s from %08x\n", gX86_watch, v, t->name, ret); lastw = v; } } }
    double t0 = gEmu_profile ? emu_now_ms() : 0;
    uint32_t r = t->fn(c, (uint32_t*)G2H(c->reg[X86_ESP]));
    if (gEmu_profile) t->ms += emu_now_ms() - t0;
    if (t->nargs > 0) c->reg[X86_ESP] += t->nargs * 4;
    c->reg[X86_EAX] = r;
    c->eip = ret;
}

static X86HookFn (*gNative_lookup)(uint32_t target);

static X86HookFn hook_lookup(uint32_t target) {
    if (target >= EMU_TRAP_BASE) {
        if (target == EMU_RET_SENTINEL) return NULL;
        return trap_hook;
    }
    if (gNative_lookup) return gNative_lookup(target);
    return NULL;
}

void emu_set_native_lookup(X86HookFn (*fn)(uint32_t)) { gNative_lookup = fn; }

/* ------------------------------------------------------------------ guest calls */
uint32_t emu_call(uint32_t addr, int n, const uint32_t* args) {
    X86Cpu* c = &gX86;
    uint32_t saved_eip = c->eip;
    uint32_t saved_esp = c->reg[X86_ESP];
    for (int i = n - 1; i >= 0; i--) x86_push32(c, args[i]);
    x86_push32(c, EMU_RET_SENTINEL);
    X86HookFn h = hook_lookup(addr);
    gEmu_depth++;
    if (h) {
        h(c, addr);
    } else {
        c->eip = addr;
    }
    if (c->eip != EMU_RET_SENTINEL && !c->halted) x86_run(c, EMU_RET_SENTINEL);
    gEmu_depth--;
    c->eip = saved_eip;
    c->reg[X86_ESP] = saved_esp;
    return c->reg[X86_EAX];
}

uint32_t emu_call4(uint32_t addr, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t args[4] = { a, b, c, d };
    return emu_call(addr, 4, args);
}

void emu_yield(void) {
    if (gEmu_depth == 0) gX86.yield = 1;
}

/* ------------------------------------------------------------------ PE loader */
static uint8_t* read_whole(const char* path, long* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc(*size);
    if (fread(b, 1, *size, f) != (size_t)*size) { fclose(f); free(b); return NULL; }
    fclose(f);
    return b;
}

#define RD16(p) (*(uint16_t*)(p))
#define RD32(p) (*(uint32_t*)(p))

int emu_load_exe(const char* path, uint32_t* entry) {
    long n;
    uint8_t* b = read_whole(path, &n);
    if (!b) { emu_log("cannot read %s", path); return -1; }
    uint32_t pe = RD32(b + 0x3c);
    uint16_t nsec = RD16(b + pe + 6), optsz = RD16(b + pe + 20);
    uint8_t* opt = b + pe + 24;
    uint32_t image_base = RD32(opt + 28), image_size = RD32(opt + 56), hdr_size = RD32(opt + 60);
    *entry = image_base + RD32(opt + 16);
    uint8_t* img = (uint8_t*)G2H(image_base);
#ifndef __EMSCRIPTEN__
    void* m = mmap(img, image_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m != img) { perror("image mmap"); return -1; }
#endif
    memset(img, 0, image_size);
    memcpy(img, b, hdr_size);
    uint8_t* sh = opt + optsz;
    for (int i = 0; i < nsec; i++, sh += 40) {
        uint32_t va = RD32(sh + 12), rsz = RD32(sh + 16), roff = RD32(sh + 20), vsz = RD32(sh + 8);
        uint32_t cpy = rsz < vsz || vsz == 0 ? rsz : vsz;
        memcpy(img + va, b + roff, cpy);
    }
    /* imports */
    uint32_t imp_rva = RD32(opt + 104);
    int missing = 0;
    for (uint8_t* d = img + imp_rva; RD32(d + 12) != 0; d += 20) {
        const char* dll = (const char*)img + RD32(d + 12);
        uint32_t oft = RD32(d), ft = RD32(d + 16);
        uint32_t* names = (uint32_t*)(img + (oft ? oft : ft));
        uint32_t* iat = (uint32_t*)(img + ft);
        for (int i = 0; names[i] != 0; i++) {
            char nm[64];
            if (names[i] & 0x80000000u) snprintf(nm, sizeof(nm), "#%u", names[i] & 0xffff);
            else snprintf(nm, sizeof(nm), "%s", (const char*)img + names[i] + 2);
            iat[i] = emu_resolve_import(dll, nm);
            if (gTraps[(iat[i] - EMU_TRAP_BASE) / EMU_TRAP_STRIDE].fn == NULL) missing++;
        }
    }
    emu_log("loaded %s: base %08x size %08x entry %08x, %d traps (%d unimplemented)", path, image_base, image_size, *entry, gNum_traps, missing);
    free(b);
    return 0;
}

void emu_dump_api_stats(void) {
    for (int i = 0; i < gNum_traps; i++) {
        if (gTraps[i].calls) emu_log("  %-10s %-32s %8u %8.1fms", gTraps[i].dll, gTraps[i].name, gTraps[i].calls, gTraps[i].ms);
    }
}

/* ------------------------------------------------------------------ paths */
static int ci_lookup(const char* dir, const char* name, char* out, size_t outsz) {
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    struct stat st;
    if (stat(p, &st) == 0) { snprintf(out, outsz, "%s", p); return 1; }
    DIR* d = opendir(dir);
    if (!d) return 0;
    struct dirent* e;
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        if (strcasecmp(e->d_name, name) == 0) {
            snprintf(out, outsz, "%s/%s", dir, e->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    if (!found) snprintf(out, outsz, "%s/%s", dir, name);
    return found;
}

extern char gEmu_cwd[260]; /* DOS-style, relative to root, e.g. "" or "DATA" */

int emu_host_path(const char* dospath, char* out, size_t outsz) {
    char tmp[512], comps[32][128];
    int nc = 0;
    const char* s = dospath;
    int absolute = 0;
    if (isalpha((unsigned char)s[0]) && s[1] == ':') { s += 2; absolute = 1; }
    if (*s == '\\' || *s == '/') absolute = 1;
    if (!absolute && gEmu_cwd[0]) snprintf(tmp, sizeof(tmp), "%s\\%s", gEmu_cwd, s);
    else snprintf(tmp, sizeof(tmp), "%s", s);
    /* absolute paths into the install dir: strip a leading component naming the game dir */
    char* save;
    for (char* tok = strtok_r(tmp, "\\/", &save); tok; tok = strtok_r(NULL, "\\/", &save)) {
        if (strcmp(tok, ".") == 0 || tok[0] == 0) continue;
        if (strcmp(tok, "..") == 0) { if (nc) nc--; continue; }
        if (nc < 32) snprintf(comps[nc++], 128, "%s", tok);
    }
    if (absolute && nc > 0 && (strcasecmp(comps[0], "CARMA2") == 0)) {
        memmove(comps[0], comps[1], sizeof(comps[0]) * (nc - 1));
        nc--;
    }
    char cur[1024];
    snprintf(cur, sizeof(cur), "%s", emu_root());
    int ok = 1;
    for (int i = 0; i < nc; i++) {
        char nx[1024];
        if (!ci_lookup(cur, comps[i], nx, sizeof(nx))) ok = 0;
        snprintf(cur, sizeof(cur), "%s", nx);
    }
    snprintf(out, outsz, "%s", cur);
    return ok;
}

void emu_init(void) {
    gX86_hook_lookup = hook_lookup;
}
