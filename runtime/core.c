/* Core runtime: guest memory, heap, host thunks, indirect calls, threads, handles, x87 helpers. */
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "rt.h"

uint8_t *M;
int g_trace;
static FILE *g_logf;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ SEH */
/* Landings of the functions whose frames may be live, newest last. A landing whose frame is at or
 * below a new one's belongs to a function that has returned. */
static __thread SehLanding *t_seh[256];
static __thread int t_nseh;
void seh_push(SehLanding *l) {
    while (t_nseh && t_seh[t_nseh - 1]->frame <= l->frame) t_nseh--;
    if (t_nseh < 256) t_seh[t_nseh++] = l;
}
/* Walk the guest's exception chain like _except_handler3: the first frame with an __except whose
 * filter is "mov eax, 1; ret" (EXCEPTION_EXECUTE_HANDLER) and whose function registered a landing
 * gets control: trylevel set to the enclosing one, inner records dropped, EBP/ESP of the frame. */
bool seh_dispatch(CPU *c, uint32_t fault_addr) {
    if (!c) return false;
    uint32_t rec = R32(c->fs_base);
    for (int depth = 0; rec && rec != 0xFFFFFFFFu && depth < 64; depth++, rec = R32(rec)) {
        uint32_t frame = rec + 0x10, table = R32(rec + 8);
        int32_t level = (int32_t)R32(rec + 12);
        for (int k = 0; level >= 0 && level < 64 && k < 16; k++) {
            uint32_t e = table + 12u * (uint32_t)level, filt = R32(e + 4), hand = R32(e + 8);
            if (filt && !memcmp(M + filt, "\xB8\x01\x00\x00\x00\xC3", 6)) {
                for (int i = t_nseh - 1; i >= 0; i--)
                    if (t_seh[i]->frame == frame) {
                        SehLanding *l = t_seh[i];
                        t_nseh = i + 1;
                        W32(rec + 12, R32(e));
                        W32(c->fs_base, rec);
                        c->r[5].d = frame;
                        c->r[4].d = R32(frame - 0x18);
                        l->target = hand;
                        rt_log("SEH: fault at %08x handled by %08x, as on Windows", fault_addr, hand);
                        siglongjmp(l->jb, 1);
                    }
                return false;
            }
            level = (int32_t)R32(e);
        }
    }
    return false;
}

bool rt_after(const char *env) {
    const char *v = getenv(env);
    return v && plat_ticks_us() >= g_plat_t0 + (uint64_t)(atof(v) * 1e6);
}
bool rt_bltlog(void) {
    static double from = -2;
    if (from == -2) from = getenv("MGS_BLTLOG") ? atof(getenv("MGS_BLTLOG")) : -1;
    return from >= 0 && plat_ticks_us() >= g_plat_t0 + (uint64_t)(from * 1e6);
}

void rt_log(const char *fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&g_log_mu);
    if (!g_logf) {
        const char *p = getenv("MGS_LOG");
        g_logf = fopen(p ? p : "/tmp/mgs-recomp.log", "w");
        if (!g_logf) g_logf = stderr;
    }
    va_start(ap, fmt);
    vfprintf(g_logf, fmt, ap);
    va_end(ap);
    fputc('\n', g_logf);
    fflush(g_logf);
    if (getenv("MGS_LOG_STDERR")) {
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }
    pthread_mutex_unlock(&g_log_mu);
}

void rt_fatal(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    CPU *c = cpu_current();
    rt_log("FATAL: %s", buf);
    if (c) {
        rt_log("  eip(block)=%08x eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x",
               c->eip, EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI);
        /* guest stack words: return addresses into .text stand out */
        char line[512];
        int n = 0;
        for (int i = 0; i < 48 && n < (int)sizeof line - 16; i++) {
            uint32_t v = R32(ESP + 4 * i);
            if (v >= 0x401000 && v < 0x64a000) n += snprintf(line + n, sizeof line - n, " %08x", v);
        }
        rt_log("  code addresses on stack:%s", line);
    }
    fprintf(stderr, "FATAL: %s (see log)\n", buf);
    _exit(3);
}

/* ------------------------------------------------------------------ memory */
void mem_init(void) {
    /* 4 GB of guest address space. Pages are allocated lazily by the kernel. */
    void *p = mmap(NULL, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) rt_fatal("cannot reserve guest memory: %s", strerror(errno));
    M = p;
    mprotect(M, G_NULL_GUARD, PROT_NONE);                         /* null pointer guard */
    mprotect(M + G_THUNK_BASE, 0x100000000ull - G_THUNK_BASE, PROT_NONE);
}

/* Heap: size classes for small blocks, first-fit extents for big ones. 16-byte header before each block. */
static pthread_mutex_t g_heap_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_heap_top = G_HEAP_LO;
#define NCLASS 64
static uint32_t g_free[NCLASS];      /* singly linked free lists (next pointer in user area) */
typedef struct { uint32_t addr, size; } Extent;
static Extent g_big_free[4096];
static int g_nbig;
#define HDR 16u
#define MAGIC 0x4D475348u

static uint32_t class_size(int k) { return k < 48 ? 16u * (k + 1) : 1024u << (k - 48); } /* 16..768, 1K..32M */
static int class_of(uint32_t n) {
    if (n <= 768) return (int)((n + 15) / 16) - 1;
    for (int k = 48; k < NCLASS; k++) if (class_size(k) >= n) return k;
    return -1;
}

static uint32_t bump(uint32_t n) {
    uint32_t p = g_heap_top;
    if (G_HEAP_HI - p < n) rt_fatal("guest heap exhausted (%u bytes requested)", n);
    g_heap_top += n;
    return p;
}

uint32_t heap_alloc(uint32_t size, bool zero) {
    if (size == 0) size = 1;
    pthread_mutex_lock(&g_heap_mu);
    uint32_t total = size + HDR, blk = 0, cap;
    int k = class_of(total);
    if (k >= 0 && k < 58) {
        cap = class_size(k);
        if (g_free[k]) {
            blk = g_free[k];
            g_free[k] = R32(blk + HDR);
        } else {
            blk = bump(cap);
        }
    } else {
        cap = (total + 0xFFFFu) & ~0xFFFFu;
        for (int i = 0; i < g_nbig; i++)
            if (g_big_free[i].size >= cap) {
                blk = g_big_free[i].addr;
                if (g_big_free[i].size > cap) {
                    g_big_free[i].addr += cap;
                    g_big_free[i].size -= cap;
                } else {
                    g_big_free[i] = g_big_free[--g_nbig];
                }
                break;
            }
        if (!blk) {
            g_heap_top = (g_heap_top + 0xFFFFu) & ~0xFFFFu;
            blk = bump(cap);
        }
        k = 255;
    }
    pthread_mutex_unlock(&g_heap_mu);
    W32(blk, MAGIC);
    W32(blk + 4, size);
    W32(blk + 8, cap);
    W32(blk + 12, (uint32_t)k);
    if (zero) memset(M + blk + HDR, 0, size);
    return blk + HDR;
}

void heap_free(uint32_t p) {
    if (!p) return;
    uint32_t blk = p - HDR;
    if (R32(blk) != MAGIC) {
        rt_log("heap_free: bad block %08x", p);
        return;
    }
    uint32_t cap = R32(blk + 8), k = R32(blk + 12);
    W32(blk, 0);
    pthread_mutex_lock(&g_heap_mu);
    if (k < 58) {
        W32(blk + HDR, g_free[k]);
        g_free[k] = blk;
    } else if (g_nbig < 4096) {
        g_big_free[g_nbig].addr = blk;
        g_big_free[g_nbig].size = cap;
        g_nbig++;
    }
    pthread_mutex_unlock(&g_heap_mu);
}

uint32_t heap_size(uint32_t p) {
    if (!p || R32(p - HDR) != MAGIC) return 0xFFFFFFFFu;
    return R32(p - HDR + 4);
}

uint32_t heap_realloc(uint32_t p, uint32_t size, bool zero) {
    if (!p) return heap_alloc(size, zero);
    uint32_t old = heap_size(p);
    if (old == 0xFFFFFFFFu) return 0;
    uint32_t cap = R32(p - HDR + 8);
    if (size + HDR <= cap) {
        if (zero && size > old) memset(M + p + old, 0, size - old);
        W32(p - HDR + 4, size);
        return p;
    }
    uint32_t q = heap_alloc(size, zero);
    memcpy(M + q, M + p, old < size ? old : size);
    heap_free(p);
    return q;
}

static pthread_mutex_t g_host_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_host_top = G_HOST_LO;
uint32_t host_alloc(uint32_t size) {
    pthread_mutex_lock(&g_host_mu);
    uint32_t p = (g_host_top + 15) & ~15u;
    g_host_top = p + size;
    if (g_host_top > G_HOST_HI) rt_fatal("host object area exhausted");
    pthread_mutex_unlock(&g_host_mu);
    memset(M + p, 0, size);
    return p;
}

uint32_t guest_strdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1, p = host_alloc(n);
    memcpy(M + p, s, n);
    return p;
}

/* ------------------------------------------------------------------ thunks and dispatch */
#define MAX_THUNKS 8192
static void (*g_thunk_fn[MAX_THUNKS])(CPU *);
static const char *g_thunk_name[MAX_THUNKS];
static int g_nthunks;
static pthread_mutex_t g_thunk_mu = PTHREAD_MUTEX_INITIALIZER;

uint32_t rt_thunk(void (*fn)(CPU *), const char *name) {
    pthread_mutex_lock(&g_thunk_mu);
    for (int i = 0; i < g_nthunks; i++)
        if (g_thunk_fn[i] == fn) {
            pthread_mutex_unlock(&g_thunk_mu);
            return G_THUNK_BASE + G_THUNK_STEP * (uint32_t)i;
        }
    if (g_nthunks >= MAX_THUNKS) rt_fatal("too many host thunks");
    int i = g_nthunks++;
    g_thunk_fn[i] = fn;
    g_thunk_name[i] = name;
    pthread_mutex_unlock(&g_thunk_mu);
    return G_THUNK_BASE + G_THUNK_STEP * (uint32_t)i;
}

static void (*lookup_fn(uint32_t a))(CPU *) {
    uint32_t lo = 0, hi = g_fn_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_fn_addr[mid] < a) lo = mid + 1;
        else hi = mid;
    }
    if (lo < g_fn_count && g_fn_addr[lo] == a) return g_fn_ptr[lo];
    return NULL;
}

void call_indirect(CPU *c, uint32_t target) {
    if (target >= G_THUNK_BASE) {
        uint32_t i = (target - G_THUNK_BASE) / G_THUNK_STEP;
        if (i < (uint32_t)g_nthunks && (target - G_THUNK_BASE) % G_THUNK_STEP == 0) {
            if (g_trace) {
                rt_log("[%08x] -> %s", c->eip, g_thunk_name[i]);
                if (g_trace > 1 && --g_trace == 1) g_trace = 0;  /* MGS_TRACE_AT=t:n traces n calls */
            }
            g_thunk_fn[i](c);
            return;
        }
    }
    void (*f)(CPU *) = lookup_fn(target);
    if (!f) rt_fatal("indirect call/jump to unknown address %08x (from block %08x)", target, c->eip);
    f(c);
}

/* Call guest code from the host: push args right to left, a sentinel return address, run. */
static uint32_t call_guest_v(CPU *c, uint32_t addr, int nargs, va_list ap) {
    uint32_t args[16];
    for (int i = 0; i < nargs; i++) args[i] = va_arg(ap, uint32_t);
    uint32_t esp0 = ESP;
    for (int i = nargs - 1; i >= 0; i--) PUSH32(args[i]);
    PUSH32(G_RET_SENTINEL);
    uint32_t saved_eip = c->eip;
    call_indirect(c, addr);
    c->eip = saved_eip;
    /* stdcall callees pop their arguments; cdecl ones leave them */
    ESP = esp0;
    return EAX;
}
void call_guest(CPU *c, uint32_t addr, int nargs, ...) {
    va_list ap;
    va_start(ap, nargs);
    call_guest_v(c, addr, nargs, ap);
    va_end(ap);
}
uint32_t call_guest_ret(CPU *c, uint32_t addr, int nargs, ...) {
    va_list ap;
    va_start(ap, nargs);
    uint32_t r = call_guest_v(c, addr, nargs, ap);
    va_end(ap);
    return r;
}

void rt_trap(CPU *c, uint32_t addr, const char *why) {
    (void)c;
    rt_fatal("trap at %08x: %s", addr, why);
}

void rt_div_fault(CPU *c, uint32_t addr) {
    (void)c;
    rt_fatal("integer divide fault at %08x", addr);
}

void rt_unimplemented(CPU *c, const char *dll, const char *name) {
    (void)c;
    rt_fatal("unimplemented import %s!%s", dll, name);
}

void rt_cpuid(CPU *c) {
    uint32_t leaf = EAX;
    if (leaf == 0) {
        EAX = 1;
        EBX = 0x756e6547; /* "GenuineIntel" */
        EDX = 0x49656e69;
        ECX = 0x6c65746e;
    } else if (leaf == 1) {
        EAX = 0x00000652; /* Pentium II */
        EBX = 0;
        ECX = 0;
        EDX = 0x0080A111;  /* FPU TSC CX8 CMOV MMX */
    } else {
        EAX = EBX = ECX = EDX = 0;
    }
}

void rt_rdtsc(CPU *c) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    uint64_t ns = mach_absolute_time() * tb.numer / tb.denom;
    uint64_t cyc = ns * 3 / 10;  /* pretend 300 MHz */
    EAX = (uint32_t)cyc;
    EDX = (uint32_t)(cyc >> 32);
}

uint64_t plat_ticks_us(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom / 1000;
}

void plat_sleep_ms(uint32_t ms) { usleep(ms * 1000); }

/* ------------------------------------------------------------------ threads */
static pthread_key_t g_cpu_key;
static pthread_once_t g_cpu_once = PTHREAD_ONCE_INIT;
static uint32_t g_next_teb;
static uint32_t g_next_stack = G_STACK_LO;
static pthread_mutex_t g_thr_mu = PTHREAD_MUTEX_INITIALIZER;
static void make_key(void) { pthread_key_create(&g_cpu_key, NULL); }

CPU *cpu_current(void) {
    pthread_once(&g_cpu_once, make_key);
    return pthread_getspecific(g_cpu_key);
}

CPU *cpu_new(uint32_t stack_size) {
    pthread_once(&g_cpu_once, make_key);
    CPU *c = calloc(1, sizeof(CPU));
    pthread_mutex_lock(&g_thr_mu);
    if (stack_size < 0x100000) stack_size = 0x100000;
    stack_size = (stack_size + 0xFFFF) & ~0xFFFFu;
    uint32_t lo = g_next_stack, hi = lo + stack_size;
    g_next_stack = hi + 0x10000; /* guard gap */
    if (g_next_stack > G_STACK_HI) rt_fatal("out of guest stack space");
    uint32_t teb = G_TEB_BASE + 0x1000 * g_next_teb++;
    pthread_mutex_unlock(&g_thr_mu);
    memset(M + teb, 0, 0x1000);
    W32(teb + 0x00, 0xFFFFFFFFu); /* SEH chain end */
    W32(teb + 0x04, hi);
    W32(teb + 0x08, lo);
    W32(teb + 0x18, teb);
    W32(teb + 0x30, G_PEB);
    c->fs_base = teb;
    ESP = hi - 64;
    fninit(c);
    c->fcw = 0x27F; /* Windows default: 53-bit precision, all exceptions masked */
    return c;
}

void cpu_bind(CPU *c) {
    pthread_once(&g_cpu_once, make_key);
    pthread_setspecific(g_cpu_key, c);
}

/* ------------------------------------------------------------------ handles */
#define MAX_HANDLES 4096
static KHandle g_handles[MAX_HANDLES];
static pthread_mutex_t g_h_mu = PTHREAD_MUTEX_INITIALIZER;

uint32_t handle_new(int type, void *p, int fd) {
    pthread_mutex_lock(&g_h_mu);
    for (int i = 16; i < MAX_HANDLES; i++)
        if (g_handles[i].type == H_FREE) {
            g_handles[i].type = type;
            g_handles[i].p = p;
            g_handles[i].fd = fd;
            g_handles[i].extra = 0;
            pthread_mutex_unlock(&g_h_mu);
            return (uint32_t)i * 4;
        }
    pthread_mutex_unlock(&g_h_mu);
    rt_fatal("out of handles");
}

KHandle *handle_get(uint32_t h, int type) {
    uint32_t i = h / 4;
    if (h % 4 || i >= MAX_HANDLES || g_handles[i].type == H_FREE) return NULL;
    if (type && g_handles[i].type != type) return NULL;
    return &g_handles[i];
}

void handle_close(uint32_t h) {
    uint32_t i = h / 4;
    if (h % 4 || i >= MAX_HANDLES) return;
    pthread_mutex_lock(&g_h_mu);
    g_handles[i].type = H_FREE;
    pthread_mutex_unlock(&g_h_mu);
}

