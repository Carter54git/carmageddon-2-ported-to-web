/* carma2web: x86 -> WebAssembly translator for hot guest code.
 *
 * Regions: a set of basic blocks discovered from a hot entry by following direct
 * branches. Each region becomes one wasm function f(entry_block) with all GPRs in
 * locals and a br_table dispatch loop between its blocks. Instructions that are not
 * translated natively are executed by calling back into the interpreter for that
 * single instruction (state is spilled to the X86Cpu struct around the call).
 * Control leaves the region on calls, returns, indirect jumps and branches to
 * addresses outside the region; the interpreter loop then dispatches again.
 *
 * Flags are lazy: the last flag-producing operation is kept in locals and turned
 * into EFLAGS only when needed (see x86_materialize). */
#include "x86cpu.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define G2H(a) ((void*)(uintptr_t)(a))
#define H2G(p) ((uint32_t)(uintptr_t)(p))

int gJit_enabled;
/* direct-mapped cache of compiled entries, also probed by generated code to chain regions */
#define DC_BITS 14
typedef struct { uint32_t eip; int32_t fn; int32_t blk; } DcEnt;
static DcEnt gDc[1 << DC_BITS];
static int32_t gJit_depth;
uint32_t gJit_entries;
#define CHAIN_MAX 24
int gJit_threshold = 100;
int gJit_log;
unsigned gJit_stats_regions, gJit_stats_insns, gJit_stats_fallback, gJit_stats_bytes, gJit_stats_failed;

/* ================================================================== decoder */
typedef struct {
    uint32_t addr, len, next;
    uint8_t op, op2, os16, rep, fs;
    uint8_t has_modrm, mod, reg, rm;
    uint8_t has_sib, base, index, scale;
    int32_t disp;
    uint32_t imm;
    uint32_t imm2;
} Insn;

static inline uint8_t rb(uint32_t a) { return *(uint8_t*)G2H(a); }
static inline uint16_t rw(uint32_t a) { uint16_t v; memcpy(&v, G2H(a), 2); return v; }
static inline uint32_t rd(uint32_t a) { uint32_t v; memcpy(&v, G2H(a), 4); return v; }

/* returns 0 if the opcode is unknown */
static int decode(uint32_t addr, Insn* in) {
    memset(in, 0, sizeof(*in));
    in->addr = addr;
    uint32_t pc = addr;
    uint8_t op;
    for (;;) {
        op = rb(pc++);
        if (op == 0x66) in->os16 = 1;
        else if (op == 0xf2 || op == 0xf3) in->rep = op;
        else if (op == 0x64) in->fs = 1;
        else if (op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e || op == 0x65 || op == 0xf0) {}
        else break;
        if (pc - addr > 6) return 0;
    }
    in->op = op;
    int modrm = 0, imm = 0; /* imm: 1=8, 2=16, 4=32, 5=z(16/32), 6=enter(16+8) */
    int z = in->os16 ? 2 : 4;
    if (op == 0x0f) {
        uint8_t o2 = rb(pc++);
        in->op2 = o2;
        if (o2 >= 0x80 && o2 <= 0x8f) imm = 4;
        else if ((o2 >= 0x90 && o2 <= 0x9f) || (o2 >= 0x40 && o2 <= 0x4f)) modrm = 1;
        else if (o2 >= 0xc8 && o2 <= 0xcf) {}
        else switch (o2) {
            case 0x31: case 0xa2: case 0xa0: case 0xa1: case 0xa8: case 0xa9: break;
            case 0xa3: case 0xab: case 0xb3: case 0xbb: case 0xa5: case 0xad: case 0xaf: case 0xb0: case 0xb1:
            case 0xc0: case 0xc1: case 0xb6: case 0xb7: case 0xbe: case 0xbf: case 0xbc: case 0xbd: modrm = 1; break;
            case 0xba: case 0xa4: case 0xac: modrm = 1; imm = 1; break;
            default: return 0;
        }
    } else if (op < 0x40) {
        int f = op & 7;
        if (f < 4) modrm = 1;
        else if (f == 4) imm = 1;
        else if (f == 5) imm = z;
        /* 6/7: push/pop seg, BCD - no operands */
    } else if (op < 0x60) {
    } else switch (op) {
        case 0x60: case 0x61: break;
        case 0x68: imm = z; break;
        case 0x69: modrm = 1; imm = z; break;
        case 0x6a: imm = 1; break;
        case 0x6b: modrm = 1; imm = 1; break;
        case 0x80: case 0x82: case 0x83: modrm = 1; imm = 1; break;
        case 0x81: modrm = 1; imm = z; break;
        case 0xa0: case 0xa1: case 0xa2: case 0xa3: imm = 4; break;
        case 0xa8: imm = 1; break;
        case 0xa9: imm = z; break;
        case 0xc0: case 0xc1: case 0xc6: modrm = 1; imm = 1; break;
        case 0xc7: modrm = 1; imm = z; break;
        case 0xc2: imm = 2; break;
        case 0xc8: imm = 6; break;
        case 0xcd: imm = 1; break;
        case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xeb: imm = 1; break;
        case 0xe8: case 0xe9: imm = 4; break;
        case 0xf6: case 0xf7: modrm = 1; break; /* imm depends on reg */
        default:
            if (op >= 0x70 && op <= 0x7f) imm = 1;
            else if (op >= 0x84 && op <= 0x8f) modrm = 1;
            else if (op >= 0xb0 && op <= 0xb7) imm = 1;
            else if (op >= 0xb8 && op <= 0xbf) imm = z;
            else if ((op >= 0xd0 && op <= 0xd3) || (op >= 0xd8 && op <= 0xdf) || op == 0xfe || op == 0xff) modrm = 1;
            else if ((op >= 0x90 && op <= 0x99) || (op >= 0x9b && op <= 0x9f) || (op >= 0xa4 && op <= 0xa7) || (op >= 0xaa && op <= 0xaf) ||
                     op == 0xc3 || op == 0xc9 || op == 0xcc || op == 0xd7 || op == 0xf4 || op == 0xf5 || (op >= 0xf8 && op <= 0xfd)) {}
            else return 0;
    }
    if (modrm) {
        uint8_t m = rb(pc++);
        in->has_modrm = 1;
        in->mod = m >> 6;
        in->reg = (m >> 3) & 7;
        in->rm = m & 7;
        if (in->mod != 3) {
            if (in->rm == 4) {
                uint8_t sib = rb(pc++);
                in->has_sib = 1;
                in->scale = sib >> 6;
                in->index = (sib >> 3) & 7;
                in->base = sib & 7;
                if (in->base == 5 && in->mod == 0) { in->disp = (int32_t)rd(pc); pc += 4; in->base = 0xff; }
            } else if (in->rm == 5 && in->mod == 0) {
                in->disp = (int32_t)rd(pc);
                pc += 4;
                in->base = 0xff; /* no base */
            } else {
                in->base = in->rm;
            }
            if (in->mod == 1) { in->disp += (int8_t)rb(pc); pc += 1; }
            else if (in->mod == 2) { in->disp += (int32_t)rd(pc); pc += 4; }
        }
        if ((op == 0xf6 || op == 0xf7) && in->reg < 2) imm = (op == 0xf6) ? 1 : z;
    }
    switch (imm) {
    case 1: in->imm = rb(pc); pc += 1; break;
    case 2: in->imm = rw(pc); pc += 2; break;
    case 4: in->imm = rd(pc); pc += 4; break;
    case 6: in->imm = rw(pc); in->imm2 = rb(pc + 2); pc += 3; break;
    }
    in->len = pc - addr;
    in->next = pc;
    return 1;
}

/* control-flow classification */
enum { CF_NONE, CF_JCC, CF_JMP, CF_CALL, CF_RET, CF_IND_CALL, CF_IND_JMP, CF_STOP };
static int cf_kind(const Insn* in) {
    uint8_t op = in->op;
    if (op >= 0x70 && op <= 0x7f) return CF_JCC;
    if (op == 0x0f && in->op2 >= 0x80 && in->op2 <= 0x8f) return CF_JCC;
    if (op == 0xeb || op == 0xe9) return CF_JMP;
    if (op == 0xe8) return CF_CALL;
    if (op == 0xc3 || op == 0xc2) return CF_RET;
    if (op == 0xff && in->reg == 2) return CF_IND_CALL;
    if (op == 0xff && in->reg == 4) return CF_IND_JMP;
    /* other control transfers / privileged: leave them to the interpreter */
    if (op == 0xe0 || op == 0xe1 || op == 0xe2 || op == 0xe3 || op == 0xcc || op == 0xcd || op == 0xf4 || op == 0xc8 ||
        op == 0x9d || op == 0xcf || op == 0xca || op == 0xcb || op == 0x9a || op == 0xea)
        return CF_STOP;
    if (op == 0xff && (in->reg == 3 || in->reg == 5)) return CF_STOP;
    return CF_NONE;
}
static uint32_t branch_target(const Insn* in) {
    if (in->op >= 0x70 && in->op <= 0x7f) return in->next + (int8_t)in->imm;
    if (in->op == 0xeb) return in->next + (int8_t)in->imm;
    return in->next + (int32_t)in->imm;
}

/* ================================================================== wasm emitter */
typedef struct {
    uint8_t* p;
    size_t n, cap;
} Buf;
static void bput(Buf* b, uint8_t v) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4096;
        b->p = realloc(b->p, b->cap);
    }
    b->p[b->n++] = v;
}
static void bputs(Buf* b, const void* d, size_t n) { for (size_t i = 0; i < n; i++) bput(b, ((const uint8_t*)d)[i]); }
static void uleb(Buf* b, uint32_t v) { do { uint8_t c = v & 0x7f; v >>= 7; if (v) c |= 0x80; bput(b, c); } while (v); }
static void sleb(Buf* b, int64_t v) {
    for (;;) {
        uint8_t c = v & 0x7f;
        v >>= 7;
        if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) { bput(b, c); return; }
        bput(b, c | 0x80);
    }
}

static Buf gCode; /* function body being generated */
#define E(x) bput(&gCode, (uint8_t)(x))

enum {
    W_UNREACHABLE = 0x00, W_BLOCK = 0x02, W_LOOP = 0x03, W_IF = 0x04, W_ELSE = 0x05, W_END = 0x0b, W_BR = 0x0c, W_BR_IF = 0x0d,
    W_BR_TABLE = 0x0e, W_RETURN = 0x0f, W_CALL = 0x10, W_DROP = 0x1a, W_SELECT = 0x1b,
    W_LGET = 0x20, W_LSET = 0x21, W_LTEE = 0x22,
    W_I32_LOAD = 0x28, W_I64_LOAD = 0x29, W_F32_LOAD = 0x2a, W_F64_LOAD = 0x2b, W_I32_LOAD8_S = 0x2c, W_I32_LOAD8_U = 0x2d,
    W_I32_LOAD16_S = 0x2e, W_I32_LOAD16_U = 0x2f,
    W_I32_STORE = 0x36, W_I64_STORE = 0x37, W_F32_STORE = 0x38, W_F64_STORE = 0x39, W_I32_STORE8 = 0x3a, W_I32_STORE16 = 0x3b,
    W_I32_CONST = 0x41, W_I64_CONST = 0x42, W_F64_CONST = 0x44,
    W_I32_EQZ = 0x45, W_I32_EQ = 0x46, W_I32_NE = 0x47, W_I32_LT_S = 0x48, W_I32_LT_U = 0x49, W_I32_GT_S = 0x4a, W_I32_GT_U = 0x4b,
    W_I32_LE_S = 0x4c, W_I32_LE_U = 0x4d, W_I32_GE_S = 0x4e, W_I32_GE_U = 0x4f,
    W_I64_NE = 0x52,
    W_F64_EQ = 0x61, W_F64_NE = 0x62, W_F64_LT = 0x63, W_F64_GT = 0x64,
    W_I32_ADD = 0x6a, W_I32_SUB = 0x6b, W_I32_MUL = 0x6c, W_I32_AND = 0x71, W_I32_OR = 0x72, W_I32_XOR = 0x73, W_I32_SHL = 0x74,
    W_I32_SHR_S = 0x75, W_I32_SHR_U = 0x76, W_I32_ROTL = 0x77, W_I32_ROTR = 0x78, W_I32_POPCNT = 0x69,
    W_I64_ADD = 0x7c, W_I64_MUL = 0x7e, W_I64_SHR_U = 0x88, W_I64_SHR_S = 0x87, W_I64_SHL = 0x86, W_I64_OR = 0x84,
    W_I64_DIV_S = 0x7f, W_I64_DIV_U = 0x80, W_I64_REM_S = 0x81, W_I64_REM_U = 0x82, W_F64_CONVERT_I64_S = 0xb9,
    W_F64_ABS = 0x99, W_F64_NEG = 0x9a, W_F64_SQRT = 0x9f, W_F64_ADD = 0xa0, W_F64_SUB = 0xa1, W_F64_MUL = 0xa2, W_F64_DIV = 0xa3,
    W_I32_WRAP_I64 = 0xa7, W_I64_EXTEND_I32_S = 0xac, W_I64_EXTEND_I32_U = 0xad, W_F32_DEMOTE = 0xb6, W_F64_CONVERT_I32_S = 0xb7,
    W_F64_PROMOTE = 0xbb, W_I32_EXTEND8_S = 0xc0, W_I32_EXTEND16_S = 0xc1,
};
#define T_VOID 0x40
#define T_I32 0x7f

static void e_lget(int l) { E(W_LGET); uleb(&gCode, l); }
static void e_lset(int l) { E(W_LSET); uleb(&gCode, l); }
static void e_ltee(int l) { E(W_LTEE); uleb(&gCode, l); }
static void e_i32(int32_t v) { E(W_I32_CONST); sleb(&gCode, v); }
static void e_i64(int64_t v) { E(W_I64_CONST); sleb(&gCode, v); }
static void e_f64(double v) { E(W_F64_CONST); bputs(&gCode, &v, 8); }
static void e_mem(int op, uint32_t align, uint32_t off) { E(op); uleb(&gCode, align); uleb(&gCode, off); }
static void e_call(int f) { E(W_CALL); uleb(&gCode, f); }

/* locals */
enum {
    L_ENTRY = 0,
    L_R0 = 1, /* 1..8 = eax..edi */
    L_STATE = 9, L_TOP = 10, L_BUDGET = 11, L_LK = 12, L_LA = 13, L_LB = 14, L_LR = 15, L_LSZ = 16, L_LCIN = 17,
    L_EA = 18, L_V = 19, L_A = 20, L_B = 21, L_R = 22, L_T0 = 23, L_T1 = 24, L_IC = 25,
    L_NUM_I32 = 26,
    L_F0 = 26, L_F1 = 27, L_F2 = 28,
    L_FC0 = 29, /* 29..36: cached x87 slots */
    L_NUM_F64 = 11,
    L_Q0 = 37, L_NUM_I64 = 1,
};
#define LREG(r) (L_R0 + (r))

/* imported helper function indices */
enum { H_INTERP = 0, H_MAT = 1, H_FIST = 2, H_FILD64 = 3, H_NUM };

static uint32_t gCpu; /* guest address of gX86 */
#define OFF_REG(r) ((uint32_t)offsetof(X86Cpu, reg) + 4u * (r))
#define OFF_EIP ((uint32_t)offsetof(X86Cpu, eip))
#define OFF_EFLAGS ((uint32_t)offsetof(X86Cpu, eflags))
#define OFF_ST ((uint32_t)offsetof(X86Cpu, st))
#define OFF_TOP ((uint32_t)offsetof(X86Cpu, top))
#define OFF_FSW ((uint32_t)offsetof(X86Cpu, fsw))
#define OFF_FCW ((uint32_t)offsetof(X86Cpu, fcw))
#define OFF_FSBASE ((uint32_t)offsetof(X86Cpu, fs_base))
#define OFF_ICOUNT ((uint32_t)offsetof(X86Cpu, icount))
#define OFF_LZK ((uint32_t)offsetof(X86Cpu, lz_kind))
#define OFF_LZSZ ((uint32_t)offsetof(X86Cpu, lz_size))
#define OFF_LZA ((uint32_t)offsetof(X86Cpu, lz_a))
#define OFF_LZB ((uint32_t)offsetof(X86Cpu, lz_b))
#define OFF_LZR ((uint32_t)offsetof(X86Cpu, lz_r))
#define OFF_LZCIN ((uint32_t)offsetof(X86Cpu, lz_cin))

/* load/store a cpu field */
static void cpu_load32(uint32_t off) { e_i32((int32_t)gCpu); e_mem(W_I32_LOAD, 2, off); }
static void cpu_store32_from(int local, uint32_t off) { e_i32((int32_t)gCpu); e_lget(local); e_mem(W_I32_STORE, 2, off); }

static void spill_all(void) {
    for (int r = 0; r < 8; r++) cpu_store32_from(LREG(r), OFF_REG(r));
    cpu_store32_from(L_TOP, OFF_TOP);
    cpu_store32_from(L_LK, OFF_LZK);
    cpu_store32_from(L_LSZ, OFF_LZSZ);
    cpu_store32_from(L_LA, OFF_LZA);
    cpu_store32_from(L_LB, OFF_LZB);
    cpu_store32_from(L_LR, OFF_LZR);
    cpu_store32_from(L_LCIN, OFF_LZCIN);
}
static void reload_all(void) {
    for (int r = 0; r < 8; r++) { cpu_load32(OFF_REG(r)); e_lset(LREG(r)); }
    cpu_load32(OFF_TOP); e_lset(L_TOP);
    cpu_load32(OFF_LZK); e_lset(L_LK);
    cpu_load32(OFF_LZSZ); e_lset(L_LSZ);
    cpu_load32(OFF_LZA); e_lset(L_LA);
    cpu_load32(OFF_LZB); e_lset(L_LB);
    cpu_load32(OFF_LZR); e_lset(L_LR);
    cpu_load32(OFF_LZCIN); e_lset(L_LCIN);
}
static void flush_icount(void) {
    /* cpu->icount += ic; ic = 0 */
    e_i32((int32_t)gCpu);
    e_i32((int32_t)gCpu); e_mem(W_I64_LOAD, 3, OFF_ICOUNT);
    e_lget(L_IC); E(W_I64_EXTEND_I32_U);
    E(W_I64_ADD);
    e_mem(W_I64_STORE, 3, OFF_ICOUNT);
    e_i32(0); e_lset(L_IC);
}

/* ------------------------------------------------------------------ static flag knowledge */
static int gSk;     /* statically known lazy kind for current point (0 = unknown/dynamic) */
static int gSk_size;

static void set_lazy(int kind, int size) {
    /* L_LA/L_LB/L_LR already hold operands/result */
    e_i32(kind); e_lset(L_LK);
    e_i32(size); e_lset(L_LSZ);
    gSk = kind;
    gSk_size = size;
}

/* materialize flags into cpu->eflags (state spilled before) */
static void materialize(void) {
    /* if (lk) { store lazy; call mat; lk = 0 } */
    e_lget(L_LK);
    E(W_IF); E(T_VOID);
    cpu_store32_from(L_LK, OFF_LZK);
    cpu_store32_from(L_LSZ, OFF_LZSZ);
    cpu_store32_from(L_LA, OFF_LZA);
    cpu_store32_from(L_LB, OFF_LZB);
    cpu_store32_from(L_LR, OFF_LZR);
    cpu_store32_from(L_LCIN, OFF_LZCIN);
    e_call(H_MAT);
    e_i32(0); e_lset(L_LK);
    E(W_END);
    gSk = 0;
}

static uint32_t szmask(int size) { return size == 8 ? 0xff : size == 16 ? 0xffff : 0xffffffffu; }
static void e_sign_of(int local, int size) {
    /* push (local's sign bit for size) != 0 */
    e_lget(local);
    if (size == 32) { e_i32(0); E(W_I32_LT_S); }
    else { e_i32(size == 8 ? 0x80 : 0x8000); E(W_I32_AND); e_i32(0); E(W_I32_NE); }
}
static void e_sext(int local, int size) {
    e_lget(local);
    if (size == 8) E(W_I32_EXTEND8_S);
    else if (size == 16) E(W_I32_EXTEND16_S);
}

/* push condition cc (0..15) as i32 0/1 */
static void e_cond_from_eflags(int cc) {
    int base = cc >> 1, neg = cc & 1;
    switch (base) {
    case 0: cpu_load32(OFF_EFLAGS); e_i32(X86_OF); E(W_I32_AND); break;
    case 1: cpu_load32(OFF_EFLAGS); e_i32(X86_CF); E(W_I32_AND); break;
    case 2: cpu_load32(OFF_EFLAGS); e_i32(X86_ZF); E(W_I32_AND); break;
    case 3: cpu_load32(OFF_EFLAGS); e_i32(X86_CF | X86_ZF); E(W_I32_AND); break;
    case 4: cpu_load32(OFF_EFLAGS); e_i32(X86_SF); E(W_I32_AND); break;
    case 5: cpu_load32(OFF_EFLAGS); e_i32(X86_PF); E(W_I32_AND); break;
    case 6: /* SF != OF */
        cpu_load32(OFF_EFLAGS); e_i32(7); E(W_I32_SHR_U); /* SF at bit 0 */
        cpu_load32(OFF_EFLAGS); e_i32(11); E(W_I32_SHR_U); /* OF at bit 0 */
        E(W_I32_XOR); e_i32(1); E(W_I32_AND);
        break;
    default: /* ZF | SF != OF */
        cpu_load32(OFF_EFLAGS); e_i32(7); E(W_I32_SHR_U);
        cpu_load32(OFF_EFLAGS); e_i32(11); E(W_I32_SHR_U);
        E(W_I32_XOR);
        cpu_load32(OFF_EFLAGS); e_i32(6); E(W_I32_SHR_U);
        E(W_I32_OR); e_i32(1); E(W_I32_AND);
        break;
    }
    if (neg) E(W_I32_EQZ);
    else { e_i32(0); E(W_I32_NE); }
}

static void e_cond(int cc) {
    int base = cc >> 1, neg = cc & 1;
    int k = gSk, sz = gSk_size;
    int ok = 1;
    if (base == 5) ok = 0; /* parity: generic */
    if (k == X86_LZ_SUB) {
        switch (base) {
        case 0: e_lget(L_LA); e_lget(L_LB); E(W_I32_XOR); e_lget(L_LA); e_lget(L_LR); E(W_I32_XOR); E(W_I32_AND); e_lset(L_T0); e_sign_of(L_T0, sz); break;
        case 1: e_lget(L_LA); e_lget(L_LB); E(W_I32_LT_U); break;
        case 2: e_lget(L_LR); E(W_I32_EQZ); break;
        case 3: e_lget(L_LA); e_lget(L_LB); E(W_I32_LE_U); break;
        case 4: e_sign_of(L_LR, sz); break;
        case 6: e_sext(L_LA, sz); e_sext(L_LB, sz); E(W_I32_LT_S); break;
        case 7: e_sext(L_LA, sz); e_sext(L_LB, sz); E(W_I32_LE_S); break;
        default: ok = 0;
        }
    } else if (k == X86_LZ_LOGIC) {
        switch (base) {
        case 0: case 1: e_i32(0); break;
        case 2: case 3: e_lget(L_LR); E(W_I32_EQZ); break;
        case 4: case 6: e_sign_of(L_LR, sz); break;
        case 7: e_lget(L_LR); E(W_I32_EQZ); e_sign_of(L_LR, sz); E(W_I32_OR); break;
        default: ok = 0;
        }
    } else if (k == X86_LZ_ADD) {
        switch (base) {
        case 1: e_lget(L_LR); e_lget(L_LA); E(W_I32_LT_U); break;
        case 2: e_lget(L_LR); E(W_I32_EQZ); break;
        case 3: e_lget(L_LR); e_lget(L_LA); E(W_I32_LT_U); e_lget(L_LR); E(W_I32_EQZ); E(W_I32_OR); break;
        case 4: e_sign_of(L_LR, sz); break;
        default: ok = 0;
        }
    } else if (k == X86_LZ_INC || k == X86_LZ_DEC) {
        switch (base) {
        case 1: e_lget(L_LCIN); e_i32(0); E(W_I32_NE); break;
        case 2: e_lget(L_LR); E(W_I32_EQZ); break;
        case 3: e_lget(L_LCIN); e_i32(0); E(W_I32_NE); e_lget(L_LR); E(W_I32_EQZ); E(W_I32_OR); break;
        case 4: e_sign_of(L_LR, sz); break;
        default: ok = 0;
        }
    } else {
        ok = 0;
    }
    if (ok) {
        if (neg) E(W_I32_EQZ);
        return;
    }
    materialize();
    e_cond_from_eflags(cc);
}

/* compute CF (0/1) of the current lazy state into L_LCIN (for inc/dec) */
static void e_carry_to_lcin(void) {
    if (gSk == X86_LZ_SUB) { e_lget(L_LA); e_lget(L_LB); E(W_I32_LT_U); e_lset(L_LCIN); return; }
    if (gSk == X86_LZ_ADD) { e_lget(L_LR); e_lget(L_LA); E(W_I32_LT_U); e_lset(L_LCIN); return; }
    if (gSk == X86_LZ_LOGIC) { e_i32(0); e_lset(L_LCIN); return; }
    if (gSk == X86_LZ_INC || gSk == X86_LZ_DEC) return; /* L_LCIN already holds CF */
    materialize();
    cpu_load32(OFF_EFLAGS); e_i32(1); E(W_I32_AND); e_lset(L_LCIN);
}

/* ------------------------------------------------------------------ operands */
static const Insn* gIn;

/* effective address into L_EA */
static void e_ea(const Insn* in) {
    int have = 0;
    if (in->base != 0xff) { e_lget(LREG(in->base)); have = 1; }
    if (in->has_sib && in->index != 4) {
        e_lget(LREG(in->index));
        if (in->scale) { e_i32(in->scale); E(W_I32_SHL); }
        if (have) E(W_I32_ADD);
        have = 1;
    }
    if (!have) e_i32(in->disp);
    else if (in->disp) { e_i32(in->disp); E(W_I32_ADD); }
    if (in->fs) { cpu_load32(OFF_FSBASE); E(W_I32_ADD); }
    e_lset(L_EA);
}

static void e_getreg(int r, int size) {
    if (size == 32) { e_lget(LREG(r)); return; }
    if (size == 16) { e_lget(LREG(r)); e_i32(0xffff); E(W_I32_AND); return; }
    if (r < 4) { e_lget(LREG(r)); e_i32(0xff); E(W_I32_AND); }
    else { e_lget(LREG(r - 4)); e_i32(8); E(W_I32_SHR_U); e_i32(0xff); E(W_I32_AND); }
}
/* value in local v -> register r */
static void e_setreg_from(int r, int size, int v) {
    if (size == 32) { e_lget(v); e_lset(LREG(r)); return; }
    if (size == 16) {
        e_lget(LREG(r)); e_i32((int32_t)0xffff0000u); E(W_I32_AND);
        e_lget(v); e_i32(0xffff); E(W_I32_AND); E(W_I32_OR); e_lset(LREG(r));
        return;
    }
    if (r < 4) {
        e_lget(LREG(r)); e_i32((int32_t)0xffffff00u); E(W_I32_AND);
        e_lget(v); e_i32(0xff); E(W_I32_AND); E(W_I32_OR); e_lset(LREG(r));
    } else {
        e_lget(LREG(r - 4)); e_i32((int32_t)0xffff00ffu); E(W_I32_AND);
        e_lget(v); e_i32(0xff); E(W_I32_AND); e_i32(8); E(W_I32_SHL); E(W_I32_OR); e_lset(LREG(r - 4));
    }
}
static void e_loadmem(int size) {
    e_lget(L_EA);
    if (size == 32) e_mem(W_I32_LOAD, 0, 0);
    else if (size == 16) e_mem(W_I32_LOAD16_U, 0, 0);
    else e_mem(W_I32_LOAD8_U, 0, 0);
}
static void e_storemem_from(int size, int v) {
    e_lget(L_EA);
    e_lget(v);
    if (size == 32) e_mem(W_I32_STORE, 0, 0);
    else if (size == 16) e_mem(W_I32_STORE16, 0, 0);
    else e_mem(W_I32_STORE8, 0, 0);
}
/* r/m operand of current insn (EA must be computed first for memory) */
static void e_rm_load(int size) {
    if (gIn->mod == 3) e_getreg(gIn->rm, size);
    else e_loadmem(size);
}
static void e_rm_store_from(int size, int v) {
    if (gIn->mod == 3) e_setreg_from(gIn->rm, size, v);
    else e_storemem_from(size, v);
}

/* ------------------------------------------------------------------ fallback */
static void f_flush(void);
static void e_fallback(uint32_t addr) {
    f_flush();
    spill_all();
    e_i32((int32_t)addr);
    e_call(H_INTERP);
    reload_all();
    gSk = 0;
    gJit_stats_fallback++;
}

/* ------------------------------------------------------------------ ALU */
/* aop: 0 add 1 or 2 adc 3 sbb 4 and 5 sub 6 xor 7 cmp; operands in L_A, L_B; result -> L_R (masked); sets lazy */
static int e_alu(int aop, int size) {
    uint32_t m = szmask(size);
    if (aop == 2 || aop == 3) {
        /* adc/sbb: carry-in from the current flag state */
        e_carry_to_lcin();
        e_lget(L_A); e_lget(L_B); E(aop == 2 ? W_I32_ADD : W_I32_SUB);
        e_lget(L_LCIN); E(aop == 2 ? W_I32_ADD : W_I32_SUB);
        if (size != 32) { e_i32((int32_t)m); E(W_I32_AND); }
        e_lset(L_R);
        e_lget(L_A); e_lset(L_LA);
        e_lget(L_B); e_lset(L_LB);
        e_lget(L_R); e_lset(L_LR);
        set_lazy(aop == 2 ? X86_LZ_ADC : X86_LZ_SBB, size);
        gSk = 0;
        return 1;
    }
    switch (aop) {
    case 0: e_lget(L_A); e_lget(L_B); E(W_I32_ADD); break;
    case 1: e_lget(L_A); e_lget(L_B); E(W_I32_OR); break;
    case 4: e_lget(L_A); e_lget(L_B); E(W_I32_AND); break;
    case 5: case 7: e_lget(L_A); e_lget(L_B); E(W_I32_SUB); break;
    case 6: e_lget(L_A); e_lget(L_B); E(W_I32_XOR); break;
    default: return 0;
    }
    if (size != 32) { e_i32((int32_t)m); E(W_I32_AND); }
    e_lset(L_R);
    e_lget(L_A); e_lset(L_LA);
    e_lget(L_B); e_lset(L_LB);
    e_lget(L_R); e_lset(L_LR);
    set_lazy(aop == 0 ? X86_LZ_ADD : (aop == 5 || aop == 7) ? X86_LZ_SUB : X86_LZ_LOGIC, size);
    return 1;
}

/* ------------------------------------------------------------------ x87 */
/* Within a basic block the x87 stack is cached in f64 locals: slot s (relative to the
 * top at block entry, kept in L_TOP) lives in local L_FC0+s. FXCH and push/pop become
 * compile-time renaming. The cache is written back (and L_TOP updated) at flush points. */
static int gF_rtop;      /* current top relative to L_TOP (mod 8) */
static uint8_t gF_valid[8], gF_dirty[8];

static void f_reset(void) { gF_rtop = 0; memset(gF_valid, 0, 8); memset(gF_dirty, 0, 8); }
static void e_phys_addr(int slot) {
    e_lget(L_TOP);
    if (slot) { e_i32(slot); E(W_I32_ADD); }
    e_i32(7); E(W_I32_AND);
    e_i32(3); E(W_I32_SHL);
    e_i32((int32_t)(gCpu + OFF_ST)); E(W_I32_ADD);
}
static int fslot(int i) { return (gF_rtop + i) & 7; }
/* push ST(i) */
static void f_get(int i) {
    int s = fslot(i);
    if (!gF_valid[s]) {
        e_phys_addr(s); e_mem(W_F64_LOAD, 3, 0); e_lset(L_FC0 + s);
        gF_valid[s] = 1;
        gF_dirty[s] = 0;
    }
    e_lget(L_FC0 + s);
}
/* pop wasm stack into ST(i) */
static void f_set(int i) {
    int s = fslot(i);
    e_lset(L_FC0 + s);
    gF_valid[s] = 1;
    gF_dirty[s] = 1;
}
static void f_push(void) { gF_rtop = (gF_rtop - 1) & 7; f_set(0); }
static void f_pop(void) {
    int s = fslot(0);
    /* the slot becomes empty; keep its value consistent in memory only if already written */
    if (gF_valid[s] && gF_dirty[s]) { e_phys_addr(s); e_lget(L_FC0 + s); e_mem(W_F64_STORE, 3, 0); }
    gF_valid[s] = 0;
    gF_dirty[s] = 0;
    gF_rtop = (gF_rtop + 1) & 7;
}
static void f_flush(void) {
    for (int s = 0; s < 8; s++) {
        if (gF_valid[s] && gF_dirty[s]) { e_phys_addr(s); e_lget(L_FC0 + s); e_mem(W_F64_STORE, 3, 0); }
    }
    if (gF_rtop) { e_lget(L_TOP); e_i32(gF_rtop); E(W_I32_ADD); e_i32(7); E(W_I32_AND); e_lset(L_TOP); }
    f_reset();
}

/* fsw C0/C2/C3 from compare of the two f64 values in locals a, b */
static void e_fcompare(int fa, int fb) {
    e_i32((int32_t)gCpu);
    e_i32((int32_t)gCpu); e_mem(W_I32_LOAD16_U, 1, OFF_FSW); e_i32(~0x4700 & 0xffff); E(W_I32_AND);
    e_i32(0x100); e_i32(0x4000);
    e_i32(0); e_i32(0x4500);
    e_lget(fa); e_lget(fb); E(W_F64_GT); E(W_SELECT);
    e_lget(fa); e_lget(fb); E(W_F64_EQ); E(W_SELECT);
    e_lget(fa); e_lget(fb); E(W_F64_LT); E(W_SELECT);
    E(W_I32_OR);
    e_mem(W_I32_STORE16, 1, OFF_FSW);
}
/* push (a op b) where a, b already pushed: op 0 add 1 mul 4 sub 5 subr 6 div 7 divr */
static void e_fop(int op) {
    switch (op) {
    case 0: E(W_F64_ADD); break;
    case 1: E(W_F64_MUL); break;
    case 4: E(W_F64_SUB); break;
    case 5: e_lset(L_F2); e_lset(L_F1); e_lget(L_F2); e_lget(L_F1); E(W_F64_SUB); break;
    case 6: E(W_F64_DIV); break;
    case 7: e_lset(L_F2); e_lset(L_F1); e_lget(L_F2); e_lget(L_F1); E(W_F64_DIV); break;
    }
}

/* returns 1 if compiled natively */
static int e_fpu(const Insn* in) {
    uint8_t op = in->op;
    int r = in->reg;
    if (in->mod != 3) {
        e_ea(in);
        switch (op) {
        case 0xd8: case 0xdc: case 0xda:
            if (op == 0xda && (r == 2 || r == 3)) return 0;
            if (r == 2 || r == 3) {
                f_get(0); e_lset(L_F0);
                e_lget(L_EA);
                if (op == 0xd8) { e_mem(W_F32_LOAD, 0, 0); E(W_F64_PROMOTE); } else e_mem(W_F64_LOAD, 0, 0);
                e_lset(L_F1);
                e_fcompare(L_F0, L_F1);
                if (r == 3) f_pop();
                return 1;
            }
            f_get(0);
            e_lget(L_EA);
            if (op == 0xd8) { e_mem(W_F32_LOAD, 0, 0); E(W_F64_PROMOTE); }
            else if (op == 0xdc) e_mem(W_F64_LOAD, 0, 0);
            else { e_mem(W_I32_LOAD, 0, 0); E(W_F64_CONVERT_I32_S); }
            e_fop(r);
            f_set(0);
            return 1;
        case 0xd9:
            if (r == 5) { e_i32((int32_t)gCpu); e_lget(L_EA); e_mem(W_I32_LOAD16_U, 0, 0); e_mem(W_I32_STORE16, 1, OFF_FCW); return 1; }
            if (r == 7) { e_lget(L_EA); e_i32((int32_t)gCpu); e_mem(W_I32_LOAD16_U, 1, OFF_FCW); e_mem(W_I32_STORE16, 0, 0); return 1; }
            if (r == 0) { e_lget(L_EA); e_mem(W_F32_LOAD, 0, 0); E(W_F64_PROMOTE); f_push(); return 1; }
            if (r == 2 || r == 3) { e_lget(L_EA); f_get(0); E(W_F32_DEMOTE); e_mem(W_F32_STORE, 0, 0); if (r == 3) f_pop(); return 1; }
            return 0;
        case 0xdd:
            if (r == 0) { e_lget(L_EA); e_mem(W_F64_LOAD, 0, 0); f_push(); return 1; }
            if (r == 2 || r == 3) { e_lget(L_EA); f_get(0); e_mem(W_F64_STORE, 0, 0); if (r == 3) f_pop(); return 1; }
            return 0;
        case 0xdb:
            if (r == 0) { e_lget(L_EA); e_mem(W_I32_LOAD, 0, 0); E(W_F64_CONVERT_I32_S); f_push(); return 1; }
            if (r == 3 || r == 2) { e_lget(L_EA); f_get(0); e_i32(32); e_call(H_FIST); if (r == 3) f_pop(); return 1; }
            return 0;
        case 0xdf:
            if (r == 0) { e_lget(L_EA); e_mem(W_I32_LOAD16_S, 0, 0); E(W_F64_CONVERT_I32_S); f_push(); return 1; }
            if (r == 5) { e_lget(L_EA); e_call(H_FILD64); f_push(); return 1; }
            if (r == 7) { e_lget(L_EA); f_get(0); e_i32(64); e_call(H_FIST); f_pop(); return 1; }
            return 0;
        }
        return 0;
    }
    int i = in->rm;
    uint8_t modrm = 0xc0 | (r << 3) | i;
    switch (op) {
    case 0xd8:
        if (r == 2 || r == 3) {
            f_get(0); e_lset(L_F0); f_get(i); e_lset(L_F1);
            e_fcompare(L_F0, L_F1);
            if (r == 3) f_pop();
            return 1;
        }
        f_get(0); f_get(i); e_fop(r); f_set(0);
        return 1;
    case 0xdc: case 0xde:
        if (op == 0xde && modrm == 0xd9) { /* fcompp */
            f_get(0); e_lset(L_F0); f_get(1); e_lset(L_F1);
            e_fcompare(L_F0, L_F1); f_pop(); f_pop(); return 1;
        }
        if (r == 2 || r == 3) return 0;
        {
            /* st(i) = st(i) op st(0) with reversed sub/div encodings */
            int rop = r;
            if (r == 4) rop = 5; else if (r == 5) rop = 4; else if (r == 6) rop = 7; else if (r == 7) rop = 6;
            f_get(i); f_get(0); e_fop(rop); f_set(i);
        }
        if (op == 0xde) f_pop();
        return 1;
    case 0xd9:
        if (r == 0) { f_get(i); f_push(); return 1; }
        if (r == 1) {
            /* fxch: rename */
            int s0 = fslot(0), si = fslot(i);
            if (!gF_valid[s0]) { f_get(0); E(W_DROP); }
            if (!gF_valid[si]) { f_get(i); E(W_DROP); }
            e_lget(L_FC0 + s0); e_lget(L_FC0 + si); e_lset(L_FC0 + s0); e_lset(L_FC0 + si);
            gF_dirty[s0] = gF_dirty[si] = 1;
            return 1;
        }
        switch (modrm) {
        case 0xd0: return 1;
        case 0xe0: f_get(0); E(W_F64_NEG); f_set(0); return 1;
        case 0xe1: f_get(0); E(W_F64_ABS); f_set(0); return 1;
        case 0xe8: e_f64(1.0); f_push(); return 1;
        case 0xee: e_f64(0.0); f_push(); return 1;
        case 0xfa: f_get(0); E(W_F64_SQRT); f_set(0); return 1;
        case 0xe4: f_get(0); e_lset(L_F0); e_f64(0.0); e_lset(L_F1); e_fcompare(L_F0, L_F1); return 1;
        }
        return 0;
    case 0xdd:
        if (r == 2) { f_get(0); f_set(i); return 1; }
        if (r == 3) { f_get(0); f_set(i); f_pop(); return 1; }
        if (r == 4 || r == 5) { f_get(0); e_lset(L_F0); f_get(i); e_lset(L_F1); e_fcompare(L_F0, L_F1); if (r == 5) f_pop(); return 1; }
        return 0;
    case 0xda:
        if (modrm == 0xe9) { f_get(0); e_lset(L_F0); f_get(1); e_lset(L_F1); e_fcompare(L_F0, L_F1); f_pop(); f_pop(); return 1; }
        return 0;
    case 0xdf:
        if (modrm == 0xe0) { /* fnstsw ax: top = L_TOP + rtop */
            e_lget(LREG(0)); e_i32((int32_t)0xffff0000u); E(W_I32_AND);
            e_i32((int32_t)gCpu); e_mem(W_I32_LOAD16_U, 1, OFF_FSW); e_i32(~0x3800 & 0xffff); E(W_I32_AND); E(W_I32_OR);
            e_lget(L_TOP); if (gF_rtop) { e_i32(gF_rtop); E(W_I32_ADD); } e_i32(7); E(W_I32_AND); e_i32(11); E(W_I32_SHL); E(W_I32_OR);
            e_lset(LREG(0));
            return 1;
        }
        return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ region compile */
#define MAX_BLOCKS 64
#define MAX_INSNS 900
typedef struct {
    uint32_t start;
    int first, count; /* index into gInsns */
} Blk;
static Blk gBlocks[MAX_BLOCKS];
static int gNum_blocks;
static Insn gInsns[MAX_INSNS];
static int gNum_insns;
static int gRetpoints[MAX_BLOCKS]; /* blocks that are call continuations */
static int gNum_retpoints;

static int find_block(uint32_t addr) {
    for (int i = 0; i < gNum_blocks; i++) if (gBlocks[i].start == addr) return i;
    return -1;
}

static int add_block(uint32_t addr) {
    int b = find_block(addr);
    if (b >= 0) return b;
    if (gNum_blocks >= MAX_BLOCKS) return -1;
    gBlocks[gNum_blocks].start = addr;
    gBlocks[gNum_blocks].first = -1;
    gBlocks[gNum_blocks].count = 0;
    return gNum_blocks++;
}

/* hooks decide which call targets are interesting; addresses >= this are traps */
#define TRAP_BASE 0xFFFE0000u

static int discover(uint32_t entry) {
    gNum_blocks = 0;
    gNum_insns = 0;
    gNum_retpoints = 0;
    add_block(entry);
    for (int bi = 0; bi < gNum_blocks; bi++) {
        Blk* b = &gBlocks[bi];
        uint32_t pc = b->start;
        b->first = gNum_insns;
        for (;;) {
            if (gNum_insns >= MAX_INSNS) break;
            /* stop at the start of another block (fallthrough edge) */
            if (pc != b->start && find_block(pc) >= 0) break;
            Insn* in = &gInsns[gNum_insns];
            if (!decode(pc, in)) { in->op = 0xff; in->reg = 7; in->len = 0; break; }
            int k = cf_kind(in);
            if (k == CF_STOP) break;
            gNum_insns++;
            b->count++;
            pc = in->next;
            if (k == CF_NONE) {
                if (b->count >= 64) break;
                continue;
            }
            if (k == CF_JCC) { add_block(branch_target(in)); add_block(in->next); }
            else if (k == CF_JMP) { add_block(branch_target(in)); }
            else if (k == CF_CALL || k == CF_IND_CALL) {
                int rb_ = add_block(in->next);
                if (rb_ >= 0 && gNum_retpoints < MAX_BLOCKS) {
                    int dup = 0;
                    for (int q = 0; q < gNum_retpoints; q++) if (gRetpoints[q] == rb_) dup = 1;
                    if (!dup) gRetpoints[gNum_retpoints++] = rb_;
                }
                if (k == CF_CALL) {
                    uint32_t t = branch_target(in);
                    if (t >= 0x400000u && t < 0x80000000u) add_block(t);
                }
            }
            break;
        }
        b = &gBlocks[bi];
        if (b->count == 0) {
            /* nothing compilable at this block start: keep it as a trampoline back to the interpreter */
        }
    }
    return gNum_blocks;
}

/* exit the region with eip = constant */
static void e_exit_const(uint32_t eip, int depth_to_exit) {
    e_i32((int32_t)gCpu); e_i32((int32_t)eip); e_mem(W_I32_STORE, 2, OFF_EIP);
    E(W_BR); uleb(&gCode, depth_to_exit);
}
/* exit with eip in local */
static void e_exit_local(int l, int depth_to_exit) {
    cpu_store32_from(l, OFF_EIP);
    E(W_BR); uleb(&gCode, depth_to_exit);
}
/* jump to target: block in region (via dispatch loop) or exit */
static void e_goto(uint32_t target, int bi) {
    int depth_exit = gNum_blocks - 1 - bi;
    int depth_loop = gNum_blocks - bi;
    int tb = find_block(target);
    if (tb >= 0 && gBlocks[tb].count > 0) {
        e_i32(tb); e_lset(L_STATE);
        E(W_BR); uleb(&gCode, depth_loop);
    } else {
        e_exit_const(target, depth_exit);
    }
}

static int compile_insn(const Insn* in, int bi) {
    gIn = in;
    uint8_t op = in->op;
    int osz = in->os16 ? 16 : 32;
    int depth_exit = gNum_blocks - 1 - bi;

    int k = cf_kind(in);
    if (k != CF_NONE) f_flush();
    if (k == CF_JCC) {
        int cc = (op == 0x0f) ? (in->op2 & 0xf) : (op & 0xf);
        e_cond(cc);
        E(W_IF); E(T_VOID);
        {
            /* inside if: one extra nesting level */
            int tb = find_block(branch_target(in));
            if (tb >= 0 && gBlocks[tb].count > 0) {
                e_i32(tb); e_lset(L_STATE);
                E(W_BR); uleb(&gCode, gNum_blocks - bi + 1);
            } else {
                e_i32((int32_t)gCpu); e_i32((int32_t)branch_target(in)); e_mem(W_I32_STORE, 2, OFF_EIP);
                E(W_BR); uleb(&gCode, depth_exit + 1);
            }
        }
        E(W_END);
        e_goto(in->next, bi);
        return 1;
    }
    if (k == CF_JMP) { e_goto(branch_target(in), bi); return 1; }
    if (k == CF_CALL) {
        /* push return address; continue in the callee if it is part of this region */
        e_lget(LREG(4)); e_i32(4); E(W_I32_SUB); e_ltee(LREG(4));
        e_i32((int32_t)in->next); e_mem(W_I32_STORE, 2, 0);
        e_goto(branch_target(in), bi);
        return 1;
    }
    if (k == CF_RET) {
        e_lget(LREG(4)); e_mem(W_I32_LOAD, 2, 0); e_lset(L_T0);
        e_lget(LREG(4)); e_i32(4 + (op == 0xc2 ? (int32_t)in->imm : 0)); E(W_I32_ADD); e_lset(LREG(4));
        /* return into a call continuation of this region without leaving it */
        for (int q = 0; q < gNum_retpoints; q++) {
            int tb = gRetpoints[q];
            if (gBlocks[tb].count == 0) continue;
            e_lget(L_T0); e_i32((int32_t)gBlocks[tb].start); E(W_I32_EQ);
            E(W_IF); E(T_VOID);
            e_i32(tb); e_lset(L_STATE);
            E(W_BR); uleb(&gCode, gNum_blocks - bi + 1);
            E(W_END);
        }
        e_exit_local(L_T0, depth_exit);
        return 1;
    }
    if (k == CF_IND_CALL || k == CF_IND_JMP) {
        if (in->mod != 3) { e_ea(in); e_loadmem(32); } else e_getreg(in->rm, 32);
        e_lset(L_T0);
        if (k == CF_IND_CALL) {
            e_lget(LREG(4)); e_i32(4); E(W_I32_SUB); e_ltee(LREG(4));
            e_i32((int32_t)in->next); e_mem(W_I32_STORE, 2, 0);
        }
        e_exit_local(L_T0, depth_exit);
        return 1;
    }

    /* ---- straight-line instructions ---- */
    if (op < 0x40 && (op & 7) < 6) {
        int aop = op >> 3, form = op & 7;
        int size = (form & 1) ? osz : 8;
        if (form < 4) {
            if (in->mod != 3) e_ea(in);
            if (form < 2) { e_rm_load(size); e_lset(L_A); e_getreg(in->reg, size); e_lset(L_B); }
            else { e_getreg(in->reg, size); e_lset(L_A); e_rm_load(size); e_lset(L_B); }
            /* xor r,r / sub r,r idiom */
            e_alu(aop, size);
            if (aop != 7) {
                if (form < 2) e_rm_store_from(size, L_R);
                else e_setreg_from(in->reg, size, L_R);
            }
        } else {
            e_getreg(0, size); e_lset(L_A);
            e_i32((int32_t)(in->imm & szmask(size))); e_lset(L_B);
            e_alu(aop, size);
            if (aop != 7) e_setreg_from(0, size, L_R);
        }
        return 1;
    }
    if (op >= 0x40 && op <= 0x4f) {
        int r = op & 7;
        e_carry_to_lcin();
        e_getreg(r, osz); e_lset(L_A);
        e_lget(L_A); e_i32(1); E(op < 0x48 ? W_I32_ADD : W_I32_SUB);
        if (osz != 32) { e_i32((int32_t)szmask(osz)); E(W_I32_AND); }
        e_lset(L_R);
        e_lget(L_A); e_lset(L_LA); e_i32(1); e_lset(L_LB); e_lget(L_R); e_lset(L_LR);
        set_lazy(op < 0x48 ? X86_LZ_INC : X86_LZ_DEC, osz);
        e_setreg_from(r, osz, L_R);
        return 1;
    }
    if (op >= 0x50 && op <= 0x57 && !in->os16) {
        e_getreg(op & 7, 32); e_lset(L_V);
        e_lget(LREG(4)); e_i32(4); E(W_I32_SUB); e_ltee(LREG(4));
        e_lget(L_V); e_mem(W_I32_STORE, 2, 0);
        return 1;
    }
    if (op >= 0x58 && op <= 0x5f && !in->os16) {
        e_lget(LREG(4)); e_mem(W_I32_LOAD, 2, 0); e_lset(L_V);
        e_lget(LREG(4)); e_i32(4); E(W_I32_ADD); e_lset(LREG(4));
        e_lget(L_V); e_lset(LREG(op & 7));
        return 1;
    }
    if ((op == 0x68 || op == 0x6a) && !in->os16) {
        uint32_t v = op == 0x6a ? (uint32_t)(int32_t)(int8_t)in->imm : in->imm;
        e_lget(LREG(4)); e_i32(4); E(W_I32_SUB); e_ltee(LREG(4));
        e_i32((int32_t)v); e_mem(W_I32_STORE, 2, 0);
        return 1;
    }
    if ((op >= 0x80 && op <= 0x83)) {
        int size = (op & 1) ? osz : 8;
        int aop = in->reg;
        uint32_t imm = (op == 0x83) ? (uint32_t)(int32_t)(int8_t)in->imm : in->imm;
        if (in->mod != 3) e_ea(in);
        e_rm_load(size); e_lset(L_A);
        e_i32((int32_t)(imm & szmask(size))); e_lset(L_B);
        e_alu(aop, size);
        if (aop != 7) e_rm_store_from(size, L_R);
        return 1;
    }
    if (op == 0x84 || op == 0x85) {
        int size = (op & 1) ? osz : 8;
        if (in->mod != 3) e_ea(in);
        e_rm_load(size); e_lset(L_A);
        e_getreg(in->reg, size); e_lset(L_B);
        e_alu(4, size);
        return 1;
    }
    if (op == 0xa8 || op == 0xa9) {
        int size = (op & 1) ? osz : 8;
        e_getreg(0, size); e_lset(L_A);
        e_i32((int32_t)(in->imm & szmask(size))); e_lset(L_B);
        e_alu(4, size);
        return 1;
    }
    if (op >= 0x88 && op <= 0x8b) {
        int size = (op & 1) ? osz : 8;
        if (in->mod != 3) e_ea(in);
        if (op <= 0x89) { e_getreg(in->reg, size); e_lset(L_V); e_rm_store_from(size, L_V); }
        else { e_rm_load(size); e_lset(L_V); e_setreg_from(in->reg, size, L_V); }
        return 1;
    }
    if (op == 0x8d && in->mod != 3 && !in->os16) {
        Insn t = *in;
        t.fs = 0;
        e_ea(&t);
        e_lget(L_EA); e_lset(LREG(in->reg));
        return 1;
    }
    if (op == 0x90) return 1;
    if (op >= 0xb0 && op <= 0xb7) { e_i32((int32_t)(in->imm & 0xff)); e_lset(L_V); e_setreg_from(op & 7, 8, L_V); return 1; }
    if (op >= 0xb8 && op <= 0xbf) { e_i32((int32_t)in->imm); e_lset(L_V); e_setreg_from(op & 7, osz, L_V); return 1; }
    if (op == 0xc6 || op == 0xc7) {
        int size = (op & 1) ? osz : 8;
        if (in->reg != 0) goto fallback;
        if (in->mod != 3) e_ea(in);
        e_i32((int32_t)(in->imm & szmask(size))); e_lset(L_V);
        e_rm_store_from(size, L_V);
        return 1;
    }
    if (op >= 0xa0 && op <= 0xa3) {
        int size = (op & 1) ? osz : 8;
        Insn t = *in;
        t.base = 0xff; t.has_sib = 0; t.disp = (int32_t)in->imm;
        e_ea(&t);
        if (op <= 0xa1) { e_loadmem(size); e_lset(L_V); e_setreg_from(0, size, L_V); }
        else { e_getreg(0, size); e_lset(L_V); e_storemem_from(size, L_V); }
        return 1;
    }
    if (op == 0x99 && !in->os16) { e_lget(LREG(0)); e_i32(31); E(W_I32_SHR_S); e_lset(LREG(2)); return 1; }
    if (op == 0x98 && !in->os16) { e_lget(LREG(0)); E(W_I32_EXTEND16_S); e_lset(LREG(0)); return 1; }
    if ((op == 0xc1 || op == 0xc0 || op == 0xd1 || op == 0xd0) && (in->reg == 4 || in->reg == 5 || in->reg == 7)) {
        int size = (op & 1) ? osz : 8;
        unsigned cnt = (op <= 0xc1) ? (in->imm & 31) : 1;
        if (cnt == 0) return 1; /* no-op, flags unchanged */
        if (cnt >= (unsigned)size) goto fallback;
        if (in->mod != 3) e_ea(in);
        e_rm_load(size); e_lset(L_A);
        e_lget(L_A); e_i32(cnt);
        if (in->reg == 4) E(W_I32_SHL);
        else if (in->reg == 5) E(W_I32_SHR_U);
        else { /* sar: sign-extend first */
            E(W_DROP); E(W_DROP);
            e_sext(L_A, size); e_i32(cnt); E(W_I32_SHR_S);
        }
        if (size != 32) { e_i32((int32_t)szmask(size)); E(W_I32_AND); }
        e_lset(L_R);
        /* carry-out for shl in L_LCIN */
        if (in->reg == 4) { e_lget(L_A); e_i32(size - cnt); E(W_I32_SHR_U); e_i32(1); E(W_I32_AND); e_lset(L_LCIN); }
        e_lget(L_A); e_lset(L_LA); e_i32(cnt); e_lset(L_LB); e_lget(L_R); e_lset(L_LR);
        set_lazy(in->reg == 4 ? X86_LZ_SHL : in->reg == 5 ? X86_LZ_SHR : X86_LZ_SAR, size);
        gSk = 0; /* conditions after shifts: generic path */
        e_rm_store_from(size, L_R);
        return 1;
    }
    if ((op == 0xf7 || op == 0xf6) && in->reg < 4) {
        int size = (op & 1) ? osz : 8;
        if (in->reg == 0 || in->reg == 1) {
            if (in->mod != 3) e_ea(in);
            e_rm_load(size); e_lset(L_A);
            e_i32((int32_t)(in->imm & szmask(size))); e_lset(L_B);
            e_alu(4, size);
            return 1;
        }
        if (in->reg == 2) {
            if (in->mod != 3) e_ea(in);
            e_rm_load(size); e_i32(-1); E(W_I32_XOR); e_lset(L_V);
            e_rm_store_from(size, L_V);
            return 1;
        }
        if (in->reg == 3) {
            if (in->mod != 3) e_ea(in);
            e_rm_load(size); e_lset(L_A);
            e_i32(0); e_lget(L_A); E(W_I32_SUB);
            if (size != 32) { e_i32((int32_t)szmask(size)); E(W_I32_AND); }
            e_lset(L_R);
            e_lget(L_A); e_lset(L_LA); e_i32(0); e_lset(L_LB); e_lget(L_R); e_lset(L_LR);
            set_lazy(X86_LZ_NEG, size);
            gSk = 0;
            e_rm_store_from(size, L_R);
            return 1;
        }
        goto fallback;
    }
    if (op == 0xfe || op == 0xff) {
        int size = op == 0xfe ? 8 : osz;
        if (in->reg == 0 || in->reg == 1) {
            if (in->mod != 3) e_ea(in);
            e_carry_to_lcin();
            e_rm_load(size); e_lset(L_A);
            e_lget(L_A); e_i32(1); E(in->reg == 0 ? W_I32_ADD : W_I32_SUB);
            if (size != 32) { e_i32((int32_t)szmask(size)); E(W_I32_AND); }
            e_lset(L_R);
            e_lget(L_A); e_lset(L_LA); e_i32(1); e_lset(L_LB); e_lget(L_R); e_lset(L_LR);
            set_lazy(in->reg == 0 ? X86_LZ_INC : X86_LZ_DEC, size);
            e_rm_store_from(size, L_R);
            return 1;
        }
        if (in->reg == 6 && op == 0xff && !in->os16) {
            if (in->mod != 3) e_ea(in);
            e_rm_load(32); e_lset(L_V);
            e_lget(LREG(4)); e_i32(4); E(W_I32_SUB); e_ltee(LREG(4));
            e_lget(L_V); e_mem(W_I32_STORE, 2, 0);
            return 1;
        }
        goto fallback;
    }
    if (op == 0x0f && (in->op2 == 0xa4 || in->op2 == 0xa5 || in->op2 == 0xac || in->op2 == 0xad) && !in->os16) goto shxd;
    if (op == 0x0f) {
        uint8_t o2 = in->op2;
        if (o2 == 0xb6 || o2 == 0xb7 || o2 == 0xbe || o2 == 0xbf) {
            int ssz = (o2 & 1) ? 16 : 8;
            if (in->mod != 3) e_ea(in);
            e_rm_load(ssz);
            if (o2 >= 0xbe) E(ssz == 8 ? W_I32_EXTEND8_S : W_I32_EXTEND16_S);
            e_lset(L_V);
            e_setreg_from(in->reg, osz, L_V);
            return 1;
        }
        if (o2 >= 0x90 && o2 <= 0x9f) {
            if (in->mod != 3) e_ea(in);
            e_cond(o2 & 0xf); e_lset(L_V);
            e_rm_store_from(8, L_V);
            return 1;
        }
        if (o2 == 0xaf && !in->os16) {
            if (in->mod != 3) e_ea(in);
            e_lget(LREG(in->reg)); E(W_I64_EXTEND_I32_S);
            e_rm_load(32); E(W_I64_EXTEND_I32_S);
            E(W_I64_MUL); e_lset(L_Q0);
            e_lget(L_Q0); E(W_I32_WRAP_I64); e_lset(LREG(in->reg));
            /* overflow if result != sign-extended low */
            e_lget(L_Q0); e_lget(LREG(in->reg)); E(W_I64_EXTEND_I32_S); E(W_I64_NE); e_lset(L_LR);
            set_lazy(X86_LZ_MUL, 32);
            gSk = 0;
            return 1;
        }
        goto fallback;
    }
    if ((op == 0x69 || op == 0x6b) && !in->os16) {
        int32_t imm = op == 0x6b ? (int32_t)(int8_t)in->imm : (int32_t)in->imm;
        if (in->mod != 3) e_ea(in);
        e_rm_load(32); E(W_I64_EXTEND_I32_S);
        e_i64(imm);
        E(W_I64_MUL); e_lset(L_Q0);
        e_lget(L_Q0); E(W_I32_WRAP_I64); e_lset(LREG(in->reg));
        e_lget(L_Q0); e_lget(LREG(in->reg)); E(W_I64_EXTEND_I32_S); E(W_I64_NE); e_lset(L_LR);
        set_lazy(X86_LZ_MUL, 32);
        gSk = 0;
        return 1;
    }
    if (op >= 0xd8 && op <= 0xdf) {
        if (e_fpu(in)) return 1;
        goto fallback;
    }
    if (op == 0x9b) return 1; /* fwait */
    if (op == 0xc9 && !in->os16) { /* leave */
        e_lget(LREG(5)); e_lset(LREG(4));
        e_lget(LREG(4)); e_mem(W_I32_LOAD, 2, 0); e_lset(LREG(5));
        e_lget(LREG(4)); e_i32(4); E(W_I32_ADD); e_lset(LREG(4));
        return 1;
    }
    if (op == 0xf7 && (in->reg == 6 || in->reg == 7) && !in->os16) {
        /* div/idiv r/m32: edx:eax / src; divide by zero is left to the interpreter */
        f_flush();
        if (in->mod != 3) e_ea(in);
        e_rm_load(32); e_lset(L_B);
        e_lget(L_B); E(W_I32_EQZ);
        E(W_IF); E(T_VOID);
        e_fallback(in->addr);
        E(W_ELSE);
        e_lget(LREG(2)); E(W_I64_EXTEND_I32_U); e_i64(32); E(W_I64_SHL);
        e_lget(LREG(0)); E(W_I64_EXTEND_I32_U); E(W_I64_OR); e_lset(L_Q0);
        if (in->reg == 6) {
            e_lget(L_Q0); e_lget(L_B); E(W_I64_EXTEND_I32_U); E(W_I64_DIV_U); E(W_I32_WRAP_I64); e_lset(L_A);
            e_lget(L_Q0); e_lget(L_B); E(W_I64_EXTEND_I32_U); E(W_I64_REM_U); E(W_I32_WRAP_I64); e_lset(LREG(2));
        } else {
            e_lget(L_Q0); e_lget(L_B); E(W_I64_EXTEND_I32_S); E(W_I64_DIV_S); E(W_I32_WRAP_I64); e_lset(L_A);
            e_lget(L_Q0); e_lget(L_B); E(W_I64_EXTEND_I32_S); E(W_I64_REM_S); E(W_I32_WRAP_I64); e_lset(LREG(2));
        }
        e_lget(L_A); e_lset(LREG(0));
        E(W_END);
        gSk = 0;
        return 1;
    }
    if (op == 0xf7 && (in->reg == 4 || in->reg == 5) && !in->os16) {
        /* mul/imul r/m32 -> edx:eax */
        if (in->mod != 3) e_ea(in);
        e_lget(LREG(0)); E(in->reg == 4 ? W_I64_EXTEND_I32_U : W_I64_EXTEND_I32_S);
        e_rm_load(32); E(in->reg == 4 ? W_I64_EXTEND_I32_U : W_I64_EXTEND_I32_S);
        E(W_I64_MUL); e_lset(L_Q0);
        e_lget(L_Q0); E(W_I32_WRAP_I64); e_lset(LREG(0));
        e_lget(L_Q0); e_i64(32); E(W_I64_SHR_U); E(W_I32_WRAP_I64); e_lset(LREG(2));
        if (in->reg == 4) { e_lget(LREG(2)); e_i32(0); E(W_I32_NE); }
        else { e_lget(LREG(2)); e_lget(LREG(0)); e_i32(31); E(W_I32_SHR_S); E(W_I32_NE); }
        e_lset(L_LR);
        set_lazy(X86_LZ_MUL, 32);
        gSk = 0;
        return 1;
    }
    if ((op == 0xd3) && (in->reg == 4 || in->reg == 5 || in->reg == 7) && !in->os16) {
        /* shift r/m32 by cl */
        if (in->mod != 3) e_ea(in);
        e_lget(LREG(1)); e_i32(31); E(W_I32_AND); e_lset(L_B);
        e_lget(L_B);
        E(W_IF); E(T_VOID);
        e_rm_load(32); e_lset(L_A);
        e_lget(L_A); e_lget(L_B);
        E(in->reg == 4 ? W_I32_SHL : in->reg == 5 ? W_I32_SHR_U : W_I32_SHR_S);
        e_lset(L_R);
        if (in->reg == 4) { e_lget(L_A); e_i32(32); e_lget(L_B); E(W_I32_SUB); E(W_I32_SHR_U); e_i32(1); E(W_I32_AND); e_lset(L_LCIN); }
        e_lget(L_A); e_lset(L_LA); e_lget(L_B); e_lset(L_LB); e_lget(L_R); e_lset(L_LR);
        e_i32(in->reg == 4 ? X86_LZ_SHL : in->reg == 5 ? X86_LZ_SHR : X86_LZ_SAR); e_lset(L_LK);
        e_i32(32); e_lset(L_LSZ);
        e_rm_store_from(32, L_R);
        E(W_END);
        gSk = 0;
        return 1;
    }
    if ((op == 0xa5 || op == 0xa4) && !in->rep && !in->os16 && !in->fs) {
        /* movsd/movsb with DF = 0 */
        int n = op == 0xa5 ? 4 : 1;
        f_flush();
        cpu_load32(OFF_EFLAGS); e_i32(X86_DF); E(W_I32_AND);
        E(W_IF); E(T_VOID);
        e_fallback(in->addr);
        E(W_ELSE);
        e_lget(LREG(7)); e_lget(LREG(6)); e_mem(n == 4 ? W_I32_LOAD : W_I32_LOAD8_U, 0, 0); e_mem(n == 4 ? W_I32_STORE : W_I32_STORE8, 0, 0);
        e_lget(LREG(6)); e_i32(n); E(W_I32_ADD); e_lset(LREG(6));
        e_lget(LREG(7)); e_i32(n); E(W_I32_ADD); e_lset(LREG(7));
        E(W_END);
        gSk = 0;
        return 1;
    }
    if (op == 0x0f && (in->op2 == 0xa4 || in->op2 == 0xa5 || in->op2 == 0xac || in->op2 == 0xad) && !in->os16) {
    shxd:;
        /* shld/shrd r/m32, r32, imm8/cl */
        int left = in->op2 <= 0xa5;
        if (in->mod != 3) e_ea(in);
        if (in->op2 == 0xa4 || in->op2 == 0xac) e_i32(in->imm & 31); else { e_lget(LREG(1)); e_i32(31); E(W_I32_AND); }
        e_lset(L_B);
        e_lget(L_B);
        E(W_IF); E(T_VOID);
        e_rm_load(32); e_lset(L_A);
        e_getreg(in->reg, 32); e_lset(L_T1);
        if (left) {
            e_lget(L_A); e_lget(L_B); E(W_I32_SHL);
            e_lget(L_T1); e_i32(32); e_lget(L_B); E(W_I32_SUB); E(W_I32_SHR_U); E(W_I32_OR); e_lset(L_R);
            e_lget(L_A); e_i32(32); e_lget(L_B); E(W_I32_SUB); E(W_I32_SHR_U); e_i32(1); E(W_I32_AND); e_lset(L_LCIN);
        } else {
            e_lget(L_A); e_lget(L_B); E(W_I32_SHR_U);
            e_lget(L_T1); e_i32(32); e_lget(L_B); E(W_I32_SUB); E(W_I32_SHL); E(W_I32_OR); e_lset(L_R);
            e_lget(L_A); e_lget(L_B); e_i32(1); E(W_I32_SUB); E(W_I32_SHR_U); e_i32(1); E(W_I32_AND); e_lset(L_LCIN);
        }
        e_lget(L_R); e_lset(L_LR);
        e_i32(X86_LZ_SHD); e_lset(L_LK);
        e_i32(32); e_lset(L_LSZ);
        e_rm_store_from(32, L_R);
        E(W_END);
        gSk = 0;
        return 1;
    }
fallback:
    e_fallback(in->addr);
    return 1;
}

/* ------------------------------------------------------------------ module assembly */
static Buf gMod;

static void sec(Buf* out, int id, Buf* content) {
    bput(out, (uint8_t)id);
    uleb(out, (uint32_t)content->n);
    bputs(out, content->p, content->n);
    content->n = 0;
}

static int build_module(void) {
    gMod.n = 0;
    static const uint8_t hdr[8] = { 0, 'a', 's', 'm', 1, 0, 0, 0 };
    bputs(&gMod, hdr, 8);
    Buf s = { 0 };
    /* types: 0 (i32)->() ; 1 ()->() ; 2 (i32,f64,i32)->() ; 3 (i32)->f64 */
    uleb(&s, 4);
    bput(&s, 0x60); uleb(&s, 1); bput(&s, 0x7f); uleb(&s, 0);
    bput(&s, 0x60); uleb(&s, 0); uleb(&s, 0);
    bput(&s, 0x60); uleb(&s, 3); bput(&s, 0x7f); bput(&s, 0x7c); bput(&s, 0x7f); uleb(&s, 0);
    bput(&s, 0x60); uleb(&s, 1); bput(&s, 0x7f); uleb(&s, 1); bput(&s, 0x7c);
    sec(&gMod, 1, &s);
    /* imports: env.memory, env.table, env.interp (type0), env.mat (type1), env.fist (type2), env.fild64 (type3) */
    uleb(&s, 6);
    uleb(&s, 3); bputs(&s, "env", 3); uleb(&s, 6); bputs(&s, "memory", 6); bput(&s, 2); bput(&s, 0); uleb(&s, 1);
    uleb(&s, 3); bputs(&s, "env", 3); uleb(&s, 5); bputs(&s, "table", 5); bput(&s, 1); bput(&s, 0x70); bput(&s, 0); uleb(&s, 0);
    uleb(&s, 3); bputs(&s, "env", 3); uleb(&s, 6); bputs(&s, "interp", 6); bput(&s, 0); uleb(&s, 0);
    uleb(&s, 3); bputs(&s, "env", 3); uleb(&s, 3); bputs(&s, "mat", 3); bput(&s, 0); uleb(&s, 1);
    uleb(&s, 3); bputs(&s, "env", 3); uleb(&s, 4); bputs(&s, "fist", 4); bput(&s, 0); uleb(&s, 2);
    uleb(&s, 3); bputs(&s, "env", 3); uleb(&s, 6); bputs(&s, "fild64", 6); bput(&s, 0); uleb(&s, 3);
    sec(&gMod, 2, &s);
    /* function section: one function of type 0 */
    uleb(&s, 1); uleb(&s, 0);
    sec(&gMod, 3, &s);
    /* export "f" = func index 3 (after 3 imported funcs) */
    uleb(&s, 1); uleb(&s, 1); bput(&s, 'f'); bput(&s, 0); uleb(&s, H_NUM);
    sec(&gMod, 7, &s);
    /* code */
    Buf body = { 0 };
    /* locals: i32 x (L_NUM_I32-1), f64 x3, i64 x1 */
    uleb(&body, 3);
    uleb(&body, L_NUM_I32 - 1); bput(&body, 0x7f);
    uleb(&body, L_NUM_F64); bput(&body, 0x7c);
    uleb(&body, L_NUM_I64); bput(&body, 0x7e);
    bputs(&body, gCode.p, gCode.n);
    bput(&body, W_END);
    uleb(&s, 1);
    uleb(&s, (uint32_t)body.n);
    bputs(&s, body.p, body.n);
    sec(&gMod, 10, &s);
    free(body.p);
    free(s.p);
    return 1;
}

static uint32_t* gCur_table;
static int compile_region(uint32_t entry) {
    discover(entry);
    if (gBlocks[0].count == 0) return 0;
    gCur_table = malloc(sizeof(uint32_t) * gNum_blocks);
    for (int i = 0; i < gNum_blocks; i++) gCur_table[i] = gBlocks[i].start;
    gCpu = H2G(&gX86);
    gCode.n = 0;
    /* prologue: count entries (statistics) */
    e_i32((int32_t)H2G(&gJit_entries)); e_i32((int32_t)H2G(&gJit_entries)); e_mem(W_I32_LOAD, 2, 0); e_i32(1); E(W_I32_ADD); e_mem(W_I32_STORE, 2, 0);
    /* prologue: load state */
    for (int r = 0; r < 8; r++) { cpu_load32(OFF_REG(r)); e_lset(LREG(r)); }
    cpu_load32(OFF_TOP); e_lset(L_TOP);
    cpu_load32(OFF_LZK); e_lset(L_LK);
    cpu_load32(OFF_LZSZ); e_lset(L_LSZ);
    cpu_load32(OFF_LZA); e_lset(L_LA);
    cpu_load32(OFF_LZB); e_lset(L_LB);
    cpu_load32(OFF_LZR); e_lset(L_LR);
    cpu_load32(OFF_LZCIN); e_lset(L_LCIN);
    e_i32(20000); e_lset(L_BUDGET);
    e_lget(L_ENTRY); e_lset(L_STATE);
    /* loop $L { block $exit { block b_{n-1} ... block b0 { br_table } code0 } code1 ... } } */
    E(W_LOOP); E(T_VOID);
    /* budget check: if (--budget == 0) exit at current block's address */
    E(W_BLOCK); E(T_VOID); /* $exit */
    for (int i = gNum_blocks - 1; i >= 0; i--) { E(W_BLOCK); E(T_VOID); }
    e_lget(L_BUDGET); e_i32(1); E(W_I32_SUB); e_ltee(L_BUDGET);
    E(W_I32_EQZ);
    E(W_IF); E(T_VOID);
    {
        /* store eip = address of block[state] via a small select chain is costly; use a br_table-free trick:
           exit with eip computed from a constant table in memory */
        e_i32((int32_t)gCpu);
        e_lget(L_STATE); e_i32(2); E(W_I32_SHL);
        e_i32((int32_t)H2G(gCur_table));
        E(W_I32_ADD);
        e_mem(W_I32_LOAD, 2, 0);
        e_mem(W_I32_STORE, 2, OFF_EIP);
        E(W_BR); uleb(&gCode, gNum_blocks + 1);
    }
    E(W_END);
    e_lget(L_STATE);
    E(W_BR_TABLE); uleb(&gCode, gNum_blocks);
    for (int i = 0; i < gNum_blocks; i++) uleb(&gCode, i);
    uleb(&gCode, gNum_blocks); /* default -> $exit */
    for (int bi = 0; bi < gNum_blocks; bi++) {
        E(W_END); /* end of block b_bi: its code follows */
        Blk* b = &gBlocks[bi];
        gSk = 0;
        if (b->count == 0) {
            e_exit_const(b->start, gNum_blocks - 1 - bi);
            continue;
        }
        f_reset();
        e_lget(L_IC); e_i32(b->count); E(W_I32_ADD); e_lset(L_IC);
        for (int i = 0; i < b->count; i++) {
            const Insn* in = &gInsns[b->first + i];
            compile_insn(in, bi);
        }
        const Insn* last = &gInsns[b->first + b->count - 1];
        int k = cf_kind(last);
        if (k == CF_NONE) {
            /* fell off the block (split point or length limit) */
            f_flush();
            e_goto(last->next, bi);
        }
    }
    E(W_END); /* $exit */
    /* epilogue: spill, then chain directly into the next compiled region if cached */
    spill_all();
    flush_icount();
    cpu_load32(OFF_EIP); e_lset(L_T0);
    e_lget(L_T0); e_lget(L_T0); e_i32(14); E(W_I32_SHR_U); E(W_I32_XOR); e_i32((1 << DC_BITS) - 1); E(W_I32_AND);
    e_i32(12); E(W_I32_MUL); e_i32((int32_t)H2G(gDc)); E(W_I32_ADD); e_lset(L_T1);
    e_lget(L_T1); e_mem(W_I32_LOAD, 2, 0); e_lget(L_T0); E(W_I32_EQ);
    e_i32((int32_t)H2G(&gJit_depth)); e_mem(W_I32_LOAD, 2, 0); e_i32(CHAIN_MAX); E(W_I32_LT_S);
    E(W_I32_AND);
    E(W_IF); E(T_VOID);
    e_i32((int32_t)H2G(&gJit_depth)); e_i32((int32_t)H2G(&gJit_depth)); e_mem(W_I32_LOAD, 2, 0); e_i32(1); E(W_I32_ADD); e_mem(W_I32_STORE, 2, 0);
    e_lget(L_T1); e_mem(W_I32_LOAD, 2, 8);
    e_lget(L_T1); e_mem(W_I32_LOAD, 2, 4);
    E(0x11); uleb(&gCode, 0); uleb(&gCode, 0); /* call_indirect type 0, table 0 */
    e_i32((int32_t)H2G(&gJit_depth)); e_i32((int32_t)H2G(&gJit_depth)); e_mem(W_I32_LOAD, 2, 0); e_i32(1); E(W_I32_SUB); e_mem(W_I32_STORE, 2, 0);
    E(W_END);
    E(W_END); /* loop: falls out after exit */
    gJit_stats_insns += gNum_insns;
    return 1;
}

/* ------------------------------------------------------------------ entry table */
typedef struct {
    uint32_t eip;
    int32_t fn;      /* wasm table index, 0 = none */
    int16_t blk;
    uint16_t hits;
    uint8_t pending;
} Ent;
#define ENT_BITS 16
#define ENT_SIZE (1u << ENT_BITS)
static Ent* gEnts;

static Ent* ent_get(uint32_t eip, int create) {
    uint32_t h = (eip * 2654435761u) >> (32 - ENT_BITS);
    for (uint32_t i = 0; i < ENT_SIZE; i++) {
        Ent* e = &gEnts[(h + i) & (ENT_SIZE - 1)];
        if (e->eip == eip) return e;
        if (e->eip == 0) {
            if (!create) return NULL;
            e->eip = eip;
            return e;
        }
    }
    return NULL;
}

typedef struct {
    int nblocks;
    uint32_t* starts;
    uint32_t* eip_table; /* guest memory: start address per block (for budget exits) */
    int* counts;
} Region;
#define MAX_REGIONS 8192
static Region gRegions[MAX_REGIONS];
static int gNum_regions;

#ifdef __EMSCRIPTEN__
extern void jit_js_compile(const uint8_t* bytes, int len, int region);
#endif

/* called from JS when the module for a region is instantiated */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void jit_installed(int region, int fnidx) {
    Region* r = &gRegions[region];
    if (fnidx <= 0) { gJit_stats_failed++; return; }
    for (int i = 0; i < r->nblocks; i++) {
        if (r->counts[i] == 0) continue;
        Ent* e = ent_get(r->starts[i], 1);
        if (!e) continue;
        if (e->fn == 0) { e->fn = fnidx; e->blk = (int16_t)i; }
    }
    gJit_stats_regions++;
}

/* helpers imported by generated modules */
uint32_t gJit_fb_hist[65536 * 2];
uint32_t gJit_fb_addr[4096];
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void jit_h_interp(uint32_t addr) {
    {
        const uint8_t* p = G2H(addr);
        int i = 0;
        while ((p[i] == 0x66 || p[i] == 0xf3 || p[i] == 0xf2 || p[i] == 0x64) && i < 4) i++;
        uint32_t key = p[i] == 0x0f ? (0x10000u | p[i + 1]) : ((uint32_t)p[i] << 8) | ((p[i] >= 0xd8 && p[i] <= 0xdf) || p[i] == 0xf7 || p[i] == 0xff || p[i] == 0xd1 || p[i] == 0xd3 || p[i] == 0xc1 || p[i] == 0x81 || p[i] == 0x83 ? ((p[i + 1] >> 3) & 7) | (p[i+1] >= 0xc0 ? 8 : 0) : 0);
        gJit_fb_hist[key & 0x1ffff]++;
        gJit_fb_addr[(addr * 2654435761u) >> 20]++;
    }
    gX86.eip = addr;
    x86_step_one(&gX86);
}
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void jit_h_mat(void) { x86_materialize(&gX86); }
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double jit_h_fild64(uint32_t addr) { return x86_fild64(addr); }
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void jit_h_fist(uint32_t addr, double v, int bits) {
    double r;
    switch ((gX86.fcw >> 10) & 3) {
    case 0: r = __builtin_nearbyint(v); break;
    case 1: r = __builtin_floor(v); break;
    case 2: r = __builtin_ceil(v); break;
    default: r = __builtin_trunc(v); break;
    }
    if (bits == 32) {
        int32_t q = (r >= -2147483648.0 && r <= 2147483647.0) ? (int32_t)r : (int32_t)0x80000000u;
        memcpy(G2H(addr), &q, 4);
        return;
    }
    int64_t q;
    if (!x86_fist64_exact(v, &q)) q = (r >= -9223372036854775808.0 && r < 9223372036854775808.0) ? (int64_t)r : (int64_t)0x8000000000000000ull;
    memcpy(G2H(addr), &q, 8);
}

/* Fast start: the entry addresses of compiled regions are remembered between sessions (web/main.c
 * stores them in localStorage) and compiled ahead of time on the next start, a few milliseconds per
 * frame, so the race begins with warm code instead of compiling while driving. */
static uint32_t gJit_order[MAX_REGIONS];
static int gJit_order_n;
static uint32_t* gJit_pre;
static int gJit_pre_n, gJit_pre_cap, gJit_pre_pos;
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int jit_order_count(void) { return gJit_order_n; }
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
uint32_t* jit_order_ptr(void) { return gJit_order; }

double emu_now_ms(void);
static void try_compile(uint32_t eip);
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void jit_prewarm_add(uint32_t eip) {
    if (!gJit_enabled) return;
    if (eip < 0x400000u || eip >= 0x800000u) return; /* the exe image only */
    if (gJit_pre_n == gJit_pre_cap) {
        gJit_pre_cap = gJit_pre_cap ? gJit_pre_cap * 2 : 1024;
        gJit_pre = realloc(gJit_pre, sizeof(uint32_t) * gJit_pre_cap);
    }
    gJit_pre[gJit_pre_n++] = eip;
}
/* compile queued entries for up to budget_ms; returns how many are left */
int jit_prewarm_step(double budget_ms) {
    double t0 = emu_now_ms();
    while (gJit_pre_pos < gJit_pre_n) {
        uint32_t eip = gJit_pre[gJit_pre_pos++];
        Ent* e = ent_get(eip, 1);
        if (!e || e->fn > 0 || e->pending) continue;
        e->pending = 1;
        try_compile(eip);
        if (emu_now_ms() - t0 >= budget_ms) break;
    }
    if (gJit_pre_pos >= gJit_pre_n && gJit_pre) { free(gJit_pre); gJit_pre = NULL; gJit_pre_n = gJit_pre_cap = gJit_pre_pos = 0; }
    return gJit_pre_n - gJit_pre_pos;
}

static void try_compile(uint32_t eip) {
    if (gNum_regions >= MAX_REGIONS) return;
    if (!compile_region(eip)) return;
    /* patch the eip table address used by budget exits */
    Region* r = &gRegions[gNum_regions];
    r->nblocks = gNum_blocks;
    r->starts = malloc(sizeof(uint32_t) * gNum_blocks);
    r->eip_table = gCur_table;
    r->counts = malloc(sizeof(int) * gNum_blocks);
    for (int i = 0; i < gNum_blocks; i++) { r->starts[i] = gBlocks[i].start; r->counts[i] = gBlocks[i].count; }
    build_module();
    gJit_stats_bytes += (unsigned)gMod.n;
    if (gJit_log) fprintf(stderr, "jit: region %d @%08x blocks=%d insns=%d bytes=%zu\n", gNum_regions, eip, gNum_blocks, gNum_insns, gMod.n);
    for (int i = 0; i < gNum_blocks; i++) {
        if (gBlocks[i].count == 0) continue;
        Ent* pe = ent_get(gBlocks[i].start, 1);
        if (pe) pe->pending = 1;
    }
    int id = gNum_regions++;
    if (gJit_order_n < (int)(sizeof(gJit_order) / sizeof(gJit_order[0]))) gJit_order[gJit_order_n++] = eip;
#ifdef __EMSCRIPTEN__
    jit_js_compile(gMod.p, (int)gMod.n, id);
#else
    (void)id;
    if (getenv("JITDUMP")) {
        char nm[64];
        snprintf(nm, sizeof(nm), "/tmp/jit_%d.wasm", id);
        FILE* f = fopen(nm, "wb");
        fwrite(gMod.p, 1, gMod.n, f);
        fclose(f);
    }
#endif
}

typedef void (*JitFn)(int);


static int jit_dispatch(X86Cpu* c) {
    uint32_t eip = c->eip;
    uint32_t di = (eip ^ (eip >> 14)) & ((1 << DC_BITS) - 1);
    if (gDc[di].eip == eip) {
#ifdef __EMSCRIPTEN__
        ((JitFn)(uintptr_t)gDc[di].fn)(gDc[di].blk);
        return 1;
#endif
    }
    if (eip < 0x400000u || eip >= 0x80000000u) return 0;
    Ent* e = ent_get(eip, 1);
    if (!e) return 0;
    if (e->fn > 0) {
#ifdef __EMSCRIPTEN__
        gDc[di].eip = eip; gDc[di].fn = e->fn; gDc[di].blk = e->blk;
        ((JitFn)(uintptr_t)e->fn)(e->blk);
        return 1;
#else
        return 0;
#endif
    }
    if (!e->pending && ++e->hits >= gJit_threshold) {
        e->pending = 1;
        try_compile(eip);
    }
    return 0;
}

void jit_init(void) {
    gEnts = calloc(ENT_SIZE, sizeof(Ent));
    gX86_jit_dispatch = jit_dispatch;
    gJit_enabled = 1;
}
