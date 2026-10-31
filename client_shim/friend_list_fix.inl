// Play Tracks (Track Central) can go blank until the game restarts (user report 2026-10-30: open
// Last Played, back out, repeat ~5 times, leave Play Tracks, open it again -> empty map, no tiles;
// leaderboards stopped too in some sessions).
// Every UserTrackFeedSelectMenu starts with `if (!GameCode.initFriendList()) return;` and only
// continues (getTrackCentralFeeds) from its friendListLoadFinished listener. The native binding
// "initFriendList" (0x6e5e60) answers true when the friends component has the list for the active
// user, otherwise it asks for a reload (0x482020) and answers false; the listener only fires from
// the friends component's completion callbacks (0x481e10 / 0x481e30). In the broken sessions the
// menu sent nothing after that point (no getFeeds, no server traffic), i.e. the reload never
// reported back. 74 menus use the same wait (multiplayer friends, race results, track search).
// Fix: the binding (registered at 0x7044e8 `push 0x6e5e60`) is wrapped so it always answers true;
// a reload it started still runs, and its late friendListLoadFinished only refreshes the menu.
// Each "not ready" is logged, to confirm the cause. Rollback: datapack\trials_revive_no_friend_list_fix.txt.

namespace {

typedef void(__cdecl* FlashBindingFn)(void** params);
FlashBindingFn g_init_friend_list_original;
volatile LONG g_friend_list_forced;

void __cdecl init_friend_list_ready(void** params) {
    g_init_friend_list_original(params);
    BYTE* result = params ? (BYTE*)params[0] : nullptr;   // Flash value: +0 type (1 = bool), +0x20 bool
    if (!result || *(int*)result != 1 || result[0x20]) return;
    result[0x20] = 1;
    LONG n = InterlockedIncrement(&g_friend_list_forced);
    if (n <= 50) {
        char line[96];
        sprintf_s(line, "friend list fix: list not ready (#%ld), menu told it is", n);
        shim_log(line);
    }
}

void start_friend_list_fix() {
    if (game_file_exists("trials_revive_no_friend_list_fix.txt")) {
        shim_log("friend list fix: DISABLED by trials_revive_no_friend_list_fix.txt");
        return;
    }
    const unsigned char push_binding[5] = {0x68, 0x60, 0x5e, 0x6e, 0x00};   // push 0x6e5e60
    uintptr_t at = GA(0x7044e8);
    DWORD old;
    if (!at || !GA(0x6e5e60) || code_differs(at, push_binding, 5) ||
        !VirtualProtect((void*)(at + 1), 4, PAGE_EXECUTE_READWRITE, &old)) {
        shim_log("friend list fix: skipped (unexpected code)");
        return;
    }
    g_init_friend_list_original = (FlashBindingFn)GA(0x6e5e60);
    *(DWORD*)(at + 1) = (DWORD)&init_friend_list_ready;
    VirtualProtect((void*)(at + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
    shim_log("friend list fix: initFriendList always answers ready (Play Tracks can't wait forever)");
}

}  // namespace
