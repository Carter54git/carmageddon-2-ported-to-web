/* carma2web: browser host for the hybrid runtime (CARMA2_HW.EXE under the x86 interpreter) */
#include "emu.h"

#include <emscripten.h>
#include <emscripten/html5.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int emu_start(const char* root);
int emu_step(void);
void jit_init(void);
int jit_prewarm_step(double budget_ms);
int jit_order_count(void);
uint32_t* jit_order_ptr(void);


/* ---- network: the game's IPX datagrams go through a WebSocket relay (serve.ps1 / relay.py: /net) ---- */
EM_JS(int, js_net_open, (uint8_t* node), {
    var N = Module.__net;
    if (!N) {
        N = Module.__net = { q: [], out: [], ws: null, node: null, fails: 0 };
        var id = null;
        try { id = sessionStorage.getItem('carma2.node'); } catch (e) {}
        if (!id || id.length !== 12) {
            id = '02';
            for (var i = 0; i < 5; i++) id += ('0' + ((Math.random() * 256) | 0).toString(16)).slice(-2);
            try { sessionStorage.setItem('carma2.node', id); } catch (e) {}
        }
        N.node = id;
        var q = new URLSearchParams(location.search);
        var room = q.get('room') || 'lan';
        var url = q.get('relay') || (typeof window !== 'undefined' && window.CARMA2_RELAY) || ((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/net');
        url += (url.indexOf('?') >= 0 ? '&' : '?') + 'room=' + encodeURIComponent(room) + '&node=' + id;
        N.status = function (t) { if (Module.onNet) Module.onNet(t); };
        N.connect = function () {
            var ws;
            try { ws = new WebSocket(url); } catch (e) { err('net: ' + e); return; }
            ws.binaryType = 'arraybuffer';
            ws.onopen = function () {
                out('net: connected to ' + url);
                N.ws = ws; N.fails = 0;
                for (var i = 0; i < N.out.length; i++) ws.send(N.out[i]);
                N.out = [];
                N.status('online');
            };
            ws.onmessage = function (e) {
                if (typeof e.data === 'string') { N.status(e.data); return; }
                if (N.q.length < 2048) N.q.push(new Uint8Array(e.data));
            };
            ws.onclose = function () {
                if (N.ws === ws) N.ws = null;
                N.fails++;
                if (N.fails === 1) err('net: no relay at ' + url + ' (start the game with serve.cmd for network play)');
                N.status('offline');
                setTimeout(N.connect, N.fails < 5 ? 2000 : 10000);
            };
            ws.onerror = function () {};
        };
        N.connect();
    }
    for (var i = 0; i < 6; i++) HEAPU8[node + i] = parseInt(N.node.substr(i * 2, 2), 16);
    return 1;
});
EM_JS(void, js_net_send, (const uint8_t* node, const uint8_t* data, int len), {
    var N = Module.__net;
    if (!N) return;
    var b = new Uint8Array(6 + len);
    b.set(HEAPU8.subarray(node, node + 6), 0);
    b.set(HEAPU8.subarray(data, data + len), 6);
    if (N.ws && N.ws.readyState === 1) N.ws.send(b);
    else if (N.out.length < 256) N.out.push(b);
});
EM_JS(int, js_net_recv, (uint8_t* node, uint8_t* data, int max), {
    var N = Module.__net;
    if (!N || !N.q.length) return -1;
    var b = N.q.shift();
    var n = Math.min(b.length - 6, max);
    if (n < 0) return -1;
    HEAPU8.set(b.subarray(0, 6), node);
    HEAPU8.set(b.subarray(6, 6 + n), data);
    return n;
});
EM_JS(int, js_net_pending, (void), { var N = Module.__net; return N && N.q.length ? N.q[0].length - 6 : 0; });
EM_JS(char*, js_player_name, (void), {
    var q = new URLSearchParams(location.search), n = q.get('name');
    try { if (n) localStorage.setItem('carma2.name', n); else n = localStorage.getItem('carma2.name'); } catch (e) {}
    if (!n) { n = 'Player' + ((Math.random() * 900 + 100) | 0); try { localStorage.setItem('carma2.name', n); } catch (e) {} }
    n = n.replace(/[^A-Za-z0-9 \-]/g, '').slice(0, 15) || 'Player';
    var len = lengthBytesUTF8(n) + 1, p = _malloc(len);
    stringToUTF8(n, p, len);
    return p;
});

/* JIT fast start: remember which code got compiled, compile it ahead next time */
#define JIT_STORE_KEY "carma2.jit.v1"
EM_JS(void, js_jit_load, (void), {
    var list = null;
    try { list = JSON.parse(localStorage.getItem("carma2.jit.v1") || "null"); } catch (e) {}
    if (!list || !list.length) return;
    for (var i = 0; i < list.length; i++) _jit_prewarm_add(list[i] >>> 0);
    out('jit: ' + list.length + ' regions to compile ahead (fast start)');
    Module.__jitSaved = list.length;
});
EM_JS(void, js_jit_save, (void), {
    var n = _jit_order_count();
    if (!n || n <= (Module.__jitSaved | 0)) return;
    var p = _jit_order_ptr() >> 2, list = new Array(n);
    for (var i = 0; i < n; i++) list[i] = HEAPU32[p + i];
    try { localStorage.setItem('carma2.jit.v1', JSON.stringify(list)); Module.__jitSaved = n; } catch (e) {}
});
EM_JS(void, js_jit_hook_unload, (void), {
    var f = function () { try { _jit_save_now(); } catch (e) {} };
    window.addEventListener('pagehide', f);
    document.addEventListener('visibilitychange', function () { if (document.visibilityState === 'hidden') f(); });
});
EMSCRIPTEN_KEEPALIVE void jit_save_now(void) { js_jit_save(); }
static int gJit_on, gJit_left;
static double gJit_save_t;
extern unsigned gJit_stats_regions, gJit_stats_insns, gJit_stats_fallback, gJit_stats_bytes, gJit_stats_failed;

static void fault(X86Cpu* c, const char* why) {
    fprintf(stderr, "x86 fault: %s\n", why);
    fprintf(stderr, "eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n", c->reg[0], c->reg[1], c->reg[2], c->reg[3],
        c->reg[4], c->reg[5], c->reg[6], c->reg[7]);
}

static uint32_t* gRgba;

/* WebAudio output: a ScriptProcessor pulls mixed samples from emu_audio_mix */
EM_JS(void, js_audio_init, (void), {
    var AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return;
    var ctx;
    try { ctx = new AC({ latencyHint: 'interactive' }); } catch (e) { return; }
    var n = 2048;
    var node = ctx.createScriptProcessor(n, 0, 2);
    var bufL = _malloc(n * 4), bufR = _malloc(n * 4);
    node.onaudioprocess = function (ev) {
        var out = ev.outputBuffer, frames = out.length;
        _emu_audio_mix(bufL, bufR, frames, ctx.sampleRate);
        out.getChannelData(0).set(HEAPF32.subarray(bufL >> 2, (bufL >> 2) + frames));
        out.getChannelData(1).set(HEAPF32.subarray(bufR >> 2, (bufR >> 2) + frames));
    };
    var master = ctx.createWaveShaper(), curve = new Float32Array(2049);
    for (var i = 0; i < curve.length; i++) {
        var x = i / 1024 - 1, ax = Math.abs(x), knee = 0.6;
        var y = ax <= knee ? ax : knee + (1 - knee) * Math.tanh((ax - knee) / (1 - knee));
        curve[i] = x < 0 ? -y : y;
    }
    master.curve = curve;
    master.oversample = '2x';
    master.connect(ctx.destination);
    var fx = ctx.createGain();
    fx.gain.value = Module.fxGain || 0.8;
    node.connect(fx);
    fx.connect(master);
    Module.__audio = ctx;
    Module.__master = master;
    var resume = function () { if (ctx.state !== 'running') ctx.resume(); };
    ['keydown', 'mousedown', 'touchstart', 'pointerdown'].forEach(function (ev) { window.addEventListener(ev, resume, true); });
});

EM_JS(void, js_present, (const uint32_t* rgba, int w, int h), {
    var canvas = Module["canvas"];
    if (!canvas) return;
    if (canvas.width != w || canvas.height != h) { canvas.width = w; canvas.height = h; }
    if (!Module.__ctx) Module.__ctx = canvas.getContext("2d");
    if (!Module.__img || Module.__img.width != w || Module.__img.height != h) Module.__img = Module.__ctx.createImageData(w, h);
    Module.__img.data.set(HEAPU8.subarray(rgba, rgba + w * h * 4));
    Module.__ctx.putImageData(Module.__img, 0, 0);
});

static void present(void) {
    int n = gEmu_fb_w * gEmu_fb_h;
    if (!gRgba) gRgba = malloc(1024 * 768 * 4);
    for (int i = 0; i < n; i++) {
        uint32_t p = gEmu_fb[i];
        uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
        gRgba[i] = 0xff000000u | (((b << 3) | (b >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((r << 3) | (r >> 2));
    }
    js_present(gRgba, gEmu_fb_w, gEmu_fb_h);
}

static int gDead;
static double gMips_t0;
static uint64_t gMips_i0;
static int gMips_f0;

/* keys from JS (gamepad, touch controls) */
EMSCRIPTEN_KEEPALIVE void emu_js_key(int sc, int vk, int down) { emu_post_key(vk, sc, down); }

/* touch as a mouse (menus): fx, fy = position inside the canvas, 0..1 */
EMSCRIPTEN_KEEPALIVE void emu_js_pointer(float fx, float fy, int buttons) {
    extern float gWide_vw;
    float vw = gWide_vw > 0 ? gWide_vw : (float)gEmu_fb_w;
    int x = (int)(fx * vw - (vw - gEmu_fb_w) * 0.5f), y = (int)(fy * gEmu_fb_h);
    if (x < 0) x = 0;
    if (x >= gEmu_fb_w) x = gEmu_fb_w - 1;
    if (y < 0) y = 0;
    if (y >= gEmu_fb_h) y = gEmu_fb_h - 1;
    emu_post_mouse(x, y, buttons);
}

/* Gamepad (standard mapping): stick/d-pad = arrows, RT/A = accelerate... see table */
EM_JS(void, js_poll_gamepad, (void), {
    if (!navigator.getGamepads) return;
    var pads = navigator.getGamepads(), gp = null;
    for (var i = 0; i < pads.length; i++) if (pads[i] && pads[i].connected) { gp = pads[i]; break; }
    var S = Module.__pad || (Module.__pad = {});
    function set(name, on, sc, vk) {
        on = !!on;
        if (S[name] === on) return;
        S[name] = on;
        _emu_js_key(sc, vk, on ? 1 : 0);
    }
    var b = function (n) { return gp && gp.buttons[n] && (gp.buttons[n].pressed || gp.buttons[n].value > 0.4); };
    var ax = gp ? gp.axes[0] || 0 : 0, ay = gp ? gp.axes[1] || 0 : 0;
    set('up', b(12) || b(7) || ay < -0.5, 0xc8, 0x26);    /* d-pad up / RT / stick up: accelerate */
    set('down', b(13) || b(6) || ay > 0.5, 0xd0, 0x28);   /* d-pad down / LT / stick down: brake/reverse */
    set('left', b(14) || ax < -0.4, 0xcb, 0x25);
    set('right', b(15) || ax > 0.4, 0xcd, 0x27);
    set('enter', b(0), 0x1c, 0x0d);                        /* A: select */
    set('esc', b(1) || b(9), 0x01, 0x1b);                  /* B / Start: back, pause */
    set('space', b(2), 0x39, 0x20);                        /* X: handbrake */
    set('repair', b(3), 0x0e, 0x08);                       /* Y: repair (backspace) */
    set('camera', b(4), 0x2e, 0x43);                       /* LB: change view (C) */
    set('recover', b(5), 0xd2, 0x2d);                      /* RB: recover the car (Insert) */
});

/* Widescreen: widen the horizontal field of view of the race cameras to the screen shape
 * (Hor+). carma2_HW.exe: gCamera_list[2] at 0x74a950, gWidth/gHeight at 0x701fc4/0x701fc0;
 * br_actor::type_data at +0x5c, br_camera::aspect at +0x10. */
static int gCam_dbg;
static void widen_cameras(void) {
    extern float gWide_vw;
    static float base[3], written[3];
    /* relative to the 640-pixel Glide screen (the game's gWidth changes while the map is shown).
     * Always widened: the sky model is built from the camera aspect when a race starts. */
    float k = gWide_vw > gEmu_fb_w ? gWide_vw / (float)gEmu_fb_w : 1.0f;
    int gw = gEmu_fb_w;
    /* gCamera_list[0..1] at 0x74a950, gRearview_camera at 0x758e60 */
    static const uint32_t slots[3] = { 0x74a950, 0x74a954, 0x758e60 };
    for (int i = 0; i < 3; i++) {
        uint32_t actor = *(uint32_t*)G2H(slots[i]);
        if (actor < 0x10000 || actor >= 0x7ff00000u) continue;
        uint32_t cam = *(uint32_t*)G2H(actor + 0x5c);
        if (cam < 0x10000 || cam >= 0x7ff00000u) continue;
        float* pa = (float*)G2H(cam + 0x10);
        if (!(*pa > 0.05f && *pa < 50.0f)) continue;
        if (*pa != written[i]) {
            if (gCam_dbg) emu_log("CAMSET slot%d cam=%08x game set %.4f (we wrote %.4f) k=%.3f gw=%d", i, cam, *pa, written[i], k, gw);
            base[i] = *pa; /* the game set a new aspect */
        }
        written[i] = base[i] * k;
        *pa = written[i];
    }
}

static void frame(void) {
    js_poll_gamepad();
    widen_cameras();
    if (gDead) return;
    if (gJit_left) gJit_left = jit_prewarm_step(6.0);
    double t0 = emu_now_ms();
    gEmu_frame_ready = 0;
    /* run until the guest presents a frame, or ~30ms passed (keeps the page responsive while loading) */
    while (!gEmu_frame_ready && emu_now_ms() - t0 < 30.0) {
        if (emu_step() != 0) {
            gDead = 1;
            EM_ASM({ if (Module.onEmuExit) Module.onEmuExit(); });
            break;
        }
    }
    if (gEmu_frame_ready && gEmu_fb) present();
    if (gCam_dbg && gEmu_frame_ready && (gEmu_frames % 30) == 0) {
        uint32_t cur = *(uint32_t*)G2H(0x74b4fc), l0 = *(uint32_t*)G2H(0x74a950), l1 = *(uint32_t*)G2H(0x74a954);
        float asp = -1;
        if (cur > 0x10000 && cur < 0x7ff00000u) { uint32_t cam = *(uint32_t*)G2H(cur + 0x5c); if (cam > 0x10000 && cam < 0x7ff00000u) asp = *(float*)G2H(cam + 0x10); }
        extern int gLfb_locks[2];
        emu_log("CAMDBG f=%d gCamera=%08x list=%08x,%08x aspect=%.3f locks r=%d w=%d", gEmu_frames, cur, l0, l1, asp, gLfb_locks[0], gLfb_locks[1]);
        gLfb_locks[0] = gLfb_locks[1] = 0;
    }
    double now = emu_now_ms();
    if (now - gMips_t0 > 2000) {
        double mips = (gX86.icount - gMips_i0) / (now - gMips_t0) / 1000.0;
        double fps = (gEmu_frames - gMips_f0) * 1000.0 / (now - gMips_t0);
        EM_ASM({ if (Module.onStats) Module.onStats($0, $1, $2, $3, $4); }, mips, fps, gJit_stats_regions, gJit_stats_fallback, gJit_stats_failed);
        gMips_t0 = now;
        gMips_i0 = gX86.icount;
        gMips_f0 = gEmu_frames;
    }
    if (gJit_on && now - gJit_save_t > 15000) {
        gJit_save_t = now;
        js_jit_save();
    }
}

/* JS KeyboardEvent.code -> (DirectInput scancode, Windows VK) */
static const struct { const char* code; int sc; int vk; } kKeys[] = {
    { "Escape", 0x01, 0x1b }, { "Digit1", 0x02, '1' }, { "Digit2", 0x03, '2' }, { "Digit3", 0x04, '3' }, { "Digit4", 0x05, '4' },
    { "Digit5", 0x06, '5' }, { "Digit6", 0x07, '6' }, { "Digit7", 0x08, '7' }, { "Digit8", 0x09, '8' }, { "Digit9", 0x0a, '9' },
    { "Digit0", 0x0b, '0' }, { "Minus", 0x0c, 0xbd }, { "Equal", 0x0d, 0xbb }, { "Backspace", 0x0e, 0x08 }, { "Tab", 0x0f, 0x09 },
    { "KeyQ", 0x10, 'Q' }, { "KeyW", 0x11, 'W' }, { "KeyE", 0x12, 'E' }, { "KeyR", 0x13, 'R' }, { "KeyT", 0x14, 'T' },
    { "KeyY", 0x15, 'Y' }, { "KeyU", 0x16, 'U' }, { "KeyI", 0x17, 'I' }, { "KeyO", 0x18, 'O' }, { "KeyP", 0x19, 'P' },
    { "BracketLeft", 0x1a, 0xdb }, { "BracketRight", 0x1b, 0xdd }, { "Enter", 0x1c, 0x0d }, { "ControlLeft", 0x1d, 0x11 },
    { "KeyA", 0x1e, 'A' }, { "KeyS", 0x1f, 'S' }, { "KeyD", 0x20, 'D' }, { "KeyF", 0x21, 'F' }, { "KeyG", 0x22, 'G' },
    { "KeyH", 0x23, 'H' }, { "KeyJ", 0x24, 'J' }, { "KeyK", 0x25, 'K' }, { "KeyL", 0x26, 'L' }, { "Semicolon", 0x27, 0xba },
    { "Quote", 0x28, 0xde }, { "Backquote", 0x29, 0xc0 }, { "ShiftLeft", 0x2a, 0x10 }, { "Backslash", 0x2b, 0xdc },
    { "KeyZ", 0x2c, 'Z' }, { "KeyX", 0x2d, 'X' }, { "KeyC", 0x2e, 'C' }, { "KeyV", 0x2f, 'V' }, { "KeyB", 0x30, 'B' },
    { "KeyN", 0x31, 'N' }, { "KeyM", 0x32, 'M' }, { "Comma", 0x33, 0xbc }, { "Period", 0x34, 0xbe }, { "Slash", 0x35, 0xbf },
    { "ShiftRight", 0x36, 0x10 }, { "NumpadMultiply", 0x37, 0x6a }, { "AltLeft", 0x38, 0x12 }, { "Space", 0x39, 0x20 },
    { "CapsLock", 0x3a, 0x14 }, { "F1", 0x3b, 0x70 }, { "F2", 0x3c, 0x71 }, { "F3", 0x3d, 0x72 }, { "F4", 0x3e, 0x73 },
    { "F5", 0x3f, 0x74 }, { "F6", 0x40, 0x75 }, { "F7", 0x41, 0x76 }, { "F8", 0x42, 0x77 }, { "F9", 0x43, 0x78 },
    { "F10", 0x44, 0x79 }, { "Numpad7", 0x47, 0x67 }, { "Numpad8", 0x48, 0x68 }, { "Numpad9", 0x49, 0x69 },
    { "NumpadSubtract", 0x4a, 0x6d }, { "Numpad4", 0x4b, 0x64 }, { "Numpad5", 0x4c, 0x65 }, { "Numpad6", 0x4d, 0x66 },
    { "NumpadAdd", 0x4e, 0x6b }, { "Numpad1", 0x4f, 0x61 }, { "Numpad2", 0x50, 0x62 }, { "Numpad3", 0x51, 0x63 },
    { "Numpad0", 0x52, 0x60 }, { "NumpadDecimal", 0x53, 0x6e }, { "F11", 0x57, 0x7a }, { "F12", 0x58, 0x7b },
    { "NumpadEnter", 0x9c, 0x0d }, { "ControlRight", 0x9d, 0x11 }, { "NumpadDivide", 0xb5, 0x6f }, { "AltRight", 0xb8, 0x12 },
    { "Home", 0xc7, 0x24 }, { "ArrowUp", 0xc8, 0x26 }, { "PageUp", 0xc9, 0x21 }, { "ArrowLeft", 0xcb, 0x25 },
    { "ArrowRight", 0xcd, 0x27 }, { "End", 0xcf, 0x23 }, { "ArrowDown", 0xd0, 0x28 }, { "PageDown", 0xd1, 0x22 },
    { "Insert", 0xd2, 0x2d }, { "Delete", 0xd3, 0x2e },
};

static EM_BOOL on_key(int type, const EmscriptenKeyboardEvent* e, void* ud) {
    (void)ud;
    if (e->repeat) return EM_TRUE;
    for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++) {
        if (strcmp(e->code, kKeys[i].code) == 0) {
            int down = type == EMSCRIPTEN_EVENT_KEYDOWN;
            emu_post_key(kKeys[i].vk, kKeys[i].sc, down);
            return EM_TRUE;
        }
    }
    return EM_FALSE;
}

static int gButtons;
static EM_BOOL on_mouse(int type, const EmscriptenMouseEvent* e, void* ud) {
    (void)ud;
    double cw, ch;
    emscripten_get_element_css_size("#canvas", &cw, &ch);
    /* the canvas is gWide_vw Glide pixels wide; menus are drawn 4:3 in its centre */
    extern float gWide_vw;
    float vw = gWide_vw > 0 ? gWide_vw : (float)gEmu_fb_w;
    int x = (int)(e->targetX * vw / (cw > 0 ? cw : vw) - (vw - gEmu_fb_w) * 0.5f);
    int y = (int)(e->targetY * gEmu_fb_h / (ch > 0 ? ch : 480));
    if (x < 0) x = 0;
    if (x >= gEmu_fb_w) x = gEmu_fb_w - 1;
    int bit = e->button == 0 ? 1 : e->button == 2 ? 2 : 0;
    if (type == EMSCRIPTEN_EVENT_MOUSEDOWN) gButtons |= bit;
    if (type == EMSCRIPTEN_EVENT_MOUSEUP) gButtons &= ~bit;
    emu_post_mouse(x, y, gButtons);
    return EM_TRUE;
}

/* ---- persistence: files the game writes are mirrored into IndexedDB (/persist) ---- */
extern void (*gEmu_file_written)(const char* host_path);
EM_JS(void, js_persist_file, (const char* path), {
    var p = UTF8ToString(path);
    if (p.indexOf('/carma2/') !== 0) return;
    var rel = p.substring('/carma2/'.length);
    try {
        var parts = rel.split('/'), dir = '/persist';
        for (var i = 0; i < parts.length - 1; i++) { dir += '/' + parts[i]; try { FS.mkdir(dir); } catch (e) {} }
        FS.writeFile('/persist/' + rel, FS.readFile(p));
        if (Module.__syncTimer) clearTimeout(Module.__syncTimer);
        Module.__syncTimer = setTimeout(function () { FS.syncfs(false, function (e) { if (e) err('save sync failed: ' + e); }); }, 500);
    } catch (e) { err('persist ' + p + ': ' + e); }
});
EM_JS(void, js_restore_persisted, (void), {
    function walk(dir, rel) {
        var names;
        try { names = FS.readdir(dir); } catch (e) { return; }
        names.forEach(function (n) {
            if (n === '.' || n === '..') return;
            var src = dir + '/' + n, r = rel ? rel + '/' + n : n;
            var st = FS.stat(src);
            if (FS.isDir(st.mode)) { try { FS.mkdir('/carma2/' + r); } catch (e) {} walk(src, r); }
            else { try { FS.writeFile('/carma2/' + r, FS.readFile(src)); out('restored ' + r); } catch (e) {} }
        });
    }
    walk('/persist', '');
});
static void persist_file(const char* p) { js_persist_file(p); }

/* ---- CD music from files next to the page: music/Track02.ogg (.mp3/.wav) ... ---- */
extern int (*gEmu_music_tracks)(void);
extern void (*gEmu_music_play)(int track);
extern void (*gEmu_music_stop)(void);
extern void (*gEmu_music_pause)(int paused);
extern int (*gEmu_music_is_playing)(void);
EM_JS(int, js_music_tracks, (void), { return Module.__musicMax || 0; });
EM_JS(void, js_music_play, (int track), {
    var T = Module.__musicTracks || {};
    var url = T[track];
    if (!url) {
        var keys = Object.keys(T).map(Number).sort(function (a, b) { return a - b; });
        if (!keys.length) return;
        url = T[keys[(Math.max(track, 2) - 2) % keys.length]];
    }
    var M = Module.__mus || (Module.__mus = {});
    if (!M.el) {
        M.el = new Audio();
        M.el.preload = 'auto';
        M.el.volume = 1;
        try {
            var ctx = Module.__audio;
            if (ctx && ctx.createMediaElementSource && Module.__master) {
                var src = ctx.createMediaElementSource(M.el), g = ctx.createGain();
                g.gain.value = Module.musicGain || 1.25;
                src.connect(g);
                g.connect(Module.__master);
                M.gain = g;
            }
        } catch (e) {}
        var kick = function () { if (M.want && M.el.paused) M.el.play().catch(function () {}); };
        ['keydown', 'mousedown', 'touchstart', 'pointerdown'].forEach(function (ev) { window.addEventListener(ev, kick, true); });
        Module.__music = M.el;
    }
    M.want = true;
    if (M.stopTimer) { clearTimeout(M.stopTimer); M.stopTimer = 0; }
    var playing = M.el.src && !M.el.paused && !M.el.ended;
    if (playing && M.url === url) return;
    if (M.url !== url) { M.url = url; M.el.src = url; }
    else { try { M.el.currentTime = 0; } catch (e) {} }
    M.el.play().catch(function () {});
});
EM_JS(void, js_music_stop, (void), {
    var M = Module.__mus;
    if (!M || !M.el) return;
    M.want = false;
    M.stopAt = Date.now();
    if (M.stopTimer) clearTimeout(M.stopTimer);
    M.stopTimer = setTimeout(function () { M.stopTimer = 0; if (!M.want) M.el.pause(); }, 400);
});
EM_JS(void, js_music_pause, (int paused), { var M = Module.__mus; if (M && M.el) { M.want = !paused; if (paused) M.el.pause(); else M.el.play().catch(function () {}); } });
EM_JS(int, js_music_is_playing, (void), { var M = Module.__mus; return M && M.el && (M.want || M.stopTimer) && !M.el.ended ? 1 : 0; });

/* The default keymaps steer with the numeric keypad and pan the camera/map with the
 * arrows. For keyboards without a keypad swap them: arrows drive, keypad 8/2/4/6 pans.
 * Only files still holding the default bindings are changed, so user remaps survive. */
/* OPTIONS.TXT ships with the head-up map at 268,109 which is the middle of a 640x480
 * screen; put it in the top-right corner under the HUD instead (only if unchanged). */
static void patch_options(void) {
    char hp[512];
    if (!emu_host_path("DATA\\OPTIONS.TXT", hp, sizeof(hp))) return;
    FILE* f = fopen(hp, "rb");
    if (!f) return;
    static char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    char* x = strstr(buf, "HeadupMapX ");
    char* y = strstr(buf, "HeadupMapY ");
    char* al = strstr(buf, "AutoLoad ");
    int fix_map = x && y && !(atoi(x + 11) >= 500 && atoi(y + 11) >= 380);
    /* load the most recent saved game on start (once: the player may switch it off in the options) */
    int fix_auto = al && atoi(al + 9) == 0 && EM_ASM_INT({ try { return localStorage.getItem('carma2.autoload') ? 0 : 1; } catch (e) { return 1; } });
    if (!fix_map && !fix_auto) return;
    char out[16384];
    size_t o = 0;
    for (char* p = buf; *p && o < sizeof(out) - 64;) {
        char* eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p + 1) : strlen(p);
        if (fix_map && strncmp(p, "HeadupMapX ", 11) == 0) o += snprintf(out + o, sizeof(out) - o, "HeadupMapX 570\r\n");
        else if (fix_map && strncmp(p, "HeadupMapY ", 11) == 0) o += snprintf(out + o, sizeof(out) - o, "HeadupMapY 420\r\n");
        else if (fix_auto && strncmp(p, "AutoLoad ", 9) == 0) o += snprintf(out + o, sizeof(out) - o, "AutoLoad 1\r\n");
        else { memcpy(out + o, p, len); o += len; }
        p += len;
    }
    f = fopen(hp, "wb");
    if (!f) return;
    fwrite(out, 1, o, f);
    fclose(f);
    js_persist_file(hp);
    if (fix_auto) EM_ASM({ try { localStorage.setItem('carma2.autoload', '1'); } catch (e) {} });
    emu_log("options:%s%s", fix_map ? " head-up map in the bottom-right corner" : "", fix_auto ? " auto-load the last saved game" : "");
}

/* The shipped options have the music volume at 0 (there was no CD); when music files are
 * present turn it up so the game starts CD playback. */
static void patch_music_volume(void) {
    if (js_music_tracks() <= 0) return;
    char hp[512];
    if (!emu_host_path("DATA\\OPTIONS.TXT", hp, sizeof(hp))) return;
    FILE* f = fopen(hp, "rb");
    if (!f) return;
    static char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    char* m = strstr(buf, "MVolume 0\r");
    if (!m) m = strstr(buf, "MVolume 0\n");
    if (!m) return;
    static char out[16400];
    size_t pre = (size_t)(m - buf);
    memcpy(out, buf, pre);
    size_t o = pre + (size_t)sprintf(out + pre, "MVolume 200");
    size_t rest = n - pre - 9;
    memcpy(out + o, m + 9, rest);
    o += rest;
    f = fopen(hp, "wb");
    if (!f) return;
    fwrite(out, 1, o, f);
    fclose(f);
    emu_log("options: music volume raised (music files found)");
}

static void patch_keymaps(void) {
    static const char* names[] = { "Keymap_0.txt", "Keymap_1.txt", "Keymap_2.txt", "Keymap_3.txt",
                                   "DKEYMAP0.TXT", "DKEYMAP1.TXT", "DKEYMAP2.TXT", "DKEYMAP3.TXT", NULL };
    for (int n = 0; names[n]; n++) {
        char path[256], hp[512];
        snprintf(path, sizeof(path), "DATA\\%s", names[n]);
        if (!emu_host_path(path, hp, sizeof(hp))) continue;
        FILE* f = fopen(hp, "rb");
        if (!f) continue;
        int v[128], cnt = 0;
        while (cnt < 128 && fscanf(f, "%d", &v[cnt]) == 1) cnt++;
        fclose(f);
        if (cnt < 51) continue;
        /* 31..34 = up/down/left/right (camera), 47..50 = steer left/right, accelerate, brake */
        if (v[31] == 72 && v[32] == 73 && v[33] == 70 && v[34] == 71 && v[47] == 85 && v[48] == 87 && v[49] == 89 && v[50] == 83) {
            v[47] = 70; v[48] = 71; v[49] = 72; v[50] = 73;
            v[31] = 89; v[32] = 83; v[33] = 85; v[34] = 87;
            f = fopen(hp, "wb");
            if (!f) continue;
            for (int i = 0; i < cnt; i++) fprintf(f, "%d\r\n", v[i]);
            fclose(f);
            emu_log("keymap %s: arrows drive, keypad pans the camera", names[n]);
        }
    }
}

int main(int argc, char** argv) {
    js_restore_persisted();
    gEmu_file_written = persist_file;
    gCam_dbg = EM_ASM_INT({ return location.search.indexOf('camdbg') >= 0 ? 1 : 0; });
    { extern int gGl_dbg; gGl_dbg = EM_ASM_INT({ return location.search.indexOf('gldbg') >= 0 ? 1 : 0; }); }
    { extern int gEmu_mcilog; gEmu_mcilog = EM_ASM_INT({ return location.search.indexOf('mcilog') >= 0 ? 1 : 0; }); }
    gEmu_music_tracks = js_music_tracks;
    gEmu_music_play = js_music_play;
    gEmu_music_stop = js_music_stop;
    gEmu_music_pause = js_music_pause;
    gEmu_music_is_playing = js_music_is_playing;
    {
        extern int (*gEmu_net_open)(uint8_t*);
        extern void (*gEmu_net_send)(const uint8_t*, const uint8_t*, int);
        extern int (*gEmu_net_recv)(uint8_t*, uint8_t*, int);
        extern int (*gEmu_net_pending)(void);
        extern int gEmu_netlog;
        extern const char* gEmu_computer_name;
        gEmu_net_open = js_net_open;
        gEmu_net_send = js_net_send;
        gEmu_net_recv = js_net_recv;
        gEmu_net_pending = js_net_pending;
        gEmu_netlog = EM_ASM_INT({ return location.search.indexOf('netlog') >= 0 ? 1 : 0; });
        gEmu_computer_name = js_player_name();
    }
    (void)argc;
    (void)argv;
    gX86_fault = fault;
    if (!EM_ASM_INT({ return (typeof location !== 'undefined' && location.search.indexOf('nojit') >= 0) ? 1 : 0; })) {
        jit_init();
        gJit_on = 1;
        if (!EM_ASM_INT({ return location.search.indexOf('nojitcache') >= 0 ? 1 : 0; })) {
            js_jit_load();
            gJit_left = 1;
        }
        js_jit_hook_unload();
    }
    if (emu_start("/carma2") != 0) {
        fprintf(stderr, "failed to start the game\n");
        return 1;
    }
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, EM_TRUE, on_key);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, EM_TRUE, on_key);
    emscripten_set_mousedown_callback("#canvas", NULL, EM_TRUE, on_mouse);
    emscripten_set_mouseup_callback("#canvas", NULL, EM_TRUE, on_mouse);
    emscripten_set_mousemove_callback("#canvas", NULL, EM_TRUE, on_mouse);
    patch_keymaps();
    patch_options();
    patch_music_volume();
    js_audio_init();
    gMips_t0 = emu_now_ms();
    emscripten_set_main_loop(frame, 0, 0);
    return 0;
}
