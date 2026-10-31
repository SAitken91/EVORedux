// Aerial Architect (Riders of Doom 427, GitHub issue #2): the bike loses its wheels at GO and is
// flown like a jetpack (RT = jet area effectors moved onto the bike every tick, LT = magnet).
// On PC the run ended at GO. Two DLC behaviours were missing:
//
// 1. Physics pause on a bike part. The track sets object property 0x2d (physics pause, setter
//    0x564e90) to 1 at tick 0 and back to 0 at tick 19, targeting bike part 0x3a05dccc. PC's setter
//    only knows "RiderDummy" / "BikeDummy" objects, so the bike was never frozen while its wheels
//    were detached and full lean was applied, and it was thrown at GO. Those two body parts
//    (0x3a05dccc, 0x4ff08f15; the same pair the rider crash routine 0x4641d0 uses) now freeze and
//    release the bike and rider together (0x4582e0 + 0x4582a0); release also re-enables both
//    physics groups (0x457e70 / 0x458080), as the setters' false path only clears the flag.
//
// 2. Detached parts while paused. Before each world step (call 0x7a4150 at 0x4a1620), parts
//    detached from a paused bike get their bodies enabled again (0x7cefd0) and their joints to
//    the bike disabled (except 0x0f88117e), so they fall away instead of pulling the frozen bike
//    apart. Restricted to this track by its object layout (dummy 10, area effectors 87 / 104).
//    The same pre-step hook also evaluates DLC colour vectors (dlc_vectors.inl).
//
// Together with retargetor resolution for object-info / distance queries and EventPhysics
// (dlc_retarget_queries.inl). User-tested 2026-10-30.
// Off switch: datapack\trials_revive_no_aerial_architect.txt (parts 1 and 2; colour vectors stay).

DWORD object_model(void* object);   // dlc_fixes.cpp
extern BYTE* g_nt_objects;          // dlc_nt_trace.inl
void update_dlc_vector_colors();    // dlc_vectors.inl

namespace {

bool g_aerial_fix_enabled;

BYTE* aerial_current_bike() {
    BYTE* game = *(BYTE**)GAME_GLOBAL;
    BYTE* track = game ? *(BYTE**)(game + 0x30) : nullptr;
    return track && *(DWORD*)(track + 0x1cc) ? *(BYTE**)(track + 0x1bc) : nullptr;
}

// ---- 1. physics pause on bike parts --------------------------------------------------------
typedef void(__fastcall* PauseSetFn)(void*, void*, DWORD, DWORD);
PauseSetFn g_pause_set_original;

// 0x564e90 walks the node's target list (head at self+0x1c: node+4 next, node+8 object) and
// compares [object+0x24]+0x30 with the "RiderDummy" / "BikeDummy" names.
void pause_body_parts(void* self, float value) {
    BYTE* bike = aerial_current_bike();
    if (!bike) return;
    BYTE* node = *(BYTE**)((BYTE*)self + 0x1c);
    for (int k = 0; node && k < 32; ++k, node = *(BYTE**)(node + 4)) {
        BYTE* object = *(BYTE**)(node + 8);
        DWORD name = object && *(BYTE**)(object + 0x24) ? *(DWORD*)(*(BYTE**)(object + 0x24) + 0x30) : 0;
        if (name != 0x3a05dccc && name != 0x4ff08f15) continue;
        const bool on = value != 0.0f;
        ((void(__fastcall*)(void*, void*, bool))GA(0x4582e0))(bike, nullptr, on);
        ((void(__fastcall*)(void*, void*, bool))GA(0x4582a0))(bike, nullptr, on);
        if (!on) {
            ((void(__fastcall*)(void*, void*, bool))GA(0x457e70))(bike, nullptr, false);
            ((void(__fastcall*)(void*, void*, bool))GA(0x458080))(bike, nullptr, false);
        }
        static volatile LONG told;
        if (InterlockedIncrement(&told) <= 10) {
            char line[96];
            sprintf_s(line, "aerial architect: bike and rider %s", on ? "held" : "released");
            shim_log(line);
        }
    }
}

// Arguments: property id (0x2d), value (float bits).
void __fastcall pause_set_hook(void* self, void* edx, DWORD property, DWORD value) {
    __try { pause_body_parts(self, *(float*)&value); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_pause_set_original(self, edx, property, value);
}

// ---- 2. detached parts of a paused bike ----------------------------------------------------
void aerial_detached_parts() {
    __try {
        BYTE* bike = aerial_current_bike();
        if (!bike || !g_nt_objects || *(DWORD*)g_nt_objects < 831) return;
        void** owners = *(void***)(g_nt_objects + 8);
        if (!owners || object_model(owners[10]) != 0x95c4737d || object_model(owners[87]) != 0xe916f7d0 ||
            object_model(owners[104]) != 0xe916f7d0) return;
        const DWORD names[] = {0x455e5241, 0x6d886067, 0x8d32199b, 0xf84642f0};
        for (DWORD name : names) {
            BYTE* part = ((BYTE*(__fastcall*)(void*, void*, const DWORD*))GA(0x4594c0))(bike, nullptr, &name);
            BYTE* body = part ? *(BYTE**)(part + 0x84) : nullptr;
            if (!body) continue;
            if (*(DWORD*)(body + 0xcc) & 1)
                ((void(__fastcall*)(void*, void*, bool))GA(0x7cefd0))(part, nullptr, true);
            BYTE* node = *(BYTE**)(part + 0x94);
            for (unsigned k = 0; node && k < 128; ++k) {
                BYTE* joint = *(BYTE**)(node + 8);
                node = *(BYTE**)(node + 4);
                if (!joint || *(DWORD*)(joint + 0x18) == 0x0f88117e || *(DWORD*)(joint + 0x1c) == 0x0f88117e) continue;
                joint[0x14] = 1;
                if (joint[0x15]) ((void(__fastcall*)(void*, void*))(*(void***)joint)[3])(joint, nullptr);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void aerial_pre_step() {
    update_dlc_vector_colors();
    if (g_aerial_fix_enabled) aerial_detached_parts();
}

uintptr_t g_world_step = GA(0x7a4150);
__declspec(naked) void world_step_stub() {
    __asm {
        pushfd
        pushad
        call aerial_pre_step
        popad
        popfd
        jmp dword ptr [g_world_step]
    }
}

void start_aerial_architect_fix() {
    g_aerial_fix_enabled = !game_file_exists("trials_revive_no_aerial_architect.txt");
    const uintptr_t step_call = GA(0x4a1620);
    BYTE call[5] = {0xe8};
    *(int*)(call + 1) = (int)(g_world_step - (step_call + 5));
    shim_log(step_call && g_world_step && patch_call(step_call, call, sizeof call, (void*)&world_step_stub)
                 ? "dlc fix: pre-step hook installed (colour vectors, Aerial Architect parts)"
                 : "dlc fix: pre-step hook skipped (unexpected code)");
    if (!g_aerial_fix_enabled) {
        shim_log("aerial architect: DISABLED by trials_revive_no_aerial_architect.txt");
        return;
    }
    const BYTE entry[6] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};   // 0x564e90 (pause property setter)
    g_pause_set_original = (PauseSetFn)audio_garage_trampoline(GA(0x564e90), entry, sizeof entry);
    shim_log(g_pause_set_original && patch_jmp(GA(0x564e90), entry, sizeof entry, (void*)&pause_set_hook)
                 ? "aerial architect: bike-part physics pause enabled"
                 : "aerial architect: pause hook skipped (unexpected code)");
}

}  // namespace
