#pragma once
#include "recomp.h"

typedef struct {
    uint32_t slot;            /* IAT address in the guest image */
    void (*fn)(CPU *c);
    const char *dll, *name;
} ImportSlot;

extern const uint32_t g_fn_count;
extern const uint32_t g_fn_addr[];
extern void (*const g_fn_ptr[])(CPU *);
extern const uint32_t g_import_count;
extern const ImportSlot g_imports[];
extern const uint32_t g_image_base, g_entry;
extern const char g_exe_name[];       /* "mgsi.exe" or "mgsvr.exe": the program this build was made from */
extern const uint32_t g_exe_size;
