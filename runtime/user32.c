/* USER32: one top-level window, a message queue fed by the platform layer, string resources. */
#include <strings.h>
#include "rt.h"

#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_NCCREATE 0x0081
#define HWND_MAIN 0x00010010u

/* ------------------------------------------------------------------ resources */
static uint32_t rsrc_base(void) {
    uint32_t pe = R32(g_image_base + 0x3C);
    return g_image_base + R32(g_image_base + pe + 24 + 96 + 2 * 8);
}
/* Find entry `id` in a resource directory; returns the entry's OffsetToData or 0. */
static uint32_t rsrc_dir_find(uint32_t dir, uint32_t id, bool any) {
    uint32_t root = rsrc_base();
    uint16_t named = R16(dir + 12), ids = R16(dir + 14);
    for (uint32_t i = 0; i < (uint32_t)named + ids; i++) {
        uint32_t e = dir + 16 + 8 * i;
        uint32_t name = R32(e), off = R32(e + 4);
        if (any || (!(name & 0x80000000u) && name == id)) return off ? off : 0xFFFFFFFFu;
    }
    (void)root;
    return 0;
}
bool rsrc_find(uint32_t type, uint32_t id, uint32_t *addr, uint32_t *size) {
    uint32_t root = rsrc_base();
    uint32_t t = rsrc_dir_find(root, type, false);
    if (!t || !(t & 0x80000000u)) return false;
    uint32_t n = rsrc_dir_find(root + (t & 0x7FFFFFFFu), id, false);
    if (!n || !(n & 0x80000000u)) return false;
    uint32_t l = rsrc_dir_find(root + (n & 0x7FFFFFFFu), 0, true);
    if (!l || (l & 0x80000000u)) return false;
    uint32_t data = root + l;
    *addr = g_image_base + R32(data);
    *size = R32(data + 4);
    return true;
}

void I_user32_LoadStringA(CPU *c) { /* (hinst, id, buf, n) */
    uint32_t id = ARG(1), buf = ARG(2), n = ARG(3), a, sz;
    if (!n) { RET(4, 0); return; }
    if (!rsrc_find(6, id / 16 + 1, &a, &sz)) { W8(buf, 0); RET(4, 0); return; }
    for (uint32_t i = 0; i < id % 16; i++) a += 2 + 2 * R16(a);
    uint32_t len = R16(a), k;
    for (k = 0; k < len && k + 1 < n; k++) {
        uint16_t ch = R16(a + 2 + 2 * k);
        W8(buf + k, ch < 256 ? (uint8_t)ch : '?');
    }
    W8(buf + k, 0);
    rt_log("LoadStringA(%u) = \"%s\"", id, GSTR(buf));
    RET(4, k);
}

/* ------------------------------------------------------------------ message queues */
/* One queue per thread: window and input messages go to the window's thread (the main game
 * thread, id 0x80); PostThreadMessage reaches the named thread (the movie decoder uses this). */
typedef struct { uint32_t hwnd, msg, wp, lp, time; } Msg;
#define QN 1024
typedef struct {
    uint32_t tid;
    Msg q[QN];
    unsigned head, tail;
    bool quit;
    uint32_t quit_code;
    pthread_cond_t cv;
} Queue;
static Queue g_queues[16];
static int g_nqueues;
static pthread_mutex_t g_qmu = PTHREAD_MUTEX_INITIALIZER;
uint32_t k32_current_tid(void);
#define MAIN_TID 0x80u

static Queue *queue_for(uint32_t tid) { /* caller holds g_qmu */
    for (int i = 0; i < g_nqueues; i++)
        if (g_queues[i].tid == tid) return &g_queues[i];
    if (g_nqueues >= 16) return &g_queues[0];
    Queue *q = &g_queues[g_nqueues++];
    memset(q, 0, sizeof *q);
    q->tid = tid;
    pthread_cond_init(&q->cv, NULL);
    return q;
}
static void post_to(uint32_t tid, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    pthread_mutex_lock(&g_qmu);
    Queue *q = queue_for(tid);
    if (q->tail - q->head < QN) {
        Msg *m = &q->q[q->tail++ % QN];
        m->hwnd = hwnd;
        m->msg = msg;
        m->wp = wp;
        m->lp = lp;
        m->time = (uint32_t)(plat_ticks_us() / 1000);
    }
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&g_qmu);
}
void user32_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) { post_to(MAIN_TID, hwnd, msg, wp, lp); }
/* platform input arrives here (any thread) */
void user32_post_input(uint32_t msg, uint32_t wp, uint32_t lp) { user32_post(HWND_MAIN, msg, wp, lp); }

static bool q_take(Queue *q, Msg *out, uint32_t lo, uint32_t hi, bool remove) {
    for (unsigned i = q->head; i != q->tail; i++) {
        Msg *m = &q->q[i % QN];
        if ((lo || hi) && (m->msg < lo || m->msg > hi)) continue;
        *out = *m;
        if (remove) {
            for (unsigned j = i; j + 1 != q->tail; j++) q->q[j % QN] = q->q[(j + 1) % QN];
            q->tail--;
        }
        return true;
    }
    return false;
}
static void put_msg(uint32_t p, const Msg *m) {
    W32(p, m->hwnd);
    W32(p + 4, m->msg);
    W32(p + 8, m->wp);
    W32(p + 12, m->lp);
    W32(p + 16, m->time);
    W32(p + 20, 0);
    W32(p + 24, 0);
}
void I_user32_PeekMessageA(CPU *c) { /* (msg, hwnd, min, max, remove) */
    ddraw_idle();
    harvest_tick(c);
    Msg m;
    pthread_mutex_lock(&g_qmu);
    Queue *q = queue_for(k32_current_tid());
    bool got = q_take(q, &m, ARG(2), ARG(3), ARG(4) & 1);
    if (!got && q->quit) {
        m.hwnd = 0; m.msg = WM_QUIT; m.wp = q->quit_code; m.lp = 0; m.time = 0;
        got = true;
        if (ARG(4) & 1) q->quit = false;
    }
    pthread_mutex_unlock(&g_qmu);
    if (got) put_msg(ARG(0), &m);
    RET(5, got ? 1 : 0);
}
void I_user32_GetMessageA(CPU *c) { /* (msg, hwnd, min, max) */
    ddraw_idle();
    Msg m;
    pthread_mutex_lock(&g_qmu);
    Queue *q = queue_for(k32_current_tid());
    for (;;) {
        if (q_take(q, &m, ARG(2), ARG(3), true)) break;
        if (q->quit) {
            m.hwnd = 0; m.msg = WM_QUIT; m.wp = q->quit_code; m.lp = 0; m.time = 0;
            q->quit = false;
            break;
        }
        pthread_cond_wait(&q->cv, &g_qmu);
    }
    pthread_mutex_unlock(&g_qmu);
    put_msg(ARG(0), &m);
    RET(4, m.msg == WM_QUIT ? 0 : 1);
}
void I_user32_PostMessageA(CPU *c) { user32_post(ARG(0), ARG(1), ARG(2), ARG(3)); RET(4, 1); }
void I_user32_PostThreadMessageA(CPU *c) { /* (tid, msg, wp, lp) */
    post_to(ARG(0), 0, ARG(1), ARG(2), ARG(3));
    RET(4, 1);
}
void I_user32_PostQuitMessage(CPU *c) {
    pthread_mutex_lock(&g_qmu);
    Queue *q = queue_for(k32_current_tid());
    q->quit = true;
    q->quit_code = ARG(0);
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&g_qmu);
    rt_log("PostQuitMessage(%u) on thread %x", ARG(0), k32_current_tid());
    RET(1, 0);
}
void I_user32_TranslateMessage(CPU *c) { RET(1, 0); }

/* ------------------------------------------------------------------ window */
static uint32_t g_wndproc, g_class_name;
static int g_cw = 640, g_ch = 480, g_wx, g_wy;
static uint32_t g_style;
static bool g_window_made;
static uint32_t send(CPU *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!g_wndproc) return 0;
    return call_guest_ret(c, g_wndproc, 4, hwnd, msg, wp, lp);
}
void I_user32_DispatchMessageA(CPU *c) {
    uint32_t p = ARG(0);
    uint32_t r = 0;
    uint32_t hwnd = R32(p), msg = R32(p + 4), wp = R32(p + 8), lp = R32(p + 12);
    if (hwnd == HWND_MAIN) r = send(c, hwnd, msg, wp, lp);
    RET(1, r);
}
void I_user32_DefWindowProcA(CPU *c) { /* (hwnd, msg, wp, lp) */
    uint32_t msg = ARG(1);
    if (msg == WM_CLOSE) {
        uint32_t hwnd = ARG(0);
        ESP += 0; /* send WM_DESTROY like DestroyWindow does */
        send(c, hwnd, WM_DESTROY, 0, 0);
    }
    RET(4, msg == WM_NCCREATE ? 1 : 0);
}
void I_user32_RegisterClassA(CPU *c) {
    uint32_t wc = ARG(0);
    g_wndproc = R32(wc + 4);
    g_class_name = R32(wc + 36);
    rt_log("RegisterClassA(%s) wndproc %08x", g_class_name > 0xFFFF ? GSTR(g_class_name) : "#", g_wndproc);
    RET(1, 0xC001);
}
void I_user32_FindWindowA(CPU *c) { RET(2, 0); }
void I_user32_CreateWindowExA(CPU *c) {
    /* (exstyle, class, title, style, x, y, w, h, parent, menu, hinst, param) */
    const char *title = ARG(2) ? GSTR(ARG(2)) : "";
    g_style = ARG(3);
    g_wx = (int32_t)ARG(4);
    g_wy = (int32_t)ARG(5);
    int w = (int32_t)ARG(6), h = (int32_t)ARG(7);
    rt_log("CreateWindowExA(%s, style %08x, %d,%d %dx%d)", title, g_style, g_wx, g_wy, w, h);
    if (w > 0 && w < 10000) g_cw = w;
    if (h > 0 && h < 10000) g_ch = h;
    plat_window_create(g_cw, g_ch, title);
    g_window_made = true;
    uint32_t cs = heap_alloc(48, true);
    W32(cs, ARG(11));
    W32(cs + 4, ARG(10));
    W32(cs + 8, ARG(9));
    W32(cs + 12, ARG(8));
    W32(cs + 16, (uint32_t)g_ch);
    W32(cs + 20, (uint32_t)g_cw);
    W32(cs + 24, (uint32_t)g_wy);
    W32(cs + 28, (uint32_t)g_wx);
    W32(cs + 32, g_style);
    W32(cs + 36, ARG(2));
    W32(cs + 40, ARG(1));
    W32(cs + 44, ARG(0));
    uint32_t args[12];
    for (int i = 0; i < 12; i++) args[i] = ARG(i);
    send(c, HWND_MAIN, WM_NCCREATE, 0, cs);
    send(c, HWND_MAIN, WM_CREATE, 0, cs);
    (void)args;
    RET(12, HWND_MAIN);
}
void I_user32_ShowWindow(CPU *c) {
    rt_log("ShowWindow(%u)", ARG(1));
    plat_window_show();
    send(c, HWND_MAIN, WM_SHOWWINDOW, 1, 0);
    send(c, HWND_MAIN, WM_ACTIVATEAPP, 1, 0);
    send(c, HWND_MAIN, WM_ACTIVATE, 1, 0);
    send(c, HWND_MAIN, WM_SETFOCUS, 0, 0);
    send(c, HWND_MAIN, WM_SIZE, 0, (uint32_t)g_cw | ((uint32_t)g_ch << 16));
    RET(2, 0);
}
void I_user32_UpdateWindow(CPU *c) { RET(1, 1); }
void I_user32_MoveWindow(CPU *c) { /* (hwnd, x, y, w, h, repaint) */
    rt_log("MoveWindow(%d,%d %dx%d)", (int32_t)ARG(1), (int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(4));
    RET(6, 1);
}
void I_user32_SetWindowTextA(CPU *c) { plat_window_set_title(GSTR(ARG(1))); RET(2, 1); }
void I_user32_DestroyWindow(CPU *c) {
    rt_log("DestroyWindow");
    send(c, HWND_MAIN, WM_DESTROY, 0, 0);
    RET(1, 1);
}
void I_user32_GetClientRect(CPU *c) {
    uint32_t r = ARG(1);
    int w, h;
    plat_client_size(&w, &h);
    W32(r, 0);
    W32(r + 4, 0);
    W32(r + 8, (uint32_t)w);
    W32(r + 12, (uint32_t)h);
    RET(2, 1);
}
void I_user32_ClientToScreen(CPU *c) { RET(2, 1); } /* client origin is the screen origin */
void I_user32_GetSystemMetrics(CPU *c) {
    int w, h;
    plat_screen_size(&w, &h);
    uint32_t i = ARG(0), v = 0;
    if (i == 0) v = (uint32_t)w;        /* SM_CXSCREEN */
    else if (i == 1) v = (uint32_t)h;   /* SM_CYSCREEN */
    else if (i == 4) v = 20;            /* SM_CYCAPTION */
    else if (i == 7 || i == 8) v = 3;   /* fixed frame */
    else if (i == 32 || i == 33) v = 4; /* sizing frame */
    RET(1, v);
}
static int g_cursor = 0;
void I_user32_ShowCursor(CPU *c) {
    g_cursor += ARG(0) ? 1 : -1;
    plat_cursor_visible(g_cursor >= 0);
    RET(1, (uint32_t)g_cursor);
}
void I_user32_LoadIconA(CPU *c) { RET(2, 0x00020001u); }
void I_user32_MessageBoxA(CPU *c) { /* (hwnd, text, caption, type) */
    rt_log("MessageBoxA(%s: %s) type %x", ARG(2) ? GSTR(ARG(2)) : "", ARG(1) ? GSTR(ARG(1)) : "", ARG(3));
    RET(4, (ARG(3) & 0xF) == 4 ? 6 : 1); /* MB_YESNO -> IDYES, otherwise IDOK */
}
void I_user32_GetAsyncKeyState(CPU *c) { RET(1, plat_vk_down((int)ARG(0)) ? 0x8000u : 0); }
void I_user32_MapVirtualKeyA(CPU *c) { /* (code, type): 0 vk->scan, 1 scan->vk */
    RET(2, plat_map_key(ARG(0), ARG(1)));
}
void I_user32_GetKeyNameTextA(CPU *c) { /* (lparam, buf, n) */
    uint32_t scan = (ARG(0) >> 16) & 0x1FF, buf = ARG(1), n = ARG(2);
    const char *name = plat_scan_name(scan);
    snprintf((char *)GPTR(buf), n, "%s", name);
    RET(3, (uint32_t)strlen(GSTR(buf)));
}
uint32_t gdi_load_bitmap_file(const char *host_path);
uint32_t gdi_load_bitmap_resource(uint32_t id);
void I_user32_LoadImageA(CPU *c) { /* (hinst, name, type, cx, cy, flags) */
    uint32_t name = ARG(1), type = ARG(2), flags = ARG(5);
    if (type == 0 && (flags & 0x10)) {
        char host[1200];
        path_to_host(GSTR(name), host, sizeof host);
        uint32_t h = gdi_load_bitmap_file(host);
        rt_log("LoadImageA(file %s) -> %08x", GSTR(name), h);
        RET(6, h);
        return;
    }
    if (type == 0 && name < 0x10000) {
        uint32_t h = gdi_load_bitmap_resource(name);
        rt_log("LoadImageA(bitmap resource %u) -> %08x", name, h);
        RET(6, h);
        return;
    }
    rt_log("LoadImageA(%s, type %u, flags %x) -> fake", name > 0xFFFF ? GSTR(name) : "#id", type, flags);
    RET(6, 0x00030001u);
}
