/* ADVAPI32: a read-only registry holding the values GOG's installer writes. */
#include <strings.h>
#include "rt.h"

#define HKCU 0x80000001u
#define HKLM 0x80000002u
typedef struct { uint32_t root; const char *key, *name; int type; const char *sval; uint32_t dval; } RegVal;
static const RegVal g_reg[] = {
    {HKCU, "Software\\Metal Gear Solid\\1.0", "FIRSTRUN", 4, NULL, 1},
    {HKLM, "Software\\Metal Gear Solid\\1.0", "LangID", 4, NULL, 0},
    {HKLM, "Software\\Metal Gear Solid\\1.0", "Version", 1, "1", 0},
    {HKLM, "Software\\Metal Gear Solid\\1.0", "Launched", 1, "1", 0},
    {HKLM, "Software\\Metal Gear Solid\\1.0", "PID", 1, "53585-442-8000312-36463", 0},
    {HKLM, "Software\\Metal Gear Solid\\1.0", "EXE Path", 1, "C:\\MGS", 0},
    {HKLM, "Software\\Metal Gear Solid\\1.0", "CDPath", 1, "C:\\MGS\\", 0},
    {HKLM, "Software\\Metal Gear Solid\\1.0", "VersionType", 1, "RetailVersion", 0},
    {0, NULL, NULL, 0, NULL, 0},
};
typedef struct { uint32_t root; char key[256]; } OpenKey;
static OpenKey g_keys[64];

static bool key_exists(uint32_t root, const char *k) {
    size_t n = strlen(k);
    for (const RegVal *v = g_reg; v->key; v++)
        if ((v->root == root || root == 0) && !strncasecmp(v->key, k, n) && (v->key[n] == 0 || v->key[n] == '\\'))
            return true;
    return false;
}

void I_advapi32_RegOpenKeyExA(CPU *c) { /* (hkey, subkey, opts, sam, phk) */
    uint32_t root = ARG(0);
    const char *sub = ARG(1) ? GSTR(ARG(1)) : "";
    char full[256];
    if (root >= 0x80000000u) {
        snprintf(full, sizeof full, "%s", sub);
    } else {
        uint32_t i = root / 4 - 1;
        if (i >= 64 || !g_keys[i].root) { RET(5, 6); return; }
        snprintf(full, sizeof full, "%s%s%s", g_keys[i].key, sub[0] ? "\\" : "", sub);
        root = g_keys[i].root;
    }
    if (!key_exists(root, full)) {
        rt_log("RegOpenKeyExA(%08x, %s) -> not found", root, full);
        RET(5, 2);
        return;
    }
    for (int i = 0; i < 64; i++)
        if (!g_keys[i].root) {
            g_keys[i].root = root;
            snprintf(g_keys[i].key, sizeof g_keys[i].key, "%s", full);
            W32(ARG(4), (uint32_t)(i + 1) * 4);
            RET(5, 0);
            return;
        }
    RET(5, 8);
}

void I_advapi32_RegQueryValueExA(CPU *c) { /* (hkey, name, reserved, ptype, data, pcb) */
    uint32_t i = ARG(0) / 4 - 1;
    const char *name = ARG(1) ? GSTR(ARG(1)) : "";
    uint32_t ptype = ARG(3), data = ARG(4), pcb = ARG(5);
    if (i >= 64 || !g_keys[i].root) { RET(6, 6); return; }
    for (const RegVal *v = g_reg; v->key; v++) {
        if (v->root != g_keys[i].root || strcasecmp(v->key, g_keys[i].key) || strcasecmp(v->name, name)) continue;
        uint32_t need = v->type == 4 ? 4 : (uint32_t)strlen(v->sval) + 1;
        if (ptype) W32(ptype, (uint32_t)v->type);
        uint32_t have = pcb ? R32(pcb) : 0;
        if (pcb) W32(pcb, need);
        if (data) {
            if (have < need) { RET(6, 234); return; } /* ERROR_MORE_DATA */
            if (v->type == 4) W32(data, v->dval);
            else memcpy(GPTR(data), v->sval, need);
        }
        rt_log("RegQueryValueExA(%s\\%s)", g_keys[i].key, name);
        RET(6, 0);
        return;
    }
    rt_log("RegQueryValueExA(%s\\%s) -> not found", g_keys[i].key, name);
    RET(6, 2);
}

void I_advapi32_RegCloseKey(CPU *c) {
    uint32_t i = ARG(0) / 4 - 1;
    if (i < 64) g_keys[i].root = 0;
    RET(1, 0);
}
