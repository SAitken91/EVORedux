// Larger terrain decal pool (DLC base terrain layer).
// VTBlockRendererGame::loadTriangles (0x8098d0) stores every decal's vertices and indices in 64 KB
// blocks whose pointers live in a fixed 256-entry table (0x1ef8b90, block count at 0x1ef8f90, no
// bounds check), so a world's decals must fit in 16 MB. The PC base world uses 13.6 MB. The DLC
// worlds' base terrain layer (tools/dlc_terrain_base.py, the 360's vtPageTerrainBase pass baked
// into decalsTerrain<N>.dat) needs ~31 MB, and overflowing the table crashed the game on loading
// the world. The eight instructions that address the table (allocator 0x806410, free-all 0x806470
// and the two copies inlined in loadTriangles) are pointed at a 2048-entry table here (128 MB;
// the full-resolution bake uses ~75 MB).
// Rollback: create datapack\trials_revive_no_decal_pool.txt.

namespace {

const DWORD DECAL_POOL_ENTRIES = 2048;
void* g_decal_pool[DECAL_POOL_ENTRIES + 1];   // [0] is "table - 4", read as the current block

struct DecalPoolSite { uintptr_t site; uintptr_t old_value; uintptr_t new_value; };

void start_decal_pool_fix() {
    if (game_file_exists("trials_revive_no_decal_pool.txt")) {
        shim_log("dlc texture fix: decal pool enlargement DISABLED by trials_revive_no_decal_pool.txt");
        return;
    }
    const uintptr_t table = GA(0x1ef8b90), table_minus4 = GA(0x1ef8b8c), count = GA(0x1ef8f90);
    const uintptr_t new_table = (uintptr_t)&g_decal_pool[1], new_minus4 = (uintptr_t)&g_decal_pool[0];
    const DecalPoolSite sites[] = {
        {GA(0x80643e), table, new_table},          // allocator: store new block
        {GA(0x806483), table, new_table},          // free-all: read block
        {GA(0x806490), table, new_table},          // free-all: clear entry
        {GA(0x809e30), table, new_table},          // loadTriangles (vertices): store new block
        {GA(0x80a3b4), table, new_table},          // loadTriangles (indices): store new block
        {GA(0x806457), table_minus4, new_minus4},  // allocator: current block
        {GA(0x809e49), table_minus4, new_minus4},  // loadTriangles (vertices): current block
        {GA(0x80a3cc), table_minus4, new_minus4},  // loadTriangles (indices): current block
    };
    bool ok = table && table_minus4 && count;
    for (const DecalPoolSite& s : sites)
        ok = ok && s.site && *(uintptr_t*)s.site == s.old_value;
    if (!ok) {
        shim_log("dlc texture fix: decal pool enlargement skipped (unexpected code)");
        return;
    }
    // Blocks already allocated (normally none yet) move to the new table first.
    DWORD used = *(DWORD*)count;
    if (used > 256) used = 256;
    memcpy(&g_decal_pool[1], (void*)table, used * sizeof(void*));
    for (const DecalPoolSite& s : sites) {
        DWORD old;
        if (VirtualProtect((void*)s.site, 4, PAGE_EXECUTE_READWRITE, &old)) {
            *(uintptr_t*)s.site = s.new_value;
            VirtualProtect((void*)s.site, 4, old, &old);
        }
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    shim_log("dlc texture fix: terrain decal pool enlarged to 128 MB (2048 blocks)");
}

}  // namespace
