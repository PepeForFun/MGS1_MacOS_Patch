/* DirectInput 7: system keyboard plus one game controller exposed as a DIJOYSTATE joystick. */
#include <stdlib.h>
#include "com.h"

#define GUID_SysKeyboard "6F1D2B61-D5A0-11CF-BFC7-444553540000"
#define GUID_SysMouse "6F1D2B60-D5A0-11CF-BFC7-444553540000"
#define GUID_Joystick "A1B2C3D4-0001-11D1-8000-4D4753524543"  /* our controller's instance guid */
#define GUID_XAxis "A36D02E0-C9F3-11CF-BFC7-444553540000"
#define GUID_YAxis "A36D02E1-C9F3-11CF-BFC7-444553540000"
#define GUID_ZAxis "A36D02E2-C9F3-11CF-BFC7-444553540000"
#define GUID_RzAxis "A36D02E3-C9F3-11CF-BFC7-444553540000"
#define GUID_Button "A36D02F0-C9F3-11CF-BFC7-444553540000"
#define GUID_POV "A36D02F2-C9F3-11CF-BFC7-444553540000"
#define GUID_Key "55728220-D33C-11CF-BFC7-444553540000"

enum { DEV_KEYBOARD, DEV_MOUSE, DEV_JOYSTICK };
typedef struct {
    int kind;
    uint32_t data_size;
    int32_t amin[8], amax[8];
    uint32_t deadzone;
    /* buffered keyboard events */
    struct { uint32_t ofs, data, time, seq; } buf[256];
    unsigned head, tail, seq;
    uint32_t bufsize;
    bool acquired;
} Dev;
static Dev *g_keyboard;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

void dinput_key_event(int scan, bool down) {
    pthread_mutex_lock(&g_mu);
    Dev *d = g_keyboard;
    if (d && d->bufsize && d->tail - d->head < 256) {
        unsigned i = d->tail++ % 256;
        d->buf[i].ofs = (uint32_t)scan;
        d->buf[i].data = down ? 0x80 : 0;
        d->buf[i].time = (uint32_t)(plat_ticks_us() / 1000);
        d->buf[i].seq = d->seq++;
    }
    pthread_mutex_unlock(&g_mu);
}

/* ------------------------------------------------------------------ IDirectInputDevice7A */
static Dev *dev(CPU *c) { return com_host(ARG(0)); }
static void d_QueryInterface(CPU *c) { W32(ARG(2), ARG(0)); com_addref(ARG(0)); RET(3, S_OK); }
static void d_AddRef(CPU *c) { RET(1, com_addref(ARG(0))); }
static void d_Release(CPU *c) { RET(1, com_release(ARG(0))); }
static void d_GetCapabilities(CPU *c) {
    Dev *d = dev(c);
    uint32_t p = ARG(1);
    W32(p + 4, 1); /* DIDC_ATTACHED */
    if (d->kind == DEV_KEYBOARD) { W32(p + 8, 0x0103); W32(p + 12, 0); W32(p + 16, 128); W32(p + 20, 0); }
    else { W32(p + 8, 0x10104); W32(p + 12, 4); W32(p + 16, 12); W32(p + 20, 1); }
    RET(2, S_OK);
}
static void enum_obj(CPU *c, uint32_t cb, uint32_t ref, uint32_t inst, const char *guid, uint32_t ofs,
                     uint32_t type, const char *name) {
    memset(GPTR(inst), 0, 316);
    W32(inst, 316);
    guid_put(inst + 4, guid);
    W32(inst + 20, ofs);
    W32(inst + 24, type);
    snprintf((char *)GPTR(inst + 32), 260, "%s", name);
    call_guest(c, cb, 2, inst, ref);
}
static void d_EnumObjects(CPU *c) { /* (this, cb, ref, flags) */
    Dev *d = dev(c);
    uint32_t cb = ARG(1), ref = ARG(2), inst = heap_alloc(316, true);
    if (d->kind == DEV_JOYSTICK) {
        static const char *axes[4] = {GUID_XAxis, GUID_YAxis, GUID_ZAxis, GUID_RzAxis};
        static const char *an[4] = {"X Axis", "Y Axis", "Z Axis", "Z Rotation"};
        static const uint32_t ofs[4] = {0, 4, 8, 20};
        for (uint32_t i = 0; i < 4; i++) enum_obj(c, cb, ref, inst, axes[i], ofs[i], 0x2 | (i << 8), an[i]);
        for (uint32_t i = 0; i < 12; i++) {
            char n[32];
            snprintf(n, sizeof n, "Button %u", i + 1);
            enum_obj(c, cb, ref, inst, GUID_Button, 48 + i, 0x4 | (i << 8), n);
        }
        enum_obj(c, cb, ref, inst, GUID_POV, 32, 0x10, "Hat Switch");
    }
    heap_free(inst);
    RET(4, S_OK);
}
static void d_GetProperty(CPU *c) {
    Dev *d = dev(c);
    uint32_t prop = ARG(1), p = ARG(2);
    if (prop == 4) { W32(p + 16, (uint32_t)d->amin[0]); W32(p + 20, (uint32_t)d->amax[0]); }
    else if (prop == 1) W32(p + 16, d->bufsize);
    else if (prop == 5) W32(p + 16, d->deadzone);
    RET(3, S_OK);
}
static int axis_index(uint32_t obj, uint32_t how) {
    if (how == 0) return -1; /* DIPH_DEVICE: all axes */
    if (how == 1) {          /* DIPH_BYOFFSET */
        switch (obj) { case 0: return 0; case 4: return 1; case 8: return 2; case 12: return 3; case 16: return 4;
                       case 20: return 5; case 24: return 6; case 28: return 7; }
    }
    return (int)((obj >> 8) & 7);
}
static void d_SetProperty(CPU *c) {
    Dev *d = dev(c);
    uint32_t prop = ARG(1), p = ARG(2);
    uint32_t obj = R32(p + 8), how = R32(p + 12);
    if (prop == 4) { /* DIPROP_RANGE */
        int a = axis_index(obj, how);
        for (int i = 0; i < 8; i++)
            if (a < 0 || a == i) { d->amin[i] = (int32_t)R32(p + 16); d->amax[i] = (int32_t)R32(p + 20); }
    } else if (prop == 1) {
        d->bufsize = R32(p + 16);
    } else if (prop == 5) {
        d->deadzone = R32(p + 16);
    }
    RET(3, S_OK);
}
static void d_Acquire(CPU *c) { dev(c)->acquired = true; RET(1, S_OK); }
static void d_Unacquire(CPU *c) { dev(c)->acquired = false; RET(1, S_OK); }

static int32_t scale_axis(Dev *d, int i, float v) {
    if (fabsf(v) * 10000 < (float)d->deadzone) v = 0;
    float t = (v + 1) * 0.5f;
    return d->amin[i] + (int32_t)lroundf(t * (float)(d->amax[i] - d->amin[i]));
}
static void d_GetDeviceState(CPU *c) { /* (this, size, data) */
    ddraw_idle();
    Dev *d = dev(c);
    uint32_t n = ARG(1), p = ARG(2);
    memset(GPTR(p), 0, n);
    if (d->kind == DEV_KEYBOARD) {
        for (uint32_t i = 0; i < n && i < 256; i++)
            if (plat_scan_down((int)i)) W8(p + i, 0x80);
    } else if (d->kind == DEV_JOYSTICK) {
        PadState s;
        plat_pad(&s);
        W32(p + 0, (uint32_t)scale_axis(d, 0, s.lx));
        W32(p + 4, (uint32_t)scale_axis(d, 1, s.ly));
        W32(p + 8, (uint32_t)scale_axis(d, 2, s.rx));
        W32(p + 20, (uint32_t)scale_axis(d, 5, s.ry));
        uint32_t pov = 0xFFFFFFFFu;
        int up = s.buttons & PAD_UP ? 1 : 0, dn = s.buttons & PAD_DOWN ? 1 : 0;
        int lf = s.buttons & PAD_LEFT ? 1 : 0, rt = s.buttons & PAD_RIGHT ? 1 : 0;
        if (up && rt) pov = 4500; else if (rt && dn) pov = 13500; else if (dn && lf) pov = 22500;
        else if (lf && up) pov = 31500; else if (up) pov = 0; else if (rt) pov = 9000; else if (dn) pov = 18000;
        else if (lf) pov = 27000;
        W32(p + 32, pov);
        W32(p + 36, 0xFFFFFFFFu);
        W32(p + 40, 0xFFFFFFFFu);
        W32(p + 44, 0xFFFFFFFFu);
        for (int i = 0; i < 12; i++)
            if (s.buttons & (1u << i)) W8(p + 48 + (uint32_t)i, 0x80);
    }
    RET(3, S_OK);
}
static void d_GetDeviceData(CPU *c) { /* (this, objsize, rgdod, inout, flags) */
    Dev *d = dev(c);
    uint32_t objsize = ARG(1), out = ARG(2), inout = ARG(3), flags = ARG(4);
    uint32_t want = R32(inout), got = 0;
    pthread_mutex_lock(&g_mu);
    while (d->head != d->tail && got < want) {
        unsigned i = d->head % 256;
        if (out) {
            uint32_t o = out + got * objsize;
            W32(o, d->buf[i].ofs);
            W32(o + 4, d->buf[i].data);
            W32(o + 8, d->buf[i].time);
            W32(o + 12, d->buf[i].seq);
        }
        if (!(flags & 1)) d->head++; /* DIGDD_PEEK */
        got++;
        if (flags & 1) break;
    }
    if (!out && want == 0xFFFFFFFFu) { got = d->tail - d->head; d->head = d->tail; }
    pthread_mutex_unlock(&g_mu);
    W32(inout, got);
    RET(5, S_OK);
}
static void d_SetDataFormat(CPU *c) {
    Dev *d = dev(c);
    d->data_size = R32(ARG(1) + 12);
    rt_log("dinput SetDataFormat(kind %d, data size %u)", d->kind, d->data_size);
    RET(2, S_OK);
}
static void d_SetEventNotification(CPU *c) { RET(2, S_OK); }
static void d_SetCooperativeLevel(CPU *c) { RET(3, S_OK); }
static void d_GetDeviceInfo(CPU *c);
static void d_Poll(CPU *c) { RET(1, S_OK); }
static void d_RunControlPanel(CPU *c) { RET(3, S_OK); }
static void d_EnumEffects(CPU *c) { RET(4, S_OK); }
static void d_CreateEffect(CPU *c) { RET(5, E_NOTIMPL); }
static void d_SendForceFeedbackCommand(CPU *c) { RET(2, S_OK); }

static const ComMethod g_dev_methods[] = {
    {"QueryInterface", d_QueryInterface}, {"AddRef", d_AddRef}, {"Release", d_Release},
    {"GetCapabilities", d_GetCapabilities}, {"EnumObjects", d_EnumObjects}, {"GetProperty", d_GetProperty},
    {"SetProperty", d_SetProperty}, {"Acquire", d_Acquire}, {"Unacquire", d_Unacquire},
    {"GetDeviceState", d_GetDeviceState}, {"GetDeviceData", d_GetDeviceData}, {"SetDataFormat", d_SetDataFormat},
    {"SetEventNotification", d_SetEventNotification}, {"SetCooperativeLevel", d_SetCooperativeLevel},
    {"GetObjectInfo", NULL}, {"GetDeviceInfo", d_GetDeviceInfo}, {"RunControlPanel", d_RunControlPanel},
    {"Initialize", NULL}, {"CreateEffect", d_CreateEffect}, {"EnumEffects", d_EnumEffects},
    {"GetEffectInfo", NULL}, {"GetForceFeedbackState", NULL}, {"SendForceFeedbackCommand", d_SendForceFeedbackCommand},
    {"EnumCreatedEffectObjects", NULL}, {"Escape", NULL}, {"Poll", d_Poll}, {"SendDeviceData", NULL},
    {"EnumEffectsInFile", NULL}, {"WriteEffectToFile", NULL},
};
static ComIface g_dev_iface = {"IDirectInputDevice7A", 29, g_dev_methods, 0};

static void fill_instance(uint32_t p, int kind) {
    memset(GPTR(p), 0, 580);
    W32(p, 580);
    const char *g = kind == DEV_KEYBOARD ? GUID_SysKeyboard : kind == DEV_MOUSE ? GUID_SysMouse : GUID_Joystick;
    guid_put(p + 4, g);
    guid_put(p + 20, g);
    W32(p + 36, kind == DEV_KEYBOARD ? 0x0103 : kind == DEV_MOUSE ? 0x0102 : 0x10104);
    const char *name = kind == DEV_KEYBOARD ? "Keyboard" : kind == DEV_MOUSE ? "Mouse" : "Game Controller";
    snprintf((char *)GPTR(p + 40), 260, "%s", name);
    snprintf((char *)GPTR(p + 300), 260, "%s", name);
}
static void d_GetDeviceInfo(CPU *c) { fill_instance(ARG(1), dev(c)->kind); RET(2, S_OK); }

/* ------------------------------------------------------------------ IDirectInput7A */
static void i_QueryInterface(CPU *c) { W32(ARG(2), ARG(0)); com_addref(ARG(0)); RET(3, S_OK); }
static void i_AddRef(CPU *c) { RET(1, com_addref(ARG(0))); }
static void i_Release(CPU *c) { RET(1, com_release(ARG(0))); }
static uint32_t make_device(uint32_t guid) {
    Dev *d = calloc(1, sizeof *d);
    if (guid_eq(guid, GUID_SysKeyboard)) {
        d->kind = DEV_KEYBOARD;
        g_keyboard = d;
    } else if (guid_eq(guid, GUID_SysMouse)) {
        d->kind = DEV_MOUSE;
    } else if (guid_eq(guid, GUID_Joystick)) {
        d->kind = DEV_JOYSTICK;
    } else {
        char s[40];
        guid_str(guid, s);
        rt_log("dinput: unknown device guid %s", s);
        free(d);
        return 0;
    }
    for (int i = 0; i < 8; i++) { d->amin[i] = 0; d->amax[i] = 65535; }
    rt_log("dinput: created device kind %d", d->kind);
    return com_new(&g_dev_iface, d, 0);
}
static void i_CreateDevice(CPU *c) { /* (this, guid, out, outer) */
    uint32_t o = make_device(ARG(1));
    W32(ARG(2), o);
    RET(4, o ? S_OK : 0x80040154u);
}
static void i_CreateDeviceEx(CPU *c) { /* (this, guid, riid, out, outer) */
    uint32_t o = make_device(ARG(1));
    W32(ARG(3), o);
    RET(5, o ? S_OK : 0x80040154u);
}
static void i_EnumDevices(CPU *c) { /* (this, type, cb, ref, flags) */
    uint32_t type = ARG(1) & 0xFF, cb = ARG(2), ref = ARG(3);
    uint32_t inst = heap_alloc(580, true);
    PadState s;
    plat_pad(&s);
    rt_log("dinput EnumDevices(type %u) controller %s", type, s.connected ? "present" : "absent");
    bool go = true;
    if (go && (type == 0 || type == 3)) { fill_instance(inst, DEV_KEYBOARD); go = call_guest_ret(c, cb, 2, inst, ref) != 0; }
    if (go && (type == 0 || type == 2)) { fill_instance(inst, DEV_MOUSE); go = call_guest_ret(c, cb, 2, inst, ref) != 0; }
    if (go && (type == 0 || type == 4) && s.connected) { fill_instance(inst, DEV_JOYSTICK); call_guest(c, cb, 2, inst, ref); }
    heap_free(inst);
    RET(5, S_OK);
}
static void i_GetDeviceStatus(CPU *c) {
    PadState s;
    plat_pad(&s);
    RET(2, guid_eq(ARG(1), GUID_Joystick) && !s.connected ? 1 : S_OK);
}
static const ComMethod g_di_methods[] = {
    {"QueryInterface", i_QueryInterface}, {"AddRef", i_AddRef}, {"Release", i_Release},
    {"CreateDevice", i_CreateDevice}, {"EnumDevices", i_EnumDevices}, {"GetDeviceStatus", i_GetDeviceStatus},
    {"RunControlPanel", NULL}, {"Initialize", NULL}, {"FindDevice", NULL}, {"CreateDeviceEx", i_CreateDeviceEx},
};
static ComIface g_di_iface = {"IDirectInput7A", 10, g_di_methods, 0};

void I_dinput_DirectInputCreateEx(CPU *c) { /* (hinst, version, riid, out, outer) */
    rt_log("DirectInputCreateEx(version %x)", ARG(1));
    W32(ARG(3), com_new(&g_di_iface, NULL, 0));
    RET(5, S_OK);
}
