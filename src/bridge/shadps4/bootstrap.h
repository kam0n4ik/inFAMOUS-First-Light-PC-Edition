// SecondSonPC — TEMPORARY Phase 0 bootstrap over the shadPS4 core.
// The only header of SecondSonPC that is implemented with shadPS4 code. Removed in Phase 5.
#pragma once

namespace SecondSon::App {
struct Config;
}

namespace SecondSon::Bridge {

/// Loads eboot.bin with the shadPS4 core, binds SecondSonPC exports first, runs the game.
/// Does not return (the session ends through base/lifecycle.h: Lifecycle::Exit, P3.6).
[[noreturn]] void RunGame(const App::Config& config);

/// DIAGNOSTIC ONLY (--stock-bootstrap): shadPS4's own Core::Emulator::Run inside this binary, to
/// separate "toolchain/build" problems from "SecondSonPC bootstrap" problems. No SecondSonPC exports.
[[noreturn]] void RunStockForDiagnostics(const App::Config& config);

} // namespace SecondSon::Bridge
