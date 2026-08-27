// license:BSD-3-Clause
// copyright-holders:Stephen Spiller
/*
    mc6847_charset_fixture.h

    Character generator ROM stand-ins for the MC6847 charset helpers.

    MAME cannot redistribute the MC6847 or MC6847T1 font ROMs, and the helpers
    under test are deliberately ROM agnostic, so the fixtures below supply
    synthetic ROM images whose every bit is distinguishable.  The reference
    rows are written from the documented ROM layout rather than by calling the
    production helpers, which keeps the expectations independent of the code
    they check.
*/

#ifndef MAME_TESTS_EMU_VIDEO_MC6847_CHARSET_FIXTURE_H
#define MAME_TESTS_EMU_VIDEO_MC6847_CHARSET_FIXTURE_H

#pragma once

#include "video/mc6847_charset.h"

#include <array>
#include <cstdint>

namespace mc6847_charset_test
{

using internal_rom_image = std::array<std::uint8_t, mc6847_charset::INTERNAL_ROM_BYTES>;
using t1_rom_image = std::array<std::uint8_t, mc6847_charset::T1_ROM_BYTES>;

// Deterministic stand-ins for the MC6847 and MC6847T1 character generator
// ROMs.  The tests deliberately carry no real font data: the fixtures only
// have to make every ROM bit distinguishable so that the expansion can be
// checked bit for bit against an independently written reference.
inline internal_rom_image const &internal_rom()
{
	static internal_rom_image const image = []
			{
				internal_rom_image result{};
				std::uint32_t state = 0x6847'0001U;
				for (std::uint8_t &value : result)
				{
					state = state * 1'103'515'245U + 12'345U;
					value = std::uint8_t(state >> 16);
				}
				return result;
			}();
	return image;
}

inline t1_rom_image const &t1_rom()
{
	static t1_rom_image const image = []
			{
				t1_rom_image result{};
				std::uint32_t state = 0x6847'7431U;
				for (std::uint8_t &value : result)
				{
					state = state * 1'103'515'245U + 12'345U;
					// the VDG cell is only six pixels wide
					value = std::uint8_t((state >> 16) & 0x3f);
				}
				return result;
			}();
	return image;
}

// Reference expansions, written from the ROM layout rather than by calling the
// production helpers.

inline std::uint8_t internal_reference_row(
		internal_rom_image const &rom,
		unsigned character,
		unsigned row)
{
	if ((row < 3) || (row > 9))
		return 0;

	unsigned const code = character % 64;
	unsigned const bit_shift = 7 - (code % 8);
	unsigned const glyph_row = row - 3;
	std::uint8_t result = 0;
	for (unsigned column = 0; column < 5; column++)
	{
		// the packed array stores one bit column per glyph row, bottom row first
		unsigned const index = 224 + (code / 8) + glyph_row * 8 - column * 56;
		if ((rom[index] >> bit_shift) & 1)
			result |= 0x20 >> column;
	}
	return result;
}

inline std::uint8_t t1_reference_row(
		t1_rom_image const &rom,
		unsigned character,
		unsigned row)
{
	return rom[character * mc6847_charset::GLYPH_HEIGHT + row];
}

inline std::uint8_t semigraphics_reference_row(
		unsigned character,
		unsigned row,
		unsigned row_height)
{
	unsigned const slice = (mc6847_charset::GLYPH_HEIGHT - 1 - row) / row_height;
	std::uint8_t result = 0;
	if ((character >> (slice * 2 + 1)) & 1)
		result |= 0xf0;
	if ((character >> (slice * 2)) & 1)
		result |= 0x0f;
	return result;
}

} // namespace mc6847_charset_test

#endif // MAME_TESTS_EMU_VIDEO_MC6847_CHARSET_FIXTURE_H
