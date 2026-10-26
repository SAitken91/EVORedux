// Trials Evolution revival: optional capture of the engine's own debug log (development aid).
//
// The engine routes its diagnostics ("Found marketplace item", "DLC mount failed", ...) through
// one logger at 0x42a5b0: cdecl log(logger, 1, 1, level, file, function, line, fmt, ...). Retail
// builds never show them. When datapack\trials_revive_gamelog.enable exists, this patches a jump
// at the logger's entry, appends each formatted line to trials_revive_gamelog.txt, then runs the
// original logger unchanged. Without the .enable file nothing is patched.
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <share.h>
#include <tlhelp32.h>

void shim_log(const char* msg);

namespace {

const uintptr_t LOGGER = 0x42a5b0;
const unsigned char LOGGER_PROLOGUE[5] = {0x51, 0x8b, 0x44, 0x24, 0x20};   // push ecx; mov eax,[esp+20h]
const int MAX_LINES = 50000;

void* g_trampoline;
FILE* g_file;
volatile LONG g_lines;
extern volatile DWORD g_last_log_tick;
CRITICAL_SECTION g_lock;

bool path_next_to_game(char* out, const char* name) {
    DWORD n = GetModuleFileNameA(nullptr, out, MAX_PATH);
    while (n && out[n - 1] != '\\') n--;
    if (!n || n + lstrlenA(name) + 1 > MAX_PATH) return false;
    lstrcpyA(out + n, name);
    return true;
}

// The engine passes some format strings the secure CRT rejects; its invalid-parameter handler
// would terminate the game. This DLL links the CRT statically, so this only affects our copy.
void __cdecl ignore_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {}

// args points at the logger's stack arguments.
extern "C" void __cdecl capture(DWORD* args) {
    g_last_log_tick = GetTickCount();
    if (InterlockedIncrement(&g_lines) > MAX_LINES) return;
    const char* file = (const char*)args[4];
    const char* func = (const char*)args[5];
    int line = (int)args[6];
    const char* fmt = (const char*)args[7];
    // The engine logs the login (user, password, CD key) in MPGameCallbacks::Login and
    // Tri_RDVOnline::Login. Never capture any line from a login function.
    __try {
        if (func && strstr(func, "Login")) {
            // Verification only: say whether the password/CD key the game uses is the revival key.
            if (fmt && (strncmp(fmt, "Login Pwd", 9) == 0 || strncmp(fmt, "Login cdkey", 11) == 0)) {
                const char* value = (const char*)args[8];
                bool is_key = value && strncmp(value, "trv1-", 5) == 0;
                EnterCriticalSection(&g_lock);
                fprintf(g_file, "[login check] %s field is %s\n", fmt[6] == 'P' ? "password" : "CD key",
                        is_key ? "the revival key" : "NOT the revival key (value not logged)");
                fflush(g_file);
                LeaveCriticalSection(&g_lock);
            }
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    char text[1024];
    text[0] = 0;
    __try {
        if (_vsnprintf_s(text, sizeof text, _TRUNCATE, fmt, (va_list)&args[8]) < 0 && !text[0])
            strncpy_s(text, fmt, _TRUNCATE);   // rejected format: keep the raw format string
        for (char* p = text; *p; p++)
            if (*p == '\r' || *p == '\n') *p = ' ';
        EnterCriticalSection(&g_lock);
        fprintf(g_file, "[%s] %s\n", func ? func : "?", text);
        fflush(g_file);
        LeaveCriticalSection(&g_lock);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    (void)file;
    (void)line;
}

__declspec(naked) void logger_hook() {
    __asm {
        pushad
        pushfd
        lea eax, [esp + 40]        // 32 (pushad) + 4 (pushfd) + 4 (return address)
        push eax
        call capture
        add esp, 4
        popfd
        popad
        jmp dword ptr [g_trampoline]
    }
}

// Exceptions: log the real fault (address, registers, likely return addresses, C++ type name)
// before Steam's crash handler replaces it with its own fast-fail.
volatile LONG g_exceptions;

bool is_code(DWORD v) { return v >= 0x401000 && v < 0xd28000; }

// True when the bytes before v are a call instruction: call rel32 into game code, call [abs32],
// call reg, call [reg], call [reg+disp8]. Stricter than "any 0xFF nearby" so that text on the
// stack (UTF-16 like 004e0049) is not mistaken for return addresses.
bool looks_like_return(DWORD v) {
    __try {
        const unsigned char* p = (const unsigned char*)v;
        if (p[-5] == 0xE8) return is_code(v + *(const int*)(p - 4));
        if (p[-6] == 0xFF && p[-5] == 0x15) return true;
        if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10 && ((p[-1] & 0xC0) == 0xC0 || (p[-1] & 0xC7) < 0x04 || (p[-1] & 0xC7) == 0x06 || (p[-1] & 0xC7) == 0x07))
            return true;
        if (p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50 && (p[-2] & 7) != 4) return true;
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

LONG CALLBACK exception_logger(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0x406D1388 || code == 0x40010006 || code == 0x4001000A || code == DBG_PRINTEXCEPTION_WIDE_C)
        return EXCEPTION_CONTINUE_SEARCH;   // thread naming / debug output
    // Some system DLLs probe memory and fault at startup over and over; log each address at most
    // twice so they can't use up the budget before the fault that matters.
    static DWORD seen_addr[64];
    static LONG seen_count[64];
    DWORD at = (DWORD)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    for (int i = 0; i < 64; i++) {
        if (seen_addr[i] == at || InterlockedCompareExchange((LONG*)&seen_addr[i], (LONG)at, 0) == 0) {
            if (InterlockedIncrement(&seen_count[i]) > 2) return EXCEPTION_CONTINUE_SEARCH;
            break;
        }
    }
    if (InterlockedIncrement(&g_exceptions) > 200) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    char line[1024];
    int n = sprintf_s(line, "EXCEPTION %08lx at %08lx eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
                      code, (DWORD)(uintptr_t)ep->ExceptionRecord->ExceptionAddress, c->Eax, c->Ebx, c->Ecx, c->Edx,
                      c->Esi, c->Edi, c->Ebp, c->Esp);
    if (code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2)
        n += sprintf_s(line + n, sizeof line - n, " (%s %08lx)", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                       (DWORD)ep->ExceptionRecord->ExceptionInformation[1]);
    __try {
        if (code == 0xE06D7363 && ep->ExceptionRecord->NumberParameters >= 3) {   // MSVC C++ throw
            const DWORD* throw_info = (const DWORD*)ep->ExceptionRecord->ExceptionInformation[2];
            const DWORD* types = (const DWORD*)throw_info[3];
            const DWORD* first = (const DWORD*)types[1];
            const char* name = (const char*)(first[1] + 8);
            n += sprintf_s(line + n, sizeof line - n, " C++ %s", name);
        }
        const DWORD* sp = (const DWORD*)c->Esp;
        n += sprintf_s(line + n, sizeof line - n, " [esp..]:");
        for (int i = 0; i < 12; i++) n += sprintf_s(line + n, sizeof line - n, " %08lx", sp[i]);
        if ((DWORD)(uintptr_t)ep->ExceptionRecord->ExceptionAddress == 0x6f44a7) {   // event menu: track info at [esp+1Ch]
            const unsigned char* ti = (const unsigned char*)sp[7];
            n += sprintf_s(line + n, sizeof line - n, " trackinfo id=%u board=%u", *(const unsigned short*)ti, ti[2]);
        }
        n += sprintf_s(line + n, sizeof line - n, " stack:");
        int found = 0;
        for (int i = 0; i < 1024 && found < 16; i++) {
            DWORD v = sp[i];
            if (is_code(v) && looks_like_return(v)) {
                n += sprintf_s(line + n, sizeof line - n, " %08lx", v);
                found++;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    EnterCriticalSection(&g_lock);
    fprintf(g_file, "%s\n", line);
    fflush(g_file);
    LeaveCriticalSection(&g_lock);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Fatal CRT exits: the engine's allocator calls abort() when a heap chunk is bad (double free,
// overrun), and _purecall/_invalid_parameter/_amsg_exit end the process the same way. None of
// them raise an exception, so the logger above never sees them and Steam's crash handler then
// overwrites the stack. Log the caller chain first, then run the original.
struct FatalImport {
    uintptr_t slot;        // trialsFMX import address table entry (MSVCR90)
    const char* name;
    void* original;
};
FatalImport g_fatal[] = {
    {0xd283d8, "abort", nullptr},
    {0xd283b4, "_purecall", nullptr},
    {0xd283d4, "_invalid_parameter_noinfo", nullptr},
    {0xd28348, "_amsg_exit", nullptr},
    {0xd283cc, "exit", nullptr},
    {0xd285b8, "_exit", nullptr},
    {0xd28158, "TerminateProcess", nullptr},    // KERNEL32; a game that quits itself leaves no exception
};

void __cdecl log_fatal(int which, const DWORD* sp) {
    char line[1024];
    int n = sprintf_s(line, "FATAL %s called from %08lx thread %lu stack:", g_fatal[which].name, sp[0], GetCurrentThreadId());
    __try {
        int found = 0;
        for (int i = 1; i < 4096 && found < 32; i++) {
            DWORD v = sp[i];
            if (is_code(v) && looks_like_return(v)) {
                n += sprintf_s(line + n, sizeof line - n, " %08lx", v);
                found++;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    EnterCriticalSection(&g_lock);
    fprintf(g_file, "%s\n", line);
    fflush(g_file);
    LeaveCriticalSection(&g_lock);
}

#define FATAL_THUNK(i)                                       \
    __declspec(naked) void fatal_thunk_##i() {               \
        __asm { pushad }                                     \
        __asm { lea eax, [esp + 32] }                        \
        __asm { push eax }                                   \
        __asm { push i }                                     \
        __asm { call log_fatal }                             \
        __asm { add esp, 8 }                                 \
        __asm { popad }                                      \
        __asm { jmp dword ptr [g_fatal + i * 12 + 8] }       \
    }
FATAL_THUNK(0)
FATAL_THUNK(1)
FATAL_THUNK(2)
FATAL_THUNK(3)
FATAL_THUNK(4)
FATAL_THUNK(5)
FATAL_THUNK(6)

void hook_fatal_imports() {
    void (*thunks[])() = {fatal_thunk_0, fatal_thunk_1, fatal_thunk_2, fatal_thunk_3, fatal_thunk_4, fatal_thunk_5, fatal_thunk_6};
    for (int i = 0; i < 7; i++) {
        void** slot = (void**)g_fatal[i].slot;
        DWORD old;
        if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) continue;
        g_fatal[i].original = *slot;
        *slot = (void*)thunks[i];
        VirtualProtect(slot, 4, old, &old);
    }
}

// Hang watchdog: when the engine log goes quiet, record where every game thread is (instruction
// pointer and likely return addresses only - no memory contents). All threads, because the thread
// that loaded this DLL is not necessarily the one running the game loop.
volatile DWORD g_last_log_tick;
DWORD g_main_thread_id;

void log_thread_position(DWORD tid, const char* why) {
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
    if (!t) return;
    char line[1024];
    int n = sprintf_s(line, "%s: thread %lu", why, tid);
    if (SuspendThread(t) != (DWORD)-1) {
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(t, &c)) {
            n += sprintf_s(line + n, sizeof line - n, " eip=%08lx esp=%08lx stack:", c.Eip, c.Esp);
            __try {
                const DWORD* sp = (const DWORD*)c.Esp;
                int found = 0;
                for (int i = 0; i < 4096 && found < 24; i++) {
                    DWORD v = sp[i];
                    if ((v & 0xFF00FF00) == 0) continue;   // UTF-16 text on the stack, e.g. 004e0049
                    if (is_code(v) && looks_like_return(v)) {
                        n += sprintf_s(line + n, sizeof line - n, " %08lx", v);
                        found++;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        ResumeThread(t);
    }
    CloseHandle(t);
    EnterCriticalSection(&g_lock);
    fprintf(g_file, "%s\n", line);
    fflush(g_file);
    LeaveCriticalSection(&g_lock);
}

DWORD WINAPI hang_watchdog(LPVOID) {
    int reports = 0;
    for (;;) {
        Sleep(3000);
        DWORD last = g_last_log_tick;
        if (!last || GetTickCount() - last < 15000) {
            reports = 0;
            continue;
        }
        if (reports < 2) {
            const char* why = reports ? "STILL QUIET" : "LOG QUIET 15s";
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te = {sizeof te};
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                    if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId())
                        log_thread_position(te.th32ThreadID, why);
                CloseHandle(snap);
            }
            reports++;
        }
    }
}

}  // namespace

void start_gamelog_hook() {
    char path[MAX_PATH];
    if (!path_next_to_game(path, "trials_revive_gamelog.enable") || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        return;
    if ((uintptr_t)GetModuleHandleA(nullptr) != 0x400000 || memcmp((void*)LOGGER, LOGGER_PROLOGUE, 5) != 0) {
        shim_log("game log capture: unknown build, not enabled");
        return;
    }
    // The game starts twice (a launcher instance hands off to the real one): one file per process,
    // opened shared so neither instance blocks the other.
    char name[64];
    sprintf_s(name, "trials_revive_gamelog_%lu.txt", GetCurrentProcessId());
    g_file = path_next_to_game(path, name) ? _fsopen(path, "w", _SH_DENYNO) : nullptr;
    if (!g_file) return;
    InitializeCriticalSection(&g_lock);
    _set_invalid_parameter_handler(ignore_invalid_parameter);
    AddVectoredExceptionHandler(1, exception_logger);
    hook_fatal_imports();
    g_main_thread_id = GetCurrentThreadId();   // DllMain runs on the game's main thread at startup
    if (HANDLE w = CreateThread(nullptr, 0, hang_watchdog, nullptr, 0, nullptr)) CloseHandle(w);

    unsigned char* tramp = (unsigned char*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return;
    memcpy(tramp, LOGGER_PROLOGUE, 5);
    tramp[5] = 0xE9;
    *(int*)(tramp + 6) = (int)(LOGGER + 5 - (uintptr_t)(tramp + 10));
    g_trampoline = tramp;

    DWORD old;
    if (!VirtualProtect((void*)LOGGER, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    unsigned char* p = (unsigned char*)LOGGER;
    p[0] = 0xE9;
    *(int*)(p + 1) = (int)((uintptr_t)&logger_hook - (LOGGER + 5));
    VirtualProtect((void*)LOGGER, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)LOGGER, 5);
    shim_log("game log capture enabled");
}
