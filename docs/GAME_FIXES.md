# Races in the game's own code the PC exposes (src/engine/game_fixes.*)

Each fix is a byte patch of the eboot written before any guest code runs, only when the original bytes match exactly
(another game version: nothing is written, the log says why). Off with its switch.

## The engine's reference counting (eboot 0x1be900 strong / 0x1be820 weak) - `SSPC_FIX_REFCOUNT_RACE=0` turns it off

Crash (2026-10-18 11:38, 16:15, 17:35, hand play, the powers menu switching its `powerk_*.bsf` movies): thread
"MovieDecoder" executes 0x0. Call `eboot+0x1da53a -> 0x1be900` (the strong release of the movie entry's object at
+0x78, from 0x1da420 called by 0x28d0e0 case 2); the object's "vtable" `rax` was a heap pointer next to it - the link of
the pool's free list (0x1d53b0 takes objects from `DAT_00efaad0`, whose first qword is that link). The object was
already back in its pool.

An object has a strong count at +0x10 and a weak count at +0x14 and is destroyed (vtable slot 2) when both are 0:
- `0x1be900` (strong): `lock dec [rdi+0x10]; cmp [rdi+0x10],0; jnz ret; cmp [rdi+0x14],0; jz destroy; ret`
- `0x1be820` (weak, under the manager's mutex - which the strong release does not take): `lock dec [r14+0x14];
  cmp [r14+0x14],0; jnz done; cmp [r14+0x10],0; jz destroy; call [vtable+0x18]`

Each decrements its own count and then reads the other one. The last strong and the last weak release on two threads
both read (0, 0) and both destroy: the object enters the pool twice, is handed to two owners later, and one of them
uses it after the other released it. (The first fix, 01deec9, only removed the re-read of the same count after the
decrement: the crash came back at 17:35.)

Fixed: `0x1be900` jumps to `StrongRelease`, and the 19 bytes at `0x1be8a2` call `WeakRelease` (rdi = r14) and
jump to where it says (done / destroy / the +0x18 notification). Both change the two counts in ONE atomic 64-bit
operation on +0x10; only the release whose operation leaves (0, 0) destroys. Before a vtable is called it must lie in
the game's image; otherwise an Error `reference counting: ... already back in its pool - not called` is logged.

Checked: installs (`game fix installed: the engine's reference counting`), a 150 s game run with every Phase 2
native subsystem clean, FPS unchanged. The powers menu's movie switching: to be checked in hand play.
