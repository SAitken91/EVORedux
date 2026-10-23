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
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>

void shim_log(const char* msg);

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
    shim_log(patch_call(AREA_COLLECTION_PUSH, AREA_COLLECTION_ORIG, 5, (void*)&area_collection_stub)
                 ? "dlc fix: area volumes use the pack's own collection"
                 : "dlc fix: area volume patch skipped (unexpected code)");
    shim_log(patch_call(TEXTURE_OBJECT_LOAD, TEXTURE_OBJECT_ORIG, 5, (void*)&texture_object_stub)
                 ? "dlc fix: texture reload checks the reused texture object"
                 : "dlc fix: texture reload patch skipped (unexpected code)");
    shim_log(patch_call(MATERIAL_TEXTURE_WAIT, MATERIAL_TEXTURE_ORIG, 5, (void*)&material_texture_wait_stub)
                 ? "dlc fix: material textures still loading are left alone, not freed"
                 : "dlc fix: material texture patch skipped (unexpected code)");
    unsigned char lb_call[5] = {0xE8};
    *(int*)(lb_call + 1) = (int)(LB_LOOKUP - (LB_LOOKUP_CALL + 5));
    shim_log(patch_call(LB_LOOKUP_CALL, lb_call, 5, (void*)&lb_lookup_checked)
                 ? "dlc fix: event menu leaderboard lookup has a fallback"
                 : "dlc fix: event menu patch skipped (unexpected code)");
}
