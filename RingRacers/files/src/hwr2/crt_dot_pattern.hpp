// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Ronald "Eidolon" Kinard
// Copyright (C) 2025 by Kart Krew
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------

#ifndef __SRB2_HWR2_CRT_DOT_PATTERN_HPP__
#define __SRB2_HWR2_CRT_DOT_PATTERN_HPP__

#include <cstdint>

namespace srb2::hwr2
{

// The SalCRT shadow mask: a 12x4 RGBA tile the CRT shaders repeat across the
// frame as their second sampler. Kept here rather than as a .png in the pk3s,
// and kept in one place because two renderers want it - the RHI builds it as
// an rhi::Texture for the monitor blit, and the VR module uploads it straight
// to GL for the per-eye pass.
constexpr int kCrtDotPatternWidth = 12;
constexpr int kCrtDotPatternHeight = 4;

constexpr uint8_t kCrtDotPattern[] = {
	255, 0, 0, 255,
	0, 0, 0, 255,
	0, 255, 0, 255,
	0, 0, 0, 255,
	0, 0, 255, 255,
	0, 0, 0, 255,
	255, 0, 0, 255,
	0, 0, 0, 255,
	0, 255, 0, 255,
	0, 0, 0, 255,
	0, 0, 255, 255,
	0, 0, 0, 255,

	255, 0, 0, 255,
	0, 0, 0, 255,
	0, 255, 0, 255,
	0, 0, 0, 255,
	0, 0, 255, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,

	255, 0, 0, 255,
	0, 0, 0, 255,
	0, 255, 0, 255,
	0, 0, 0, 255,
	0, 0, 255, 255,
	0, 0, 0, 255,
	255, 0, 0, 255,
	0, 0, 0, 255,
	0, 255, 0, 255,
	0, 0, 0, 255,
	0, 0, 255, 255,
	0, 0, 0, 255,

	0, 0, 0, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,
	0, 0, 0, 255,
	255, 0, 0, 255,
	0, 0, 0, 255,
	0, 255, 0, 255,
	0, 0, 0, 255,
	0, 0, 255, 255,
	0, 0, 0, 255,
};

} // namespace srb2::hwr2

#endif // __SRB2_HWR2_CRT_DOT_PATTERN_HPP__
