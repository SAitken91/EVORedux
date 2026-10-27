// Shader constant upload fix (FPS in DLC worlds, e.g. The Great Fire).
// Native 0x84d750 copies a material's constant blocks into the mapped constant buffer and,
// for instances with a modulation table (flag 0x4000 at +0xa, table at +0x78), multiplies the
// copied floats IN PLACE. The destination is write-combined GPU memory (PAGE_WRITECOMBINE),
// so every read-back stalls; DLC scenes use this path for thousands of draws per frame and the
// main thread spent ~70% of its time there (~15 FPS vs ~30 on base tracks).
// Replacement does the same work but only writes to the destination. x87 and SSE products of
// two floats round identically (exact 48-bit product, one rounding), so output is bit-exact.
// Rollback: create datapack\trials_revive_no_wc_fix.txt.

namespace {

float* __cdecl shader_constants_upload(void*, float* dst, const BYTE* shader, const BYTE* inst) {
    const BYTE* data = *(const BYTE* const*)(shader + 0x48);
    const BYTE* layout = *(const BYTE* const*)(data + 8);
    int blocks = *(const int*)layout;
    if (blocks <= 0) return dst;
    const short* rec = *(const short* const*)(layout + 8);
    const float* src = *(const float* const*)(data + 0x14);
    const float* mul = nullptr;
    if (*(const WORD*)(inst + 0xa) & 0x4000) {
        const BYTE* mod = *(const BYTE* const*)(inst + 0x78);
        if (mod) mul = *(const float* const*)(mod + 0x14);
    }
    for (int b = 0; b < blocks; b++, rec += 10) {  // 0x14-byte records: s16 offset, s16 count
        int off = rec[0], n = rec[1];
        if (mul) {
            for (int i = 0; i < n; i++) dst[i] = mul[off + i] * src[off + i];
        } else {
            memcpy(dst, src + off, (size_t)n * 4);
        }
        dst += n;
    }
    return dst;
}

void start_shader_constants_fix() {
    if (game_file_exists("trials_revive_no_wc_fix.txt")) {
        shim_log("shader constant fix: DISABLED by trials_revive_no_wc_fix.txt");
        return;
    }
    const unsigned char entry[5] = {0x51, 0x8b, 0x44, 0x24, 0x10};
    shim_log(patch_jmp(GA(0x84d750), entry, sizeof entry, (void*)&shader_constants_upload)
                 ? "shader constant fix: write-only constant upload enabled"
                 : "shader constant fix: skipped (unexpected code)");
}

}  // namespace
