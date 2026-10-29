// Gateways' attached bike path. TU3 8255169c saves state, 8254a928 pauses
// both physical groups, 8254f320 warps them, and 8254aaf0 restores state.
// Keep Neverending's already-tested generation loop separate.
struct GatewaysMotion { BYTE* body; float linear[3], angular[3]; };
GatewaysMotion g_gateways_motion[128];
unsigned g_gateways_motion_count;
BYTE* g_gateways_bike;
BYTE g_gateways_saved_flags[4];
float g_gateways_relative[3], g_gateways_previous[3];
bool g_gateways_previous_valid;
int g_gateways_exit = -1;
bool g_gateways_supported = true;
DWORD g_gateways_near_tick[32];
DWORD g_gateways_camera_tick = 0xffffffff;
typedef void(__fastcall* GatewaysCameraFn)(void*,void*);
GatewaysCameraFn g_gateways_camera_original;

bool portal_complete_warp(BYTE* bike, DlcPortal& q);

bool portal_is_gateways() {
    // boards.json identifies Gateways as OoP track 210. Match its complete
    // authored POR3 signature; track 204 is a different portal level.
    static const unsigned indices[] = {1008,1009,1872,1873,2300,2301,3249,3250};
    if (!g_gateways_supported || g_portal_count != _countof(indices)) return false;
    for (unsigned i=0;i<_countof(indices);++i)
        if (g_portals[i].index!=indices[i] || g_portals[i].target_index!=indices[i^1]) return false;
    return true;
}

void gateways_check_native() {
    const BYTE group[] = {0x55,0x8b,0xec,0x83,0xe4,0xf8,0xb8,0x54,0x40,0,0};
    const BYTE bike_pause[] = {0x8b,0x44,0x24,0x04,0x56,0x8b,0xf1,0x88,0x86,0x44,0x06,0,0};
    const BYTE rider_pause[] = {0x8b,0x44,0x24,0x04,0x56,0x8b,0xf1,0x88,0x86,0x45,0x06,0,0};
    g_gateways_supported = !code_differs(GA(0x457e70),group,sizeof group) &&
        !code_differs(GA(0x458080),group,sizeof group) &&
        !code_differs(GA(0x4582a0),bike_pause,sizeof bike_pause) &&
        !code_differs(GA(0x4582e0),rider_pause,sizeof rider_pause);
    if (!g_gateways_supported) shim_log("portal: Gateways disabled: native pause code differs");
}

void portal_cancel_gateways() {
    // On checkpoint reset the native bike state has already been restored.
    g_gateways_bike = nullptr;
    g_gateways_motion_count = 0;
    g_gateways_previous_valid = false;
    g_gateways_exit = -1;
    memset(g_gateways_near_tick,0,sizeof g_gateways_near_tick);
    g_gateways_camera_tick = 0xffffffff;
}

void gateways_camera_warp(const float before[3],const float after[3]) {
    BYTE* game=*(BYTE**)GAME_GLOBAL;
    BYTE* camera=game ? *(BYTE**)(game+0x3c) : nullptr;
    if (!camera || !g_portal_active_track || !g_gateways_camera_original) return;
    // Native camera position, previous interpolation position and target eye.
    // Its orientation vectors (+24/+4c/+64) are not world-space positions.
    const unsigned positions[] = {4,0x40,0x58};
    for (unsigned i=0;i<_countof(positions);++i)
        for (unsigned j=0;j<3;++j) *(float*)(camera+positions[i]+4*j)+=after[j]-before[j];
    g_gateways_camera_tick=*(DWORD*)(g_portal_active_track+0x14)+1;
}

void __fastcall gateways_camera_update(void* self,void*) {
    g_gateways_camera_original(self,nullptr);
    if (g_gateways_camera_tick==0xffffffff || !g_portal_gateways || !g_portal_active_track) return;
    BYTE* game=*(BYTE**)GAME_GLOBAL;
    if (!game || *(void**)(game+0x3c)!=self || *(BYTE**)(game+0x30)!=g_portal_active_track) return;
    if (*(DWORD*)(g_portal_active_track+0x14)<g_gateways_camera_tick) return;
    // PC counterpart of TU3 82397ff0: reset interpolation from the freshly
    // computed target eye/orientation, after the next camera update.
    ((void(__fastcall*)(void*,void*))GA(0x472a20))(self,nullptr);
    g_gateways_camera_tick=0xffffffff;
}

void gateways_start_camera() {
    if (!g_gateways_supported) return;
    const BYTE expected[] = {0x81,0xec,0x94,0,0,0,0x56,0x8b,0xf1};
    if (code_differs(GA(0x472a90),expected,sizeof expected)) return;
    BYTE* trampoline=(BYTE*)VirtualAlloc(nullptr,14,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    if (!trampoline) return;
    memcpy(trampoline,(void*)GA(0x472a90),9);trampoline[9]=0xe9;
    *(int*)(trampoline+10)=(int)(GA(0x472a99)-((uintptr_t)trampoline+14));
    g_gateways_camera_original=(GatewaysCameraFn)trampoline;
    if (!patch_jmp(GA(0x472a90),expected,sizeof expected,(void*)&gateways_camera_update))
        g_gateways_camera_original=nullptr;
}

bool gateways_pause(BYTE* bike, const float center[3], const float source[3]) {
    if (!bike || bike[0xa0] & 1) return false; // attached bike/rider only
    PortalNativeBodyList bodies = {};
    ((void(__fastcall*)(void*, void*, void*, int, int, int, int))GA(0x7d7c60))
        (*(void**)(bike + 0x4dc), nullptr, &bodies, 3, 1, 0, 0);
    if (!bodies.count || bodies.count > 128) return false;
    g_gateways_motion_count = 0;
    for (unsigned i = 0; i < bodies.count; ++i) {
        BYTE* body = (BYTE*)bodies.items[i];
        BYTE* physics = body ? *(BYTE**)(body + 0x84) : nullptr;
        if (!physics || body[0x8c]) continue;
        GatewaysMotion& m = g_gateways_motion[g_gateways_motion_count++];
        m.body = body;
        memcpy(m.linear, physics + 0x13c, sizeof m.linear);
        memcpy(m.angular, physics + 0x14c, sizeof m.angular);
    }
    if (!g_gateways_motion_count) return false;
    g_gateways_bike = bike;
    const unsigned offsets[4] = {0x644,0x645,0x618,0x619};
    for (unsigned i = 0; i < 4; ++i) g_gateways_saved_flags[i] = bike[offsets[i]];
    for (unsigned i = 0; i < 3; ++i) g_gateways_relative[i] = center[i] - source[i];
    ((void(__fastcall*)(void*,void*,bool))GA(0x4582a0))(bike,nullptr,true);
    ((void(__fastcall*)(void*,void*,bool))GA(0x4582e0))(bike,nullptr,true);
    bike[0x618] = bike[0x619] = 1;
    return true;
}

bool gateways_resume(BYTE* bike) {
    if (!bike || bike != g_gateways_bike) { portal_cancel_gateways(); return false; }
    const unsigned offsets[4] = {0x644,0x645,0x618,0x619};
    for (unsigned i = 0; i < 4; ++i) bike[offsets[i]] = g_gateways_saved_flags[i];
    // The state setters' false path only clears a flag. Re-enable the actual
    // physics groups too, honoring any pause which was present before contact.
    ((void(__fastcall*)(void*,void*,bool))GA(0x457e70))(bike,nullptr,g_gateways_saved_flags[1] != 0);
    ((void(__fastcall*)(void*,void*,bool))GA(0x458080))(bike,nullptr,g_gateways_saved_flags[0] != 0);
    for (unsigned i = 0; i < g_gateways_motion_count; ++i) {
        GatewaysMotion& m = g_gateways_motion[i];
        ((void(__fastcall*)(void*,void*,const float*))GA(0x7d4820))(m.body,nullptr,m.linear);
        ((void(__fastcall*)(void*,void*,const float*))GA(0x7d48e0))(m.body,nullptr,m.angular);
    }
    g_gateways_bike = nullptr;
    g_gateways_motion_count = 0;
    return true;
}

void gateways_pose(BYTE* bike, DlcPortal& q, float origin[3], float quat[4]) {
    object_world_position(q.owner, origin);
    ((float*(__fastcall*)(void*,void*,float*))GA(0x7d7850))(q.owner,nullptr,quat);
    if (!q.flags[5]) return;
    BYTE* path=*(BYTE**)(bike+0x614);
    void* track=*(void**)(bike+0x3c);
    if (!path || !track || *(unsigned*)(path+0x20)<3) return;
    float projected[2],direction[2],parameter;
    unsigned index=0;
    ((float*(__fastcall*)(void*,void*,float*,void*,unsigned*,const float*,float*,float*))GA(0x485be0))
        (track,nullptr,projected,path,&index,origin,direction,&parameter);
    // Force Position To Driving Line (TU3 8254e3cc..8254e45c) keeps Y.
    origin[0]=projected[0]; origin[2]=projected[1];
}

void gateways_local(BYTE* bike, DlcPortal& q, const float point[3], float out[3]) {
    float origin[3], quat[4], relative[3];
    gateways_pose(bike,q,origin,quat);
    for (unsigned i=0;i<3;++i) { relative[i]=point[i]-origin[i]; quat[i]=-quat[i]; }
    portal_native_rotate(quat,relative,out);
}

// The portal tool is a thin cylinder, facing along local X, rather than a
// sphere extending two visual-effect radii in front of the entrance.
// Sweep the mean position through its plane so fast bikes cannot skip it.
__declspec(noinline) bool gateways_contact(const float before[3], const float now[3], float scale) {
    if (!(scale > .0001f) || !(scale < 100.f)) return false;
    float travel=now[0]-before[0];
    if (fabsf(travel)<.00001f || before[0]*now[0]>0.f) return false;
    float fraction=-before[0]/travel;
    if (fraction<0.f || fraction>1.f) return false;
    float y=before[1]+fraction*(now[1]-before[1]);
    float z=before[2]+fraction*(now[2]-before[2]);
    return y*y+z*z <= 4.f*scale*scale;
}

void gateways_tick(BYTE* bike, DWORD tick) {
    float center[3];
    if (!portal_bike_center(bike,center)) return;
    if (g_portal_pending >= 0) {
        if (bike != g_gateways_bike) { g_portal_pending=-1; portal_cancel_gateways(); return; }
        if (tick-g_portal_pending_tick < g_portals[g_portal_pending].delay) return;
        unsigned i=(unsigned)g_portal_pending;
        bool resumed=gateways_resume(bike);
        bool done=resumed && portal_complete_warp(bike,g_portals[i]);
        g_portal_pending=-1;
        g_gateways_previous_valid=false;
        g_portal_cooldown=done ? 30 : 1;
        g_gateways_exit=done ? (int)g_portals[i].target_index : -1;
        portal_log(done ? "Gateways committed; native physics resumed with captured momentum" : "Gateways warp rejected; bike resumed");
        return;
    }
    if (g_portal_cooldown>0) --g_portal_cooldown;
    if (g_gateways_previous_valid && !g_portal_cooldown && !(bike[0xa0]&1)) {
        for (unsigned i=0;i<g_portal_count;++i) {
            DlcPortal& q=g_portals[i];
            float before[3],now[3];
            if (!(q.hit_mask&0x40)) continue; // attached bike; mask 1 accepts loose objects only
            gateways_local(bike,q,g_gateways_previous,before); gateways_local(bike,q,center,now);
            BYTE* node=*(BYTE**)((BYTE*)q.owner+0x24);
            float scale=node ? *(float*)(node+0x24) : 1.f;
            float radius=scale*q.size;
            if (fabsf(now[0])<8.f && now[1]*now[1]+now[2]*now[2]<4.f*radius*radius &&
                tick-g_gateways_near_tick[i]>60) {
                char line[220];
                sprintf_s(line,"Gateways near=%u local=(%.2f,%.2f,%.2f) previous-x=%.2f radius=%.2f enabled=%u",
                    q.index,now[0],now[1],now[2],before[0],radius,q.enabled);
                portal_log(line);g_gateways_near_tick[i]=tick;
            }
            if ((int)q.index==g_gateways_exit) {
                if (fabsf(now[0])>3.f*scale || now[1]*now[1]+now[2]*now[2]>9.f*scale*scale) g_gateways_exit=-1;
                continue;
            }
            if (!q.enabled || !q.target || !gateways_contact(before,now,radius*.5f)) continue;
            float source[3],quat[4]; gateways_pose(bike,q,source,quat);
            if (!gateways_pause(bike,center,source)) continue;
            g_portal_pending=(int)i; g_portal_pending_tick=tick;
            if (q.flags[1]) q.enabled=false;
            char line[230];
            sprintf_s(line,"Gateways entry=%u exit=%u delay=%lu bodies=%u center=(%.2f,%.2f,%.2f)",
                q.index,q.target_index,q.delay,g_gateways_motion_count,center[0],center[1],center[2]);
            InterlockedExchange(&g_portal_log_lines,0); portal_log(line);
            portal_hit_bike(q.owner,bike);
            return;
        }
    }
    memcpy(g_gateways_previous,center,sizeof center); g_gateways_previous_valid=true;
}
