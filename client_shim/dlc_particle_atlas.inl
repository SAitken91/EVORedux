// Particle atlas world filter (missing fire / torches in DLC worlds, e.g. The Great Fire).
// The particle TextureAtlas rebuild 0x78b5a0 packs a particle material only if
//   [mat+0x4b] == 0 || [mat+0x4a] == current world || current world == 0xff.
// Base PC particle materials have world [mat+0x4a] = 0 but a nonzero byte at +0x4b (values like
// 47/101/114, apparently never meant as a flag on PC), so in DLC worlds 1/2 Fire_2_sequence and
// other base sprites were left out of the atlas and fire particles drew as flat grey squares
// (RenderDoc capture 2026-10-25). Fix: world-0 (base) materials also pass. In world 0 this
// changes nothing (they already passed by [mat+0x4a] == 0).
// Rollback: create datapack\trials_revive_no_atlas_fix.txt.

namespace {

const uintptr_t ATLAS_FILTER = GA(0x78b68c);  // cmp byte [edi+4bh],0 ; je 78b6ac (pass)
// 0x78b692: movzx ecx,byte [edi+4ah] ; compare with current world (original fall-through)
const unsigned char ATLAS_FILTER_ORIG[6] = {0x80, 0x7f, 0x4b, 0x00, 0x74, 0x1a};

__declspec(naked) void particle_atlas_filter_stub() {
    __asm {
        cmp byte ptr [edi + 0x4b], 0
        je pass
        cmp byte ptr [edi + 0x4a], 0
        je pass
        push ga_78b692  // ATLAS_FILTER_WORLD
        ret
    pass:
        push ga_78b6ac  // ATLAS_FILTER_PASS
        ret
    }
}

void start_particle_atlas_fix() {
    if (game_file_exists("trials_revive_no_atlas_fix.txt")) {
        shim_log("particle atlas fix: DISABLED by trials_revive_no_atlas_fix.txt");
        return;
    }
    shim_log(patch_jmp(ATLAS_FILTER, ATLAS_FILTER_ORIG, sizeof ATLAS_FILTER_ORIG, (void*)&particle_atlas_filter_stub)
                 ? "particle atlas fix: base particle materials allowed in DLC worlds"
                 : "particle atlas fix: skipped (unexpected code)");
}

}  // namespace
