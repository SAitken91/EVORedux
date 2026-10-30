// Magic Marker (Origin of Pain 206): tripled sun so its paper mask wall disappears like on the 360.
// HAND-TUNED. With the sun cap raised (dlc_sun_cap.inl) the sunlit concrete mask wall reaches
// ~98.5% of the paper colour, still showing faint concrete speckle; on the 360 it is flat paper.
// A numpy rebuild of capture 3191 (Help session, sim3.py) matches the 360 at sun x2 (wall std 3.8,
// pen keeps its grime as on the 360); in game x2 still left the concrete's joint lines faintly
// visible (user screenshot 2026-10-29), so x3 (Help sim: wall std 2.5, pen still fine). The x3 is not derived from the 360 shader (no Xenos decoder),
// and other DLC tracks on the 360 are not brighter, so it applies to this track only: identified by
// its ENVA lighting block, which no other DLC track shares (sun HSV 1e 00 ff, sun-dir HSV 21 00 ff,
// ambient HSV 31 ff 00 / 8f ff 00, sun intensity byte 100 -> 20.0 at env+0x2c).
// Rollback: create datapack\trials_revive_no_magic_marker_sun.txt.

namespace {

typedef int(__fastcall* EnvaReadFn)(void* track, void* edx, void* stream, DWORD size, DWORD flag);
EnvaReadFn g_enva_read;

int __fastcall enva_read_hook(void* track, void* edx, void* stream, DWORD size, DWORD flag) {
    int result = g_enva_read(track, edx, stream, size, flag);
    if (result || (flag & 0xff)) return result;              // flag set: the reader only skipped the chunk
    __try {
        BYTE* env = *(BYTE**)(*(BYTE**)GA(0x1ee3548) + 0x30) + 0x57bc;
        static const BYTE sun[3] = {0x1e, 0x00, 0xff}, dir[3] = {0x21, 0x00, 0xff};
        static const BYTE amb_bottom[3] = {0x31, 0xff, 0x00}, amb_top[3] = {0x8f, 0xff, 0x00};
        if (!memcmp(env + 0x14, sun, 3) && !memcmp(env + 0x28, dir, 3) && !memcmp(env + 0x108, amb_bottom, 3) &&
            !memcmp(env + 0x11c, amb_top, 3) && env[0x30] == 100 && *(float*)(env + 0x2c) == 20.0f) {
            *(float*)(env + 0x2c) = 60.0f;
            shim_log("dlc light fix: Magic Marker sun intensity 20 -> 60 (paper wall)");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return result;
}

void start_magic_marker_sun_fix() {
    if (game_file_exists("trials_revive_no_magic_marker_sun.txt")) {
        shim_log("dlc light fix: Magic Marker sun DISABLED by trials_revive_no_magic_marker_sun.txt");
        return;
    }
    const uintptr_t at = GA(0x4eb1c7);                       // track loader: call ENVA reader 0x4bc290
    const unsigned char call_enva[5] = {0xe8, 0xc4, 0x10, 0xfd, 0xff};
    if (at && !code_differs(at, call_enva, sizeof call_enva)) {
        g_enva_read = (EnvaReadFn)(at + 5 + *(int*)(at + 1));
        if (patch_call(at, call_enva, sizeof call_enva, (void*)&enva_read_hook)) {
            shim_log("dlc light fix: Magic Marker sun hook armed");
            return;
        }
    }
    shim_log("dlc light fix: Magic Marker sun skipped (unexpected code)");
}

}  // namespace
