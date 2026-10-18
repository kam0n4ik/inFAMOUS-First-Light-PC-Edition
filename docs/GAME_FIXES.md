# Races in the game's own code the PC exposes (src/engine/game_fixes.*)

Each fix is a byte patch of the eboot written before any guest code runs, only when the original bytes match exactly
(another game version: nothing is written, the log says why). Off with its switch.

## The engine's reference release (eboot 0x1be900) - `SSPC_FIX_REFCOUNT_RACE=0` turns it off

Crash (2026-10-18 11:38 and 16:15, hand play, the powers menu switching its `powerk_*.bsf` movies): thread
"MovieDecoder" executes 0x0. Call `eboot+0x1da53a -> 0x1be900`; `rax` (the object's "vtable") was a heap pointer next
to the object - the allocator's link in freed memory.

`0x1be900`: `lock dec [rdi+0x10]; cmp [rdi+0x10],0; jnz ret; cmp [rdi+0x14],0; jz destroy; ret;
destroy: mov rax,[rdi]; jmp [rax+0x10]`. The count is read again after the atomic decrement: two threads dropping the
last two references can both see 0 and destroy twice, the second time through the freed memory. Fixed: `jnz ret`
right after `lock dec` (its own zero flag; 6 bytes: `75 0a 90 90 90 90` at +4), the rest unchanged.

Checked: the fix installs (log `game fix installed`), a 45 s run clean. The menu's movie switching itself: to be
checked in hand play.
