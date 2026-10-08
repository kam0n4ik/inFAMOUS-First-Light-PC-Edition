// SecondSonPC — crash report in SecondSonPC.log (unhandled exceptions).
#pragma once

namespace SecondSon::Base {

/// Logs the code, address, registers and the return addresses found on the stack of an unhandled
/// exception (as exe+offset, for llvm-symbolizer with SecondSonPC.pdb), then flushes the log.
void InstallCrashLogger();

} // namespace SecondSon::Base
