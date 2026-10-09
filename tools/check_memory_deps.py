#!/usr/bin/env python3
"""P2.3.3a (A4): nothing reaches the bridge's memory manager directly outside the documented exceptions.

docs/PHASE2_P233_MEMORY_DEPENDENCIES.md, sections 6-7. Scans the bridge (third_party/shadPS4/src) and
SecondSonPC (src) for direct uses of Core::MemoryManager (comments ignored) and checks each file with a
use against the list of exceptions below; the files moved to the interfaces (core/guest_memory.h,
core/gpu_memory.h) must not include core/memory.h at all; the bridge libraries excused because the
game does not import them must really have no import in docs/generated; the Linux-only use must be
inside #ifdef __linux__.

    python -I tools\\check_memory_deps.py        exit code 0 = clean, 1 = a forbidden dependency
"""
import csv
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BRIDGE = ROOT / "third_party" / "shadPS4" / "src"
OWN = ROOT / "src"

PATTERNS = {
    # "core/memory.h" or <core/memory.h>; a bare or relative "memory.h" is core/memory.h only inside src/core
    # (none in the tree today; kernel/memory.h is always included with its full path).
    "includes core/memory.h": re.compile(r'#\s*include\s*[<"](?:\.\./)*(?:core/)?memory\.h[>"]'),
    "Memory::Instance()": re.compile(r"\bMemory::Instance\s*\("),
    "MemoryManager": re.compile(r"\bMemoryManager\b"),
    "GetAddressSpace()": re.compile(r"\bGetAddressSpace\s*\("),
    "the bridge's AddressSpace": re.compile(r"\bAddressSpace\b"),
    "SecondSonMemoryAccess": re.compile(r"\bSecondSonMemoryAccess\b"),
}
INCLUDE_ONLY = "includes core/memory.h"

# Exceptions (docs/PHASE2_P233_MEMORY_DEPENDENCIES.md section 6). Paths relative to the repository.
B = "third_party/shadPS4/src/"
EXCEPTIONS = {
    # E1 (libkernel's memory API, core/libraries/kernel/memory.cpp) is gone since patch 0012 (P2.3.4): moved.
    B + "core/linker.cpp": ("E2", "--loader shadps4 (the oracle's loader)"),
    B + "core/linker.h": ("E2", "--loader shadps4 (the oracle's loader)"),
    B + "core/module.cpp": ("E2", "--loader shadps4 (the oracle's loader)"),
    B + "core/module.h": ("E2", "--loader shadps4 (the oracle's loader)"),
    B + "emulator.cpp": ("E2", "Core::Emulator, not used by SecondSonPC"),
    B + "emulator.h": ("E2", "Core::Emulator, not used by SecondSonPC"),
    B + "core/devtools/widget/memory_map.cpp": ("E3", "the bridge's debug window of its own maps"),
    B + "core/devtools/widget/memory_map.h": ("E3", "the bridge's debug window of its own maps"),
    "src/bridge/shadps4/memory_host.cpp": ("E3", "glue of the bridge's manager with the native direct memory"),
    B + "video_core/page_manager.cpp": ("E5", "Linux only (UffdImpl)"),
    "src/bridge/shadps4/selftest.cpp": ("E6", "self-test of the bridge's implementation"),
    "src/bridge/shadps4/selftest_vmm.cpp": ("E6", "self-test: GameLayout against the bridge's regions"),
    B + "core/memory.h": ("E7", "the implementation"),
    B + "core/memory.cpp": ("E7", "the implementation"),
    B + "core/address_space.h": ("E7", "the implementation"),
    B + "core/address_space.cpp": ("E7", "the implementation"),
    # Includes the header and uses nothing of it (checked: no other pattern may match).
    "src/bridge/shadps4/generated/bridge_exports.cpp": ("E8", "generated BRIDGE export table (include only)"),
    B + "imgui/renderer/imgui_impl_sdl3.cpp": ("E8", "upstream include without a use (include only)"),
}
INCLUDE_ONLY_FILES = {"src/bridge/shadps4/generated/bridge_exports.cpp", B + "imgui/renderer/imgui_impl_sdl3.cpp"}

# E4: files of bridge libraries the game does not import -> the PS4 library each implements. By file,
# not by folder: core/libraries/ime/ also holds libSceErrorDialog, which the game does import.
UNIMPORTED_FILES = {
    B + "core/libraries/avplayer/avplayer_source.cpp": "libSceAvPlayer",
    B + "core/libraries/camera/camera.cpp": "libSceCamera",
    B + "core/libraries/vr_tracker/vr_tracker.cpp": "libSceVrTracker",
    B + "core/libraries/np/np_signaling/np_signaling_helpers.cpp": "libSceNpSignaling",
    B + "core/libraries/ime/ime_dialog_ui.cpp": "libSceImeDialog",
    B + "core/libraries/font/font.cpp": "libSceFont",
}

# Users moved to the interfaces (U1-U15, P2.3.2's GPU files): must not include core/memory.h.
MOVED = [
    "src/bridge/shadps4/bootstrap.cpp",
    "src/bridge/shadps4/loader_host.cpp",
    "src/bridge/shadps4/fs_host.cpp",
    B + "core/libraries/kernel/threads/stack.cpp",
    B + "core/libraries/kernel/threads/thread_state.cpp",
    B + "core/libraries/kernel/threads/pthread.cpp",
    B + "core/libraries/kernel/file_system.cpp",
    B + "core/libraries/np/np_trophy.cpp",
    B + "video_core/renderer_vulkan/vk_rasterizer.cpp",
    B + "video_core/buffer_cache/buffer_cache.cpp",
    B + "video_core/texture_cache/texture_cache.cpp",
    B + "video_core/amdgpu/liverpool.cpp",
    B + "core/libraries/gnmdriver/gnmdriver.cpp",
    B + "core/libraries/kernel/memory.cpp",  # P2.3.4, patch 0012
]


# Macros the moved files used to get through core/memory.h (core/address_space.h -> common/arch.h): a moved
# file testing one in #if must include its header itself, else the test silently goes the other way (P2.3.3a:
# pthread.cpp compiled the UNREACHABLE _runOnAnotherStack without ARCH_X86_64).
MACRO_HEADERS = {"ARCH_X86_64": "common/arch.h", "ARCH_ARM64": "common/arch.h"}


def strip_comments(text: str) -> str:
    """Comments become blanks (line numbers kept); string literals are left alone."""
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def linux_only_lines(text: str) -> set[int]:
    """Line numbers inside #ifdef __linux__ ... #endif (nested conditionals tracked)."""
    inside: set[int] = set()
    stack: list[bool] = []
    for number, line in enumerate(text.splitlines(), 1):
        directive = line.strip()
        if directive.startswith("#if"):
            stack.append(directive in ("#ifdef __linux__", "#if defined(__linux__)"))
        elif directive.startswith("#endif") and stack:
            stack.pop()
        elif directive.startswith(("#else", "#elif")) and stack and stack[-1]:
            stack[-1] = False
        if any(stack):
            inside.add(number)
    return inside


def imported_libraries() -> set[str]:
    libraries: set[str] = set()
    for name in ("imports.csv", "prx_imports.csv"):
        with open(ROOT / "docs" / "generated" / name, newline="", encoding="utf-8") as f:
            for row in csv.DictReader(f):
                libraries.add(row["library"])
                libraries.add(row["module"])
    return libraries


def main() -> int:
    problems: list[str] = []
    excused: list[str] = []
    imported = imported_libraries()
    files = [p for base in (BRIDGE, OWN) for p in base.rglob("*") if p.suffix in (".cpp", ".h", ".inc")]
    for path in sorted(files):
        rel = path.relative_to(ROOT).as_posix()
        raw = path.read_text(encoding="utf-8", errors="replace")
        code = strip_comments(raw)
        hits = []
        for number, line in enumerate(code.splitlines(), 1):
            for what, pattern in PATTERNS.items():
                if pattern.search(line):
                    hits.append((number, what))
        if not hits:
            continue
        where = ", ".join(f"{n}: {w}" for n, w in hits[:6]) + (" ..." if len(hits) > 6 else "")
        if rel in EXCEPTIONS:
            tag, reason = EXCEPTIONS[rel]
            if rel in INCLUDE_ONLY_FILES and any(w != INCLUDE_ONLY for _, w in hits):
                problems.append(f"{rel}: excused as include-only ({tag}) but uses the manager: {where}")
                continue
            if tag == "E5":
                linux = linux_only_lines(raw)
                outside = [(n, w) for n, w in hits if n not in linux and w != INCLUDE_ONLY]
                if outside:
                    problems.append(f"{rel}: outside #ifdef __linux__ ({tag}): {outside}")
                    continue
            excused.append(f"  {tag} {rel} ({len(hits)}): {reason}")
            continue
        if rel in UNIMPORTED_FILES:
            library = UNIMPORTED_FILES[rel]
            if library in imported:
                problems.append(f"{rel}: excused because the game does not import {library}, but it does")
            else:
                excused.append(f"  E4 {rel} ({len(hits)}): the game imports nothing from {library}")
            continue
        problems.append(f"{rel}: direct use of the memory manager: {where}")
    for rel in MOVED:
        text = strip_comments((ROOT / rel).read_text(encoding="utf-8", errors="replace"))
        if PATTERNS[INCLUDE_ONLY].search(text):
            problems.append(f"{rel}: moved to the interfaces but still includes core/memory.h")
        conditionals = [line for line in text.splitlines() if re.match(r"\s*#\s*(if|elif)", line)]
        for macro, header in MACRO_HEADERS.items():
            used = any(re.search(rf"\b{macro}\b", line) for line in conditionals)
            if used and not re.search(rf'#\s*include\s*"{re.escape(header)}"', text):
                problems.append(f"{rel}: tests {macro} in #if but does not include {header}")
    print(f"memory manager dependencies: {len(files)} files scanned, {len(excused)} excused, "
          f"{len(problems)} forbidden")
    for line in excused:
        print(line)
    for line in problems:
        print("FORBIDDEN", line)
    print("PASS" if not problems else "FAIL")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
