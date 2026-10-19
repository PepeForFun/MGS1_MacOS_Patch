/* Runtime-internal declarations shared by the Windows/DirectX replacements. */
#pragma once
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include "recomp.h"
#include "tables.h"

/* ---- guest address space layout */
#define G_NULL_GUARD   0x00010000u
#define G_TEB_BASE     0x00020000u   /* one 4 KB TEB per thread, up to 64 threads */
#define G_PEB          0x00060000u
#define G_MISC         0x00070000u   /* static strings handed to the game (command line, env) */
#define G_HEAP_LO      0x01000000u
#define G_HEAP_HI      0x30000000u   /* 0x30000000-0x50000000: VirtualAlloc */
#define G_STACK_LO     0x50000000u
#define G_STACK_HI     0x60000000u
#define G_HOST_LO      0x60000000u   /* objects the runtime creates for the game (COM objects, ...) */
#define G_HOST_HI      0x70000000u
#define G_THUNK_BASE   0xFFF00000u   /* host functions get fake addresses here (never mapped) */
#define G_THUNK_STEP   16u
#define G_RET_SENTINEL 0xFFEFFFF0u   /* return address used when the host calls guest code */

/* ---- arguments of stdcall imports: [esp] = return address, [esp+4] = arg 0 */
#define ARG(n) R32(ESP + 4u + 4u * (n))
#define RET(nargs, v) do { EAX = (uint32_t)(v); ESP += 4u + 4u * (nargs); } while (0)
#define GSTR(a) ((const char *)(M + (a)))
#define GPTR(a) ((void *)(M + (a)))

void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
__attribute__((noreturn)) void rt_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int g_trace;  /* log every import call when set */

/* memory */
void mem_init(void);
uint32_t heap_alloc(uint32_t size, bool zero);
void heap_free(uint32_t p);
uint32_t heap_size(uint32_t p);
uint32_t heap_realloc(uint32_t p, uint32_t size, bool zero);
uint32_t host_alloc(uint32_t size);  /* never freed; for runtime-owned guest objects */
uint32_t guest_strdup(const char *s);

/* host functions callable from guest code */
uint32_t rt_thunk(void (*fn)(CPU *c), const char *name);
void call_guest(CPU *c, uint32_t addr, int nargs, ...);  /* stdcall/cdecl callback into guest code */
uint32_t call_guest_ret(CPU *c, uint32_t addr, int nargs, ...);

/* threads */
CPU *cpu_new(uint32_t stack_size);
CPU *cpu_current(void);
void cpu_bind(CPU *c);
uint32_t thread_start(uint32_t entry, uint32_t param, bool suspended, uint32_t *tid);

/* handles */
enum { H_FREE, H_FILE, H_EVENT, H_THREAD, H_MAPPING, H_FIND, H_HEAP, H_PROCESS, H_MODULE, H_OTHER };
typedef struct {
    int type;
    void *p;
    int fd;
    uint32_t extra;
} KHandle;
uint32_t handle_new(int type, void *p, int fd);
KHandle *handle_get(uint32_t h, int type);
void handle_close(uint32_t h);

/* paths: game folder and Windows-to-host translation */
extern char g_game_dir[1024];
void path_to_host(const char *win, char *out, size_t n);
void path_to_win(const char *host, char *out, size_t n);

/* platform (platform.m) */
void plat_init(void);
void plat_run_main_loop(void (*game_thread)(void));   /* never returns */
uint64_t plat_ticks_us(void);
extern uint64_t g_plat_t0;   /* plat_init time: MGS_KEYS and MGS_SHOT_FROM count from here */
bool rt_bltlog(void);        /* MGS_BLTLOG=s: log 2D drawing (blits, locks, DCs, GDI) from s seconds after start */
bool rt_after(const char *env);   /* env=s is set and s seconds have passed since start */
bool seh_dispatch(CPU *c, uint32_t fault_addr);   /* fault in guest code: resume in an __except handler */
void plat_sleep_ms(uint32_t ms);
void plat_window_create(int w, int h, const char *title);
void plat_window_show(void);
void plat_window_fullscreen(bool game_wants);   /* first call decides: last player choice, else the game */
void plat_window_set_title(const char *t);
void plat_client_size(int *w, int *h);     /* size the game believes its window has */
void plat_set_game_size(int w, int h);     /* the game changed display mode */
void plat_screen_size(int *w, int *h);
void plat_cursor_visible(bool v);
bool plat_vk_down(int vk);
bool plat_scan_down(int scan);
uint32_t plat_map_key(uint32_t code, uint32_t type);
const char *plat_scan_name(uint32_t scan);
void *plat_metal_layer(void);              /* CAMetalLayer of the game view */
void plat_drawable_size(int *w, int *h);   /* pixels of the view's backing store */

/* game controller (platform.m) */
typedef struct { bool connected; float lx, ly, rx, ry, lt, rt; uint32_t buttons; } PadState;
enum { PAD_A = 1, PAD_B = 2, PAD_X = 4, PAD_Y = 8, PAD_LB = 16, PAD_RB = 32, PAD_LT = 64, PAD_RT = 128,
       PAD_BACK = 256, PAD_START = 512, PAD_LS = 1024, PAD_RS = 2048,
       PAD_UP = 1 << 16, PAD_DOWN = 1 << 17, PAD_LEFT = 1 << 18, PAD_RIGHT = 1 << 19 };
void plat_pad(PadState *s);
bool plat_eula_accepted(const char *eula_host_path);
void dinput_key_event(int scan, bool down);

/* user32 */
void user32_post_input(uint32_t msg, uint32_t wp, uint32_t lp);
void ddraw_idle(void);   /* show front-buffer drawing that is still waiting (game thread polls input/messages) */
void harvest_tick(CPU *c);                          /* texture harvest driver (MGS_HARVEST), game thread */
void harvest_stdout(const char *text, uint32_t n);  /* the game printed text */
bool rsrc_find(uint32_t type, uint32_t id, uint32_t *addr, uint32_t *size);
