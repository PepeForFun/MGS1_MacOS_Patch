/* Texture harvest: MGS_HARVEST=@file (one area name per line) loads each area in turn the way the
 * game's own "load" script command does (previous code saved, GV_StrCode of the name, current area
 * set, load request), waits for the loader's "LoadEnd", then writes every texture the game holds to
 * the HD dump (MGS_TEXDUMP). So all of the game's textures can be upscaled without playing it.
 * Progress is appended to MGS_HARVEST_LOG; an area that was started but never finished (it crashed
 * the game) is skipped by the next run, which carries on with the rest. */
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "ddraw.h"

typedef struct {
    const char *exe;
    uint32_t strcode, set_stage;   /* GV_StrCode(name), GM_SetCurrentStage(code, name) */
    uint32_t cur_code, prev_code;  /* 16-bit area codes in the game status */
    uint32_t load_req;             /* GM_LoadRequest */
    uint32_t gm_work;              /* the game-state actor: +0x20 state (1 = running), +0x24 countdown */
    /* what the game's own "back to the title" does first: clear flag bit 0 (a cutscene or codec
     * running keeps the load waiting), stop what runs, clear the second request word */
    uint32_t gm_flags, stop_a, stop_b, req2;
} Addrs;
static const Addrs g_games[] = {
    {"mgsi.exe", 0x40b38e, 0x44eb27, 0x78e7ec, 0x78e874, 0x7227a0, 0x722760, 0x791a0c, 0x40191f, 0x4583bb, 0x7227a4},
    {"mgsvr.exe", 0x40b5c0, 0x44c79b, 0x79d34c, 0x79d3d4, 0x730e78, 0x730e38, 0x7a056c, 0x401946, 0x4567e2, 0x730e7c},
};
static const Addrs *g_a;

static char **g_names;
static int g_count, g_next;
static FILE *g_progress;
static enum { H_OFF, H_WAIT, H_NEXT, H_LOADING } g_state;
static volatile int g_load_ends;
static int g_ends_at_request;
static uint64_t g_t_request, g_t_end, g_t_ready_wait;
static uint32_t g_name_buf;
uint32_t k32_current_tid(void);

static double now_s(void) { return (double)(plat_ticks_us() - g_plat_t0) / 1e6; }

static void harvest_init(void) {
    const char *spec = getenv("MGS_HARVEST");
    if (!spec || spec[0] != '@') return;
    for (size_t i = 0; i < sizeof g_games / sizeof g_games[0]; i++)
        if (!strcmp(g_games[i].exe, g_exe_name)) g_a = &g_games[i];
    if (!g_a) { rt_log("harvest: no addresses for %s", g_exe_name); return; }
    /* areas an earlier run started (finished or crashed in) are not loaded again */
    char done[4096][16];
    int ndone = 0;
    const char *logp = getenv("MGS_HARVEST_LOG");
    FILE *old = logp ? fopen(logp, "r") : NULL;
    char line[256];
    while (old && fgets(line, sizeof line, old) && ndone < 4096) {
        char what[32], name[64];
        if (sscanf(line, "%31s %63s", what, name) == 2 && !strcmp(what, "start")) snprintf(done[ndone++], 16, "%s", name);
    }
    if (old) fclose(old);
    FILE *f = fopen(spec + 1, "r");
    if (!f) { rt_log("harvest: cannot read %s", spec + 1); return; }
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n ")] = 0;
        if (!line[0]) continue;
        bool skip = false;
        for (int i = 0; i < ndone && !skip; i++) skip = !strcmp(done[i], line);
        if (skip) continue;
        g_names = realloc(g_names, sizeof *g_names * (size_t)(g_count + 1));
        g_names[g_count++] = strdup(line);
    }
    fclose(f);
    g_progress = logp ? fopen(logp, "a") : NULL;
    g_name_buf = heap_alloc(64, true);
    g_state = H_WAIT;
    rt_log("harvest: %d areas to load (%d done before)", g_count, ndone);
}

static void progress(const char *what, const char *name) {
    if (!g_progress) return;
    fprintf(g_progress, "%s %s %.1f\n", what, name, now_s());
    fflush(g_progress);
}

void harvest_stdout(const char *text, uint32_t n) {
    if (g_state != H_OFF && memmem(text, n, "LoadEnd", 7)) {
        g_load_ends++;
        g_t_end = plat_ticks_us();
    }
}

/* called on the game thread between frames (PeekMessage) */
void harvest_tick(CPU *c) {
    static bool inited;
    if (!inited) { inited = true; harvest_init(); }
    if (g_state == H_OFF || k32_current_tid() != 0x80) return;
    double start = getenv("MGS_HARVEST_START") ? atof(getenv("MGS_HARVEST_START")) : 25;
    /* MGS_HARVEST_HOLD=s: stay that long in each loaded area before the dump and the next one */
    double hold = getenv("MGS_HARVEST_HOLD") ? atof(getenv("MGS_HARVEST_HOLD")) : 3;
    switch (g_state) {
    case H_WAIT:
        if (now_s() >= start) g_state = H_NEXT;
        break;
    case H_NEXT: {
        /* only while the game runs an area (not loading, not counting down to a load); an area that
         * leaves the game elsewhere (photo mode, menus) ends the run, the next run skips it */
        if (R32(g_a->gm_work + 0x20) != 1 || (int32_t)R32(g_a->gm_work + 0x24) > 0) {
            if (!g_t_ready_wait) g_t_ready_wait = plat_ticks_us();
            if (plat_ticks_us() - g_t_ready_wait > 30000000) {
                rt_log("harvest: the game did not come back to an area in 30 s, ending this run");
                progress("stuck", g_next ? g_names[g_next - 1] : "-");
                exit(4);
            }
            break;
        }
        g_t_ready_wait = 0;
        if (g_next >= g_count) {
            rt_log("harvest: all %d areas loaded", g_count);
            progress("finished", "-");
            exit(0);
        }
        const char *name = g_names[g_next++];
        progress("start", name);
        rt_log("harvest: loading %s (%d/%d), request flags were %x", name, g_next, g_count, R32(g_a->load_req));
        snprintf((char *)GPTR(g_name_buf), 64, "%s", name);
        uint32_t saved[8];
        for (int i = 0; i < 8; i++) saved[i] = c->r[i].d;
        W32(g_a->gm_flags, R32(g_a->gm_flags) & ~1u);
        call_guest(c, g_a->stop_a, 0);
        call_guest(c, g_a->stop_b, 0);
        W32(g_a->req2, 0);
        W16(g_a->prev_code, R16(g_a->cur_code));
        uint32_t code = call_guest_ret(c, g_a->strcode, 1, g_name_buf) & 0xFFFF;
        W16(g_a->cur_code, (uint16_t)code);
        call_guest(c, g_a->set_stage, 2, (uint32_t)(int32_t)(int16_t)code, g_name_buf);
        for (int i = 0; i < 8; i++) c->r[i].d = saved[i];
        W32(g_a->load_req, 1);
        g_ends_at_request = g_load_ends;
        g_t_request = plat_ticks_us();
        g_state = H_LOADING;
        break;
    }
    case H_LOADING: {
        const char *name = g_names[g_next - 1];
        if (g_load_ends > g_ends_at_request && plat_ticks_us() - g_t_end > (uint64_t)(hold * 1e6)) {
            int n = ddraw_dump_textures();
            rt_log("harvest: %s loaded, %d textures held", name, n);
            progress("done", name);
            g_state = H_NEXT;
        } else if (plat_ticks_us() - g_t_request > 60000000) {
            rt_log("harvest: %s did not finish loading in 60 s", name);
            progress("timeout", name);
            g_state = H_NEXT;
        }
        break;
    }
    default:
        break;
    }
}
