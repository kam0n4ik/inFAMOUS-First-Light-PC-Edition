// First Light (research/prompt_textures_scan.py: Second Son's pictures looked for in its packs by their bytes, each
// hit checked by the top level's hash): only the two disc pictures of core_common are there unchanged; its d-pad
// wheels are its own and it has no graffiti tutorial. The touch pad print is First Light's own picture (found by its
// T# in core_common, 256x256 BC3 with 9 levels, in no other pack; its hash is First Light's in KindInfo).
// {pack file name, byte offset of the texture data in the file, picture}
{"core_common.sprig.xpps", 0x10e83e78, PictureKind::kDiscTriangle128},
{"core_common.sprig.xpps", 0x10e76e78, PictureKind::kDiscSquare128},
{"core_common.sprig.xpps", 0xf1c2b78, PictureKind::kTouchPrint256},
