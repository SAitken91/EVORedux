// Retargetors are dynamic references, not physical objects. The transform path
// resolves them separately; scalar geometry queries and physics events need the
// same resolution before entering their native PC implementations.
namespace {
typedef float(__fastcall* RetargetQueryFn)(void*,void*);
RetargetQueryFn g_retarget_object_info,g_retarget_distance;
typedef unsigned char(__fastcall* RetargetPhysicsFn)(void*,void*);
RetargetPhysicsFn g_retarget_physics;

float __fastcall retarget_object_info(void* self,void* edx) {
    void** reference=(void**)((BYTE*)self+0x3c);
    void* original=*reference;
    *reference=resolve_dlc_retarget(original);
    float value=g_retarget_object_info(self,edx);
    *reference=original;
    return value;
}
float __fastcall retarget_distance(void* self,void* edx) {
    void** references=(void**)((BYTE*)self+0x3c);
    void* originals[]={references[0],references[1]};
    references[0]=resolve_dlc_retarget(originals[0]);
    references[1]=resolve_dlc_retarget(originals[1]);
    float value=g_retarget_distance(self,edx);
    references[0]=originals[0];references[1]=originals[1];
    return value;
}
unsigned char __fastcall retarget_physics(void* self,void* edx) {
    void** slots[1024];void* originals[1024];unsigned count=0;
    BYTE* node=*(BYTE**)((BYTE*)self+0x88);
    for(unsigned visited=0;node && visited<32768;++visited,node=*(BYTE**)(node+4)) {
        void** slot=(void**)(node+8);void* original=*slot;
        if(!original || !retarget_model(object_model(original)) || count==_countof(slots))continue;
        void* resolved=resolve_dlc_retarget(original);
        // An empty retargetor's icon has no physical bodies, so the native event
        // safely does nothing for it. Its handler cannot accept a null list entry.
        if(!resolved)continue;
        slots[count]=slot;originals[count++]=original;*slot=resolved;
    }
    unsigned char result=g_retarget_physics(self,edx);
    while(count) {--count;*slots[count]=originals[count];}
    return result;
}
void start_retarget_query_fix() {
    const BYTE info[]={0xb8,0x24,0x42,0x00,0x00};
    const BYTE distance[]={0x6a,0xff,0x68,0xf9,0x23,0xce,0x00};
    const BYTE physics[]={0x6a,0xff,0x68,0x5a,0x1c,0xce,0x00};
    g_retarget_object_info=(RetargetQueryFn)audio_garage_trampoline(GA(0x62b410),info,sizeof info);
    g_retarget_distance=(RetargetQueryFn)audio_garage_trampoline(GA(0x6272a0),distance,sizeof distance);
    g_retarget_physics=(RetargetPhysicsFn)audio_garage_trampoline(GA(0x61a380),physics,sizeof physics);
    const bool installed=g_retarget_object_info && g_retarget_distance && g_retarget_physics &&
        patch_jmp(GA(0x62b410),info,sizeof info,(void*)&retarget_object_info) &&
        patch_jmp(GA(0x6272a0),distance,sizeof distance,(void*)&retarget_distance) &&
        patch_jmp(GA(0x61a380),physics,sizeof physics,(void*)&retarget_physics);
    shim_log(installed?"dlc retargets: object info, distance and physics events enabled":"dlc retargets: query hooks skipped (unexpected code)");
}
}
