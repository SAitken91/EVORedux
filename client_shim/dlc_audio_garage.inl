// PC creates its sound-object collection before DLC is mounted. Missing sound ids fall back
// to 9bda0fce (Battleground), explaining both the distant gunfire and missing secret music.
// Keep each pack's XML alive in a separate native collection: its project prefix and parameter
// defaults must travel with the returned sound node. Do not replace the base collection.
typedef void* (__fastcall* AudioFindFn)(void*, void*, DWORD);
typedef void* (__fastcall* AudioPathFn)(void*, void*, void*, void*);
typedef void* (__fastcall* AudioCtorFn)(void*, void*);
typedef void (__fastcall* BikeListFn)(void*, void*, void*);
void* g_audio_find_original;
void* g_audio_path_original;
void* g_bike_list_original;
void* g_audio_collections[2];
bool g_audio_hooks_ready;
struct AudioNodeOwner { void* node; void* collection; };
AudioNodeOwner g_audio_nodes[384]; // two shipped collections have 149 + 177 sounds
unsigned g_audio_node_count;
__declspec(thread) const char* g_audio_filename;
uintptr_t ga_audio_default_filename = GA(0x118f0cc);
const char* __cdecl audio_constructor_filename() {
    return g_audio_filename ? g_audio_filename : (const char*)ga_audio_default_filename;
}

// Replace the constructor's push filename without changing the native string/XML ownership.
__declspec(naked) void audio_filename_stub() {
    __asm {
        pushad
        call audio_constructor_filename
        mov [esp + 28], eax
        popad
        // EAX and EDX are overwritten by the native code immediately after this push.
        pop edx
        push eax
        jmp edx
    }
}

void* __fastcall audio_find_with_dlc(void* self, void* edx, DWORD id) {
    void* node = ((AudioFindFn)g_audio_find_original)(self, edx, id);
    if (node) return node;
    EnterCriticalSection(&g_bikes_lock);
    for (int i = 0; i < 2 && !node; ++i) {
        if (!g_audio_collections[i]) continue;
        node = ((AudioFindFn)g_audio_find_original)(g_audio_collections[i], nullptr, id);
        if (!node) continue;
        bool known = false;
        for (unsigned n = 0; n < g_audio_node_count; ++n)
            if (g_audio_nodes[n].node == node) { known = true; break; }
        if (!known && g_audio_node_count < sizeof g_audio_nodes / sizeof g_audio_nodes[0]) {
            g_audio_nodes[g_audio_node_count++] = {node, g_audio_collections[i]};
            if (g_audio_node_count <= 12 || id == 361369217u) {
                char line[120];
                sprintf_s(line, "dlc audio: resolved sound %lu in pack%d (native fallback avoided)", id, i + 1);
                shim_log(line);
            }
        }
    }
    LeaveCriticalSection(&g_bikes_lock);
    return node;
}

void* __fastcall audio_path_with_dlc(void* self, void* edx, void* out, void* node) {
    EnterCriticalSection(&g_bikes_lock);
    for (unsigned n = 0; n < g_audio_node_count; ++n)
        if (g_audio_nodes[n].node == node) { self = g_audio_nodes[n].collection; break; }
    LeaveCriticalSection(&g_bikes_lock);
    return ((AudioPathFn)g_audio_path_original)(self, edx, out, node);
}

void load_dlc_audio_collections(void* game) {
    if (!g_audio_hooks_ready) return;
    BYTE* track = *(BYTE**)((BYTE*)game + 0x30);
    if (!track) return;
    const char* folders[2] = {"OriginofPain", "RidersofDoom"};
    EnterCriticalSection(&g_bikes_lock);
    for (int i = 0; i < 2; ++i) {
        char path[MAX_PATH];
        sprintf_s(path, "DLCContent\\%s\\data\\configs.pak", folders[i]);
        if (g_audio_collections[i] || !game_file_exists(path)) continue;
        void* collection = ((void* (__cdecl*)(size_t))GA(0x436a40))(0x34);
        if (!collection) continue;
        g_audio_filename = i ? "audiocollection2.xml" : "audiocollection1.xml";
        ((AudioCtorFn)GA(0x5e6ba0))(collection, nullptr);
        g_audio_filename = nullptr;
        if (!*(void**)((BYTE*)collection + 0x20)) {
            ((void (__fastcall*)(void*, void*))GA(0x5e6d80))(collection, nullptr);
            ((void (__cdecl*)(void*))ENGINE_FREE)(collection);
            continue;
        }
        g_audio_collections[i] = collection;
        // The startup music scan also ran before these packs were registered. Its loader adds
        // owned strings to the id table, preserving the existing base playlist (ids 0..24).
        ((void (__fastcall*)(void*, void*, DWORD))GA(0x4f9c50))(track + 0x57bc, nullptr, i + 1);
        char line[120];
        sprintf_s(line, "dlc audio: registered pack%d sound definitions and playlist", i + 1);
        shim_log(line);
    }
    LeaveCriticalSection(&g_bikes_lock);
}

void __fastcall bikes_for_all_menus(void* table, void* edx, void* list) {
    load_pending_bikes();
    ((BikeListFn)g_bike_list_original)(table, edx, list);
}

// These entry prefixes contain whole instructions and no relative operands. Copy live bytes
// after code_differs validates the build-specific addresses (including SEH handler pointers).
void* audio_garage_trampoline(uintptr_t at, const BYTE* expected, unsigned size) {
    if (!at || code_differs(at, expected, size)) return nullptr;
    BYTE* code = (BYTE*)VirtualAlloc(nullptr, size + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) return nullptr;
    memcpy(code, (const void*)at, size);
    code[size] = 0xe9;
    *(int*)(code + size + 1) = (int)((at + size) - ((uintptr_t)code + size + 5));
    return code;
}

void start_audio_garage_fix() {
    const BYTE find[5] = {0x8b,0x49,0x20,0x53,0x55};
    const BYTE path[7] = {0x6a,0xff,0x68,0x01,0xff,0xcd,0x00};
    const BYTE list[5] = {0x56,0x8b,0x74,0x24,0x08};
    BYTE filename[5] = {0x68};
    *(DWORD*)(filename + 1) = 0x118f0cc;
    const bool compatible = GA(0x5e6ba0) && GA(0x5e6d80) && GA(0x4f9c50) &&
        GA(0xcdff01) && !code_differs(GA(0x5e6c1d), filename, sizeof filename);
    if (compatible) {
        g_audio_find_original = audio_garage_trampoline(GA(0x5e2b20), find, sizeof find);
        g_audio_path_original = audio_garage_trampoline(GA(0x5e5070), path, sizeof path);
        if (g_audio_find_original && g_audio_path_original &&
            patch_call(GA(0x5e6c1d), filename, sizeof filename, (void*)&audio_filename_stub) &&
            patch_jmp(GA(0x5e2b20), find, sizeof find, (void*)&audio_find_with_dlc) &&
            patch_jmp(GA(0x5e5070), path, sizeof path, (void*)&audio_path_with_dlc)) {
            g_audio_hooks_ready = true;
            shim_log("dlc audio: pack sound lookup and project routing enabled");
        } else shim_log("dlc audio: hooks unavailable; keeping native audio");
    }
    g_bike_list_original = audio_garage_trampoline(GA(0x653890), list, sizeof list);
    if (g_bike_list_original && patch_jmp(GA(0x653890), list, sizeof list, (void*)&bikes_for_all_menus))
        shim_log("dlc garage: pending bikes/items loaded before garage enumeration");
    else shim_log("dlc garage: enumeration hook skipped (unexpected code)");
}
