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
#pragma comment(lib, "user32.lib")   // EnumWindows: find the game window's thread
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
// DLCEnumerateJob vtable: [1] run = DLCHandler::enumerate (0x654b80), [2] finish (0x653ae0 hand-off).
const DWORD DLC_JOB_VTABLE = 0x1753890, DLC_JOB_RUN = 0x66c480, DLC_JOB_FINISH = 0x66c490;
volatile LONG g_dlc_state;   // 0 = not checked, 1 = waiting for sign-in, 2 = started or not needed
bool g_dlc_from_frame;       // maybe_start_dlc called from dlc_frame (main loop, safe to run inline)

bool dlc_folder_present() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    while (n && path[n - 1] != '\\') n--;
    if (!n || n + 12 > MAX_PATH) return false;
    lstrcpyA(path + n, "DLCContent");
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// First call: is there DLC to load, and is this the game build the addresses below belong to?
// Nothing reads the game's globals before this has passed (a different build crashed reading them).
bool dlc_supported() {
    if (g_dlc_state == 0) {
        // Only the Steam build (loaded at 0x400000) has this code at this address; on other builds
        // the address may not even be mapped (Ubisoft Connect's loads at another base).
        bool code = false;
        if ((uintptr_t)GetModuleHandleA(nullptr) == 0x400000) {
            __try {
                code = memcmp((void*)DLC_JOB_START, DLC_JOB_START_CODE, sizeof DLC_JOB_START_CODE) == 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                code = false;
            }
        }
        bool ok = dlc_folder_present() && code;
        g_dlc_state = ok ? 1 : 2;
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)GetModuleHandleA(nullptr);
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const BYTE*)dos + dos->e_lfanew);
        char line[200];
        sprintf_s(line, "dlc: game build %08lx size %08lx, DLC loader %s, DLCContent %s",
            nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage, code ? "recognised" : "NOT recognised (other build)",
            dlc_folder_present() ? "present" : "absent");
        shim_log(line);
    }
    return g_dlc_state == 1;
}

// The game's readiness for loading DLC: signed in, event manager built, loading finished.
bool dlc_game_ready() {
    __try {
        char* platform = *(char**)0x1ee34cc;
        char* session = platform ? *(char**)(platform + 0x127c) : nullptr;
        char* game = *(char**)0x1ee3548;
        return session && *(unsigned char*)(session + 0x24) && game && *(char**)(game + 0x9c)
               && *(void**)(game + 0xa0) && *(volatile unsigned char*)0x1ee3541;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void maybe_start_dlc() {
    if (!dlc_supported()) return;
    char* platform = *(char**)0x1ee34cc;
    char* session = platform ? *(char**)(platform + 0x127c) : nullptr;
    char* game = *(char**)0x1ee3548;
    char* dlc = game ? *(char**)(game + 0x9c) : nullptr;
    if (!session || !dlc || !*(unsigned char*)(session + 0x24)) return;   // not signed in yet
    if (!*(void**)(game + 0xa0)) return;   // event manager (pack events register into it) not built yet
    // LoadThreadClass::run (0x436962) sets this when the game's loading is done. Before that the
    // init thread is still setting up the menu track's trigger logic; running the pack loader
    // (XML parsing, bike/sound loading) beside it once left a hash table it shares corrupted and
    // the init thread spinning in its insert (0x6040e0), a hang on the loading screen.
    if (!*(volatile unsigned char*)0x1ee3541) return;
    g_dlc_state = 2;
    // Queued, the job waited 9-60 s behind other jobs (Track Central enumeration, downloads); a
    // world map opened meanwhile was built without the DLC. When this runs from the per-frame
    // dlc_frame() below, run the job's own run/finish slots right here on the main thread instead,
    // so the packs are registered before the player can reach the map.
    if (g_dlc_from_frame && *(DWORD*)dlc == DLC_JOB_VTABLE && !*(unsigned char*)(dlc + 0x1c)
        && ((DWORD*)DLC_JOB_VTABLE)[1] == DLC_JOB_RUN && ((DWORD*)DLC_JOB_VTABLE)[2] == DLC_JOB_FINISH) {
        DWORD started = GetTickCount();
        *(unsigned char*)(dlc + 0x1c) = 1;                       // what start() marks: running
        ((JobStartFn)DLC_JOB_RUN)(dlc);                            // DLCHandler enumerate: registers packs
        ((JobStartFn)DLC_JOB_FINISH)(dlc);                         // clears the flag, hands over results
        char line[128];
        sprintf_s(line, "dlc: DLC packs registered on the main thread in %lu ms (thread %lu)",
            GetTickCount() - started, GetCurrentThreadId());
        shim_log(line);
        return;
    }
    ((JobStartFn)DLC_JOB_START)(dlc);
    char line[96];
    sprintf_s(line, "dlc: started DLC enumeration job (thread %lu)", GetCurrentThreadId());
    shim_log(line);
}

// Called by OrbitClient::Update, which the game calls from its main loop every frame. Once the
// main menu has asked for its text (g_dlc_menu_seen), registers the DLC from here: outside any
// localisation lookup, which the pack loader adds strings to.
volatile LONG g_dlc_menu_seen;
volatile DWORD g_dlc_menu_thread;      // thread that looked up the menu text (the game's main thread)
volatile LONG g_dlc_frames;            // per-frame calls seen
volatile DWORD g_dlc_menu_tick;        // when the menu text was first looked up
// The thread that owns the game's visible window: its main loop. OrbitClient::Update is also
// called from the loading thread; registering the DLC there once deadlocked against the main thread.
BOOL CALLBACK find_game_window(HWND hwnd, LPARAM out) {
    DWORD pid = 0;
    DWORD thread = GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) {
        *(DWORD*)out = thread;
        return FALSE;
    }
    return TRUE;
}

void dlc_frame_main() {
    static DWORD update_thread, window_thread;
    DWORD me = GetCurrentThreadId();
    if (!update_thread) {
        update_thread = me;
        char line[120];
        sprintf_s(line, "dlc: per-frame update first seen on thread %lu", me);
        shim_log(line);
    }
    g_dlc_frames++;
    if (g_dlc_state == 2) return;
    if (!window_thread) {
        if (g_dlc_frames % 30) return;                       // look for the window now and then
        EnumWindows(find_game_window, (LPARAM)&window_thread);
        if (!window_thread) return;
        char line[120];
        sprintf_s(line, "dlc: game window thread %lu", window_thread);
        shim_log(line);
    }
    if (me != window_thread) return;
    // The menu text hooks are not a reliable signal (a player can reach Single Player before the
    // ticker is ever asked for). maybe_start_dlc's own readiness checks (signed in, event manager
    // built, loading finished) are what made it safe; once they have held for ~30 of this thread's
    // frames in a row, register the DLC right here.
    static int ready_frames;
    if (!dlc_supported()) return;
    ready_frames = dlc_game_ready() ? ready_frames + 1 : 0;
    if (ready_frames < 30) return;
    g_dlc_from_frame = true;
    maybe_start_dlc();
    g_dlc_from_frame = false;
}
void* __fastcall get_text_hook(void* self, void* /*edx*/, const char* key) {
    // The main menu's ticker text is looked up on the game's main thread: from then on the DLC is
    // registered by dlc_frame_main. World names are also looked up while the map is built (possibly
    // on another thread); they only drive the fallback.
    if (key && strncmp(key, "TICKER_TEXT_", 12) == 0 && !g_dlc_menu_seen) {
        g_dlc_menu_thread = GetCurrentThreadId();
        g_dlc_menu_tick = GetTickCount();
        g_dlc_menu_seen = 1;
        char line[80];
        sprintf_s(line, "dlc: main menu reached (thread %lu)", GetCurrentThreadId());
        shim_log(line);
    }
    // Fallback (old path, queued job): the inline registration hasn't happened 3 s after the menu.
    if (key && (strncmp(key, "TICKER_TEXT_", 12) == 0 || strncmp(key, "WORLD_", 6) == 0)
        && g_dlc_menu_seen && GetTickCount() - g_dlc_menu_tick > 3000)
        maybe_start_dlc();
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

// Per-frame entry for OrbitClient::Update (orbit_shim.cpp).
void dlc_frame() {
    dlc_frame_main();
}
