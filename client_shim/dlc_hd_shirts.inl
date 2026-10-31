// Trials HD rider shirts (GitHub issue #10): owned by everyone.
// The garage's rider gear list (0x6fbcc0) hardcodes two items to the TrialsHDAwards bits
// (*(*(*0x1ee3548 + 0x78) + 0x18)), which only the 360 sets from the profile's Trials HD
// achievements: id 65 Trials HD shirt needs bit 1 plus the "checked" byte at +8, id 58 Ultimate
// Endurance shirt needs bit 4 (Marathon). Nothing on PC sets them, so both always showed locked.
// Dropping the three branches lists both as owned; the save's award bits are not touched.
// Items come from tools/dlc_rider_gear.py. Rollback: create datapack\trials_revive_no_hd_shirts.txt.

namespace {

bool nop_jcc(uintptr_t at, const unsigned char* expected, size_t len, const size_t* jumps, size_t count) {
    if (!at || code_differs(at, expected, len)) return false;
    DWORD old;
    if (!VirtualProtect((void*)at, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    for (size_t i = 0; i < count; ++i) {
        unsigned char* p = (unsigned char*)at + jumps[i];
        p[0] = p[1] = 0x90;                                  // je rel8
    }
    VirtualProtect((void*)at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, len);
    return true;
}

void start_hd_shirts_fix() {
    if (game_file_exists("trials_revive_no_hd_shirts.txt")) {
        shim_log("hd shirts: unlock DISABLED by trials_revive_no_hd_shirts.txt");
        return;
    }
    // cmp byte [eax+8],0 / je locked / test byte [eax],1 / je locked / push 3
    const unsigned char hd[13] = {0x80,0x78,0x08,0x00, 0x74,0x75, 0xf6,0x00,0x01, 0x74,0x70, 0x6a,0x03};
    const size_t hd_jumps[2] = {4, 9};
    // mov edx,[eax] / shr edx,2 / test dl,1 / je locked / push 3
    const unsigned char ue[12] = {0x8b,0x10, 0xc1,0xea,0x02, 0xf6,0xc2,0x01, 0x74,0x6d, 0x6a,0x03};
    const size_t ue_jumps[1] = {8};
    const uintptr_t hd_at = GA(0x6fc87d), ue_at = GA(0x6fc9ee);
    if (!hd_at || !ue_at || code_differs(hd_at, hd, sizeof hd) || code_differs(ue_at, ue, sizeof ue)) {
        shim_log("hd shirts: unexpected garage code; unlock skipped");
        return;
    }
    shim_log(nop_jcc(hd_at, hd, sizeof hd, hd_jumps, 2) && nop_jcc(ue_at, ue, sizeof ue, ue_jumps, 1)
                 ? "hd shirts: Trials HD and Ultimate Endurance shirts owned in the garage"
                 : "hd shirts: unlock patch failed");
}

}  // namespace
