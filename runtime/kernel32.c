/* KERNEL32 replacements (the 116 functions the game imports). */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "rt.h"

#define TRACE(...) do { if (g_trace) rt_log(__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ errors (TEB+0x34) */
static void set_err(CPU *c, uint32_t e) { W32(c->fs_base + 0x34, e); }
static uint32_t errno_to_win(int e) {
    switch (e) {
    case ENOENT: return 2;   /* ERROR_FILE_NOT_FOUND */
    case ENOTDIR: return 3;  /* ERROR_PATH_NOT_FOUND */
    case EACCES: case EPERM: return 5;
    case EEXIST: return 183; /* ERROR_ALREADY_EXISTS */
    default: return 31;      /* ERROR_GEN_FAILURE */
    }
}

void I_kernel32_GetLastError(CPU *c) { RET(0, R32(c->fs_base + 0x34)); }
void I_kernel32_SetLastError(CPU *c) { set_err(c, ARG(0)); RET(1, 0); }

/* ------------------------------------------------------------------ process / version / startup */
extern uint32_t g_cmdline_ptr;
void I_kernel32_GetCommandLineA(CPU *c) { RET(0, g_cmdline_ptr); }
void I_kernel32_GetVersion(CPU *c) { RET(0, 0x0A280105u); } /* NT 5.1 build 2600 (Windows XP) */
void I_kernel32_GetVersionExA(CPU *c) {
    uint32_t p = ARG(0);
    W32(p + 4, 5);
    W32(p + 8, 1);
    W32(p + 12, 2600);
    W32(p + 16, 2); /* VER_PLATFORM_WIN32_NT */
    strcpy((char *)GPTR(p + 20), "Service Pack 3");
    RET(1, 1);
}
void I_kernel32_GetStartupInfoA(CPU *c) {
    uint32_t p = ARG(0);
    memset(GPTR(p), 0, 68);
    W32(p, 68);
    RET(1, 0);
}
void I_kernel32_ExitProcess(CPU *c) {
    rt_log("ExitProcess(%u)", ARG(0));
    exit((int)ARG(0));
}
void I_kernel32_TerminateProcess(CPU *c) {
    rt_log("TerminateProcess(%08x, %u)", ARG(0), ARG(1));
    if (ARG(0) == 0xFFFFFFFFu) exit((int)ARG(1));
    RET(2, 0);
}
void I_kernel32_GetCurrentProcess(CPU *c) { RET(0, 0xFFFFFFFFu); }
void I_kernel32_GetExitCodeProcess(CPU *c) { W32(ARG(1), 0); RET(2, 1); }
void I_kernel32_CreateProcessA(CPU *c) {
    rt_log("CreateProcessA(%s, %s) refused", ARG(0) ? GSTR(ARG(0)) : "-", ARG(1) ? GSTR(ARG(1)) : "-");
    set_err(c, 2);
    RET(10, 0);
}
void I_kernel32_SetErrorMode(CPU *c) { RET(1, 0); }
void I_kernel32_SetHandleCount(CPU *c) { RET(1, ARG(0)); }
void I_kernel32_SetConsoleCtrlHandler(CPU *c) { RET(2, 1); }
void I_kernel32_IsBadReadPtr(CPU *c) { RET(2, ARG(0) < G_NULL_GUARD && ARG(1) ? 1 : 0); }
void I_kernel32_IsBadWritePtr(CPU *c) { RET(2, ARG(0) < G_NULL_GUARD && ARG(1) ? 1 : 0); }
void I_kernel32_OutputDebugStringA(CPU *c) { rt_log("OutputDebugString: %s", GSTR(ARG(0))); RET(1, 0); }
void I_kernel32_DebugBreak(CPU *c) { rt_log("DebugBreak at block %08x", c->eip); RET(0, 0); }
void I_kernel32_FatalAppExitA(CPU *c) { rt_fatal("FatalAppExit: %s", GSTR(ARG(1))); }
void I_kernel32_RaiseException(CPU *c) {
    rt_fatal("RaiseException code %08x flags %x nargs %u", ARG(0), ARG(1), ARG(2));
}
void I_kernel32_RtlUnwind(CPU *c) { rt_fatal("RtlUnwind called (SEH unwinding is not supported)"); }
void I_kernel32_UnhandledExceptionFilter(CPU *c) { RET(1, 1); }
void I_kernel32_MulDiv(CPU *c) { /* rounds half away from zero */
    int64_t a = (int32_t)ARG(0), b = (int32_t)ARG(1), d = (int32_t)ARG(2);
    if (!d) { RET(3, 0xFFFFFFFFu); return; }
    int64_t p = a * b, ad = d < 0 ? -d : d;
    int64_t q = (p >= 0 ? p + ad / 2 : p - ad / 2) / ad;
    RET(3, (uint32_t)(int32_t)(d < 0 ? -q : q));
}
void I_kernel32_GlobalMemoryStatus(CPU *c) {
    uint32_t p = ARG(0);
    W32(p, 32);
    W32(p + 4, 10);           /* memory load % */
    W32(p + 8, 512u << 20);   /* total phys */
    W32(p + 12, 384u << 20);  /* avail phys */
    W32(p + 16, 1024u << 20); /* total page file */
    W32(p + 20, 896u << 20);
    W32(p + 24, 2047u << 20); /* total virtual */
    W32(p + 28, 1800u << 20);
    RET(1, 0);
}

/* ------------------------------------------------------------------ environment and locale */
static uint32_t g_env_a;
void I_kernel32_GetEnvironmentStrings(CPU *c) {
    if (!g_env_a) {
        static const char env[] = "PATH=C:\\WINDOWS;C:\\WINDOWS\\SYSTEM32\0WINDIR=C:\\WINDOWS\0\0";
        g_env_a = host_alloc(sizeof env);
        memcpy(GPTR(g_env_a), env, sizeof env);
    }
    RET(0, g_env_a);
}
void I_kernel32_GetEnvironmentStringsW(CPU *c) { set_err(c, 120); RET(0, 0); }
void I_kernel32_FreeEnvironmentStringsA(CPU *c) { RET(1, 1); }
void I_kernel32_FreeEnvironmentStringsW(CPU *c) { RET(1, 1); }
void I_kernel32_SetEnvironmentVariableA(CPU *c) { RET(2, 1); }
void I_kernel32_GetACP(CPU *c) { RET(0, 1252); }
void I_kernel32_GetOEMCP(CPU *c) { RET(0, 437); }
void I_kernel32_IsValidCodePage(CPU *c) { RET(1, 1); }
void I_kernel32_IsValidLocale(CPU *c) { RET(2, 1); }
void I_kernel32_GetUserDefaultLCID(CPU *c) { RET(0, 0x409); }
void I_kernel32_EnumSystemLocalesA(CPU *c) { RET(2, 1); }
void I_kernel32_GetCPInfo(CPU *c) {
    uint32_t p = ARG(1);
    memset(GPTR(p), 0, 20);
    W32(p, 1);       /* MaxCharSize */
    W8(p + 4, '?');  /* DefaultChar */
    RET(2, 1);
}
static const char *locale_str(uint32_t type) {
    switch (type & 0xFFFF) {
    case 0x1004: return "1252";      /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x000B: return "437";       /* LOCALE_IDEFAULTCODEPAGE */
    case 0x0007: return "ENU";       /* LOCALE_SABBREVLANGNAME */
    case 0x0003: return "ENU";
    case 0x1001: return "English";   /* LOCALE_SENGLANGUAGE */
    case 0x1002: return "United States";
    case 0x0005: return "1";         /* LOCALE_ICOUNTRY */
    case 0x0007 + 0x100: return "USA";
    case 0x000E: return ".";         /* LOCALE_SDECIMAL */
    case 0x000F: return ",";         /* LOCALE_STHOUSAND */
    case 0x0010: return "3;0";
    case 0x0014: return "$";
    case 0x0016: return ".";
    case 0x0017: return ",";
    case 0x0018: return "3;0";
    case 0x0019: return "2";
    case 0x001A: return "2";
    case 0x001B: return "0";
    case 0x001C: return "0";
    default: return NULL;
    }
}
void I_kernel32_GetLocaleInfoA(CPU *c) {
    const char *s = locale_str(ARG(1));
    uint32_t buf = ARG(2), n = ARG(3);
    if (!s) { set_err(c, 87); RET(4, 0); return; }
    uint32_t len = (uint32_t)strlen(s) + 1;
    if (n) {
        if (n < len) { set_err(c, 122); RET(4, 0); return; }
        memcpy(GPTR(buf), s, len);
    }
    RET(4, len);
}
void I_kernel32_GetLocaleInfoW(CPU *c) {
    const char *s = locale_str(ARG(1));
    uint32_t buf = ARG(2), n = ARG(3);
    if (!s) { set_err(c, 87); RET(4, 0); return; }
    uint32_t len = (uint32_t)strlen(s) + 1;
    if (n) {
        if (n < len) { set_err(c, 122); RET(4, 0); return; }
        for (uint32_t i = 0; i < len; i++) W16(buf + 2 * i, (uint8_t)s[i]);
    }
    RET(4, len);
}
void I_kernel32_MultiByteToWideChar(CPU *c) {
    uint32_t src = ARG(2), dst = ARG(4), dn = ARG(5);
    int32_t sn = (int32_t)ARG(3);
    uint32_t n = sn < 0 ? (uint32_t)strlen(GSTR(src)) + 1 : (uint32_t)sn;
    if (dn == 0) { RET(6, n); return; }
    if (dn < n) n = dn;
    for (uint32_t i = 0; i < n; i++) W16(dst + 2 * i, R8(src + i));
    RET(6, n);
}
void I_kernel32_WideCharToMultiByte(CPU *c) {
    uint32_t src = ARG(2), dst = ARG(4), dn = ARG(5);
    int32_t sn = (int32_t)ARG(3);
    uint32_t n = 0;
    if (sn < 0) { while (R16(src + 2 * n)) n++; n++; } else n = (uint32_t)sn;
    if (dn == 0) { RET(8, n); return; }
    if (dn < n) n = dn;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t w = R16(src + 2 * i);
        W8(dst + i, w < 256 ? (uint8_t)w : '?');
    }
    RET(8, n);
}
/* LCMapString: lower/upper case mapping (LCMAP_LOWERCASE 0x100, LCMAP_UPPERCASE 0x200) */
void I_kernel32_LCMapStringA(CPU *c) {
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4), dn = ARG(5);
    int32_t sn = (int32_t)ARG(3);
    uint32_t n = sn < 0 ? (uint32_t)strlen(GSTR(src)) + 1 : (uint32_t)sn;
    if (dn == 0) { RET(6, n); return; }
    if (dn < n) { set_err(c, 122); RET(6, 0); return; }
    for (uint32_t i = 0; i < n; i++) {
        uint8_t ch = R8(src + i);
        if (flags & 0x100) ch = (uint8_t)tolower(ch);
        if (flags & 0x200) ch = (uint8_t)toupper(ch);
        W8(dst + i, ch);
    }
    RET(6, n);
}
void I_kernel32_LCMapStringW(CPU *c) {
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4), dn = ARG(5);
    int32_t sn = (int32_t)ARG(3);
    uint32_t n = 0;
    if (sn < 0) { while (R16(src + 2 * n)) n++; n++; } else n = (uint32_t)sn;
    if (dn == 0) { RET(6, n); return; }
    if (dn < n) { set_err(c, 122); RET(6, 0); return; }
    for (uint32_t i = 0; i < n; i++) {
        uint16_t ch = R16(src + 2 * i);
        if (ch < 128 && (flags & 0x100)) ch = (uint16_t)tolower(ch);
        if (ch < 128 && (flags & 0x200)) ch = (uint16_t)toupper(ch);
        W16(dst + 2 * i, ch);
    }
    RET(6, n);
}
static uint16_t ctype1(int ch) {
    uint16_t t = 0;
    if (isupper(ch)) t |= 0x1;
    if (islower(ch)) t |= 0x2;
    if (isdigit(ch)) t |= 0x4;
    if (isspace(ch)) t |= 0x8;
    if (ispunct(ch)) t |= 0x10;
    if (iscntrl(ch)) t |= 0x20;
    if (ch == ' ' || ch == '\t') t |= 0x40;
    if (isxdigit(ch)) t |= 0x80;
    if (isalpha(ch)) t |= 0x100;
    return t;
}
void I_kernel32_GetStringTypeA(CPU *c) { /* (lcid, type, src, n, out) */
    uint32_t src = ARG(2), out = ARG(4);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    for (int32_t i = 0; i < n; i++) W16(out + 2 * (uint32_t)i, ctype1(R8(src + (uint32_t)i)));
    RET(5, 1);
}
void I_kernel32_GetStringTypeW(CPU *c) { /* (type, src, n, out) */
    uint32_t src = ARG(1), out = ARG(3);
    int32_t n = (int32_t)ARG(2);
    if (n < 0) { n = 0; while (R16(src + 2 * (uint32_t)n)) n++; n++; }
    for (int32_t i = 0; i < n; i++) {
        uint16_t ch = R16(src + 2 * (uint32_t)i);
        W16(out + 2 * (uint32_t)i, ch < 128 ? ctype1(ch) : 0x100);
    }
    RET(4, 1);
}
void I_kernel32_CompareStringA(CPU *c) { /* (lcid, flags, s1, n1, s2, n2) */
    uint32_t flags = ARG(1), s1 = ARG(2), s2 = ARG(4);
    int32_t n1 = (int32_t)ARG(3), n2 = (int32_t)ARG(5);
    if (n1 < 0) n1 = (int32_t)strlen(GSTR(s1));
    if (n2 < 0) n2 = (int32_t)strlen(GSTR(s2));
    int32_t n = n1 < n2 ? n1 : n2, r = 0;
    for (int32_t i = 0; i < n && !r; i++) {
        int a = R8(s1 + (uint32_t)i), b = R8(s2 + (uint32_t)i);
        if (flags & 1) { a = tolower(a); b = tolower(b); }
        r = (a > b) - (a < b);
    }
    if (!r) r = (n1 > n2) - (n1 < n2);
    RET(6, (uint32_t)(r + 2));
}
void I_kernel32_CompareStringW(CPU *c) {
    uint32_t flags = ARG(1), s1 = ARG(2), s2 = ARG(4);
    int32_t n1 = (int32_t)ARG(3), n2 = (int32_t)ARG(5);
    if (n1 < 0) { n1 = 0; while (R16(s1 + 2 * (uint32_t)n1)) n1++; }
    if (n2 < 0) { n2 = 0; while (R16(s2 + 2 * (uint32_t)n2)) n2++; }
    int32_t n = n1 < n2 ? n1 : n2, r = 0;
    for (int32_t i = 0; i < n && !r; i++) {
        int a = R16(s1 + 2 * (uint32_t)i), b = R16(s2 + 2 * (uint32_t)i);
        if ((flags & 1) && a < 128 && b < 128) { a = tolower(a); b = tolower(b); }
        r = (a > b) - (a < b);
    }
    if (!r) r = (n1 > n2) - (n1 < n2);
    RET(6, (uint32_t)(r + 2));
}

/* ------------------------------------------------------------------ heap and virtual memory */
void I_kernel32_HeapCreate(CPU *c) { RET(3, handle_new(H_HEAP, NULL, -1)); }
void I_kernel32_HeapDestroy(CPU *c) { handle_close(ARG(0)); RET(1, 1); }
void I_kernel32_HeapAlloc(CPU *c) { RET(3, heap_alloc(ARG(2), (ARG(1) & 8) != 0)); }
void I_kernel32_HeapFree(CPU *c) { heap_free(ARG(2)); RET(3, 1); }
void I_kernel32_HeapValidate(CPU *c) { RET(3, 1); }
void I_kernel32_HeapReAlloc(CPU *c) {
    uint32_t flags = ARG(1), p = ARG(2), n = ARG(3);
    if (flags & 0x10) { /* HEAP_REALLOC_IN_PLACE_ONLY */
        uint32_t cap = R32(p - 16 + 8);
        if (n + 16 > cap) { RET(4, 0); return; }
        uint32_t old = heap_size(p);
        if ((flags & 8) && n > old) memset(GPTR(p + old), 0, n - old);
        W32(p - 16 + 4, n);
        RET(4, p);
        return;
    }
    RET(4, heap_realloc(p, n, (flags & 8) != 0));
}

/* VirtualAlloc: 64 KB granular regions, always zeroed, kept apart from the heap. */
#define VA_LO 0x30000000u
#define VA_HI 0x50000000u
typedef struct { uint32_t base, size; } VaRegion;
static VaRegion g_va[2048];
static int g_nva;
static uint32_t g_va_top = VA_LO;
static pthread_mutex_t g_va_mu = PTHREAD_MUTEX_INITIALIZER;
void I_kernel32_VirtualAlloc(CPU *c) {
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    pthread_mutex_lock(&g_va_mu);
    if (addr) {
        /* commit inside a region we already reserved: memory is already there */
        for (int i = 0; i < g_nva; i++)
            if (addr >= g_va[i].base && addr + size <= g_va[i].base + g_va[i].size) {
                pthread_mutex_unlock(&g_va_mu);
                RET(4, addr & ~0xFFFu);
                return;
            }
        pthread_mutex_unlock(&g_va_mu);
        rt_log("VirtualAlloc at fixed address %08x size %x type %x refused", addr, size, type);
        RET(4, 0);
        return;
    }
    uint32_t sz = (size + 0xFFFFu) & ~0xFFFFu;
    uint32_t base = 0;
    if (VA_HI - g_va_top < sz) {
        pthread_mutex_unlock(&g_va_mu);
        rt_log("VirtualAlloc out of space (%x)", sz);
        RET(4, 0);
        return;
    }
    base = g_va_top;
    g_va_top += sz;
    if (g_nva < 2048) { g_va[g_nva].base = base; g_va[g_nva].size = sz; g_nva++; }
    pthread_mutex_unlock(&g_va_mu);
    memset(GPTR(base), 0, sz);
    RET(4, base);
}
void I_kernel32_VirtualFree(CPU *c) {
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    if (type & 0x4000) memset(GPTR(addr), 0, size); /* MEM_DECOMMIT: pages read back as zero */
    RET(3, 1);
}

/* ------------------------------------------------------------------ TLS (TEB+0xE10 slots) */
static uint64_t g_tls_used;
void I_kernel32_TlsAlloc(CPU *c) {
    for (uint32_t i = 0; i < 64; i++)
        if (!(g_tls_used & (1ull << i))) {
            g_tls_used |= 1ull << i;
            RET(0, i);
            return;
        }
    RET(0, 0xFFFFFFFFu);
}
void I_kernel32_TlsFree(CPU *c) { g_tls_used &= ~(1ull << (ARG(0) & 63)); RET(1, 1); }
void I_kernel32_TlsGetValue(CPU *c) { set_err(c, 0); RET(1, R32(c->fs_base + 0xE10 + 4 * (ARG(0) & 63))); }
void I_kernel32_TlsSetValue(CPU *c) { W32(c->fs_base + 0xE10 + 4 * (ARG(0) & 63), ARG(1)); RET(2, 1); }

/* ------------------------------------------------------------------ synchronisation */
/* CRITICAL_SECTION: host recursive mutex index kept in the LockSemaphore field (+0x10). */
#define MAX_CS 1024
static pthread_mutex_t g_cs[MAX_CS];
static int g_ncs;
static pthread_mutex_t g_cs_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *cs_get(uint32_t p, bool create) {
    uint32_t id = R32(p + 0x10);
    if (R32(p + 0x14) == 0x43534D47u && id < (uint32_t)g_ncs) return &g_cs[id];
    if (!create) {
        rt_log("critical section %08x used before InitializeCriticalSection", p);
    }
    pthread_mutex_lock(&g_cs_mu);
    if (g_ncs >= MAX_CS) rt_fatal("too many critical sections");
    id = (uint32_t)g_ncs++;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_cs[id], &a);
    pthread_mutex_unlock(&g_cs_mu);
    W32(p + 0x10, id);
    W32(p + 0x14, 0x43534D47u);
    return &g_cs[id];
}
void I_kernel32_InitializeCriticalSection(CPU *c) {
    uint32_t p = ARG(0);
    memset(GPTR(p), 0, 24);
    cs_get(p, true);
    RET(1, 0);
}
void I_kernel32_DeleteCriticalSection(CPU *c) { RET(1, 0); }
void I_kernel32_EnterCriticalSection(CPU *c) { pthread_mutex_lock(cs_get(ARG(0), false)); RET(1, 0); }
void I_kernel32_LeaveCriticalSection(CPU *c) { pthread_mutex_unlock(cs_get(ARG(0), false)); RET(1, 0); }
void I_kernel32_InterlockedIncrement(CPU *c) {
    uint32_t v = __atomic_add_fetch((uint32_t *)GPTR(ARG(0)), 1, __ATOMIC_SEQ_CST);
    RET(1, v);
}
void I_kernel32_InterlockedDecrement(CPU *c) {
    uint32_t v = __atomic_sub_fetch((uint32_t *)GPTR(ARG(0)), 1, __ATOMIC_SEQ_CST);
    RET(1, v);
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool manual, signaled;
} Event;
void I_kernel32_CreateEventA(CPU *c) {
    Event *e = calloc(1, sizeof *e);
    pthread_mutex_init(&e->mu, NULL);
    pthread_cond_init(&e->cv, NULL);
    e->manual = ARG(1) != 0;
    e->signaled = ARG(2) != 0;
    RET(4, handle_new(H_EVENT, e, -1));
}
void I_kernel32_SetEvent(CPU *c) {
    KHandle *h = handle_get(ARG(0), H_EVENT);
    if (!h) { RET(1, 0); return; }
    Event *e = h->p;
    pthread_mutex_lock(&e->mu);
    e->signaled = true;
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->mu);
    RET(1, 1);
}
void I_kernel32_ResetEvent(CPU *c) {
    KHandle *h = handle_get(ARG(0), H_EVENT);
    if (!h) { RET(1, 0); return; }
    Event *e = h->p;
    pthread_mutex_lock(&e->mu);
    e->signaled = false;
    pthread_mutex_unlock(&e->mu);
    RET(1, 1);
}

/* threads */
typedef struct GThread {
    pthread_t pt;
    CPU *cpu;
    uint32_t entry, param, tid;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int suspend;      /* > 0: not started yet */
    bool done;
    uint32_t exit_code;
} GThread;
static uint32_t g_next_tid = 0x100;
static __thread GThread *t_self;

static void *thread_main(void *arg) {
    GThread *t = arg;
    t_self = t;
    cpu_bind(t->cpu);
    pthread_mutex_lock(&t->mu);
    while (t->suspend > 0) pthread_cond_wait(&t->cv, &t->mu);
    pthread_mutex_unlock(&t->mu);
    rt_log("thread %x starts at %08x(%08x)", t->tid, t->entry, t->param);
    uint32_t code = call_guest_ret(t->cpu, t->entry, 1, t->param);
    pthread_mutex_lock(&t->mu);
    t->done = true;
    t->exit_code = code;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mu);
    rt_log("thread %x exits with %u", t->tid, code);
    return NULL;
}

uint32_t thread_start(uint32_t entry, uint32_t param, bool suspended, uint32_t *tid) {
    GThread *t = calloc(1, sizeof *t);
    t->cpu = cpu_new(0x200000);
    t->entry = entry;
    t->param = param;
    t->tid = __atomic_fetch_add(&g_next_tid, 4, __ATOMIC_SEQ_CST);
    t->suspend = suspended ? 1 : 0;
    pthread_mutex_init(&t->mu, NULL);
    pthread_cond_init(&t->cv, NULL);
    t->cpu->thread = t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64u << 20); /* deep recursion in recompiled code needs host stack */
    pthread_create(&t->pt, &a, thread_main, t);
    if (tid) *tid = t->tid;
    return handle_new(H_THREAD, t, -1);
}

void I_kernel32_CreateThread(CPU *c) {
    uint32_t tid;
    uint32_t h = thread_start(ARG(2), ARG(3), (ARG(4) & 4) != 0, &tid);
    if (ARG(5)) W32(ARG(5), tid);
    RET(6, h);
}
void I_kernel32_ResumeThread(CPU *c) {
    KHandle *h = handle_get(ARG(0), H_THREAD);
    if (!h) { RET(1, 0xFFFFFFFFu); return; }
    GThread *t = h->p;
    pthread_mutex_lock(&t->mu);
    int prev = t->suspend;
    if (t->suspend > 0) t->suspend--;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mu);
    RET(1, (uint32_t)prev);
}
void I_kernel32_SuspendThread(CPU *c) {
    KHandle *h = handle_get(ARG(0), H_THREAD);
    rt_log("SuspendThread(%08x) ignored for a running thread", ARG(0));
    RET(1, h ? 0 : 0xFFFFFFFFu);
}
void I_kernel32_TerminateThread(CPU *c) {
    rt_log("TerminateThread(%08x) ignored", ARG(0));
    RET(2, 1);
}
void I_kernel32_ExitThread(CPU *c) {
    GThread *t = t_self;
    if (t) {
        pthread_mutex_lock(&t->mu);
        t->done = true;
        t->exit_code = ARG(0);
        pthread_cond_broadcast(&t->cv);
        pthread_mutex_unlock(&t->mu);
    }
    rt_log("ExitThread(%u)", ARG(0));
    pthread_exit(NULL);
}
void I_kernel32_GetExitCodeThread(CPU *c) {
    KHandle *h = handle_get(ARG(0), H_THREAD);
    if (!h) { RET(2, 0); return; }
    GThread *t = h->p;
    W32(ARG(1), t->done ? t->exit_code : 0x103);
    RET(2, 1);
}
void I_kernel32_GetCurrentThread(CPU *c) { RET(0, 0xFFFFFFFEu); }
uint32_t k32_current_tid(void) { return t_self ? t_self->tid : 0x80; }
void I_kernel32_GetCurrentThreadId(CPU *c) { RET(0, k32_current_tid()); }

static bool wait_until(pthread_cond_t *cv, pthread_mutex_t *mu, uint32_t ms, uint64_t start_us) {
    if (ms == 0xFFFFFFFFu) {
        pthread_cond_wait(cv, mu);
        return true;
    }
    uint64_t end = start_us + (uint64_t)ms * 1000;
    struct timeval now;
    gettimeofday(&now, NULL);
    uint64_t now_us = plat_ticks_us();
    if (now_us >= end) return false;
    uint64_t left = end - now_us;
    struct timespec ts;
    uint64_t abs_us = (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_usec + left;
    ts.tv_sec = (time_t)(abs_us / 1000000);
    ts.tv_nsec = (long)(abs_us % 1000000) * 1000;
    return pthread_cond_timedwait(cv, mu, &ts) == 0;
}
void I_kernel32_WaitForSingleObject(CPU *c) {
    uint32_t hv = ARG(0), ms = ARG(1);
    KHandle *h = handle_get(hv, 0);
    uint64_t t0 = plat_ticks_us();
    if (!h) { RET(2, 0xFFFFFFFFu); return; }
    if (h->type == H_EVENT) {
        Event *e = h->p;
        pthread_mutex_lock(&e->mu);
        while (!e->signaled) {
            if (!wait_until(&e->cv, &e->mu, ms, t0) && !e->signaled) {
                if (ms != 0xFFFFFFFFu && plat_ticks_us() - t0 >= (uint64_t)ms * 1000) break;
            }
        }
        bool ok = e->signaled;
        if (ok && !e->manual) e->signaled = false;
        pthread_mutex_unlock(&e->mu);
        RET(2, ok ? 0 : 0x102);
        return;
    }
    if (h->type == H_THREAD) {
        GThread *t = h->p;
        pthread_mutex_lock(&t->mu);
        while (!t->done) {
            if (!wait_until(&t->cv, &t->mu, ms, t0) && !t->done) {
                if (ms != 0xFFFFFFFFu && plat_ticks_us() - t0 >= (uint64_t)ms * 1000) break;
            }
        }
        bool ok = t->done;
        pthread_mutex_unlock(&t->mu);
        RET(2, ok ? 0 : 0x102);
        return;
    }
    RET(2, 0);
}
void I_kernel32_Sleep(CPU *c) {
    uint32_t ms = ARG(0);
    if (ms == 0) sched_yield();
    else plat_sleep_ms(ms);
    RET(1, 0);
}

void I_kernel32_CloseHandle(CPU *c) {
    KHandle *h = handle_get(ARG(0), 0);
    if (h && h->type == H_FILE && h->fd > 2) close(h->fd);
    if (h && h->type != H_THREAD) handle_close(ARG(0)); /* keep thread records alive */
    RET(1, h ? 1 : 0);
}

/* ------------------------------------------------------------------ time */
static uint64_t unix_us_to_filetime(uint64_t us) { return us * 10 + 116444736000000000ull; }
void I_kernel32_GetSystemTimeAsFileTime(CPU *c) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    W64(ARG(0), unix_us_to_filetime((uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec));
    RET(1, 0);
}
void I_kernel32_FileTimeToLocalFileTime(CPU *c) { W64(ARG(1), R64(ARG(0))); RET(2, 1); }
void I_kernel32_FileTimeToSystemTime(CPU *c) {
    uint64_t ft = R64(ARG(0));
    time_t t = (time_t)((ft - 116444736000000000ull) / 10000000ull);
    struct tm tm;
    gmtime_r(&t, &tm);
    uint32_t p = ARG(1);
    W16(p, (uint16_t)(tm.tm_year + 1900));
    W16(p + 2, (uint16_t)(tm.tm_mon + 1));
    W16(p + 4, (uint16_t)tm.tm_wday);
    W16(p + 6, (uint16_t)tm.tm_mday);
    W16(p + 8, (uint16_t)tm.tm_hour);
    W16(p + 10, (uint16_t)tm.tm_min);
    W16(p + 12, (uint16_t)tm.tm_sec);
    W16(p + 14, (uint16_t)((ft / 10000) % 1000));
    RET(2, 1);
}
void I_kernel32_GetTimeZoneInformation(CPU *c) { memset(GPTR(ARG(0)), 0, 172); RET(1, 0); }
void I_kernel32_QueryPerformanceFrequency(CPU *c) { W64(ARG(0), 1000000); RET(1, 1); }
void I_kernel32_QueryPerformanceCounter(CPU *c) { W64(ARG(0), plat_ticks_us()); RET(1, 1); }
void I_winmm_timeGetTime(CPU *c) { RET(0, (uint32_t)(plat_ticks_us() / 1000)); }

/* ------------------------------------------------------------------ files */
#define STD_IN 0xFFFFFFF6u
static uint32_t g_std[3];
static uint32_t std_handle(int i) {
    if (!g_std[i]) g_std[i] = handle_new(H_FILE, NULL, i);
    return g_std[i];
}
void I_kernel32_GetStdHandle(CPU *c) {
    int32_t n = (int32_t)ARG(0);
    RET(1, n == -10 ? std_handle(0) : n == -11 ? std_handle(1) : n == -12 ? std_handle(2) : 0xFFFFFFFFu);
}
void I_kernel32_SetStdHandle(CPU *c) { RET(2, 1); }
void I_kernel32_GetFileType(CPU *c) {
    KHandle *h = handle_get(ARG(0), H_FILE);
    RET(1, !h ? 0 : h->fd <= 2 ? 2 : 1);
}

void I_kernel32_CreateFileA(CPU *c) {
    const char *name = GSTR(ARG(0));
    uint32_t access = ARG(1), disp = ARG(4);
    char host[1200];
    path_to_host(name, host, sizeof host);
    int fl = 0;
    bool w = (access & 0x40000000u) || (access & 0x10000000u);
    bool r = (access & 0x80000000u) || (access & 0x10000000u) || !w;
    fl = r && w ? O_RDWR : w ? O_WRONLY : O_RDONLY;
    switch (disp) {
    case 1: fl |= O_CREAT | O_EXCL; break;   /* CREATE_NEW */
    case 2: fl |= O_CREAT | O_TRUNC; break;  /* CREATE_ALWAYS */
    case 3: break;                            /* OPEN_EXISTING */
    case 4: fl |= O_CREAT; break;             /* OPEN_ALWAYS */
    case 5: fl |= O_TRUNC; break;             /* TRUNCATE_EXISTING */
    }
    int fd = open(host, fl, 0644);
    if (fd < 0) {
        rt_log("CreateFileA(%s -> %s, %08x, %u) failed: %s", name, host, access, disp, strerror(errno));
        set_err(c, errno_to_win(errno));
        RET(7, 0xFFFFFFFFu);
        return;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        set_err(c, 5);
        RET(7, 0xFFFFFFFFu);
        return;
    }
    rt_log("CreateFileA(%s, %08x, %u) -> fd %d", name, access, disp, fd);
    set_err(c, (disp == 2 || disp == 4) && st.st_size ? 183 : 0);
    RET(7, handle_new(H_FILE, NULL, fd));
}
void I_kernel32_ReadFile(CPU *c) {
    KHandle *h = handle_get(ARG(0), H_FILE);
    uint32_t buf = ARG(1), n = ARG(2), outp = ARG(3);
    if (!h) { set_err(c, 6); RET(5, 0); return; }
    ssize_t got = read(h->fd, GPTR(buf), n);
    if (got < 0) { set_err(c, errno_to_win(errno)); RET(5, 0); return; }
    if (outp) W32(outp, (uint32_t)got);
    RET(5, 1);
}
void I_kernel32_WriteFile(CPU *c) {
    KHandle *h = handle_get(ARG(0), H_FILE);
    uint32_t buf = ARG(1), n = ARG(2), outp = ARG(3);
    if (!h) { set_err(c, 6); RET(5, 0); return; }
    if (h->fd <= 2) {
        rt_log("stdout: %.*s", (int)n, GSTR(buf));
        harvest_stdout(GSTR(buf), n);
        /* MGS_STDOUT_STACK=text: dump the guest stack when the game prints that text (debugging) */
        const char *want = getenv("MGS_STDOUT_STACK");
        if (want && memmem(GPTR(buf), n, want, strlen(want))) {
            char line[1024];
            for (uint32_t row = 0; row < 128; row++) {
                int k = snprintf(line, sizeof line, "  esp+%03x:", row * 32);
                for (uint32_t i = 0; i < 8; i++)
                    k += snprintf(line + k, sizeof line - (size_t)k, " %08x", R32(ESP + row * 32 + i * 4));
                rt_log("%s", line);
            }
        }
        if (outp) W32(outp, n);
        RET(5, 1);
        return;
    }
    ssize_t put = write(h->fd, GPTR(buf), n);
    if (put < 0) { set_err(c, errno_to_win(errno)); RET(5, 0); return; }
    if (outp) W32(outp, (uint32_t)put);
    RET(5, 1);
}
void I_kernel32_SetFilePointer(CPU *c) {
    KHandle *h = handle_get(ARG(0), H_FILE);
    int64_t off = (int32_t)ARG(1);
    uint32_t hi = ARG(2);
    if (hi) off = (int64_t)(((uint64_t)R32(hi) << 32) | ARG(1));
    if (!h) { set_err(c, 6); RET(4, 0xFFFFFFFFu); return; }
    off_t r = lseek(h->fd, off, ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) { set_err(c, 131); RET(4, 0xFFFFFFFFu); return; }
    if (hi) W32(hi, (uint32_t)((uint64_t)r >> 32));
    set_err(c, 0);
    RET(4, (uint32_t)r);
}
void I_kernel32_GetFileSize(CPU *c) {
    KHandle *h = handle_get(ARG(0), H_FILE);
    struct stat st;
    if (!h || fstat(h->fd, &st)) { RET(2, 0xFFFFFFFFu); return; }
    if (ARG(1)) W32(ARG(1), (uint32_t)((uint64_t)st.st_size >> 32));
    RET(2, (uint32_t)st.st_size);
}
void I_kernel32_FlushFileBuffers(CPU *c) { RET(1, 1); }
void I_kernel32_SetEndOfFile(CPU *c) {
    KHandle *h = handle_get(ARG(0), H_FILE);
    if (!h) { RET(1, 0); return; }
    off_t pos = lseek(h->fd, 0, SEEK_CUR);
    RET(1, ftruncate(h->fd, pos) == 0);
}
static void put_filetime(uint32_t p, struct timespec ts) {
    W64(p, unix_us_to_filetime((uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000));
}
void I_kernel32_GetFileInformationByHandle(CPU *c) {
    KHandle *h = handle_get(ARG(0), H_FILE);
    struct stat st;
    if (!h || fstat(h->fd, &st)) { RET(2, 0); return; }
    uint32_t p = ARG(1);
    memset(GPTR(p), 0, 52);
    W32(p, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
    put_filetime(p + 4, st.st_birthtimespec);
    put_filetime(p + 12, st.st_atimespec);
    put_filetime(p + 20, st.st_mtimespec);
    W32(p + 28, 0x12345678);
    W32(p + 32, (uint32_t)((uint64_t)st.st_size >> 32));
    W32(p + 36, (uint32_t)st.st_size);
    W32(p + 40, (uint32_t)st.st_nlink);
    W32(p + 44, (uint32_t)(st.st_ino >> 32));
    W32(p + 48, (uint32_t)st.st_ino);
    RET(2, 1);
}
void I_kernel32_GetFileAttributesA(CPU *c) {
    char host[1200];
    path_to_host(GSTR(ARG(0)), host, sizeof host);
    struct stat st;
    if (stat(host, &st)) {
        set_err(c, 2);
        RET(1, 0xFFFFFFFFu);
        return;
    }
    RET(1, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
}
void I_kernel32_SetFileAttributesA(CPU *c) { RET(2, 1); }
void I_kernel32_DeleteFileA(CPU *c) {
    char host[1200];
    path_to_host(GSTR(ARG(0)), host, sizeof host);
    rt_log("DeleteFileA(%s)", GSTR(ARG(0)));
    if (unlink(host)) { set_err(c, errno_to_win(errno)); RET(1, 0); return; }
    RET(1, 1);
}
void I_kernel32_CreateDirectoryA(CPU *c) {
    char host[1200];
    path_to_host(GSTR(ARG(0)), host, sizeof host);
    if (mkdir(host, 0755)) { set_err(c, errno_to_win(errno)); RET(2, 0); return; }
    RET(2, 1);
}
static char g_cwd_win[260] = "C:\\MGS";
void I_kernel32_GetCurrentDirectoryA(CPU *c) {
    uint32_t n = ARG(0), buf = ARG(1);
    uint32_t len = (uint32_t)strlen(g_cwd_win);
    if (n <= len) { RET(2, len + 1); return; }
    memcpy(GPTR(buf), g_cwd_win, len + 1);
    RET(2, len);
}
void I_kernel32_SetCurrentDirectoryA(CPU *c) {
    const char *d = GSTR(ARG(0));
    rt_log("SetCurrentDirectoryA(%s)", d);
    if (!strcmp(d, ".") || !d[0]) { RET(1, 1); return; }
    if (d[0] && d[1] == ':') snprintf(g_cwd_win, sizeof g_cwd_win, "%s", d);
    else {
        char t[260];
        snprintf(t, sizeof t, "%s\\%s", g_cwd_win, d);
        snprintf(g_cwd_win, sizeof g_cwd_win, "%s", t);
    }
    size_t n = strlen(g_cwd_win);
    if (n > 3 && (g_cwd_win[n - 1] == '\\' || g_cwd_win[n - 1] == '/')) g_cwd_win[n - 1] = 0;
    RET(1, 1);
}

/* FindFirstFile: WIN32_FIND_DATAA is 320 bytes; cFileName at +44 */
typedef struct {
    char dir[1200];
    char pat[260];
    DIR *d;
} Find;
static bool find_next(Find *f, uint32_t out) {
    struct dirent *e;
    while ((e = readdir(f->d))) {
        if (fnmatch(f->pat, e->d_name, FNM_CASEFOLD) != 0) continue;
        char full[1600];
        snprintf(full, sizeof full, "%s/%s", f->dir, e->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        memset(GPTR(out), 0, 320);
        W32(out, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
        put_filetime(out + 4, st.st_birthtimespec);
        put_filetime(out + 12, st.st_atimespec);
        put_filetime(out + 20, st.st_mtimespec);
        W32(out + 28, (uint32_t)((uint64_t)st.st_size >> 32));
        W32(out + 32, (uint32_t)st.st_size);
        snprintf((char *)GPTR(out + 44), 260, "%s", e->d_name);
        return true;
    }
    return false;
}
void I_kernel32_FindFirstFileA(CPU *c) {
    char host[1200];
    const char *win = GSTR(ARG(0));
    path_to_host(win, host, sizeof host);
    Find *f = calloc(1, sizeof *f);
    char *slash = strrchr(host, '/');
    if (slash) {
        *slash = 0;
        snprintf(f->dir, sizeof f->dir, "%s", host[0] ? host : "/");
        snprintf(f->pat, sizeof f->pat, "%s", slash + 1);
    } else {
        snprintf(f->dir, sizeof f->dir, ".");
        snprintf(f->pat, sizeof f->pat, "%s", host);
    }
    if (!strcmp(f->pat, "*.*")) strcpy(f->pat, "*");
    f->d = opendir(f->dir);
    if (!f->d || !find_next(f, ARG(1))) {
        if (f->d) closedir(f->d);
        free(f);
        rt_log("FindFirstFileA(%s): no match", win);
        set_err(c, 2);
        RET(2, 0xFFFFFFFFu);
        return;
    }
    RET(2, handle_new(H_FIND, f, -1));
}
void I_kernel32_FindNextFileA(CPU *c) {
    KHandle *h = handle_get(ARG(0), H_FIND);
    if (!h || !find_next(h->p, ARG(1))) { set_err(c, 18); RET(2, 0); return; }
    RET(2, 1);
}
void I_kernel32_FindClose(CPU *c) {
    KHandle *h = handle_get(ARG(0), H_FIND);
    if (h) {
        Find *f = h->p;
        closedir(f->d);
        free(f);
        handle_close(ARG(0));
    }
    RET(1, 1);
}

/* File mappings: the view is a private copy of the file in guest memory. */
typedef struct { int fd; uint32_t size; } Mapping;
void I_kernel32_CreateFileMappingA(CPU *c) {
    KHandle *f = handle_get(ARG(0), H_FILE);
    if (!f) { RET(6, 0); return; }
    struct stat st;
    fstat(f->fd, &st);
    Mapping *m = calloc(1, sizeof *m);
    m->fd = dup(f->fd);
    m->size = ARG(5) ? ARG(5) : (uint32_t)st.st_size;
    RET(6, handle_new(H_MAPPING, m, -1));
}
void I_kernel32_MapViewOfFile(CPU *c) {
    KHandle *h = handle_get(ARG(0), H_MAPPING);
    if (!h) { RET(5, 0); return; }
    Mapping *m = h->p;
    uint32_t off = ARG(3), n = ARG(4) ? ARG(4) : m->size - off;
    uint32_t p = heap_alloc(n, true);
    ssize_t r = pread(m->fd, GPTR(p), n, off);
    (void)r;
    RET(5, p);
}
void I_kernel32_UnmapViewOfFile(CPU *c) { heap_free(ARG(0)); RET(1, 1); }

/* ------------------------------------------------------------------ drives (no CD check needed) */
void I_kernel32_GetDriveTypeA(CPU *c) {
    const char *r = ARG(0) ? GSTR(ARG(0)) : "C:\\";
    uint32_t t = (toupper((unsigned char)r[0]) == 'D') ? 5 : 3; /* D: is the "CD-ROM" */
    rt_log("GetDriveTypeA(%s) -> %u", r, t);
    RET(1, t);
}
void I_kernel32_GetLogicalDrives(CPU *c) { RET(0, 0x0C); } /* C: and D: */
void I_kernel32_GetVolumeInformationA(CPU *c) {
    const char *r = ARG(0) ? GSTR(ARG(0)) : "C:\\";
    rt_log("GetVolumeInformationA(%s)", r);
    if (ARG(1) && ARG(2)) snprintf((char *)GPTR(ARG(1)), ARG(2), "%s", "MGS");
    if (ARG(3)) W32(ARG(3), 0x1998);
    if (ARG(4)) W32(ARG(4), 255);
    if (ARG(5)) W32(ARG(5), 0);
    if (ARG(6) && ARG(7)) snprintf((char *)GPTR(ARG(6)), ARG(7), "%s", "NTFS");
    RET(8, 1);
}
void I_kernel32_PeekNamedPipe(CPU *c) { RET(6, 0); }

/* ------------------------------------------------------------------ modules */
#define MOD_KERNEL32 0x7FE00000u
#define MOD_USER32 0x7FE10000u
#define MOD_OTHER 0x7FE20000u
typedef struct { const char *dll, *name; void (*fn)(CPU *); } Export;
extern const Export g_extra_exports[];  /* functions the game may look up with GetProcAddress */
void I_kernel32_GetModuleHandleA(CPU *c) {
    uint32_t n = ARG(0);
    if (!n) { RET(1, g_image_base); return; }
    const char *s = GSTR(n);
    rt_log("GetModuleHandleA(%s)", s);
    if (!strncasecmp(s, "kernel32", 8)) { RET(1, MOD_KERNEL32); return; }
    if (!strncasecmp(s, "user32", 6)) { RET(1, MOD_USER32); return; }
    RET(1, 0);
}
static char g_loaded[16][64];
static int g_nloaded;
void I_kernel32_LoadLibraryA(CPU *c) {
    const char *s = GSTR(ARG(0));
    rt_log("LoadLibraryA(%s)", s);
    if (!strncasecmp(s, "kernel32", 8)) { RET(1, MOD_KERNEL32); return; }
    if (!strncasecmp(s, "user32", 6)) { RET(1, MOD_USER32); return; }
    for (int i = 0; i < g_nloaded; i++)
        if (!strcasecmp(g_loaded[i], s)) { RET(1, MOD_OTHER + 0x10000u * (uint32_t)i); return; }
    /* only modules we provide something for */
    for (const Export *e = g_extra_exports; e->dll; e++)
        if (!strncasecmp(e->dll, s, strlen(e->dll)) && g_nloaded < 16) {
            snprintf(g_loaded[g_nloaded], 64, "%s", e->dll);
            RET(1, MOD_OTHER + 0x10000u * (uint32_t)g_nloaded++);
            return;
        }
    set_err(c, 126);
    RET(1, 0);
}
void I_kernel32_FreeLibrary(CPU *c) { RET(1, 1); }
void I_kernel32_GetProcAddress(CPU *c) {
    uint32_t mod = ARG(0), np = ARG(1);
    const char *name = np < 0x10000 ? "#ordinal" : GSTR(np);
    const char *dll = mod == MOD_KERNEL32 ? "kernel32" : mod == MOD_USER32 ? "user32" :
                      (mod >= MOD_OTHER && (mod - MOD_OTHER) / 0x10000u < (uint32_t)g_nloaded) ?
                      g_loaded[(mod - MOD_OTHER) / 0x10000u] : "?";
    for (const Export *e = g_extra_exports; e->dll; e++)
        if (!strncasecmp(e->dll, dll, strlen(e->dll)) && !strcmp(e->name, name)) {
            RET(2, rt_thunk(e->fn, e->name));
            return;
        }
    /* imports of the main image are also valid answers */
    for (uint32_t i = 0; i < g_import_count; i++)
        if (!strcmp(g_imports[i].name, name)) {
            RET(2, rt_thunk(g_imports[i].fn, g_imports[i].name));
            return;
        }
    rt_log("GetProcAddress(%s, %s) -> not found", dll, name);
    set_err(c, 127);
    RET(2, 0);
}
extern char g_exe_win[260];
void I_kernel32_GetModuleFileNameA(CPU *c) {
    uint32_t buf = ARG(1), n = ARG(2);
    uint32_t len = (uint32_t)strlen(g_exe_win);
    if (n == 0) { RET(3, 0); return; }
    if (len >= n) len = n - 1;
    memcpy(GPTR(buf), g_exe_win, len);
    W8(buf + len, 0);
    RET(3, len);
}

const char *k32_cwd(void) { return g_cwd_win; }
