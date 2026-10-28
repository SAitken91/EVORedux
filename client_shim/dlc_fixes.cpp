// Trials Evolution revival: code fixes needed to run the Xbox 360 DLC packs on PC.
//
// DLC assets require datapack\DLCContent. Serialized v47 compatibility also applies without DLC.
// Game addresses are translated and patch bytes checked for each supported build.
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
#include "game_build.h"

// Game addresses the naked stubs below use, translated for the running build (game_build.h).
uintptr_t ga_1ee34c4 = GA(0x1ee34c4);
uintptr_t ga_4eb20b = GA(0x4eb20b);
uintptr_t ga_4eb28a = GA(0x4eb28a);
uintptr_t ga_5ff3ef = GA(0x5ff3ef);
uintptr_t ga_5ff418 = GA(0x5ff418);
uintptr_t ga_5ff600 = GA(0x5ff600);
uintptr_t ga_5ff79a = GA(0x5ff79a);
uintptr_t ga_5ff7c0 = GA(0x5ff7c0);
uintptr_t ga_5ff9a0 = GA(0x5ff9a0);
uintptr_t ga_5ffbea = GA(0x5ffbea);
uintptr_t ga_5ffc10 = GA(0x5ffc10);
uintptr_t ga_5ffdb0 = GA(0x5ffdb0);
uintptr_t ga_6018c0 = GA(0x6018c0);
uintptr_t ga_6178bf = GA(0x6178bf);
uintptr_t ga_618af8 = GA(0x618af8);
uintptr_t ga_62373f = GA(0x62373f);
uintptr_t ga_63355a = GA(0x63355a);
uintptr_t ga_6f1f47 = GA(0x6f1f47);
uintptr_t ga_78b692 = GA(0x78b692);
uintptr_t ga_78b6ac = GA(0x78b6ac);
uintptr_t ga_78bac4 = GA(0x78bac4);
uintptr_t ga_80e171 = GA(0x80e171);
uintptr_t ga_cf5267 = GA(0xcf5267);

#include <intrin.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <string.h>
#include "lzma_peek.inl"

void shim_log(const char* msg);
static bool game_file_exists(const char* name) {
    char out[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, out, MAX_PATH);
    while (n && out[n - 1] != '\\') n--;
    if (!n || n + lstrlenA(name) + 1 > MAX_PATH) return false;
    lstrcpyA(out + n, name);
    return GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
}
// Per-record TRI logging (thousands of lines per track load) only when diagnostics are switched on:
// trials_revive_gamelog.enable or trials_revive_tri_trace.txt in datapack. Read once at load.
static const bool g_tri_trace = game_file_exists("trials_revive_gamelog.enable") ||
                                game_file_exists("trials_revive_tri_trace.txt");

namespace {

const uintptr_t AREA_COLLECTION_PUSH = GA(0x528002);
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

const uintptr_t TEXTURE_OBJECT_LOAD = GA(0x7bcce3);
const unsigned char TEXTURE_OBJECT_ORIG[5] = {0x8b, 0x43, 0x3c, 0x85, 0xc0};   // mov eax,[ebx+3Ch]; test eax,eax
const uintptr_t GAME_TEXT_START = game_text_start(), GAME_TEXT_END = game_text_end();
const uintptr_t GAME_RDATA_START = game_rdata_start(), GAME_RDATA_END = game_rdata_end();
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

const uintptr_t MATERIAL_TEXTURE_WAIT = GA(0x78ba83);
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
        push eax                          // past the free and the detach
        mov eax, ga_78bac4
        mov dword ptr [esp + 4], eax
        pop eax
        ret
    }
}


// ---- 2c. texture atlas drop diagnostics (temporary) ----------------------------------------
// TextureAtlas::addTexture (0x83f950) silently drops an image whose resource belongs to a
// different atlas ([img+0x38] != [atlas+0x30], checked at 0x83fc03). Logs each drop.
const uintptr_t ATLAS_CHECK = GA(0x83fc03);
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

const uintptr_t LB_LOOKUP_CALL = GA(0x6f4455);
const uintptr_t LB_LOOKUP = GA(0x661890);           // TrackInfoManager::getGameModeSupportForLB
const DWORD GAME_GLOBAL = GA(0x1ee3548);            // game object; [+0x4c] = game-mode support table
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

const uintptr_t PACK_TRACKS_CALL = GA(0x65428c);
const uintptr_t PACK_TRACKS_LOAD = GA(0x660360);    // TrackInfoManager: (pack, "pack<N>/tracks.xml")
const uintptr_t BIKES_LOAD = GA(0x6532e0);          // bike table: (0, "pack0/bikes.xml"), ret 8
const uintptr_t ITEMS_LOAD = GA(0x657910);          // item table [game+0x6c]: (0, "pack0/items.xml"), ret 8
const uintptr_t BIKE_GFX = GA(0x656aa0);            // item table: bike id -> bike model gfx id (ret 4)
const uintptr_t BIKE_LOOKUP = GA(0x46d670);         // bike table hash lookup: (&id, &slot) -> bucket
const uintptr_t STRING_FROM_CSTR = GA(0x40c200);    // engine string ctor (thiscall, ret 4)
const uintptr_t ENGINE_FREE = GA(0x436ab0);         // frees an engine string's heap text (flag bit 0 at +0xc)
const uintptr_t GET_BIKE_INFOS = GA(0x6f1f40);      // UI binding; first bytes: push -1; push 0CF5267h
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
        push ga_cf5267
        push ga_6f1f47
        ret
    }
}

bool patch_jmp(uintptr_t at, const unsigned char* expected, size_t len, void* target) {
    if (code_differs((uintptr_t)at, expected, len) != 0) return false;
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
    if (code_differs((uintptr_t)at, expected, len) != 0) return false;
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
    ((SourceResetFn)GA(0x62df70))(self, nullptr);
}

int __fastcall selector_read(void* self, void*, void* reader, void* objects, void* selected, int version) {
    int result = ((SourceReadFn)GA(0x6236c0))(self, nullptr, reader, objects, selected, version);
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
    if (*(BYTE*)GA(0x1eec404) && *(DWORD*)(source + 0x30) == tick) return *(float*)(source + 0x28);
    if (depth > 500 || g_active_selector_count >= 500) {
        *(BYTE*)GA(0x1eec406) = 1;
        return *(float*)(source + 0x28);
    }
    for (int i = 0; i < g_active_selector_count; ++i) {
        if (g_active_selectors[i] == self) {
            *(BYTE*)GA(0x1eec407) = 1;
            return *(float*)(source + 0x28);
        }
    }
    g_active_selectors[g_active_selector_count++] = self;
    float* values = (float*)(source + 0x3c);
    for (DWORD i = 0; i < 3; ++i) {
        void* input = selector_input(*(void**)(source + 4), SELECTOR_PROPERTY + i);
        if (input) {
            float value = ((SourceEvalFn)(*(DWORD**)input)[6])(input, nullptr, visited, depth + 1);
            if (!*(BYTE*)GA(0x1eec405)) values[i] = value;
        }
    }
    if (!*(BYTE*)GA(0x1eec405)) *(float*)(source + 0x28) = selector_value(self, nullptr);
    *(DWORD*)(source + 0x30) = tick;
    --g_active_selector_count;
    return *(float*)(source + 0x28);
}

void __fastcall selector_copy(void* self, void*, void* other) {
    memcpy((BYTE*)self + 0x3c, (BYTE*)other + 0x3c, 12);
}

extern "C" void* __cdecl create_selector(void* owner) {
    void* memory = ((void*(__cdecl*)(size_t))GA(0x436a40))(0x48);
    if (!memory) return nullptr;
    ((SourceCtorFn)GA(0x62e050))(memory, nullptr, owner);
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
        push ga_5ffc10
        ret
    native_noperation:
        push ga_5ffbea
        ret
    other:
        push ebp
        push dword ptr [eax + 0x40]
        call create_dlc_source
        add esp, 8
        test eax, eax
        jz unknown
        push ga_5ffc10
        ret
    unknown:
        push ga_5ffdb0
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
    return (DWORD)(int)((short*)GA(0x1eebec8))[serialized];
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
        push ga_62373f
        ret
    }
}

void start_selector_fix() {
    const BYTE factory[] = {0x81,0x78,0x40,0x45,0xbf,0x61,0xe6,0x0f,0x85,0xc6,0x01,0x00,0x00};
    const BYTE property[] = {0x0f,0xbf,0x0c,0x45,0xc8,0xbe,0xee,0x01};
    if (code_differs((uintptr_t)GA(0x5ffbdd),factory,sizeof factory) || code_differs((uintptr_t)GA(0x623737),property,sizeof property)) {
        shim_log("dlc fix: selector patch skipped (unexpected code)");
        return;
    }
    memcpy(g_selector_vtable,(void*)GA(0x150a61c),sizeof g_selector_vtable);
    g_selector_vtable[1] = (DWORD)&selector_reset;
    g_selector_vtable[4] = (DWORD)&selector_value;
    g_selector_vtable[6] = (DWORD)&selector_evaluate;
    g_selector_vtable[11] = (DWORD)&selector_copy;
    g_selector_vtable[12] = (DWORD)&selector_read;
    if (patch_jmp(GA(0x623737),property,sizeof property,(void*)&selector_property_stub) &&
        patch_jmp(GA(0x5ffbdd),factory,sizeof factory,(void*)&selector_factory_stub))
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
    if (g_tri_trace && count <= 1000) {
        position = ((ReaderPositionFn)(*(DWORD**)reader)[0x4c / 4])(reader, nullptr);
        sprintf_s(line, "trigger trace: begin #%ld this=%p vtable=%08lx reader=%p pos=%lu version=%d",
                  count, self, *(DWORD*)self, reader, position, version);
        shim_log(line);
    }
    int result = ((TriggerLoadFn)g_trigger_load_original)(self, nullptr, reader, objects, selected, version);
    if (g_tri_trace && count <= 1000) {
        DWORD end = ((ReaderPositionFn)(*(DWORD**)reader)[0x4c / 4])(reader, nullptr);
        sprintf_s(line, "trigger trace: end #%ld pos=%lu consumed=%lu result=%d", count, end, end-position, result);
        shim_log(line);
    }
    return result;
}

void start_trigger_trace() {
    const unsigned char code[5] = {0x83, 0xec, 0x0c, 0x55, 0x56};
    if (code_differs((uintptr_t)GA(0x6236c0), code, 5)) return;
    BYTE* trampoline = (BYTE*)VirtualAlloc(nullptr, 10, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) return;
    memcpy(trampoline, (const void*)GA(0x6236c0), 5);      // the live bytes (verified above)
    trampoline[5] = 0xe9;
    *(int*)(trampoline + 6) = (int)(GA(0x6236c5) - ((uintptr_t)trampoline + 10));
    g_trigger_load_original = trampoline;
    if (patch_jmp(GA(0x6236c0), code, 5, (void*)&trace_trigger_load))
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
        for (int j = 0; j < 5; ++j) r.targets[j] = ((RefRead)GA(0x4d06b0))(stream, objects, 0);
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
        if (input) ((void(__fastcall*)(void*, void*, int))GA(0x621ab0))(input, nullptr, 0);
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
        push ga_6178bf
        ret
    empty_target:
        push ga_618af8
        ret
    }
}

void start_retarget_runtime() {
    const BYTE original[5] = {0x8b,0x59,0x08,0x33,0xf6};
    g_retarget_hook_enabled = patch_jmp(GA(0x6178ba), original, sizeof original, (void*)&retarget_transform_stub);
    shim_log(g_retarget_hook_enabled ? "dlc runtime: transform retarget resolution enabled"
                                    : "dlc runtime: retarget hook not installed (unexpected code)");
}

// ---- 6. DLC-only track chunks ----------------------------------------------------------------
// TrackLoader::loadChunks (0x4ea450) fails a track with 0x401 ("Track loading error") on any chunk
// id it doesn't know (0x4eb285). The DLC tracks use three families the PC engine has no code for
// at all (not in its chunk family table at 0xbf9e3c): RET0 (5 tracks, e.g. Magic Marker), EMI1
// (emitters, 4 tracks) and POR3 (portals, 12 tracks). These three are now read past and logged,
// so the tracks load without those features; any other unknown id still fails as before.

const uintptr_t UNKNOWN_CHUNK = GA(0x4eb285);        // mov eax,[1EE34C4h] (logger) on the failure path
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
        push ga_4eb20b
        ret
    failed:
        mov eax, ga_1ee34c4
        mov eax, dword ptr [eax]
        push ga_4eb28a
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

const uintptr_t TRI_MISMATCH = GA(0x4b7ced);         // mov eax,401h after the seek to the chunk end
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
// Buccaneer Bay (Origin of Pain track 222) has one object, OBJX #1651 (a DataSourceVariable,
// model 16903902), for which the 360 saved no TRI record: its bytes at body offset 128053 belong
// to the next datasource. The PC read a record for it anyway, every later record was misaligned, a
// vector reader then ran 4 KB to the end and each remaining record waited at EOF (the loading
// screen froze; GitHub issue #2). The 360's Variable reader (TU3 0x825d87a0) skips an owner that
// is missing from the loader's record set, but on PC that set (loader+0x24a14) holds every object
// (2493 here, 1651 included), so the 360's rule for leaving it out isn't reproducible yet. It is
// the only such object in all 82 official DLC tracks and in 5,808 of 5,825 converted 360 tracks
// (offline walk), so this one record is skipped when every detail matches.
bool is_buccaneer_bay_missing_record(void* objects, DWORD owner, DWORD start, int version) {
    if (version != 47 || start != 128053) return false;
    __try {
        const DWORD count = *(const DWORD*)objects;
        const DWORD* items = *(const DWORD* const*)((const BYTE*)objects + 8);
        if (count != 2493 || items[1651] != owner) return false;
        const BYTE* info = *(const BYTE* const*)((const BYTE*)owner + 0x44);       // object -> instance info
        const BYTE* definition = info ? *(const BYTE* const*)info : nullptr;
        return definition && *(const DWORD*)(definition + 0x40) == 0x16903902;   // DataSourceVariable
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int __fastcall trace_trigger_record(void* self, DWORD slot, void* reader, void* objects, void* selected, int version) {
    bool datasource = slot == 0x130;   // from trace_pass30ds: the datasource pass (same vtable slot as filters)
    slot &= 0xff;
    DWORD start = ((ReaderPositionFn)(*(DWORD**)reader)[0x4c / 4])(reader, nullptr);
    DWORD vt = *(DWORD*)self;
    char line[240];
    if (datasource && is_buccaneer_bay_missing_record(objects, ((DWORD*)self)[1], start, version)) {
        shim_log("dlc fix: Buccaneer Bay object 1651 has no TRI record (360 saved none); skipped");
        return 0;
    }
    if (g_tri_trace) {
        sprintf_s(line, "TRI record begin: slot=%lx vtable=%08lx id=%lu pos=%lu", slot, vt, ((DWORD*)self)[1], start);
        shim_log(line);
    }
    int result = ((TriggerLoadFn)(*(DWORD**)self)[slot / 4])(self, nullptr, reader, objects, selected, version);
    DWORD end = ((ReaderPositionFn)(*(DWORD**)reader)[0x4c / 4])(reader, nullptr);
    // Xbox DLC v47 appends two flags to EventPlayerState. The PC implementation
    // reads the common fields only. Leaving these bytes behind misaligns every
    // subsequent event (Forge failed at EventFire with a bogus input count 257).
    // Keep common PC behavior; the two DLC-only flag semantics remain unmapped.
    // Version 47 is the 360 (TU3) format: PC tracks are v42 (TRIg) and lack these bytes, while 360
    // custom tracks in base worlds have them too (Blood Moon failed at EventSound when this was
    // limited to DLC worlds).
    if (!result && slot == 0x38 && vt == GA(0x141d39c) && version == 47 && end > start) {
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
    if (!result && slot == 0x30 && vt == GA(0x1589a1c) && version == 47 && end > start) {
        BYTE flag = 0;
        StreamReadFn read = *(StreamReadFn*)(*(BYTE**)reader + 0x34);
        if (read(reader, nullptr, &flag, 1) != 1) result = 0x401;
        sprintf_s(line, "dlc impulse-counter record: consumed extension %u at %lu", flag, end);
        shim_log(line);
        end = ((ReaderPositionFn)(*(DWORD**)reader)[0x4c / 4])(reader, nullptr);
    }
    if (g_tri_trace) {
        sprintf_s(line, "TRI record end: slot=%lx vtable=%08lx id=%lu pos=%lu consumed=%lu result=%d",
                  slot, vt, ((DWORD*)self)[1], end, end-start, result);
        shim_log(line);
    }
    if (!result && slot == 0x28 && vt == GA(0x12b1f54)) portal_register_hit(self);
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
__declspec(naked) void trace_pass30ds() {
    __asm { mov edx, 0x130
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
        push ga_6018c0
        ret
    }
}

// The PC VT loader selects terrain by world, but hardcodes the base object
// index. The staged indices retain base pages and overlay the selected DLC's
// pages, with unique raw-file codes so overlapping filenames cannot collide.
// Base worlds get vtex_objects_dlc0.raw.hdr: the base index plus only the DLC pages whose ids the
// base index doesn't use (taken from the two staged DLC indices). Base textures are unchanged and
// DLC-only textures exist, so a DLC bike ridden on a base track keeps its frame (the Gecko 520's
// frame was invisible on base tracks: its pages are DLC-only). Built here from the DLC indices
// the importer already staged, so players need no new import.
#pragma pack(push, 1)
struct VtRow { DWORD page, offset; WORD size; BYTE file; };
#pragma pack(pop)

bool read_vt_index(const char* path, VtRow** rows, DWORD* count) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, nullptr), n = 0, got = 0;
    bool ok = size >= 4 && ReadFile(f, &n, 4, &got, nullptr) && got == 4 && size == 4 + n * sizeof(VtRow);
    VtRow* r = ok ? (VtRow*)HeapAlloc(GetProcessHeap(), 0, n * sizeof(VtRow) + 1) : nullptr;
    ok = r && ReadFile(f, r, n * sizeof(VtRow), &got, nullptr) && got == n * sizeof(VtRow);
    CloseHandle(f);
    if (!ok) { if (r) HeapFree(GetProcessHeap(), 0, r); return false; }
    *rows = r;
    *count = n;
    return true;
}

bool vt_has(const VtRow* rows, DWORD n, DWORD page) {          // rows sorted by page
    DWORD lo = 0, hi = n;
    while (lo < hi) {
        DWORD mid = (lo + hi) / 2;
        if (rows[mid].page < page) lo = mid + 1; else hi = mid;
    }
    return lo < n && rows[lo].page == page;
}

int vt_compare(const void* a, const void* b) {
    DWORD x = ((const VtRow*)a)->page, y = ((const VtRow*)b)->page;
    return x < y ? -1 : x > y;
}

ULONGLONG file_time(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
    return ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
}

// dir ends with a backslash. True when vtex_objects_dlc0.raw.hdr is ready to use.
bool ensure_base_world_index(const char* dir) {
    static int state;                                  // 0 unknown, 1 ready, 2 unavailable
    if (state) return state == 1;
    char base[MAX_PATH], d1[MAX_PATH], d2[MAX_PATH], out[MAX_PATH], tmp[MAX_PATH];
    sprintf_s(base, "%svtex_objects.raw.hdr", dir);
    sprintf_s(d1, "%svtex_objects_dlc1.raw.hdr", dir);
    sprintf_s(d2, "%svtex_objects_dlc2.raw.hdr", dir);
    sprintf_s(out, "%svtex_objects_dlc0.raw.hdr", dir);
    sprintf_s(tmp, "%svtex_objects_dlc0.raw.hdr.tmp", dir);
    ULONGLONG t1 = file_time(d1), t2 = file_time(d2), t0 = file_time(out);
    if (!t1 && !t2) { state = 2; return false; }       // DLC not imported
    if (t0 && t0 >= t1 && t0 >= t2 && t0 >= file_time(base)) { state = 1; return true; }
    VtRow *b = nullptr, *m1 = nullptr, *m2 = nullptr;
    DWORD nb = 0, n1 = 0, n2 = 0;
    state = 2;
    if (!read_vt_index(base, &b, &nb)) return false;
    if (t1) read_vt_index(d1, &m1, &n1);
    if (t2) read_vt_index(d2, &m2, &n2);
    VtRow* all = (VtRow*)HeapAlloc(GetProcessHeap(), 0, (nb + n1 + n2) * sizeof(VtRow) + 1);
    if (all) {
        DWORD n = nb, from_dlc = 0;
        memcpy(all, b, nb * sizeof(VtRow));
        for (DWORD i = 0; i < n1; i++)
            if (!vt_has(b, nb, m1[i].page)) { all[n++] = m1[i]; from_dlc++; }
        for (DWORD i = 0; i < n2; i++)
            if (!vt_has(b, nb, m2[i].page) && !(m1 && vt_has(m1, n1, m2[i].page))) { all[n++] = m2[i]; from_dlc++; }
        qsort(all, n, sizeof(VtRow), vt_compare);
        HANDLE f = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD put = 0, put2 = 0;
        bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, &n, 4, &put, nullptr)
               && WriteFile(f, all, n * sizeof(VtRow), &put2, nullptr) && put2 == n * sizeof(VtRow);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        ok = ok && MoveFileExA(tmp, out, MOVEFILE_REPLACE_EXISTING);
        if (!ok) DeleteFileA(tmp);
        char line[200];
        sprintf_s(line, ok ? "dlc texture fix: base-world index written (%lu base + %lu DLC-only pages)"
                           : "dlc texture fix: could not write the base-world index (%lu + %lu pages)", nb, from_dlc);
        shim_log(line);
        if (ok) state = 1;
        HeapFree(GetProcessHeap(), 0, all);
    }
    HeapFree(GetProcessHeap(), 0, b);
    if (m1) HeapFree(GetProcessHeap(), 0, m1);
    if (m2) HeapFree(GetProcessHeap(), 0, m2);
    return state == 1;
}

// DLC worlds the same way: world N gets vtex_objects_dlcN_all.raw.hdr = its staged index (base +
// pack N, unchanged) plus the other pack's pages whose ids it doesn't have (only those; nothing
// that resolves today changes). World 1 lacked 37,819 Riders-of-Doom-only pages and world 2 21,605
// Origin-of-Pain-only ones; the Gecko 520 kept its wheels but lost its frame on Paine Island tracks
// while showing it on base tracks, whose dlc0 index already had both packs' pages.
bool ensure_dlc_world_index(const char* dir, DWORD world) {
    static int state[3];                               // per world: 0 unknown, 1 ready, 2 unavailable
    if (world != 1 && world != 2) return false;
    if (state[world]) return state[world] == 1;
    char own[MAX_PATH], other[MAX_PATH], out[MAX_PATH], tmp[MAX_PATH];
    sprintf_s(own, "%svtex_objects_dlc%lu.raw.hdr", dir, world);
    sprintf_s(other, "%svtex_objects_dlc%lu.raw.hdr", dir, 3 - world);
    sprintf_s(out, "%svtex_objects_dlc%lu_all.raw.hdr", dir, world);
    sprintf_s(tmp, "%svtex_objects_dlc%lu_all.raw.hdr.tmp", dir, world);
    ULONGLONG t_own = file_time(own), t_other = file_time(other), t_out = file_time(out);
    state[world] = 2;
    if (!t_own || !t_other) return false;              // one pack not imported: keep its own index
    if (t_out && t_out >= t_own && t_out >= t_other) { state[world] = 1; return true; }
    VtRow *a = nullptr, *b = nullptr;
    DWORD na = 0, nb = 0;
    if (!read_vt_index(own, &a, &na)) return false;
    if (!read_vt_index(other, &b, &nb)) { HeapFree(GetProcessHeap(), 0, a); return false; }
    VtRow* all = (VtRow*)HeapAlloc(GetProcessHeap(), 0, (na + nb) * sizeof(VtRow) + 1);
    if (all) {
        DWORD n = na, added = 0;
        memcpy(all, a, na * sizeof(VtRow));
        for (DWORD i = 0; i < nb; i++)
            if (!vt_has(a, na, b[i].page)) { all[n++] = b[i]; added++; }
        qsort(all, n, sizeof(VtRow), vt_compare);
        HANDLE f = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD put = 0, put2 = 0;
        bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, &n, 4, &put, nullptr)
               && WriteFile(f, all, n * sizeof(VtRow), &put2, nullptr) && put2 == n * sizeof(VtRow);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        ok = ok && MoveFileExA(tmp, out, MOVEFILE_REPLACE_EXISTING);
        if (!ok) DeleteFileA(tmp);
        char line[200];
        sprintf_s(line, ok ? "dlc texture fix: world %lu index written (%lu pages + %lu from the other pack)"
                           : "dlc texture fix: could not write the world %lu index (%lu + %lu pages)", world, na, added);
        shim_log(line);
        if (ok) state[world] = 1;
        HeapFree(GetProcessHeap(), 0, all);
    }
    HeapFree(GetProcessHeap(), 0, a);
    HeapFree(GetProcessHeap(), 0, b);
    return state[world] == 1;
}

void portal_forget_track();
extern "C" const char* __cdecl dlc_object_index(DWORD world) {
    portal_forget_track();
    g_texture_world = world;
    const char* original = "vtex/vtex_objects.raw.hdr";
    if (world != 1 && world != 2) {
        char dir[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, dir, MAX_PATH);
        while (n && dir[n - 1] != '\\') n--;
        if (!n || n + 40 >= MAX_PATH) return original;
        sprintf_s(dir + n, MAX_PATH - n, "..\\build\\data\\vtex\\");
        return ensure_base_world_index(dir) ? "vtex/vtex_objects_dlc0.raw.hdr" : original;
    }
    const char* selected = world == 1 ? "vtex/vtex_objects_dlc1.raw.hdr"
                                    : "vtex/vtex_objects_dlc2.raw.hdr";
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return original;
    while (n && path[n - 1] != '\\') n--;
    if (!n || n + 70 >= MAX_PATH) return original;
    sprintf_s(path + n, MAX_PATH - n, "..\\build\\data\\vtex\\");
    // Test switch: datapack\trials_revive_world_index_own.txt keeps each DLC world on its own index.
    if (!game_file_exists("trials_revive_world_index_own.txt") && ensure_dlc_world_index(path, world)) {
        selected = world == 1 ? "vtex/vtex_objects_dlc1_all.raw.hdr" : "vtex/vtex_objects_dlc2_all.raw.hdr";
        char line[160];
        sprintf_s(line, "dlc texture fix: world %lu uses %s", world, selected);
        shim_log(line);
        return selected;
    }
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
        push ga_80e171
        ret
    }
}

// Decal objects. The block renderer (0x80a4a0) opens the terrain decals per world
// ("vtex/decalsTerrain" + world+1, from [renderer+0xd4]) but hardcodes "vtex/decalsObjects.dat" at
// 0x80a569, so DLC worlds drew their decal objects (sand, rock, weed patches) from the PC base table:
// Buccaneer Bay's seabed came out as white tiles and sign lettering (RenderDoc: the terrain draw).
// Both DLC packs ship the 360's own vtex/decalsObjects.dat (identical in the two packs, not the PC
// file); the importer stages it as build\data\vtex\decalsObjects_dlc<N>.dat. DLC worlds use it when
// it is there; base worlds keep the PC file.
extern "C" const char* __cdecl dlc_decal_objects(DWORD world) {
    const char* original = "vtex/decalsObjects.dat";
    if (world != 1 && world != 2) return original;
    const char* selected = world == 1 ? "vtex/decalsObjects_dlc1.dat" : "vtex/decalsObjects_dlc2.dat";
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return original;
    while (n && path[n - 1] != '\\') n--;
    if (!n || n + 60 >= MAX_PATH) return original;
    sprintf_s(path + n, MAX_PATH - n, "..\\build\\data\\%s", selected);
    DWORD attr = GetFileAttributesA(path);
    char line[160];
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        sprintf_s(line, "dlc texture fix: %s not staged; world %lu keeps the base decal objects", selected, world);
        shim_log(line);
        return original;
    }
    sprintf_s(line, "dlc texture fix: world %lu decal objects from %s", world, selected);
    shim_log(line);
    return selected;
}

uintptr_t ga_80a56e = GA(0x80a56e);
// Replaces push 01ADCC10h at 80a569 (ebp = block renderer); same contract as the index stub above.
__declspec(naked) void dlc_decal_objects_stub() {
    __asm {
        pushfd
        pushad
        push dword ptr [ebp + 0xd4]
        call dlc_decal_objects
        add esp, 4
        mov [esp + 0x24], eax
        popad
        popfd
        push ga_80a56e
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
    // Read only (its effect is not implemented). Only 360 custom tracks use it: 15 of them, each
    // record the event base + u8 + float (01 0001 ffff | 01 | 3f800000), found by the offline walk.
    {{0xd236eb2a, 0x0e2c8adb}, 'e', "drama", "Ebf"},
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
#include "dlc_page_clear.inl"

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
        int result = ((SourceReadFn)GA(0x634040))(self, nullptr, stream, objects, selected, version);
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
    ((unsigned char(__fastcall*)(void*, void*))GA(0x633f50))(self, nullptr);
    if (!*((BYTE*)self + 0x70)) return 0x1f; // native disabled-filter bypass
    const float* values = (const float*)((BYTE*)self + 0x78);
    void* owner = ((void**)self)[1];
    void* input = selector_input(owner, DLC_PROPERTY_BASE + 508);
    // The 360 switch refreshes its comparison from the input before matching (TU3 0x825eccf0);
    // PC data sources are evaluated on demand, so evaluate the input first.
    if (input) ((void(__fastcall*)(void*, void*, int))GA(0x621ab0))(input, nullptr, 0);
    float comparison = input ? *(float*)((BYTE*)input + 0x28) : values[0];
    unsigned char mask = 0;                      // every matching case fires, as on the 360
    for (DWORD k = 0; k < 4; ++k)
        if (comparison == values[k + 1]) mask |= (unsigned char)(1u << k);
    if (!mask) mask = 0x10;
    nt_trace_switch(owner, comparison, input != nullptr, mask);
    return mask;
}

void __fastcall dlc_switch_copy(void* self, void*, void* other) {
    ((void(__fastcall*)(void*, void*, void*))GA(0x633f20))(self, nullptr, other);
    if (other) memcpy((BYTE*)self + 0x78, (BYTE*)other + 0x78, 5 * sizeof(float));
}

// Placeholder construction: the closest native class (same allocation size and constructor),
// then the cloned vtable whose read slot consumes the DLC layout.
struct NodeBase { DWORD size; uintptr_t ctor; uintptr_t vtable; int slots; int read_slot; };
const NodeBase NODE_BASE[3] = {
    {0x48, GA(0x62e050), GA(0x150a61c), 16, 0x30 / 4},   // 'd' DataSourceOperation
    {0xb4, GA(0x61de50), GA(0x146618c), 18, 0x38 / 4},   // 'e' EventState (no targets -> no effect)
    {0x8c, GA(0x633e90), GA(0x15adf38), 13, 0x30 / 4},   // 'f' FilterImpulseSplitter (no outputs)
};

const NodeBase& node_base(char kind) { return NODE_BASE[kind == 'd' ? 0 : kind == 'e' ? 1 : 2]; }
const NodeBase VECTOR_CURVE_BASE = {0x60, GA(0x625690), GA(0x14af43c), 16, 12};

void init_dlc_nodes() {
    memcpy(g_vector_adapter_vtable, (void*)GA(0x150a61c), sizeof g_vector_adapter_vtable);
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
    void* memory = ((void*(__cdecl*)(size_t))GA(0x436a40))(b.size);
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
        push ga_5ff3ef
        ret
    not_game:
        push 'e'
        push ebp
        push eax
        call create_dlc_node
        add esp, 12
        test eax, eax
        jz unknown
        push ga_5ff418
        ret
    unknown:
        push ga_5ff600
        ret
    }
}

// Filter factory (0x5ff620): edx = definition, ebp = owner.
// Replaces `cmp dword ptr [edx+40h],84671807h; jne 5FF9A0h`.
__declspec(naked) void dlc_filter_factory_stub() {
    __asm {
        cmp dword ptr [edx + 0x40], 0x84671807
        jne not_splitter
        push ga_5ff79a
        ret
    not_splitter:
        push 'f'
        push ebp
        push dword ptr [edx + 0x40]
        call create_dlc_node
        add esp, 12
        test eax, eax
        jz unknown
        push ga_5ff7c0
        ret
    unknown:
        push ga_5ff9a0
        ret
    }
}

const unsigned char EVENT_FACTORY_TAIL[11] = {0x3d, 0x2f, 0xeb, 0xb7, 0xa9, 0x0f, 0x85, 0x11, 0x02, 0x00, 0x00};
const unsigned char FILTER_FACTORY_TAIL[13] = {0x81, 0x7a, 0x40, 0x07, 0x18, 0x67, 0x84, 0x0f, 0x85, 0x06, 0x02, 0x00, 0x00};

void start_dlc_nodes() {
    g_vector_runtime_enabled = game_file_exists("trials_revive_vector_runtime.txt");
    init_dlc_nodes();
    shim_log(patch_jmp(GA(0x5ff3e4), EVENT_FACTORY_TAIL, sizeof EVENT_FACTORY_TAIL, (void*)&dlc_event_factory_stub) &&
             patch_jmp(GA(0x5ff78d), FILTER_FACTORY_TAIL, sizeof FILTER_FACTORY_TAIL, (void*)&dlc_filter_factory_stub)
                 ? (g_vector_runtime_enabled ? "dlc fix: switch routing and staged vector runtime enabled"
                                            : "dlc fix: switch routing enabled; staged vector runtime disabled")
                 : "dlc fix: DLC node placeholders skipped (unexpected code)");
}

// ---- 9. DLC object definitions outside DLC worlds ---------------------------------------------
// An object definition only gets its type flags ([def+0x50]: trigger, event, datasource, filter,
// object, ...; bit 13 = has game logic) from Track::loadObjectsFromObjectCollectionCategory
// (0x49a4b0), which world init (0x4a74c0) runs over the whole collection. On PC the DLC packs are
// registered after the startup world init, so objectcollection1/2.xml (loaded at the end of the
// pack loader, 0x6545f0 -> 0x42f3f0) stay flagless until a DLC world is opened. DLC tracks never
// notice (opening their world re-runs world init); a custom track in a base world does: its DLC
// objects create no trigger nodes (Mysterium: its pack-1 switch has no record reader, so the next
// filter read the switch's bytes and the load failed with 0x401) and are not treated as objects
// (start platforms missing). The 360 loads every installed collection before the first world.
// After the pack loader has loaded the collections, the same pass is run once over each DLC
// category ("DLC<n>", the root of objectcollection<n>.xml); base categories are left alone.
const uintptr_t PACK_COLLECTIONS_CALL = GA(0x6545f0);   // pack loader: call 42F3F0h, ecx = game
const uintptr_t LOAD_PACK_COLLECTIONS = GA(0x42f3f0);   // GameBase: objectcollection1..256.xml
const uintptr_t CLASSIFY_CATEGORY = GA(0x49a4b0);       // Track: (category), ret 4; recurses
const uintptr_t CATEGORY_SUBCATEGORIES = GA(0x4af7e0);  // category: (&iterator) -> first list node
const uintptr_t OBJECT_COLLECTION = GA(0x1eee614);      // [[..]] = collection = root category
const uintptr_t DEFINITION_OF = GA(0x786e10);           // collection: (model) -> definition, ret 4
typedef void(__fastcall* GameFn)(void* self, void* edx);
typedef void(__fastcall* ClassifyFn)(void* track, void* edx, void* category);
typedef void*(__fastcall* IteratorFn)(void* category, void* edx, void* out);
typedef BYTE*(__fastcall* DefinitionFn)(void* collection, void* edx, DWORD model);

void* g_classified[8];
int g_classified_count;

// List nodes: +4 next, +8 category. Category name: engine string at +0x1c (length +6, text +8).
bool is_dlc_category(const BYTE* category) {
    const char* name = *(const char* const*)(category + 0x1c + 8);
    unsigned len = *(const unsigned short*)(category + 0x1c + 6);
    if (len < 4 || len > 6 || memcmp(name, "DLC", 3) != 0) return false;
    for (unsigned i = 3; i < len; i++)
        if (!isdigit((unsigned char)name[i])) return false;
    return true;
}

// The DLC categories below `category` (searched two levels deep), each classified once.
void classify_dlc_categories(void* track, BYTE* category, int depth, char* names, size_t names_size) {
    DWORD iterator[4] = {};
    ((IteratorFn)CATEGORY_SUBCATEGORIES)(category, nullptr, iterator);
    for (const BYTE* node = (const BYTE*)iterator[0]; node; node = *(const BYTE* const*)(node + 4)) {
        BYTE* child = *(BYTE* const*)(node + 8);
        if (!child) continue;
        if (!is_dlc_category(child)) {
            if (depth < 1) classify_dlc_categories(track, child, depth + 1, names, names_size);
            continue;
        }
        bool done = false;
        for (int i = 0; i < g_classified_count; i++) done |= g_classified[i] == child;
        if (done || g_classified_count == _countof(g_classified)) continue;
        g_classified[g_classified_count++] = child;
        ((ClassifyFn)CLASSIFY_CATEGORY)(track, nullptr, child);
        size_t n = strlen(names);
        sprintf_s(names + n, names_size - n, "%s%.*s", n ? ", " : "",
                  (int)*(const unsigned short*)(child + 0x1c + 6), *(const char* const*)(child + 0x1c + 8));
    }
}

DWORD definition_flags(void* collection, DWORD model) {
    BYTE* definition = ((DefinitionFn)DEFINITION_OF)(collection, nullptr, model);
    return definition ? *(DWORD*)(definition + 0x50) : 0xffffffff;
}

void __fastcall load_pack_collections_and_classify(void* game, void* edx) {
    ((GameFn)LOAD_PACK_COLLECTIONS)(game, edx);
    char line[256], names[96] = "";
    __try {
        void* track = *(void**)((BYTE*)game + 0x30);
        void* manager = *(void**)OBJECT_COLLECTION;
        void* collection = manager ? *(void**)manager : nullptr;
        if (!track || !collection) {
            shim_log("dlc fix: DLC object definitions not classified (no track or collection yet)");
            return;
        }
        classify_dlc_categories(track, (BYTE*)collection, 0, names, sizeof names);
        // Pack 1 / pack 2 switch_filter: 0x20 = filter, 0x2000 = game logic.
        sprintf_s(line, "dlc fix: classified DLC object categories [%s]; switch definitions %08lx %08lx",
                  names, definition_flags(collection, 0xaee65e01), definition_flags(collection, 0x8b49a0d7));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sprintf_s(line, "dlc fix: classifying DLC object definitions faulted (%08lx)", GetExceptionCode());
    }
    shim_log(line);
}

// ---- 10. 360 custom tracks built in a DLC world -----------------------------------------------
// A 360 track made in a DLC world says so in its DLC0 chunk: u32 pack bits (DLC objects), then a
// byte naming the world it was built in (1 = Paine Island, 2 = Riders of Doom; 1,781 of the 5,825
// converted tracks). The 360 reads it from the track buffer (TU3 0x823dfbc0, the same 0x13880-byte
// buffer as PC 0x4b5e80) so the menu can load that world. The PC never switches world for a user
// track, so these played in whatever world was loaded, normally the base one: the course buried
// under the base world's hills (TT-Blood Moon) or the start below the base terrain (TT-Plywood
// Punisher sinks into the floor).
// The menu's startPlayUserTrack binding (0x6ecb10) hands the running id to the sharing manager at
// 0x6ecc91 (track with a leaderboard) or 0x6eccc1 (lbIdx -1: every converted track). Before that call: read the track with the game's own loader (0x6643c0, as
// loadUserTrackSynch does; GameStatePlay reads it again afterwards), decode the start of the body,
// and when its world differs from the loaded one switch exactly as the menu's loadWorld binding
// (0x6e3840) does. Tracks from other worlds (DLC0 byte 0, PC tracks) go back to world 0.
const uintptr_t PLAY_USER_TRACK_CALL = GA(0x6ecc91);   // call 7176B0h, ecx = [game+0x80]
const uintptr_t PLAY_USER_TRACK_CALL_NO_LB = GA(0x6eccc1);   // the same call when lbIdx is -1
const uintptr_t PLAY_USER_TRACK = GA(0x7176b0);        // SharingManager: 8 args, running id first; ret 20h
const uintptr_t USER_TRACK_INFO = GA(0x662640);        // UserTrackManager: (running id) -> entry, ret 4
const uintptr_t LOAD_USER_TRACK_FILES = GA(0x6643c0);  // UserTrackManager: (running id, 1) -> 0 = ok, ret 8
const uintptr_t WORLD_LEAVE = GA(0x5dee50);            // [track+0x5ab4]: (1), ret 4
const uintptr_t TRACK_CLEAR = GA(0x4a98d0);            // track, ret 0
const uintptr_t WORLD_LOAD = GA(0x4ab1c0);             // track: (world), ret 4 -> world init 0x4a74c0
const uintptr_t WORLD_SET_AREA = GA(0x483430);         // track: (value) -> [track+0x5b04], ret 4
typedef void(__fastcall* PlayUserTrackFn)(void* sharing, void* edx, DWORD id, DWORD a1, DWORD a2, DWORD a3,
                                         DWORD a4, DWORD a5, DWORD a6, DWORD a7);
typedef BYTE*(__fastcall* UserTrackInfoFn)(void* manager, void* edx, DWORD id);
typedef int(__fastcall* LoadUserTrackFilesFn)(void* manager, void* edx, DWORD id, DWORD flag);
typedef void(__fastcall* ThisArgFn)(void* self, void* edx, DWORD arg);
typedef void(__fastcall* ThisFn)(void* self, void* edx);

// World byte of a track.trk image (magic, 33 header bytes, LZMA props at 37, size, stream at 46),
// or -1 when the image has no readable DLC0 chunk.
int track_world(const BYTE* trk, DWORD size) {
    static const BYTE PC_MAGIC[4] = {0xde, 0xad, 0xba, 0xbe}, X360_MAGIC[4] = {0xca, 0xfe, 0xb0, 0x0b};
    if (!trk || size < 64 || size > 0x100000) return -1;
    if (memcmp(trk, PC_MAGIC, 4) != 0 && memcmp(trk, X360_MAGIC, 4) != 0) return -1;
    BYTE body[512];
    size_t n = lzma_peek::decode(trk + 37, trk + 46, size - 46, body, sizeof body);
    for (size_t o = 0; o + 8 <= n;) {
        DWORD len = (DWORD)body[o + 4] << 24 | (DWORD)body[o + 5] << 16 | (DWORD)body[o + 6] << 8 | body[o + 7];
        if (memcmp(body + o, "DLC0", 4) == 0) return len >= 5 && o + 8 + 5 <= n ? body[o + 8 + 4] : -1;
        if (len > n) break;
        o += 8 + len;
    }
    return -1;
}

void load_user_track_world(DWORD id) {
    char line[200];
    BYTE* game = *(BYTE**)GAME_GLOBAL;
    void* manager = *(void**)(game + 0x88);
    BYTE* track = *(BYTE**)(game + 0x30);
    if (!manager || !track) return;
    const BYTE* data = nullptr;
    DWORD size = 0;
    BYTE* entry = ((UserTrackInfoFn)USER_TRACK_INFO)(manager, nullptr, id);
    if (!entry) {
        sprintf_s(line, "dlc world: user track %lu not found; world unchanged", id);
        shim_log(line);
        return;
    }
    const char* source = entry[5] ? "memory" : "file";
    if (entry[5]) {   // track held in memory (loadUserTrackSynch's first branch)
        const BYTE* m = *(const BYTE* const*)(entry + 0x28);
        data = *(const BYTE* const*)(m + 4) + *(const DWORD*)(m + 0xc);
        size = *(const DWORD*)(m + 8) - *(const DWORD*)(m + 0xc);
    } else {
        int result = ((LoadUserTrackFilesFn)LOAD_USER_TRACK_FILES)(manager, nullptr, id, 1);
        if (result) {
            sprintf_s(line, "dlc world: user track %lu could not be read ahead (%d); world unchanged", id, result);
            shim_log(line);
            return;
        }
        const BYTE* loader = *(const BYTE* const*)(game + 0x64);
        data = loader + 8;
        size = *(const DWORD*)(loader + 0x13888);
    }
    int world = track_world(data, size);
    DWORD current = *(DWORD*)(track + 0x5b00);
    bool load = world >= 0 && world <= 2 && (DWORD)world != current;
    sprintf_s(line, "dlc world: user track %lu (%s, %lu bytes): built in world %d, loaded world %lu%s", id, source,
              size, world, current, load ? "; loading its world" : world < 0 || world > 2 ? "; no usable DLC0, kept" : "");
    shim_log(line);
    if (!load) return;
    DWORD area = *(DWORD*)(track + 0x5b04);
    ((ThisArgFn)WORLD_LEAVE)(*(void**)(track + 0x5ab4), nullptr, 1);
    ((ThisFn)TRACK_CLEAR)(track, nullptr);
    ((ThisArgFn)WORLD_LOAD)(track, nullptr, (DWORD)world);
    ((ThisArgFn)WORLD_SET_AREA)(track, nullptr, area);
}

void __fastcall play_user_track_in_its_world(void* sharing, void* edx, DWORD id, DWORD a1, DWORD a2, DWORD a3,
                                             DWORD a4, DWORD a5, DWORD a6, DWORD a7) {
    __try {
        load_user_track_world(id & 0xffff);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        char line[96];
        sprintf_s(line, "dlc world: choosing the user track's world faulted (%08lx)", GetExceptionCode());
        shim_log(line);
    }
    ((PlayUserTrackFn)PLAY_USER_TRACK)(sharing, edx, id, a1, a2, a3, a4, a5, a6, a7);
}

// ---- 11. Virtual-texture tiles left over from the previous world -----------------------------------
// Changing world (world switch 0x4ab1c0 -> VT index loader 0x80de30) loads the new world's terrain and
// object page indices but keeps the page cache's already-decoded tiles. Page ids repeat between
// worlds, so tiles of the previous world kept showing: Paine Island ground on base tracks played
// after it (GitHub issue), patches of other ground and sign textures on Buccaneer Bay, Recon Rider
// and Adrenaline Fair. The PC shipped with one world and never needed more; the engine still has the
// two "reload everything" calls (0x80cb20 / 0x80cb60: mark the cache's two dirty rectangles as the
// whole 0..0x40000 range, under the cache lock), which nothing calls. After the loader returns, both
// are called on the page cache ([[[0x1eee680]+0x40]], the one VirtualTexture::0x7ac000 updates every
// frame), so the next frame reloads every tile from the new world's data.
const uintptr_t WORLD_VT_LOAD_CALL = GA(0x4ab33c);   // world switch: call 80DE30h, ecx = VT system
const uintptr_t VT_LOAD_INDICES = GA(0x80de30);      // VT system: (world), ret 4
const uintptr_t VT_RELOAD_ALL_1 = GA(0x80cb20);      // page cache: whole first dirty rectangle
const uintptr_t VT_RELOAD_ALL_2 = GA(0x80cb60);      // page cache: whole second dirty rectangle
const uintptr_t VIRTUAL_TEXTURE = GA(0x1eee680);     // [..] = VirtualTexture; [+0x40] -> {page cache, VT system, ...}

void __fastcall load_world_textures_fresh(void* vt_system, void* edx, DWORD world) {
    ((ThisArgFn)VT_LOAD_INDICES)(vt_system, edx, world);
    __try {
        const BYTE* texture = *(const BYTE* const*)VIRTUAL_TEXTURE;
        void* page_cache = texture ? **(void* const* const*)(texture + 0x40) : nullptr;
        if (!page_cache) return;
        ((ThisFn)VT_RELOAD_ALL_1)(page_cache, nullptr);
        ((ThisFn)VT_RELOAD_ALL_2)(page_cache, nullptr);
        char line[96];
        sprintf_s(line, "dlc texture fix: world %lu loaded; texture page cache reloaded", world);
        shim_log(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        shim_log("dlc texture fix: reloading the texture page cache faulted");
    }
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
    if (!game_build_known()) return;
    start_trigger_trace();
    const unsigned char pass28[5] = {0x8b,0x11,0x8b,0x42,0x28};
    const unsigned char pass38[5] = {0x8b,0x11,0x8b,0x42,0x38};
    const unsigned char pass30[5] = {0x8b,0x11,0x8b,0x42,0x30};
    if (patch_call(GA(0x6017d7), pass28, 5, (void*)&trace_pass28) &&
        patch_call(GA(0x601818), pass38, 5, (void*)&trace_pass38) &&
        patch_call(GA(0x601859), pass30, 5, (void*)&trace_pass30) &&
        patch_call(GA(0x601899), pass30, 5, (void*)&trace_pass30ds))
        shim_log("TRI all-pass record diagnostics enabled");
    const unsigned char trigger_return[7] = {0x5f, 0x5e, 0x5d, 0x5b, 0x83, 0xc4, 0x0c};
    shim_log(patch_jmp(GA(0x6018b9), trigger_return, 7, (void*)&trigger_failure_stub)
                 ? "trigger failure diagnostics enabled"
                 : "trigger failure diagnostics skipped (unexpected code)");
    // TRIl is also used by 360 tracks containing only base objects. Their player-state and
    // impulse-counter extensions must be consumed even on a PC with no DLCContent directory.
    if (!dlc_present()) {
        shim_log("360 track v47 readers enabled; DLC asset hooks not needed (no DLCContent)");
        return;
    }
    start_selector_fix();
    start_shader_constants_fix();
    start_renderdoc_loader();
    start_particle_atlas_fix();
    start_counter_gate();
    start_page_clear_fix();
    const unsigned char decal_objects_push[5] = {0x68, 0x10, 0xcc, 0xad, 0x01};   // push 01ADCC10h
    shim_log(ga_80a56e && patch_call(GA(0x80a569), decal_objects_push, 5, (void*)&dlc_decal_objects_stub)
                 ? "dlc texture fix: DLC worlds use their own decal objects"
                 : "dlc texture fix: decal objects hook skipped (unexpected code or address unknown for this build)");
    const unsigned char object_index_push[5] = {0x68, 0xd4, 0xcd, 0xad, 0x01};
    if (game_file_exists("trials_revive_no_object_index.txt"))
        shim_log("dlc texture fix: per-world object index DISABLED by trials_revive_no_object_index.txt (test)");
    else
    shim_log(patch_call(GA(0x80e16c), object_index_push, 5, (void*)&dlc_object_index_stub)
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
    if (code_differs((uintptr_t)GET_BIKE_INFOS, GET_BIKE_INFOS_ORIG, sizeof GET_BIKE_INFOS_ORIG) == 0 &&
        patch_call(PACK_TRACKS_CALL, tracks_call, 5, (void*)&pack_tracks_and_note_bikes) &&
        patch_jmp(GET_BIKE_INFOS, GET_BIKE_INFOS_ORIG, sizeof GET_BIKE_INFOS_ORIG, (void*)&get_bike_infos_stub))
        shim_log("dlc fix: DLC packs' bikes.xml load on first bike lookup");
    else
        shim_log("dlc fix: DLC bikes patch skipped (unexpected code)");
    unsigned char collections_call[5] = {0xE8};
    *(int*)(collections_call + 1) = (int)(LOAD_PACK_COLLECTIONS - (PACK_COLLECTIONS_CALL + 5));
    shim_log(PACK_COLLECTIONS_CALL && LOAD_PACK_COLLECTIONS && CLASSIFY_CATEGORY && CATEGORY_SUBCATEGORIES &&
                     OBJECT_COLLECTION && DEFINITION_OF &&
                     patch_call(PACK_COLLECTIONS_CALL, collections_call, 5, (void*)&load_pack_collections_and_classify)
                 ? "dlc fix: DLC object definitions are classified when the packs load (custom tracks in base worlds)"
                 : "dlc fix: DLC object classification skipped (unexpected code or address unknown for this build)");
    unsigned char vt_call[5] = {0xE8};
    *(int*)(vt_call + 1) = (int)(VT_LOAD_INDICES - (WORLD_VT_LOAD_CALL + 5));
    shim_log(WORLD_VT_LOAD_CALL && VT_LOAD_INDICES && VT_RELOAD_ALL_1 && VT_RELOAD_ALL_2 && VIRTUAL_TEXTURE &&
                     patch_call(WORLD_VT_LOAD_CALL, vt_call, 5, (void*)&load_world_textures_fresh)
                 ? "dlc texture fix: changing world reloads the texture page cache"
                 : "dlc texture fix: page cache reload skipped (unexpected code or address unknown for this build)");
    unsigned char play_call[5] = {0xE8};
    *(int*)(play_call + 1) = (int)(PLAY_USER_TRACK - (PLAY_USER_TRACK_CALL + 5));
    unsigned char play_call_no_lb[5] = {0xE8};
    *(int*)(play_call_no_lb + 1) = (int)(PLAY_USER_TRACK - (PLAY_USER_TRACK_CALL_NO_LB + 5));
    shim_log(PLAY_USER_TRACK_CALL && PLAY_USER_TRACK_CALL_NO_LB && PLAY_USER_TRACK && USER_TRACK_INFO &&
                     LOAD_USER_TRACK_FILES && WORLD_LEAVE && TRACK_CLEAR && WORLD_LOAD && WORLD_SET_AREA &&
                     code_differs(PLAY_USER_TRACK_CALL, play_call, 5) == 0 &&
                     patch_call(PLAY_USER_TRACK_CALL_NO_LB, play_call_no_lb, 5, (void*)&play_user_track_in_its_world) &&
                     patch_call(PLAY_USER_TRACK_CALL, play_call, 5, (void*)&play_user_track_in_its_world)
                 ? "dlc fix: user tracks load in the world they were built in (DLC0 world byte)"
                 : "dlc fix: user track world selection skipped (unexpected code or address unknown for this build)");
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
