// PC implementation of the missing TU3 Bike::teleport physics work.
// Reference: TU3 82383b28..82384350, NOT the generic object portal routine.
// Include after the existing portal helpers, inside dlc_fixes.cpp's namespace.
// Parent supplies the destination centre and game-space rotation delta from POR3.

struct PortalNativeBodyList {
    void* items[4092];
    unsigned count;                         // native fixed-vector count is +0x3ff0
    BYTE padding[12];
};
static_assert(sizeof(PortalNativeBodyList) == 0x4000, "native body-list ABI");

void portal_native_rotate(const float game_rotation[4], const float vector[3], float out[3]) {
    float matrix[16], one[3] = {1,1,1}, zero[3] = {};
    ((void(__fastcall*)(void*, void*, const float*, const float*, const float*))GA(0x7d49b0))
        (matrix, nullptr, game_rotation, one, zero);
    ((float*(__fastcall*)(void*, void*, float*, const float*))GA(0x46d100))
        (matrix, nullptr, out, vector);
}

// TU3 8254f3a0..8254f538. The multiplication order follows the game's row-vector
// convention: inverse(source orientation) first, exit orientation second.
void portal_exit_rotation(BYTE* bike, const float source_quat[4], const float destination_quat[4],
                          bool preserve_orientation, float out[4]) {
    float from[4], inverse[4];
    if (preserve_orientation) memcpy(from, source_quat, sizeof from);
    else ((float*(__fastcall*)(void*, void*, float*))GA(0x45adf0))(bike, nullptr, from);
    for (unsigned k = 0; k < 3; ++k) inverse[k] = -from[k];
    inverse[3] = from[3];
    ((float*(__fastcall*)(void*, void*, float*, const float*))GA(0x46b570))
        (inverse, nullptr, out, destination_quat);
}

// Returns the current centre of every physical bike/rider body; the 360 uses
// this mean as its pivot, rather than moving just the rendered bike root.
bool portal_bike_center(BYTE* bike, float out[3]) {
    if (!bike || !*(void**)(bike + 0x4dc)) return false;
    PortalNativeBodyList bodies = {};
    ((void(__fastcall*)(void*, void*, void*, int, int, int, int))GA(0x7d7c60))
        (*(void**)(bike + 0x4dc), nullptr, &bodies, 3, 1, 0, 0);
    if (!bodies.count || bodies.count > 4092) return false;
    out[0] = out[1] = out[2] = 0;
    unsigned n = 0;
    for (unsigned k = 0; k < bodies.count; ++k) {
        BYTE* body = (BYTE*)bodies.items[k];
        BYTE* physics = body ? *(BYTE**)(body + 0x84) : nullptr;
        if (!physics || body[0x8c]) continue;
        for (unsigned j = 0; j < 3; ++j) out[j] += *(float*)(physics + 0x34 + j * 4);
        ++n;
    }
    if (!n) return false;
    for (unsigned j = 0; j < 3; ++j) out[j] /= (float)n;
    return true;
}

// The native PC projection has the same nearest-segment algorithm as TU3
// 823057a8. Reset the search index to zero so a portal can cross many segments.
// Physics uses the returned projected line while keeping each body's height.
void portal_reproject_bike(BYTE* bike) {
    void* track = *(void**)(bike + 0x3c);
    BYTE* path = *(BYTE**)(bike + 0x614);
    if (!track || !path || *(unsigned*)(path + 0x20) < 3) return;
    BYTE* node = *(BYTE**)(bike + 0x0c);
    for (unsigned count = 0; node && count < 4092; ++count, node = *(BYTE**)(node + 4)) {
        BYTE* part = *(BYTE**)(node + 8);
        BYTE* physics = part ? *(BYTE**)(part + 0x84) : nullptr;
        if (!physics || !*(void**)(part + 0x7c)) continue;
        float position[3], projected[2] = {}, direction[2] = {}, parameter = 0;
        memcpy(position, physics + 0x34, sizeof position);
        unsigned index = 0;
        ((float*(__fastcall*)(void*, void*, float*, void*, unsigned*, const float*, float*, float*))GA(0x485be0))
            (track, nullptr, projected, path, &index, position, direction, &parameter);
        *(unsigned*)(part + 0x90) = index;
        BYTE* def = *(BYTE**)(part + 0x24);
        DWORD name = def ? *(DWORD*)(def + 0x30) : 0;
        if (name == 0xf84642f0 || name == 0x8d32199b) {
            // TU3 preserves orientation and height while bringing the two
            // suspension anchors halfway onto the selected driving line.
            *(float*)(physics + 0x34) = (position[0] + projected[0]) * 0.5f;
            *(float*)(physics + 0x3c) = (position[2] + projected[1]) * 0.5f;
        }
        if (name == 0x1329aad5) {
            *(unsigned*)(bike + 0xa8) = index;
            *(float*)(bike + 0xac) = parameter;
            *(float*)(bike + 0x94) = direction[0];
            *(float*)(bike + 0x98) = direction[1];
        }
        if (name == 0x5bd7085d) {
            *(unsigned*)(bike + 0xb0) = index;
            *(float*)(bike + 0xb4) = parameter;
        }
    }
}

// Faithful whole-bike core for the normal (attached bike and rider) portal case.
// The caller computes the portal's relative-position and orientation options.
// Keep-velocity-direction in TU3 means rotate each saved velocity with the warp;
// otherwise preserve speed along the exit portal's forward direction.
bool portal_warp_bike_physics(BYTE* bike, const float destination[3], const float rotation[4],
                             const float forward[3], bool keep_velocity, bool keep_direction) {
    if (!bike || !*(void**)(bike + 0x4dc)) return false;
    BYTE* path = *(BYTE**)(bike + 0x614);
    if (!*(void**)(bike + 0x3c) || !path || *(unsigned*)(path + 0x20) < 3) return false;
    PortalNativeBodyList bodies = {};
    ((void(__fastcall*)(void*, void*, void*, int, int, int, int))GA(0x7d7c60))
        (*(void**)(bike + 0x4dc), nullptr, &bodies, 3, 1, 0, 0);
    if (!bodies.count || bodies.count > 4092) return false;
    float center[3] = {};
    unsigned count = 0;
    for (unsigned k = 0; k < bodies.count; ++k) {
        BYTE* b = (BYTE*)bodies.items[k];
        BYTE* p = b ? *(BYTE**)(b + 0x84) : nullptr;
        if (!p || b[0x8c]) continue;
        for (unsigned j = 0; j < 3; ++j) center[j] += *(float*)(p + 0x34 + j * 4);
        ++count;
    }
    if (!count) return false;
    for (unsigned j = 0; j < 3; ++j) center[j] /= (float)count;
    const float scale[3] = {1,1,1};
    for (unsigned k = 0; k < bodies.count; ++k) {
        BYTE* body = (BYTE*)bodies.items[k];
        BYTE* physics = body ? *(BYTE**)(body + 0x84) : nullptr;
        if (!physics || body[0x8c]) continue;
        float old[3], offset[3], moved[3], linear[3], angular[3], body_quat[4], new_quat[4], matrix[16];
        memcpy(old, physics + 0x34, sizeof old);
        memcpy(linear, physics + 0x13c, sizeof linear);
        memcpy(angular, physics + 0x14c, sizeof angular);
        for (unsigned j = 0; j < 3; ++j) offset[j] = old[j] - center[j];
        portal_native_rotate(rotation, offset, moved);
        for (unsigned j = 0; j < 3; ++j) moved[j] += destination[j];
        ((float*(__fastcall*)(void*, void*, float*))GA(0x7cf770))(body, nullptr, body_quat);
        ((float*(__fastcall*)(void*, void*, float*, const float*))GA(0x46b570))
            (body_quat, nullptr, new_quat, rotation);
        // Physics quaternion convention has the opposite W from scene objects.
        new_quat[3] = -new_quat[3];
        ((void(__fastcall*)(void*, void*, const float*, const float*, const float*))GA(0x7d49b0))
            (matrix, nullptr, new_quat, scale, moved);
        ((void(__fastcall*)(void*, void*, const float*))GA(0x4543c0))(physics, nullptr, matrix);
        if (!keep_velocity) {
            memset(linear, 0, sizeof linear); memset(angular, 0, sizeof angular);
        } else if (keep_direction) {
            float transformed[3]; portal_native_rotate(rotation, linear, transformed);
            memcpy(linear, transformed, sizeof linear);
        } else {
            float speed = sqrtf(linear[0]*linear[0] + linear[1]*linear[1] + linear[2]*linear[2]);
            for (unsigned j = 0; j < 3; ++j) linear[j] = forward[j] * speed;
        }
        ((void(__fastcall*)(void*, void*, const float*))GA(0x7d4820))(body, nullptr, linear);
        ((void(__fastcall*)(void*, void*, const float*))GA(0x7d48e0))(body, nullptr, angular);
    }
    portal_reproject_bike(bike);
    return true;
}
