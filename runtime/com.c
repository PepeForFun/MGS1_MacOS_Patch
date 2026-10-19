/* COM plumbing: vtables in guest memory point at host thunks; unimplemented methods report
 * their interface and name. */
#include <stdlib.h>
#include "com.h"

#define MAX_HOST 65536
#define COM_MAGIC 0x434F4D21u
static void *g_host[MAX_HOST];
static ComIface *g_owner[MAX_HOST];
static int g_nhost;                  /* slots ever handed out */
static int g_free_slot[MAX_HOST], g_nfree;
static uint64_t g_created, g_freed;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
#define MAX_IFACE 64
static ComIface *g_ifaces[MAX_IFACE];
static int g_nifaces;

static void unimpl(CPU *c, int slot) {
    uint32_t vt = R32(ARG(0));
    for (int i = 0; i < g_nifaces; i++)
        if (g_ifaces[i]->vtable == vt) {
            const char *m = slot < g_ifaces[i]->nmethods && g_ifaces[i]->methods[slot].name
                                ? g_ifaces[i]->methods[slot].name : "?";
            rt_fatal("unimplemented COM method %s::%s (slot %d)", g_ifaces[i]->name, m, slot);
        }
    rt_fatal("unimplemented COM method slot %d on unknown vtable %08x", slot, vt);
}
#define STUB(n) static void stub_##n(CPU *c) { unimpl(c, n); }
STUB(0) STUB(1) STUB(2) STUB(3) STUB(4) STUB(5) STUB(6) STUB(7) STUB(8) STUB(9) STUB(10) STUB(11) STUB(12)
STUB(13) STUB(14) STUB(15) STUB(16) STUB(17) STUB(18) STUB(19) STUB(20) STUB(21) STUB(22) STUB(23) STUB(24)
STUB(25) STUB(26) STUB(27) STUB(28) STUB(29) STUB(30) STUB(31) STUB(32) STUB(33) STUB(34) STUB(35) STUB(36)
STUB(37) STUB(38) STUB(39) STUB(40) STUB(41) STUB(42) STUB(43) STUB(44) STUB(45) STUB(46) STUB(47) STUB(48)
STUB(49) STUB(50) STUB(51) STUB(52) STUB(53) STUB(54) STUB(55) STUB(56) STUB(57) STUB(58) STUB(59)
static void (*const g_stubs[60])(CPU *) = {
    stub_0, stub_1, stub_2, stub_3, stub_4, stub_5, stub_6, stub_7, stub_8, stub_9, stub_10, stub_11, stub_12,
    stub_13, stub_14, stub_15, stub_16, stub_17, stub_18, stub_19, stub_20, stub_21, stub_22, stub_23, stub_24,
    stub_25, stub_26, stub_27, stub_28, stub_29, stub_30, stub_31, stub_32, stub_33, stub_34, stub_35, stub_36,
    stub_37, stub_38, stub_39, stub_40, stub_41, stub_42, stub_43, stub_44, stub_45, stub_46, stub_47, stub_48,
    stub_49, stub_50, stub_51, stub_52, stub_53, stub_54, stub_55, stub_56, stub_57, stub_58, stub_59};

static uint32_t vtable_of(ComIface *f) {
    pthread_mutex_lock(&g_mu);
    if (!f->vtable) {
        uint32_t vt = host_alloc(4u * (uint32_t)f->nmethods);
        for (int i = 0; i < f->nmethods; i++) {
            void (*fn)(CPU *) = f->methods[i].fn ? f->methods[i].fn : g_stubs[i];
            W32(vt + 4u * (uint32_t)i, rt_thunk(fn, f->methods[i].name ? f->methods[i].name : f->name));
        }
        f->vtable = vt;
        if (g_nifaces < MAX_IFACE) g_ifaces[g_nifaces++] = f;
    }
    pthread_mutex_unlock(&g_mu);
    return f->vtable;
}

/* The game creates and releases hundreds of texture surfaces per area: a released one gives its
 * slot back (the guest object memory is not reused, so a stale pointer finds no magic) */
uint32_t com_new(ComIface *iface, void *host, uint32_t extra) {
    uint32_t o = host_alloc(16 + extra);
    W32(o, vtable_of(iface));
    W32(o + 4, 1);
    pthread_mutex_lock(&g_mu);
    int i;
    if (g_nfree) i = g_free_slot[--g_nfree];
    else if (g_nhost < MAX_HOST) i = g_nhost++;
    else rt_fatal("too many COM objects (%llu created, %llu freed)", g_created, g_freed);
    g_host[i] = host;
    g_owner[i] = iface;
    if (++g_created % 2048 == 0) rt_log("COM objects: %llu created, %llu freed", g_created, g_freed);
    pthread_mutex_unlock(&g_mu);
    W32(o + 8, (uint32_t)i);
    W32(o + 12, COM_MAGIC);
    return o;
}
void com_each(ComIface *iface, void (*fn)(void *host, void *ctx), void *ctx) {
    pthread_mutex_lock(&g_mu);
    int n = g_nhost;
    pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < n; i++)
        if (g_owner[i] == iface && g_host[i]) fn(g_host[i], ctx);
}
void *com_host(uint32_t o) {
    if (!o || R32(o + 12) != COM_MAGIC) return NULL;
    uint32_t i = R32(o + 8);
    return i < (uint32_t)g_nhost ? g_host[i] : NULL;
}
uint32_t com_addref(uint32_t o) {
    if (!o || R32(o + 12) != COM_MAGIC) return 0;
    uint32_t n = R32(o + 4) + 1;
    W32(o + 4, n);
    return n;
}
uint32_t com_release(uint32_t o) {
    if (!o || R32(o + 12) != COM_MAGIC) return 0;
    uint32_t n = R32(o + 4);
    if (n) n--;
    W32(o + 4, n);
    if (n) return n;
    uint32_t i = R32(o + 8);
    pthread_mutex_lock(&g_mu);
    ComIface *f = i < (uint32_t)g_nhost ? g_owner[i] : NULL;
    void *host = f ? g_host[i] : NULL;
    if (!f || !f->destroy) {
        pthread_mutex_unlock(&g_mu);
        return 0;
    }
    g_host[i] = NULL;
    g_owner[i] = NULL;
    g_free_slot[g_nfree++] = (int)i;
    g_freed++;
    pthread_mutex_unlock(&g_mu);
    W32(o + 12, 0);
    f->destroy(o, host);
    return 0;
}

static int hexv(char ch) { return ch <= '9' ? ch - '0' : (ch | 32) - 'a' + 10; }
static void guid_parse(const char *t, uint8_t out[16]) {
    uint8_t b[16];
    int k = 0;
    for (const char *p = t; *p && k < 16; p++) {
        if (*p == '-') continue;
        b[k++] = (uint8_t)(hexv(p[0]) << 4 | hexv(p[1]));
        p++;
    }
    /* Data1 (4 bytes), Data2, Data3 are little-endian; Data4 is bytes */
    out[0] = b[3]; out[1] = b[2]; out[2] = b[1]; out[3] = b[0];
    out[4] = b[5]; out[5] = b[4]; out[6] = b[7]; out[7] = b[6];
    memcpy(out + 8, b + 8, 8);
}
bool guid_eq(uint32_t g, const char *text) {
    uint8_t want[16];
    guid_parse(text, want);
    return g && memcmp(M + g, want, 16) == 0;
}
void guid_put(uint32_t g, const char *text) {
    uint8_t v[16];
    guid_parse(text, v);
    memcpy(M + g, v, 16);
}
void guid_str(uint32_t g, char *out) {
    const uint8_t *b = M + g;
    sprintf(out, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", b[3], b[2], b[1], b[0], b[5],
            b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}
