/* carma2web: process setup and the run loop shared by native and web hosts */
#include "emu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void emu_init(void);
static char gRoot[512];
const char* emu_root(void) { return gRoot; }

int emu_start(const char* root) {
    snprintf(gRoot, sizeof(gRoot), "%s", root);
    emu_init();
    emu_win32_init();
    emu_dx_init();
    emu_glide_init();
    char exe[600];
    snprintf(exe, sizeof(exe), "%s/carma2_HW.exe", root);
    uint32_t entry;
    if (emu_load_exe(exe, &entry) != 0) return -1;
    X86Cpu* c = &gX86;
    memset(c, 0, sizeof(*c));
    c->fcw = 0x27f;
    uint32_t stack_size = 4u << 20;
    uint8_t* stack = gcalloc(stack_size);
    c->reg[X86_ESP] = H2G(stack) + stack_size - 64;
    uint32_t* tib = gcalloc(4096);
    tib[0] = 0xFFFFFFFFu;
    tib[1] = H2G(stack) + stack_size;
    tib[2] = H2G(stack);
    tib[6] = H2G(tib);
    c->fs_base = H2G(tib);
    x86_push32(c, EMU_RET_SENTINEL);
    c->eip = entry;
    return 0;
}

/* run until the guest yields (end of a frame) or exits; returns 0 to continue, -1 on exit */
int emu_step(void) {
    X86Cpu* c = &gX86;
    c->yield = 0;
    int r = x86_run(c, EMU_RET_SENTINEL);
    if (r == 1) return 0;
    if (c->eip == EMU_RET_SENTINEL) emu_log("guest returned from entry point");
    return -1;
}
