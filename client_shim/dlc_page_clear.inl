// Terrain page decal targets are never cleared (Buccaneer Bay's seabed).
// Each virtual-texture page is built by drawing the terrain decals of its cell into three 4x MSAA
// 128x128 targets (albedo, normal, material), resolving them, and combining them with the page's
// height map. The PC build never clears those targets: every cell of the PC decalsTerrain.dat has
// decals that cover it. The DLC worlds' tables leave most cells empty (Origin of Pain 601 of 1024,
// Riders of Doom 1002), so a page there draws nothing and inherits the previous page's pixels; on
// Buccaneer Bay's seabed that was a sign/skull object page repeated across the ground (RenderDoc:
// 8 pages with different height maps, identical albedo/normal inputs).
// Fix: the device wrapper's resolve method (vtable slot 0x1ae1a74 -> 0x84bb10) is wrapped; after
// it resolves one of those small MSAA targets, the target is cleared to a neutral "no decal"
// value (plain sand albedo, flat normal, the material default the object pages leave), so the
// next page starts clean. Pages with decals draw over the clear as before.
// Rollback: create datapack\trials_revive_no_page_clear.txt.

#include <d3d11.h>

namespace {

const uintptr_t DEVICE_RESOLVE_SLOT = GA(0x1ae1a74);
const uintptr_t DEVICE_RESOLVE = GA(0x84bb10);

typedef void(__fastcall* DeviceResolveFn)(BYTE* device, void* edx, void* dst, DWORD target, void* rect, DWORD sub);
DeviceResolveFn g_device_resolve;

// Clear values for render target slots 0..2 (map1 albedo, map2 normal, map3 material).
const float PAGE_CLEAR[3][4] = {
    {0.38f, 0.34f, 0.26f, 1.0f},
    {0.5f, 0.5f, 1.0f, 1.0f},
    {0.0f, 1.0f, 0.0f, 1.0f},
};

void clear_page_target(BYTE* device, DWORD slot) {
    // Same source the wrapper resolved from: color target slots at +0x1fc.., context at +0x140.
    ID3D11DeviceContext* ctx = *(ID3D11DeviceContext**)(device + 0x140);
    ID3D11Resource* src = *(ID3D11Resource**)(device + 0x1fc + slot * 4);
    if (!ctx || !src) return;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(src->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) || !tex) return;
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    tex->Release();
    if (desc.SampleDesc.Count < 2 || desc.Width != 128 || desc.Height != 128) return;
    ID3D11RenderTargetView* views[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, views, nullptr);
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
        if (!views[i]) continue;
        ID3D11Resource* bound = nullptr;
        views[i]->GetResource(&bound);
        if (bound == src) ctx->ClearRenderTargetView(views[i], PAGE_CLEAR[slot]);
        if (bound) bound->Release();
        views[i]->Release();
    }
}

void __fastcall device_resolve_and_clear(BYTE* device, void* edx, void* dst, DWORD target, void* rect, DWORD sub) {
    g_device_resolve(device, edx, dst, target, rect, sub);
    DWORD slot = target & 7;
    if (slot < 3 && *(BYTE*)(device + 0x1d4)) clear_page_target(device, slot);
}

void start_page_clear_fix() {
    if (game_file_exists("trials_revive_no_page_clear.txt")) {
        shim_log("dlc texture fix: page decal target clear DISABLED by trials_revive_no_page_clear.txt");
        return;
    }
    bool ok = false;
    if (DEVICE_RESOLVE_SLOT && DEVICE_RESOLVE && *(uintptr_t*)DEVICE_RESOLVE_SLOT == DEVICE_RESOLVE) {
        DWORD old;
        if (VirtualProtect((void*)DEVICE_RESOLVE_SLOT, 4, PAGE_READWRITE, &old)) {
            g_device_resolve = (DeviceResolveFn)DEVICE_RESOLVE;
            *(void**)DEVICE_RESOLVE_SLOT = (void*)&device_resolve_and_clear;
            VirtualProtect((void*)DEVICE_RESOLVE_SLOT, 4, old, &old);
            ok = true;
        }
    }
    shim_log(ok ? "dlc texture fix: terrain page decal targets are cleared after each resolve"
                : "dlc texture fix: page clear skipped (unexpected vtable contents)");
}

}  // namespace
