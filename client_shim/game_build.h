// Which trialsFMX.exe is running, and where the game's code and data live in it.
//
// Every fix was written against the Steam build (fixed base 0x400000). The Ubisoft Connect
// build is the same source compiled a month earlier, loaded at a random base: GA() maps a Steam
// address to the same spot in the running build (identity on Steam, 0 if not known).
#pragma once
#include <windows.h>
#include <stdint.h>

enum GameBuild { BUILD_UNKNOWN, BUILD_STEAM, BUILD_UBISOFT };

GameBuild game_build();
const char* game_build_name();
bool game_build_known();

// Address in this process of the spot at Steam-build address `steam` (0 if this build lacks it).
uintptr_t GA(uintptr_t steam);

// Drop-in for memcmp((void*)at, expected, len) on game code, `at` already translated by GA():
// 0 when the code matches. On other builds, absolute game addresses inside `expected` are
// translated and relative call/jmp/jcc operands must reach the translated target.
int code_differs(uintptr_t at, const void* expected, size_t len);

// The running image's code and read-only data ranges (sanity checks on pointers).
uintptr_t game_text_start();
uintptr_t game_text_end();
uintptr_t game_rdata_start();
uintptr_t game_rdata_end();
