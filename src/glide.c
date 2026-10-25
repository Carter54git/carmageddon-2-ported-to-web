/* carma2web: Glide 2.x (as used by 3dfx_win.bdd) implemented as a software rasterizer.
 * Framebuffers are RGB565 like a Voodoo; depth is kept as float. */
#include "emu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define API(n) static uint32_t n##_(X86Cpu* c, uint32_t* a)
#define UNUSED (void)c; (void)a
#define P32(i) ((uint32_t*)G2H(a[i]))
static inline float F(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

uint16_t* gEmu_fb;
int gEmu_fb_w = 640, gEmu_fb_h = 480;
int gEmu_frame_ready;
int gEmu_frames;

#define TEXMEM_SIZE (4u << 20)

static struct {
    int w, h;
    int origin_lower_left;
    int color_format;
    uint16_t* buf[2]; /* 0 = front, 1 = back */
    float* depth;
    int render_buffer;
    int rgb_mask, depth_mask;
    int depth_mode, depth_func;
    int cull_mode;
    /* combine */
    int cc_func, cc_factor, cc_local, cc_other, cc_invert;
    int ac_func, ac_factor, ac_local, ac_other, ac_invert;
    int tc_func;
    float const_r, const_g, const_b, const_a; /* 0..255 */
    int blend_src, blend_dst, blend_asrc, blend_adst;
    int chroma_mode;
    uint16_t chroma_565;
    int fog_mode;
    float fog_r, fog_g, fog_b;
    uint8_t fog_table[64];
    float fog_w[64];
    /* texture */
    uint8_t* texmem;
    uint32_t tex_addr;
    int tex_small_lod, tex_large_lod, tex_aspect, tex_format;
    int tex_w, tex_h; /* largest lod dimensions */
    int clamp_s, clamp_t;
    int filter_bilinear;
    uint32_t palette[256];
    uint32_t ncc_dummy;
    /* lfb */
    int lfb_locked;
    int lfb_write_mode;
    void* lfb_shadow;
    int lfb_buffer;
    uint32_t error_cb;
} G;

#ifdef __EMSCRIPTEN__
extern int glide_gl_init(int w, int h);
extern int glide_gl_tex_create(int w, int h, int levels, const uint32_t* ptrs);
extern void glide_gl_tex_delete(int id);
extern void glide_gl_fog(const float* color, const float* table);
extern void glide_gl_draw(int prim, const float* v, int n, const void* state);
extern void glide_gl_clear(int r, int g, int b, float depth, int cm, int dm);
extern float glide_gl_swap(int pillarbox);
extern float glide_gl_vw(void);
extern void glide_gl_blit(float x, float y, float dw, float dh, int w, int h, const void* rgba);
extern void glide_gl_read(int x, int y, int w, int h, void* rgba);
static int gGlide_gl;
static int gFog_dirty = 1;
static uint32_t gPal_gen;
static void gl_flush(void);
static void gl_flush_if_active(void);
static void texc_invalidate(uint32_t addr, uint32_t size);
#endif
/* ------------------------------------------------------------------ helpers */
static void lod_dims(int lod, int aspect, int* w, int* h) {
    int L = 256 >> lod;
    *w = *h = L;
    if (aspect < 3) { *h = L >> (3 - aspect); }
    else if (aspect > 3) { *w = L >> (aspect - 3); }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}
static int tex_bpp(int fmt) { return fmt >= 8 ? 2 : 1; }
static uint32_t mipmap_size(int small, int large, int aspect, int fmt) {
    uint32_t s = 0;
    for (int l = large; l <= small; l++) {
        int w, h;
        lod_dims(l, aspect, &w, &h);
        s += w * h * tex_bpp(fmt);
    }
    return s;
}
static inline uint32_t color_to_argb(uint32_t col) {
    switch (G.color_format) {
    case 1: return (col & 0xff00ff00u) | ((col >> 16) & 0xff) | ((col & 0xff) << 16); /* ABGR */
    case 2: return (col >> 8) | (col << 24);                                           /* RGBA */
    case 3: return ((col & 0xff) << 24) | ((col >> 24) & 0xff) | ((col >> 8) & 0xff00) | ((col << 8) & 0xff0000); /* BGRA */
    default: return col;
    }
}
static inline uint16_t argb_to_565(uint32_t c) {
    return (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
}
static float decode_wdepth(uint32_t d) {
    d &= 0xffff;
    if (d == 0xffff) return INFINITY;
    if (d == 0) return 0.0f;
    int e = d >> 12, m = d & 0xfff;
    return ldexpf(1.0f + m / 4096.0f, e);
}

/* ------------------------------------------------------------------ init / buffers */
int gGl_dbg;
static char gEv[16384]; static int gEv_n, gEv_lastp, gEv_rep; static char gEv_last[64];
static void ev(const char* fmt, int a, int b, int c, int d) {
    if (!gGl_dbg || gEv_n > 16000) return;
    char t[64];
    snprintf(t, sizeof(t), fmt, a, b, c, d);
    if (strcmp(t, gEv_last) == 0) { gEv_rep++; return; }
    if (gEv_rep) gEv_n += snprintf(gEv + gEv_n, sizeof(gEv) - gEv_n, "x%d", gEv_rep + 1);
    gEv_rep = 0;
    strcpy(gEv_last, t);
    gEv_n += snprintf(gEv + gEv_n, sizeof(gEv) - gEv_n, "%s", t);
}
#ifdef __EMSCRIPTEN__
static void lfb_flush_pending(void);
static int gLfb_pending;
#endif
int gLfb_locks[2];
/* Widescreen. The Glide screen stays 640x480 for the game; the GL target is gWide_vw x 480
 * "Glide pixels". In race frames (wide mode) the 3D scene is stretched over the whole width (the
 * camera aspect is widened to match, see web/main.c) while HUD pieces keep their size and stick
 * to the left / right edge or the centre. Other frames (menus) are drawn 4:3 in the centre. */
float gWide_vw = 640.0f;
int gWide_mode;
static int gF_n3d, gF_hud, gF_biglfb;
enum { ANCHOR_L, ANCHOR_C, ANCHOR_R, ANCHOR_S };
static int hud_anchor(float x0, float y0, float x1, float y1) {
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, w = x1 - x0;
    if (w > 560) return ANCHOR_S;
    if (cy < 70) return cx < 270 ? ANCHOR_L : cx >= 398 ? ANCHOR_R : ANCHOR_C;
    if (cy > 420) return cx < 545 ? ANCHOR_L : ANCHOR_R;
    if (w > 400) return ANCHOR_S;
    if (x1 < 110) return ANCHOR_L;
    if (x0 > 530) return ANCHOR_R;
    return ANCHOR_C;
}
static void anchor_xf(int anchor, float* a, float* b) {
    float ex = gWide_vw - 640.0f;
    *a = 1.0f;
    switch (anchor) {
    case ANCHOR_L: *b = 0; break;
    case ANCHOR_R: *b = ex; break;
    case ANCHOR_S: *a = gWide_vw / 640.0f; *b = 0; break;
    default: *b = ex * 0.5f; break;
    }
}
static int gDbg_n3d, gDbg_nb, gDbg_hud, gDbg_nread, gDbg_nunlock;
static double gDbg_tread, gDbg_tunlock;
static float gDbg_bb[1024][5];
API(grGlideInit) { UNUSED; return 0; }
API(grGlideShutdown) { UNUSED; return 0; }
API(grSstSelect) { UNUSED; return 0; }
API(grSstQueryHardware) {
    UNUSED;
    uint32_t* hw = P32(0);
    memset(hw, 0, 4 * 10); /* only the first SST entry: the caller's struct may be smaller than ours */
    hw[0] = 1;  /* num_sst */
    hw[1] = 0;  /* GR_SSTTYPE_VOODOO */
    hw[2] = 4;  /* fbRam */
    hw[3] = 2;  /* fbiRev */
    hw[4] = 1;  /* nTexelfx */
    hw[5] = 0;  /* sliDetect */
    hw[6] = 1;  /* tmuRev */
    hw[7] = 4;  /* tmuRam */
    return 1;
}
API(grErrorSetCallback) { UNUSED; G.error_cb = a[0]; return 0; }
API(grSstWinOpen) {
    UNUSED;
    int res = a[1];
    int w = 640, h = 480;
    switch (res) {
    case 0: w = 320; h = 200; break;
    case 1: w = 320; h = 240; break;
    case 2: w = 400; h = 256; break;
    case 3: w = 512; h = 384; break;
    case 4: w = 640; h = 200; break;
    case 5: w = 640; h = 350; break;
    case 6: w = 640; h = 400; break;
    case 7: w = 640; h = 480; break;
    case 8: w = 800; h = 600; break;
    case 9: w = 960; h = 720; break;
    case 10: w = 856; h = 480; break;
    case 11: w = 512; h = 256; break;
    case 12: w = 1024; h = 768; break;
    }
    emu_log("grSstWinOpen res=%d (%dx%d) cformat=%d origin=%d buffers=%d aux=%d", res, w, h, a[3], a[4], a[5], a[6]);
    G.w = w;
    G.h = h;
    G.color_format = a[3];
    G.origin_lower_left = a[4] == 1;
    for (int i = 0; i < 2; i++) {
        if (G.buf[i]) gfree(G.buf[i]);
        G.buf[i] = gcalloc(w * h * 2);
    }
    free(G.depth);
    G.depth = malloc(sizeof(float) * w * h);
    for (int i = 0; i < w * h; i++) G.depth[i] = INFINITY;
    G.render_buffer = 1;
    G.rgb_mask = 1;
    G.depth_mask = 1;
    gEmu_fb = G.buf[0];
    gEmu_fb_w = w;
    gEmu_fb_h = h;
#ifdef __EMSCRIPTEN__
    if (!getenv("CARMA2_SOFT") && glide_gl_init(w, h)) {
        gGlide_gl = 1;
        gWide_vw = glide_gl_vw();
        gEmu_fb = NULL; /* presented by WebGL */
        emu_log("Glide: WebGL2 backend");
    }
#endif
    return 1;
}
static int gStw_hint;
API(grHints) { UNUSED; if (a[0] == 0) gStw_hint = a[1]; return 0; }
API(grGammaCorrectionValue) { UNUSED; return 0; }
API(grRenderBuffer) { UNUSED; G.render_buffer = a[0] == 0 ? 0 : 1; return 0; }
API(grBufferNumPending) { UNUSED; return 0; }
API(grBufferClear) {
    UNUSED;
#ifdef __EMSCRIPTEN__
    if (gGlide_gl) {
        lfb_flush_pending();
        gl_flush();
        uint32_t col = color_to_argb(a[0]);
        float d = (G.depth_mode == 2 || G.depth_mode == 4) ? 1.0f : (a[2] & 0xffff) / 65535.0f;
        glide_gl_clear((col >> 16) & 0xff, (col >> 8) & 0xff, col & 0xff, d, G.rgb_mask, G.depth_mask && G.depth_mode != 0);
        return 0;
    }
#endif
    int n = G.w * G.h;
    if (G.rgb_mask) {
        uint16_t c565 = argb_to_565(color_to_argb(a[0]));
        uint16_t* b = G.buf[G.render_buffer];
        for (int i = 0; i < n; i++) b[i] = c565;
    }
    if (G.depth_mask && G.depth_mode != 0) {
        float d = (G.depth_mode == 2 || G.depth_mode == 4) ? decode_wdepth(a[2]) : (float)(a[2] & 0xffff);
        for (int i = 0; i < n; i++) G.depth[i] = d;
    }
    return 0;
}
API(grBufferSwap) {
    UNUSED;
#ifdef __EMSCRIPTEN__
    if (gGlide_gl) {
        lfb_flush_pending();
        gl_flush();
        if (gGl_dbg) {
            if (gEmu_frames % 50 == 0) {
                emu_log("EVDBG f=%d%s", gEmu_frames, gEv);
                emu_log("LFBDBG f=%d reads=%d %.1fms unlocks=%d %.1fms (per 50 frames)", gEmu_frames, gDbg_nread, gDbg_tread, gDbg_nunlock, gDbg_tunlock);
                gDbg_nread = gDbg_nunlock = 0; gDbg_tread = gDbg_tunlock = 0;
                char line[32768]; int o = snprintf(line, sizeof(line), "GLDBG f=%d 3d=%d:", gEmu_frames, gDbg_n3d);
                for (int i = 0; i < gDbg_nb && o < (int)sizeof(line) - 64; i++)
                    o += snprintf(line + o, sizeof(line) - o,  " %d[%d,%d-%d,%d]", (int)gDbg_bb[i][4], (int)gDbg_bb[i][0], (int)gDbg_bb[i][1], (int)gDbg_bb[i][2], (int)gDbg_bb[i][3]);
                emu_log("%s", line);
            }
            gDbg_n3d = 0; gDbg_nb = 0; gDbg_hud = 0; gEv_n = 0; gEv[0] = 0; gEv_lastp = 0; gEv_rep = 0; gEv_last[0] = 0;
        }
        {
            /* widescreen: decide the layout of the next frame from what this one contained */
            int was_wide = gWide_mode;
            gWide_mode = gWide_vw > 640.5f && gF_n3d >= 100 && !gF_biglfb;
            if (gGl_dbg && gWide_mode != was_wide) emu_log("WIDE f=%d mode %d -> %d (n3d=%d biglfb=%d)", gEmu_frames, was_wide, gWide_mode, gF_n3d, gF_biglfb);
            gF_n3d = gF_hud = gF_biglfb = 0;
            gWide_vw = glide_gl_swap(!was_wide);
        }
        gEmu_frame_ready = 1;
        gEmu_frames++;
        emu_yield();
        return 0;
    }
#endif
    uint16_t* t = G.buf[0];
    G.buf[0] = G.buf[1];
    G.buf[1] = t;
    gEmu_fb = G.buf[0];
    gEmu_frame_ready = 1;
    gEmu_frames++;
    emu_yield();
    return 0;
}
API(grColorMask) { UNUSED; G.rgb_mask = a[0] != 0; return 0; }
API(grDepthMask) { UNUSED; G.depth_mask = a[0] != 0; return 0; }
API(grDepthBufferMode) { UNUSED; G.depth_mode = a[0]; return 0; }
API(grDepthBufferFunction) { UNUSED; G.depth_func = a[0]; return 0; }
API(grCullMode) { UNUSED; G.cull_mode = a[0]; return 0; }

/* ------------------------------------------------------------------ combine state */
API(grAlphaBlendFunction) { UNUSED; G.blend_src = a[0]; G.blend_dst = a[1]; G.blend_asrc = a[2]; G.blend_adst = a[3]; return 0; }
API(grAlphaCombine) { UNUSED; G.ac_func = a[0]; G.ac_factor = a[1]; G.ac_local = a[2]; G.ac_other = a[3]; G.ac_invert = a[4]; return 0; }
API(grAlphaTestReferenceValue) { UNUSED; return 0; }
static void set_cc(int f, int fac, int l, int o, int inv) { G.cc_func = f; G.cc_factor = fac; G.cc_local = l; G.cc_other = o; G.cc_invert = inv; }
API(guColorCombineFunction) {
    UNUSED;
    switch (a[0]) {
    case 0: set_cc(0, 0, 0, 0, 0); break;
    case 1: set_cc(1, 0, 1, 2, 0); break;
    case 2: case 3: set_cc(1, 0, 0, 2, 0); break;
    case 4: set_cc(3, 8, 0, 1, 0); break;
    case 5: set_cc(3, 1, 1, 1, 0); break;
    case 6: case 7: set_cc(3, 1, 0, 1, 0); break;
    case 8: set_cc(5, 1, 0, 1, 0); break;
    case 9: set_cc(3, 3, 0, 1, 0); break;
    case 10: set_cc(4, 3, 0, 1, 0); break;
    case 11: set_cc(4, 8, 0, 1, 0); break;
    case 12: set_cc(6, 8, 0, 1, 0); break;
    case 13: set_cc(7, 4, 0, 2, 0); break;
    case 14: set_cc(5, 1, 0, 1, 0); break;
    case 15: set_cc(4, 3, 0, 1, 0); break;
    case 16: set_cc(0, 0, 0, 0, 1); break;
    default: emu_log("guColorCombineFunction(%d)?", a[0]); break;
    }
    return 0;
}
API(grTexCombineFunction) { UNUSED; G.tc_func = a[1]; return 0; }
API(grConstantColorValue) {
    UNUSED;
    uint32_t col = color_to_argb(a[0]);
    G.const_a = (col >> 24) & 0xff; G.const_r = (col >> 16) & 0xff; G.const_g = (col >> 8) & 0xff; G.const_b = col & 0xff;
    return 0;
}
API(grConstantColorValue4) { UNUSED; G.const_a = F(a[0]); G.const_r = F(a[1]); G.const_g = F(a[2]); G.const_b = F(a[3]); return 0; }
API(grChromakeyMode) { UNUSED; G.chroma_mode = a[0]; return 0; }
API(grChromakeyValue) { UNUSED; G.chroma_565 = argb_to_565(color_to_argb(a[0])); return 0; }
API(grFogMode) { UNUSED; G.fog_mode = a[0]; return 0; }
API(grFogColorValue) {
    UNUSED;
    uint32_t col = color_to_argb(a[0]);
#ifdef __EMSCRIPTEN__
    gl_flush_if_active();
    gFog_dirty = 1;
#endif
    G.fog_r = (col >> 16) & 0xff; G.fog_g = (col >> 8) & 0xff; G.fog_b = col & 0xff;
    return 0;
}
API(grFogTable) {
    UNUSED;
    memcpy(G.fog_table, G2H(a[0]), 64);
#ifdef __EMSCRIPTEN__
    gl_flush_if_active();
    gFog_dirty = 1;
#endif
    return 0;
}

/* ------------------------------------------------------------------ textures */
API(grTexMinAddress) { UNUSED; return 0; }
API(grTexMaxAddress) { UNUSED; return TEXMEM_SIZE - 8; }
API(grTexTextureMemRequired) {
    UNUSED;
    uint32_t* info = P32(1);
    uint32_t s = mipmap_size(info[0], info[1], info[2], info[3]);
    return (s + 7) & ~7u;
}
API(grTexDownloadMipMap) {
    UNUSED;
    uint32_t* info = P32(3);
    uint32_t addr = a[1];
    uint32_t s = mipmap_size(info[0], info[1], info[2], info[3]);
    if (addr + s > TEXMEM_SIZE) { emu_log("grTexDownloadMipMap out of range %x+%x", addr, s); return 0; }
#ifdef __EMSCRIPTEN__
    if (gGlide_gl) texc_invalidate(addr, s);
#endif
    memcpy(G.texmem + addr, G2H(info[4]), s);
    return 0;
}
static inline uint32_t decode_px(int fmt, const uint8_t* m, uint32_t idx);
API(grTexSource) {
    UNUSED;
    uint32_t* info = P32(3);
    G.tex_addr = a[1];
    G.tex_small_lod = info[0];
    G.tex_large_lod = info[1];
    G.tex_aspect = info[2];
    G.tex_format = info[3];
    lod_dims(G.tex_large_lod, G.tex_aspect, &G.tex_w, &G.tex_h);
    {
        static long dump_addr = -2;
        if (dump_addr == -2) dump_addr = getenv("TEXDUMP") ? strtol(getenv("TEXDUMP"), 0, 0) : -1;
        if (dump_addr >= 0 && (uint32_t)dump_addr == G.tex_addr && gEmu_frames > 100) {
            { FILE* r = fopen("/tmp/texdump.raw", "wb"); fwrite(G.texmem + G.tex_addr, 1, G.tex_w * G.tex_h * tex_bpp(G.tex_format), r); fclose(r); }
            FILE* f = fopen("/tmp/texdump.ppm", "wb");
            fprintf(f, "P6\n%d %d\n255\n", G.tex_w * 2, G.tex_h);
            for (int y = 0; y < G.tex_h; y++) {
                for (int x = 0; x < G.tex_w; x++) { uint32_t c = decode_px(G.tex_format, G.texmem + G.tex_addr, y * G.tex_w + x); unsigned char rgb[3] = { c >> 16, c >> 8, c }; fwrite(rgb, 1, 3, f); }
                for (int x = 0; x < G.tex_w; x++) { uint32_t c = decode_px(G.tex_format, G.texmem + G.tex_addr, y * G.tex_w + x); unsigned char rgb[3] = { c >> 24, c >> 24, c >> 24 }; fwrite(rgb, 1, 3, f); }
            }
            fclose(f);
            dump_addr = -1;
            emu_log("dumped texture %x fmt %d %dx%d lods %d..%d", G.tex_addr, G.tex_format, G.tex_w, G.tex_h, G.tex_large_lod, G.tex_small_lod);
        }
    }
    return 0;
}
API(grTexDownloadTable) {
    UNUSED;
    if (a[1] == 2) {
        if (memcmp(G.palette, G2H(a[2]), 1024) != 0) {
#ifdef __EMSCRIPTEN__
            gPal_gen++;
#endif
            memcpy(G.palette, G2H(a[2]), 1024);
        }
    }
    return 0;
}
API(grTexMipMapMode) { UNUSED; return 0; }
API(grTexFilterMode) { UNUSED; G.filter_bilinear = a[2] == 1; return 0; }
API(grTexClampMode) { UNUSED; G.clamp_s = a[1]; G.clamp_t = a[2]; return 0; }
API(grTexDetailControl) { UNUSED; return 0; }
API(grTexLodBiasValue) { UNUSED; return 0; }
API(guTexSource) { UNUSED; if (a[0] != 0xFFFFFFFFu) emu_log("guTexSource(%d) unsupported", a[0]); return 0; }

static inline uint32_t decode_px(int fmt, const uint8_t* m, uint32_t idx) {
    switch (fmt) {
    case 0: { uint8_t p = m[idx]; uint32_t r = (p >> 5) * 255 / 7, g = ((p >> 2) & 7) * 255 / 7, b = (p & 3) * 85; return 0xff000000u | (r << 16) | (g << 8) | b; }
    case 2: return (uint32_t)m[idx] << 24 | 0xffffff;
    case 3: { uint32_t i = m[idx]; return 0xff000000u | i * 0x010101u; }
    case 4: { uint8_t p = m[idx]; uint32_t al = (p >> 4) * 17, i = (p & 15) * 17; return (al << 24) | i * 0x010101u; }
    case 5: return G.palette[m[idx]] | 0xff000000u;
    case 8: { uint16_t p = ((const uint16_t*)m)[idx]; uint32_t al = p >> 8, q = p & 0xff; uint32_t r = (q >> 5) * 255 / 7, g = ((q >> 2) & 7) * 255 / 7, b = (q & 3) * 85; return (al << 24) | (r << 16) | (g << 8) | b; }
    case 0xa: { uint16_t p = ((const uint16_t*)m)[idx]; uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31; return 0xff000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2)); }
    case 0xb: { uint16_t p = ((const uint16_t*)m)[idx]; uint32_t r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31; return ((p & 0x8000) ? 0xff000000u : 0) | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2)); }
    case 0xc: { uint16_t p = ((const uint16_t*)m)[idx]; return (((p >> 12) & 15) * 17u << 24) | (((p >> 8) & 15) * 17u << 16) | (((p >> 4) & 15) * 17u << 8) | ((p & 15) * 17u); }
    case 0xd: { uint16_t p = ((const uint16_t*)m)[idx]; return ((uint32_t)(p >> 8) << 24) | (p & 0xff) * 0x010101u; }
    case 0xe: { uint16_t p = ((const uint16_t*)m)[idx]; return ((uint32_t)(p >> 8) << 24) | (G.palette[p & 0xff] & 0xffffff); }
    default: return 0xffff00ffu;
    }
}

static inline uint32_t texel(int u, int v) {
    if (G.clamp_s) { if (u < 0) u = 0; else if (u >= G.tex_w) u = G.tex_w - 1; }
    else u &= G.tex_w - 1;
    if (G.clamp_t) { if (v < 0) v = 0; else if (v >= G.tex_h) v = G.tex_h - 1; }
    else v &= G.tex_h - 1;
    return decode_px(G.tex_format, G.texmem + G.tex_addr, v * G.tex_w + u);
}

/* ------------------------------------------------------------------ rasterizer */
typedef struct {
    float x, y, z, r, g, b, ooz, a, oow, sow, tow, toow;
} Vtx;

static void load_vtx(Vtx* v, uint32_t addr) {
    const float* f = G2H(addr);
    v->x = f[0]; v->y = f[1];
    v->r = f[3]; v->g = f[4]; v->b = f[5];
    v->ooz = f[6]; v->a = f[7]; v->oow = f[8];
    v->sow = f[9]; v->tow = f[10]; v->toow = f[11];
    if (G.origin_lower_left) v->y = G.h - v->y;
}

static inline float clamp255(float v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static inline int depth_test(float d, float cur) {
    switch (G.depth_func) {
    case 0: return 0;
    case 1: return d < cur;
    case 2: return d == cur;
    case 3: return d <= cur;
    case 4: return d > cur;
    case 5: return d != cur;
    case 6: return d >= cur;
    default: return 1;
    }
}

static inline float blend_factor(int f, float sa, float da, float sc, float dc, int is_dst) {
    switch (f) {
    case 0: return 0;
    case 1: return sa;
    case 2: return is_dst ? sc : dc; /* SRC_COLOR for dst, DST_COLOR for src */
    case 3: return da;
    case 4: return 1;
    case 5: return 1 - sa;
    case 6: return is_dst ? 1 - sc : 1 - dc;
    case 7: return 1 - da;
    case 15: { float m = sa < 1 - da ? sa : 1 - da; return is_dst ? 1 : m; }
    default: return 1;
    }
}

static int gUses_texture;
static int gNeeds_blend;

static void update_pipeline_flags(void) {
    gUses_texture = (G.cc_other == 1 && G.cc_func >= 3) || G.cc_factor == 4 || G.cc_factor == 12 ||
                    (G.ac_other == 1 && G.ac_func >= 3) || G.ac_factor == 4 || G.ac_factor == 12;
    gNeeds_blend = !(G.blend_src == 4 && G.blend_dst == 0);
}

static void fog_prepare(void) {
    static int ready;
    if (ready) return;
    for (int i = 0; i < 64; i++) {
        /* guFogTableIndexToW */
        G.fog_w[i] = (float)(pow(2.0, 3.0 + (double)(i >> 2)) / (8 - (i & 3)));
    }
    ready = 1;
}
static inline float fog_factor(float w) {
    if (w <= G.fog_w[0]) return G.fog_table[0] / 255.0f;
    for (int i = 1; i < 64; i++) {
        if (w < G.fog_w[i]) {
            float t = (w - G.fog_w[i - 1]) / (G.fog_w[i] - G.fog_w[i - 1]);
            return (G.fog_table[i - 1] + t * (G.fog_table[i] - G.fog_table[i - 1])) / 255.0f;
        }
    }
    return G.fog_table[63] / 255.0f;
}

static inline void shade_pixel(int x, int y, float r, float g, float b, float al, float depth, float w, float s, float t) {
    int idx = y * G.w + x;
    if (G.depth_mode != 0) {
        if (!depth_test(depth, G.depth[idx])) return;
    }
    float tr = 0, tg = 0, tb = 0, ta = 255;
    if (gUses_texture) {
        int u = (int)floorf(s), v = (int)floorf(t);
        uint32_t tx = texel(u, v);
        ta = (tx >> 24) & 0xff; tr = (tx >> 16) & 0xff; tg = (tx >> 8) & 0xff; tb = tx & 0xff;
    }
    /* color combine */
    float lr, lg, lb, la, or_, og, ob, oa;
    if (G.cc_local == 1) { lr = G.const_r; lg = G.const_g; lb = G.const_b; } else { lr = r; lg = g; lb = b; }
    la = (G.ac_local == 1) ? G.const_a : al;
    if (G.cc_other == 1) { or_ = tr; og = tg; ob = tb; }
    else if (G.cc_other == 2) { or_ = G.const_r; og = G.const_g; ob = G.const_b; }
    else { or_ = r; og = g; ob = b; }
    oa = (G.ac_other == 1) ? ta : (G.ac_other == 2) ? G.const_a : al;
    float fr, fg, fb;
    switch (G.cc_factor) {
    case 0: fr = fg = fb = 0; break;
    case 1: fr = lr / 255; fg = lg / 255; fb = lb / 255; break;
    case 2: fr = fg = fb = oa / 255; break;
    case 3: fr = fg = fb = la / 255; break;
    case 4: fr = fg = fb = ta / 255; break;
    case 8: fr = fg = fb = 1; break;
    case 9: fr = 1 - lr / 255; fg = 1 - lg / 255; fb = 1 - lb / 255; break;
    case 10: fr = fg = fb = 1 - oa / 255; break;
    case 11: fr = fg = fb = 1 - la / 255; break;
    case 12: fr = fg = fb = 1 - ta / 255; break;
    default: fr = fg = fb = 1; break;
    }
    float cr, cg, cb;
    switch (G.cc_func) {
    case 0: cr = cg = cb = 0; break;
    case 1: cr = lr; cg = lg; cb = lb; break;
    case 2: cr = cg = cb = la; break;
    case 3: cr = fr * or_; cg = fg * og; cb = fb * ob; break;
    case 4: cr = fr * or_ + lr; cg = fg * og + lg; cb = fb * ob + lb; break;
    case 5: cr = fr * or_ + la; cg = fg * og + la; cb = fb * ob + la; break;
    case 6: cr = fr * (or_ - lr); cg = fg * (og - lg); cb = fb * (ob - lb); break;
    case 7: cr = fr * (or_ - lr) + lr; cg = fg * (og - lg) + lg; cb = fb * (ob - lb) + lb; break;
    case 8: cr = fr * (or_ - lr) + la; cg = fg * (og - lg) + la; cb = fb * (ob - lb) + la; break;
    case 9: cr = -fr * lr + lr; cg = -fg * lg + lg; cb = -fb * lb + lb; break;
    case 16: cr = -fr * lr + la; cg = -fg * lg + la; cb = -fb * lb + la; break;
    default: cr = lr; cg = lg; cb = lb; break;
    }
    cr = clamp255(cr); cg = clamp255(cg); cb = clamp255(cb);
    if (G.cc_invert) { cr = 255 - cr; cg = 255 - cg; cb = 255 - cb; }
    /* alpha combine */
    float fa;
    switch (G.ac_factor) {
    case 0: fa = 0; break;
    case 1: case 3: fa = la / 255; break;
    case 2: fa = oa / 255; break;
    case 4: fa = ta / 255; break;
    case 8: fa = 1; break;
    case 9: case 11: fa = 1 - la / 255; break;
    case 10: fa = 1 - oa / 255; break;
    case 12: fa = 1 - ta / 255; break;
    default: fa = 1; break;
    }
    float ca;
    switch (G.ac_func) {
    case 0: ca = 0; break;
    case 1: case 2: ca = la; break;
    case 3: ca = fa * oa; break;
    case 4: case 5: ca = fa * oa + la; break;
    case 6: ca = fa * (oa - la); break;
    case 7: case 8: ca = fa * (oa - la) + la; break;
    case 9: case 16: ca = -fa * la + la; break;
    default: ca = la; break;
    }
    ca = clamp255(ca);
    if (G.ac_invert) ca = 255 - ca;
    if (G.chroma_mode) {
        uint16_t c565 = (uint16_t)((((int)cr >> 3) << 11) | (((int)cg >> 2) << 5) | ((int)cb >> 3));
        if (c565 == G.chroma_565) return;
    }
    /* fog */
    if (G.fog_mode & 0xff) {
        float f = (G.fog_mode & 0xff) == 1 ? al / 255 : fog_factor(w);
        cr = cr + f * (G.fog_r - cr);
        cg = cg + f * (G.fog_g - cg);
        cb = cb + f * (G.fog_b - cb);
    }
    uint16_t* dst = &G.buf[G.render_buffer][idx];
    if (gNeeds_blend) {
        uint16_t d = *dst;
        float dr = ((d >> 11) & 31) * (255.0f / 31), dg = ((d >> 5) & 63) * (255.0f / 63), db = (d & 31) * (255.0f / 31);
        float sa = ca / 255, da = 1;
        float sfr = blend_factor(G.blend_src, sa, da, 0, 0, 0);
        float dfr = blend_factor(G.blend_dst, sa, da, 0, 0, 1);
        if (G.blend_src == 2 || G.blend_src == 6 || G.blend_dst == 2 || G.blend_dst == 6) {
            /* colour-dependent factors per channel */
            float sr_ = blend_factor(G.blend_src, sa, da, cr / 255, dr / 255, 0), sg_ = blend_factor(G.blend_src, sa, da, cg / 255, dg / 255, 0), sb_ = blend_factor(G.blend_src, sa, da, cb / 255, db / 255, 0);
            float dr_ = blend_factor(G.blend_dst, sa, da, cr / 255, dr / 255, 1), dg_ = blend_factor(G.blend_dst, sa, da, cg / 255, dg / 255, 1), db_ = blend_factor(G.blend_dst, sa, da, cb / 255, db / 255, 1);
            cr = cr * sr_ + dr * dr_; cg = cg * sg_ + dg * dg_; cb = cb * sb_ + db * db_;
        } else {
            cr = cr * sfr + dr * dfr; cg = cg * sfr + dg * dfr; cb = cb * sfr + db * dfr;
        }
        cr = clamp255(cr); cg = clamp255(cg); cb = clamp255(cb);
    }
    if (G.rgb_mask) *dst = (uint16_t)((((int)cr >> 3) << 11) | (((int)cg >> 2) << 5) | ((int)cb >> 3));
    if (G.depth_mask && G.depth_mode != 0) G.depth[idx] = depth;
}


/* ------------------------------------------------------------------ WebGL backend (browser build) */
#ifdef __EMSCRIPTEN__

typedef struct { int32_t i[26]; float f[4]; } GlState;
#define VB_MAX 30000
static float gVb[VB_MAX * 11];
static int gVb_n, gVb_prim;
static GlState gCur_state;

typedef struct { uint32_t addr, palgen; uint8_t fmt, small, large, aspect; int id; int used; } TexEnt;
#define TEXC 8192
static TexEnt gTexc[TEXC];
static int gTexc_count;

static void gl_flush(void) {
    if (gVb_n == 0) return;
    if (gFog_dirty) {
        float col[3] = { G.fog_r / 255.0f, G.fog_g / 255.0f, G.fog_b / 255.0f }, tab[64];
        for (int i = 0; i < 64; i++) tab[i] = G.fog_table[i] / 255.0f;
        glide_gl_fog(col, tab);
        gFog_dirty = 0;
    }
    glide_gl_draw(gVb_prim, gVb, gVb_n, &gCur_state);
    gVb_n = 0;
}

static void texc_clear_all(void) {
    for (int i = 0; i < TEXC; i++) {
        if (gTexc[i].id > 0) glide_gl_tex_delete(gTexc[i].id);
        gTexc[i].used = 0;
        gTexc[i].id = 0;
    }
    gTexc_count = 0;
}

static int tex_get(void) {
    uint32_t palgen = (G.tex_format == 5 || G.tex_format == 0xe) ? gPal_gen : 0;
    uint32_t h = (G.tex_addr * 2654435761u) ^ (G.tex_format * 97u) ^ (G.tex_large_lod * 13u) ^ (G.tex_aspect * 7u) ^ (palgen * 31u);
    for (int probe = 0; probe < TEXC; probe++) {
        TexEnt* e = &gTexc[(h + probe) & (TEXC - 1)];
        if (!e->used) {
            if (gTexc_count > TEXC * 3 / 4) { gl_flush(); texc_clear_all(); return tex_get(); }
            e->used = 1;
            e->addr = G.tex_addr; e->palgen = palgen; e->fmt = G.tex_format; e->small = G.tex_small_lod; e->large = G.tex_large_lod; e->aspect = G.tex_aspect;
            e->id = 0;
            gTexc_count++;
        } else if (!(e->addr == G.tex_addr && e->palgen == palgen && e->fmt == G.tex_format && e->small == G.tex_small_lod &&
                       e->large == G.tex_large_lod && e->aspect == G.tex_aspect)) {
            continue;
        }
        if (e->id == 0) {
            /* decode all mip levels to RGBA */
            uint32_t ptrs[9];
            int levels = 0;
            const uint8_t* src = G.texmem + G.tex_addr;
            int bw = 0, bh = 0;
            for (int l = G.tex_large_lod; l <= G.tex_small_lod && levels < 9; l++) {
                int w, hh;
                lod_dims(l, G.tex_aspect, &w, &hh);
                if (levels == 0) { bw = w; bh = hh; }
                else {
                    /* GL needs each level to be exactly half of the previous */
                    int ew = bw >> levels, eh = bh >> levels;
                    if (ew < 1) ew = 1;
                    if (eh < 1) eh = 1;
                    if (ew != w || eh != hh) break;
                }
                uint32_t* out = malloc(w * hh * 4);
                for (int i = 0; i < w * hh; i++) {
                    uint32_t c = decode_px(G.tex_format, src, i);
                    out[i] = (c & 0xff00ff00u) | ((c >> 16) & 0xff) | ((c & 0xff) << 16); /* ARGB -> ABGR (RGBA bytes) */
                }
                ptrs[levels++] = H2G(out);
                src += w * hh * tex_bpp(G.tex_format);
            }
            gl_flush();
            e->id = glide_gl_tex_create(bw, bh, levels, ptrs);
            for (int i = 0; i < levels; i++) free(G2H(ptrs[i]));
        }
        return e->id;
    }
    gl_flush();
    texc_clear_all();
    return tex_get();
}

static void texc_invalidate(uint32_t addr, uint32_t size) {
    for (int i = 0; i < TEXC; i++) {
        TexEnt* e = &gTexc[i];
        if (!e->used || e->id <= 0) continue;
        uint32_t es = mipmap_size(e->small, e->large, e->aspect, e->fmt);
        if (e->addr < addr + size && addr < e->addr + es) {
            gl_flush();
            glide_gl_tex_delete(e->id);
            e->id = 0;
        }
    }
}

static void update_pipeline_flags(void);

static void gl_vertex(const Vtx* v, int tdiff, float tsx, float tsy, float xa, float xb) {
    float* o = &gVb[gVb_n * 11];
    o[0] = v->x * xa + xb;
    o[1] = v->y;
    if (G.depth_mode == 2 || G.depth_mode == 4) {
        float d = 1.0f - v->oow;
        o[2] = d < 0 ? 0 : d > 1 ? 1 : d;
    } else {
        float d = v->ooz / 65535.0f;
        o[2] = d < 0 ? 0 : d > 1 ? 1 : d;
    }
    o[3] = v->r; o[4] = v->g; o[5] = v->b; o[6] = v->a;
    o[7] = v->sow * tsx;
    o[8] = v->tow * tsy;
    o[9] = tdiff ? v->toow : v->oow;
    o[10] = v->oow != 0 ? 1.0f / v->oow : 0;
    gVb_n++;
}

static void dbg_bbox(const Vtx** vs, int n, int kind) {
    float x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int i = 0; i < n; i++) {
        if (vs[i]->x < x0) x0 = vs[i]->x; if (vs[i]->x > x1) x1 = vs[i]->x;
        if (vs[i]->y < y0) y0 = vs[i]->y; if (vs[i]->y > y1) y1 = vs[i]->y;
    }
    if (0) {
        float* b = gDbg_bb[gDbg_nb - 1];
        if (x0 < b[0]) b[0] = x0; if (y0 < b[1]) b[1] = y0; if (x1 > b[2]) b[2] = x1; if (y1 > b[3]) b[3] = y1;
        return;
    }
    if (gDbg_nb < 1024) { float* b = gDbg_bb[gDbg_nb++]; b[0] = x0; b[1] = y0; b[2] = x1; b[3] = y1; b[4] = kind; }
}
static float gXa = 1.0f, gXb = 0.0f;
static void gl_prim(int prim, const Vtx** vs, int n) {
    if (gGl_dbg) ev(" P%d", G.lfb_locked, 0, 0, 0);
    if (gLfb_pending >= 0) lfb_flush_pending();
    update_pipeline_flags();
    GlState st;
    memset(&st, 0, sizeof(st));
    st.i[0] = gUses_texture ? tex_get() : 0;
    st.i[1] = G.cc_func; st.i[2] = G.cc_factor; st.i[3] = G.cc_local; st.i[4] = G.cc_other; st.i[5] = G.cc_invert;
    st.i[6] = G.ac_func; st.i[7] = G.ac_factor; st.i[8] = G.ac_local; st.i[9] = G.ac_other; st.i[10] = G.ac_invert;
    st.i[11] = G.blend_src; st.i[12] = G.blend_dst; st.i[13] = G.blend_asrc; st.i[14] = G.blend_adst;
    st.i[15] = G.depth_mode; st.i[16] = G.depth_func; st.i[17] = G.depth_mask; st.i[18] = G.rgb_mask;
    st.i[19] = G.fog_mode; st.i[20] = G.chroma_mode; st.i[21] = G.chroma_565;
    st.i[22] = G.filter_bilinear; st.i[23] = G.clamp_s; st.i[24] = G.clamp_t;
    st.f[0] = G.const_r; st.f[1] = G.const_g; st.f[2] = G.const_b; st.f[3] = G.const_a;
    {
        /* 2D overlays (menus, fonts, HUD) are drawn with w = 1: keep the game's own filtering for them */
        int tdiff2 = (gStw_hint & 2) != 0, flat = 1;
        for (int i = 0; i < n; i++) {
            float q = tdiff2 ? vs[i]->toow : vs[i]->oow;
            if (q != 1.0f) flat = 0;
        }
        st.i[25] = flat;
        if (gWide_vw > 640.5f) {
            int anchor = ANCHOR_C;
            if (gWide_mode) {
                int hud = 0;
                if (gF_n3d && vs[0]->ooz < 8192.0f) {
                    if (flat) gF_hud = 1;
                    hud = gF_hud;
                }
                if (hud) {
                    float x0 = vs[0]->x, x1 = x0, y0 = vs[0]->y, y1 = y0;
                    for (int i = 1; i < n; i++) {
                        if (vs[i]->x < x0) x0 = vs[i]->x; if (vs[i]->x > x1) x1 = vs[i]->x;
                        if (vs[i]->y < y0) y0 = vs[i]->y; if (vs[i]->y > y1) y1 = vs[i]->y;
                    }
                    anchor = hud_anchor(x0, y0, x1, y1);
                } else anchor = ANCHOR_S;
            }
            anchor_xf(anchor, &gXa, &gXb);
        } else { gXa = 1.0f; gXb = 0.0f; }
        if (!flat) gF_n3d++;
        if (gGl_dbg) { if (flat && gDbg_n3d && vs[0]->ooz < 8192) gDbg_hud = 1; if (flat) dbg_bbox(vs, n, (gDbg_n3d ? (vs[0]->ooz < 8192 ? 3 : 4) : 5) * 100000 + st.i[0] * 10); else { gDbg_n3d++; int same = 1; for (int i = 1; i < n; i++) if (vs[i]->oow != vs[0]->oow) same = 0;
            if (same || gDbg_hud) dbg_bbox(vs, n, 7000000 + (int)vs[0]->ooz * 10 + same);
            else if (gDbg_nb && (int)gDbg_bb[gDbg_nb-1][4] == 9) gDbg_bb[gDbg_nb-1][0]++; else if (gDbg_nb < 1024) { float* b = gDbg_bb[gDbg_nb++]; b[0] = 1; b[4] = 9; } } }
    }
    if (gVb_n + n > VB_MAX || prim != gVb_prim || memcmp(&st, &gCur_state, sizeof(st)) != 0) {
        gl_flush();
        gCur_state = st;
        gVb_prim = prim;
    }
    float L = G.tex_w > G.tex_h ? G.tex_w : G.tex_h;
    float tsx = (L / 256.0f) / G.tex_w, tsy = (L / 256.0f) / G.tex_h;
    int tdiff = (gStw_hint & 2) != 0;
    for (int i = 0; i < n; i++) gl_vertex(vs[i], tdiff, tsx, tsy, gXa, gXb);
}

static uint16_t* gLfb_shadow565;
#define LFB_SENTINEL 0x0821
static uint32_t* gLfb_rgba;

static void lfb_composite(int x0, int y0, int w, int h, const uint32_t* rgba, int stride_px) {
    /* find bbox of opaque pixels */
    int minx = w, miny = h, maxx = -1, maxy = -1;
    for (int y = 0; y < h; y++) {
        const uint32_t* row = rgba + y * stride_px;
        for (int x = 0; x < w; x++) {
            if (row[x] >> 24) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }
    }
    if (maxx < 0) return;
    if (gGl_dbg && gDbg_nb < 1024) { float* b = gDbg_bb[gDbg_nb++]; b[0] = x0 + minx; b[1] = y0 + miny; b[2] = x0 + maxx; b[3] = y0 + maxy; b[4] = 2; }
    int bw = maxx - minx + 1, bh = maxy - miny + 1;
    uint32_t* tmp = malloc(bw * bh * 4);
    for (int y = 0; y < bh; y++) memcpy(tmp + y * bw, rgba + (miny + y) * stride_px + minx, bw * 4);
    gl_flush();
    {
        float bx = (float)(x0 + minx), by = (float)(y0 + miny), a = 1.0f, b = 0.0f;
        int anchor = ANCHOR_C;
        if (bw * bh > 640 * 480 / 3) { gF_biglfb = 1; if (gGl_dbg) emu_log("BIGLFB f=%d %d,%d %dx%d mode=%d", gEmu_frames, x0 + minx, y0 + miny, bw, bh, G.lfb_write_mode); }
        if (gWide_mode) anchor = bw * bh > 640 * 480 / 3 ? ANCHOR_S : hud_anchor(bx, by, bx + bw, by + bh);
        if (gWide_vw > 640.5f) anchor_xf(anchor, &a, &b);
        glide_gl_blit(bx * a + b, by, bw * a, bh, bw, bh, tmp);
    }
    free(tmp);
}
static inline uint32_t rgb565_to_rgba(uint16_t p) {
    uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
    return 0xff000000u | (((b << 3) | (b >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((r << 3) | (r >> 2));
}

/* Write locks of the LFB accumulate in one persistent buffer that is composited lazily, before the
 * next draw / clear / read / swap: the map screen locks the LFB about a hundred times per frame. */
static void* gLfb_acc;           /* guest memory, w * h * 4 bytes */
static int gLfb_acc_clean = -9;  /* write mode the buffer is currently clean for */
static int gLfb_pending = -1;    /* write mode with pixels waiting, -1 = nothing */
static int lfb_bpp(int mode) { return (mode == 4 || mode == 5) ? 4 : 2; }
static void lfb_acc_clean_rows(int mode, int y0, int y1) {
    if (mode == 0) {
        for (int y = y0; y < y1; y++) {
            uint16_t* r = (uint16_t*)gLfb_acc + (size_t)y * G.w;
            for (int x = 0; x < G.w; x++) r[x] = LFB_SENTINEL;
        }
    } else {
        int bpp = lfb_bpp(mode);
        memset((uint8_t*)gLfb_acc + (size_t)y0 * G.w * bpp, 0, (size_t)(y1 - y0) * G.w * bpp);
    }
}
static void lfb_flush_pending(void) {
    if (gLfb_pending < 0) return;
    int mode = gLfb_pending, w = G.w, h = G.h, bpp = lfb_bpp(mode);
    gLfb_pending = -1;
    int minx = w, maxx = -1, miny = h, maxy = -1;
    for (int y = 0; y < h; y++) {
        int rx0 = -1, rx1 = -1;
        if (bpp == 4) {
            const uint32_t* r = (const uint32_t*)gLfb_acc + (size_t)y * w;
            for (int x = 0; x < w; x++) if (r[x]) { if (rx0 < 0) rx0 = x; rx1 = x; }
        } else {
            const uint16_t* r = (const uint16_t*)gLfb_acc + (size_t)y * w;
            uint16_t clean = mode == 0 ? LFB_SENTINEL : 0;
            const uint64_t c4 = (uint64_t)clean * 0x0001000100010001ull;
            int x = 0;
            for (; x + 4 <= w; x += 4) {
                uint64_t q;
                memcpy(&q, r + x, 8);
                if (q != c4) break;
            }
            for (; x < w; x++) if (r[x] != clean) { if (rx0 < 0) rx0 = x; rx1 = x; }
        }
        if (rx0 < 0) continue;
        if (rx0 < minx) minx = rx0;
        if (rx1 > maxx) maxx = rx1;
        if (y < miny) miny = y;
        maxy = y;
    }
    if (maxx < 0) return;
    int bw = maxx - minx + 1, bh = maxy - miny + 1;
    if (!gLfb_rgba) gLfb_rgba = malloc(1024 * 768 * 4);
    for (int y = 0; y < bh; y++) {
        uint32_t* o = gLfb_rgba + (size_t)y * bw;
        if (mode == 0) {
            const uint16_t* r = (const uint16_t*)gLfb_acc + (size_t)(miny + y) * w + minx;
            for (int x = 0; x < bw; x++) o[x] = r[x] == LFB_SENTINEL ? 0 : rgb565_to_rgba(r[x]);
        } else if (bpp == 2) {
            const uint16_t* r = (const uint16_t*)gLfb_acc + (size_t)(miny + y) * w + minx;
            for (int x = 0; x < bw; x++) {
                uint16_t q = r[x];
                o[x] = (q == 0 || (mode == 2 && !(q & 0x8000))) ? 0 : rgb565_to_rgba((uint16_t)(((q & 0x7fe0) << 1) | (q & 0x1f)));
            }
        } else {
            const uint32_t* r = (const uint32_t*)gLfb_acc + (size_t)(miny + y) * w + minx;
            for (int x = 0; x < bw; x++) {
                uint32_t c = r[x];
                o[x] = c ? (0xff000000u | ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff)) : 0;
            }
        }
    }
    lfb_acc_clean_rows(mode, miny, maxy + 1);
    gEv_lastp = 0; ev(" F[%d,%d %dx%d]", minx, miny, bw, bh);
    lfb_composite(minx, miny, bw, bh, gLfb_rgba, bw);
}
static void gl_readback565(uint16_t* dst, int x0, int y0, int w, int h, int dst_stride_px) {
    uint32_t* tmp = malloc(w * h * 4);
    gl_flush();
    glide_gl_read(x0, G.h - y0 - h, w, h, tmp);
    for (int y = 0; y < h; y++) {
        const uint32_t* row = tmp + (h - 1 - y) * w;
        for (int x = 0; x < w; x++) {
            uint32_t c = row[x];
            uint32_t r = c & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff;
            dst[y * dst_stride_px + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
    free(tmp);
}
static void gl_flush_if_active(void) { if (gGlide_gl) gl_flush(); }
#define GLIDE_GL_ACTIVE gGlide_gl
#else
#define GLIDE_GL_ACTIVE 0
#endif

typedef struct { float dx, dy, c0; } Plane;
static inline void make_plane(Plane* p, const Vtx* v0, const Vtx* v1, const Vtx* v2, float a0, float a1, float a2, float inv_area) {
    float e1x = v1->x - v0->x, e1y = v1->y - v0->y, e2x = v2->x - v0->x, e2y = v2->y - v0->y;
    float d1 = a1 - a0, d2 = a2 - a0;
    p->dx = (d1 * e2y - d2 * e1y) * inv_area;
    p->dy = (d2 * e1x - d1 * e2x) * inv_area;
    p->c0 = a0 - p->dx * v0->x - p->dy * v0->y;
}
#define PEVAL(p, x, y) ((p).c0 + (p).dx * (x) + (p).dy * (y))

static void draw_triangle(Vtx* v0, Vtx* v1, Vtx* v2) {
    float area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    if (area == 0 || !isfinite(area)) return;
    if (G.cull_mode == 1 && area < 0) return;
    if (G.cull_mode == 2 && area > 0) return;
#ifdef __EMSCRIPTEN__
    if (gGlide_gl) { const Vtx* vs[3] = { v0, v1, v2 }; gl_prim(2, vs, 3); return; }
#endif
    { static int noraster = -1; if (noraster < 0) noraster = getenv("NORASTER") != NULL; if (noraster) return; }
    float inv_area = 1.0f / area;
    float tscale = G.tex_w > G.tex_h ? G.tex_w / 256.0f : G.tex_h / 256.0f;
    Plane pr, pg, pb, pa, pz, pw, ps, pt;
    make_plane(&pr, v0, v1, v2, v0->r, v1->r, v2->r, inv_area);
    make_plane(&pg, v0, v1, v2, v0->g, v1->g, v2->g, inv_area);
    make_plane(&pb, v0, v1, v2, v0->b, v1->b, v2->b, inv_area);
    make_plane(&pa, v0, v1, v2, v0->a, v1->a, v2->a, inv_area);
    make_plane(&pz, v0, v1, v2, v0->ooz, v1->ooz, v2->ooz, inv_area);
    make_plane(&pw, v0, v1, v2, v0->oow, v1->oow, v2->oow, inv_area);
    Plane ptw;
    int tdiff = (gStw_hint & 2) != 0;
    if (tdiff) make_plane(&ptw, v0, v1, v2, v0->toow, v1->toow, v2->toow, inv_area);
    make_plane(&ps, v0, v1, v2, v0->sow * tscale, v1->sow * tscale, v2->sow * tscale, inv_area);
    make_plane(&pt, v0, v1, v2, v0->tow * tscale, v1->tow * tscale, v2->tow * tscale, inv_area);
    int wmode = G.depth_mode == 2 || G.depth_mode == 4;
    /* sort by y */
    const Vtx* s[3] = { v0, v1, v2 };
    if (s[0]->y > s[1]->y) { const Vtx* t = s[0]; s[0] = s[1]; s[1] = t; }
    if (s[1]->y > s[2]->y) { const Vtx* t = s[1]; s[1] = s[2]; s[2] = t; }
    if (s[0]->y > s[1]->y) { const Vtx* t = s[0]; s[0] = s[1]; s[1] = t; }
    int y0 = (int)ceilf(s[0]->y - 0.5f), y1 = (int)ceilf(s[2]->y - 0.5f);
    if (y0 < 0) y0 = 0;
    if (y1 > G.h) y1 = G.h;
    fog_prepare();
    update_pipeline_flags();
    for (int y = y0; y < y1; y++) {
        float yc = y + 0.5f;
        /* long edge s0->s2, short edges s0->s1 / s1->s2 */
        float xa = s[0]->x + (s[2]->x - s[0]->x) * (yc - s[0]->y) / (s[2]->y - s[0]->y);
        float xb;
        if (yc < s[1]->y) xb = s[0]->x + (s[1]->x - s[0]->x) * (yc - s[0]->y) / (s[1]->y - s[0]->y);
        else xb = (s[2]->y != s[1]->y) ? s[1]->x + (s[2]->x - s[1]->x) * (yc - s[1]->y) / (s[2]->y - s[1]->y) : s[1]->x;
        if (xa > xb) { float t = xa; xa = xb; xb = t; }
        int x0 = (int)ceilf(xa - 0.5f), x1 = (int)ceilf(xb - 0.5f);
        if (x0 < 0) x0 = 0;
        if (x1 > G.w) x1 = G.w;
        if (x0 >= x1) continue;
        float xc = x0 + 0.5f;
        float r = PEVAL(pr, xc, yc), g = PEVAL(pg, xc, yc), b = PEVAL(pb, xc, yc), al = PEVAL(pa, xc, yc);
        float z = PEVAL(pz, xc, yc), ow = PEVAL(pw, xc, yc), so = PEVAL(ps, xc, yc), to = PEVAL(pt, xc, yc);
        float tw = tdiff ? PEVAL(ptw, xc, yc) : 0;
        for (int x = x0; x < x1; x++) {
            float w = ow != 0 ? 1.0f / ow : 0;
            float twv = tdiff ? (tw != 0 ? 1.0f / tw : 0) : w;
            shade_pixel(x, y, r, g, b, al, wmode ? w : z, w, so * twv, to * twv);
            r += pr.dx; g += pg.dx; b += pb.dx; al += pa.dx; z += pz.dx; ow += pw.dx; so += ps.dx; to += pt.dx;
            if (tdiff) tw += ptw.dx;
        }
    }
}

static int gDbg_frame = -2, gDbg_count;
API(grDrawTriangle) {
    UNUSED;
    Vtx v[3];
    for (int i = 0; i < 3; i++) load_vtx(&v[i], a[i]);
    static int oowstat = -1; if (oowstat < 0) oowstat = getenv("OOWSTAT") != NULL;
    if (oowstat) { static float mn = 1e30f, mx = -1e30f, zmn = 1e30f, zmx = -1e30f; static int cnt;
        for (int i = 0; i < 3; i++) { if (v[i].oow < mn) mn = v[i].oow; if (v[i].oow > mx) mx = v[i].oow; if (v[i].ooz < zmn) zmn = v[i].ooz; if (v[i].ooz > zmx) zmx = v[i].ooz; }
        if (++cnt % 200000 == 0) emu_log("oow range %g..%g ooz %g..%g depthmode=%d func=%d", mn, mx, zmn, zmx, G.depth_mode, G.depth_func); }
    if (gDbg_frame == -2) { const char* e = getenv("GLIDEDBG"); gDbg_frame = e ? atoi(e) : -1; }
    if (gEmu_frames == gDbg_frame && gDbg_count++ < 100000) {
        emu_log("tri cc=%d/%d/%d/%d/%d ac=%d/%d/%d/%d/%d blend=%d,%d chroma=%d fog=%d depth=%d/%d cull=%d tex@%x fmt=%d %dx%d clamp=%d/%d const=%.0f,%.0f,%.0f,%.0f",
            G.cc_func, G.cc_factor, G.cc_local, G.cc_other, G.cc_invert, G.ac_func, G.ac_factor, G.ac_local, G.ac_other, G.ac_invert,
            G.blend_src, G.blend_dst, G.chroma_mode, G.fog_mode, G.depth_mode, G.depth_func, G.cull_mode, G.tex_addr, G.tex_format, G.tex_w, G.tex_h, G.clamp_s, G.clamp_t,
            G.const_r, G.const_g, G.const_b, G.const_a);
        { const float* f = G2H(a[0]); emu_log("   raw %g %g %g | %g %g %g | %g %g %g | %g %g %g | %g %g %g", f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7],f[8],f[9],f[10],f[11],f[12],f[13],f[14]); }
        for (int i = 0; i < 3; i++) emu_log("   v%d x=%.1f y=%.1f rgba=%.0f,%.0f,%.0f,%.0f ooz=%.1f oow=%.4f s=%.2f t=%.2f", i, v[i].x, v[i].y, v[i].r, v[i].g, v[i].b, v[i].a, v[i].ooz, v[i].oow, v[i].sow / v[i].oow, v[i].tow / v[i].oow);
    }
    draw_triangle(&v[0], &v[1], &v[2]);
    return 0;
}
API(grDrawLine) {
    UNUSED;
    Vtx v0, v1;
    load_vtx(&v0, a[0]);
    load_vtx(&v1, a[1]);
#ifdef __EMSCRIPTEN__
    if (gGlide_gl) { const Vtx* vs[2] = { &v0, &v1 }; gl_prim(1, vs, 2); return 0; }
#endif
    float dx = v1.x - v0.x, dy = v1.y - v0.y;
    int n = (int)(fabsf(dx) > fabsf(dy) ? fabsf(dx) : fabsf(dy)) + 1;
    update_pipeline_flags();
    fog_prepare();
    for (int i = 0; i <= n; i++) {
        float t = (float)i / n;
        int x = (int)(v0.x + dx * t), y = (int)(v0.y + dy * t);
        if (x < 0 || y < 0 || x >= G.w || y >= G.h) continue;
        float ow = v0.oow + (v1.oow - v0.oow) * t, w = ow ? 1 / ow : 0;
        shade_pixel(x, y, v0.r + (v1.r - v0.r) * t, v0.g + (v1.g - v0.g) * t, v0.b + (v1.b - v0.b) * t, v0.a + (v1.a - v0.a) * t,
            (G.depth_mode == 2 || G.depth_mode == 4) ? w : v0.ooz + (v1.ooz - v0.ooz) * t, w, 0, 0);
    }
    return 0;
}
API(grDrawPoint) {
    UNUSED;
    Vtx v;
    load_vtx(&v, a[0]);
#ifdef __EMSCRIPTEN__
    if (gGlide_gl) { const Vtx* vs[1] = { &v }; gl_prim(0, vs, 1); return 0; }
#endif
    int x = (int)v.x, y = (int)v.y;
    if (x < 0 || y < 0 || x >= G.w || y >= G.h) return 0;
    update_pipeline_flags();
    fog_prepare();
    float w = v.oow ? 1 / v.oow : 0;
    shade_pixel(x, y, v.r, v.g, v.b, v.a, (G.depth_mode == 2 || G.depth_mode == 4) ? w : v.ooz, w, 0, 0);
    return 0;
}

/* ------------------------------------------------------------------ linear frame buffer */
static uint16_t* lfb_target(uint32_t buffer) { return G.buf[buffer == 0 ? 0 : 1]; }

API(grLfbLock) {
    UNUSED;
    uint32_t type = a[0], buffer = a[1], wmode = a[2];
    { static int n; if (n++ < 6 || (n % 200) == 0) emu_log("grLfbLock type=%d buffer=%d wmode=%d origin=%d pipeline=%d frame=%d ret=%08x", type, buffer, wmode, a[3], a[4], gEmu_frames, *(uint32_t*)G2H(c->reg[X86_ESP] - 4)); }
    uint32_t* info = P32(5);
    uint16_t* b = lfb_target(buffer);
    gLfb_locks[type & 1]++;
#ifdef __EMSCRIPTEN__
    if (gGlide_gl) {
        G.lfb_locked = 1;
        G.lfb_buffer = buffer;
        G.lfb_write_mode = (type & 1) ? (int)(wmode == 0xff ? 0 : wmode) : -1;
        gEv_lastp = 0; ev(" L%d/%d/%d", G.lfb_write_mode, (int)a[4], (int)buffer, 0);
        if (G.lfb_write_mode < 0) {
            lfb_flush_pending();
            if (G.lfb_shadow) gfree(G.lfb_shadow);
            G.lfb_shadow = gmalloc(G.w * G.h * 2);
            double t0 = emu_now_ms();
            gl_readback565(G.lfb_shadow, 0, 0, G.w, G.h, G.w);
            gDbg_tread += emu_now_ms() - t0; gDbg_nread++;
            info[1] = H2G(G.lfb_shadow);
            info[2] = G.w * 2;
            info[3] = 0;
        } else {
            int mode = G.lfb_write_mode;
            if (gLfb_pending >= 0 && gLfb_pending != mode) lfb_flush_pending();
            if (!gLfb_acc) gLfb_acc = gmalloc(G.w * G.h * 4);
            if (gLfb_acc_clean != mode) { lfb_acc_clean_rows(mode, 0, G.h); gLfb_acc_clean = mode; }
            info[1] = H2G(gLfb_acc);
            info[2] = G.w * lfb_bpp(mode);
            info[3] = mode;
        }
        info[4] = 0;
        return 1;
    }
#endif
    G.lfb_locked = 1;
    G.lfb_buffer = buffer;
    G.lfb_write_mode = (type & 1) ? (int)wmode : 0;
    if ((type & 1) && wmode != 0 && wmode != 0xff) {
        /* non-565 writes go to a shadow buffer converted at unlock */
        int bpp = (wmode == 4 || wmode == 5) ? 4 : 2;
        if (G.lfb_shadow) gfree(G.lfb_shadow);
        G.lfb_shadow = gcalloc(G.w * G.h * bpp);
        info[1] = H2G(G.lfb_shadow);
        info[2] = G.w * bpp;
        info[3] = wmode;
    } else {
        info[1] = H2G(b);
        info[2] = G.w * 2;
        info[3] = 0;
    }
    info[4] = 0;
    return 1;
}
API(grLfbUnlock) {
    UNUSED;
#ifdef __EMSCRIPTEN__
    if (gGlide_gl) {
        if (G.lfb_write_mode >= 0) gLfb_pending = G.lfb_write_mode;
        if (G.lfb_shadow) { gfree(G.lfb_shadow); G.lfb_shadow = NULL; }
        G.lfb_locked = 0;
        gDbg_nunlock++;
        ev(" U", 0, 0, 0, 0);
        return 1;
    }
#endif
    if (G.lfb_shadow && G.lfb_write_mode != 0) {
        uint16_t* b = lfb_target(G.lfb_buffer);
        int n = G.w * G.h;
        if (G.lfb_write_mode == 1 || G.lfb_write_mode == 2) {
            const uint16_t* s = G.lfb_shadow;
            for (int i = 0; i < n; i++) {
                uint16_t p = s[i];
                if (G.lfb_write_mode == 2 && !(p & 0x8000)) continue;
                b[i] = (uint16_t)(((p & 0x7fe0) << 1) | ((p & 0x0200) ? 0x20 : 0) | (p & 0x1f));
            }
        } else {
            const uint32_t* s = G.lfb_shadow;
            for (int i = 0; i < n; i++) b[i] = argb_to_565(s[i]);
        }
        gfree(G.lfb_shadow);
        G.lfb_shadow = NULL;
    }
    G.lfb_locked = 0;
    return 1;
}
API(grLfbReadRegion) {
    UNUSED;
    uint16_t* b = lfb_target(a[0]);
    uint32_t x0 = a[1], y0 = a[2], w = a[3], h = a[4], stride = a[5];
    uint8_t* dst = G2H(a[6]);
#ifdef __EMSCRIPTEN__
    if (gGlide_gl) {
        lfb_flush_pending();
        if (x0 + w > (uint32_t)G.w) w = G.w - x0;
        if (y0 + h > (uint32_t)G.h) h = G.h - y0;
        gl_readback565((uint16_t*)dst, x0, y0, w, h, stride / 2);
        return 1;
    }
#endif
    for (uint32_t y = 0; y < h && y0 + y < (uint32_t)G.h; y++) {
        memcpy(dst + y * stride, b + (y0 + y) * G.w + x0, w * 2);
    }
    return 1;
}
API(grLfbWriteRegion) {
    UNUSED;
    uint16_t* b = lfb_target(a[0]);
    uint32_t x0 = a[1], y0 = a[2], fmt = a[3], w = a[4], h = a[5], stride = a[6];
    const uint8_t* src = G2H(a[7]);
#ifdef __EMSCRIPTEN__
    if (gGlide_gl) {
        lfb_flush_pending();
        if (x0 + w > (uint32_t)G.w) w = G.w - x0;
        if (y0 + h > (uint32_t)G.h) h = G.h - y0;
        uint32_t* tmp = malloc(w * h * 4);
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                uint32_t o;
                if (fmt == 0) o = rgb565_to_rgba(((const uint16_t*)(src + y * stride))[x]);
                else if (fmt == 1 || fmt == 2) {
                    uint16_t q = ((const uint16_t*)(src + y * stride))[x];
                    o = (fmt == 2 && !(q & 0x8000)) ? 0 : rgb565_to_rgba((uint16_t)(((q & 0x7fe0) << 1) | (q & 0x1f)));
                } else {
                    uint32_t cc = ((const uint32_t*)(src + y * stride))[x];
                    o = 0xff000000u | ((cc & 0xff) << 16) | (cc & 0xff00) | ((cc >> 16) & 0xff);
                }
                tmp[y * w + x] = o;
            }
        }
        lfb_composite(x0, y0, w, h, tmp, w);
        free(tmp);
        return 1;
    }
#endif
    for (uint32_t y = 0; y < h && y0 + y < (uint32_t)G.h; y++) {
        for (uint32_t x = 0; x < w && x0 + x < (uint32_t)G.w; x++) {
            uint16_t p;
            if (fmt == 0) p = ((const uint16_t*)(src + y * stride))[x];
            else if (fmt == 1 || fmt == 2) {
                uint16_t q = ((const uint16_t*)(src + y * stride))[x];
                if (fmt == 2 && !(q & 0x8000)) continue;
                p = (uint16_t)(((q & 0x7fe0) << 1) | (q & 0x1f));
            } else p = argb_to_565(((const uint32_t*)(src + y * stride))[x]);
            b[(y0 + y) * G.w + x0 + x] = p;
        }
    }
    return 1;
}

static const EmuApiDef kGlide[] = {
    { "glide2x", "_grGlideInit@0", grGlideInit_, 0 },
    { "glide2x", "_grGlideShutdown@0", grGlideShutdown_, 0 },
    { "glide2x", "_grSstSelect@4", grSstSelect_, 1 },
    { "glide2x", "_grSstQueryHardware@4", grSstQueryHardware_, 1 },
    { "glide2x", "_grErrorSetCallback@4", grErrorSetCallback_, 1 },
    { "glide2x", "_grSstWinOpen@28", grSstWinOpen_, 7 },
    { "glide2x", "_grHints@8", grHints_, 2 },
    { "glide2x", "_grGammaCorrectionValue@4", grGammaCorrectionValue_, 1 },
    { "glide2x", "_grRenderBuffer@4", grRenderBuffer_, 1 },
    { "glide2x", "_grBufferNumPending@0", grBufferNumPending_, 0 },
    { "glide2x", "_grBufferClear@12", grBufferClear_, 3 },
    { "glide2x", "_grBufferSwap@4", grBufferSwap_, 1 },
    { "glide2x", "_grColorMask@8", grColorMask_, 2 },
    { "glide2x", "_grDepthMask@4", grDepthMask_, 1 },
    { "glide2x", "_grDepthBufferMode@4", grDepthBufferMode_, 1 },
    { "glide2x", "_grDepthBufferFunction@4", grDepthBufferFunction_, 1 },
    { "glide2x", "_grCullMode@4", grCullMode_, 1 },
    { "glide2x", "_grAlphaBlendFunction@16", grAlphaBlendFunction_, 4 },
    { "glide2x", "_grAlphaCombine@20", grAlphaCombine_, 5 },
    { "glide2x", "_grAlphaTestReferenceValue@4", grAlphaTestReferenceValue_, 1 },
    { "glide2x", "_guColorCombineFunction@4", guColorCombineFunction_, 1 },
    { "glide2x", "_grTexCombineFunction@8", grTexCombineFunction_, 2 },
    { "glide2x", "_grConstantColorValue@4", grConstantColorValue_, 1 },
    { "glide2x", "_grConstantColorValue4@16", grConstantColorValue4_, 4 },
    { "glide2x", "_grChromakeyMode@4", grChromakeyMode_, 1 },
    { "glide2x", "_grChromakeyValue@4", grChromakeyValue_, 1 },
    { "glide2x", "_grFogMode@4", grFogMode_, 1 },
    { "glide2x", "_grFogColorValue@4", grFogColorValue_, 1 },
    { "glide2x", "_grFogTable@4", grFogTable_, 1 },
    { "glide2x", "_grTexMinAddress@4", grTexMinAddress_, 1 },
    { "glide2x", "_grTexMaxAddress@4", grTexMaxAddress_, 1 },
    { "glide2x", "_grTexTextureMemRequired@8", grTexTextureMemRequired_, 2 },
    { "glide2x", "_grTexDownloadMipMap@16", grTexDownloadMipMap_, 4 },
    { "glide2x", "_grTexSource@16", grTexSource_, 4 },
    { "glide2x", "_grTexDownloadTable@12", grTexDownloadTable_, 3 },
    { "glide2x", "_grTexMipMapMode@12", grTexMipMapMode_, 3 },
    { "glide2x", "_grTexFilterMode@12", grTexFilterMode_, 3 },
    { "glide2x", "_grTexClampMode@12", grTexClampMode_, 3 },
    { "glide2x", "_grTexDetailControl@16", grTexDetailControl_, 4 },
    { "glide2x", "_grTexLodBiasValue@8", grTexLodBiasValue_, 2 },
    { "glide2x", "_guTexSource@4", guTexSource_, 1 },
    { "glide2x", "_grDrawTriangle@12", grDrawTriangle_, 3 },
    { "glide2x", "_grDrawLine@8", grDrawLine_, 2 },
    { "glide2x", "_grDrawPoint@4", grDrawPoint_, 1 },
    { "glide2x", "_grLfbLock@24", grLfbLock_, 6 },
    { "glide2x", "_grLfbUnlock@8", grLfbUnlock_, 2 },
    { "glide2x", "_grLfbReadRegion@28", grLfbReadRegion_, 7 },
    { "glide2x", "_grLfbWriteRegion@32", grLfbWriteRegion_, 8 },
    { NULL, NULL, NULL, 0 },
};

void emu_glide_init(void) {
    G.texmem = calloc(1, TEXMEM_SIZE);
    G.depth_func = 7;
    G.blend_src = 4;
    G.blend_dst = 0;
    G.cc_func = 1;
    G.ac_func = 1;
    emu_register_apis(kGlide);
}
