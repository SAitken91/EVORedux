// Trials Evolution revival: login shim for ubiorbitapi_r2_loader.dll.
//
// The game asks Ubisoft Connect for the player's login with OrbitClient::GetLoginDetails and
// sends the password it gets back to the online server. The game's own traffic encryption uses a
// public key, so that password is readable by anyone on the network path.
//
// This DLL sits in front of the real loader (renamed ubiorbitapi_r2_loader_orig.dll). Every
// export is forwarded unchanged (see orbit_shim.def), so Ubisoft Connect still starts, checks
// ownership and handles saves. Only GetLoginDetails is wrapped: the username is passed through,
// and both the password and the CD key are replaced with a random per-install key kept in
// trials_revive_login.key next to the game. Neither real value reaches the game.
//
// Callback layout verified in trialsFMX.exe: the listener's only vtable slot (0x739fc0) is a
// __thiscall with 4 stack args (`ret 0x10`) that calls MPGameCallbacks::Login(user, pwd, cdkey)
// (0x6886f0) with args 2, 3 and 4, i.e. (requestId, userName, password, cdKey). An earlier version
// assumed (requestId, accountId, userName, password) and replaced only the CD key: the real
// password was still sent. The password argument is UTF-16 (the callback converts it to UTF-8),
// so the key is passed to the game as a wide string; until 2026-10-23 it was passed narrow, which
// the game read as mangled UTF-16 (still derived from the key, never the real password).
//
// ticker_hook.cpp adds occasional revival messages to the main-menu news ticker.
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")

namespace mg { namespace orbitclient {

class IGetLoginDetailsListener {
public:
    virtual void GetLoginDetailsListenerCallback(unsigned int requestUniqueId, const char* userName,
                                                 const char* password, const char* cdKey) = 0;
};

class OrbitClient {
public:
    void GetLoginDetails(unsigned int requestUniqueId, IGetLoginDetailsListener* listener);
    bool StartLauncher(unsigned int productId, unsigned int flags, const char* language, const char* args);
    void Update();
};

}}  // namespace mg::orbitclient

using mg::orbitclient::IGetLoginDetailsListener;

void start_ticker_hook();   // ticker_hook.cpp
void start_gamelog_hook();  // gamelog_hook.cpp (opt-in)
void start_dlc_fixes();     // dlc_fixes.cpp (only with DLCContent)

static HMODULE g_self;
static char g_key[64];

// Development aid: a second copy of the game on the same PC can log in as a separate test player.
// With TRIALS_REVIVE_PROFILE=<n> set in its environment it uses trials_revive_login_<n>.key and the
// account name "<name>_P<n>". Unset (the normal case) nothing changes.
static const char* test_profile() {
    static char profile[8];
    static bool read;
    if (!read) {
        read = true;
        DWORD n = GetEnvironmentVariableA("TRIALS_REVIVE_PROFILE", profile, sizeof profile);
        if (n == 0 || n >= sizeof profile) profile[0] = 0;
        if (!profile[0]) {   // or a trials_revive_profile.txt next to the game (a separate test install)
            char path[MAX_PATH];
            DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
            while (len && path[len - 1] != '\\') len--;
            FILE* f = nullptr;
            if (len && len + 32 < MAX_PATH) {
                lstrcpyA(path + len, "trials_revive_profile.txt");
                if (fopen_s(&f, path, "r") == 0 && f) {
                    if (!fgets(profile, sizeof profile, f)) profile[0] = 0;
                    fclose(f);
                    for (char* p = profile; *p; p++)
                        if (*p == '\r' || *p == '\n' || *p == ' ') *p = 0;
                }
            }
        }
        for (char* p = profile; *p; p++)
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) profile[0] = 0;
    }
    return profile[0] ? profile : nullptr;
}

void shim_log(const char* msg) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(g_self, path, MAX_PATH);
    while (n && path[n - 1] != '\\') n--;
    if (!n || n + 32 > MAX_PATH) return;
    lstrcpyA(path + n, "trials_revive_shim.log");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") == 0 && f) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, msg);
        fclose(f);
    }
}

// Loads the per-install login key, creating it on first use. It is not a secret that protects
// anything outside this game: it only stands in for the password the game would otherwise send.
static const char* login_key() {
    if (g_key[0]) return g_key;
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(g_self, path, MAX_PATH);
    while (n && path[n - 1] != '\\') n--;
    if (!n || n + 32 > MAX_PATH) n = 0;
    if (const char* profile = test_profile())
        sprintf_s(path + n, MAX_PATH - n, "trials_revive_login_%s.key", profile);
    else
        lstrcpyA(path + n, "trials_revive_login.key");

    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") == 0 && f) {
        if (fgets(g_key, sizeof g_key, f)) {
            for (char* p = g_key; *p; p++)
                if (*p == '\r' || *p == '\n') *p = 0;
        }
        fclose(f);
    }
    if (lstrlenA(g_key) < 16) {
        static const char alphabet[] = "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        unsigned char rnd[24];
        if (BCryptGenRandom(nullptr, rnd, sizeof rnd, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            shim_log("could not generate a login key");
            lstrcpyA(g_key, "trials-revive-no-key");
            return g_key;
        }
        lstrcpyA(g_key, "trv1-");
        for (int i = 0; i < 24; i++) g_key[5 + i] = alphabet[rnd[i] % (sizeof alphabet - 1)];
        g_key[29] = 0;
        if (fopen_s(&f, path, "w") == 0 && f) {
            fprintf(f, "%s\n", g_key);
            fclose(f);
            shim_log("created a new login key");
        }
    }
    return g_key;
}

static const wchar_t* login_key_wide() {
    static wchar_t wide[64];
    const char* key = login_key();
    int i = 0;
    for (; key[i] && i < 63; i++) wide[i] = (wchar_t)(unsigned char)key[i];   // the key is ASCII
    wide[i] = 0;
    return wide;
}

class LoginListener : public IGetLoginDetailsListener {
public:
    explicit LoginListener(IGetLoginDetailsListener* inner) : inner_(inner) {}

    void GetLoginDetailsListenerCallback(unsigned int requestUniqueId, const char* userName,
                                         const char* /*password: discarded*/, const char* /*cdKey: discarded*/) override {
        shim_log("login details passed to the game with password and CD key replaced");
        IGetLoginDetailsListener* inner = inner_;
        delete this;
        // The password argument is a UTF-16 string (the callback converts it with mode 2 =
        // UTF-16 -> UTF-8 at 0x73a03b); the user name and CD key are narrow strings.
        char profile_name[96];
        if (const char* profile = test_profile()) {
            sprintf_s(profile_name, "%s_P%s", userName ? userName : "Player", profile);
            userName = profile_name;
            shim_log("test profile: logging in as a separate player");
        }
        inner->GetLoginDetailsListenerCallback(requestUniqueId, userName, (const char*)login_key_wide(), login_key());
    }

private:
    IGetLoginDetailsListener* inner_;
};

static LoginListener* g_pending_login;   // test profile: answered from the next Update()
static unsigned int g_pending_login_id;

typedef void(__thiscall* GetLoginDetailsFn)(void* self, unsigned int requestUniqueId, IGetLoginDetailsListener* listener);

void mg::orbitclient::OrbitClient::GetLoginDetails(unsigned int requestUniqueId, IGetLoginDetailsListener* listener) {
    static GetLoginDetailsFn real = nullptr;
    if (!real) {
        HMODULE orig = GetModuleHandleA("ubiorbitapi_r2_loader_orig.dll");
        if (!orig) orig = LoadLibraryA("ubiorbitapi_r2_loader_orig.dll");
        if (orig)
            real = (GetLoginDetailsFn)GetProcAddress(
                orig, "?GetLoginDetails@OrbitClient@orbitclient@mg@@QAEXIPAVIGetLoginDetailsListener@23@@Z");
    }
    if (test_profile() && listener) {
        // A test copy is started outside Ubisoft Connect (see StartLauncher), which has no login
        // to give it: log it in as a separate test player. Like the real client, answer from the
        // next Update() call: answering here, during startup, crashes the game (0x728b3f).
        shim_log("test profile: logging in without Ubisoft Connect");
        g_pending_login = new LoginListener(listener);
        g_pending_login_id = requestUniqueId;
        return;
    }
    if (!real) {
        shim_log("ubiorbitapi_r2_loader_orig.dll is missing: online login is unavailable");
        return;   // the game then just stays offline
    }
    login_key();
    real(this, requestUniqueId, listener ? new LoginListener(listener) : nullptr);
}

typedef void(__thiscall* UpdateFn)(void* self);
void dlc_frame();   // ticker_hook.cpp

void mg::orbitclient::OrbitClient::Update() {
    if (LoginListener* pending = g_pending_login) {
        g_pending_login = nullptr;
        pending->GetLoginDetailsListenerCallback(g_pending_login_id, "Tester", "", "");
    }
    static UpdateFn real = nullptr;
    if (!real) {
        HMODULE orig = GetModuleHandleA("ubiorbitapi_r2_loader_orig.dll");
        if (!orig) orig = LoadLibraryA("ubiorbitapi_r2_loader_orig.dll");
        if (orig) real = (UpdateFn)GetProcAddress(orig, "?Update@OrbitClient@orbitclient@mg@@QAEXXZ");
    }
    if (real) real(this);
    dlc_frame();   // ticker_hook.cpp: registers the DLC packs on the main thread once the menu is up
}

// At startup the game calls StartLauncher (0x73a2e5); when it returns true the game exits so that
// Ubisoft Connect can start it. A second copy would be handed to Ubisoft Connect, which just shows
// the running one. Test-profile copies skip the hand-off and keep running.
typedef bool(__thiscall* StartLauncherFn)(void* self, unsigned int productId, unsigned int flags, const char* language, const char* args);

bool mg::orbitclient::OrbitClient::StartLauncher(unsigned int productId, unsigned int flags, const char* language, const char* args) {
    if (test_profile()) {
        shim_log("test profile: not handing the game over to Ubisoft Connect");
        return false;
    }
    static StartLauncherFn real = nullptr;
    if (!real) {
        HMODULE orig = GetModuleHandleA("ubiorbitapi_r2_loader_orig.dll");
        if (!orig) orig = LoadLibraryA("ubiorbitapi_r2_loader_orig.dll");
        if (orig)
            real = (StartLauncherFn)GetProcAddress(orig, "?StartLauncher@OrbitClient@orbitclient@mg@@QAE_NIIPBD0@Z");
    }
    return real ? real(this, productId, flags, language, args) : false;
}

// Server gate: the game fetches its online config from the host in OnlineConfigHost.txt with a
// fixed onlineConfigID (string at 0x1a910f8, read once at 0x72a3e6). The revival server only
// answers a revival-specific ID, which this DLL puts in place of Ubisoft's. A game started
// without this DLL asks for Ubisoft's ID, gets "not found", stays offline exactly as with the
// original dead servers, and so never reaches the login step that would send the real password.
static const uintptr_t ONLINE_CONFIG_ID = 0x1a910f8;
static const char UBISOFT_CONFIG_ID[] = "e940a83f3cc84074a9933a0f360a0b69";
static const char REVIVAL_CONFIG_ID[] = "c5855ba01ddd954b564be2c8a4ab86e4";   // must match server.py

static bool replace_config_id_at(char* id) {
    DWORD old;
    if (!VirtualProtect(id, sizeof REVIVAL_CONFIG_ID, PAGE_READWRITE, &old)) return false;
    memcpy(id, REVIVAL_CONFIG_ID, sizeof REVIVAL_CONFIG_ID);
    VirtualProtect(id, sizeof REVIVAL_CONFIG_ID, old, &old);
    return true;
}

// Other game builds (the Ubisoft Connect copy of the game is a different exe than Steam's) keep
// the same ID string somewhere else: look for it anywhere in the game's own image.
static int replace_config_id_in_image() {
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    BYTE* end = base + nt->OptionalHeader.SizeOfImage;
    int replaced = 0;
    for (BYTE* p = base; p < end;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof mbi)) break;
        BYTE* region_end = min((BYTE*)mbi.BaseAddress + mbi.RegionSize, end);
        bool can_read = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                        (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        if (can_read) {
            for (BYTE* q = p; q + sizeof UBISOFT_CONFIG_ID <= region_end; ++q) {
                if (*q == 'e' && memcmp(q, UBISOFT_CONFIG_ID, sizeof UBISOFT_CONFIG_ID) == 0 &&
                    replace_config_id_at((char*)q))
                    ++replaced;
            }
        }
        p = region_end;
    }
    return replaced;
}

static DWORD WINAPI retry_config_id(LPVOID) {
    for (int i = 0; i < 400; ++i) {          // up to 20 s, for builds that unpack after loading us
        Sleep(50);
        if (int n = replace_config_id_in_image()) {
            char line[120];
            sprintf_s(line, "online config id replaced in game image (%d place%s, after %d ms)", n, n == 1 ? "" : "s", (i + 1) * 50);
            shim_log(line);
            return 0;
        }
    }
    shim_log("online config id NOT found in this game build: the revival server will refuse it (send this log)");
    return 0;
}

static void use_revival_config_id() {
    static_assert(sizeof UBISOFT_CONFIG_ID == sizeof REVIVAL_CONFIG_ID, "same length");
    char* id = (char*)ONLINE_CONFIG_ID;
    if ((uintptr_t)GetModuleHandleA(nullptr) == 0x400000 && memcmp(id, UBISOFT_CONFIG_ID, sizeof UBISOFT_CONFIG_ID) == 0) {
        replace_config_id_at(id);                // the Steam build
        return;
    }
    if (int n = replace_config_id_in_image()) {
        char line[96];
        sprintf_s(line, "online config id replaced in game image (%d place%s)", n, n == 1 ? "" : "s");
        shim_log(line);
        return;
    }
    shim_log("online config id not in the game image yet; retrying in the background");
    HANDLE t = CreateThread(nullptr, 0, retry_config_id, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

// Multiplayer crash: entering Multiplayer > Online, the party code at 0x73a730 calls
// UPLAY_PARTY_GetFullMemberList(&list) and walks list {count, members[]} without checking the
// result. With Ubisoft's party service gone, Ubisoft Connect hands back a nonzero count with
// null members, and the game reads member 0 through a null pointer (0x73a7ba). Clear the list
// first and pass on only a list whose members are all readable; otherwise report no party.
static const uintptr_t PARTY_MEMBER_LIST_IAT = 0xd289e8;   // uplay_r1_loader!UPLAY_PARTY_GetFullMemberList
typedef int(__cdecl* PartyMemberListFn)(DWORD* list);
static PartyMemberListFn g_party_member_list;

static bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return (const char*)p + n <= (const char*)mbi.BaseAddress + mbi.RegionSize;
}

static int __cdecl safe_party_member_list(DWORD* list) {
    list[0] = list[1] = 0;
    int result = g_party_member_list(list);
    DWORD count = list[0];
    const DWORD* members = (const DWORD*)list[1];
    bool ok = count <= 16 && (count == 0 || readable(members, count * sizeof(DWORD)));
    for (DWORD i = 0; ok && i < count; i++)
        ok = readable((const void*)members[i], 8);
    if (!ok) {
        static LONG logged;
        if (!InterlockedExchange(&logged, 1)) shim_log("party: Ubisoft Connect returned an unusable member list, treating it as no party");
        list[0] = list[1] = 0;
    }
    return result;
}

static void guard_party_member_list() {
    void** slot = (void**)PARTY_MEMBER_LIST_IAT;
    HMODULE uplay = GetModuleHandleA("uplay_r1_loader.dll");
    if ((uintptr_t)GetModuleHandleA(nullptr) != 0x400000 || !uplay ||
        *slot != (void*)GetProcAddress(uplay, "UPLAY_PARTY_GetFullMemberList")) {
        shim_log("unknown game build: multiplayer party fix not applied");
        return;
    }
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return;
    g_party_member_list = (PartyMemberListFn)*slot;
    *slot = (void*)safe_party_member_list;
    VirtualProtect(slot, sizeof *slot, old, &old);
    shim_log("multiplayer fix: party member list is checked");
}

// Test profile only: the game refuses to start twice (it looks for an existing "TrialsPCMainWnd"
// window, brings it to the front and exits). A test-profile copy doesn't see that window.
static const uintptr_t FIND_WINDOW_A_IAT = 0xd28684;   // USER32!FindWindowA
typedef HWND(WINAPI* FindWindowAFn)(LPCSTR cls, LPCSTR title);
static FindWindowAFn g_find_window;

static HWND WINAPI test_profile_find_window(LPCSTR cls, LPCSTR title) {
    if (cls && !IS_INTRESOURCE(cls) && lstrcmpA(cls, "TrialsPCMainWnd") == 0) return nullptr;
    return g_find_window(cls, title);
}

// Test profile only: two copies on one PC would both open the game port (UDP 3050) and the Quazal
// port (3074); Windows lets them share it, and a joining copy then sees the host's address as its
// own and refuses to connect. A test-profile copy moves to 3060 / 3084 instead.
static const uintptr_t WSOCK_BIND_IAT = 0xd28778;   // WSOCK32!bind (ordinal 2)
typedef int(WINAPI* BindFn)(SOCKET s, const sockaddr* name, int namelen);
static BindFn g_bind;

static int WINAPI test_profile_bind(SOCKET s, const sockaddr* name, int namelen) {
    if (name && namelen >= (int)sizeof(sockaddr_in) && name->sa_family == AF_INET) {
        sockaddr_in moved = *(const sockaddr_in*)name;
        unsigned short port = ntohs(moved.sin_port);
        if (port == 3050 || port == 3074) {
            moved.sin_port = htons(port + 10);
            return g_bind(s, (const sockaddr*)&moved, sizeof moved);
        }
    }
    return g_bind(s, name, namelen);
}

static void allow_second_copy() {
    if (!test_profile()) return;
    void** bind_slot = (void**)WSOCK_BIND_IAT;
    HMODULE wsock = GetModuleHandleA("wsock32.dll");
    DWORD old_bind;
    if (wsock && *bind_slot == (void*)GetProcAddress(wsock, MAKEINTRESOURCEA(2)) &&
        VirtualProtect(bind_slot, sizeof *bind_slot, PAGE_READWRITE, &old_bind)) {
        g_bind = (BindFn)*bind_slot;
        *bind_slot = (void*)test_profile_bind;
        VirtualProtect(bind_slot, sizeof *bind_slot, old_bind, &old_bind);
        shim_log("test profile: game ports moved to 3060 / 3084");
    }
    void** slot = (void**)FIND_WINDOW_A_IAT;
    if ((uintptr_t)GetModuleHandleA(nullptr) != 0x400000 ||
        *slot != (void*)GetProcAddress(GetModuleHandleA("user32.dll"), "FindWindowA"))
        return;
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return;
    g_find_window = (FindWindowAFn)*slot;
    *slot = (void*)test_profile_find_window;
    VirtualProtect(slot, sizeof *slot, old, &old);
    shim_log("test profile: running alongside another copy");
}

// Diagnostic for game builds the fixes don't recognise (e.g. Ubisoft Connect's): logs where the
// image is loaded and the bytes at a few functions' Steam-build offsets, so it can be told whether
// the build is the same code at another address or a different compile. Waits for the code to
// appear first (a DRM-wrapped image is unpacked after start). Reads only; changes nothing.
static DWORD WINAPI build_probe(LPVOID) {
    static const DWORD rvas[] = {0x26c450, 0x254b80, 0x217810, 0x7c510, 0x89ce0, 0x22ef10};
    static const char* names[] = {"dlc job start", "dlc enumerate", "transform exec", "tick logic",
                                  "set transform", "random value"};
    const BYTE* base = (const BYTE*)GetModuleHandleA(nullptr);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    if ((uintptr_t)base == 0x400000 && memcmp((void*)0x66c450, "\x8b\xc1\x80\x78\x1c\x00", 6) == 0)
        return 0;                                               // the Steam build: nothing to report
    for (int i = 0; i < 60; ++i) {                              // up to 60 s for an unpacking image
        bool code = false;
        __try { code = *(const DWORD*)(base + rvas[0]) != 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (code) break;
        Sleep(1000);
    }
    char line[300];
    sprintf_s(line, "build probe: base %p, image size %08lx, timestamp %08lx, checksum %08lx, entry %08lx",
        base, nt->OptionalHeader.SizeOfImage, nt->FileHeader.TimeDateStamp, nt->OptionalHeader.CheckSum,
        nt->OptionalHeader.AddressOfEntryPoint);
    shim_log(line);
    for (int k = 0; k < 6; ++k) {
        int n = sprintf_s(line, "build probe: %-14s rva %06lx:", names[k], rvas[k]);
        for (int j = 0; j < 32 && n > 0; ++j) {
            int v = -1;
            __try { v = base[rvas[k] + j]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            n += v < 0 ? sprintf_s(line + n, sizeof line - n, " ??") : sprintf_s(line + n, sizeof line - n, " %02x", v);
        }
        shim_log(line);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        use_revival_config_id();
        guard_party_member_list();
        allow_second_copy();
        start_gamelog_hook();
        start_dlc_fixes();
        start_ticker_hook();
        if (HANDLE t = CreateThread(nullptr, 0, build_probe, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
