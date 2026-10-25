/*
 * carma2web hybrid runtime: a small i386 + x87 interpreter that executes code
 * from the original CARMA2_HW.EXE image. Guest addresses are identical to
 * WebAssembly linear memory addresses (the exe image is loaded at its preferred
 * base 0x400000, and everything allocated by the C side is directly usable).
 */
#ifndef CARMA2WEB_X86CPU_H
#define CARMA2WEB_X86CPU_H

#include <stdint.h>

enum {
    X86_EAX = 0,
    X86_ECX = 1,
    X86_EDX = 2,
    X86_EBX = 3,
    X86_ESP = 4,
    X86_EBP = 5,
    X86_ESI = 6,
    X86_EDI = 7,
};

#define X86_CF 0x0001u
#define X86_PF 0x0004u
#define X86_AF 0x0010u
#define X86_ZF 0x0040u
#define X86_SF 0x0080u
#define X86_DF 0x0400u
#define X86_OF 0x0800u

typedef struct X86Cpu {
    uint32_t reg[8];
    uint32_t eip;
    uint32_t eflags;
    /* x87 */
    double st[8];
    int top;
    uint16_t fsw; /* condition codes C0..C3 kept in their x87 bit positions */
    uint16_t fcw;
    uint32_t fs_base;
    uint64_t icount;
    int halted;
    int yield; /* set by a hook to make the outermost x86_run return 1 */
    int single; /* x86_step_one in progress */
    /* lazy flags left by compiled code: kind, operands, result, operand size, carry-in */
    int lz_kind, lz_size;
    uint32_t lz_a, lz_b, lz_r, lz_cin;
} X86Cpu;

extern X86Cpu gX86;
extern uint32_t gX86_break; /* debug: call gX86_break_fn when eip hits this */
extern void (*gX86_break_fn)(X86Cpu*);
extern uint32_t* gX86_prof; /* optional EIP sample histogram, 16-byte buckets from 0x400000 */

/* Handler invoked when guest code transfers control to a hooked address.
 * On entry ESP points to the return address (as after a CALL). The handler
 * must pop the return address (and any callee-cleaned arguments) and set EIP. */
typedef void (*X86HookFn)(X86Cpu* cpu, uint32_t target);

/* Returns a hook for target, or NULL to execute guest code at target. */
extern X86HookFn (*gX86_hook_lookup)(uint32_t target);

/* Called for an undecodable/unsupported instruction. */
extern void (*gX86_fault)(X86Cpu* cpu, const char* why);

/* Run until EIP == stop_eip. Returns 0 on success. */
int x86_run(X86Cpu* cpu, uint32_t stop_eip);
int x86_step_one(X86Cpu* cpu); /* execute exactly one (non control-transfer) instruction */

enum { X86_LZ_NONE, X86_LZ_ADD, X86_LZ_SUB, X86_LZ_LOGIC, X86_LZ_INC, X86_LZ_DEC, X86_LZ_ADC, X86_LZ_SBB, X86_LZ_SHL, X86_LZ_SHR, X86_LZ_SAR, X86_LZ_NEG, X86_LZ_MUL, X86_LZ_SHD };
void x86_materialize(X86Cpu* cpu); /* turn lazy flags into eflags */
/* optional compiled-code dispatcher: returns 1 if it executed code at cpu->eip */
extern int (*gX86_jit_dispatch)(X86Cpu* cpu);
extern int gX86_native_hooks;

/* Convenience: push a 32-bit value on the guest stack. */
static inline void x86_push32(X86Cpu* cpu, uint32_t v) {
    cpu->reg[X86_ESP] -= 4;
    *(uint32_t*)(uintptr_t)cpu->reg[X86_ESP] = v;
}
static inline uint32_t x86_pop32(X86Cpu* cpu) {
    uint32_t v = *(uint32_t*)(uintptr_t)cpu->reg[X86_ESP];
    cpu->reg[X86_ESP] += 4;
    return v;
}
static inline double x86_st(X86Cpu* cpu, int i) {
    return cpu->st[(cpu->top + i) & 7];
}
void x86_fpush(X86Cpu* cpu, double v);
double x86_fild64(uint32_t addr);
int x86_fist64_exact(double d, int64_t* out);
double x86_fpop(X86Cpu* cpu);

#endif
