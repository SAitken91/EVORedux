// DLC impulse counters keep their count on a checkpoint restart (Magic Marker's pen, issue #9).
// The DLC (v47) FilterImpulseCounter record ends with one byte the PC format lacks: "reset in
// checkpoint", the counter version of FilterImpulseDelay's IMPULSE_DELAY_RESET_IN_CHECKPOINT. The
// PC delay reset (0x6339c0) skips clearing on a checkpoint restart when that flag is off; the PC
// counter reset (vtable slot 0x18, 0x6333c0) always clears the count. Magic Marker (206) moves its
// pen through 11 sync counters that need two impulses, "pen arrived" and "bike crossed the plane",
// all with the flag 0. A crash while the pen waited at one cleared the pen's impulse, the pen never
// sends it again, and the bike's crossing then only reached 1 of 2: the pen stopped for good
// (nt trace 2026-10-29: counter 610 count=1 at tick 1624, checkpoint restart 1650, count=1 at 2161).
// The record reader (trace_trigger_record) stores each counter's flag; base-game counters get 1.
// Rollback: create datapack\trials_revive_no_counter_keep.txt.

namespace {

typedef void(__fastcall* CounterResetFn)(void* self, void* edx, int checkpoint);
CounterResetFn g_counter_reset_original;

void __fastcall counter_reset(void* self, void* edx, int checkpoint) {
    if ((checkpoint & 0xff) && counter_reset_in_checkpoint(self) == 0) {
        // Keep +0x84 (first impulse tick) and +0x88 (count); run the common filter reset only.
        ((CounterResetFn)GA(0x6323c0))(self, edx, checkpoint);
        return;
    }
    g_counter_reset_original(self, edx, checkpoint);
}

void start_counter_keep_fix() {
    if (game_file_exists("trials_revive_no_counter_keep.txt")) {
        shim_log("dlc logic fix: counter checkpoint flag DISABLED by trials_revive_no_counter_keep.txt");
        return;
    }
    DWORD* slot = (DWORD*)(GA(0x1589a1c) + 0x18);          // FilterImpulseCounter vtable, reset
    DWORD old;
    if (!GA(0x1589a1c) || !GA(0x6323c0) || *slot != (DWORD)GA(0x6333c0) ||
        !VirtualProtect(slot, 4, PAGE_READWRITE, &old)) {
        shim_log("dlc logic fix: counter checkpoint flag skipped (unexpected vtable)");
        return;
    }
    g_counter_reset_original = (CounterResetFn)*slot;
    *slot = (DWORD)&counter_reset;
    VirtualProtect(slot, 4, old, &old);
    shim_log("dlc logic fix: DLC impulse counters honour their reset-in-checkpoint flag");
}

}  // namespace
