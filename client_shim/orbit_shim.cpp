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
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "bcrypt.lib")

namespace mg { namespace orbitclient {

class IGetLoginDetailsListener {
public:
    virtual void GetLoginDetailsListenerCallback(unsigned int requestUniqueId, const char* userName,
                                                 const char* password, const char* cdKey) = 0;
};

class OrbitClient {
public:
    void GetLoginDetails(unsigned int requestUniqueId, IGetLoginDetailsListener* listener);
};

}}  // namespace mg::orbitclient

using mg::orbitclient::IGetLoginDetailsListener;

void start_ticker_hook();   // ticker_hook.cpp
void start_gamelog_hook();  // gamelog_hook.cpp (opt-in)
void start_dlc_fixes();     // dlc_fixes.cpp (only with DLCContent)

static HMODULE g_self;
static char g_key[64];

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
        inner->GetLoginDetailsListenerCallback(requestUniqueId, userName, (const char*)login_key_wide(), login_key());
    }

private:
    IGetLoginDetailsListener* inner_;
};

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
    if (!real) {
        shim_log("ubiorbitapi_r2_loader_orig.dll is missing: online login is unavailable");
        return;   // the game then just stays offline
    }
    login_key();
    real(this, requestUniqueId, listener ? new LoginListener(listener) : nullptr);
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

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        use_revival_config_id();
        start_gamelog_hook();
        start_dlc_fixes();
        start_ticker_hook();
    }
    return TRUE;
}
