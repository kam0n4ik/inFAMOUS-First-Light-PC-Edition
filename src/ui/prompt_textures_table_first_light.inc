// First Light (research/prompt_textures_scan.py: Second Son's pictures looked for in its packs by their bytes, each
// hit checked by the top level's hash): only the two disc pictures of core_common are there unchanged; its d-pad
// wheels are its own and it has no graffiti tutorial. The touch pad print is First Light's own picture (found by its
// T# in core_common, 256x256 BC3 with 9 levels, in no other pack; its hash is First Light's in KindInfo). So are its
// d-pad wheel (the race start's prompt, 256x256, 9 levels): found by its T# in core_common, in no other pack, its hash
// First Light's in KindInfo. Its small d-pad (core_common 0x1152c378, 64x64) is not a wheel: a transparent cross whose
// left and right arms are the highlighted ones; DrawDpad would draw four equal keys, so it stays the game's.
// {pack file name, byte offset of the texture data in the file, picture}
{"core_common.sprig.xpps", 0x10e83e78, PictureKind::kDiscTriangle128},
{"core_common.sprig.xpps", 0x10e76e78, PictureKind::kDiscSquare128},
{"core_common.sprig.xpps", 0xf1c2b78, PictureKind::kTouchPrint256},
{"core_common.sprig.xpps", 0xf32ab78, PictureKind::kDpad256},
