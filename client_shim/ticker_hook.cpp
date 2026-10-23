// Trials Evolution revival: occasional revival messages in the main-menu news ticker.
//
// The ticker's built-in lines are the localization keys TICKER_TEXT_1..4 ("You have %MEDALS gold
// medals" etc.), each fetched through the localization manager's vtable slot 0x1c. The server
// can't add new text (tms/ticker.xml can only reference keys that already exist in data_loc.pak),
// so this wraps that one lookup: now and then a TICKER_TEXT_* request returns a revival message
// instead. Every other string the game looks up is passed straight through.
//
// The messages come from http://<OnlineConfigHost>/revival/ticker.txt at startup, so they can be
// changed on the server without a new DLL:
//     chance=20                       percent of ticker lines replaced (0-100)
//     Any other non-empty line        one message; one is picked at random each time
// Without the file (or the server), the built-in default below is used.
//
// Addresses are from trialsFMX.exe (Steam, fixed base 0x400000) and are checked before anything
// is patched; any other build is left alone.
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "winhttp.lib")

void shim_log(const char* msg);

namespace {

const uintptr_t LOC_MANAGER_PTR = 0x1eee62c;     // global localization manager
const uintptr_t STRING_VTABLE = 0xd28f98;        // vtable of the game's string class
const uintptr_t TICKER_CALL_SITE = 0x69e93c;     // push "TICKER_TEXT_4" in the ticker builder
const char* const TICKER_KEY_4 = (const char*)0x18d91f8;
const int GET_TEXT_SLOT = 0x1c / 4;
const int VTABLE_COPY = 64;                      // slots copied; the manager uses far fewer

const char DEFAULT_MESSAGE[] = "Enjoying the Trials Evolution revival? Support it at ko-fi.com/StewYork91";
const int MAX_MESSAGES = 8;
const int MAX_LEN = 160;

// Layout of the game's string class as read by its copy routine (0x40b9f0):
// +6 u16 length, +8 char* data. Capacity and flags stay zero so it is never freed or resized.
#pragma pack(push, 1)
struct GameString {
    uintptr_t vtable;
    unsigned short capacity;
    unsigned short length;
    const char* data;
    unsigned short flags;
    unsigned short pad;
};
#pragma pack(pop)

char g_text[MAX_MESSAGES][MAX_LEN + 1];
GameString g_strings[MAX_MESSAGES];
int g_count;
volatile LONG g_chance = 20;
unsigned int g_rng;

typedef void*(__thiscall* GetTextFn)(void* self, const char* key);
GetTextFn g_original;
void* g_vtable[VTABLE_COPY];

unsigned int next_random() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

// __fastcall with an unused edx parameter has the same stack layout as __thiscall.
volatile LONG g_seen_ticker, g_seen_swap;

// DLC: the PC build contains the Xbox DLC loader (DLCHandler, scanning datapack\DLCContent\*),
// but nothing ever starts its enumerate job (DLCEnumerateJob::start at 0x66c450 has no callers;
// on Xbox it ran on content/sign-in notifications). Start it once, on the game's main thread
// (this hook runs there), after sign-in, and only if a DLCContent folder exists. It is started from
// the first main-menu ticker lookup: started during loading it crashed registering the pack events
// (null event table in 0x65dd90) because the game wasn't fully initialised yet.
const uintptr_t DLC_JOB_START = 0x66c450;
const unsigned char DLC_JOB_START_CODE[] = {0x8b, 0xc1, 0x80, 0x78, 0x1c, 0x00};   // mov eax,ecx; cmp byte [eax+1Ch],0
typedef void(__thiscall* JobStartFn)(void* job);
volatile LONG g_dlc_state;   // 0 = not checked, 1 = waiting for sign-in, 2 = started or not needed

bool dlc_folder_present() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    while (n && path[n - 1] != '\\') n--;
    if (!n || n + 12 > MAX_PATH) return false;
    lstrcpyA(path + n, "DLCContent");
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

void maybe_start_dlc() {
    if (g_dlc_state == 2) return;
    if (g_dlc_state == 0) {
        bool ok = dlc_folder_present() && memcmp((void*)DLC_JOB_START, DLC_JOB_START_CODE, sizeof DLC_JOB_START_CODE) == 0;
        g_dlc_state = ok ? 1 : 2;
        if (!ok) return;
    }
    char* platform = *(char**)0x1ee34cc;
    char* session = platform ? *(char**)(platform + 0x127c) : nullptr;
    char* game = *(char**)0x1ee3548;
    char* dlc = game ? *(char**)(game + 0x9c) : nullptr;
    if (!session || !dlc || !*(unsigned char*)(session + 0x24)) return;   // not signed in yet
    if (!*(void**)(game + 0xa0)) return;   // event manager (pack events register into it) not built yet
    g_dlc_state = 2;
    ((JobStartFn)DLC_JOB_START)(dlc);
    shim_log("dlc: started DLC enumeration job");
}

void* __fastcall get_text_hook(void* self, void* /*edx*/, const char* key) {
    // Main menu is up (ticker) or the world map is being built (world names): safe to load DLC.
    if (key && (strncmp(key, "TICKER_TEXT_", 12) == 0 || strncmp(key, "WORLD_", 6) == 0)) maybe_start_dlc();
    if (g_count && key && strncmp(key, "TICKER_TEXT_", 12) == 0) {
        if (!InterlockedExchange(&g_seen_ticker, 1)) shim_log("first ticker line requested");
        if ((int)(next_random() % 100) < g_chance) {
            if (!InterlockedExchange(&g_seen_swap, 1)) shim_log("first ticker line replaced");
            return &g_strings[next_random() % g_count];
        }
    }
    return g_original(self, key);
}

void add_message(const char* line, int len) {
    if (g_count >= MAX_MESSAGES || len <= 0) return;
    int n = 0;
    for (int i = 0; i < len && n < MAX_LEN; i++) {
        unsigned char c = (unsigned char)line[i];
        if (c >= 32 && c < 127 && c != '%') g_text[g_count][n++] = (char)c;   // '%' would be a placeholder
    }
    if (!n) return;
    g_text[g_count][n] = 0;
    g_strings[g_count] = {STRING_VTABLE, (unsigned short)n, (unsigned short)n, g_text[g_count], 0, 0};
    g_count++;
}

void parse_config(const char* body) {
    const char* p = body;
    while (*p) {
        const char* end = p;
        while (*end && *end != '\n' && *end != '\r') end++;
        int len = (int)(end - p);
        if (len > 7 && strncmp(p, "chance=", 7) == 0) {
            int v = atoi(p + 7);
            g_chance = v < 0 ? 0 : v > 100 ? 100 : v;
        } else if (len && p[0] != '#') {
            add_message(p, len);
        }
        p = *end ? end + 1 : end;
    }
}

// Reads the server host from OnlineConfigHost.txt next to the game executable.
bool read_host(wchar_t* host, int size) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    while (n && path[n - 1] != '\\') n--;
    if (!n || n + 24 > MAX_PATH) return false;
    lstrcpyA(path + n, "OnlineConfigHost.txt");
    FILE* f = nullptr;
    char buf[128] = {};
    if (fopen_s(&f, path, "r") || !f) return false;
    fgets(buf, sizeof buf, f);
    fclose(f);
    int len = 0;
    while (buf[len] && buf[len] != '\r' && buf[len] != '\n' && buf[len] != ' ' && len < size - 1) {
        host[len] = (wchar_t)(unsigned char)buf[len];
        len++;
    }
    host[len] = 0;
    return len > 0;
}

void fetch_config() {
    wchar_t host[128];
    if (!read_host(host, 128)) return;
    HINTERNET session = WinHttpOpen(L"TrialsRevival/1", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    if (!session) return;
    WinHttpSetTimeouts(session, 3000, 3000, 3000, 3000);
    HINTERNET connect = WinHttpConnect(session, host, 80, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", L"/revival/ticker.txt", nullptr,
                                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    char body[4096];
    DWORD total = 0, status = 0, size = sizeof status;
    if (request && WinHttpSendRequest(request, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)
        && WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr)
        && status == 200) {
        DWORD got = 0;
        while (total < sizeof body - 1 && WinHttpReadData(request, body + total, sizeof body - 1 - total, &got) && got)
            total += got;
    }
    body[total] = 0;
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (total) {
        parse_config(body);
        shim_log(g_count ? "ticker messages loaded from the server" : "server ticker list is empty: ticker unchanged");
    }
}

bool is_supported_build() {
    if ((uintptr_t)GetModuleHandleA(nullptr) != 0x400000) return false;
    const unsigned char expected[] = {0x68, 0xf8, 0x91, 0x8d, 0x01};   // push TICKER_KEY_4
    __try {
        return memcmp((const void*)TICKER_CALL_SITE, expected, sizeof expected) == 0
            && strcmp(TICKER_KEY_4, "TICKER_TEXT_4") == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

DWORD WINAPI ticker_thread(LPVOID) {
    if (!is_supported_build()) {
        shim_log("unknown game build: ticker messages disabled");
        return 0;
    }
    g_rng = GetTickCount() | 1;
    fetch_config();
    if (!g_count && g_chance) add_message(DEFAULT_MESSAGE, (int)strlen(DEFAULT_MESSAGE));
    if (!g_chance) return 0;

    // Give the localization manager a copy of its vtable with slot 0x1c pointing at the hook.
    // Polled because the manager is created after this DLL loads, and its vtable pointer can be
    // reset afterwards (re-initialisation in place), so re-apply whenever it isn't ours.
    int installs = 0;
    for (;;) {
        void** manager = *(void***)LOC_MANAGER_PTR;
        if (manager && *manager != g_vtable) {
            void** original = (void**)*manager;
            if (original[GET_TEXT_SLOT] != (void*)&get_text_hook) {
                memcpy(g_vtable, original, sizeof g_vtable);
                g_original = (GetTextFn)original[GET_TEXT_SLOT];
                g_vtable[GET_TEXT_SLOT] = (void*)&get_text_hook;
            }
            InterlockedExchangePointer((PVOID*)manager, g_vtable);
            if (++installs <= 3) shim_log(installs == 1 ? "ticker hook installed" : "ticker hook re-applied");
        }
        Sleep(500);
    }
}

}  // namespace

void start_ticker_hook() {
    HANDLE t = CreateThread(nullptr, 0, ticker_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}
