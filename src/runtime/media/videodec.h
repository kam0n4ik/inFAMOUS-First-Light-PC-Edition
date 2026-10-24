// SecondSonPC — what the presenter needs to know of the game's movies (videodec.cpp).
#pragma once

namespace SecondSon::Media {

/// A full-screen movie is being shown: a decoder that gives 16:9 pictures at least 1280 wide is alive and gave one in
/// the last 1.5 s. The game draws such a movie over its whole picture, stretched or cut on one that is not 16:9
/// (docs/RESOLUTION_RESEARCH.md); the GPU backend then keeps that draw 16:9 (docs/RESOLUTION_IMPLEMENTATION.md).
bool FullScreenMovieShowing();

} // namespace SecondSon::Media
