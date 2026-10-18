// SecondSonPC — `--gcn-verify-decoder` (gcn_decoder_check.cpp): SecondSonPC's GCN decoder against the oracle's.
#pragma once

namespace SecondSon::App {
struct Config;
}

namespace SecondSon::Bridge {

/// Decodes every program of the offline index with both decoders and writes <logs>\gcn_decoder_check.txt.
/// 0: no difference; 2: differences (the report lists them); 1: no index.
int RunGcnDecoderCheck(const App::Config& cfg);

} // namespace SecondSon::Bridge
