// Runtime for the shipped literal vectors, scalar getters, position vectors and
// set-vector events. Kept separate from the binary compatibility hooks.
struct VectorState {
    VectorState* next;
    void* self;
    void* reference;
    void* adapters[2];
    void* targets[1024];
    unsigned target_count, count, index, operation;
    int kind;
    bool reset, triple, evaluating, publishing;
    float initial[100], values[100];
    float traced[3];                 // last values written to the nt trace
};
VectorState* g_vectors;
void nt_trace_set_vector(VectorState* s, const float* values, unsigned width);
void nt_trace_vector(VectorState* s, const char* what);
DWORD g_vector_adapter_vtable[16];
struct VectorAdapter { BYTE native[0x48]; VectorState* state; unsigned component; };

VectorState* vector_state(void* self) {
    for (VectorState* s = g_vectors; s; s = s->next) if (s->self == self) return s;
    return nullptr;
}
VectorState* vector_owner(void* owner) {
    for (VectorState* s = g_vectors; s; s = s->next)
        if (((void**)s->self)[1] == owner && s->kind != 5) return s;
    return nullptr;
}
void vector_compute(VectorState* s, void* visited, int depth);
float vector_input(VectorState* s, unsigned property, float fallback, void* visited, int depth) {
    void* input = selector_input(((void**)s->self)[1], DLC_PROPERTY_BASE + property);
    if (!input) return fallback;
    return visited ? ((SourceEvalFn)(*(DWORD**)input)[6])(input, nullptr, visited, depth + 1)
                   : *(float*)((BYTE*)input + 0x28);
}
float __fastcall vector_adapter_eval(void* self, void*, void* visited, int depth) {
    VectorAdapter* a = (VectorAdapter*)self;
    vector_compute(a->state, visited, depth);
    float value = a->state->values[a->component];
    *(float*)((BYTE*)self + 0x28) = value;
    return value;
}
float __fastcall vector_adapter_value(void* self, void*) {
    VectorAdapter* a = (VectorAdapter*)self;
    return a->state->values[a->component];
}
void vector_compute(VectorState* s, void* visited, int depth) {
    if (!s || s->evaluating || depth > 500) return;
    s->evaluating = true;
    if (s->kind == 0) {
        for (unsigned k = 0; k < 3 && k < s->count; ++k)
            s->values[k] = vector_input(s, 467 + k, s->values[k], visited, depth);
    } else if (s->kind == 1) {
        VectorState* input = vector_owner(resolve_dlc_retarget(s->reference));
        vector_compute(input, visited, depth + 1);
        float index = vector_input(s, 498, (float)s->index, visited, depth);
        s->values[0] = input && index >= 0 && index < (float)input->count ? input->values[(unsigned)index] : 0.0f;
    } else if (s->kind == 2) {
        VectorState* start = vector_owner(resolve_dlc_retarget(s->reference));
        VectorState* end = vector_owner(resolve_dlc_retarget(s->targets[0]));
        vector_compute(start, visited, depth + 1);
        vector_compute(end, visited, depth + 1);
        // 520 is duration in ticks: all five authored wires agree with the
        // serialized durations (12/6/116), not the enable flag.
        float duration = vector_input(s, 520, (float)*(int*)((BYTE*)s->self + 0x50), visited, depth);
        if (duration >= 1 && duration < 2147483648.0f)
            *(int*)((BYTE*)s->self + 0x50) = (int)duration;
        // The native curve owns timing, easing, enable/disable and checkpoint
        // state. Use its normalized 0..1 output for every vector component.
        float phase = ((float(__fastcall*)(void*, void*))GA(0x624960))(s->self, nullptr);
        s->count = start && end ? (start->count < end->count ? start->count : end->count) : 3;
        for (unsigned k = 0; k < s->count; ++k) {
            float a = start ? start->values[k] : 0.0f;
            float b = end ? end->values[k] : 0.0f;
            s->values[k] = a + (b - a) * phase;
        }
    } else if (s->kind == 3) {
        // Native ObjectInfo's first three modes return world position, including
        // physics/group/player handling. It only reads target+3c and mode+40.
        BYTE native[0x44] = {};
        *(void**)(native + 0x3c) = resolve_dlc_retarget(s->reference);
        for (unsigned k = 0; k < 3; ++k) {
            *(unsigned*)(native + 0x40) = k;
            s->values[k] = ((float(__fastcall*)(void*, void*))GA(0x62b410))(native, nullptr);
        }
    }
    if (memcmp(s->traced, s->values, sizeof s->traced)) {
        memcpy(s->traced, s->values, sizeof s->traced);
        nt_trace_vector(s, "vector value");
    }
    *(float*)((BYTE*)s->self + 0x28) = s->values[0];
    for (unsigned k = 0; k < 2; ++k) if (s->adapters[k])
        *(float*)((BYTE*)s->adapters[k] + 0x28) = s->values[k + 1];
    s->evaluating = false;
}
void vector_publish(VectorState* s) {
    if (s->publishing) return;
    s->publishing = true;
    for (unsigned k = 0; k < 2; ++k) if (s->adapters[k])
        ((void(__fastcall*)(void*, void*, int))GA(0x621ab0))(s->adapters[k], nullptr, 0);
    s->publishing = false;
}
float __fastcall vector_eval(void* self, void*, void* visited, int depth) {
    VectorState* s = vector_state(self);
    if (!s) return 0.0f;
    vector_compute(s, visited, depth);
    // Native callers can enter 621ab0 directly instead of the update vslot.
    // Publishing here keeps Y/Z wires live through both paths.
    if (depth == 0) vector_publish(s);
    return s->values[0];
}
float __fastcall vector_value(void* self, void*) {
    VectorState* s = vector_state(self);
    return s ? s->values[0] : 0.0f;
}
void __fastcall vector_reset(void* self, void*, bool checkpoint) {
    ((void(__fastcall*)(void*, void*, bool))GA(0x6208c0))(self, nullptr, checkpoint);
    VectorState* s = vector_state(self);
    if (s && s->kind == 0 && (s->reset || !checkpoint))
        memcpy(s->values, s->initial, sizeof s->values);
    ((void(__fastcall*)(void*, void*, int))GA(0x621ab0))(self, nullptr, 0);
}
void* __fastcall vector_destroy(void* self, void*, unsigned flags) {
    DWORD destructor = ((DWORD*)GA(0x150a61c))[0];
    VectorState** at = &g_vectors;
    while (*at && (*at)->self != self) at = &(*at)->next;
    if (*at) {
        VectorState* s = *at;
        if (s->kind == 5) destructor = ((DWORD*)GA(0x146618c))[0];
        if (s->kind == 2) destructor = ((DWORD*)GA(0x14af43c))[0];
        *at = s->next;
        for (unsigned k = 0; k < 2; ++k) if (s->adapters[k])
            ((void*(__fastcall*)(void*, void*, unsigned))GA(0x62ec70))(s->adapters[k], nullptr, 1);
        delete s;
    }
    return ((void*(__fastcall*)(void*, void*, unsigned))destructor)(self, nullptr, flags);
}
void* vector_component(VectorState* s, unsigned component) {
    if (!component) return s->self;
    if (!s->adapters[component - 1]) {
        VectorAdapter* a = (VectorAdapter*)((void*(__cdecl*)(size_t))GA(0x436a40))(sizeof(VectorAdapter));
        if (!a) return nullptr;
        memset(a, 0, sizeof *a);
        ((TriggerCtorFn)GA(0x62e050))(a, nullptr, ((void**)s->self)[1]);
        *(DWORD**)a = g_vector_adapter_vtable;
        a->state = s; a->component = component;
        s->adapters[component - 1] = a;
    }
    return s->adapters[component - 1];
}
DWORD vector_u32(void* stream) {
    DWORD v = 0;
    ((void(__fastcall*)(void*, void*, DWORD*))(*(DWORD**)stream)[0x10 / 4])(stream, nullptr, &v);
    return v;
}
float vector_float(void* stream) {
    float v = 0;
    ((ReadFloatFn)(*(DWORD**)stream)[1])(stream, nullptr, &v);
    return v;
}
void* vector_ref(void* stream, void* objects) {
    return ((void*(__cdecl*)(void*, void*, int))GA(0x4d06b0))(stream, objects, 0);
}
int vector_read(VectorState* s, void* stream, void* objects, void* selected, int version) {
    DlcReader r{stream};
    if (s->kind == 5) {
        int result = ((int(__fastcall*)(void*, void*, void*, void*, int))GA(0x5fcc40))
            ((BYTE*)s->self + 0x20, nullptr, stream, objects, version);
        if (result) return result;
        s->operation = vector_u32(stream);
        s->index = r.u8(); s->triple = r.u8() != 0;
        for (unsigned k = 0; k < 3; ++k) s->values[k] = vector_float(stream);
        s->target_count = r.u16();
        if (s->target_count > 1024) return 0x401;
        for (unsigned k = 0; k < s->target_count; ++k) s->targets[k] = vector_ref(stream, objects);
        return s->operation == 0 ? 0 : 0x401;
    }
    if (s->kind == 1) {
        int result = ((SourceReadFn)GA(0x6236c0))(s->self, nullptr, stream, objects, selected, version);
        if (result) return result;
    } else {
        unsigned n = r.u16();
        if (n > 1024) return 0x401;
        while (n--) {
            void* target = vector_ref(stream, objects);
            unsigned m = r.u16();
            if (m > 1024) return 0x401;
            while (m--) {
                unsigned property = r.u16(), component = 0;
                if (property >= 390 && property <= 392) component = property - 390;
                else if (property >= 502 && property <= 504) component = property - 502;
                if (!target) continue;
                void* source = vector_component(s, component);
                if (!source) return 0x401;
                ((void(__fastcall*)(void*, void*, void*, DWORD, int))GA(0x622e40))
                    (source, nullptr, target, selector_property(target, property), 0);
            }
        }
    }
    if (s->kind == 0) {
        s->reset = r.u8() != 0;
        DWORD type = vector_u32(stream);
        s->count = r.u16();
        if (!s->count || s->count > 100 || type != 0) return 0x401;
        for (unsigned k = 0; k < s->count; ++k) s->initial[k] = s->values[k] = vector_float(stream);
        BYTE interpolate = r.u8(); vector_float(stream); r.u8(); vector_float(stream); vector_float(stream);
        if (interpolate) return 0x401;
    } else if (s->kind == 1) {
        s->count = 1; s->index = vector_u32(stream); s->reference = vector_ref(stream, objects);
    } else if (s->kind == 2) {
        BYTE* native = (BYTE*)s->self;
        *(DWORD*)(native + 0x3c) = r.u8();
        native[0x54] = r.u8();
        s->reference = vector_ref(stream, objects);
        s->targets[0] = vector_ref(stream, objects);
        *(DWORD*)(native + 0x50) = vector_u32(stream);
        *(DWORD*)(native + 0x58) = vector_u32(stream);
        native[0x55] = r.u8(); native[0x5c] = r.u8();
        native[0x5d] = r.u8(); native[0x5e] = r.u8();
        *(float*)(native + 0x40) = 0.0f;
        *(float*)(native + 0x44) = 1.0f;
        s->count = 3;
    } else if (s->kind == 3) {
        s->count = 3;
        if (r.u8() != 0) return 0x401;
        s->reference = vector_ref(stream, objects);
    }
    return 0;
}
unsigned char __fastcall vector_set(void* self, void*) {
    unsigned char result = ((unsigned char(__fastcall*)(void*, void*))GA(0x61d720))(self, nullptr);
    VectorState* s = vector_state(self);
    if (s) nt_trace_vector(s, *((BYTE*)self + 0x70) ? "set_vector fired" : "set_vector fired while disabled");
    if (!s || !*((BYTE*)self + 0x70)) return result;
    float values[3];
    for (unsigned k = 0; k < 3; ++k) {
        void* input = selector_input(((void**)self)[1], DLC_PROPERTY_BASE + 502 + k);
        // Evaluate using the engine's real visited table (621ab0), never nullptr.
        if (input) ((void(__fastcall*)(void*, void*, int))GA(0x621ab0))(input, nullptr, 0);
        values[k] = input ? *(float*)((BYTE*)input + 0x28) : s->values[k];
    }
    float index = (float)s->index;
    void* index_input = selector_input(((void**)self)[1], DLC_PROPERTY_BASE + 501);
    if (index_input) {
        ((void(__fastcall*)(void*, void*, int))GA(0x621ab0))(index_input, nullptr, 0);
        index = *(float*)((BYTE*)index_input + 0x28);
    }
    if (!(index >= 0 && index < 100)) return result;
    unsigned at = (unsigned)index;
    if (!s->triple) {
        void* scalar = selector_input(((void**)self)[1], DLC_PROPERTY_BASE + 516);
        if (scalar) {
            ((void(__fastcall*)(void*, void*, int))GA(0x621ab0))(scalar, nullptr, 0);
            values[0] = *(float*)((BYTE*)scalar + 0x28);
        }
    }
    nt_trace_set_vector(s, values, s->triple ? 3 : 1);
    for (unsigned k = 0; k < s->target_count; ++k) {
        VectorState* target = vector_owner(resolve_dlc_retarget(s->targets[k]));
        if (!target || target->kind != 0) continue;
        unsigned width = s->triple ? 3 : 1;
        if (at >= target->count || width > target->count - at) continue;
        for (unsigned j = 0; j < width; ++j) target->values[at + j] = values[j];
        ((void(__fastcall*)(void*, void*, int))GA(0x621ab0))(target->self, nullptr, 0);
    }
    return result;
}
