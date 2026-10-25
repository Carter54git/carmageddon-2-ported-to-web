/* carma2web: DirectInput (keyboard via host key state) and DirectSound (absent for now) */
#include "emu.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#define API(n) static uint32_t n##_(X86Cpu* c, uint32_t* a)
#define UNUSED (void)c; (void)a
#define P32(i) ((uint32_t*)G2H(a[i]))

typedef struct {
    const char* name;
    EmuApiFn fn;
    int nargs; /* including this */
} Method;

static uint32_t make_vtbl(const char* iface, const Method* m, int n) {
    uint32_t* v = gcalloc(n * 4);
    for (int i = 0; i < n; i++) {
        v[i] = emu_trap(iface, m[i].name, m[i].fn, m[i].nargs);
    }
    return H2G(v);
}
static uint32_t make_obj(uint32_t vtbl, size_t extra) {
    uint32_t* o = gcalloc(16 + extra);
    o[0] = vtbl;
    o[1] = 1;
    return H2G(o);
}

API(QueryInterface) { UNUSED; *P32(2) = a[0]; return 0; }
API(AddRef) { UNUSED; return ++((uint32_t*)G2H(a[0]))[1]; }
API(Release) { UNUSED; return 0; }
API(Ok) { UNUSED; return 0; }
API(Fail) { UNUSED; return 0x80004005u; }

/* ---- IDirectInputDevice ---- */
static int gDev_is_keyboard(uint32_t dev) { return ((uint32_t*)G2H(dev))[2] == 1; }

API(DevGetDeviceState) {
    UNUSED;
    static unsigned polls;
    if ((++polls & 15) == 0) emu_yield(); /* busy-wait loops on the keyboard must let the host run */
    if (gDev_is_keyboard(a[0])) {
        uint32_t n = a[1] < 256 ? a[1] : 256;
        memcpy(G2H(a[2]), gEmu_keys, n);
    } else {
        memset(G2H(a[2]), 0, a[1]);
    }
    return 0;
}
API(DevGetDeviceData) {
    UNUSED;
    if (a[3]) *P32(3) = 0;
    return 0;
}
API(DevGetCapabilities) {
    UNUSED;
    uint32_t* caps = P32(1);
    uint32_t sz = caps[0];
    memset(caps, 0, sz);
    caps[0] = sz;
    caps[1] = 5; /* attached | emulated */
    caps[2] = 0x13; /* keyboard */
    return 0;
}

static uint32_t gDevice_vtbl;
static const Method kDevice[] = {
    { "QueryInterface", QueryInterface_, 3 },
    { "AddRef", AddRef_, 1 },
    { "Release", Release_, 1 },
    { "GetCapabilities", DevGetCapabilities_, 2 },
    { "EnumObjects", Ok_, 4 },
    { "GetProperty", Fail_, 3 },
    { "SetProperty", Ok_, 3 },
    { "Acquire", Ok_, 1 },
    { "Unacquire", Ok_, 1 },
    { "GetDeviceState", DevGetDeviceState_, 3 },
    { "GetDeviceData", DevGetDeviceData_, 5 },
    { "SetDataFormat", Ok_, 2 },
    { "SetEventNotification", Ok_, 2 },
    { "SetCooperativeLevel", Ok_, 3 },
    { "GetObjectInfo", Fail_, 4 },
    { "GetDeviceInfo", Fail_, 2 },
    { "RunControlPanel", Ok_, 3 },
    { "Initialize", Ok_, 4 },
    { "CreateEffect", Fail_, 5 },
    { "EnumEffects", Ok_, 4 },
    { "GetEffectInfo", Fail_, 3 },
    { "GetForceFeedbackState", Fail_, 2 },
    { "SendForceFeedbackCommand", Ok_, 2 },
    { "EnumCreatedEffectObjects", Ok_, 4 },
    { "Escape", Fail_, 2 },
    { "Poll", Ok_, 1 },
    { "SendDeviceData", Ok_, 5 },
};

/* ---- IDirectInput ---- */
API(DICreateDevice) {
    UNUSED;
    const uint8_t* guid = G2H(a[1]);
    /* GUID_SysKeyboard = 6F1D2B61-D5A0-11CF-BFC7-444553540000 */
    int kb = *(const uint32_t*)guid == 0x6F1D2B61u;
    int mouse = *(const uint32_t*)guid == 0x6F1D2B60u;
    emu_log("DirectInput CreateDevice(%08x) %s", *(const uint32_t*)guid, kb ? "keyboard" : mouse ? "mouse" : "other");
    if (!kb && !mouse) return 0x80040154u;
    uint32_t o = make_obj(gDevice_vtbl, 16);
    ((uint32_t*)G2H(o))[2] = kb ? 1 : 2;
    *P32(2) = o;
    return 0;
}
API(DIEnumDevices) { UNUSED; return 0; }
static const Method kDInput[] = {
    { "QueryInterface", QueryInterface_, 3 },
    { "AddRef", AddRef_, 1 },
    { "Release", Release_, 1 },
    { "CreateDevice", DICreateDevice_, 4 },
    { "EnumDevices", DIEnumDevices_, 5 },
    { "GetDeviceStatus", Ok_, 2 },
    { "RunControlPanel", Ok_, 3 },
    { "Initialize", Ok_, 3 },
};
static uint32_t gDInput_vtbl;
API(DirectInputCreateA) {
    UNUSED;
    *P32(2) = make_obj(gDInput_vtbl, 0);
    return 0;
}
/* ---- DirectSound: buffers are mixed in C, pulled by the host audio callback ---- */
typedef struct {
    int used;
    uint8_t* data;
    uint32_t size;
    int channels, bits, rate;
    int primary;
    int playing, looping;
    double pos; /* in frames */
    uint32_t freq;
    float gain, lgain, rgain;
    int volume, pan;
} DsBuf;
#define MAX_DSBUF 512
static DsBuf gDsb[MAX_DSBUF];
static uint32_t gDs_vtbl, gDsb_vtbl;

static DsBuf* dsb_of(uint32_t obj) {
    uint32_t i = ((uint32_t*)G2H(obj))[2];
    return i < MAX_DSBUF ? &gDsb[i] : NULL;
}
static void dsb_update_gain(DsBuf* b) {
    b->gain = b->volume <= -10000 ? 0.0f : powf(10.0f, b->volume / 2000.0f);
    b->lgain = b->pan > 0 ? powf(10.0f, -b->pan / 2000.0f) : 1.0f;
    b->rgain = b->pan < 0 ? powf(10.0f, b->pan / 2000.0f) : 1.0f;
}
static uint32_t dsb_new(void) {
    for (int i = 0; i < MAX_DSBUF; i++) {
        if (!gDsb[i].used) {
            memset(&gDsb[i], 0, sizeof(DsBuf));
            gDsb[i].used = 1;
            gDsb[i].channels = 1; gDsb[i].bits = 16; gDsb[i].rate = 22050;
            dsb_update_gain(&gDsb[i]);
            uint32_t o = make_obj(gDsb_vtbl, 16);
            ((uint32_t*)G2H(o))[2] = i;
            return o;
        }
    }
    return 0;
}

API(DSCreateSoundBuffer) {
    UNUSED;
    const uint32_t* desc = G2H(a[1]);
    uint32_t flags = desc[1], bytes = desc[2], wfx = desc[4];
    uint32_t o = dsb_new();
    if (!o) return 0x8007000eu;
    DsBuf* b = dsb_of(o);
    if (flags & 1) {
        b->primary = 1;
    } else {
        if (wfx) {
            const uint8_t* w = G2H(wfx);
            b->channels = *(const uint16_t*)(w + 2);
            b->rate = *(const uint32_t*)(w + 4);
            b->bits = *(const uint16_t*)(w + 14);
        }
        b->size = bytes;
        b->data = gcalloc(bytes + 4);
    }
    *P32(2) = o;
    return 0;
}
API(DSDuplicateSoundBuffer) {
    UNUSED;
    DsBuf* src = dsb_of(a[1]);
    uint32_t o = dsb_new();
    if (!o || !src) return 0x8007000eu;
    DsBuf* b = dsb_of(o);
    int idx = (int)(b - gDsb);
    *b = *src;
    b->playing = 0;
    b->pos = 0;
    (void)idx;
    *P32(2) = o;
    return 0;
}
API(DSGetCaps) { UNUSED; uint32_t* caps = P32(1); uint32_t sz = caps[0]; memset(caps, 0, sz); caps[0] = sz; caps[1] = 0xf5f; return 0; }

API(DsbRelease) {
    UNUSED;
    DsBuf* b = dsb_of(a[0]);
    if (b) b->playing = 0, b->used = 0; /* data stays allocated: duplicates may share it */
    return 0;
}
API(DsbGetCaps) { UNUSED; uint32_t* caps = P32(1); DsBuf* b = dsb_of(a[0]); caps[1] = 0xe0; caps[2] = b ? b->size : 0; return 0; }
API(DsbGetCurrentPosition) {
    UNUSED;
    DsBuf* b = dsb_of(a[0]);
    uint32_t bpf = b ? (uint32_t)(b->channels * b->bits / 8) : 1;
    uint32_t p = b ? (uint32_t)b->pos * bpf : 0;
    if (a[1]) *P32(1) = p;
    if (a[2]) *P32(2) = p;
    return 0;
}
API(DsbGetVolume) { UNUSED; DsBuf* b = dsb_of(a[0]); *P32(1) = b ? (uint32_t)b->volume : 0; return 0; }
API(DsbGetPan) { UNUSED; DsBuf* b = dsb_of(a[0]); *P32(1) = b ? (uint32_t)b->pan : 0; return 0; }
API(DsbGetFrequency) { UNUSED; DsBuf* b = dsb_of(a[0]); *P32(1) = b ? (b->freq ? b->freq : (uint32_t)b->rate) : 22050; return 0; }
API(DsbGetStatus) {
    UNUSED;
    DsBuf* b = dsb_of(a[0]);
    *P32(1) = b && b->playing ? (1u | (b->looping ? 4u : 0u)) : 0u;
    return 0;
}
API(DsbLock) {
    UNUSED;
    DsBuf* b = dsb_of(a[0]);
    if (!b || b->primary || !b->data) return 0x88780096u;
    uint32_t off = a[1], n = a[2];
    if (a[7] & 2) { off = 0; n = b->size; } /* DSBLOCK_ENTIREBUFFER */
    if (off > b->size) off = 0;
    uint32_t n1 = n, n2 = 0;
    if (off + n > b->size) { n1 = b->size - off; n2 = n - n1; if (n2 > off) n2 = off; }
    *P32(3) = H2G(b->data + off);
    *P32(4) = n1;
    if (a[5]) *P32(5) = n2 ? H2G(b->data) : 0;
    if (a[6]) *P32(6) = n2;
    return 0;
}
API(DsbPlay) {
    UNUSED;
    DsBuf* b = dsb_of(a[0]);
    if (b) { b->playing = 1; b->looping = (a[3] & 1) != 0; }
    return 0;
}
API(DsbSetCurrentPosition) {
    UNUSED;
    DsBuf* b = dsb_of(a[0]);
    if (b) { uint32_t bpf = b->channels * b->bits / 8; b->pos = bpf ? a[1] / bpf : 0; }
    return 0;
}
API(DsbSetVolume) { UNUSED; DsBuf* b = dsb_of(a[0]); if (b) { b->volume = (int32_t)a[1]; dsb_update_gain(b); } return 0; }
API(DsbSetPan) { UNUSED; DsBuf* b = dsb_of(a[0]); if (b) { b->pan = (int32_t)a[1]; dsb_update_gain(b); } return 0; }
API(DsbSetFrequency) { UNUSED; DsBuf* b = dsb_of(a[0]); if (b) b->freq = a[1]; return 0; }
API(DsbStop) { UNUSED; DsBuf* b = dsb_of(a[0]); if (b) b->playing = 0; return 0; }

static const Method kDs[] = {
    { "QueryInterface", QueryInterface_, 3 },
    { "AddRef", AddRef_, 1 },
    { "Release", Release_, 1 },
    { "CreateSoundBuffer", DSCreateSoundBuffer_, 4 },
    { "GetCaps", DSGetCaps_, 2 },
    { "DuplicateSoundBuffer", DSDuplicateSoundBuffer_, 3 },
    { "SetCooperativeLevel", Ok_, 3 },
    { "Compact", Ok_, 1 },
    { "GetSpeakerConfig", Ok_, 2 },
    { "SetSpeakerConfig", Ok_, 2 },
    { "Initialize", Ok_, 2 },
};
static const Method kDsb[] = {
    { "QueryInterface", QueryInterface_, 3 },
    { "AddRef", AddRef_, 1 },
    { "Release", DsbRelease_, 1 },
    { "GetCaps", DsbGetCaps_, 2 },
    { "GetCurrentPosition", DsbGetCurrentPosition_, 3 },
    { "GetFormat", Fail_, 4 },
    { "GetVolume", DsbGetVolume_, 2 },
    { "GetPan", DsbGetPan_, 2 },
    { "GetFrequency", DsbGetFrequency_, 2 },
    { "GetStatus", DsbGetStatus_, 2 },
    { "Initialize", Ok_, 3 },
    { "Lock", DsbLock_, 8 },
    { "Play", DsbPlay_, 4 },
    { "SetCurrentPosition", DsbSetCurrentPosition_, 2 },
    { "SetFormat", Ok_, 2 },
    { "SetVolume", DsbSetVolume_, 2 },
    { "SetPan", DsbSetPan_, 2 },
    { "SetFrequency", DsbSetFrequency_, 2 },
    { "Stop", DsbStop_, 1 },
    { "Unlock", Ok_, 5 },
    { "Restore", Ok_, 1 },
};

int gEmu_sound_enabled = 1;
API(DirectSoundCreate) {
    UNUSED;
    if (!gEmu_sound_enabled) return 0x88780078u; /* DSERR_NODRIVER */
    *P32(1) = make_obj(gDs_vtbl, 0);
    emu_log("DirectSoundCreate -> emulated device");
    return 0;
}

/* Mix all playing buffers into float stereo output (called by the host audio callback). */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EMSCRIPTEN_KEEPALIVE
#endif
void emu_audio_mix(float* left, float* right, int frames, int out_rate) {
    for (int i = 0; i < frames; i++) left[i] = right[i] = 0.0f;
    for (int bi = 0; bi < MAX_DSBUF; bi++) {
        DsBuf* b = &gDsb[bi];
        if (!b->used || !b->playing || b->primary || !b->data || b->size == 0) continue;
        int bpf = b->channels * b->bits / 8;
        if (bpf <= 0) continue;
        uint32_t nframes = b->size / bpf;
        if (nframes == 0) continue;
        double rate = b->freq ? (double)b->freq : (double)b->rate;
        double step = rate / out_rate;
        double pos = b->pos;
        float lg = b->gain * b->lgain, rg = b->gain * b->rgain;
        for (int i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)pos;
            if (f >= nframes) {
                if (!b->looping) { b->playing = 0; break; }
                pos -= nframes * (double)(uint32_t)(pos / nframes);
                f = (uint32_t)pos;
                if (f >= nframes) f = 0;
            }
            float l, r;
            const uint8_t* p = b->data + f * bpf;
            if (b->bits == 16) {
                l = ((const int16_t*)p)[0] / 32768.0f;
                r = b->channels == 2 ? ((const int16_t*)p)[1] / 32768.0f : l;
            } else {
                l = (p[0] - 128) / 128.0f;
                r = b->channels == 2 ? (p[1] - 128) / 128.0f : l;
            }
            left[i] += l * lg;
            right[i] += r * rg;
            pos += step;
        }
        b->pos = pos;
    }
    for (int i = 0; i < frames; i++) {
        if (left[i] > 1) left[i] = 1; else if (left[i] < -1) left[i] = -1;
        if (right[i] > 1) right[i] = 1; else if (right[i] < -1) right[i] = -1;
    }
}

static const EmuApiDef kDx[] = {
    { "DINPUT", "DirectInputCreateA", DirectInputCreateA_, 4 },
    { "DSOUND", "DirectSoundCreate", DirectSoundCreate_, 3 },
    { NULL, NULL, NULL, 0 },
};

void emu_dx_init(void) {
    gDevice_vtbl = make_vtbl("IDirectInputDevice2", kDevice, sizeof(kDevice) / sizeof(kDevice[0]));
    gDInput_vtbl = make_vtbl("IDirectInput", kDInput, sizeof(kDInput) / sizeof(kDInput[0]));
    gDs_vtbl = make_vtbl("IDirectSound", kDs, sizeof(kDs) / sizeof(kDs[0]));
    gDsb_vtbl = make_vtbl("IDirectSoundBuffer", kDsb, sizeof(kDsb) / sizeof(kDsb[0]));
    emu_register_apis(kDx);
}
