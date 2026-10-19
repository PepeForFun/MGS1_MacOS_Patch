/* Startup: map the original mgsi.exe into guest memory, wire imports, run the CRT entry point. */
#include <ctype.h>
#include <signal.h>
#include <errno.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "rt.h"

char g_game_dir[1024];
char g_exe_win[260];
uint32_t g_cmdline_ptr;
static char g_extra_args[512];

/* ------------------------------------------------------------------ paths */
void path_to_host(const char *win, char *out, size_t n) {
    char tmp[1024];
    size_t k = 0;
    const char *s = win;
    extern const char *k32_cwd(void);
    char drive = 0;
    if (isalpha((unsigned char)s[0]) && s[1] == ':') {
        drive = (char)toupper((unsigned char)s[0]);
        s += 2;
    }
    bool absolute = s[0] == '\\' || s[0] == '/';
    if (!absolute && !drive) {
        /* relative to the current directory, which always lives under C:\MGS */
        const char *cwd = k32_cwd();
        if (strncasecmp(cwd, "C:\\MGS", 6) == 0) {
            snprintf(tmp, sizeof tmp, "%s%s/%s", g_game_dir, cwd + 6, s);
        } else {
            snprintf(tmp, sizeof tmp, "%s/%s", g_game_dir, s);
        }
    } else if (drive == 'D') {
        snprintf(tmp, sizeof tmp, "%s%s", g_game_dir, s);   /* the "CD" is the game folder */
    } else if (strncasecmp(s, "\\MGS", 4) == 0 && (s[4] == 0 || s[4] == '\\' || s[4] == '/')) {
        snprintf(tmp, sizeof tmp, "%s%s", g_game_dir, s + 4);
    } else {
        snprintf(tmp, sizeof tmp, "%s/_drive_c%s", g_game_dir, s);
    }
    for (char *p = tmp; *p; p++)
        if (*p == '\\') *p = '/';
    /* collapse "//" and drop "/./" */
    for (size_t i = 0; tmp[i] && k + 1 < n; i++) {
        if (tmp[i] == '/' && k && out[k - 1] == '/') continue;
        if (tmp[i] == '/' && tmp[i + 1] == '.' && (tmp[i + 2] == '/' || tmp[i + 2] == 0)) { i++; continue; }
        out[k++] = tmp[i];
    }
    out[k] = 0;
}

void path_to_win(const char *host, char *out, size_t n) {
    size_t gl = strlen(g_game_dir);
    if (!strncmp(host, g_game_dir, gl)) snprintf(out, n, "C:\\MGS%s", host + gl);
    else snprintf(out, n, "C:\\%s", host);
    for (char *p = out; *p; p++)
        if (*p == '/') *p = '\\';
}

/* ------------------------------------------------------------------ image */
static void load_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal("cannot open %s: %s", path, strerror(errno));
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) rt_fatal("short read on %s", path);
    fclose(f);
    uint32_t pe = *(uint32_t *)(b + 0x3C);
    uint16_t nsec = *(uint16_t *)(b + pe + 6), optsz = *(uint16_t *)(b + pe + 20);
    uint32_t opt = pe + 24;
    uint32_t base = *(uint32_t *)(b + opt + 28), entry = *(uint32_t *)(b + opt + 16);
    uint32_t hdrs = *(uint32_t *)(b + opt + 60);
    if ((uint32_t)sz != g_exe_size || base != g_image_base || base + entry != g_entry)
        rt_fatal("%s is not the executable this build was made from (base %08x entry %08x)", path, base,
                 base + entry);
    memcpy(M + base, b, hdrs);
    for (int i = 0; i < nsec; i++) {
        uint8_t *s = b + opt + optsz + 40 * i;
        uint32_t vsz = *(uint32_t *)(s + 8), va = *(uint32_t *)(s + 12), rsz = *(uint32_t *)(s + 16),
                 rptr = *(uint32_t *)(s + 20);
        uint32_t n = rsz < vsz || !vsz ? rsz : vsz;
        memcpy(M + base + va, b + rptr, n);
        rt_log("section %.8s at %08x size %x (raw %x)", (char *)s, base + va, vsz, n);
    }
    free(b);
    for (uint32_t i = 0; i < g_import_count; i++)
        W32(g_imports[i].slot, rt_thunk(g_imports[i].fn, g_imports[i].name));
    rt_log("image mapped, %u imports wired, %u functions", g_import_count, g_fn_count);
}

static void setup_process(void) {
    memset(M + G_PEB, 0, 0x1000);
    W32(G_PEB + 0x08, g_image_base);  /* ImageBaseAddress */
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\"%s%s", g_exe_win, g_extra_args[0] ? " " : "", g_extra_args);
    g_cmdline_ptr = guest_strdup(cmd);
    rt_log("command line: %s", cmd);
}

static void game_thread(void) {
    CPU *c = cpu_new(0x400000);
    cpu_bind(c);
    rt_log("calling entry point %08x", g_entry);
    PUSH32(G_RET_SENTINEL);
    call_indirect(c, g_entry);
    rt_log("entry point returned eax=%08x", EAX);
    exit((int)EAX);
}

static bool is_game_dir(const char *d) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", d, g_exe_name);
    struct stat st;
    return stat(p, &st) == 0;
}

static void find_game_dir(int argc, char **argv) {
    const char *env = getenv("MGS_GAME_DIR");
    if (env && is_game_dir(env)) { snprintf(g_game_dir, sizeof g_game_dir, "%s", env); return; }
    if (argc > 1 && is_game_dir(argv[1])) { snprintf(g_game_dir, sizeof g_game_dir, "%s", argv[1]); return; }
    /* next to the app bundle, or one folder below it */
    char exe[1024];
    uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n) == 0) {
        char *d = dirname(exe);
        for (int up = 0; up < 4 && d; up++) {
            if (is_game_dir(d)) { snprintf(g_game_dir, sizeof g_game_dir, "%s", d); return; }
            d = dirname(d);
        }
    }
    rt_fatal("game folder not found: set MGS_GAME_DIR to the folder holding %s", g_exe_name);
}

static void on_fault(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    uintptr_t a = (uintptr_t)si->si_addr, base = (uintptr_t)M;
    CPU *c = cpu_current();
    char buf[256];
    if (a >= base && a < base + 0x100000000ull && seh_dispatch(c, (uint32_t)(a - base))) return;
    if (a >= base && a < base + 0x100000000ull)
        snprintf(buf, sizeof buf, "signal %d at guest address %08x", sig, (uint32_t)(a - base));
    else
        snprintf(buf, sizeof buf, "signal %d at host address %p", sig, si->si_addr);
    if (c) rt_fatal("%s (block %08x)", buf, c->eip);
    rt_fatal("%s (no guest thread)", buf);
}

int main(int argc, char **argv) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    find_game_dir(argc, argv);
    const char *a = getenv("MGS_ARGS");
    if (a) snprintf(g_extra_args, sizeof g_extra_args, "%s", a);
    g_trace = getenv("MGS_TRACE") != NULL;
    rt_log("game folder: %s", g_game_dir);
    mem_init();
    char exe[1200];
    snprintf(exe, sizeof exe, "%s/%s", g_game_dir, g_exe_name);
    snprintf(g_exe_win, sizeof g_exe_win, "C:\\MGS\\%s", g_exe_name);
    load_image(exe);
    setup_process();
    plat_init();
    plat_run_main_loop(game_thread);
    return 0;
}
