// Far blur / distance haze in the final post pass (GitHub issue #9, "FarBlur").
// The Xbox postCombine pixel shader (data.pak shaders/postCombine.fx_Ps.o, decoded with Xenos
// microcode: instructions 53-57) blends towards the blurred post buffer where the fog pass faded
// the scene:  w = saturate(2 * |fogColor - blurred|^2 + scene.alpha);  colour = lerp(blurred, scene, w).
// scene.alpha is what the fog pass leaves (fog transmittance), so distant fogged areas take the
// soft, washed-out blurred image (the 360's High Level Degree cranes). The PC postCombine shaders
// (technique 0 "PS" and technique 2 "PS_DOF") skip that step. Both PC shaders are recognised by
// their DXBC checksums, recompiled from the HLSL below (a line-for-line copy of their PC
// disassembly plus the far blur) with the system d3dcompiler_47.dll, and created instead.
// Anything unexpected falls back to the original shader.
// Rollback: create datapack\trials_revive_no_far_blur.txt.

namespace {

const char FAR_BLUR_HLSL[] = R"HLSL(
cbuffer constantsPS : register(b0) {
    float4 gp_fogColor; float gp_bloomMul; float gp_bloomExp; float gp_focalDist; float gp_focusNear;
    float gp_focusFar; float gp_exposure; float gp_fogNearDensity; float gp_fogFarDensity;
    float gp_fogExp; float3 gp_colorizeRGB;
    float4 gp_dofParameters; float2 gp_zConversion; float gp_param1; float gp_effectFactor; float gp_time;
};
Texture2D postProcessInput1 : register(t0);
Texture2D postProcessInput2 : register(t1);
Texture2D postProcessInput4 : register(t3);
Texture3D colorCorrectionLookUp : register(t5);
SamplerState postProcessInput1Sampler : register(s0);
SamplerState postProcessInput2Sampler : register(s1);
SamplerState postProcessInput4Sampler : register(s3);
SamplerState colorCorrectionLookUpSampler : register(s5);

float3 far_blur(float4 scene, float3 blurred) {
    float3 d = gp_fogColor.xyz - blurred;
    return lerp(blurred, scene.xyz, saturate(dot(d, d) * 2.0 + scene.w));
}

float4 PS(float4 pos : SV_Position, float4 uv : TEXTURE0) : SV_Target {
    float4 scene = postProcessInput1.Sample(postProcessInput1Sampler, uv.xy);
    float4 blur = postProcessInput2.Sample(postProcessInput2Sampler, uv.xy);
    float3 c = far_blur(scene, blur.xyz);
    c = saturate(c + (blur.xyz + 0.5) * saturate(blur.w));
    c = sqrt(c) * 0.9375 + 0.029297;
    return float4(colorCorrectionLookUp.Sample(colorCorrectionLookUpSampler, c).xyz * gp_colorizeRGB, 0);
}

float4 PS_DOF(float4 pos : SV_Position, float4 uv : TEXTURE0) : SV_Target {
    float z = postProcessInput4.Sample(postProcessInput4Sampler, uv.xy).x;
    float dist = gp_zConversion.y / (z - gp_zConversion.x);
    float nearAmount = (1.0 - dist / gp_dofParameters.x) * gp_dofParameters.z;
    float farAmount = (1.0 - gp_dofParameters.x / dist) * gp_dofParameters.w;
    float amount = saturate(0 >= dist - gp_dofParameters.x ? nearAmount : farAmount);
    float4 scene = postProcessInput1.Sample(postProcessInput1Sampler, uv.xy);
    float4 blur = postProcessInput2.Sample(postProcessInput2Sampler, uv.xy);
    float3 c = lerp(far_blur(scene, blur.xyz), blur.xyz, amount);
    c = saturate(c + (blur.xyz + 0.5) * saturate(blur.w));
    c = min(sqrt(c) + 0.03125, 1.0) * 0.9375;
    return float4(colorCorrectionLookUp.Sample(colorCorrectionLookUpSampler, c).xyz * gp_colorizeRGB, 1);
}
)HLSL";

struct FarBlurTarget { DWORD checksum[4]; const char* entry; };
const FarBlurTarget FAR_BLUR_TARGETS[] = {
    {{0x8ef9dec0, 0x54dbfb16, 0x1860d8fa, 0x5d71132e}, "PS"},       // postCombine technique 0
    {{0xf27995a5, 0xa85f9172, 0x79d8a741, 0x7992b253}, "PS_DOF"},   // postCombine technique 2
};

typedef HRESULT(WINAPI* D3DCompileFn)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);
bool g_far_blur_enabled;

// Creates the far-blur version of a matching postCombine shader. True = handled (*hr set).
bool far_blur_create(void* device, const void* code, SIZE_T size, void* linkage, void** out,
                     CreatePixelShaderFn create, HRESULT* hr) {
    if (!g_far_blur_enabled || !code || size < 24 || memcmp(code, "DXBC", 4)) return false;
    const FarBlurTarget* target = nullptr;
    for (const FarBlurTarget& t : FAR_BLUR_TARGETS)
        if (!memcmp((const BYTE*)code + 4, t.checksum, 16)) target = &t;
    if (!target) return false;
    static D3DCompileFn compile = (D3DCompileFn)GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile");
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    if (!compile || FAILED(compile(FAR_BLUR_HLSL, sizeof FAR_BLUR_HLSL - 1, "far_blur", nullptr, nullptr, target->entry,
                                   "ps_4_0", 0, 0, &blob, &errors)) || !blob) {
        char line[400];
        _snprintf_s(line, _TRUNCATE, "dlc post fix: far blur %s not compiled (%s); using the original", target->entry,
                    errors ? (const char*)errors->GetBufferPointer() : "d3dcompiler_47 unavailable");
        shim_log(line);
        if (errors) errors->Release();
        return false;
    }
    if (errors) errors->Release();
    *hr = create(device, blob->GetBufferPointer(), blob->GetBufferSize(), linkage, out);
    blob->Release();
    if (FAILED(*hr)) {
        shim_log("dlc post fix: far blur shader rejected; using the original");
        return false;
    }
    char line[120];
    _snprintf_s(line, _TRUNCATE, "dlc post fix: far blur added to postCombine %s", target->entry);
    shim_log(line);
    return true;
}

void start_far_blur_fix() {
    g_far_blur_enabled = !game_file_exists("trials_revive_no_far_blur.txt");
    shim_log(g_far_blur_enabled ? "dlc post fix: far blur enabled (applied when postCombine is created)"
                                : "dlc post fix: far blur DISABLED by trials_revive_no_far_blur.txt");
}

}  // namespace
