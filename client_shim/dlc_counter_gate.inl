// Disabled impulse counters block (Neverending Trials section cycle). EXPERIMENTAL.
// FilterImpulseCounter execute 0x633530 returns "pass" when the filter is disabled ([this+0x70]==0),
// so an EventState that switches a DLC impulse_clamp OFF turns it into a plain wire. In Neverending
// (424) state event 3639 disables park clamp 3640 at race start; the 12-tick delay chain
// 3622 -> 3630 -> 3640 then parks every section (transform 1078) ~95 ticks after it was placed
// (nt trace 2026-10-25: "3640@1589a1c:0" in the park chain). With this gate a disabled counter
// drops the impulse. Enabled by datapack\trials_revive_counter_gate.txt.

namespace {

const uintptr_t COUNTER_ENABLED_CHECK = 0x633550;  // cmp byte [esi+70h],0 ; jne 63355a
const unsigned char COUNTER_ENABLED_ORIG[6] = {0x80, 0x7e, 0x70, 0x00, 0x75, 0x04};

__declspec(naked) void counter_gate_stub() {
    __asm {
        cmp byte ptr [esi + 0x70], 0
        je blocked
        push 0x63355a        // enabled: original counting path
        ret
    blocked:
        xor al, al           // disabled: do not pass the impulse
        pop esi
        ret
    }
}

void start_counter_gate() {
    if (!game_file_exists("trials_revive_counter_gate.txt")) return;
    shim_log(patch_jmp(COUNTER_ENABLED_CHECK, COUNTER_ENABLED_ORIG, sizeof COUNTER_ENABLED_ORIG, (void*)&counter_gate_stub)
                 ? "counter gate: disabled impulse counters block impulses (experimental)"
                 : "counter gate: skipped (unexpected code)");
}

}  // namespace
