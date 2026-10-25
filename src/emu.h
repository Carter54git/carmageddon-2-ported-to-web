/* carma2web: runtime for executing CARMA2_HW.EXE under the x86 interpreter. */
#ifndef CARMA2WEB_EMU_H
#define CARMA2WEB_EMU_H

#include "x86cpu.h"

#include <stddef.h>
#include <stdint.h>

#define G2H(a) ((void*)(uintptr_t)(a))
#define H2G(p) ((uint32_t)(uintptr_t)(p))

#define EMU_TRAP_BASE 0xFFFE0000u
#define EMU_TRAP_STRIDE 8u
#define EMU_RET_SENTINEL 0xFFFFFFF0u

/* API handler: args points at the first stack argument. Returns EAX. */
typedef uint32_t (*EmuApiFn)(X86Cpu* c, uint32_t* args);

typedef struct {
    const char* dll;
    const char* name;
    EmuApiFn fn;
    int nargs; /* stdcall: number of dword args popped; -1 = cdecl */
} EmuApiDef;

/* guest-addressable heap */
void* gmalloc(size_t n);
void* gcalloc(size_t n);
void gfree(void* p);
void* grealloc(void* p, size_t n);
size_t gsize(void* p);
char* gstrdup(const char* s);

/* trap registry */
uint32_t emu_trap(const char* dll, const char* name, EmuApiFn fn, int nargs);
uint32_t emu_resolve_import(const char* dll, const char* name); /* never 0 */
void emu_register_apis(const EmuApiDef* defs);

/* call guest code at addr with n dword args (pushed right to left) */
uint32_t emu_call(uint32_t addr, int n, const uint32_t* args);
uint32_t emu_call4(uint32_t addr, uint32_t a, uint32_t b, uint32_t c, uint32_t d);

/* request the outermost interpreter loop to return to the host */
void emu_yield(void);
extern int gEmu_depth;
extern int gEmu_trace;
extern int gEmu_profile;

/* PE */
int emu_load_exe(const char* path, uint32_t* entry);

/* host services */
double emu_now_ms(void);
const char* emu_root(void); /* host directory with the game files */
void emu_log(const char* fmt, ...);
void emu_fatal(const char* fmt, ...);

/* file helpers (case-insensitive DOS path -> host path) */
int emu_host_path(const char* dospath, char* out, size_t outsz);

/* subsystems */
void emu_win32_init(void);
void emu_glide_init(void);
void emu_dx_init(void);

/* input/video interface to the host shell */
extern uint8_t gEmu_keys[256];       /* DirectInput scan code state */
void emu_post_key(int vk, int scancode, int down);
void emu_post_mouse(int x, int y, int buttons);
extern uint16_t* gEmu_fb;            /* 640x480 RGB565 front buffer for presentation */
extern int gEmu_fb_w, gEmu_fb_h;
extern int gEmu_frame_ready;
extern int gEmu_frames;

#endif
