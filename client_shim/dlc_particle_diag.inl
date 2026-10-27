// Bounded particle-spawn diagnostic. No material, effect or particle limits are
// changed. Aggregate by material hash; write at most one batch per five seconds, for up to
// PARTICLE_REPORTS batches (enough for one base track and one DLC track).
// frame_a/frame_b are the emitter's flipbook frame fields (+0x10c/+0x110) that the spawn
// function fills lazily from the material's texture sequence; 0 would mean no frames to draw.
typedef void(__fastcall* ParticleSpawnFn)(void*, void*, DWORD, DWORD, DWORD, DWORD);
ParticleSpawnFn g_particle_spawn;
struct ParticleSample {
    DWORD material, calls, capped, hidden, null_control, added, max_live, limit;
    float rate, frame_a, frame_b;
    DWORD flags;
};
const LONG PARTICLE_REPORTS = 120;
ParticleSample g_particle_samples[128];
DWORD g_particle_report_tick;
volatile LONG g_particle_reports;
volatile LONG g_particle_sample_lock;
void __fastcall particle_spawn_diag(void* self, void*, DWORD a, DWORD b, DWORD c, DWORD d) {
    if (g_particle_reports >= PARTICLE_REPORTS) { g_particle_spawn(self, nullptr, a, b, c, d); return; }
    if (InterlockedCompareExchange(&g_particle_sample_lock, 1, 0)) {
        g_particle_spawn(self, nullptr, a, b, c, d); return;
    }
    BYTE* emitter = (BYTE*)self;
    BYTE* system = *(BYTE**)GA(0x1eee670);
    DWORD live = *(DWORD*)(system + 0x8c), limit = *(DWORD*)(system + 0x88);
    DWORD material = *(DWORD*)(emitter + 0x104);
    unsigned slot = (material ^ (material >> 16)) & 127;
    for (unsigned n = 0; n < 128; ++n, slot = (slot + 1) & 127)
        if (!g_particle_samples[slot].calls || g_particle_samples[slot].material == material) break;
    ParticleSample& sample = g_particle_samples[slot];
    sample.material = material; ++sample.calls;
    sample.capped += live >= limit;
    sample.hidden += (*(WORD*)(emitter + 0xa) & 0x20) != 0;
    sample.null_control += a == 0;
    sample.limit = limit;
    sample.rate = *(float*)(emitter + 0x58);
    if (live > sample.max_live) sample.max_live = live;
    g_particle_spawn(self, nullptr, a, b, c, d);
    sample.frame_a = *(float*)(emitter + 0x10c);
    sample.frame_b = *(float*)(emitter + 0x110);
    sample.flags = *(DWORD*)(emitter + 0x8);
    DWORD after = *(DWORD*)(system + 0x8c);
    if (after > live) sample.added += after - live;
    DWORD now = GetTickCount();
    if (!g_particle_report_tick) g_particle_report_tick = now;
    if (now - g_particle_report_tick < 5000) { InterlockedExchange(&g_particle_sample_lock, 0); return; }
    g_particle_report_tick = now; ++g_particle_reports;
    for (unsigned i = 0; i < 128; ++i) {
        const ParticleSample& s = g_particle_samples[i];
        if (!s.calls) continue;
        char line[320];
        sprintf_s(line, "particle sample %u: material=%08lx calls=%lu capped=%lu hidden=%lu null=%lu added=%lu peak=%lu limit=%lu rate=%g frames=%g/%g flags=%08lx",
            g_particle_reports, s.material, s.calls, s.capped, s.hidden, s.null_control,
            s.added, s.max_live, s.limit, s.rate, s.frame_a, s.frame_b, s.flags);
        shim_log(line);
    }
    memset(g_particle_samples, 0, sizeof g_particle_samples);
    InterlockedExchange(&g_particle_sample_lock, 0);
}
void start_particle_diagnostic() {
    if (!game_file_exists("trials_revive_particle_diag.txt")) return;
    const BYTE expected[] = {0x55,0x8b,0xec,0x83,0xe4,0xc0};
    if (code_differs((uintptr_t)GA(0x817c20), expected, sizeof expected)) return;
    BYTE* trampoline = (BYTE*)VirtualAlloc(nullptr, 11, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) return;
    memcpy(trampoline, expected, 6); trampoline[6] = 0xe9;
    *(int*)(trampoline + 7) = (int)(GA(0x817c26) - ((uintptr_t)trampoline + 11));
    g_particle_spawn = (ParticleSpawnFn)trampoline;
    if (patch_jmp(GA(0x817c20), expected, sizeof expected, (void*)&particle_spawn_diag))
        shim_log("particle spawn diagnostic enabled: up to 120 five-second aggregate batches, then stops sampling");
}
