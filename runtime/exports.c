/* Functions the game may look up by name with GetProcAddress (beyond its own import table). */
#include "rt.h"

typedef struct { const char *dll, *name; void (*fn)(CPU *); } Export;

/* EBUEula.dll!EBUEula(regkey, eula_file, warranty_file, flags), cdecl: the GOG build refuses to
 * start unless it returns non-zero. We ask once with a native dialog (see plat_eula_accepted). */
static void x_EBUEula(CPU *c) {
    char host[1200];
    path_to_host(ARG(1) ? GSTR(ARG(1)) : "EULA.RTF", host, sizeof host);
    bool ok = plat_eula_accepted(host);
    rt_log("EBUEula(%s, %s) -> %s", GSTR(ARG(0)), ARG(1) ? GSTR(ARG(1)) : "-", ok ? "accepted" : "declined");
    EAX = ok ? 1 : 0;
    ESP += 4;
}

const Export g_extra_exports[] = {
    {"EBUEula", "EBUEula", x_EBUEula},
    {NULL, NULL, NULL},
};
