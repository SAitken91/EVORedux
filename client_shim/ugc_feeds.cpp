// Trials Evolution revival: Track Central feed setup (tms/ugc.xml) from the revival server.
//
// The game reads its Track Central feed list from the TMS document tms/ugc.xml. Ubisoft's TMS is
// gone, so it always falls back to the copy built into the exe, whose "RedLynx Picks" feed has an
// empty track list. RDVGetFeedsTask treats an empty picks list as an error and drops every
// feed, so Get Tracks stays empty. This fetches http://<OnlineConfigHost>/revival/ugc.xml at
// startup and points the fallback at it (the picks then list real tracks).
//
// Build-independent: the built-in document is found by its text in .rdata and the single
// `push <document>` in .text (TMSUGC failsafe getter, Steam 0x69fba6) by its operand, so no
// address table entry is needed. Without the server file the game is left unchanged.
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include "game_build.h"

#pragma comment(lib, "winhttp.lib")

void shim_log(const char* msg);
bool read_online_config_host(wchar_t* host, int size);   // ticker_hook.cpp

namespace {

const char BUILTIN_PREFIX[] = "<UGC>\n  <killswitch value=\"false\" />";
const int MAX_DOC = 64 * 1024;
char g_doc[MAX_DOC + 1];

uintptr_t find_bytes(uintptr_t start, uintptr_t end, const void* pat, size_t len) {
    __try {
        for (uintptr_t p = start; p + len <= end; p++)
            if (memcmp((const void*)p, pat, len) == 0) return p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

int fetch_document() {
    wchar_t host[128];
    if (!read_online_config_host(host, 128)) return 0;
    HINTERNET session = WinHttpOpen(L"TrialsRevival/1", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    if (!session) return 0;
    WinHttpSetTimeouts(session, 3000, 3000, 3000, 5000);
    HINTERNET connect = WinHttpConnect(session, host, 80, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", L"/revival/ugc.xml", nullptr,
                                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    DWORD total = 0, status = 0, size = sizeof status;
    if (request && WinHttpSendRequest(request, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)
        && WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr)
        && status == 200) {
        DWORD got = 0;
        while (total < MAX_DOC && WinHttpReadData(request, g_doc + total, MAX_DOC - total, &got) && got)
            total += got;
    }
    g_doc[total] = 0;
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return (total >= 5 && total < MAX_DOC && strncmp(g_doc, "<UGC>", 5) == 0) ? (int)total : 0;
}

uintptr_t find_reference(uintptr_t* doc_out) {
    uintptr_t doc = find_bytes(game_rdata_start(), game_rdata_end(), BUILTIN_PREFIX, sizeof BUILTIN_PREFIX - 1);
    if (!doc) return 0;
    unsigned char push[5] = {0x68};
    memcpy(push + 1, &doc, 4);
    uintptr_t site = find_bytes(game_text_start(), game_text_end(), push, sizeof push);
    if (!site || find_bytes(site + 1, game_text_end(), push, sizeof push)) return 0;
    *doc_out = doc;
    return site;
}

DWORD WINAPI feeds_thread(LPVOID) {
    if (!game_build_known()) return 0;
    if (!fetch_document()) {
        shim_log("track central feeds: no ugc.xml on the server, built-in feeds kept");
        return 0;
    }
    // Polled: the code may not be readable yet this early (Steam's DRM unpacks it after load).
    uintptr_t doc = 0, site = 0;
    for (int i = 0; i < 240 && !site; i++) {
        site = find_reference(&doc);
        if (!site) Sleep(250);
    }
    if (!site) {
        shim_log("track central feeds: built-in feed document not found, left unchanged");
        return 0;
    }
    DWORD old;
    if (!VirtualProtect((void*)(site + 1), 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    uintptr_t ours = (uintptr_t)g_doc;
    memcpy((void*)(site + 1), &ours, 4);
    VirtualProtect((void*)(site + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)(site + 1), 4);
    shim_log("track central feeds loaded from the server");
    return 0;
}

}  // namespace

void start_ugc_feeds() {
    HANDLE t = CreateThread(nullptr, 0, feeds_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}
