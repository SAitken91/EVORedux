// Bounded runtime trace of the DLC generation chain (Neverending Trials). Enabled only by
// datapack\trials_revive_nt_trace.txt. Logs what actually runs: transform events (owner object,
// enabled flag, X/Y/Z inputs), retargetor choices, set-vector executions and vector values.
// Observation only: no game state is changed.
const LONG NT_TRACE_MAX = 1500;
bool g_nt_trace;
volatile LONG g_nt_trace_lines;
BYTE* g_nt_objects;                      // track's serialized object array (track + 0x24a08)

int nt_object_index(void* object) {
    if (!g_nt_objects || !object) return -1;
    DWORD count = *(DWORD*)g_nt_objects;
    void** owners = *(void***)(g_nt_objects + 8);
    if (!owners || count > 32768) return -1;
    for (DWORD k = 0; k < count; ++k) if (owners[k] == object) return (int)k;
    return -1;
}

bool nt_trace_line() {
    return g_nt_trace && InterlockedIncrement(&g_nt_trace_lines) <= NT_TRACE_MAX;
}

void nt_log(const char* line) {
    char out[1100];
    sprintf_s(out, "nt: %s", line);
    shim_log(out);
}

float nt_input_value(void* owner, DWORD serialized, bool* found) {
    DWORD property = (DWORD)(int)((short*)GA(0x1eebec8))[serialized];
    void* input = selector_input(owner, property);
    *found = input != nullptr;
    return input ? *(float*)((BYTE*)input + 0x28) : 0.0f;
}

void nt_trace_retarget(void* object, float index, void* chosen) {
    if (!nt_trace_line()) return;
    char line[200];
    sprintf_s(line, "retargetor %d index=%g -> %d", nt_object_index(object), index, nt_object_index(chosen));
    nt_log(line);
}

typedef unsigned char(__fastcall* TransformExecFn)(void*, void*);
TransformExecFn g_transform_exec;

void nt_trace_switch(void* owner, float comparison, bool wired, unsigned char mask) {
    if (!nt_trace_line()) return;
    char line[160];
    sprintf_s(line, "switch %d compared %s%g -> output mask %02x", nt_object_index(owner),
        wired ? "" : "(unwired) ", comparison, mask);
    nt_log(line);
}

// Per-frame transforms (Neverending moves its portals every tick): log each owner's first 3 runs only.
struct NtSeen { void* owner; unsigned count; };
NtSeen g_nt_seen[64];
bool nt_first_runs(void* owner) {
    for (NtSeen& s : g_nt_seen) {
        if (s.owner == owner) return ++s.count <= 3;
        if (!s.owner) { s.owner = owner; s.count = 1; return true; }
    }
    return false;
}

// Trigger-system node (event/filter/trigger) whose owner is a track object, or -1. SEH-guarded.
int nt_node_owner_index(DWORD value) {
    __try {
        if (value < 0x10000 || value > 0x7fff0000 || (value & 3)) return -1;
        DWORD vt = *(DWORD*)value;
        if (vt < 0x1000000 || vt > 0x2000000) return -1;
        return nt_object_index(((void**)value)[1]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Who fired this transform: track-object owners of trigger-system nodes found on the stack.
void nt_trace_callers(void* self, const DWORD* stack) {
    int own = nt_object_index(((void**)self)[1]);
    if (own != 1078 && own != 2874 && own != 1081 && own != 3670) return;
    if (!nt_trace_line()) return;
    BYTE* track = *(BYTE**)(*(BYTE**)GA(0x1ee3548) + 0x30);
    char line[1000];
    int n = sprintf_s(line, "fired %d tick=%lu chain:", own, track ? *(DWORD*)(track + 0x14) : 0);
    int last = -2;
    const DWORD* top = (const DWORD*)__readfsdword(4);           // TEB StackBase (exclusive)
    for (int k = 0; k < 0x1400 && stack + k < top && n > 0 && n < 900; ++k) {
        int idx = nt_node_owner_index(stack[k]);
        if (idx >= 0 && idx != last && idx != own) { n += sprintf_s(line + n, sizeof line - n, " %d@%lx:%d", idx, *(DWORD*)stack[k], *((BYTE*)stack[k] + 0x70)); last = idx; }
    }
    nt_log(line);
}

void object_world_position(void* object, float out[3]);   // dlc_portals.inl
// EventState execute 0x61d720 (mov eax,400Ch = 5 bytes, relocatable): log owner, mode, tick, targets.
typedef unsigned char(__fastcall* StateExecFn)(void*, void*);
StateExecFn g_state_exec;
void portal_state_event(void* self);
unsigned char __fastcall nt_state_exec(void* self, void*) {
    if (g_nt_trace && nt_trace_line()) {
        BYTE* track = *(BYTE**)(*(BYTE**)GA(0x1ee3548) + 0x30);
        char line[200];
        sprintf_s(line, "state %d enabled=%d mode=%lu tick=%lu", nt_object_index(((void**)self)[1]),
            *((BYTE*)self + 0x70), *(DWORD*)((BYTE*)self + 0xb0), track ? *(DWORD*)(track + 0x14) : 0);
        nt_log(line);
        int own = nt_object_index(((void**)self)[1]);
        if (own == 3629 || own == 3639) {
            DWORD marker;
            const DWORD* stack = &marker;
            const DWORD* top = (const DWORD*)__readfsdword(4);
            char chain[1000];
            int n = sprintf_s(chain, "state %d chain:", own), last = -2;
            for (int k = 0; k < 0x1400 && stack + k < top && n > 0 && n < 900; ++k) {
                int idx = nt_node_owner_index(stack[k]);
                if (idx >= 0 && idx != last && idx != own) {
                    n += sprintf_s(chain + n, sizeof chain - n, " %d@%lx:%d", idx, *(DWORD*)stack[k], *((BYTE*)stack[k] + 0x70));
                    last = idx;
                }
            }
            nt_log(chain);
            if (own == 3629 && g_nt_objects) {
                void** owners = *(void***)(g_nt_objects + 8);
                DWORD count = *(DWORD*)g_nt_objects;
                char where[400];
                int w = sprintf_s(where, "run-over context:");
                if (track && *(DWORD*)(track + 0x1cc) && *(BYTE**)(track + 0x1bc)) {
                    float b[3];
                    ((float*(__fastcall*)(void*, void*, float*))GA(0x45ad50))(*(BYTE**)(track + 0x1bc), nullptr, b);
                    w += sprintf_s(where + w, sizeof where - w, " bike (%.1f,%.1f,%.1f)", b[0], b[1], b[2]);
                }
                const unsigned vols[3] = {309, 3672, 3673};
                for (unsigned v : vols)
                    if (v < count && owners[v]) {
                        float p[3];
                        object_world_position(owners[v], p);
                        w += sprintf_s(where + w, sizeof where - w, " vol%u (%.1f,%.1f,%.1f)", v, p[0], p[1], p[2]);
                    }
                nt_log(where);
            }
        }
    }
    unsigned char result = g_state_exec(self, nullptr);
    portal_state_event(self);
    return result;
}

// Return addresses inside the exe around the park chain (to locate the impulse dispatcher).
void nt_trace_code_addresses(const DWORD* stack) {
    const DWORD* top = (const DWORD*)__readfsdword(4);
    char line[700];
    int n = sprintf_s(line, "park stack code:");
    for (int k = 0; k < 0x200 && stack + k < top && n > 0 && n < 640; ++k)
        if (stack[k] >= GA(0x5f0000) && stack[k] < GA(0x640000)) n += sprintf_s(line + n, sizeof line - n, " %lx", stack[k]);
    nt_log(line);
}

// Experiment (datapack\trials_revive_nt_nopark.txt): park-all 1078 runs only at race start, so the
// sections 2874 places at tick 1 stay where they are. Skips like a disabled transform (returns 1).
bool g_nt_nopark;
unsigned char __fastcall nt_transform_exec(void* self, void*) {
    DWORD marker;
    static DWORD placed_tick = 0xffffffff;      // tick of the last 2874 place
    int own_index = g_nt_nopark ? nt_object_index(((void**)self)[1]) : -1;
    if (own_index == 1078 || own_index == 2874) {
        BYTE* track = *(BYTE**)(*(BYTE**)GA(0x1ee3548) + 0x30);
        DWORD tick = track ? *(DWORD*)(track + 0x14) : 0;
        if (own_index == 2874) placed_tick = tick;
        else if (placed_tick != 0xffffffff && tick > placed_tick) {
            static bool told;
            if (!told) { told = true; nt_log("nopark: skipped a later park-all 1078"); }
            *(DWORD*)((BYTE*)self + 8) = tick;
            return 1;
        }
    }
    if (g_nt_trace) nt_trace_callers(self, &marker);
    if (g_nt_trace && nt_object_index(((void**)self)[1]) == 1078 && nt_trace_line()) nt_trace_code_addresses(&marker);
    if (g_nt_trace && g_nt_trace_lines < NT_TRACE_MAX && nt_first_runs(((void**)self)[1])) {
        void* owner = ((void**)self)[1];
        bool fx, fy, fz;
        float x = nt_input_value(owner, 390, &fx), y = nt_input_value(owner, 391, &fy),
              z = nt_input_value(owner, 392, &fz);
        if (nt_trace_line()) {
            char line[240];
            sprintf_s(line, "transform %d enabled=%d inputs x=%s%g y=%s%g z=%s%g",
                nt_object_index(owner), *((BYTE*)self + 0x70),
                fx ? "" : "(none)", x, fy ? "" : "(none)", y, fz ? "" : "(none)", z);
            nt_log(line);
        }
    }
    return g_transform_exec(self, nullptr);
}

void nt_trace_set_vector(VectorState* s, const float* values, unsigned width) {
    if (!nt_trace_line()) return;
    char line[260];
    int n = sprintf_s(line, "set_vector %d values=(%g,%g,%g) width=%u targets:",
        nt_object_index(((void**)s->self)[1]), values[0], values[1], values[2], width);
    for (unsigned k = 0; k < s->target_count && k < 6 && n > 0 && n < 220; ++k)
        n += sprintf_s(line + n, sizeof line - n, " %d", nt_object_index(s->targets[k]));
    nt_log(line);
}

void nt_trace_vector(VectorState* s, const char* what) {
    if (!g_nt_trace || !nt_first_runs(s->self) || !nt_trace_line()) return;
    char line[200];
    sprintf_s(line, "%s kind=%d object=%d values=(%g,%g,%g)", what, s->kind,
        nt_object_index(((void**)s->self)[1]), s->values[0], s->values[1], s->values[2]);
    nt_log(line);
}

void start_portal_state_hook() {
    if (g_state_exec) return;
    const BYTE expected[] = {0xb8, 0x0c, 0x40, 0x00, 0x00};
    if (code_differs((uintptr_t)GA(0x61d720), expected, 5)) return;
    BYTE* st = (BYTE*)VirtualAlloc(nullptr, 10, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!st) return;
    memcpy(st, expected, 5); st[5] = 0xe9;
    *(int*)(st + 6) = (int)(GA(0x61d725) - ((uintptr_t)st + 10));
    g_state_exec = (StateExecFn)st;
    if (patch_jmp(GA(0x61d720), expected, 5, (void*)&nt_state_exec)) {
        shim_log("dlc runtime: portal EventState enable/disable/toggle enabled");
    } else g_state_exec = nullptr;
}

void start_nt_trace() {
    if (!game_file_exists("trials_revive_nt_trace.txt")) return;
    const BYTE expected[] = {0x55,0x8b,0xec,0x83,0xe4,0xf0};      // EventObjectTransform slot 3
    if (code_differs((uintptr_t)GA(0x617810), expected, sizeof expected)) return;
    BYTE* trampoline = (BYTE*)VirtualAlloc(nullptr, 11, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) return;
    memcpy(trampoline, expected, 6); trampoline[6] = 0xe9;
    *(int*)(trampoline + 7) = (int)(GA(0x617816) - ((uintptr_t)trampoline + 11));
    g_transform_exec = (TransformExecFn)trampoline;
    g_nt_trace = true;
    g_nt_nopark = game_file_exists("trials_revive_nt_nopark.txt");
    if (g_nt_nopark) shim_log("nt trace: nopark experiment ON (park-all 1078 only at race start)");
    if (patch_jmp(GA(0x617810), expected, sizeof expected, (void*)&nt_transform_exec))
        shim_log("nt trace: generation-chain trace enabled (transforms, retargetors, set-vector, vectors)");
    start_portal_state_hook();
}
