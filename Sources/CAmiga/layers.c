/* layers.library, minimal: windows own their BitMap (intuition.c), so there
 * are no damaged areas to track. Locks have nothing to protect from a single
 * running task, clip regions are only remembered. */
#include "os.h"
#include "gfx.h"

uint32_t LayersBase;

static uint32_t l_nop(M68K *c) { (void)c; return 0; }
static uint32_t l_true(M68K *c) { (void)c; return DOSTRUE; }
static uint32_t l_InstallClipRegion(M68K *c) {
    uint32_t layer = c->a[0], old = rd32(layer + Layer_ClipRegion);
    wr32(layer + Layer_ClipRegion, c->a[1]);
    return old;
}
static uint32_t l_NewLayerInfo(M68K *c) { (void)c; return exec_allocmem(Layer_Info_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR); }
static uint32_t l_DisposeLayerInfo(M68K *c) { if (c->a[0]) exec_freemem(c->a[0], Layer_Info_SIZEOF); return 0; }
static uint32_t l_CreateLayer(M68K *c) {
    (void)c;
    os_log("layers : création de couches par un programme pas encore prise en charge");
    return 0;
}

static const NativeEntry layers_natives[] = {
    { "LockLayer", l_nop }, { "UnlockLayer", l_nop }, { "LockLayers", l_nop }, { "UnlockLayers", l_nop },
    { "LockLayerInfo", l_nop }, { "UnlockLayerInfo", l_nop }, { "InitLayers", l_nop },
    { "InstallClipRegion", l_InstallClipRegion }, { "NewLayerInfo", l_NewLayerInfo },
    { "DisposeLayerInfo", l_DisposeLayerInfo }, { "FattenLayerInfo", l_true }, { "ThinLayerInfo", l_nop },
    { "CreateUpfrontLayer", l_CreateLayer }, { "CreateBehindLayer", l_CreateLayer },
    { "CreateUpfrontHookLayer", l_CreateLayer }, { "CreateBehindHookLayer", l_CreateLayer },
    { "InstallLayerHook", l_nop }, { "InstallLayerInfoHook", l_nop }, { "WhichLayer", l_nop },
    { 0, 0 }
};

void layers_init(void) {
    LayersBase = lib_create("layers.library", "layers", LIB_SIZE, layers_natives, 40, 1, "layers 40.1 (AmigaMac)");
    lib_add(LayersBase);
}
