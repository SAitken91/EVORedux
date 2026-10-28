#include <math.h>
// DLC portals (POR3, Xbox 360 TU3 feature; Neverending Trials depends on them). Experimental,
// enabled by datapack\trials_revive_portals.txt.
//
// Record layout = TU3 reader 0x8254c018 (v3 path 0x8254ce64), big-endian, one record per portal
// object in OBJX order: b b b, f32 size, u32, b b b b b, u32 delay(ticks), b entry(+08), b,
// u8, u8 hit mask, u8 n, n object refs (u16; 0xfffe + u32 = external), 4 bytes of empty lists.
// TU3 enabled is byte0; byte20 only controls the visual effect. Neverending's
// attached-bike relocation uses the dedicated whole-body implementation below.
struct DlcPortal {
    unsigned index;          // object index in the track's object array
    void* owner;
    void* target;
    float size;
    DWORD delay;
    BYTE effect_visible, flags[10], hit_mask;
    unsigned target_index, active_target;
    bool enabled;
};
DlcPortal g_portals[32];
unsigned g_portal_count;
BYTE* g_portal_objects;
BYTE* g_portal_track;
BYTE* g_portal_active_track;
void* g_portal_hits[256];
unsigned g_portal_hit_count;
bool g_portals_enabled;
int g_portal_cooldown;
volatile LONG g_portal_log_lines;
DWORD g_portal_race_tick = 0xffffffff;
int g_portal_pending = -1;
DWORD g_portal_pending_tick;
float g_portal_pending_center[3];

bool portal_model(DWORD model) { return model == 0x00ac7639 || model == 0xb2af645d; }

void portal_log(const char* line) {
    if (InterlockedIncrement(&g_portal_log_lines) > 900) return;
    char out[300];
    sprintf_s(out, "portal: %s", line);
    shim_log(out);
}

DWORD be32(const BYTE* p) { return (DWORD)p[0] << 24 | (DWORD)p[1] << 16 | (DWORD)p[2] << 8 | p[3]; }

void portal_forget_track() {
    g_portal_count = 0;
    g_portal_objects = nullptr;
    g_portal_track = nullptr;
    g_portal_active_track = nullptr;
    g_portal_hit_count = 0;
    g_portal_pending = -1;
}

void portal_register_hit(void* self) {
    if (g_portals_enabled && g_portal_count && g_portal_hit_count < 256) {
        g_portal_hits[g_portal_hit_count++] = self;
        char line[160];
        sprintf_s(line, "registered Hit owner=%d self=%p", nt_object_index(*(void**)((BYTE*)self + 0x4c)), self);
        portal_log(line);
    }
}

bool portal_hit_watches(BYTE* hit, void* portal) {
    for (unsigned side = 0; side < 2; ++side) {
        BYTE* table = hit + 0x64 + side * 0x20;
        if (*(DWORD*)(table + 0xc) && table_has(table, (DWORD)portal)) return true;
    }
    return false;
}

__declspec(noinline) void portal_reset_hits() {
    for (unsigned i = 0; i < g_portal_hit_count; ++i) {
        BYTE* hit = (BYTE*)g_portal_hits[i];
        bool watches = false;
        for (unsigned j = 0; j < g_portal_count; ++j)
            if (portal_hit_watches(hit, g_portals[j].owner)) watches = true;
        if (!watches) continue;
        // Native Hit reset restores an automatic one-shot disable, but leaves
        // an explicit EventState OFF in place. Our portal restart already
        // restores POR3 enabled states, so restore its matching Hit's authored
        // enabled state (native saved copy at +0x62) through the native setter.
        ((void(__fastcall*)(void*, void*, bool))(*(DWORD**)hit)[3])
            (hit, nullptr, *(hit + 0x62) != 0);
    }
}

// Portals have no native collision object on PC. Forward their bike contact to
// the real TriggerHit callback, which owns selection masks, one-shot state,
// timestamps and impulse dispatch. Do not fire the track's generation events
// directly. Callback ABI (60c170): scene-pair, unused, category-pair, details.
void portal_hit_bike(void* portal, BYTE* bike) {
    void* pair[2] = {portal, *(void**)(bike + 0x4dc)};
    const BYTE categories[2] = {0, 1}; // explicit portal object; attached bike
    const DWORD details[4] = {};
    if (g_nt_trace) InterlockedExchange(&g_nt_trace_lines, 0); // retain the contact's ensuing generation trace
    char line[260];
    sprintf_s(line, "CONTACT object=%d registered-hits=%u", nt_object_index(portal), g_portal_hit_count);
    portal_log(line);
    for (unsigned i = 0; i < g_portal_hit_count; ++i) {
        BYTE* hit = (BYTE*)g_portal_hits[i];
        // Only native Hit triggers explicitly watching this portal participate;
        // unrelated volume and ground contacts remain entirely native.
        bool watches = portal_hit_watches(hit, portal);
        sprintf_s(line, "Hit owner=%d watches=%u enabled=%u last=%lu first=%lu masks=%lx,%lx output0=%p",
            nt_object_index(*(void**)(hit + 0x4c)), watches, hit[0x50], *(DWORD*)(hit + 0x58),
            *(DWORD*)(hit + 0x5c), *(DWORD*)(hit + 0xe4), *(DWORD*)(hit + 0xe8), *(void**)(hit + 4));
        portal_log(line);
        BYTE* manager = *(BYTE**)(*(BYTE**)GAME_GLOBAL + 0x60);
        if (manager && *(DWORD*)(manager + 0xb0)) {
            void* active = ((void*(__fastcall*)(void*, void*, void*))GA(0x5fd960))
                (manager, nullptr, *(void**)(hit + 0x4c));
            sprintf_s(line, "Hit live-lookup registered=%p active=%p", hit, active);
            portal_log(line);
        }
        if (watches) {
            ((void(__fastcall*)(void*, void*, const void*, const void*, const void*, const void*))GA(0x60c170))
                (hit, nullptr, pair, nullptr, categories, details);
            sprintf_s(line, "Hit after owner=%d enabled=%u last=%lu first=%lu",
                nt_object_index(*(void**)(hit + 0x4c)), hit[0x50], *(DWORD*)(hit + 0x58), *(DWORD*)(hit + 0x5c));
            portal_log(line);
        }
    }
}

// Called with the raw POR3 bytes (already read past in the stream).
void load_dlc_portals(const BYTE* data, DWORD size, BYTE* track) {
    portal_forget_track();
    if (!g_portals_enabled) return;
    BYTE* objects = track + 0x24a08;
    DWORD count = *(DWORD*)objects;
    void** owners = *(void***)(objects + 8);
    if (!owners || count > 32768) return;
    DWORD p = 0;
    for (DWORD k = 0; k < count; ++k) {
        if (!owners[k] || !portal_model(object_model(owners[k]))) continue;
        if (g_portal_count == 32 || p + 25 > size) { g_portal_count = 0; return; }
        DlcPortal& q = g_portals[g_portal_count];
        const BYTE* r = data + p;
        q.index = k; q.owner = owners[k]; q.target = nullptr;
        q.target_index = 0xffffffff;
        DWORD raw = be32(r + 3); memcpy(&q.size, &raw, 4);
        const int bools[10] = {0, 1, 2, 11, 12, 13, 14, 15, 20, 21};   // byte offsets of the flags
        for (int j = 0; j < 10; ++j) q.flags[j] = r[bools[j]];
        q.delay = be32(r + 16);
        q.effect_visible = r[20];
        q.enabled = r[0] != 0;
        q.active_target = be32(r + 7);
        q.hit_mask = r[23];
        unsigned n = r[24];
        p += 25;
        for (unsigned j = 0; j < n; ++j) {
            if (p + 2 > size) { g_portal_count = 0; return; }
            unsigned ref = (unsigned)data[p] << 8 | data[p + 1];
            p += 2;
            if (ref == 0xfffe) {
                if (p + 4 > size) { g_portal_count = 0; return; }
                p += 4; continue; // external portal targets remain unsupported
            }
            if (n && j == q.active_target % n && ref < count) {
                q.target_index = ref; q.target = owners[ref];
            }
        }
        if (p + 4 > size || data[p] | data[p + 1] | data[p + 2] | data[p + 3]) { g_portal_count = 0; return; }
        p += 4;
        ++g_portal_count;
    }
    if (p != size) { g_portal_count = 0; return; }
    g_portal_objects = objects;
    g_portal_track = track;
    g_portal_cooldown = 0;
    g_portal_race_tick = 0xffffffff;
    g_portal_pending = -1;
    for (unsigned i = 0; i < g_portal_count; ++i) {
        char line[200];
        sprintf_s(line, "loaded portal %u: object %u enabled=%u visible=%u size=%g delay=%lu hit=%02x target-index=%u",
            i, g_portals[i].index, g_portals[i].enabled, g_portals[i].effect_visible, g_portals[i].size,
            g_portals[i].delay, g_portals[i].hit_mask, g_portals[i].target_index);
        portal_log(line);
    }
}

// Native EventState owns a linked list of object targets at +74's +14.
// Its existing execution handles the PC classes; this adds TU3's missing
// PortalManager branch. Outgoing impulses are still dispatched by the engine.
void portal_state_event(void* self) {
    if (!g_portals_enabled || !g_portal_count || !*((BYTE*)self + 0x70)) return;
    DWORD mode = *(DWORD*)((BYTE*)self + 0xb0);
    if (mode > 2) return;
    BYTE* node = *(BYTE**)((BYTE*)self + 0x88);
    for (unsigned n = 0; node && n < 32768; ++n, node = *(BYTE**)(node + 4)) {
        void* target = resolve_dlc_retarget(*(void**)(node + 8));
        for (unsigned i = 0; i < g_portal_count; ++i) {
            DlcPortal& p = g_portals[i];
            if (p.owner != target) continue;
            p.enabled = mode == 2 ? !p.enabled : mode == 1;
        }
    }
}

#include "dlc_portal_bike.inl"

void object_world_position(void* object, float out[3]) {
    BYTE native[0x44] = {};
    *(void**)(native + 0x3c) = object;
    for (unsigned k = 0; k < 3; ++k) {
        *(unsigned*)(native + 0x40) = k;
        out[k] = ((float(__fastcall*)(void*, void*))GA(0x62b410))(native, nullptr);
    }
}

// Debug helpers below inspect fixed object indices of Neverending Trials (424) only.
bool portal_is_neverending(void** owners, DWORD count) {
    return count > 3506 && owners[1028] && owners[3491] && owners[3506] &&
           object_model(owners[1028]) == 0x42643009 && object_model(owners[3491]) == 0x42643009 &&
           object_model(owners[3506]) == 0x42643009;
}


// Pending portal delay holds the whole rigid bike at its entry pose until the
// authored delay expires. The destination is resolved at commit, after the
// track's own impulses have positioned the next section and its exit portal.
void portal_reset_race() {
    for (unsigned i = 0; i < g_portal_count; ++i) g_portals[i].enabled = g_portals[i].flags[0] != 0;
    g_portal_pending = -1;
    g_portal_cooldown = 0;
    portal_reset_hits();
}

bool portal_complete_warp(BYTE* bike, DlcPortal& q) {
    if (!q.target) return false;
    float source[3], destination[3], from_quat[4], to_quat[4], delta[4], center[3];
    object_world_position(q.owner, source);
    object_world_position(q.target, destination);
    if (!portal_bike_center(bike, center)) return false;
    ((float*(__fastcall*)(void*, void*, float*))GA(0x7d7850))(q.owner, nullptr, from_quat);
    ((float*(__fastcall*)(void*, void*, float*))GA(0x7d7850))(q.target, nullptr, to_quat);
    portal_exit_rotation(bike, from_quat, to_quat, q.flags[7] != 0, delta);
    if (q.flags[2]) { // Preserve Relative Position
        float inverse[4] = {-from_quat[0],-from_quat[1],-from_quat[2],from_quat[3]};
        float relative[3], local[3], rotated[3];
        for (unsigned k = 0; k < 3; ++k) relative[k] = center[k] - source[k];
        portal_native_rotate(inverse, relative, local);
        const BYTE* src_node = *(BYTE**)((BYTE*)q.owner + 0x24);
        const BYTE* dst_node = *(BYTE**)((BYTE*)q.target + 0x24);
        float src_scale = src_node ? *(float*)(src_node + 0x24) : 1.0f;
        float dst_scale = dst_node ? *(float*)(dst_node + 0x24) : 1.0f;
        if (!(src_scale > 0.0001f) || !(dst_scale > 0.0001f)) return false;
        for (unsigned k = 0; k < 3; ++k) local[k] *= dst_scale / src_scale;
        portal_native_rotate(to_quat, local, rotated);
        for (unsigned k = 0; k < 3; ++k) destination[k] += rotated[k];
    }
    const float axis[3] = {1,0,0};
    float forward[3]; portal_native_rotate(to_quat, axis, forward);
    if (!portal_warp_bike_physics(bike, destination, delta, forward, q.flags[3] != 0, q.flags[6] != 0)) return false;
    float after[3]; portal_bike_center(bike, after);
    char line[260];
    sprintf_s(line, "PHYSICS TELEPORT %u -> %u: center (%.2f,%.2f,%.2f) -> (%.2f,%.2f,%.2f) target (%.2f,%.2f,%.2f)",
        q.index, q.target_index, center[0],center[1],center[2],after[0],after[1],after[2],
        destination[0],destination[1],destination[2]);
    portal_log(line);
    return true;
}

void portal_tick() {
    if (!g_portal_count || !g_portal_objects) return;
    BYTE* track = *(BYTE**)(*(BYTE**)GAME_GLOBAL + 0x30);
    // Loader TrackData and active gameplay Track are different classes/pointers.
    // Bind the latter on the first race tick; never compare it to TrackData.
    if (!track || !*(DWORD*)(track + 0x1cc)) return;
    if (g_portal_active_track && track != g_portal_active_track) { portal_forget_track(); return; }
    g_portal_active_track = track;
    void** owners = *(void***)(g_portal_objects + 8);
    DWORD count = *(DWORD*)g_portal_objects;
    if (!owners || count > 32768) return;
    for (unsigned i = 0; i < g_portal_count; ++i) {
        DlcPortal& q = g_portals[i];
        if (q.index >= count || !owners[q.index] || !portal_model(object_model(owners[q.index]))) {
            g_portal_count = 0; g_portal_pending = -1; return;
        }
        q.owner = owners[q.index];
        // OBJX pointers can be populated/replaced after POR3 is read.
        q.target = q.target_index < count ? owners[q.target_index] : nullptr;
    }
    BYTE* bike = *(BYTE**)(track + 0x1bc);
    if (!bike || !*(void**)(bike + 0x4dc)) return;
    bool neverending = portal_is_neverending(owners, count);
    // Gateways needs native portal scale, driving-line projection and contact-volume semantics.
    // Its unscaled sphere approximation teleported too early in the live test; leave it gated.
    if (!neverending) return;
    DWORD tick = *(DWORD*)(track + 0x14);
    if (g_portal_race_tick != 0xffffffff && tick < g_portal_race_tick) portal_reset_race();
    g_portal_race_tick = tick;
    if (g_portal_cooldown > 0) --g_portal_cooldown;
    float center[3];
    if (!portal_bike_center(bike, center)) return;
    if (g_portal_pending >= 0) {
        unsigned index = (unsigned)g_portal_pending;
        if (index >= g_portal_count) { g_portal_pending = -1; return; }
        DlcPortal& q = g_portals[index];
        if (tick - g_portal_pending_tick < q.delay) {
            const float identity[4] = {0,0,0,1}, forward[3] = {1,0,0};
            portal_warp_bike_physics(bike, g_portal_pending_center, identity, forward, false, false);
            return;
        }
        bool done = portal_complete_warp(bike, q);
        g_portal_pending = -1;
        g_portal_cooldown = done ? 30 : 1;
        if (!done) q.enabled = q.flags[0] != 0;
        return;
    }
    if (g_portal_cooldown) return;
    for (unsigned i = 0; i < g_portal_count; ++i) {
        DlcPortal& q = g_portals[i];
        if (!q.enabled || !q.target) continue;
        float src[3]; object_world_position(q.owner, src);
        float dx=center[0]-src[0],dy=center[1]-src[1],dz=center[2]-src[2];
        float radius = 2.0f * q.size;
        if (!(radius > 0.0f) || !(dx*dx+dy*dy+dz*dz < radius*radius)) continue;
        if (q.flags[1]) q.enabled = false; // Disable After Hit, restored by EventState
        if (q.delay) {
            g_portal_pending = (int)i;
            g_portal_pending_tick = tick;
            memcpy(g_portal_pending_center, center, sizeof center);
            portal_hit_bike(q.owner, bike);
        } else {
            portal_hit_bike(q.owner, bike);
            bool done = portal_complete_warp(bike, q);
            g_portal_cooldown = done ? 30 : 1;
            if (!done) q.enabled = q.flags[0] != 0;
        }
        return;
    }
}

typedef void(__fastcall* TickFn)(void*, void*);
TickFn g_tick_original;
void __fastcall portal_tick_hook(void* self, void*) {
    g_tick_original(self, nullptr);
    portal_tick();
}

void start_dlc_portals() {
    g_portals_enabled = game_file_exists("trials_revive_portals.txt");
    if (!g_portals_enabled) return;
    start_portal_state_hook();
    const BYTE expected[] = {0x83,0xec,0x14,0xa1,0x48,0x35,0xee,0x01};   // GameProcessor::runTickLogic
    if (code_differs((uintptr_t)GA(0x47c510), expected, sizeof expected)) { g_portals_enabled = false; return; }
    BYTE* trampoline = (BYTE*)VirtualAlloc(nullptr, 13, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) { g_portals_enabled = false; return; }
    // Copy the live bytes: they hold the game object's absolute address, which differs per build.
    memcpy(trampoline, (const void*)GA(0x47c510), 8); trampoline[8] = 0xe9;
    *(int*)(trampoline + 9) = (int)(GA(0x47c518) - ((uintptr_t)trampoline + 13));
    g_tick_original = (TickFn)trampoline;
    if (patch_jmp(GA(0x47c510), expected, sizeof expected, (void*)&portal_tick_hook))
        shim_log("portal: Neverending whole-bike physics, portal states and driving-line relocation enabled");
    else
        g_portals_enabled = false;
}
