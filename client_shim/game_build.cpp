#include "game_build.h"
#include <stdio.h>
#include <string.h>

void shim_log(const char* msg);

#include "game_addresses_ubi.inc"

namespace {

const DWORD STEAM_TIMESTAMP = 0x529D7E1B, STEAM_IMAGE_SIZE = 0x01D43000;
const DWORD UBISOFT_TIMESTAMP = 0x52785567, UBISOFT_IMAGE_SIZE = 0x01C6E000;
const uintptr_t PREFERRED_BASE = 0x400000;
const size_t UBI_COUNT = sizeof UBI_ADDRESSES / sizeof UBI_ADDRESSES[0];

uintptr_t image_base() { return (uintptr_t)GetModuleHandleA(nullptr); }

const IMAGE_NT_HEADERS* nt_headers() {
    const BYTE* base = (const BYTE*)image_base();
    return (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
}

GameBuild detect() {
    const IMAGE_NT_HEADERS* nt = nt_headers();
    DWORD stamp = nt->FileHeader.TimeDateStamp, size = nt->OptionalHeader.SizeOfImage;
    if (stamp == STEAM_TIMESTAMP && size == STEAM_IMAGE_SIZE && image_base() == PREFERRED_BASE)
        return BUILD_STEAM;
    if (stamp == UBISOFT_TIMESTAMP && size == UBISOFT_IMAGE_SIZE) return BUILD_UBISOFT;
    return BUILD_UNKNOWN;
}

// Steam address of the running build's address `at` (inverse of GA), or 0.
uintptr_t steam_of(uintptr_t at) {
    if (game_build() == BUILD_STEAM) return at;
    if (game_build() != BUILD_UBISOFT) return 0;
    uintptr_t preferred = at - image_base() + PREFERRED_BASE;
    for (size_t i = 0; i < UBI_COUNT; ++i)
        if (UBI_ADDRESSES[i][1] == preferred) return UBI_ADDRESSES[i][0];
    return 0;
}

bool is_steam_game_address(DWORD v) { return v >= 0x401000 && v < 0x2000000; }

void section_range(const char* name, uintptr_t* start, uintptr_t* end) {
    const IMAGE_NT_HEADERS* nt = nt_headers();
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
        if (strncmp((const char*)s->Name, name, 8) == 0) {
            *start = image_base() + s->VirtualAddress;
            *end = *start + s->Misc.VirtualSize;
            return;
        }
    *start = *end = 0;
}

int compare_translated(uintptr_t at, const BYTE* expected, size_t len) {
    uintptr_t steam_at = steam_of(at);
    const BYTE* code = (const BYTE*)at;
    for (size_t i = 0; i < len;) {
        // Relative call/jmp (E8/E9 rel32) or jcc (0F 8x rel32): must reach the translated target.
        size_t op = expected[i] == 0xE8 || expected[i] == 0xE9 ? 1
                  : expected[i] == 0x0F && i + 1 < len && (expected[i + 1] & 0xF0) == 0x80 ? 2 : 0;
        if (op && i + op + 4 <= len && steam_at) {
            if (memcmp(code + i, expected + i, op)) return 1;
            int steam_rel = *(const int*)(expected + i + op);
            uintptr_t target = GA(steam_at + i + op + 4 + steam_rel);
            if (target && (uintptr_t)(at + i + op + 4 + *(const int*)(code + i + op)) != target) return 1;
            i += op + 4;          // unknown target: the surrounding bytes decide
            continue;
        }
        if (i + 4 <= len) {
            DWORD v = *(const DWORD*)(expected + i);
            if (is_steam_game_address(v)) {
                uintptr_t t = GA(v);
                if (t) {
                    if (*(const DWORD*)(code + i) != (DWORD)t) return 1;
                    i += 4;
                    continue;
                }
            }
        }
        if (code[i] != expected[i]) return 1;
        ++i;
    }
    return 0;
}

}  // namespace

GameBuild game_build() {
    static GameBuild build = detect();
    return build;
}

const char* game_build_name() {
    switch (game_build()) {
    case BUILD_STEAM: return "Steam";
    case BUILD_UBISOFT: return "Ubisoft Connect";
    default: return "unknown";
    }
}

bool game_build_known() { return game_build() != BUILD_UNKNOWN; }

uintptr_t GA(uintptr_t steam) {
    switch (game_build()) {
    case BUILD_STEAM:
        return steam;
    case BUILD_UBISOFT: {
        size_t lo = 0, hi = UBI_COUNT;               // table is sorted by Steam address
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (UBI_ADDRESSES[mid][0] < steam) lo = mid + 1;
            else hi = mid;
        }
        if (lo < UBI_COUNT && UBI_ADDRESSES[lo][0] == steam)
            return UBI_ADDRESSES[lo][1] - PREFERRED_BASE + image_base();
        static volatile LONG reported;
        if (InterlockedIncrement(&reported) <= 20) {
            char line[96];
            sprintf_s(line, "game build: no Ubisoft Connect address for %08lx", (unsigned long)steam);
            shim_log(line);
        }
        return 0;
    }
    default:
        return 0;
    }
}

int code_differs(uintptr_t at, const void* expected, size_t len) {
    if (!at || game_build() == BUILD_UNKNOWN) return 1;
    __try {
        if (game_build() == BUILD_STEAM) return memcmp((const void*)at, expected, len);
        return compare_translated(at, (const BYTE*)expected, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

// Steam: the exact ranges the fixes were written with. Other builds: from the section table.
uintptr_t game_text_start() {
    if (game_build() == BUILD_STEAM) return 0x401000;
    uintptr_t s, e; section_range(".text", &s, &e); return s;
}
uintptr_t game_text_end() {
    if (game_build() == BUILD_STEAM) return 0xd28000;
    uintptr_t s, e; section_range(".text", &s, &e); return e;
}
uintptr_t game_rdata_start() {
    if (game_build() == BUILD_STEAM) return 0xd28000;
    uintptr_t s, e; section_range(".rdata", &s, &e); return s;
}
uintptr_t game_rdata_end() {
    if (game_build() == BUILD_STEAM) return 0x1d5c000;
    // Everything from .rdata up to the end of the initialised data (vtables, strings, tables).
    uintptr_t s, e; section_range(".data", &s, &e); return e;
}
