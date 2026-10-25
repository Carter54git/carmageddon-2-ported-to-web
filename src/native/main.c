#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int emu_start(const char* root);
int emu_step(void);
void emu_dump_api_stats(void);
static void fault(X86Cpu* c, const char* why) {
    fprintf(stderr, "x86 fault: %s\n", why);
    fprintf(stderr, "eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n", c->reg[0], c->reg[1], c->reg[2], c->reg[3], c->reg[4], c->reg[5], c->reg[6], c->reg[7]);
    FILE* f = fopen("/tmp/fault.bin", "wb");
    fwrite((uint8_t*)(uintptr_t)(c->eip - 256), 1, 320, f);
    fclose(f);
    fprintf(stderr, "stack:");
    for (int i = 0; i < 16; i++) fprintf(stderr, " %08x", ((uint32_t*)(uintptr_t)c->reg[4])[i]);
    fprintf(stderr, "\n");
}
static void dump(const char* path) {
    FILE* f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", gEmu_fb_w, gEmu_fb_h);
    for (int i = 0; i < gEmu_fb_w * gEmu_fb_h; i++) {
        uint16_t p = gEmu_fb[i];
        unsigned char rgb[3] = { (p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3 };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
/* scripted input: "frame:key:down,..." via KEYS env, e.g. "100:28:1,102:28:0" (scancodes, vk derived) */
static int sc2vk(int sc) {
    switch (sc) { case 28: return 13; case 1: return 27; case 0x48: return 38; case 0x50: return 40; case 0x4b: return 37; case 0x4d: return 39; case 57: return 32; default: return 0; }
}
static void on_break(X86Cpu* c) {
    fprintf(stderr, "BREAK %08x eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x st0=%g st1=%g\n", c->eip, c->reg[0], c->reg[1], c->reg[2], c->reg[3], c->reg[4], c->reg[5], c->reg[6], c->reg[7], x86_st(c, 0), x86_st(c, 1));
    const char* ma = getenv("BRKADDR");
    if (ma) { float* f = (float*)(uintptr_t)strtoul(ma, 0, 0); fprintf(stderr, "  mem[%s]: %g %g %08x\n", ma, f[0], f[1], *(uint32_t*)f); }
    const char* m = getenv("BRKMEM");
    if (m) { uint32_t reg = strtoul(m, 0, 0); float* f = (float*)(uintptr_t)c->reg[reg & 7]; fprintf(stderr, "  mem[r%d]:", reg & 7); for (int i = 0; i < 16; i++) fprintf(stderr, " %g", f[i]); fprintf(stderr, "\n"); }
}
void jit_init(void);
extern int gJit_log;
extern int (*gEmu_music_tracks)(void);
extern void (*gEmu_music_play)(int track);
static int native_tracks(void) { return getenv("MUSICTRACKS") ? atoi(getenv("MUSICTRACKS")) : 0; }
static void native_play(int t) { fprintf(stderr, "MUSIC play track %d at frame %d\n", t, gEmu_frames); }
int main(int argc, char** argv) {
    gEmu_music_tracks = native_tracks;
    gEmu_music_play = native_play;
    if (getenv("JIT")) { jit_init(); gJit_log = getenv("JITLOG") != NULL; }
    { extern uint32_t gX86_watch; if (getenv("WWATCH")) gX86_watch = strtoul(getenv("WWATCH"), 0, 0); }
    if (getenv("BRK")) { gX86_break = strtoul(getenv("BRK"), 0, 0); gX86_break_fn = on_break; }
    gX86_fault = fault;
    if (getenv("TRACE")) gEmu_trace = 1;
    if (getenv("STATS")) gEmu_profile = 1;
    static uint32_t* prof_buf; if (getenv("PROF")) prof_buf = calloc(0x400000 >> 4, 4);
    int prof_from = getenv("PROFFROM") ? atoi(getenv("PROFFROM")) : 0; double prof_t0 = 0; uint64_t prof_i0 = 0; int prof_f0 = 0;
    if (emu_start(argc > 1 ? argv[1] : "carma2") != 0) return 1;
    double t0 = emu_now_ms();
    double maxms = getenv("MAXMS") ? atof(getenv("MAXMS")) : 1e12;
    int steps = 0, maxsteps = getenv("STEPS") ? atoi(getenv("STEPS")) : 200;
    int every = getenv("SHOT") ? atoi(getenv("SHOT")) : 50;
    const char* keys = getenv("KEYS");
    int last_frame = -1;
    while (steps < maxsteps && emu_now_ms() - t0 < maxms && emu_step() == 0) {
        steps++;
        {
            static FILE* wf = (FILE*)1; static double wl; static long wn;
            if (wf == (FILE*)1) { const char* w = getenv("WAV"); wf = w ? fopen(w, "wb") : NULL; if (wf) { char hdr[44] = { 0 }; fwrite(hdr, 1, 44, wf); } wl = emu_now_ms(); }
            if (wf) {
                extern void emu_audio_mix(float*, float*, int, int);
                double now = emu_now_ms();
                int n = (int)((now - wl) * 22050 / 1000);
                if (n > 0) {
                    static float L[65536], R[65536]; if (n > 65536) n = 65536;
                    emu_audio_mix(L, R, n, 22050);
                    for (int i = 0; i < n; i++) { int16_t sm[2] = { (int16_t)(L[i] * 32767), (int16_t)(R[i] * 32767) }; fwrite(sm, 2, 2, wf); }
                    wn += n; wl += n * 1000.0 / 22050;
                    /* patch header */
                    long pos = ftell(wf);
                    uint32_t d = (uint32_t)(wn * 4);
                    unsigned char h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E','f','m','t',' ', 16,0,0,0, 1,0, 2,0, 0x22,0x56,0,0, 0x88,0x58,0x01,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
                    uint32_t riff = d + 36; memcpy(h + 4, &riff, 4); memcpy(h + 40, &d, 4);
                    fseek(wf, 0, SEEK_SET); fwrite(h, 1, 44, wf); fseek(wf, pos, SEEK_SET);
                }
            }
        }
        {
            static double lt = -1; static int lf; static uint64_t li;
            double now = emu_now_ms();
            if (lt < 0) { lt = now; lf = gEmu_frames; li = gX86.icount; }
            if (now - lt >= 5000 && getenv("TIMELINE")) {
                extern unsigned gJit_stats_regions, gJit_stats_bytes;
                fprintf(stderr, "T %5.0fs frame %5d  %5.1f fps  %6.1f MIPS  regions %u  %.1f MB\n", (now - t0) / 1000, gEmu_frames,
                    (gEmu_frames - lf) * 1000.0 / (now - lt), (gX86.icount - li) / (now - lt) / 1000.0, gJit_stats_regions, gJit_stats_bytes / 1e6);
                lt = now; lf = gEmu_frames; li = gX86.icount;
            }
        }
        static const char *env_keyms = (const char*)1, *env_keystep, *env_watch;
        if (env_keyms == (const char*)1) { env_keyms = getenv("KEYMS"); env_keystep = getenv("KEYSTEP"); env_watch = getenv("WATCH"); }
        if (prof_buf && !gX86_prof && gEmu_frames >= prof_from) { extern uint32_t gJit_entries; fprintf(stderr, "jit entries at profile start: %u\n", gJit_entries); gX86_prof = prof_buf; prof_t0 = emu_now_ms(); prof_i0 = gX86.icount; prof_f0 = gEmu_frames; gEmu_profile = 1; }
        if (env_watch) {
            static uint32_t last = 0xdeadbeef; static uint32_t wa;
            if (!wa) wa = strtoul(getenv("WATCH"), 0, 0);
            uint32_t v = 0;
            if (wa >= 0x10000000u) v = *(uint32_t*)(uintptr_t)wa;
            if (v != last) { fprintf(stderr, "WATCH %08x = %08x at step %d frame %d eip %08x\n", wa, v, steps, gEmu_frames, gX86.eip); last = v; }
        }
        {
            /* KEYF="frame:scancode,...": press when the frame is reached, release 200ms later */
            static const char* kf = (const char*)1;
            static int kdone[64]; static double krel[64]; static int ksc[64];
            if (kf == (const char*)1) kf = getenv("KEYF");
            if (kf) {
                const char* p = kf; int idx = 0;
                double now = emu_now_ms();
                while (*p && idx < 64) {
                    int fr, sc;
                    if (sscanf(p, "%d:%d", &fr, &sc) == 2) {
                        if (!kdone[idx] && gEmu_frames >= fr) { kdone[idx] = 1; krel[idx] = now + 200; ksc[idx] = sc; emu_post_key(sc2vk(sc), sc, 1); fprintf(stderr, "key %d down at frame %d\n", sc, gEmu_frames); }
                        else if (kdone[idx] == 1 && now >= krel[idx]) { kdone[idx] = 2; emu_post_key(sc2vk(ksc[idx]), ksc[idx], 0); }
                    }
                    p = strchr(p, ','); idx++;
                    if (!p) break;
                    p++;
                }
            }
        }
        {
            static const char* kh = (const char*)1; static int held;
            if (kh == (const char*)1) kh = getenv("KEYHOLD");
            int fr, sc;
            if (kh && !held && sscanf(kh, "%d:%d", &fr, &sc) == 2 && gEmu_frames >= fr) { held = 1; emu_post_key(sc2vk(sc), sc, 1); fprintf(stderr, "hold %d from frame %d\n", sc, gEmu_frames); }
        }
        if (env_keyms) {
            static int done[64];
            double now = emu_now_ms() - t0;
            const char* p = env_keyms;
            int idx = 0;
            while (*p && idx < 64) {
                int ms, sc, dn;
                if (sscanf(p, "%d:%d:%d", &ms, &sc, &dn) == 3 && !done[idx] && now >= ms) { done[idx] = 1; emu_post_key(sc2vk(sc), sc, dn); fprintf(stderr, "key %d %s at %.0fms frame %d\n", sc, dn ? "down" : "up", now, gEmu_frames); }
                p = strchr(p, ',');
                idx++;
                if (!p) break;
                p++;
            }
        }
        if (env_keystep) {
            const char* p = env_keystep;
            while (*p) {
                int st, sc, dn;
                if (sscanf(p, "%d:%d:%d", &st, &sc, &dn) == 3 && st == steps) { emu_post_key(sc2vk(sc), sc, dn); fprintf(stderr, "key %d %s at step %d frame %d\n", sc, dn ? "down" : "up", steps, gEmu_frames); }
                p = strchr(p, ',');
                if (!p) break;
                p++;
            }
        }
        if (gEmu_frames != last_frame) {
            last_frame = gEmu_frames;
            if (keys) {
                const char* p = keys;
                while (*p) {
                    int fr, sc, dn;
                    if (sscanf(p, "%d:%d:%d", &fr, &sc, &dn) == 3 && fr == gEmu_frames) emu_post_key(sc2vk(sc), sc, dn);
                    p = strchr(p, ',');
                    if (!p) break;
                    p++;
                }
            }
            if (gEmu_fb && every > 0 && gEmu_frames % every == 0) {
                char nm[256];
                snprintf(nm, sizeof(nm), "shots/emu_%04d.ppm", gEmu_frames);
                dump(nm);
            }
        }
    }
    double dt = emu_now_ms() - t0;
    fprintf(stderr, "steps=%d frames=%d icount=%llu time=%.0fms (%.1f MIPS) eip=%08x\n", steps, gEmu_frames, (unsigned long long)gX86.icount, dt, gX86.icount / dt / 1000.0, gX86.eip);
    { extern unsigned gJit_stats_regions, gJit_stats_insns, gJit_stats_fallback, gJit_stats_bytes, gJit_stats_failed;
      if (gJit_stats_regions) fprintf(stderr, "jit: %u regions, %u insns, %u fallbacks, %u bytes, %u failed\n", gJit_stats_regions, gJit_stats_insns, gJit_stats_fallback, gJit_stats_bytes, gJit_stats_failed); }
    if (getenv("FBHIST")) {
        extern uint32_t gJit_fb_hist[];
        uint32_t tot = 0; for (int i = 0; i < 131072; i++) tot += gJit_fb_hist[i];
        fprintf(stderr, "fallback executions: %u\n", tot);
        for (int n = 0; n < 20; n++) { int best = 0; for (int i = 1; i < 131072; i++) if (gJit_fb_hist[i] > gJit_fb_hist[best]) best = i; if (!gJit_fb_hist[best]) break; fprintf(stderr, "  key %05x: %u\n", best, gJit_fb_hist[best]); gJit_fb_hist[best] = 0; }
    }
    if (gEmu_fb) dump("shots/emu_last.ppm");
    if (getenv("STATS")) emu_dump_api_stats();
    { extern uint32_t gJit_entries; static uint32_t e0; (void)e0; fprintf(stderr, "jit entries total: %u\n", gJit_entries); }
    if (gX86_prof) fprintf(stderr, "profiled: %d frames, %.1f ms/frame, %.2f Minstr/frame\n", gEmu_frames - prof_f0, (emu_now_ms() - prof_t0) / (gEmu_frames - prof_f0 + 1e-9), (gX86.icount - prof_i0) / 1e6 / (gEmu_frames - prof_f0 + 1e-9));
    if (gX86_prof) { FILE* f = fopen(getenv("PROF"), "wb"); fwrite(gX86_prof, 4, 0x400000 >> 4, f); fclose(f); }
    return 0;
}
