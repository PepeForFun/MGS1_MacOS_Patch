/* DirectSound: secondary buffers live in guest memory and are mixed in a CoreAudio callback. */
#include <AudioToolbox/AudioToolbox.h>
#include <stdlib.h>
#include "com.h"

#define DS_OK 0u
#define OUT_RATE 48000.0

typedef struct Buf {
    uint32_t obj;
    uint32_t mem, size;       /* guest buffer */
    int rate, channels, bits;
    bool primary, playing, looping;
    double pos;               /* play position in frames (fractional) */
    int32_t volume, pan;      /* hundredths of a dB */
    uint32_t freq;            /* 0 = original */
    uint32_t flags;
    struct Buf *next;
} Buf;
static Buf *g_bufs;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static AudioUnit g_unit;
static float g_master = 1.0f;
static bool g_mute;

/* MGS_AUDIO_DUMP=file.wav records the mix (before the master volume, so it works muted) */
static FILE *g_dump;
static uint32_t g_dump_frames;
static void dump_header(void) {
    uint32_t data = g_dump_frames * 4;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    uint16_t w = 1; memcpy(h + 20, &w, 2);
    w = 2; memcpy(h + 22, &w, 2);
    v = (uint32_t)OUT_RATE; memcpy(h + 24, &v, 4);
    v = (uint32_t)OUT_RATE * 4; memcpy(h + 28, &v, 4);
    w = 4; memcpy(h + 32, &w, 2);
    w = 16; memcpy(h + 34, &w, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    long pos = ftell(g_dump);
    fseek(g_dump, 0, SEEK_SET);
    fwrite(h, 1, 44, g_dump);
    fseek(g_dump, pos > 44 ? pos : 44, SEEK_SET);
}

/* ------------------------------------------------------------------ mixer */
static float db_gain(int32_t v) { return v <= -10000 ? 0.0f : powf(10.0f, (float)v / 2000.0f); }
static OSStatus render(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n,
                       AudioBufferList *io) {
    (void)ref; (void)fl; (void)ts; (void)bus;
    float *out = io->mBuffers[0].mData;
    memset(out, 0, n * 2 * sizeof(float));
    pthread_mutex_lock(&g_mu);
    for (Buf *b = g_bufs; b; b = b->next) {
        if (!b->playing || b->primary || !b->size) continue;
        int bpf = b->channels * b->bits / 8;
        uint32_t frames = b->size / (uint32_t)bpf;
        if (!frames) continue;
        double step = (double)(b->freq ? b->freq : (uint32_t)b->rate) / OUT_RATE;
        float g = db_gain(b->volume) * g_master;
        float gl = g, gr = g;
        if (b->pan < 0) gr *= db_gain(b->pan);
        else if (b->pan > 0) gl *= db_gain(-b->pan);
        const uint8_t *base = M + b->mem;
        for (UInt32 i = 0; i < n; i++) {
            uint32_t f0 = (uint32_t)b->pos;
            if (f0 >= frames) {
                if (!b->looping) { b->playing = false; b->pos = 0; break; }
                b->pos -= frames;
                f0 = (uint32_t)b->pos;
            }
            const uint8_t *p = base + f0 * (uint32_t)bpf;
            float l, r;
            if (b->bits == 16) {
                l = ((const int16_t *)p)[0] / 32768.0f;
                r = b->channels == 2 ? ((const int16_t *)p)[1] / 32768.0f : l;
            } else {
                l = (p[0] - 128) / 128.0f;
                r = b->channels == 2 ? (p[1] - 128) / 128.0f : l;
            }
            out[2 * i] += l * gl;
            out[2 * i + 1] += r * gr;
            b->pos += step;
        }
    }
    pthread_mutex_unlock(&g_mu);
    if (g_dump) {
        int16_t tmp[4096];
        for (UInt32 i = 0; i < 2 * n && i < 4096; i++) {
            float v = g_master > 0 ? out[i] / g_master : out[i];
            v = v > 1 ? 1 : v < -1 ? -1 : v;
            tmp[i] = (int16_t)(v * 32767);
        }
        fwrite(tmp, 2, 2 * n < 4096 ? 2 * n : 4096, g_dump);
        g_dump_frames += n;
        if (g_dump_frames % 48000 < n) { dump_header(); fflush(g_dump); }
    }
    for (UInt32 i = 0; i < 2 * n; i++) out[i] = g_mute ? 0 : out[i] > 1 ? 1 : out[i] < -1 ? -1 : out[i];
    return noErr;
}
static void audio_start(void) {
    if (g_unit) return;
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp || AudioComponentInstanceNew(comp, &g_unit)) { rt_log("audio: no output unit"); g_unit = NULL; return; }
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = OUT_RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 32;
    f.mBytesPerFrame = 8;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = 8;
    AudioUnitSetProperty(g_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    AURenderCallbackStruct cb = {render, NULL};
    AudioUnitSetProperty(g_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (AudioUnitInitialize(g_unit) || AudioOutputUnitStart(g_unit)) rt_log("audio: start failed");
    else rt_log("audio: CoreAudio output at %.0f Hz", OUT_RATE);
}

/* ------------------------------------------------------------------ IDirectSoundBuffer */
static Buf *self(CPU *c) { return com_host(ARG(0)); }
static void b_QueryInterface(CPU *c) {
    char g[40];
    guid_str(ARG(1), g);
    rt_log("IDirectSoundBuffer::QueryInterface(%s) -> no", g);
    W32(ARG(2), 0);
    RET(3, E_NOINTERFACE);
}
static void b_AddRef(CPU *c) { RET(1, com_addref(ARG(0))); }
static void b_Release(CPU *c) {
    uint32_t n = com_release(ARG(0));
    if (n == 0) {
        Buf *b = self(c);
        pthread_mutex_lock(&g_mu);
        b->playing = false;
        pthread_mutex_unlock(&g_mu);
    }
    RET(1, n);
}
static void b_GetCaps(CPU *c) {
    Buf *b = self(c);
    uint32_t p = ARG(1);
    W32(p + 4, b->flags);
    W32(p + 8, b->size);
    W32(p + 12, 0);
    W32(p + 16, 0);
    RET(2, DS_OK);
}
static uint32_t cursor_bytes(Buf *b) {
    int bpf = b->channels * b->bits / 8;
    return ((uint32_t)b->pos * (uint32_t)bpf) % (b->size ? b->size : 1);
}
static void b_GetCurrentPosition(CPU *c) { /* (this, play*, write*) */
    Buf *b = self(c);
    pthread_mutex_lock(&g_mu);
    uint32_t play = cursor_bytes(b);
    pthread_mutex_unlock(&g_mu);
    /* the write cursor runs ~15 ms ahead of what is being heard */
    uint32_t ahead = (uint32_t)(b->rate * b->channels * b->bits / 8 * 15 / 1000);
    ahead &= ~(uint32_t)(b->channels * b->bits / 8 - 1);
    if (ARG(1)) W32(ARG(1), play);
    if (ARG(2)) W32(ARG(2), b->size ? (play + ahead) % b->size : 0);
    RET(3, DS_OK);
}
static void b_GetFormat(CPU *c) { /* (this, wfx, size, written*) */
    Buf *b = self(c);
    uint32_t p = ARG(1);
    if (p && ARG(2) >= 16) {
        W16(p, 1);
        W16(p + 2, (uint16_t)b->channels);
        W32(p + 4, (uint32_t)b->rate);
        W32(p + 8, (uint32_t)(b->rate * b->channels * b->bits / 8));
        W16(p + 12, (uint16_t)(b->channels * b->bits / 8));
        W16(p + 14, (uint16_t)b->bits);
    }
    if (ARG(3)) W32(ARG(3), 18);
    RET(4, DS_OK);
}
static void b_GetVolume(CPU *c) { W32(ARG(1), (uint32_t)self(c)->volume); RET(2, DS_OK); }
static void b_GetPan(CPU *c) { W32(ARG(1), (uint32_t)self(c)->pan); RET(2, DS_OK); }
static void b_GetFrequency(CPU *c) { Buf *b = self(c); W32(ARG(1), b->freq ? b->freq : (uint32_t)b->rate); RET(2, DS_OK); }
static void b_GetStatus(CPU *c) {
    Buf *b = self(c);
    W32(ARG(1), b->playing ? (1u | (b->looping ? 4u : 0)) : 0);
    RET(2, DS_OK);
}
static void b_Lock(CPU *c) { /* (this, off, bytes, p1*, n1*, p2*, n2*, flags) */
    Buf *b = self(c);
    uint32_t off = ARG(1), n = ARG(2), flags = ARG(7);
    if (flags & 2) { off = 0; n = b->size; }        /* DSBLOCK_ENTIREBUFFER */
    else if (flags & 1) off = (cursor_bytes(b) + 0) % (b->size ? b->size : 1); /* FROMWRITECURSOR */
    if (n > b->size) n = b->size;
    off %= b->size ? b->size : 1;
    uint32_t first = n < b->size - off ? n : b->size - off;
    W32(ARG(3), b->mem + off);
    W32(ARG(4), first);
    if (ARG(5)) W32(ARG(5), first < n ? b->mem : 0);
    if (ARG(6)) W32(ARG(6), n - first);
    RET(8, DS_OK);
}
static void b_Unlock(CPU *c) { RET(5, DS_OK); }
static void b_Play(CPU *c) { /* (this, res, prio, flags) */
    Buf *b = self(c);
    audio_start();
    pthread_mutex_lock(&g_mu);
    b->playing = true;
    b->looping = (ARG(3) & 1) != 0;
    pthread_mutex_unlock(&g_mu);
    RET(4, DS_OK);
}
static void b_Stop(CPU *c) {
    Buf *b = self(c);
    pthread_mutex_lock(&g_mu);
    b->playing = false;
    pthread_mutex_unlock(&g_mu);
    RET(1, DS_OK);
}
static void b_SetCurrentPosition(CPU *c) {
    Buf *b = self(c);
    pthread_mutex_lock(&g_mu);
    int bpf = b->channels * b->bits / 8;
    b->pos = (double)(ARG(1) / (uint32_t)(bpf ? bpf : 1));
    pthread_mutex_unlock(&g_mu);
    RET(2, DS_OK);
}
static void parse_wfx(Buf *b, uint32_t wfx) {
    b->channels = R16(wfx + 2);
    b->rate = (int)R32(wfx + 4);
    b->bits = R16(wfx + 14);
    if (b->channels < 1 || b->channels > 2) b->channels = 2;
    if (b->bits != 8 && b->bits != 16) b->bits = 16;
    if (b->rate <= 0) b->rate = 22050;
}
static void b_SetFormat(CPU *c) { parse_wfx(self(c), ARG(1)); RET(2, DS_OK); }
static void b_SetVolume(CPU *c) { self(c)->volume = (int32_t)ARG(1); RET(2, DS_OK); }
static void b_SetPan(CPU *c) { self(c)->pan = (int32_t)ARG(1); RET(2, DS_OK); }
static void b_SetFrequency(CPU *c) { self(c)->freq = ARG(1); RET(2, DS_OK); }
static void b_Restore(CPU *c) { RET(1, DS_OK); }
static void b_Initialize(CPU *c) { RET(3, DS_OK); }

static const ComMethod g_buf_methods[] = {
    {"QueryInterface", b_QueryInterface}, {"AddRef", b_AddRef}, {"Release", b_Release}, {"GetCaps", b_GetCaps},
    {"GetCurrentPosition", b_GetCurrentPosition}, {"GetFormat", b_GetFormat}, {"GetVolume", b_GetVolume},
    {"GetPan", b_GetPan}, {"GetFrequency", b_GetFrequency}, {"GetStatus", b_GetStatus},
    {"Initialize", b_Initialize}, {"Lock", b_Lock}, {"Play", b_Play}, {"SetCurrentPosition", b_SetCurrentPosition},
    {"SetFormat", b_SetFormat}, {"SetVolume", b_SetVolume}, {"SetPan", b_SetPan}, {"SetFrequency", b_SetFrequency},
    {"Stop", b_Stop}, {"Unlock", b_Unlock}, {"Restore", b_Restore},
};
static ComIface g_buf_iface = {"IDirectSoundBuffer", 21, g_buf_methods, 0};

static Buf *buf_new(void) {
    Buf *b = calloc(1, sizeof *b);
    b->obj = com_new(&g_buf_iface, b, 0);
    pthread_mutex_lock(&g_mu);
    b->next = g_bufs;
    g_bufs = b;
    pthread_mutex_unlock(&g_mu);
    return b;
}

/* ------------------------------------------------------------------ IDirectSound */
static void d_QueryInterface(CPU *c) { W32(ARG(2), ARG(0)); com_addref(ARG(0)); RET(3, DS_OK); }
static void d_AddRef(CPU *c) { RET(1, com_addref(ARG(0))); }
static void d_Release(CPU *c) { RET(1, com_release(ARG(0))); }
static void d_CreateSoundBuffer(CPU *c) { /* (this, desc, out, outer) */
    uint32_t d = ARG(1);
    uint32_t flags = R32(d + 4), bytes = R32(d + 8), wfx = R32(d + 16);
    Buf *b = buf_new();
    b->flags = flags;
    b->primary = (flags & 1) != 0;
    b->rate = 22050;
    b->channels = 2;
    b->bits = 16;
    if (wfx) parse_wfx(b, wfx);
    if (!b->primary) {
        b->size = bytes;
        b->mem = heap_alloc(bytes ? bytes : 4, true);
        if (b->bits == 8) memset(M + b->mem, 0x80, bytes);
    }
    if (g_trace) rt_log("CreateSoundBuffer(flags %x, %u bytes, %d Hz %d ch %d bit)", flags, bytes, b->rate, b->channels, b->bits);
    W32(ARG(2), b->obj);
    RET(4, DS_OK);
}
static void d_GetCaps(CPU *c) {
    uint32_t p = ARG(1);
    uint32_t n = R32(p);
    memset(GPTR(p + 4), 0, n > 4 ? n - 4 : 0);
    W32(p + 4, 0x1 | 0x2 | 0x4 | 0x10 | 0x20 | 0x100 | 0x200 | 0x400); /* continuous rate, mono/stereo, 8/16 bit */
    W32(p + 8, 100);
    W32(p + 12, 100000);
    W32(p + 20, 64);
    RET(2, DS_OK);
}
static void d_DuplicateSoundBuffer(CPU *c) { /* (this, src, out) */
    Buf *s = com_host(ARG(1));
    Buf *b = buf_new();
    if (s) {
        b->mem = s->mem; b->size = s->size; b->rate = s->rate; b->channels = s->channels; b->bits = s->bits;
        b->flags = s->flags; b->volume = s->volume; b->pan = s->pan;
    }
    W32(ARG(2), b->obj);
    RET(3, DS_OK);
}
static void d_SetCooperativeLevel(CPU *c) { RET(3, DS_OK); }
static void d_Compact(CPU *c) { RET(1, DS_OK); }
static void d_GetSpeakerConfig(CPU *c) { W32(ARG(1), 4); RET(2, DS_OK); } /* DSSPEAKER_STEREO */
static void d_SetSpeakerConfig(CPU *c) { RET(2, DS_OK); }
static void d_Initialize(CPU *c) { RET(2, DS_OK); }
static const ComMethod g_ds_methods[] = {
    {"QueryInterface", d_QueryInterface}, {"AddRef", d_AddRef}, {"Release", d_Release},
    {"CreateSoundBuffer", d_CreateSoundBuffer}, {"GetCaps", d_GetCaps}, {"DuplicateSoundBuffer", d_DuplicateSoundBuffer},
    {"SetCooperativeLevel", d_SetCooperativeLevel}, {"Compact", d_Compact}, {"GetSpeakerConfig", d_GetSpeakerConfig},
    {"SetSpeakerConfig", d_SetSpeakerConfig}, {"Initialize", d_Initialize},
};
static ComIface g_ds_iface = {"IDirectSound", 11, g_ds_methods, 0};

void I_dsound_1(CPU *c) { /* DirectSoundCreate(guid, out, outer) */
    rt_log("DirectSoundCreate");
    if (getenv("MGS_MUTE")) g_mute = true;
    if (getenv("MGS_AUDIO_DUMP") && !g_dump) {
        g_dump = fopen(getenv("MGS_AUDIO_DUMP"), "wb");
        if (g_dump) { uint8_t z[44] = {0}; fwrite(z, 1, 44, g_dump); }
    }
    W32(ARG(1), com_new(&g_ds_iface, NULL, 0));
    RET(3, DS_OK);
}
