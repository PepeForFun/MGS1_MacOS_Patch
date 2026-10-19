/* COM objects living in guest memory, with vtables of host functions. */
#pragma once
#include "rt.h"

typedef struct { const char *name; void (*fn)(CPU *c); } ComMethod;
typedef struct {
    const char *name;
    int nmethods;
    const ComMethod *methods;   /* NULL entries fall back to a stub that names the method */
    uint32_t vtable;            /* guest address, created on first use */
    /* runs when the last reference goes (surfaces, palettes); objects without it are kept */
    void (*destroy)(uint32_t obj, void *host);
} ComIface;

/* Guest object layout: +0 vtable, +4 refcount, +8 host pointer (low 32 bits index), +12.. free */
uint32_t com_new(ComIface *iface, void *host, uint32_t extra_bytes);
void *com_host(uint32_t obj);
uint32_t com_addref(uint32_t obj);
uint32_t com_release(uint32_t obj);
void com_each(ComIface *iface, void (*fn)(void *host, void *ctx), void *ctx);   /* every live object of iface */   /* the last Release of an object with destroy frees it */
bool guid_eq(uint32_t g, const char *text);   /* text: "6F1D2B61-D5A0-11CF-BFC7-444553540000" */
void guid_put(uint32_t g, const char *text);
void guid_str(uint32_t g, char *out);

#define S_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_FAIL 0x80004005u
#define E_NOTIMPL 0x80004001u
