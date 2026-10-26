// Trials Evolution revival: code fixes needed to run the Xbox 360 DLC packs on PC.
//
// Applied only when datapack\DLCContent exists, and only to the known Steam build (bytes checked).
//
// 1. Area volumes: the PC area-volume loader (0x527be0, areavolumes<pack>.xml) ignores the file's
//    collectionID and always looks the volumes' collision boxes up in the base game's collection
//    (push 0x2685b856 at 0x528002). DLC worlds keep theirs in their own collection, so the lookup
//    returned null and opening the world crashed. The push is replaced by a call to a stub that
//    pushes the right collection for the pack being loaded (the loader's argument).
//
// 2. Texture reload: opening Paine Island crashed in the texture loader (0x7bc9e0) at 0x7bcfc6.
//    When it reloads into an existing texture resource it reuses the resource's GPU texture object
//    ([res+0x3c], set at 0x7bcce3) without checking it; for some DLC textures that field points at
//    memory that is no longer a texture object (its first bytes read "pack"), and the call through
//    its vtable faulted. The check below only accepts an object whose vtable lies in the game's
//    .rdata with a code pointer in slot 0x44; anything else is logged and treated as "no object",
//    so the loader creates a fresh one, exactly as it does on first load.
//
// 3. Event menu: getGameModeSupportForLB (0x661890), called at 0x6f4455 with the event track's
//    leaderboard id, returns null when no track registered that id (TrackInfoManager skips id 0),
//    and the menu dereferences the result at 0x6f44a7. tools/dlc_leaderboards.py gives every DLC
//    track a usable id; as a safety net a null result falls back to the game-mode support for the
//    track's own mode (what the lookup would have returned) and is logged.
//
// 4. DLC bikes: the pack loader never read pack<N>/bikes.xml, so the DLC bikes (ids 5, 6) were
//    missing and selecting any DLC track that offers them killed the game in getBikeInfos. They
//    are now loaded on the first bike lookup (see section 4).
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <string.h>

void shim_log(const char* msg);
static bool game_file_exists(const char* name) {
    char out[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, out, MAX_PATH);
    while (n && out[n - 1] != '\\') n--;
    if (!n || n + lstrlenA(name) + 1 > MAX_PATH) return false;
    lstrcpyA(out + n, name);
    return GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
}

namespace {

const uintptr_t AREA_COLLECTION_PUSH = 0x528002;
const unsigned char AREA_COLLECTION_ORIG[5] = {0x68, 0x56, 0xb8, 0x85, 0x26};   // push 2685b856h
const DWORD BASE_AREA_COLLECTION = 0x2685b856;
const DWORD PACK1_AREA_COLLECTION = 2919333692u;   // Origin of Pain: areavolumes1.xml collectionID
const DWORD PACK2_AREA_COLLECTION = 65508410u;     // Riders of Doom: areavolumes2.xml collectionID

// Entered via `call` in place of the push: [esp] = return address, and the loader's pack-number
// argument sits at [esp + 4 + 0x1cc] (the frame is balanced there). eax/edx are free at this point.
__declspec(naked) void area_collection_stub() {
    __asm {
        pop edx
        mov eax, dword ptr [esp + 0x1cc]
        cmp eax, 1
        je pack1
        cmp eax, 2
        je pack2
        push BASE_AREA_COLLECTION
        jmp edx
    pack1:
        push PACK1_AREA_COLLECTION
        jmp edx
    pack2:
        push PACK2_AREA_COLLECTION
        jmp edx
    }
}

// ---- 2. texture reload ----------------------------------------------------------------------

const uintptr_t TEXTURE_OBJECT_LOAD = 0x7bcce3;
const unsigned char TEXTURE_OBJECT_ORIG[5] = {0x8b, 0x43, 0x3c, 0x85, 0xc0};   // mov eax,[ebx+3Ch]; test eax,eax
const uintptr_t GAME_TEXT_START = 0x401000, GAME_TEXT_END = 0xd28000;
const uintptr_t GAME_RDATA_START = 0xd28000, GAME_RDATA_END = 0x1d5c000;
volatile LONG g_bad_texture_objects;

// Returns obj when it looks like a live texture object, otherwise 0 (after logging the first few).
extern "C" DWORD __cdecl checked_texture_object(DWORD resource, DWORD obj) {
    DWORD vtable = 0, slot = 0;
    __try {
        vtable = *(const DWORD*)obj;
        if (vtable >= GAME_RDATA_START && vtable + 0x48 <= GAME_RDATA_END) slot = *(const DWORD*)(vtable + 0x44);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (slot >= GAME_TEXT_START && slot < GAME_TEXT_END) return obj;
    if (InterlockedIncrement(&g_bad_texture_objects) <= 20) {
        char line[256];
        int n = sprintf_s(line, "dlc fix: texture resource %08lx had a stale texture object %08lx (first bytes", resource, obj);
        __try {
            const unsigned char* p = (const unsigned char*)obj;
            for (int i = 0; i < 16; i++) n += sprintf_s(line + n, sizeof line - n, " %02x", p[i]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            n += sprintf_s(line + n, sizeof line - n, " unreadable");
        }
        sprintf_s(line + n, sizeof line - n, "); creating a new one");
        shim_log(line);
    }
    return 0;
}

// Replaces `mov eax,[ebx+3Ch]; test eax,eax` (ebx = resource); the following `je` uses our flags.
__declspec(naked) void texture_object_stub() {
    __asm {
        mov eax, dword ptr [ebx + 0x3c]
        test eax, eax
        jz done
        push ecx
        push edx
        push eax
        push ebx
        call checked_texture_object
        add esp, 8
        pop edx
        pop ecx
    done:
        test eax, eax
        ret
    }
}

// ---- 2b. material texture race -------------------------------------------------------------
// The real cause of the texture crash above. When a world's materials are (re)built (0x78b5a0)
// the same material slot is visited in two passes; the second pass frees the texture object of
// the resource the first pass queued for loading. It waits for that object to be "ready", but if
// the loader thread has not created the object yet ([res+0x3c] still 0) it skips the wait, then
// re-reads the field - now set by the loader - and frees the object mid-load. The loader then
// calls through the freed object (0x7bcfc6 / eip 0). Big DLC textures widen the window.
// Neither waiting for the load (froze the game) nor detaching the object (left the atlas with an
// empty texture; the loading screen hung) works. While the resource's load job is still running
// ([res+0x48] set, cleared when the job ends) the stub now leaves the resource untouched and jumps
// past the free and the detach (0x78bac4). The atlas frees the object itself after uploading it
// (0x83f3a0), as it does for every loaded texture.

const uintptr_t MATERIAL_TEXTURE_WAIT = 0x78ba83;
const unsigned char MATERIAL_TEXTURE_ORIG[5] = {0x8b, 0x4e, 0x3c, 0x85, 0xc9};   // mov ecx,[esi+3Ch]; test ecx,ecx
volatile LONG g_material_skips;

// Replaces `mov ecx,[esi+3Ch]; test ecx,ecx` (esi = resource); the following `je` uses our flags.
__declspec(naked) void material_texture_wait_stub() {
    __asm {
        cmp byte ptr [esi + 0x48], 0
        jne loading
        mov ecx, dword ptr [esi + 0x3c]
        test ecx, ecx
        ret
    loading:
        lock inc g_material_skips
        mov dword ptr [esp], 0x78bac4    // past the free and the detach
        ret
    }
}


// ---- 2c. texture atlas drop diagnostics (temporary) ----------------------------------------
// TextureAtlas::addTexture (0x83f950) silently drops an image whose resource belongs to a
// different atlas ([img+0x38] != [atlas+0x30], checked at 0x83fc03). Logs each drop.
const uintptr_t ATLAS_CHECK = 0x83fc03;
const unsigned char ATLAS_CHECK_ORIG[6] = {0x8b, 0x46, 0x38, 0x3b, 0x45, 0x30};
volatile LONG g_atlas_drops;

extern "C" void __cdecl log_atlas_add(const char* name, DWORD img_atlas, DWORD atlas_id, DWORD atlas) {
    char low[96] = {};
    __try {
        for (int i = 0; i < 95 && name && name[i]; i++) low[i] = (char)tolower((unsigned char)name[i]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    bool same = img_atlas == atlas_id;
    if (same && !strstr(low, "fire") && !strstr(low, "smoke") && !strstr(low, "glow")) return;
    if (InterlockedIncrement(&g_atlas_drops) > 400) return;
    char line[400];
    __try {
        sprintf_s(line, "atlas diag: %s '%.300s' atlas obj %08lx id %08lx img %08lx", same ? "added" : "DROPPED", name,
                  atlas, atlas_id, img_atlas);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    shim_log(line);
}

__declspec(naked) void atlas_check_stub() {
    __asm {
        pushfd
        pushad
        push ebp
        push dword ptr [ebp + 0x30]
        push dword ptr [esi + 0x38]
        mov ecx, dword ptr [esp + 0x84]     // the function's [esp+50h] = image key
        push dword ptr [ecx + 8]
        call log_atlas_add
        add esp, 16
        popad
        popfd
        mov eax, dword ptr [esi + 0x38]
        cmp eax, dword ptr [ebp + 0x30]
        ret
    }
}

// ---- 3. event menu leaderboard lookup -------------------------------------------------------

const uintptr_t LB_LOOKUP_CALL = 0x6f4455;
const uintptr_t LB_LOOKUP = 0x661890;           // TrackInfoManager::getGameModeSupportForLB
const DWORD GAME_GLOBAL = 0x1ee3548;            // game object; [+0x4c] = game-mode support table
typedef void*(__fastcall* LbLookupFn)(void* mgr, void* edx, DWORD lb, DWORD flag);   // thiscall, ret 8
volatile LONG g_lb_fallbacks;

void* __fastcall lb_lookup_checked(void* mgr, void* edx, DWORD lb, DWORD flag) {
    void* support = ((LbLookupFn)LB_LOOKUP)(mgr, edx, lb, flag);
    if (support) return support;
    // The caller keeps the event track's TrackInfo at [esp+1Ch] (its esp before pushing our two
    // arguments and the return address).
    const BYTE* caller_esp = (const BYTE*)_AddressOfReturnAddress() + 12;
    __try {
        const BYTE* track = *(const BYTE* const*)(caller_esp + 0x1c);
        int mode = *(const int*)(track + 0x10);
        const BYTE* table = *(const BYTE* const*)(*(const BYTE* const*)GAME_GLOBAL + 0x4c);
        if (mode >= 0 && mode < 16) support = *(void* const*)(table + 4 + mode * 4);
        if (InterlockedIncrement(&g_lb_fallbacks) <= 20) {
            char line[160];
            sprintf_s(line, "dlc fix: no track registered leaderboard %lu (track %u); using mode %d support %p", lb,
                      *(const unsigned short*)track, mode, support);
            shim_log(line);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return support;
}

// ---- 4. DLC bikes -----------------------------------------------------------------------------
// The DLC pack loader (0x653e90) reads pack<N>/editortips, tracks, events and soundproject.xml,
// but bikes are only ever read from pack0/bikes.xml at startup (0x43342f). The packs' own bikes
// (id 5 Origin of Pain, id 6 Riders of Doom) were never registered, so getBikeInfos (0x6f1f40)
// looked them up in the bike table, got null and the game died when a DLC track was selected.
// The pack loader's tracks.xml call (0x65428c) now notes the pack's bikes.xml, and getBikeInfos
// (hooked at its entry) loads the noted files into the same table before it looks bikes up, with
// the loader the startup code uses (it adds to the table; bike configs come from bikes/<config> in
// the mounted pack). The same goes for items.xml (only pack0's is read, 0x4334e5): the garage
// needs the DLC bike's item entry for its model, so pack<N>/items_bikes.xml (the bikes/bikeparts
// part of the pack's items.xml, added by tools/dlc_items.py) is loaded into the item table too.
// Keep this on the first lookup thread. The earlier apparent bike-related
// hash-table hang was subsequently traced to a missing DLC selector during track238 loading;
// see section 5 and hang_analysis/SELECTOR_FINDINGS.md.

const uintptr_t PACK_TRACKS_CALL = 0x65428c;
const uintptr_t PACK_TRACKS_LOAD = 0x660360;    // TrackInfoManager: (pack, "pack<N>/tracks.xml")
const uintptr_t BIKES_LOAD = 0x6532e0;          // bike table: (0, "pack0/bikes.xml"), ret 8
const uintptr_t ITEMS_LOAD = 0x657910;          // item table [game+0x6c]: (0, "pack0/items.xml"), ret 8
const uintptr_t BIKE_GFX = 0x656aa0;            // item table: bike id -> bike model gfx id (ret 4)
const uintptr_t BIKE_LOOKUP = 0x46d670;         // bike table hash lookup: (&id, &slot) -> bucket
const uintptr_t STRING_FROM_CSTR = 0x40c200;    // engine string ctor (thiscall, ret 4)
const uintptr_t ENGINE_FREE = 0x436ab0;         // frees an engine string's heap text (flag bit 0 at +0xc)
const uintptr_t GET_BIKE_INFOS = 0x6f1f40;      // UI binding; first bytes: push -1; push 0CF5267h
const unsigned char GET_BIKE_INFOS_ORIG[7] = {0x6a, 0xff, 0x68, 0x67, 0x52, 0xcf, 0x00};
typedef void(__fastcall* PackTracksFn)(void* mgr, void* edx, DWORD pack, void* path);
typedef DWORD(__fastcall* BikeGfxFn)(void* items, void* edx, DWORD bike);
typedef BYTE*(__fastcall* BikeLookupFn)(void* table, void* edx, DWORD* id, DWORD* slot);
typedef void*(__fastcall* StringFn)(void* str, void* edx, const char* text);

CRITICAL_SECTION g_bikes_lock;
char g_pending_bikes[4][16];   // pack folder names ("pack1") still to load bikes/items for
int g_pending_count;

void* bike_of(void* table, DWORD id) {
    DWORD slot = 0;
    BYTE* bucket = ((BikeLookupFn)BIKE_LOOKUP)(table, nullptr, &id, &slot);
    return bucket ? *(void**)(bucket + 8 + slot * 8) : nullptr;
}

void __fastcall pack_tracks_and_note_bikes(void* mgr, void* edx, DWORD pack, void* path) {
    ((PackTracksFn)PACK_TRACKS_LOAD)(mgr, edx, pack, path);
    __try {
        // path is the engine string the loader built ("pack<N>/tracks.xml"): text at +8, length at +6.
        const char* text = *(const char* const*)((const BYTE*)path + 8);
        unsigned len = *(const unsigned short*)((const BYTE*)path + 6);
        const char* slash = (const char*)memchr(text, '/', len);
        if (!slash || slash - text > 16) return;
        EnterCriticalSection(&g_bikes_lock);
        if (g_pending_count < 4) {
            sprintf_s(g_pending_bikes[g_pending_count++], "%.*s", (int)(slash - text), text);
        }
        LeaveCriticalSection(&g_bikes_lock);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Runs one of the game's table loaders on <pack>/<file> (no-op when the pack has no such file).
typedef void(__fastcall* TableLoadFn)(void* table, void* edx, DWORD zero, void* path);
void load_table_file(uintptr_t loader, void* table, const char* file) {
    alignas(8) BYTE str[0x40] = {};
    ((StringFn)STRING_FROM_CSTR)(str, nullptr, file);
    ((TableLoadFn)loader)(table, nullptr, 0, str);
    if (*(const unsigned short*)(str + 0xc) & 1) ((void(__cdecl*)(void*))ENGINE_FREE)(*(void**)(str + 8));
}

// The pack's bikes (bikes.xml -> bike table) and their garage entries (items_bikes.xml, written by
// tools/dlc_items.py -> item table: bike model + part categories; without it the garage loads a
// null bike model, 0x63d125).
void load_pack_bikes(const char* pack) {
    char line[240], bikes[48], items[48];
    sprintf_s(bikes, "%s/bikes.xml", pack);
    sprintf_s(items, "%s/items_bikes.xml", pack);
    __try {
        const BYTE* game = *(const BYTE* const*)GAME_GLOBAL;
        void* table = *(void**)(game + 0x70);
        void* item_table = *(void**)(game + 0x6c);
        load_table_file(BIKES_LOAD, table, bikes);
        load_table_file(ITEMS_LOAD, item_table, items);
        sprintf_s(line, "dlc fix: loaded %s + %s (bike 5 %s gfx %lu, bike 6 %s gfx %lu; thread %lu)", bikes, items,
                  bike_of(table, 5) ? "registered" : "missing", ((BikeGfxFn)BIKE_GFX)(item_table, nullptr, 5),
                  bike_of(table, 6) ? "registered" : "missing", ((BikeGfxFn)BIKE_GFX)(item_table, nullptr, 6),
                  GetCurrentThreadId());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sprintf_s(line, "dlc fix: loading %s bikes/items faulted (%08lx)", pack, GetExceptionCode());
    }
    shim_log(line);
}

extern "C" void __cdecl load_pending_bikes() {
    if (!g_pending_count) return;
    char files[4][16];
    EnterCriticalSection(&g_bikes_lock);
    int n = g_pending_count;
    memcpy(files, g_pending_bikes, sizeof files);
    g_pending_count = 0;
    LeaveCriticalSection(&g_bikes_lock);
    for (int i = 0; i < n; i++) load_pack_bikes(files[i]);
}

// Jumped to from getBikeInfos' entry (all registers as the caller left them).
__declspec(naked) void get_bike_infos_stub() {
    __asm {
        pushad
        call load_pending_bikes
        popad
        push -1
        push 0xcf5267
        push 0x6f1f47
        ret
    }
}

bool patch_jmp(uintptr_t at, const unsigned char* expected, size_t len, void* target) {
    if (memcmp((void*)at, expected, len) != 0) return false;
    DWORD old;
    if (!VirtualProtect((void*)at, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    unsigned char* p = (unsigned char*)at;
    p[0] = 0xE9;
    *(int*)(p + 1) = (int)((uintptr_t)target - (at + 5));
    for (size_t i = 5; i < len; i++) p[i] = 0x90;
    VirtualProtect((void*)at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, len);
    return true;
}

bool patch_call(uintptr_t at, const unsigned char* expected, size_t len, void* target) {
    if (memcmp((void*)at, expected, len) != 0) return false;
    DWORD old;
    if (!VirtualProtect((void*)at, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    unsigned char* p = (unsigned char*)at;
    p[0] = 0xE8;
    *(int*)(p + 1) = (int)((uintptr_t)target - (at + 5));
    for (size_t i = 5; i < len; i++) p[i] = 0x90;
    VirtualProtect((void*)at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, len);
    return true;
}

// ---- 5. DLC selector data source -----------------------------------------------------------
// Xbox model 46b65320 (three_input_operator_1) is absent from the PC factory. Skipping it
// also skips its TRIl record, so subsequent objects read the wrong bytes and eventually EOF.
// Track238: selector object 3409 occupies bytes 167047..167067; the complete DS list ends
// exactly at 167282 when it is included. Localized DLC help specifies: zero selects value 2,
// any other condition selects value 1. The record is base connections followed by three floats.
const DWORD SELECTOR_MODEL = 0x46b65320;
const DWORD SELECTOR_PROPERTY = 0x7000; // private IDs; never index the PC property tables with these
DWORD g_selector_vtable[16];
typedef void*(__fastcall* SourceCtorFn)(void*, void*, void*);
typedef void(__fastcall* SourceResetFn)(void*, void*);
typedef int(__fastcall* SourceReadFn)(void*, void*, void*, void*, void*, int);
typedef void(__fastcall* ReadFloatFn)(void*, void*, float*);
typedef float(__fastcall* SourceEvalFn)(void*, void*, void*, int);

DWORD object_model(void* object) {
    BYTE* definition = *(BYTE**)((BYTE*)object + 0x44);
    return definition ? *(DWORD*)(*(BYTE**)definition + 0x40) : 0;
}

float __fastcall selector_value(void* self, void*) {
    const float* values = (float*)((BYTE*)self + 0x3c);
    return values[0] != 0.0f ? values[1] : values[2];
}

void __fastcall selector_reset(void* self, void*) {
    ((SourceResetFn)0x62df70)(self, nullptr);
}

int __fastcall selector_read(void* self, void*, void* reader, void* objects, void* selected, int version) {
    int result = ((SourceReadFn)0x6236c0)(self, nullptr, reader, objects, selected, version);
    if (result) return result;
    float* values = (float*)((BYTE*)self + 0x3c);
    for (int i = 0; i < 3; ++i)
        ((ReadFloatFn)(*(DWORD**)reader)[1])(reader, nullptr, values + i);
    char line[180];
    sprintf_s(line, "dlc fix: read selector condition=%g true=%g false=%g", values[0], values[1], values[2]);
    shim_log(line);
    return 0;
}

// Match the engine's 64-bit connection-key hash (owner pointer, property ID).
void* selector_input(void* owner, DWORD property) {
    unsigned __int64 key = (unsigned __int64)(uintptr_t)owner + ((unsigned __int64)property << 32);
    unsigned __int64 hash = (~key) + (key << 18);
    hash = (hash ^ (hash >> 31)) * 21;
    hash = (hash ^ (hash >> 11)) * 65;
    hash ^= hash >> 22;
    BYTE* table = *(BYTE**)(*(BYTE**)GAME_GLOBAL + 0x60) + 0xe0;
    DWORD count = *(DWORD*)(table + 0xc);
    BYTE* buckets = *(BYTE**)(table + 0x10);
    if (!count || !buckets) return nullptr;
    DWORD index = (DWORD)hash & (count - 1);
    for (DWORD probes = 0; probes < count; ++probes) {
        BYTE* bucket = buckets + index * 16;
        if (bucket[12] == 0) return nullptr;
        if (bucket[12] == 2 && *(unsigned __int64*)bucket == key) return *(void**)(bucket + 8);
        index = (index + 1) & (count - 1);
    }
    return nullptr;
}

// The native recursive evaluator passes its visited set and depth to every input. Track our
// own participation too, so a cycle through a selector returns its previous value immediately.
__declspec(thread) void* g_active_selectors[501];
__declspec(thread) int g_active_selector_count;
float __fastcall selector_evaluate(void* self, void*, void* visited, int depth) {
    BYTE* source = (BYTE*)self;
    DWORD tick = *(DWORD*)(*(BYTE**)(*(BYTE**)GAME_GLOBAL + 0x30) + 0x14);
    if (*(BYTE*)0x1eec404 && *(DWORD*)(source + 0x30) == tick) return *(float*)(source + 0x28);
    if (depth > 500 || g_active_selector_count >= 500) {
        *(BYTE*)0x1eec406 = 1;
        return *(float*)(source + 0x28);
    }
    for (int i = 0; i < g_active_selector_count; ++i) {
        if (g_active_selectors[i] == self) {
            *(BYTE*)0x1eec407 = 1;
            return *(float*)(source + 0x28);
        }
    }
    g_active_selectors[g_active_selector_count++] = self;
    float* values = (float*)(source + 0x3c);
    for (DWORD i = 0; i < 3; ++i) {
        void* input = selector_input(*(void**)(source + 4), SELECTOR_PROPERTY + i);
        if (input) {
            float value = ((SourceEvalFn)(*(DWORD**)input)[6])(input, nullptr, visited, depth + 1);
            if (!*(BYTE*)0x1eec405) values[i] = value;
        }
    }
    if (!*(BYTE*)0x1eec405) *(float*)(source + 0x28) = selector_value(self, nullptr);
    *(DWORD*)(source + 0x30) = tick;
    --g_active_selector_count;
    return *(float*)(source + 0x28);
}

void __fastcall selector_copy(void* self, void*, void* other) {
    memcpy((BYTE*)self + 0x3c, (BYTE*)other + 0x3c, 12);
}

extern "C" void* __cdecl create_selector(void* owner) {
    void* memory = ((void*(__cdecl*)(size_t))0x436a40)(0x48);
    if (!memory) return nullptr;
    ((SourceCtorFn)0x62e050)(memory, nullptr, owner);
    *(DWORD**)memory = g_selector_vtable;
    shim_log("dlc fix: created missing selector data source");
    return memory;
}

extern "C" void* __cdecl create_dlc_source(DWORD model, void* owner);   // section 8

// Same factory registration path as native sources, preserving insertion/serialization order.
// Other DLC-only datasource models get section 8's inert placeholders.
__declspec(naked) void selector_factory_stub() {
    __asm {
        cmp dword ptr [eax + 0x40], 0xe661bf45
        je native_noperation
        cmp dword ptr [eax + 0x40], 0x46b65320
        jne other
        push ebp
        call create_selector
        add esp, 4
        push 0x5ffc10
        ret
    native_noperation:
        push 0x5ffbea
        ret
    other:
        push ebp
        push dword ptr [eax + 0x40]
        call create_dlc_source
        add esp, 8
        test eax, eax
        jz unknown
        push 0x5ffc10
        ret
    unknown:
        push 0x5ffdb0
        ret
    }
}

const DWORD PC_PROPERTY_IDS = 467;      // entries in the serialized property id table at 0x1eebec8
const DWORD DLC_PROPERTY_BASE = 0x7100; // private ids for DLC-only properties; nothing reads them

extern "C" DWORD __cdecl selector_property(void* target, DWORD serialized) {
    if (serialized >= 537 && serialized <= 539 && object_model(target) == SELECTOR_MODEL)
        return SELECTOR_PROPERTY + serialized - 537;
    // Connections into other DLC nodes use property ids the PC table doesn't have; reading past
    // the table would give a random native property. They target inert placeholders anyway.
    if (serialized >= PC_PROPERTY_IDS) return DLC_PROPERTY_BASE + serialized;
    return (DWORD)(int)((short*)0x1eebec8)[serialized];
}

// Original instruction translates serialized IDs through the base game's shorter table.
__declspec(naked) void selector_property_stub() {
    __asm {
        push eax
        push edx
        push eax
        push edi
        call selector_property
        add esp, 8
        mov ecx, eax
        pop edx
        pop eax
        push 0x62373f
        ret
    }
}

void start_selector_fix() {
    const BYTE factory[] = {0x81,0x78,0x40,0x45,0xbf,0x61,0xe6,0x0f,0x85,0xc6,0x01,0x00,0x00};
    const BYTE property[] = {0x0f,0xbf,0x0c,0x45,0xc8,0xbe,0xee,0x01};
    if (memcmp((void*)0x5ffbdd,factory,sizeof factory) || memcmp((void*)0x623737,property,sizeof property)) {
        shim_log("dlc fix: selector patch skipped (unexpected code)");
        return;
    }
    memcpy(g_selector_vtable,(void*)0x150a61c,sizeof g_selector_vtable);
    g_selector_vtable[1] = (DWORD)&selector_reset;
    g_selector_vtable[4] = (DWORD)&selector_value;
    g_selector_vtable[6] = (DWORD)&selector_evaluate;
    g_selector_vtable[11] = (DWORD)&selector_copy;
    g_selector_vtable[12] = (DWORD)&selector_read;
    if (patch_jmp(0x623737,property,sizeof property,(void*)&selector_property_stub) &&
        patch_jmp(0x5ffbdd,factory,sizeof factory,(void*)&selector_factory_stub))
        shim_log("dlc fix: selector creation, reading and input evaluation enabled");
}

// Temporary local diagnostic: identify the serialized trigger record that never returns.
void* g_trigger_load_original;
volatile LONG g_trigger_trace_count;
typedef int(__fastcall* TriggerLoadFn)(void*, void*, void*, void*, void*, int);
typedef DWORD(__fastcall* ReaderPositionFn)(void*, void*);
int __fastcall trace_trigger_load(void* self, void*, void* reader, void* objects, void* selected, int version) {
    LONG count = InterlockedIncrement(&g_trigger_trace_count);
    DWORD position = 0;
    char line[240];
    if (count <= 1000) {
        position = ((ReaderPositionFn)(*(DWORD**)reader)[0x4c / 4])(reader, nullptr);
        sprintf_s(line, "trigger trace: begin #%ld this=%p vtable=%08lx reader=%p pos=%lu version=%d",
                  count, self, *(DWORD*)self, reader, position, version);
        shim_log(line);
    }
    int result = ((TriggerLoadFn)g_trigger_load_original)(self, nullptr, reader, objects, selected, version);
    if (count <= 1000) {
        DWORD end = ((ReaderPositionFn)(*(DWORD**)reader)[0x4c / 4])(reader, nullptr);
        sprintf_s(line, "trigger trace: end #%ld pos=%lu consumed=%lu result=%d", count, end, end-position, result);
        shim_log(line);
    }
    return result;
}

void start_trigger_trace() {
    const unsigned char code[5] = {0x83, 0xec, 0x0c, 0x55, 0x56};
    if (memcmp((void*)0x6236c0, code, 5)) return;
    BYTE* trampoline = (BYTE*)VirtualAlloc(nullptr, 10, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) return;
    memcpy(trampoline, code, 5);
    trampoline[5] = 0xe9;
    *(int*)(trampoline + 6) = (int)(0x6236c5 - ((uintptr_t)trampoline + 10));
    g_trigger_load_original = trampoline;
    if (patch_jmp(0x6236c0, code, 5, (void*)&trace_trigger_load))
        shim_log("trigger trace: enabled serialized connection diagnostics");
}


// RET0 has one record per retargetor, in OBJX order. The shipped tracks all use
// enabled/index mode with an initial index of zero. Other modes remain unsupported.
struct DlcRetarget { void* owner; void* targets[5]; };
DlcRetarget g_retargets[128];
unsigned g_retarget_count;
bool g_retarget_hook_enabled;

bool retarget_model(DWORD model) { return model == 0x1d4d8ad7 || model == 0xef1b2f6b; }
extern BYTE* g_nt_objects;                                    // dlc_nt_trace.inl
void load_dlc_portals(const BYTE* data, DWORD size, BYTE* track);  // dlc_portals.inl
void nt_trace_retarget(void* object, float index, void* chosen);

bool load_dlc_retargets(void* stream, BYTE* track, DWORD size) {
    BYTE* objects = track + 0x24a08;
    g_nt_objects = objects;
    DWORD count = *(DWORD*)objects;
    void** owners = *(void***)(objects + 8);
    if (count > 32768 || (!owners && count)) return false;
    g_retarget_count = 0;
    DWORD start = ((DWORD(__fastcall*)(void*, void*))(*(DWORD**)stream)[0x4c / 4])(stream, nullptr);
    typedef void*(__cdecl* RefRead)(void*, void*, int);
    typedef int(__fastcall* RawRead)(void*, void*, void*, int);
    RawRead read = (RawRead)(*(DWORD**)stream)[0x34 / 4];
    for (DWORD k = 0; k < count; ++k) {
        if (!owners[k] || !retarget_model(object_model(owners[k]))) continue;
        if (g_retarget_count == 128) return false;
        BYTE prefix[7];
        if (read(stream, nullptr, prefix, sizeof prefix) != sizeof prefix) return false;
        const BYTE shipped[7] = {1,0,0,0,0,0,0};
        if (memcmp(prefix, shipped, sizeof prefix)) return false;
        DlcRetarget& r = g_retargets[g_retarget_count++];
        r.owner = owners[k];
        for (int j = 0; j < 5; ++j) r.targets[j] = ((RefRead)0x4d06b0)(stream, objects, 0);
    }
    DWORD end = ((DWORD(__fastcall*)(void*, void*))(*(DWORD**)stream)[0x4c / 4])(stream, nullptr);
    if (end - start != size) { g_retarget_count = 0; return false; }
    char line[96];
    sprintf_s(line, "dlc runtime: loaded %u retargetors (%lu bytes)", g_retarget_count, size);
    shim_log(line);
    return true;
}

extern "C" void* __cdecl resolve_dlc_retarget(void* object) {
    void* visited[128];
    unsigned depth = 0;
    while (object && retarget_model(object_model(object))) {
        if (depth == 128) return nullptr;
        for (unsigned j = 0; j < depth; ++j) if (visited[j] == object) return nullptr;
        visited[depth++] = object;
        const DlcRetarget* record = nullptr;
        for (unsigned j = 0; j < g_retarget_count; ++j)
            if (g_retargets[j].owner == object) { record = &g_retargets[j]; break; }
        if (!record) return nullptr;
        void* input = selector_input(object, DLC_PROPERTY_BASE + 552);
        // Data sources are evaluated on demand: refresh the input before reading its value.
        if (input) ((void(__fastcall*)(void*, void*, int))0x621ab0)(input, nullptr, 0);
        float index = input ? *(float*)((BYTE*)input + 0x28) : 0.0f;
        if (!(index > -1e6f && index < 1e6f)) { nt_trace_retarget(object, index, nullptr); return nullptr; } // NaN/huge
        // The serialized initial index is zero-based, but its wired property is
        // one-based. TU3 property setter 824b4d08 clamps to [1,5], subtracts 1,
        // then truncates; 824b4af8 exposes the stored index plus 1. Reading the
        // resolver's internal index alone missed this conversion.
        int slot = input ? (int)((index < 1.0f ? 1.0f : index > 5.0f ? 5.0f : index) - 1.0f) : 0;
        nt_trace_retarget(object, index, record->targets[slot]);
        object = record->targets[slot];
    }
    return object;
}

// Transform's target must be resolved before BOTH group expansion and the actual
// transform. Empty/cyclic targets advance its existing iterator without dereferencing null.
__declspec(naked) void retarget_transform_stub() {
    __asm {
        mov ebx, [ecx + 8]
        pushfd
        pushad
        push ebx
        call resolve_dlc_retarget
        add esp, 4
        mov [esp + 16], eax
        popad
        popfd
        test ebx, ebx
        jz empty_target
        xor esi, esi
        push 0x6178bf
        ret
    empty_target:
        push 0x618af8
        ret
    }
}

void start_retarget_runtime() {
    const BYTE original[5] = {0x8b,0x59,0x08,0x33,0xf6};
    g_retarget_hook_enabled = patch_jmp(0x6178ba, original, sizeof original, (void*)&retarget_transform_stub);
    shim_log(g_retarget_hook_enabled ? "dlc runtime: transform retarget resolution enabled"
                                    : "dlc runtime: retarget hook not installed (unexpected code)");
}

// ---- 6. DLC-only track chunks ----------------------------------------------------------------
// TrackLoader::loadChunks (0x4ea450) fails a track with 0x401 ("Track loading error") on any chunk
// id it doesn't know (0x4eb285). The DLC tracks use three families the PC engine has no code for
// at all (not in its chunk family table at 0xbf9e3c): RET0 (5 tracks, e.g. Magic Marker), EMI1
// (emitters, 4 tracks) and POR3 (portals, 12 tracks). These three are now read past and logged,
// so the tracks load without those features; any other unknown id still fails as before.

const uintptr_t UNKNOWN_CHUNK = 0x4eb285;        // mov eax,[1EE34C4h] (logger) on the failure path
const unsigned char UNKNOWN_CHUNK_ORIG[5] = {0xa1, 0xc4, 0x34, 0xee, 0x01};
typedef int(__fastcall* StreamReadFn)(void* stream, void* edx, void* buf, int len);   // vtable +0x34

// frame = the loader's esp at 0x4eb285: chunk id at +0x10, chunk size at +0x18. Returns 1 if skipped.
extern "C" int __cdecl skip_dlc_chunk(void* stream, const BYTE* frame, BYTE* track) {
    DWORD id = *(const DWORD*)(frame + 0x10);
    DWORD size = *(const DWORD*)(frame + 0x18);
    if (id != 0x30544552 && id != 0x31494d45 && id != 0x33524f50) return 0;   // "RET0" "EMI1" "POR3"
    if (id == 0x30544552 && g_retarget_hook_enabled)
        return load_dlc_retargets(stream, track, size) ? 1 : 0;
    static BYTE scratch[4096];
    StreamReadFn read = *(StreamReadFn*)(*(const BYTE* const*)stream + 0x34);
    for (DWORD left = size; left;) {
        int n = left < sizeof scratch ? (int)left : (int)sizeof scratch;
        if (read(stream, nullptr, scratch, n) != n) return 0;
        left -= n;
    }
    if (id == 0x33524f50 && size <= sizeof scratch) load_dlc_portals(scratch, size, track);
    char line[96];
    sprintf_s(line, "dlc fix: skipped DLC-only track chunk %.4s (%lu bytes)", (const char*)&id, size);
    shim_log(line);
    return 1;
}

__declspec(naked) void unknown_chunk_stub() {
    __asm {
        mov eax, esp
        pushad
        push ebp
        push eax
        push esi
        call skip_dlc_chunk
        add esp, 12
        mov [esp + 28], eax      // pushad's eax slot
        popad
        test eax, eax
        jz failed
        xor eax, eax             // as if the chunk's handler succeeded
        push 0x4eb20b
        ret
    failed:
        mov eax, dword ptr ds:[0x1ee34c4]
        push 0x4eb28a
        ret
    }
}

// ---- 7. TRI chunk length mismatch (stopgap) ---------------------------------------------------
// The TRI handler (0x4b7c70) lets every trigger-system object read its record (0x6017a0) and then
// requires the stream to end exactly at the chunk boundary, else it seeks there and fails the
// track with 0x401. Most DLC tracks' TRIl records don't line up with the PC classes yet (new
// DLC node types and record layouts; see DLC_LOAD_HANDOFF.md), so every such track failed to load.
// Until the records are understood, the mismatch is logged and the parser's own result is
// returned instead (the stream is already at the chunk end): the track loads, but trigger logic
// after the first misread record may be wrong. PC tracks never take this path.

const uintptr_t TRI_MISMATCH = 0x4b7ced;         // mov eax,401h after the seek to the chunk end
const unsigned char TRI_MISMATCH_ORIG[5] = {0xb8, 0x01, 0x04, 0x00, 0x00};
volatile LONG g_tri_mismatches;

extern "C" void __cdecl log_tri_mismatch(DWORD result) {
    LONG n = InterlockedIncrement(&g_tri_mismatches);
    if (n <= 50) {
        char line[160];
        sprintf_s(line, "dlc fix: TRI chunk length mismatch #%ld; parser result=%lu (%s)",
                  n, result, result ? "track rejected" : "partial trigger logic");
        shim_log(line);
    }
}

// Called in place of `mov eax,401h`; ebp still holds 0x6017a0's result.
__declspec(naked) void tri_mismatch_stub() {
    __asm {
        pushad
        push ebp
        call log_tri_mismatch
        add esp, 4
        popad
        mov eax, ebp
        ret
    }
}

// All four trigger-reading passes converge here. Record the failing object's
DWORD g_texture_world;
void portal_register_hit(void* self);
int __fastcall trace_trigger_record(void* self, DWORD slot, void* reader, void* objects, void* selected, int version) {
    DWORD start = ((ReaderPositionFn)(*(DWORD**)reader)[0x4c / 4])(reader, nullptr);
    DWORD vt = *(DWORD*)self;
    char line[240];
    sprintf_s(line, "TRI record begin: slot=%lx vtable=%08lx id=%lu pos=%lu", slot, vt, ((DWORD*)self)[1], start);
    shim_log(line);
    int result = ((TriggerLoadFn)(*(DWORD**)self)[slot / 4])(self, nullptr, reader, objects, selected, version);
    DWORD end = ((ReaderPositionFn)(*(DWORD**)reader)[0x4c / 4])(reader, nullptr);
    // Xbox DLC v47 appends two flags to EventPlayerState. The PC implementation
    // reads the common fields only. Leaving these bytes behind misaligns every
    // subsequent event (Forge failed at EventFire with a bogus input count 257).
    // Keep common PC behavior; the two DLC-only flag semantics remain unmapped.
    if (!result && slot == 0x38 && vt == 0x141d39c && version == 47 &&
        (g_texture_world == 1 || g_texture_world == 2) && end > start) {
        BYTE flags[2] = {};
        StreamReadFn read = *(StreamReadFn*)(*(BYTE**)reader + 0x34);
        if (read(reader, nullptr, flags, 2) != 2) result = 0x401;
        sprintf_s(line, "dlc player-state record: consumed extension %u,%u at %lu", flags[0], flags[1], end);
        shim_log(line);
        end = ((ReaderPositionFn)(*(DWORD**)reader)[0x4c / 4])(reader, nullptr);
    }
    // Likewise the DLC's FilterImpulseCounter records carry one more byte than the PC reads
    // (Forge 419: each counter ends `.. 4b c0 01`; the PC stopped before the 01, so the next
    // FilterImpulseDelay read 3 bytes and failed with 1025 at 212220). Flag semantics unmapped.
    if (!result && slot == 0x30 && vt == 0x1589a1c && version == 47 &&
        (g_texture_world == 1 || g_texture_world == 2) && end > start) {
        BYTE flag = 0;
        StreamReadFn read = *(StreamReadFn*)(*(BYTE**)reader + 0x34);
        if (read(reader, nullptr, &flag, 1) != 1) result = 0x401;
        sprintf_s(line, "dlc impulse-counter record: consumed extension %u at %lu", flag, end);
        shim_log(line);
        end = ((ReaderPositionFn)(*(DWORD**)reader)[0x4c / 4])(reader, nullptr);
    }
    sprintf_s(line, "TRI record end: slot=%lx vtable=%08lx id=%lu pos=%lu consumed=%lu result=%d",
              slot, vt, ((DWORD*)self)[1], end, end-start, result);
    shim_log(line);
    if (!result && slot == 0x28 && vt == 0x12b1f54) portal_register_hit(self);
    return result;
}

__declspec(naked) void trace_pass28() {
    __asm { mov edx, 0x28
            mov eax, offset trace_trigger_record
            ret }
}
__declspec(naked) void trace_pass38() {
    __asm { mov edx, 0x38
            mov eax, offset trace_trigger_record
            ret }
}
__declspec(naked) void trace_pass30() {
    __asm { mov edx, 0x30
            mov eax, offset trace_trigger_record
            ret }
}

// vtable and stream position without suppressing its error or altering records.
extern "C" void __cdecl log_trigger_failure(const BYTE* frame, DWORD result) {
    if (!result) return;
    void* reader = *(void* const*)(frame + 0x20);
    BYTE* node = *(BYTE* const*)(frame + 0x18);
    void* object = node ? *(void**)(node + 8) : nullptr;
    DWORD position = reader ? ((ReaderPositionFn)(*(DWORD**)reader)[0x4c / 4])(reader, nullptr) : 0;
    char line[240];
    sprintf_s(line, "trigger failure: result=%lu object=%p vtable=%08lx readerPos=%lu version=%lu",
              result, object, object ? *(DWORD*)object : 0, position, *(DWORD*)(frame + 0x2c));
    shim_log(line);
}

__declspec(naked) void trigger_failure_stub() {
    __asm {
        pushfd
        pushad
        lea ecx, [esp + 0x24]
        push eax
        push ecx
        call log_trigger_failure
        add esp, 8
        popad
        popfd
        pop edi
        pop esi
        pop ebp
        pop ebx
        add esp, 0xc
        push 0x6018c0
        ret
    }
}

// The PC VT loader selects terrain by world, but hardcodes the base object
// index. The staged indices retain base pages and overlay the selected DLC's
// pages, with unique raw-file codes so overlapping filenames cannot collide.
void portal_forget_track();
extern "C" const char* __cdecl dlc_object_index(DWORD world) {
    portal_forget_track();
    g_texture_world = world;
    const char* original = "vtex/vtex_objects.raw.hdr";
    if (world != 1 && world != 2) return original;
    const char* selected = world == 1 ? "vtex/vtex_objects_dlc1.raw.hdr"
                                    : "vtex/vtex_objects_dlc2.raw.hdr";
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return original;
    while (n && path[n - 1] != '\\') n--;
    if (!n || n + 70 >= MAX_PATH) return original;
    sprintf_s(path + n, MAX_PATH - n, "..\\build\\data\\%s", selected);
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        shim_log("dlc texture fix: merged index absent; using base index");
        return original;
    }
    char line[160];
    sprintf_s(line, "dlc texture fix: world %lu uses %s", world, selected);
    shim_log(line);
    return selected;
}

// Replaces push 01ad cdd4 at 80e16c. Preserve EAX (the just-opened terrain
// stream), all other registers and flags. Replace CALL's return stack slot
// with the chosen string, then resume exactly as the original PUSH did.
__declspec(naked) void dlc_object_index_stub() {
    __asm {
        pushfd
        pushad
        push dword ptr [esp + 0x340]
        call dlc_object_index
        add esp, 4
        mov [esp + 0x24], eax
        popad
        popfd
        push 0x80e171
        ret
    }
}

// ---- 8. DLC-only trigger-system objects ----------------------------------------------------
// The DLC added trigger-system node types the PC factories never create (vector data sources and
// operators, the set_vector event, the switch filter). Their TRIl records stay in the stream and
// every later object reads the wrong bytes: "Track loading error" (event/filter pass) or a hang
// (datasource pass). The factories now create a placeholder for each such model, in the same
// registration order. Switches and the vector runtime now retain and use their records.
// The vector operator still consumes its record without evaluating it.
// Layouts were derived offline by walking every DLC track's TRIl with the game's own readers
// (emulated) and accepting only layouts after which all records parse to the exact chunk end:
// all 82 DLC tracks walk cleanly (hang_analysis/tri_walker). Ops: b u8, h u16, i/f 4 bytes,
// r reference (u16; 0xfffe -> 4 more bytes), N u16 count + count floats, L reference list,
// C datasource connection list (u16 n; n x (r, u16 m, m x u16)), E event/filter base
// (u8, u16 n, n x r), F one float per E input.
struct DlcNode { DWORD model[2]; char kind; const char* name; const char* layout; };
const DlcNode DLC_NODES[] = {
    {{0x17814d74, 0x1d73fc5a}, 'd', "vector", "CbiNbfbff"},
    {{0x3dcd5e53, 0x3fe5bf25}, 'd', "get_vector", "Cir"},
    {{0x5e090f60, 0xea274c4e}, 'd', "vector_envelope", "Cbbrriii"},
    {{0x673985ee, 0x9d0b406c}, 'd', "vector_object_info", "Cbr"},
    {{0x93400eb4, 0xa0ab77c9}, 'd', "vector_operator", "Cbhirrhbfbfbff"},
    {{0xa5c066dd, 0xb7cf0dbc}, 'e', "set_vector", "EiiiihL"},
    {{0xaee65e01, 0x8b49a0d7}, 'f', "switch_filter", "EF"},
};
const int DLC_NODE_COUNT = sizeof DLC_NODES / sizeof DLC_NODES[0];
DWORD g_dlc_vtables[DLC_NODE_COUNT][24];
bool g_vector_runtime_enabled = true; // offline harness; installed builds require the opt-in marker
volatile LONG g_dlc_created[DLC_NODE_COUNT];
typedef void(__fastcall* StreamU16Fn)(void* stream, void* edx, WORD* out);       // vtable +0x18
typedef void(__fastcall* StreamU8Fn)(void* stream, void* edx, BYTE* out);        // vtable +0x20
typedef void*(__fastcall* TriggerCtorFn)(void* self, void* edx, void* owner);

const DlcNode* dlc_node_of(DWORD model) {
    for (int i = 0; i < DLC_NODE_COUNT; ++i)
        if (DLC_NODES[i].model[0] == model || DLC_NODES[i].model[1] == model) return &DLC_NODES[i];
    return nullptr;
}

int dlc_node_index(void* self) {
    for (int i = 0; i < DLC_NODE_COUNT; ++i)
        if (*(DWORD**)self == g_dlc_vtables[i]) return i;
    return -1;
}

struct DlcReader {
    void* stream;
    bool ok = true;
    WORD u16() { WORD v = 0; ((StreamU16Fn)(*(DWORD**)stream)[0x18 / 4])(stream, nullptr, &v); return v; }
    BYTE u8() { BYTE v = 0; ((StreamU8Fn)(*(DWORD**)stream)[0x20 / 4])(stream, nullptr, &v); return v; }
    void skip(DWORD n) {
        BYTE scratch[64];
        StreamReadFn read = *(StreamReadFn*)(*(const BYTE* const*)stream + 0x34);
        while (n && ok) {
            int k = n < sizeof scratch ? (int)n : (int)sizeof scratch;
            if (read(stream, nullptr, scratch, k) != k) ok = false;
            n -= k;
        }
    }
    void ref() { if (u16() == 0xfffe) skip(4); }
};

// Consume one DLC record; 0 on success, 0x401 on a short stream or an implausible count.
int dlc_consume(void* stream, const char* layout) {
    DlcReader r{stream};
    WORD inputs = 0;
    for (const char* op = layout; *op && r.ok; ++op) {
        switch (*op) {
        case 'b': r.skip(1); break;
        case 'h': r.skip(2); break;
        case 'i': case 'f': r.skip(4); break;
        case 'r': r.ref(); break;
        case 'N': { WORD n = r.u16(); if (n > 4096) return 0x401; r.skip(4u * n); break; }
        case 'L': { WORD n = r.u16(); if (n > 1024) return 0x401; while (n-- && r.ok) r.ref(); break; }
        case 'C': {
            WORD n = r.u16();
            if (n > 1024) return 0x401;
            while (n-- && r.ok) { r.ref(); WORD m = r.u16(); if (m > 1024) return 0x401; r.skip(2u * m); }
            break;
        }
        case 'E': { r.u8(); inputs = r.u16(); if (inputs > 1024) return 0x401;
                    for (WORD k = 0; k < inputs && r.ok; ++k) r.ref(); break; }
        case 'F': r.skip(4u * inputs); break;
        }
    }
    return r.ok ? 0 : 0x401;
}

// The engine's open-addressed id table (used by the native event/filter readers to decide
// whether this object's record is present at all).
bool table_has(const BYTE* table, DWORD key) {
    DWORD h = key + ~(key << 15);
    h = ((h >> 10) ^ h) * 9;
    h ^= h >> 6;
    h += ~(h << 11);
    h ^= h >> 16;
    DWORD mask = *(const DWORD*)(table + 0xc) - 1;
    const BYTE* buckets = *(const BYTE* const*)(table + 0x10);
    for (DWORD i = h & mask, probes = 0; probes <= mask; i = (i + 1) & mask, ++probes) {
        const BYTE* b = buckets + i * 12;
        if (b[8] == 2 && *(const DWORD*)b == key) return true;
        if (b[8] == 0) return false;
    }
    return false;
}

#include "dlc_vectors.inl"
#include "dlc_particle_diag.inl"
#include "dlc_nt_trace.inl"
#include "dlc_portals.inl"
#include "dlc_shader_constants.inl"
#include "dlc_renderdoc.inl"
#include "dlc_particle_atlas.inl"
#include "dlc_counter_gate.inl"

int __fastcall dlc_node_read(void* self, void*, void* stream, void* objects, void* selected, int version) {
    int i = dlc_node_index(self);
    if (i < 0) return 0x401;
    const DlcNode& node = DLC_NODES[i];
    // Events and filters only have a record when selected, like the native readers (0x61cbb0).
    if (node.kind != 'd' && selected && !table_has((const BYTE*)selected, ((DWORD*)self)[1])) return 0;
    if (VectorState* state = vector_state(self)) return vector_read(state, stream, objects, selected, version);
    if (node.kind == 'f') {
        // The native splitter base owns the five outgoing impulse references. Using its
        // reader also registers reverse links and lets native destruction unlink them.
        int result = ((SourceReadFn)0x634040)(self, nullptr, stream, objects, selected, version);
        if (result) return result;
        float* values = (float*)((BYTE*)self + 0x78);
        for (int k = 0; k < 5; ++k)
            ((ReadFloatFn)(*(DWORD**)stream)[1])(stream, nullptr, values + k);
        return 0;
    }
    int result = dlc_consume(stream, node.layout);
    if (result) {
        char line[120];
        sprintf_s(line, "dlc node: %s record could not be read (version %d)", node.name, version);
        shim_log(line);
    }
    return result;
}

// Four case outputs followed by the default output. Serialized floats are the
// comparison value followed by four case values (confirmed by Neverending Trials'
// chained 0..3, 4..7, 8..9 switches). Native impulse dispatch consumes this bit mask.
void nt_trace_switch(void* owner, float comparison, bool wired, unsigned char mask);  // dlc_nt_trace.inl
unsigned char __fastcall dlc_switch_activate(void* self, void*) {
    ((unsigned char(__fastcall*)(void*, void*))0x633f50)(self, nullptr);
    if (!*((BYTE*)self + 0x70)) return 0x1f; // native disabled-filter bypass
    const float* values = (const float*)((BYTE*)self + 0x78);
    void* owner = ((void**)self)[1];
    void* input = selector_input(owner, DLC_PROPERTY_BASE + 508);
    // The 360 switch refreshes its comparison from the input before matching (TU3 0x825eccf0);
    // PC data sources are evaluated on demand, so evaluate the input first.
    if (input) ((void(__fastcall*)(void*, void*, int))0x621ab0)(input, nullptr, 0);
    float comparison = input ? *(float*)((BYTE*)input + 0x28) : values[0];
    unsigned char mask = 0;                      // every matching case fires, as on the 360
    for (DWORD k = 0; k < 4; ++k)
        if (comparison == values[k + 1]) mask |= (unsigned char)(1u << k);
    if (!mask) mask = 0x10;
    nt_trace_switch(owner, comparison, input != nullptr, mask);
    return mask;
}

void __fastcall dlc_switch_copy(void* self, void*, void* other) {
    ((void(__fastcall*)(void*, void*, void*))0x633f20)(self, nullptr, other);
    if (other) memcpy((BYTE*)self + 0x78, (BYTE*)other + 0x78, 5 * sizeof(float));
}

// Placeholder construction: the closest native class (same allocation size and constructor),
// then the cloned vtable whose read slot consumes the DLC layout.
struct NodeBase { DWORD size; uintptr_t ctor; uintptr_t vtable; int slots; int read_slot; };
const NodeBase NODE_BASE[3] = {
    {0x48, 0x62e050, 0x150a61c, 16, 0x30 / 4},   // 'd' DataSourceOperation
    {0xb4, 0x61de50, 0x146618c, 18, 0x38 / 4},   // 'e' EventState (no targets -> no effect)
    {0x8c, 0x633e90, 0x15adf38, 13, 0x30 / 4},   // 'f' FilterImpulseSplitter (no outputs)
};

const NodeBase& node_base(char kind) { return NODE_BASE[kind == 'd' ? 0 : kind == 'e' ? 1 : 2]; }
const NodeBase VECTOR_CURVE_BASE = {0x60, 0x625690, 0x14af43c, 16, 12};

void init_dlc_nodes() {
    memcpy(g_vector_adapter_vtable, (void*)0x150a61c, sizeof g_vector_adapter_vtable);
    g_vector_adapter_vtable[4] = (DWORD)&vector_adapter_value;
    g_vector_adapter_vtable[6] = (DWORD)&vector_adapter_eval;
    for (int i = 0; i < DLC_NODE_COUNT; ++i) {
        const NodeBase& b = i == 2 && g_vector_runtime_enabled ? VECTOR_CURVE_BASE : node_base(DLC_NODES[i].kind);
        memcpy(g_dlc_vtables[i], (void*)b.vtable, b.slots * 4);
        g_dlc_vtables[i][b.read_slot] = (DWORD)&dlc_node_read;
        if (g_vector_runtime_enabled && (i == 0 || i == 1 || i == 2 || i == 3 || i == 5)) {
            g_dlc_vtables[i][0] = (DWORD)&vector_destroy;
            if (i == 5) g_dlc_vtables[i][3] = (DWORD)&vector_set;
            else {
                if (i != 2) g_dlc_vtables[i][3] = (DWORD)&vector_reset;
                g_dlc_vtables[i][4] = (DWORD)&vector_value;
                g_dlc_vtables[i][6] = (DWORD)&vector_eval;
            }
        }
        if (DLC_NODES[i].kind == 'f') {
            g_dlc_vtables[i][3] = (DWORD)&dlc_switch_activate;
            g_dlc_vtables[i][11] = (DWORD)&dlc_switch_copy;
        }
    }
}

extern "C" void* __cdecl create_dlc_node(DWORD model, void* owner, char kind) {
    const DlcNode* node = dlc_node_of(model);
    if (!node || node->kind != kind) return nullptr;
    int i = (int)(node - DLC_NODES);
    const NodeBase& b = i == 2 && g_vector_runtime_enabled ? VECTOR_CURVE_BASE : node_base(kind);
    void* memory = ((void*(__cdecl*)(size_t))0x436a40)(b.size);
    if (!memory) return nullptr;
    memset(memory, 0, b.size);
    ((TriggerCtorFn)b.ctor)(memory, nullptr, owner);
    *(DWORD**)memory = g_dlc_vtables[i];
    if (g_vector_runtime_enabled && (i == 0 || i == 1 || i == 2 || i == 3 || i == 5)) {
        VectorState* s = new VectorState();
        s->self = memory; s->kind = i; s->next = g_vectors;
        g_vectors = s;
    }
    if (InterlockedIncrement(&g_dlc_created[i]) == 1) {
        char line[96];
        sprintf_s(line, "dlc node: creating %s", node->name);
        shim_log(line);
    }
    return memory;
}

extern "C" void* __cdecl create_dlc_source(DWORD model, void* owner) { return create_dlc_node(model, owner, 'd'); }

// Event factory (0x5feab0): eax = model, ebp = owner. Replaces `cmp eax,0A9B7EB2Fh; jne 5FF600h`.
__declspec(naked) void dlc_event_factory_stub() {
    __asm {
        cmp eax, 0xa9b7eb2f
        jne not_game
        push 0x5ff3ef
        ret
    not_game:
        push 'e'
        push ebp
        push eax
        call create_dlc_node
        add esp, 12
        test eax, eax
        jz unknown
        push 0x5ff418
        ret
    unknown:
        push 0x5ff600
        ret
    }
}

// Filter factory (0x5ff620): edx = definition, ebp = owner.
// Replaces `cmp dword ptr [edx+40h],84671807h; jne 5FF9A0h`.
__declspec(naked) void dlc_filter_factory_stub() {
    __asm {
        cmp dword ptr [edx + 0x40], 0x84671807
        jne not_splitter
        push 0x5ff79a
        ret
    not_splitter:
        push 'f'
        push ebp
        push dword ptr [edx + 0x40]
        call create_dlc_node
        add esp, 12
        test eax, eax
        jz unknown
        push 0x5ff7c0
        ret
    unknown:
        push 0x5ff9a0
        ret
    }
}

const unsigned char EVENT_FACTORY_TAIL[11] = {0x3d, 0x2f, 0xeb, 0xb7, 0xa9, 0x0f, 0x85, 0x11, 0x02, 0x00, 0x00};
const unsigned char FILTER_FACTORY_TAIL[13] = {0x81, 0x7a, 0x40, 0x07, 0x18, 0x67, 0x84, 0x0f, 0x85, 0x06, 0x02, 0x00, 0x00};

void start_dlc_nodes() {
    g_vector_runtime_enabled = game_file_exists("trials_revive_vector_runtime.txt");
    init_dlc_nodes();
    shim_log(patch_jmp(0x5ff3e4, EVENT_FACTORY_TAIL, sizeof EVENT_FACTORY_TAIL, (void*)&dlc_event_factory_stub) &&
             patch_jmp(0x5ff78d, FILTER_FACTORY_TAIL, sizeof FILTER_FACTORY_TAIL, (void*)&dlc_filter_factory_stub)
                 ? (g_vector_runtime_enabled ? "dlc fix: switch routing and staged vector runtime enabled"
                                            : "dlc fix: switch routing enabled; staged vector runtime disabled")
                 : "dlc fix: DLC node placeholders skipped (unexpected code)");
}

bool dlc_present() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    while (n && path[n - 1] != '\\') n--;
    if (!n || n + 12 > MAX_PATH) return false;
    lstrcpyA(path + n, "DLCContent");
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

void start_dlc_fixes() {
    if ((uintptr_t)GetModuleHandleA(nullptr) != 0x400000 || !dlc_present()) return;
    start_trigger_trace();
    const unsigned char pass28[5] = {0x8b,0x11,0x8b,0x42,0x28};
    const unsigned char pass38[5] = {0x8b,0x11,0x8b,0x42,0x38};
    const unsigned char pass30[5] = {0x8b,0x11,0x8b,0x42,0x30};
    if (patch_call(0x6017d7, pass28, 5, (void*)&trace_pass28) &&
        patch_call(0x601818, pass38, 5, (void*)&trace_pass38) &&
        patch_call(0x601859, pass30, 5, (void*)&trace_pass30) &&
        patch_call(0x601899, pass30, 5, (void*)&trace_pass30))
        shim_log("TRI all-pass record diagnostics enabled");
    const unsigned char trigger_return[7] = {0x5f, 0x5e, 0x5d, 0x5b, 0x83, 0xc4, 0x0c};
    shim_log(patch_jmp(0x6018b9, trigger_return, 7, (void*)&trigger_failure_stub)
                 ? "trigger failure diagnostics enabled"
                 : "trigger failure diagnostics skipped (unexpected code)");
    start_selector_fix();
    start_shader_constants_fix();
    start_renderdoc_loader();
    start_particle_atlas_fix();
    start_counter_gate();
    const unsigned char object_index_push[5] = {0x68, 0xd4, 0xcd, 0xad, 0x01};
    if (game_file_exists("trials_revive_no_object_index.txt"))
        shim_log("dlc texture fix: per-world object index DISABLED by trials_revive_no_object_index.txt (test)");
    else
    shim_log(patch_call(0x80e16c, object_index_push, 5, (void*)&dlc_object_index_stub)
                 ? "dlc texture fix: per-world object index enabled"
                 : "dlc texture fix: index hook skipped (unexpected code)");
    shim_log(patch_call(AREA_COLLECTION_PUSH, AREA_COLLECTION_ORIG, 5, (void*)&area_collection_stub)
                 ? "dlc fix: area volumes use the pack's own collection"
                 : "dlc fix: area volume patch skipped (unexpected code)");
    shim_log(patch_call(TEXTURE_OBJECT_LOAD, TEXTURE_OBJECT_ORIG, 5, (void*)&texture_object_stub)
                 ? "dlc fix: texture reload checks the reused texture object"
                 : "dlc fix: texture reload patch skipped (unexpected code)");
    shim_log(patch_call(MATERIAL_TEXTURE_WAIT, MATERIAL_TEXTURE_ORIG, 5, (void*)&material_texture_wait_stub)
                 ? "dlc fix: material textures still loading are left alone, not freed"
                 : "dlc fix: material texture patch skipped (unexpected code)");
    shim_log(patch_call(ATLAS_CHECK, ATLAS_CHECK_ORIG, 6, (void*)&atlas_check_stub)
                 ? "atlas diag: texture atlas drops are logged"
                 : "atlas diag: skipped (unexpected code)");
    unsigned char lb_call[5] = {0xE8};
    *(int*)(lb_call + 1) = (int)(LB_LOOKUP - (LB_LOOKUP_CALL + 5));
    shim_log(patch_call(LB_LOOKUP_CALL, lb_call, 5, (void*)&lb_lookup_checked)
                 ? "dlc fix: event menu leaderboard lookup has a fallback"
                 : "dlc fix: event menu patch skipped (unexpected code)");
    unsigned char tracks_call[5] = {0xE8};
    *(int*)(tracks_call + 1) = (int)(PACK_TRACKS_LOAD - (PACK_TRACKS_CALL + 5));
    InitializeCriticalSection(&g_bikes_lock);
    if (memcmp((void*)GET_BIKE_INFOS, GET_BIKE_INFOS_ORIG, sizeof GET_BIKE_INFOS_ORIG) == 0 &&
        patch_call(PACK_TRACKS_CALL, tracks_call, 5, (void*)&pack_tracks_and_note_bikes) &&
        patch_jmp(GET_BIKE_INFOS, GET_BIKE_INFOS_ORIG, sizeof GET_BIKE_INFOS_ORIG, (void*)&get_bike_infos_stub))
        shim_log("dlc fix: DLC packs' bikes.xml load on first bike lookup");
    else
        shim_log("dlc fix: DLC bikes patch skipped (unexpected code)");
    shim_log(patch_jmp(UNKNOWN_CHUNK, UNKNOWN_CHUNK_ORIG, 5, (void*)&unknown_chunk_stub)
                 ? "dlc fix: DLC-only track chunks (RET0/EMI1/POR3) are skipped"
                 : "dlc fix: track chunk patch skipped (unexpected code)");
    shim_log(patch_call(TRI_MISMATCH, TRI_MISMATCH_ORIG, 5, (void*)&tri_mismatch_stub)
                 ? "dlc fix: TRI length mismatches load the track anyway (stopgap)"
                 : "dlc fix: TRI mismatch patch skipped (unexpected code)");
    start_dlc_nodes();
    start_retarget_runtime();
    start_particle_diagnostic();
    start_nt_trace();
    start_dlc_portals();
}
