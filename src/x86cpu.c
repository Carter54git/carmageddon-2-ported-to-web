/*
 * carma2web hybrid runtime: i386 + x87 interpreter (see x86cpu.h).
 * Covers the instruction set emitted by MSVC 5 for CARMA2_HW.EXE.
 */
#include "x86cpu.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

X86Cpu gX86;
uint32_t gX86_break;
void (*gX86_break_fn)(X86Cpu*);
uint32_t* gX86_prof;
X86HookFn (*gX86_hook_lookup)(uint32_t target);
void (*gX86_fault)(X86Cpu* cpu, const char* why);

#define MEM8(a) (*(uint8_t*)(uintptr_t)(a))
#define G2H_(a) ((void*)(uintptr_t)(a))
#define MEM16(a) (*(uint16_t*)(uintptr_t)(a))
#define MEM32(a) (*(uint32_t*)(uintptr_t)(a))

static inline uint8_t rd8(uint32_t a) { return MEM8(a); }
static inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, (void*)(uintptr_t)a, 2); return v; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, (void*)(uintptr_t)a, 4); return v; }
uint32_t gX86_watch;
#define WATCHCHK(a, n) do { if (gX86_watch && (a) <= gX86_watch && gX86_watch < (a) + (n)) { uint32_t* sp_ = (uint32_t*)(uintptr_t)gX86.reg[4]; fprintf(stderr, "WRITE %08x by eip=%08x edi=%08x ecx=%08x stack %08x %08x %08x %08x %08x %08x\n", (a), gX86.eip, gX86.reg[7], gX86.reg[1], sp_[0], sp_[1], sp_[2], sp_[3], sp_[4], sp_[5]); } } while (0)
static inline void wr8(uint32_t a, uint8_t v) { WATCHCHK(a, 1); MEM8(a) = v; }
static inline void wr16(uint32_t a, uint16_t v) { WATCHCHK(a, 2); memcpy((void*)(uintptr_t)a, &v, 2); }
static inline void wr32(uint32_t a, uint32_t v) { WATCHCHK(a, 4); memcpy((void*)(uintptr_t)a, &v, 4); }

static int fault(X86Cpu* c, const char* why) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s at eip=%08x op=%02x %02x %02x %02x", why, c->eip,
        rd8(c->eip), rd8(c->eip + 1), rd8(c->eip + 2), rd8(c->eip + 3));
    if (gX86_fault != NULL) {
        gX86_fault(c, buf);
    } else {
        fprintf(stderr, "x86: %s\n", buf);
    }
    c->halted = 1;
    return -1;
}

/* ---------- flags ---------- */

static const uint8_t parity_table[256] = {
#define P2(n) n, n ^ 1, n ^ 1, n
#define P4(n) P2(n), P2(n ^ 1), P2(n ^ 1), P2(n)
#define P6(n) P4(n), P4(n ^ 1), P4(n ^ 1), P4(n)
    P6(1), P6(0), P6(0), P6(1)
};

static inline uint32_t szmask(int size) { return size == 8 ? 0xffu : size == 16 ? 0xffffu : 0xffffffffu; }
static inline uint32_t sbit(int size) { return size == 8 ? 0x80u : size == 16 ? 0x8000u : 0x80000000u; }

static inline void set_szp(X86Cpu* c, uint32_t r, int size) {
    uint32_t f = c->eflags & ~(X86_ZF | X86_SF | X86_PF);
    r &= szmask(size);
    if (r == 0) f |= X86_ZF;
    if (r & sbit(size)) f |= X86_SF;
    if (parity_table[r & 0xff]) f |= X86_PF;
    c->eflags = f;
}

static uint32_t alu(X86Cpu* c, int op, uint32_t a, uint32_t b, int size) {
    uint32_t m = szmask(size), sb = sbit(size), r = 0, cf = 0, of = 0, af = 0;
    uint32_t carry_in;
    a &= m;
    b &= m;
    switch (op) {
    case 0: /* add */
    case 2: /* adc */
        carry_in = (op == 2) ? (c->eflags & X86_CF) : 0;
        r = (a + b + carry_in) & m;
        cf = ((uint64_t)a + b + carry_in) > m;
        of = ((~(a ^ b)) & (a ^ r) & sb) != 0;
        af = ((a ^ b ^ r) & 0x10) != 0;
        break;
    case 3: /* sbb */
    case 5: /* sub */
    case 7: /* cmp */
        carry_in = (op == 3) ? (c->eflags & X86_CF) : 0;
        r = (a - b - carry_in) & m;
        cf = (uint64_t)a < (uint64_t)b + carry_in;
        of = ((a ^ b) & (a ^ r) & sb) != 0;
        af = ((a ^ b ^ r) & 0x10) != 0;
        break;
    case 1: r = a | b; break;
    case 4: r = a & b; break;
    case 6: r = a ^ b; break;
    }
    c->eflags &= ~(X86_CF | X86_OF | X86_AF);
    if (cf) c->eflags |= X86_CF;
    if (of) c->eflags |= X86_OF;
    if (af) c->eflags |= X86_AF;
    set_szp(c, r, size);
    return r;
}

static int cond(X86Cpu* c, int cc) {
    uint32_t f = c->eflags;
    int r;
    switch (cc >> 1) {
    case 0: r = (f & X86_OF) != 0; break;
    case 1: r = (f & X86_CF) != 0; break;
    case 2: r = (f & X86_ZF) != 0; break;
    case 3: r = (f & (X86_CF | X86_ZF)) != 0; break;
    case 4: r = (f & X86_SF) != 0; break;
    case 5: r = (f & X86_PF) != 0; break;
    case 6: r = ((f & X86_SF) != 0) != ((f & X86_OF) != 0); break;
    default: r = (f & X86_ZF) || (((f & X86_SF) != 0) != ((f & X86_OF) != 0)); break;
    }
    return (cc & 1) ? !r : r;
}

/* ---------- decoding ---------- */

typedef struct {
    int mod, reg, rm;
    uint32_t ea;
} ModRM;

static uint32_t gSeg_base; /* fs: override base for the current instruction */

static uint32_t fetch_sib(X86Cpu* c, uint32_t* pc, int mod) {
    uint8_t sib = rd8((*pc)++);
    int scale = sib >> 6, index = (sib >> 3) & 7, base = sib & 7;
    uint32_t a = 0;
    if (base == 5 && mod == 0) {
        a = rd32(*pc);
        *pc += 4;
    } else {
        a = c->reg[base];
    }
    if (index != 4) a += c->reg[index] << scale;
    return a;
}

static void decode_modrm(X86Cpu* c, uint32_t* pc, ModRM* m) {
    uint8_t b = rd8((*pc)++);
    m->mod = b >> 6;
    m->reg = (b >> 3) & 7;
    m->rm = b & 7;
    if (m->mod == 3) {
        m->ea = 0;
        return;
    }
    uint32_t a;
    if (m->rm == 4) {
        a = fetch_sib(c, pc, m->mod);
    } else if (m->rm == 5 && m->mod == 0) {
        a = rd32(*pc);
        *pc += 4;
    } else {
        a = c->reg[m->rm];
    }
    if (m->mod == 1) {
        a += (int8_t)rd8(*pc);
        *pc += 1;
    } else if (m->mod == 2) {
        a += rd32(*pc);
        *pc += 4;
    }
    m->ea = a + gSeg_base;
}

static inline uint8_t reg8(X86Cpu* c, int r) { return r < 4 ? (uint8_t)c->reg[r] : (uint8_t)(c->reg[r - 4] >> 8); }
static inline void setreg8(X86Cpu* c, int r, uint8_t v) {
    if (r < 4) c->reg[r] = (c->reg[r] & 0xffffff00u) | v;
    else c->reg[r - 4] = (c->reg[r - 4] & 0xffff00ffu) | ((uint32_t)v << 8);
}
static inline uint32_t getreg(X86Cpu* c, int r, int size) {
    if (size == 8) return reg8(c, r);
    if (size == 16) return c->reg[r] & 0xffff;
    return c->reg[r];
}
static inline void setreg(X86Cpu* c, int r, uint32_t v, int size) {
    if (size == 8) setreg8(c, r, (uint8_t)v);
    else if (size == 16) c->reg[r] = (c->reg[r] & 0xffff0000u) | (v & 0xffff);
    else c->reg[r] = v;
}
static inline uint32_t rm_read(X86Cpu* c, ModRM* m, int size) {
    if (m->mod == 3) return getreg(c, m->rm, size);
    if (size == 8) return rd8(m->ea);
    if (size == 16) return rd16(m->ea);
    return rd32(m->ea);
}
static inline void rm_write(X86Cpu* c, ModRM* m, uint32_t v, int size) {
    if (m->mod == 3) {
        setreg(c, m->rm, v, size);
        return;
    }
    if (size == 8) wr8(m->ea, (uint8_t)v);
    else if (size == 16) wr16(m->ea, (uint16_t)v);
    else wr32(m->ea, v);
}

/* ---------- shifts ---------- */

static uint32_t shift_op(X86Cpu* c, int op, uint32_t v, unsigned count, int size) {
    uint32_t m = szmask(size), sb = sbit(size);
    unsigned bits = size;
    v &= m;
    count &= 31;
    if (count == 0) return v;
    uint32_t r = v;
    int cf = 0;
    switch (op) {
    case 0: /* rol */
        count %= bits;
        r = count ? ((v << count) | (v >> (bits - count))) & m : v;
        cf = r & 1;
        c->eflags = (c->eflags & ~(X86_CF | X86_OF)) | (cf ? X86_CF : 0) | ((((r & sb) != 0) ^ cf) ? X86_OF : 0);
        return r;
    case 1: /* ror */
        count %= bits;
        r = count ? ((v >> count) | (v << (bits - count))) & m : v;
        cf = (r & sb) != 0;
        c->eflags = (c->eflags & ~(X86_CF | X86_OF)) | (cf ? X86_CF : 0) | ((((r ^ (r << 1)) & sb) != 0) ? X86_OF : 0);
        return r;
    case 2: /* rcl */
    case 3: { /* rcr */
        uint64_t x = v, cfin = (c->eflags & X86_CF) ? 1 : 0;
        count %= (bits + 1);
        for (unsigned i = 0; i < count; i++) {
            if (op == 2) {
                uint64_t out = (x & sb) != 0;
                x = ((x << 1) | cfin) & m;
                cfin = out;
            } else {
                uint64_t out = x & 1;
                x = (x >> 1) | (cfin ? sb : 0);
                cfin = out;
            }
        }
        r = (uint32_t)x;
        c->eflags = (c->eflags & ~X86_CF) | (cfin ? X86_CF : 0);
        return r;
    }
    case 4: /* shl */
    case 6:
        cf = count <= bits ? ((v >> (bits - count)) & 1) : 0;
        r = count < 32 ? (v << count) & m : 0;
        c->eflags = (c->eflags & ~(X86_CF | X86_OF)) | (cf ? X86_CF : 0) | ((((r & sb) != 0) ^ cf) ? X86_OF : 0);
        set_szp(c, r, size);
        return r;
    case 5: /* shr */
        cf = (v >> (count - 1)) & 1;
        r = count < 32 ? v >> count : 0;
        c->eflags = (c->eflags & ~(X86_CF | X86_OF)) | (cf ? X86_CF : 0) | ((v & sb) && count == 1 ? X86_OF : 0);
        set_szp(c, r, size);
        return r;
    case 7: { /* sar */
        int32_t sv = size == 8 ? (int8_t)v : size == 16 ? (int16_t)v : (int32_t)v;
        cf = (sv >> (count - 1)) & 1;
        r = (uint32_t)(sv >> count) & m;
        c->eflags = (c->eflags & ~(X86_CF | X86_OF)) | (cf ? X86_CF : 0);
        set_szp(c, r, size);
        return r;
    }
    }
    return r;
}

/* ---------- lazy flags (produced by compiled code) ---------- */
int (*gX86_jit_dispatch)(X86Cpu* c);
int gX86_native_hooks; /* set when hooks exist inside the guest image (native replacements) */

void x86_materialize(X86Cpu* c) {
    int kind = c->lz_kind, size = c->lz_size;
    uint32_t a = c->lz_a, b = c->lz_b, r = c->lz_r;
    c->lz_kind = 0;
    switch (kind) {
    case X86_LZ_ADD: { uint32_t cf = c->eflags & X86_CF; (void)cf; alu(c, 0, a, b, size); break; }
    case X86_LZ_SUB: alu(c, 5, a, b, size); break;
    case X86_LZ_LOGIC: alu(c, 1, r, 0, size); break;
    case X86_LZ_INC: alu(c, 0, a, 1, size); c->eflags = (c->eflags & ~X86_CF) | (c->lz_cin ? X86_CF : 0); break;
    case X86_LZ_DEC: alu(c, 5, a, 1, size); c->eflags = (c->eflags & ~X86_CF) | (c->lz_cin ? X86_CF : 0); break;
    case X86_LZ_ADC: { c->eflags = (c->eflags & ~X86_CF) | (c->lz_cin ? X86_CF : 0); alu(c, 2, a, b, size); break; }
    case X86_LZ_SBB: { c->eflags = (c->eflags & ~X86_CF) | (c->lz_cin ? X86_CF : 0); alu(c, 3, a, b, size); break; }
    case X86_LZ_SHL: c->eflags = (c->eflags & ~X86_CF) | (c->lz_cin ? X86_CF : 0); shift_op(c, 4, a, b, size); break;
    case X86_LZ_SHR: shift_op(c, 5, a, b, size); break;
    case X86_LZ_SAR: shift_op(c, 7, a, b, size); break;
    case X86_LZ_NEG: alu(c, 5, 0, a, size); if ((a & szmask(size)) == 0) c->eflags &= ~X86_CF; else c->eflags |= X86_CF; break;
    case X86_LZ_MUL: c->eflags = (c->eflags & ~(X86_CF | X86_OF)) | (r ? (X86_CF | X86_OF) : 0); break;
    case X86_LZ_SHD: alu(c, 1, r, 0, size); c->eflags = (c->eflags & ~X86_CF) | (c->lz_cin ? X86_CF : 0); break;
    default: break;
    }
}

/* ---------- x87 ---------- */

/* 64-bit integers do not fit a double exactly. Code that copies memory with
 * fild qword / fistp qword (a common 8-bytes-at-a-time memcpy trick) needs the exact
 * bits back, so remember recent fild m64 results keyed by the double they became. */
#define FILD_CACHE 16
static struct { double d; int64_t q; } gFild_cache[FILD_CACHE];
static unsigned gFild_next;
double x86_fild64(uint32_t a) {
    int64_t q;
    memcpy(&q, (void*)(uintptr_t)a, 8);
    double d = (double)q;
    if ((int64_t)d != q || q == INT64_MIN) {
        gFild_cache[gFild_next].d = d;
        gFild_cache[gFild_next].q = q;
        gFild_next = (gFild_next + 1) % FILD_CACHE;
    }
    return d;
}
int x86_fist64_exact(double d, int64_t* out) {
    /* newest first: neighbouring blocks often round to the same double */
    for (int k = 1; k <= FILD_CACHE; k++) {
        int i = (int)((gFild_next + FILD_CACHE - k) % FILD_CACHE);
        if (memcmp(&gFild_cache[i].d, &d, 8) == 0) { *out = gFild_cache[i].q; return 1; }
    }
    return 0;
}

void x86_fpush(X86Cpu* c, double v) {
    c->top = (c->top - 1) & 7;
    c->st[c->top] = v;
}
double x86_fpop(X86Cpu* c) {
    double v = c->st[c->top];
    c->top = (c->top + 1) & 7;
    return v;
}
#define ST(i) (c->st[(c->top + (i)) & 7])
#define FC0 0x0100
#define FC1 0x0200
#define FC2 0x0400
#define FC3 0x4000

static void fcompare(X86Cpu* c, double a, double b) {
    c->fsw &= ~(FC0 | FC1 | FC2 | FC3);
    if (isnan(a) || isnan(b)) c->fsw |= FC0 | FC2 | FC3;
    else if (a < b) c->fsw |= FC0;
    else if (a == b) c->fsw |= FC3;
}

static double fround(X86Cpu* c, double v) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

static double read_f80(uint32_t a) {
    uint64_t mant = (uint64_t)rd32(a) | ((uint64_t)rd32(a + 4) << 32);
    uint16_t se = rd16(a + 8);
    int sign = se >> 15, exp = se & 0x7fff;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7fff) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}
static void write_f80(uint32_t a, double v) {
    uint64_t mant = 0;
    uint16_t se = 0;
    if (signbit(v)) se = 0x8000;
    v = fabs(v);
    if (v == 0.0) {
    } else if (isinf(v)) {
        se |= 0x7fff;
        mant = 0x8000000000000000ull;
    } else if (isnan(v)) {
        se |= 0x7fff;
        mant = 0xc000000000000000ull;
    } else {
        int e;
        double f = frexp(v, &e); /* v = f * 2^e, 0.5 <= f < 1 */
        mant = (uint64_t)ldexp(f, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    wr32(a, (uint32_t)mant);
    wr32(a + 4, (uint32_t)(mant >> 32));
    wr16(a + 8, se);
}

static int64_t fist_value(X86Cpu* c, double v, int64_t lo, int64_t hi, int64_t indef) {
    double r = fround(c, v);
    if (isnan(r) || r < (double)lo || r > (double)hi) return indef;
    return (int64_t)r;
}

static void farith(X86Cpu* c, int op, double* dst, double src) {
    switch (op) {
    case 0: *dst = *dst + src; break;
    case 1: *dst = *dst * src; break;
    case 4: *dst = *dst - src; break;
    case 5: *dst = src - *dst; break;
    case 6: *dst = *dst / src; break;
    case 7: *dst = src / *dst; break;
    }
}

static int exec_fpu(X86Cpu* c, uint8_t op, uint32_t* pc) {
    ModRM m;
    decode_modrm(c, pc, &m);
    int r = m.reg;
    if (m.mod != 3) {
        uint32_t a = m.ea;
        switch (op) {
        case 0xd8: { float f; memcpy(&f, (void*)(uintptr_t)a, 4);
            if (r == 2 || r == 3) { fcompare(c, ST(0), f); if (r == 3) x86_fpop(c); }
            else farith(c, r, &ST(0), f);
            return 0; }
        case 0xdc: { double d; memcpy(&d, (void*)(uintptr_t)a, 8);
            if (r == 2 || r == 3) { fcompare(c, ST(0), d); if (r == 3) x86_fpop(c); }
            else farith(c, r, &ST(0), d);
            return 0; }
        case 0xda: { double d = (int32_t)rd32(a);
            if (r == 2 || r == 3) { fcompare(c, ST(0), d); if (r == 3) x86_fpop(c); }
            else farith(c, r, &ST(0), d);
            return 0; }
        case 0xde: { double d = (int16_t)rd16(a);
            if (r == 2 || r == 3) { fcompare(c, ST(0), d); if (r == 3) x86_fpop(c); }
            else farith(c, r, &ST(0), d);
            return 0; }
        case 0xd9:
            switch (r) {
            case 0: { float f; memcpy(&f, (void*)(uintptr_t)a, 4); x86_fpush(c, f); return 0; }
            case 2: case 3: { float f = (float)ST(0); memcpy((void*)(uintptr_t)a, &f, 4); if (r == 3) x86_fpop(c); return 0; }
            case 4: c->fcw = rd16(a); c->fsw = rd16(a + 4) & ~0x3800; return 0; /* fldenv (partial) */
            case 5: c->fcw = rd16(a); return 0;
            case 6: memset((void*)(uintptr_t)a, 0, 28); wr16(a, c->fcw); wr16(a + 4, (c->fsw & ~0x3800) | ((c->top & 7) << 11)); wr16(a + 8, 0xffff); return 0;
            case 7: wr16(a, c->fcw); return 0;
            }
            break;
        case 0xdb:
            switch (r) {
            case 0: x86_fpush(c, (int32_t)rd32(a)); return 0;
            case 2: case 3: wr32(a, (uint32_t)(int32_t)fist_value(c, ST(0), INT32_MIN, INT32_MAX, INT32_MIN)); if (r == 3) x86_fpop(c); return 0;
            case 5: x86_fpush(c, read_f80(a)); return 0;
            case 7: write_f80(a, ST(0)); x86_fpop(c); return 0;
            }
            break;
        case 0xdd:
            switch (r) {
            case 0: { double d; memcpy(&d, (void*)(uintptr_t)a, 8); x86_fpush(c, d); return 0; }
            case 2: case 3: { double d = ST(0); memcpy((void*)(uintptr_t)a, &d, 8); if (r == 3) x86_fpop(c); return 0; }
            case 4: { /* frstor */
                c->fcw = rd16(a);
                c->fsw = rd16(a + 4) & ~0x3800;
                c->top = (rd16(a + 4) >> 11) & 7;
                for (int i = 0; i < 8; i++) ST(i) = read_f80(a + 28 + 10 * i);
                return 0; }
            case 6: { /* fnsave */
                memset((void*)(uintptr_t)a, 0, 28);
                wr16(a, c->fcw);
                wr16(a + 4, (c->fsw & ~0x3800) | ((c->top & 7) << 11));
                for (int i = 0; i < 8; i++) write_f80(a + 28 + 10 * i, ST(i));
                c->fcw = 0x37f; c->fsw = 0; c->top = 0;
                return 0; }
            case 7: wr16(a, (c->fsw & ~0x3800) | ((c->top & 7) << 11)); return 0;
            }
            break;
        case 0xdf:
            switch (r) {
            case 0: x86_fpush(c, (int16_t)rd16(a)); return 0;
            case 2: case 3: wr16(a, (uint16_t)(int16_t)fist_value(c, ST(0), INT16_MIN, INT16_MAX, INT16_MIN)); if (r == 3) x86_fpop(c); return 0;
            case 5: x86_fpush(c, x86_fild64(a)); return 0;
            case 7: { int64_t q; if (!x86_fist64_exact(ST(0), &q)) q = fist_value(c, ST(0), INT64_MIN, INT64_MAX, INT64_MIN); memcpy((void*)(uintptr_t)a, &q, 8); x86_fpop(c); return 0; }
            }
            break;
        }
        return fault(c, "unsupported x87 memory op");
    }
    /* register forms */
    int i = m.rm;
    uint8_t modrm = 0xc0 | (r << 3) | i;
    switch (op) {
    case 0xd8:
        if (r == 2 || r == 3) { fcompare(c, ST(0), ST(i)); if (r == 3) x86_fpop(c); }
        else farith(c, r, &ST(0), ST(i));
        return 0;
    case 0xdc:
        if (r == 2 || r == 3) { fcompare(c, ST(0), ST(i)); if (r == 3) x86_fpop(c); return 0; }
        /* st(i) = st(i) op st(0), sub/div direction swapped */
        switch (r) {
        case 0: ST(i) = ST(i) + ST(0); break;
        case 1: ST(i) = ST(i) * ST(0); break;
        case 4: ST(i) = ST(0) - ST(i); break;
        case 5: ST(i) = ST(i) - ST(0); break;
        case 6: ST(i) = ST(0) / ST(i); break;
        case 7: ST(i) = ST(i) / ST(0); break;
        }
        return 0;
    case 0xde:
        if (modrm == 0xd9) { fcompare(c, ST(0), ST(1)); x86_fpop(c); x86_fpop(c); return 0; }
        switch (r) {
        case 0: ST(i) = ST(i) + ST(0); break;
        case 1: ST(i) = ST(i) * ST(0); break;
        case 2: case 3: fcompare(c, ST(0), ST(i)); break;
        case 4: ST(i) = ST(0) - ST(i); break;
        case 5: ST(i) = ST(i) - ST(0); break;
        case 6: ST(i) = ST(0) / ST(i); break;
        case 7: ST(i) = ST(i) / ST(0); break;
        }
        x86_fpop(c);
        return 0;
    case 0xda:
        if (modrm == 0xe9) { fcompare(c, ST(0), ST(1)); x86_fpop(c); x86_fpop(c); return 0; }
        break;
    case 0xdb:
        if (modrm == 0xe2) { c->fsw &= 0x7f00; return 0; }
        if (modrm == 0xe3) { c->fcw = 0x37f; c->fsw = 0; c->top = 0; return 0; }
        if (modrm >= 0xe8 && modrm <= 0xf7) { /* fucomi/fcomi */
            double a = ST(0), b = ST(i);
            c->eflags &= ~(X86_ZF | X86_PF | X86_CF);
            if (isnan(a) || isnan(b)) c->eflags |= X86_ZF | X86_PF | X86_CF;
            else if (a < b) c->eflags |= X86_CF;
            else if (a == b) c->eflags |= X86_ZF;
            return 0;
        }
        break;
    case 0xdd:
        switch (r) {
        case 0: return 0; /* ffree */
        case 2: ST(i) = ST(0); return 0;
        case 3: ST(i) = ST(0); x86_fpop(c); return 0;
        case 4: fcompare(c, ST(0), ST(i)); return 0;
        case 5: fcompare(c, ST(0), ST(i)); x86_fpop(c); return 0;
        }
        break;
    case 0xdf:
        if (modrm == 0xe0) { c->reg[X86_EAX] = (c->reg[X86_EAX] & 0xffff0000u) | (c->fsw & ~0x3800) | ((c->top & 7) << 11); return 0; }
        break;
    case 0xd9:
        if (r == 0) { double v = ST(i); x86_fpush(c, v); return 0; }
        if (r == 1) { double t = ST(0); ST(0) = ST(i); ST(i) = t; return 0; }
        switch (modrm) {
        case 0xd0: return 0;
        case 0xe0: ST(0) = -ST(0); return 0;
        case 0xe1: ST(0) = fabs(ST(0)); return 0;
        case 0xe4: fcompare(c, ST(0), 0.0); return 0;
        case 0xe5: {
            double v = ST(0);
            c->fsw &= ~(FC0 | FC1 | FC2 | FC3);
            if (signbit(v)) c->fsw |= FC1;
            if (isnan(v)) c->fsw |= FC0;
            else if (isinf(v)) c->fsw |= FC0 | FC2;
            else if (v == 0.0) c->fsw |= FC3;
            else if (fpclassify(v) == FP_SUBNORMAL) c->fsw |= FC3 | FC2;
            else c->fsw |= FC2;
            return 0; }
        case 0xe8: x86_fpush(c, 1.0); return 0;
        case 0xe9: x86_fpush(c, 3.321928094887362347870); return 0;
        case 0xea: x86_fpush(c, 1.442695040888963407360); return 0;
        case 0xeb: x86_fpush(c, 3.141592653589793238463); return 0;
        case 0xec: x86_fpush(c, 0.301029995663981195214); return 0;
        case 0xed: x86_fpush(c, 0.693147180559945309417); return 0;
        case 0xee: x86_fpush(c, 0.0); return 0;
        case 0xf0: ST(0) = exp2(ST(0)) - 1.0; return 0;
        case 0xf1: ST(1) = ST(1) * log2(ST(0)); x86_fpop(c); return 0;
        case 0xf2: ST(0) = tan(ST(0)); x86_fpush(c, 1.0); c->fsw &= ~FC2; return 0;
        case 0xf3: ST(1) = atan2(ST(1), ST(0)); x86_fpop(c); return 0;
        case 0xf4: { int e; double f = frexp(ST(0), &e); ST(0) = (double)(e - 1); x86_fpush(c, f * 2.0); return 0; }
        case 0xf5: ST(0) = remainder(ST(0), ST(1)); c->fsw &= ~FC2; return 0;
        case 0xf6: c->top = (c->top - 1) & 7; return 0;
        case 0xf7: c->top = (c->top + 1) & 7; return 0;
        case 0xf8: ST(0) = fmod(ST(0), ST(1)); c->fsw &= ~FC2; return 0;
        case 0xf9: ST(1) = ST(1) * log2(ST(0) + 1.0); x86_fpop(c); return 0;
        case 0xfa: ST(0) = sqrt(ST(0)); return 0;
        case 0xfb: { double v = ST(0); ST(0) = sin(v); x86_fpush(c, cos(v)); c->fsw &= ~FC2; return 0; }
        case 0xfc: ST(0) = fround(c, ST(0)); return 0;
        case 0xfd: ST(0) = ldexp(ST(0), (int)trunc(ST(1))); return 0;
        case 0xfe: ST(0) = sin(ST(0)); c->fsw &= ~FC2; return 0;
        case 0xff: ST(0) = cos(ST(0)); c->fsw &= ~FC2; return 0;
        }
        break;
    }
    return fault(c, "unsupported x87 register op");
}

/* ---------- control transfer ---------- */

static inline int do_call(X86Cpu* c, uint32_t target, uint32_t ret) {
    x86_push32(c, ret);
    if (gX86_hook_lookup != NULL) {
        X86HookFn h = gX86_hook_lookup(target);
        if (h != NULL) {
            h(c, target);
            return 1;
        }
    }
    c->eip = target;
    return 0;
}

static inline void do_jmp(X86Cpu* c, uint32_t target) {
    if (gX86_hook_lookup != NULL) {
        X86HookFn h = gX86_hook_lookup(target);
        if (h != NULL) {
            /* tail call: return address is already on the stack */
            h(c, target);
            return;
        }
    }
    c->eip = target;
}

/* ---------- string ops ---------- */

static int string_op(X86Cpu* c, uint8_t op, int rep, int size) {
    int step = (c->eflags & X86_DF) ? -(size / 8) : (size / 8);
    uint32_t* esi = &c->reg[X86_ESI];
    uint32_t* edi = &c->reg[X86_EDI];
    uint32_t* ecx = &c->reg[X86_ECX];
    uint32_t src_base = gSeg_base;
    if (rep && *ecx == 0) return 0;
    /* fast paths for forward rep movs/stos */
    if (rep && step > 0) {
        uint32_t n = *ecx, bytes = n * (size / 8);
        if (op == 0xa4 || op == 0xa5) {
            uint32_t s0 = *esi + src_base, d0 = *edi;
            if (d0 <= s0 || d0 >= s0 + bytes) {
                memmove(G2H_(d0), G2H_(s0), bytes);
                *esi += bytes; *edi += bytes; *ecx = 0;
                return 0;
            }
        } else if (op == 0xaa && size == 8) {
            memset(G2H_(*edi), (uint8_t)c->reg[X86_EAX], n);
            *edi += n; *ecx = 0;
            return 0;
        } else if (op == 0xab && size == 32) {
            uint32_t v = c->reg[X86_EAX];
            uint32_t* d = (uint32_t*)G2H_(*edi);
            if (((uintptr_t)d & 3) == 0) {
                if (v == 0) memset(d, 0, (size_t)n * 4);
                else for (uint32_t i = 0; i < n; i++) d[i] = v;
            } else {
                for (uint32_t i = 0; i < n; i++) memcpy((uint8_t*)d + i * 4, &v, 4);
            }
            *edi += n * 4; *ecx = 0;
            return 0;
        }
    }
    for (;;) {
        switch (op) {
        case 0xa4: case 0xa5: /* movs */
            if (size == 8) wr8(*edi, rd8(*esi + src_base));
            else if (size == 16) wr16(*edi, rd16(*esi + src_base));
            else wr32(*edi, rd32(*esi + src_base));
            *esi += step; *edi += step;
            break;
        case 0xaa: case 0xab: /* stos */
            if (size == 8) wr8(*edi, (uint8_t)c->reg[X86_EAX]);
            else if (size == 16) wr16(*edi, (uint16_t)c->reg[X86_EAX]);
            else wr32(*edi, c->reg[X86_EAX]);
            *edi += step;
            break;
        case 0xac: case 0xad: /* lods */
            setreg(c, X86_EAX, size == 8 ? rd8(*esi + src_base) : size == 16 ? rd16(*esi + src_base) : rd32(*esi + src_base), size);
            *esi += step;
            break;
        case 0xa6: case 0xa7: { /* cmps */
            uint32_t a = size == 8 ? rd8(*esi + src_base) : size == 16 ? rd16(*esi + src_base) : rd32(*esi + src_base);
            uint32_t b = size == 8 ? rd8(*edi) : size == 16 ? rd16(*edi) : rd32(*edi);
            alu(c, 7, a, b, size);
            *esi += step; *edi += step;
            break; }
        case 0xae: case 0xaf: { /* scas */
            uint32_t b = size == 8 ? rd8(*edi) : size == 16 ? rd16(*edi) : rd32(*edi);
            alu(c, 7, c->reg[X86_EAX], b, size);
            *edi += step;
            break; }
        }
        if (!rep) return 0;
        *ecx -= 1;
        if (*ecx == 0) return 0;
        if (op == 0xa6 || op == 0xa7 || op == 0xae || op == 0xaf) {
            int zf = (c->eflags & X86_ZF) != 0;
            if (rep == 0xf3 && !zf) return 0; /* repe */
            if (rep == 0xf2 && zf) return 0;  /* repne */
        }
    }
}

/* ---------- main loop ---------- */

int x86_run(X86Cpu* c, uint32_t stop_eip) {
    c->halted = 0;
    int blk = !c->single;
    while (c->eip != stop_eip) {
        if (blk) {
            blk = 0;
            if (gX86_jit_dispatch) {
                int jr = gX86_jit_dispatch(c);
                if (jr) {
                    /* compiled code ran and left EIP at the next block; it may be a hooked address */
                    if (c->halted) return -1;
                    if (c->eip == stop_eip) break;
                    if (gX86_hook_lookup && (c->eip >= 0xFFFE0000u || c->eip < 0x400000u || gX86_native_hooks)) {
                        X86HookFn h = gX86_hook_lookup(c->eip);
                        if (h != NULL) {
                            h(c, c->eip);
                            if (c->yield || c->halted) return c->halted ? -1 : 1;
                        }
                    }
                    blk = 1;
                    continue;
                }
            }
        }
        if (c->lz_kind) x86_materialize(c);
        uint32_t pc = c->eip;
        int os16 = 0, rep = 0;
        gSeg_base = 0;
        uint8_t op;
        c->icount++;
        if (c->eip == gX86_break && gX86_break_fn) gX86_break_fn(c);
        if (gX86_prof && (c->icount & 1023) == 0) {
            uint32_t b = (c->eip - 0x400000u) >> 4;
            if (b < (0x400000u >> 4)) gX86_prof[b]++;
        }
    prefix:
        op = rd8(pc++);
        switch (op) {
        case 0x66: os16 = 1; goto prefix;
        case 0xf2: case 0xf3: rep = op; goto prefix;
        case 0x64: gSeg_base = c->fs_base; goto prefix;
        case 0x26: case 0x2e: case 0x36: case 0x3e: case 0x65: goto prefix;
        case 0xf0: goto prefix; /* lock */
        }
        int osz = os16 ? 16 : 32;
        ModRM m;
        switch (op) {
        /* ALU r/m,reg / reg,r/m / acc,imm */
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
        case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d:
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
        case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d:
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25:
        case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
        case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: {
            int aop = op >> 3, form = op & 7;
            int size = (form & 1) ? osz : 8;
            if (form < 4) {
                decode_modrm(c, &pc, &m);
                uint32_t a, b, r;
                if (form < 2) { a = rm_read(c, &m, size); b = getreg(c, m.reg, size); }
                else { a = getreg(c, m.reg, size); b = rm_read(c, &m, size); }
                r = alu(c, aop, a, b, size);
                if (aop != 7) {
                    if (form < 2) rm_write(c, &m, r, size);
                    else setreg(c, m.reg, r, size);
                }
            } else {
                uint32_t imm;
                if (size == 8) { imm = rd8(pc); pc += 1; }
                else if (size == 16) { imm = rd16(pc); pc += 2; }
                else { imm = rd32(pc); pc += 4; }
                uint32_t r = alu(c, aop, getreg(c, X86_EAX, size), imm, size);
                if (aop != 7) setreg(c, X86_EAX, r, size);
            }
            break;
        }
        case 0x27: case 0x2f: case 0x37: case 0x3f: return fault(c, "BCD op");
        case 0x06: case 0x0e: case 0x16: case 0x1e: x86_push32(c, 0x2b); break; /* push sreg */
        case 0x07: case 0x17: case 0x1f: x86_pop32(c); break;                   /* pop sreg */
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: {
            int r = op & 7;
            uint32_t cf = c->eflags & X86_CF;
            uint32_t v = alu(c, op < 0x48 ? 0 : 5, getreg(c, r, osz), 1, osz);
            c->eflags = (c->eflags & ~X86_CF) | cf;
            setreg(c, r, v, osz);
            break;
        }
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
            if (os16) { c->reg[X86_ESP] -= 2; wr16(c->reg[X86_ESP], (uint16_t)c->reg[op & 7]); }
            else { uint32_t v = c->reg[op & 7]; x86_push32(c, v); }
            break;
        case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
            if (os16) { uint16_t v = rd16(c->reg[X86_ESP]); c->reg[X86_ESP] += 2; setreg(c, op & 7, v, 16); }
            else { uint32_t v = x86_pop32(c); c->reg[op & 7] = v; }
            break;
        case 0x60: { /* pushad */
            uint32_t esp = c->reg[X86_ESP];
            for (int r = 0; r < 8; r++) x86_push32(c, r == X86_ESP ? esp : c->reg[r]);
            break; }
        case 0x61: /* popad */
            for (int r = 7; r >= 0; r--) { uint32_t v = x86_pop32(c); if (r != X86_ESP) c->reg[r] = v; }
            break;
        case 0x68: x86_push32(c, os16 ? rd16(pc) : rd32(pc)); pc += os16 ? 2 : 4; break;
        case 0x6a: x86_push32(c, (uint32_t)(int32_t)(int8_t)rd8(pc)); pc += 1; break;
        case 0x69: case 0x6b: {
            decode_modrm(c, &pc, &m);
            int32_t a = os16 ? (int16_t)rm_read(c, &m, 16) : (int32_t)rm_read(c, &m, 32);
            int32_t b;
            if (op == 0x6b) { b = (int8_t)rd8(pc); pc += 1; }
            else if (os16) { b = (int16_t)rd16(pc); pc += 2; }
            else { b = (int32_t)rd32(pc); pc += 4; }
            int64_t p = (int64_t)a * b;
            uint32_t res = (uint32_t)p;
            int ovf = os16 ? (p != (int16_t)p) : (p != (int32_t)p);
            c->eflags = (c->eflags & ~(X86_CF | X86_OF)) | (ovf ? (X86_CF | X86_OF) : 0);
            setreg(c, m.reg, res, osz);
            break; }
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f: {
            int8_t d = (int8_t)rd8(pc);
            pc += 1;
            if (cond(c, op & 0xf)) { pc += d; blk = 1; }
            break; }
        case 0x80: case 0x81: case 0x82: case 0x83: {
            int size = (op & 1) ? osz : 8;
            decode_modrm(c, &pc, &m);
            uint32_t imm;
            if (op == 0x81) { if (os16) { imm = rd16(pc); pc += 2; } else { imm = rd32(pc); pc += 4; } }
            else if (op == 0x83) { imm = (uint32_t)(int32_t)(int8_t)rd8(pc); pc += 1; }
            else { imm = rd8(pc); pc += 1; }
            uint32_t r = alu(c, m.reg, rm_read(c, &m, size), imm, size);
            if (m.reg != 7) rm_write(c, &m, r, size);
            break; }
        case 0x84: case 0x85: {
            int size = (op & 1) ? osz : 8;
            decode_modrm(c, &pc, &m);
            alu(c, 4, rm_read(c, &m, size), getreg(c, m.reg, size), size);
            break; }
        case 0x86: case 0x87: {
            int size = (op & 1) ? osz : 8;
            decode_modrm(c, &pc, &m);
            uint32_t a = rm_read(c, &m, size), b = getreg(c, m.reg, size);
            rm_write(c, &m, b, size);
            setreg(c, m.reg, a, size);
            break; }
        case 0x88: case 0x89: {
            int size = (op & 1) ? osz : 8;
            decode_modrm(c, &pc, &m);
            rm_write(c, &m, getreg(c, m.reg, size), size);
            break; }
        case 0x8a: case 0x8b: {
            int size = (op & 1) ? osz : 8;
            decode_modrm(c, &pc, &m);
            setreg(c, m.reg, rm_read(c, &m, size), size);
            break; }
        case 0x8c: decode_modrm(c, &pc, &m); rm_write(c, &m, 0x2b, 16); break; /* mov r/m, sreg */
        case 0x8e: decode_modrm(c, &pc, &m); break;                            /* mov sreg, r/m */
        case 0x8d: {
            uint32_t sb = gSeg_base;
            gSeg_base = 0;
            decode_modrm(c, &pc, &m);
            gSeg_base = sb;
            setreg(c, m.reg, m.ea, osz);
            break; }
        case 0x8f: {
            uint32_t v = x86_pop32(c);
            decode_modrm(c, &pc, &m);
            rm_write(c, &m, v, osz);
            break; }
        case 0x90: break;
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
            uint32_t a = getreg(c, X86_EAX, osz), b = getreg(c, op & 7, osz);
            setreg(c, X86_EAX, b, osz);
            setreg(c, op & 7, a, osz);
            break; }
        case 0x98:
            if (os16) setreg(c, X86_EAX, (uint16_t)(int16_t)(int8_t)c->reg[X86_EAX], 16);
            else c->reg[X86_EAX] = (uint32_t)(int32_t)(int16_t)c->reg[X86_EAX];
            break;
        case 0x99:
            if (os16) setreg(c, X86_EDX, (c->reg[X86_EAX] & 0x8000) ? 0xffff : 0, 16);
            else c->reg[X86_EDX] = (c->reg[X86_EAX] & 0x80000000u) ? 0xffffffffu : 0;
            break;
        case 0x9b: break; /* fwait */
        case 0xd7: setreg8(c, 0, rd8(c->reg[X86_EBX] + (c->reg[X86_EAX] & 0xff) + gSeg_base)); break; /* xlat */
        case 0x9c: x86_push32(c, c->eflags | 0x2); break;
        case 0x9d: c->eflags = x86_pop32(c); break;
        case 0x9e: c->eflags = (c->eflags & ~0xffu) | ((c->reg[X86_EAX] >> 8) & (X86_SF | X86_ZF | X86_AF | X86_PF | X86_CF)) | 2; break;
        case 0x9f: c->reg[X86_EAX] = (c->reg[X86_EAX] & 0xffff00ffu) | (((c->eflags & 0xd5u) | 2) << 8); break;
        case 0xa0: { uint32_t a = rd32(pc) + gSeg_base; pc += 4; setreg8(c, 0, rd8(a)); break; }
        case 0xa1: { uint32_t a = rd32(pc) + gSeg_base; pc += 4; setreg(c, X86_EAX, os16 ? rd16(a) : rd32(a), osz); break; }
        case 0xa2: { uint32_t a = rd32(pc) + gSeg_base; pc += 4; wr8(a, (uint8_t)c->reg[X86_EAX]); break; }
        case 0xa3: { uint32_t a = rd32(pc) + gSeg_base; pc += 4; if (os16) wr16(a, (uint16_t)c->reg[X86_EAX]); else wr32(a, c->reg[X86_EAX]); break; }
        case 0xa4: case 0xa6: case 0xaa: case 0xac: case 0xae:
            string_op(c, op, rep, 8);
            break;
        case 0xa5: case 0xa7: case 0xab: case 0xad: case 0xaf:
            string_op(c, op, rep, osz);
            break;
        case 0xa8: alu(c, 4, c->reg[X86_EAX], rd8(pc), 8); pc += 1; break;
        case 0xa9:
            if (os16) { alu(c, 4, c->reg[X86_EAX], rd16(pc), 16); pc += 2; }
            else { alu(c, 4, c->reg[X86_EAX], rd32(pc), 32); pc += 4; }
            break;
        case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
            setreg8(c, op & 7, rd8(pc)); pc += 1; break;
        case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
            if (os16) { setreg(c, op & 7, rd16(pc), 16); pc += 2; }
            else { c->reg[op & 7] = rd32(pc); pc += 4; }
            break;
        case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
            int size = (op & 1) ? osz : 8;
            decode_modrm(c, &pc, &m);
            unsigned count;
            if (op <= 0xc1) { count = rd8(pc); pc += 1; }
            else if (op <= 0xd1) count = 1;
            else count = c->reg[X86_ECX] & 0xff;
            uint32_t v = rm_read(c, &m, size);
            rm_write(c, &m, shift_op(c, m.reg, v, count, size), size);
            break; }
        case 0xc2: { uint16_t n = rd16(pc); c->eip = x86_pop32(c); c->reg[X86_ESP] += n; goto ctl; }
        case 0xc3: c->eip = x86_pop32(c); goto ctl;
        case 0xc6: case 0xc7: {
            int size = (op & 1) ? osz : 8;
            decode_modrm(c, &pc, &m);
            uint32_t imm;
            if (size == 8) { imm = rd8(pc); pc += 1; }
            else if (size == 16) { imm = rd16(pc); pc += 2; }
            else { imm = rd32(pc); pc += 4; }
            rm_write(c, &m, imm, size);
            break; }
        case 0xc8: { /* enter */
            uint16_t sz = rd16(pc);
            uint8_t lvl = rd8(pc + 2);
            pc += 3;
            if (lvl != 0) return fault(c, "enter with nesting");
            x86_push32(c, c->reg[X86_EBP]);
            c->reg[X86_EBP] = c->reg[X86_ESP];
            c->reg[X86_ESP] -= sz;
            break; }
        case 0xc9: c->reg[X86_ESP] = c->reg[X86_EBP]; c->reg[X86_EBP] = x86_pop32(c); break;
        case 0xcc: return fault(c, "int3");
        case 0xcd: return fault(c, "int");
        case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf:
            if (exec_fpu(c, op, &pc) != 0) return -1;
            break;
        case 0xe0: case 0xe1: case 0xe2: { /* loopne/loope/loop */
            int8_t d = (int8_t)rd8(pc);
            pc += 1;
            c->reg[X86_ECX] -= 1;
            int zf = (c->eflags & X86_ZF) != 0;
            int take = c->reg[X86_ECX] != 0 && (op == 0xe2 || (op == 0xe1 && zf) || (op == 0xe0 && !zf));
            if (take) { pc += d; blk = 1; }
            break; }
        case 0xe3: { int8_t d = (int8_t)rd8(pc); pc += 1; if (c->reg[X86_ECX] == 0) { pc += d; blk = 1; } break; }
        case 0xe8: {
            int32_t d = (int32_t)rd32(pc);
            pc += 4;
            do_call(c, pc + d, pc);
            if (c->yield || c->halted) return c->halted ? -1 : 1;
            goto ctl; }
        case 0xe9: { int32_t d = (int32_t)rd32(pc); pc += 4; do_jmp(c, pc + d); if (c->yield || c->halted) return c->halted ? -1 : 1; goto ctl; }
        case 0xeb: { int8_t d = (int8_t)rd8(pc); pc += 1; c->eip = pc + d; goto ctl; }
        case 0xf4: return fault(c, "hlt");
        case 0xf5: c->eflags ^= X86_CF; break;
        case 0xf6: case 0xf7: {
            int size = (op & 1) ? osz : 8;
            decode_modrm(c, &pc, &m);
            uint32_t v = rm_read(c, &m, size);
            switch (m.reg) {
            case 0: case 1: {
                uint32_t imm;
                if (size == 8) { imm = rd8(pc); pc += 1; }
                else if (size == 16) { imm = rd16(pc); pc += 2; }
                else { imm = rd32(pc); pc += 4; }
                alu(c, 4, v, imm, size);
                break; }
            case 2: rm_write(c, &m, ~v, size); break;
            case 3: {
                uint32_t r = alu(c, 5, 0, v, size);
                rm_write(c, &m, r, size);
                if ((v & szmask(size)) == 0) c->eflags &= ~X86_CF; else c->eflags |= X86_CF;
                break; }
            case 4: case 5: { /* mul / imul */
                int ovf;
                if (size == 8) {
                    uint16_t r16 = m.reg == 4 ? (uint16_t)((uint8_t)c->reg[X86_EAX] * (uint8_t)v)
                                              : (uint16_t)(int16_t)((int8_t)c->reg[X86_EAX] * (int8_t)v);
                    setreg(c, X86_EAX, r16, 16);
                    ovf = m.reg == 4 ? (r16 >> 8) != 0 : (int16_t)r16 != (int8_t)r16;
                } else if (size == 16) {
                    uint32_t r32 = m.reg == 4 ? (uint32_t)((uint16_t)c->reg[X86_EAX] * (uint16_t)v)
                                              : (uint32_t)((int16_t)c->reg[X86_EAX] * (int16_t)v);
                    setreg(c, X86_EAX, r32 & 0xffff, 16);
                    setreg(c, X86_EDX, r32 >> 16, 16);
                    ovf = m.reg == 4 ? (r32 >> 16) != 0 : (int32_t)r32 != (int16_t)r32;
                } else {
                    uint64_t r64 = m.reg == 4 ? (uint64_t)c->reg[X86_EAX] * v
                                              : (uint64_t)((int64_t)(int32_t)c->reg[X86_EAX] * (int32_t)v);
                    c->reg[X86_EAX] = (uint32_t)r64;
                    c->reg[X86_EDX] = (uint32_t)(r64 >> 32);
                    ovf = m.reg == 4 ? (r64 >> 32) != 0 : (int64_t)r64 != (int32_t)r64;
                }
                c->eflags = (c->eflags & ~(X86_CF | X86_OF)) | (ovf ? (X86_CF | X86_OF) : 0);
                break; }
            case 6: case 7: { /* div / idiv */
                if ((v & szmask(size)) == 0) return fault(c, "divide by zero");
                if (size == 8) {
                    uint16_t a = (uint16_t)c->reg[X86_EAX];
                    if (m.reg == 6) { setreg8(c, 0, (uint8_t)(a / (uint8_t)v)); setreg8(c, 4, (uint8_t)(a % (uint8_t)v)); }
                    else { setreg8(c, 0, (uint8_t)((int16_t)a / (int8_t)v)); setreg8(c, 4, (uint8_t)((int16_t)a % (int8_t)v)); }
                } else if (size == 16) {
                    uint32_t a = (c->reg[X86_EAX] & 0xffff) | ((c->reg[X86_EDX] & 0xffff) << 16);
                    if (m.reg == 6) { setreg(c, X86_EAX, a / (uint16_t)v, 16); setreg(c, X86_EDX, a % (uint16_t)v, 16); }
                    else { setreg(c, X86_EAX, (uint32_t)((int32_t)a / (int16_t)v), 16); setreg(c, X86_EDX, (uint32_t)((int32_t)a % (int16_t)v), 16); }
                } else {
                    uint64_t a = (uint64_t)c->reg[X86_EAX] | ((uint64_t)c->reg[X86_EDX] << 32);
                    if (m.reg == 6) {
                        uint64_t q = a / v;
                        c->reg[X86_EAX] = (uint32_t)q;
                        c->reg[X86_EDX] = (uint32_t)(a % v);
                    } else {
                        int64_t sa = (int64_t)a;
                        int64_t q = sa / (int32_t)v;
                        c->reg[X86_EAX] = (uint32_t)q;
                        c->reg[X86_EDX] = (uint32_t)(sa % (int32_t)v);
                    }
                }
                break; }
            }
            break; }
        case 0xf8: c->eflags &= ~X86_CF; break;
        case 0xf9: c->eflags |= X86_CF; break;
        case 0xfa: case 0xfb: break;
        case 0xfc: c->eflags &= ~X86_DF; break;
        case 0xfd: c->eflags |= X86_DF; break;
        case 0xfe: case 0xff: {
            int size = (op == 0xfe) ? 8 : osz;
            decode_modrm(c, &pc, &m);
            switch (m.reg) {
            case 0: case 1: {
                uint32_t cf = c->eflags & X86_CF;
                uint32_t r = alu(c, m.reg == 0 ? 0 : 5, rm_read(c, &m, size), 1, size);
                c->eflags = (c->eflags & ~X86_CF) | cf;
                rm_write(c, &m, r, size);
                break; }
            case 2: { uint32_t t = rm_read(c, &m, 32); do_call(c, t, pc); if (c->yield || c->halted) return c->halted ? -1 : 1; goto ctl; }
            case 4: { uint32_t t = rm_read(c, &m, 32); do_jmp(c, t); if (c->yield || c->halted) return c->halted ? -1 : 1; goto ctl; }
            case 6: x86_push32(c, rm_read(c, &m, 32)); break;
            default: return fault(c, "bad ff/fe op");
            }
            break; }
        case 0x0f: {
            uint8_t op2 = rd8(pc++);
            if (op2 >= 0x80 && op2 <= 0x8f) {
                int32_t d = (int32_t)rd32(pc);
                pc += 4;
                if (cond(c, op2 & 0xf)) { pc += d; blk = 1; }
                break;
            }
            if (op2 >= 0x90 && op2 <= 0x9f) {
                decode_modrm(c, &pc, &m);
                rm_write(c, &m, cond(c, op2 & 0xf) ? 1 : 0, 8);
                break;
            }
            if (op2 >= 0x40 && op2 <= 0x4f) {
                decode_modrm(c, &pc, &m);
                uint32_t v = rm_read(c, &m, osz);
                if (cond(c, op2 & 0xf)) setreg(c, m.reg, v, osz);
                break;
            }
            if (op2 >= 0xc8 && op2 <= 0xcf) {
                uint32_t v = c->reg[op2 & 7];
                c->reg[op2 & 7] = (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
                break;
            }
            switch (op2) {
            case 0xa0: case 0xa8: x86_push32(c, 0x53); break;
            case 0xa1: case 0xa9: x86_pop32(c); break;
            case 0x31: { /* rdtsc */
                uint64_t t = c->icount * 4;
                c->reg[X86_EAX] = (uint32_t)t;
                c->reg[X86_EDX] = (uint32_t)(t >> 32);
                break; }
            case 0xa2: /* cpuid: pretend Pentium without extensions */
                if (c->reg[X86_EAX] == 0) {
                    c->reg[X86_EAX] = 1;
                    c->reg[X86_EBX] = 0x756e6547; c->reg[X86_EDX] = 0x49656e69; c->reg[X86_ECX] = 0x6c65746e;
                } else {
                    c->reg[X86_EAX] = 0x543; c->reg[X86_EBX] = 0; c->reg[X86_ECX] = 0; c->reg[X86_EDX] = 0x1; /* FPU only */
                }
                break;
            case 0xa3: case 0xab: case 0xb3: case 0xbb: case 0xba: { /* bt/bts/btr/btc */
                decode_modrm(c, &pc, &m);
                int kind;
                uint32_t bit;
                if (op2 == 0xba) { kind = m.reg - 4; bit = rd8(pc); pc += 1; }
                else { kind = op2 == 0xa3 ? 0 : op2 == 0xab ? 1 : op2 == 0xb3 ? 2 : 3; bit = getreg(c, m.reg, osz); }
                uint32_t addr_adj = 0;
                if (m.mod != 3 && op2 != 0xba) {
                    int32_t sbit_ = (int32_t)bit;
                    addr_adj = (uint32_t)((sbit_ >> 5) * 4);
                    bit &= 31;
                    m.ea += addr_adj;
                } else {
                    bit &= osz - 1;
                }
                uint32_t v = rm_read(c, &m, osz);
                int cf = (v >> bit) & 1;
                c->eflags = (c->eflags & ~X86_CF) | (cf ? X86_CF : 0);
                if (kind == 1) rm_write(c, &m, v | (1u << bit), osz);
                else if (kind == 2) rm_write(c, &m, v & ~(1u << bit), osz);
                else if (kind == 3) rm_write(c, &m, v ^ (1u << bit), osz);
                break; }
            case 0xa4: case 0xa5: case 0xac: case 0xad: { /* shld/shrd */
                decode_modrm(c, &pc, &m);
                unsigned count;
                if (op2 == 0xa4 || op2 == 0xac) { count = rd8(pc); pc += 1; }
                else count = c->reg[X86_ECX] & 0xff;
                count &= 31;
                if (count == 0) break;
                uint32_t dst = rm_read(c, &m, osz), src = getreg(c, m.reg, osz), r;
                int cf;
                if (osz == 16) return fault(c, "16-bit shld/shrd");
                if (op2 <= 0xa5) { r = (dst << count) | (src >> (32 - count)); cf = (dst >> (32 - count)) & 1; }
                else { r = (dst >> count) | (src << (32 - count)); cf = (dst >> (count - 1)) & 1; }
                c->eflags = (c->eflags & ~X86_CF) | (cf ? X86_CF : 0);
                set_szp(c, r, 32);
                rm_write(c, &m, r, osz);
                break; }
            case 0xaf: {
                decode_modrm(c, &pc, &m);
                int64_t p = os16 ? (int64_t)(int16_t)getreg(c, m.reg, 16) * (int16_t)rm_read(c, &m, 16)
                                 : (int64_t)(int32_t)c->reg[m.reg] * (int32_t)rm_read(c, &m, 32);
                int ovf = os16 ? (p != (int16_t)p) : (p != (int32_t)p);
                c->eflags = (c->eflags & ~(X86_CF | X86_OF)) | (ovf ? (X86_CF | X86_OF) : 0);
                setreg(c, m.reg, (uint32_t)p, osz);
                break; }
            case 0xb0: case 0xb1: { /* cmpxchg */
                int size = op2 == 0xb0 ? 8 : osz;
                decode_modrm(c, &pc, &m);
                uint32_t v = rm_read(c, &m, size);
                alu(c, 7, getreg(c, X86_EAX, size), v, size);
                if (c->eflags & X86_ZF) rm_write(c, &m, getreg(c, m.reg, size), size);
                else setreg(c, X86_EAX, v, size);
                break; }
            case 0xc0: case 0xc1: { /* xadd */
                int size = op2 == 0xc0 ? 8 : osz;
                decode_modrm(c, &pc, &m);
                uint32_t a = rm_read(c, &m, size), b = getreg(c, m.reg, size);
                uint32_t r = alu(c, 0, a, b, size);
                setreg(c, m.reg, a, size);
                rm_write(c, &m, r, size);
                break; }
            case 0xb6: decode_modrm(c, &pc, &m); setreg(c, m.reg, rm_read(c, &m, 8), osz); break;
            case 0xb7: decode_modrm(c, &pc, &m); setreg(c, m.reg, rm_read(c, &m, 16), osz); break;
            case 0xbe: decode_modrm(c, &pc, &m); setreg(c, m.reg, (uint32_t)(int32_t)(int8_t)rm_read(c, &m, 8), osz); break;
            case 0xbf: decode_modrm(c, &pc, &m); setreg(c, m.reg, (uint32_t)(int32_t)(int16_t)rm_read(c, &m, 16), osz); break;
            case 0xbc: case 0xbd: {
                decode_modrm(c, &pc, &m);
                uint32_t v = rm_read(c, &m, osz);
                if (v == 0) { c->eflags |= X86_ZF; break; }
                c->eflags &= ~X86_ZF;
                int idx = op2 == 0xbc ? __builtin_ctz(v) : 31 - __builtin_clz(v);
                setreg(c, m.reg, (uint32_t)idx, osz);
                break; }
            default:
                c->eip = pc - 2;
                return fault(c, "unsupported 0f opcode");
            }
            break;
        }
        default:
            c->eip = pc - 1;
            return fault(c, "unsupported opcode");
        }
        c->eip = pc;
        if (c->halted) return -1;
        if (c->single) return 0;
        continue;
    ctl:
        blk = 1;
        if (c->single) return 0;
    }
    return c->halted ? -1 : 0;
}

int x86_step_one(X86Cpu* c) {
    c->single = 1;
    int r = x86_run(c, 0xFFFFFFFFu);
    c->single = 0;
    return r;
}
