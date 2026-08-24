// license:BSD-3-Clause
// copyright-holders:Nathan Woods,Stephen Spiller
/*********************************************************************

    mc6847_charset.h

    Shared, ROM-agnostic Motorola 6847 character generator helpers.

    These helpers deliberately contain no character data of their own.
    Callers supply the raw contents of a character generator ROM region and
    receive expanded 8x12 glyph rows, which keeps a single copy of the
    expansion and mode decoding rules for the MC6847 family and for
    peripherals (such as CoCoVGA) that reimplement the VDG character
    generator against their own copies of the same ROMs.

*********************************************************************/

#ifndef MAME_VIDEO_MC6847_CHARSET_H
#define MAME_VIDEO_MC6847_CHARSET_H

#pragma once

#include "coretmpl.h"

#include <cstddef>
#include <cstdint>

namespace mc6847_charset
{

//**************************************************************************
//  GEOMETRY
//**************************************************************************

// every glyph the VDG generates is eight pixels wide and twelve rows tall
constexpr unsigned GLYPH_HEIGHT = 12;

// the internal alphanumeric font repeats its first 32 glyphs to fill 96 codes
constexpr unsigned GLYPH_COUNT = 96;

// expanded font produced from a character generator ROM
using text_font = uint8_t[GLYPH_COUNT][GLYPH_HEIGHT];

// raw character generator ROM sizes
constexpr size_t INTERNAL_ROM_BYTES = 0x0118;   // packed 5x7 cell array
constexpr size_t T1_ROM_BYTES = GLYPH_COUNT * GLYPH_HEIGHT;

// generated semigraphics fonts
constexpr unsigned SEMIGRAPHICS4_GLYPH_COUNT = 16;
constexpr unsigned SEMIGRAPHICS6_GLYPH_COUNT = 64;
constexpr unsigned SEMIGRAPHICS4_ROW_HEIGHT = 6;
constexpr unsigned SEMIGRAPHICS6_ROW_HEIGHT = 4;


//**************************************************************************
//  MODE DECODING
//**************************************************************************

constexpr uint8_t MODE_AG      = 0x80;
constexpr uint8_t MODE_GM2     = 0x40;
constexpr uint8_t MODE_GM1     = 0x20;
constexpr uint8_t MODE_GM0     = 0x10;
constexpr uint8_t MODE_CSS     = 0x08;
constexpr uint8_t MODE_AS      = 0x04;
constexpr uint8_t MODE_INTEXT  = 0x02;
constexpr uint8_t MODE_INV     = 0x01;

enum class glyph : uint8_t
{
	NONE,               // graphics modes do not run the character generator
	TEXT,               // alphanumeric glyph from the character generator ROM
	SEMIGRAPHICS4,
	SEMIGRAPHICS6,
	STRIPES             // INT/EXT with no external ROM on a plain MC6847
};

struct mode_info
{
	glyph m_glyph;
	uint8_t m_character_mask;
	bool m_lower_case;
	bool m_invert;
};

struct character_info
{
	glyph m_glyph;
	uint8_t m_character;
	bool m_invert;
};

// drop mode flags that are not significant
constexpr uint8_t simplify_mode(uint8_t mode)
{
	return mode & ~((mode & MODE_AG) ? (MODE_AS | MODE_INV) : 0);
}

constexpr mode_info decode_mode(uint8_t mode, bool is_mc6847t1)
{
	mode = simplify_mode(mode);

	if (mode & MODE_AG)
		return { glyph::NONE, 0x00, false, false };

	if ((mode & ((is_mc6847t1 ? 0 : MODE_INTEXT) | MODE_AS)) == MODE_AS)
		return { glyph::SEMIGRAPHICS4, 0x0f, false, false };

	if (!is_mc6847t1 && ((mode & (MODE_INTEXT | MODE_AS)) == (MODE_INTEXT | MODE_AS)))
		return { glyph::SEMIGRAPHICS6, 0x3f, false, false };

	if (!is_mc6847t1 && ((mode & (MODE_INTEXT | MODE_AS)) == MODE_INTEXT))
		return { glyph::STRIPES, 0x7f, false, false };

	bool const lower_case = is_mc6847t1 && !(mode & MODE_INV) && (mode & MODE_GM0);
	bool const invert = bool(mode & MODE_INV) != (is_mc6847t1 && bool(mode & MODE_GM1));
	return { glyph::TEXT, 0x3f, lower_case, invert };
}

constexpr character_info decode_character(mode_info const &mode, uint8_t character)
{
	character &= mode.m_character_mask;
	bool invert = mode.m_invert;

	// the T1 lower case bank lives in the upper half of the font and is
	// displayed inverted relative to the selected polarity
	if (mode.m_lower_case && (character < 0x20))
	{
		character += 0x40;
		invert = !invert;
	}

	return { mode.m_glyph, character, invert };
}

constexpr character_info decode_character(uint8_t mode, uint8_t character, bool is_mc6847t1)
{
	return decode_character(decode_mode(mode, is_mc6847t1), character);
}

// "stripe" glyphs are generated directly from the character code
constexpr uint8_t stripes_row(uint8_t character)
{
	return uint8_t(~character);
}


//**************************************************************************
//  ROM EXPANSION
//**************************************************************************

//-------------------------------------------------
//  expand_internal_rom - convert the packed 5x7
//  internal font ROM to 8x12 glyph rows
//-------------------------------------------------

inline void expand_internal_rom(uint8_t const *rom, text_font &font)
{
	constexpr int NUM_CHARS = 64;
	constexpr int LINE_STRIDE = NUM_CHARS / 8;
	constexpr int GLYPH_ROWS = 7;
	constexpr int GLYPH_COLS = 5;
	constexpr int CHAR_STRIDE = LINE_STRIDE * GLYPH_ROWS;
	constexpr int GLYPH_BOTTOM_ROW = 28;

	// characters 64-95 mirror 0-31 (matching the original table layout)
	for (int ch = 0; ch < int(GLYPH_COUNT); ch++)
	{
		int const c = ch % NUM_CHARS;
		uint8_t *const out = font[ch];

		for (unsigned line = 0; line < GLYPH_HEIGHT; line++)
			out[line] = 0x00;

		int const byte_col = c >> 3;
		int const bit_shift = 7 - (c & 7);
		int base_index = GLYPH_BOTTOM_ROW * LINE_STRIDE + byte_col;

		for (int r = 0; r < GLYPH_ROWS; r++, base_index += LINE_STRIDE)
		{
			uint8_t glyph_byte = 0;
			int byte_index = base_index;

			for (int p = 0; p < GLYPH_COLS; p++, byte_index -= CHAR_STRIDE)
			{
				int const pixel = util::BIT(rom[byte_index], bit_shift);
				glyph_byte |= (pixel << (5 - p));
			}

			out[3 + r] = glyph_byte;
		}
	}
}


//-------------------------------------------------
//  expand_t1_rom - the MC6847T1 font ROM already
//  holds 8x12 glyph rows
//-------------------------------------------------

inline void expand_t1_rom(uint8_t const *rom, text_font &font)
{
	for (unsigned ch = 0; ch < GLYPH_COUNT; ch++)
	{
		for (unsigned line = 0; line < GLYPH_HEIGHT; line++)
			font[ch][line] = rom[ch * GLYPH_HEIGHT + line];
	}
}


//-------------------------------------------------
//  generate_semigraphics - the semigraphics fonts
//  are synthesized, not stored in ROM
//-------------------------------------------------

inline void generate_semigraphics(uint8_t *font, size_t char_count, size_t row_height)
{
	uint8_t *dest = font;

	for (size_t i = 0; i < char_count; i++)
	{
		for (size_t r = 0; r < GLYPH_HEIGHT; r++)
		{
			size_t const slice = (GLYPH_HEIGHT - 1 - r) / row_height;
			uint8_t const right = uint8_t(util::BIT(i, slice * 2));
			uint8_t const left = uint8_t(util::BIT(i, slice * 2 + 1));

			*dest++ = (left ? 0xf0 : 0x00) | (right ? 0x0f : 0x00);
		}
	}
}

} // namespace mc6847_charset

#endif // MAME_VIDEO_MC6847_CHARSET_H
