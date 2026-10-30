// Deferred sun light cap raised from 6 to 32 (Magic Marker's paper wall, GitHub issue #9).
// The PC sun pixel shader (gbufferNormal, shadowMapPSSM, sun colour gp_colorPSSM) ends
// with `min r0.xyz, r0.xyzx, l(6,6,6,0)` before multiplying by albedo. DLC worlds use much
// stronger suns than the base game: Origin of Pain's Magic Marker has sun intensity 20, so its
// sunlit mask wall should light to ~12 and clip to white, which the track's sepia tint turns into
// the paper colour (360 screenshot in issue #9). Capped at 6 it came out as grey concrete.
// Base-game suns stay below 6, so they are unaffected. The engine creates every pixel shader at
// 0x882c15 (call [device vtable + 0x3c] = ID3D11Device::CreatePixelShader); this wrapper copies
// matching bytecode, raises the immediate, and re-signs the DXBC (checksum verified offline
// against 40 shaders from a capture). Found by the "Help" session from capture 3191.
// Rollback: create datapack\trials_revive_no_sun_cap.txt.

namespace {

typedef HRESULT(__stdcall* CreatePixelShaderFn)(void* device, const void* code, SIZE_T size, void* linkage, void** out);
CreatePixelShaderFn g_create_pixel_shader;
volatile LONG g_sun_cap_patched;

// DXBC checksum: MD5 rounds over bytes 20.., with DXBC's own final padding.
void md5_block(DWORD st[4], const BYTE* p) {
    static const DWORD K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const BYTE R[64] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22, 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                               4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23, 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    const DWORD* m = (const DWORD*)p;
    DWORD a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; i++) {
        DWORD f; int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        f += a + K[i] + m[g];
        a = d; d = c; c = b;
        b += (f << R[i]) | (f >> (32 - R[i]));
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

void dxbc_sign(BYTE* code, DWORD size) {
    DWORD st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const BYTE* p = code + 20;
    DWORD n = size - 20, full = n / 64, rest = n % 64, bits = n * 8, second = (bits >> 2) | 1;
    for (DWORD i = 0; i < full; i++) md5_block(st, p + i * 64);
    BYTE blk[64] = {};
    if (rest >= 56) {
        memcpy(blk, p + full * 64, rest); blk[rest] = 0x80;
        md5_block(st, blk);
        memset(blk, 0, 64);
        memcpy(blk, &bits, 4); memcpy(blk + 60, &second, 4);
        md5_block(st, blk);
    } else {
        memcpy(blk, &bits, 4); memcpy(blk + 4, p + full * 64, rest); blk[4 + rest] = 0x80;
        memcpy(blk + 60, &second, 4);
        md5_block(st, blk);
    }
    memcpy(code + 4, st, 16);
}

const BYTE* find_bytes(const BYTE* p, SIZE_T n, const void* pat, SIZE_T m) {
    for (SIZE_T i = 0; i + m <= n; i++) if (!memcmp(p + i, pat, m)) return p + i;
    return nullptr;
}

HRESULT __stdcall create_pixel_shader_hook(void* device, const void* code, SIZE_T size, void* linkage, void** out) {
    // Immediate operand of `min r0.xyz, r0.xyzx, l(6,6,6,0)`: token 0x00004002 then the 4 floats.
    static const DWORD clamp6[5] = {0x00004002, 0x40c00000, 0x40c00000, 0x40c00000, 0};
    const BYTE* src = (const BYTE*)code;
    if (src && size > 64 && size < 0x100000 && !memcmp(src, "DXBC", 4) &&
        find_bytes(src, size, "gp_colorPSSM", 12) && find_bytes(src, size, "shadowMapPSSM", 13) &&
        find_bytes(src, size, "gbufferNormal", 13)) {
        const BYTE* hit = find_bytes(src, size, clamp6, sizeof clamp6);
        if (hit && !find_bytes(hit + 1, size - (hit + 1 - src), clamp6, sizeof clamp6)) {
            BYTE* copy = (BYTE*)HeapAlloc(GetProcessHeap(), 0, size);
            if (copy) {
                memcpy(copy, src, size);
                DWORD* imm = (DWORD*)(copy + (hit - src)) + 1;
                imm[0] = imm[1] = imm[2] = 0x42000000;      // 32.0f
                dxbc_sign(copy, (DWORD)size);
                HRESULT hr = g_create_pixel_shader(device, copy, size, linkage, out);
                HeapFree(GetProcessHeap(), 0, copy);
                if (SUCCEEDED(hr)) {
                    if (InterlockedIncrement(&g_sun_cap_patched) <= 8)
                        shim_log("dlc light fix: sun shader light cap raised from 6 to 32");
                    return hr;
                }
                shim_log("dlc light fix: patched sun shader rejected; using the original");
            }
        }
    }
    return g_create_pixel_shader(device, code, size, linkage, out);
}

// Replaces `mov ecx,[edx+3Ch] ; call ecx` (args already pushed, edx = device vtable).
__declspec(naked) void create_pixel_shader_stub() {
    __asm {
        mov ecx, dword ptr [edx + 0x3c]
        mov g_create_pixel_shader, ecx
        jmp create_pixel_shader_hook
    }
}

void start_sun_cap_fix() {
    if (game_file_exists("trials_revive_no_sun_cap.txt")) {
        shim_log("dlc light fix: sun light cap DISABLED by trials_revive_no_sun_cap.txt");
        return;
    }
    const unsigned char call_ps[5] = {0x8b, 0x4a, 0x3c, 0xff, 0xd1};
    shim_log(patch_call(GA(0x882c15), call_ps, sizeof call_ps, (void*)&create_pixel_shader_stub)
                 ? "dlc light fix: sun light cap hook armed"
                 : "dlc light fix: sun light cap skipped (unexpected code)");
}

}  // namespace
