// Optional RenderDoc loader for GPU captures (diagnostics only).
// The Ubisoft launcher relaunches trialsFMX, so external injection arrives after the device exists.
// With datapack\trials_revive_renderdoc.txt present, the game's D3D11/DXGI import slots are
// redirected; the first call loads RenderDoc's 32-bit renderdoc.dll (which re-patches those IAT
// slots with its own hooks) and then calls through the slot again. No marker = no change.

namespace {

const uintptr_t IAT_CREATE_DXGI_FACTORY = 0xd287c4;
const uintptr_t IAT_D3D11_CREATE_DEVICE_SWAPCHAIN = 0xd287a8;
const uintptr_t IAT_D3D11_CREATE_DEVICE = 0xd287ac;

FARPROC g_orig_dxgi_factory, g_orig_device_swapchain, g_orig_device;

void ensure_renderdoc() {
    static LONG done;
    if (InterlockedExchange(&done, 1)) return;
    HMODULE rd = LoadLibraryW(L"C:\\Program Files\\RenderDoc\\x86\\renderdoc.dll");
    shim_log(rd ? "renderdoc: loaded before D3D11 device creation (F12 captures)" : "renderdoc: load FAILED");
}

// Calls through the IAT slot if RenderDoc replaced it, otherwise the original function.
FARPROC resolve(uintptr_t slot, FARPROC self, FARPROC orig) {
    FARPROC now = *(FARPROC*)slot;
    return now && now != self ? now : orig;
}

HRESULT WINAPI rd_create_dxgi_factory(REFIID riid, void** out);
HRESULT WINAPI rd_create_device_swapchain(void*, int, HMODULE, UINT, const void*, UINT, UINT, const void*, void**, void**, void*, void**);
HRESULT WINAPI rd_create_device(void*, int, HMODULE, UINT, const void*, UINT, UINT, void**, void*, void**);

HRESULT WINAPI rd_create_dxgi_factory(REFIID riid, void** out) {
    ensure_renderdoc();
    auto f = (HRESULT(WINAPI*)(REFIID, void**))resolve(IAT_CREATE_DXGI_FACTORY, (FARPROC)&rd_create_dxgi_factory, g_orig_dxgi_factory);
    return f(riid, out);
}

HRESULT WINAPI rd_create_device_swapchain(void* adapter, int type, HMODULE sw, UINT flags, const void* levels, UINT nlevels,
                                          UINT sdk, const void* desc, void** swapchain, void** device, void* level, void** ctx) {
    ensure_renderdoc();
    auto f = (HRESULT(WINAPI*)(void*, int, HMODULE, UINT, const void*, UINT, UINT, const void*, void**, void**, void*, void**))
        resolve(IAT_D3D11_CREATE_DEVICE_SWAPCHAIN, (FARPROC)&rd_create_device_swapchain, g_orig_device_swapchain);
    return f(adapter, type, sw, flags, levels, nlevels, sdk, desc, swapchain, device, level, ctx);
}

HRESULT WINAPI rd_create_device(void* adapter, int type, HMODULE sw, UINT flags, const void* levels, UINT nlevels,
                                UINT sdk, void** device, void* level, void** ctx) {
    ensure_renderdoc();
    auto f = (HRESULT(WINAPI*)(void*, int, HMODULE, UINT, const void*, UINT, UINT, void**, void*, void**))
        resolve(IAT_D3D11_CREATE_DEVICE, (FARPROC)&rd_create_device, g_orig_device);
    return f(adapter, type, sw, flags, levels, nlevels, sdk, device, level, ctx);
}

bool patch_iat(uintptr_t slot, FARPROC expected, void* target) {
    if (!expected || *(FARPROC*)slot != expected) return false;
    DWORD old;
    if (!VirtualProtect((void*)slot, 4, PAGE_READWRITE, &old)) return false;
    *(void**)slot = target;
    VirtualProtect((void*)slot, 4, old, &old);
    return true;
}

void start_renderdoc_loader() {
    if (!game_file_exists("trials_revive_renderdoc.txt")) return;
    HMODULE d3d = GetModuleHandleA("d3d11.dll"), dxgi = GetModuleHandleA("dxgi.dll");
    g_orig_dxgi_factory = dxgi ? GetProcAddress(dxgi, "CreateDXGIFactory") : nullptr;
    g_orig_device_swapchain = d3d ? GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain") : nullptr;
    g_orig_device = d3d ? GetProcAddress(d3d, "D3D11CreateDevice") : nullptr;
    bool ok = patch_iat(IAT_CREATE_DXGI_FACTORY, g_orig_dxgi_factory, (void*)&rd_create_dxgi_factory) &
              patch_iat(IAT_D3D11_CREATE_DEVICE_SWAPCHAIN, g_orig_device_swapchain, (void*)&rd_create_device_swapchain) &
              patch_iat(IAT_D3D11_CREATE_DEVICE, g_orig_device, (void*)&rd_create_device);
    shim_log(ok ? "renderdoc: import hooks armed by trials_revive_renderdoc.txt"
                : "renderdoc: import hooks NOT armed (unexpected IAT contents)");
}

}  // namespace
