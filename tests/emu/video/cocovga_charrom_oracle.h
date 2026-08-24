// license:BSD-3-Clause
// copyright-holders:Stephen Spiller
/*
    cocovga_charrom_oracle.h

    Test-only expectations for the CoCoVGA character generator banks.

    CoCoVGA renders the MC6847 and MC6847T1 character generator ROMs into
    its own 8x12 cell and substitutes its own bitmaps for a handful of
    characters.  The substituted bitmaps below were transcribed
    mechanically from the CoCoVGA FPGA sources.

    Reproduced with authorization of rights holder Stephen Spiller and
    contributed under the BSD-3-Clause license of this file.  The FPGA
    sources are not publicly redistributed, so the exact revision the values
    came from is pinned here by hash:

        snapshot    62b780a9d8117c2c591e69191b7136bba02f053a
        file        CharRom.v
        sha256      60023fccf196f6a0d1c52f3cc1c402a24b5110ebf90e7e8c831d2c82a0f45226

    Public documentation of the same behaviour:

        http://cocovga.com/documentation/specifications/
        http://cocovga.com/documentation/software-mode-control/

    tests/emu/video/generate_cocovga_fixtures.py regenerates this file byte
    for byte from the pinned sources; --check verifies it in place.

    Derivation, which can be repeated from those sources alone:

      * decode every "N'hCC:" / "4'hRR: byteval = 8'hVV;" assignment in
        CharRom.v into rom[bank][character][row], where bank 1 is the
        MC6847T1 bank and bank 0 the standard MC6847 bank;
      * expand mc6847_charset.bin (CRC 3b22d071, SHA1 5e9d68e55e73cae3d28adaff34fe115e00029009,
        sha256 3ffb60b575a7edb088a7c64bb61ce4173febd6aaaedfa998c47d8b4945080e7d) into 8x12 glyph rows;
      * expand mc6847t1_charset_needredump.bin (CRC 42e62f8d, SHA1 1f09a076732a1e4b132cf298f0d1747df817e1c6,
        sha256 61498c6d2d252bd5a073ce62ecdc1ef1aa85936b2187d9d4d09e90a5c1fcd08e) into 8x12 glyph rows;
      * shift the T1 rows one row down and one pixel towards the left edge
        of the cell, as CoCoVGA does when it draws the T1 bank into its own
        cell;
      * list the characters whose FPGA rows differ from the ROM-derived
        rows.  Every character that is not listed below matches the ROM
        expansion exactly, so the tests take those expectations from the
        character generator ROM fixture instead.
*/

#ifndef MAME_TESTS_EMU_VIDEO_COCOVGA_CHARROM_ORACLE_H
#define MAME_TESTS_EMU_VIDEO_COCOVGA_CHARROM_ORACLE_H

#pragma once

#include "mc6847_charset_fixture.h"

#include <cstdint>

namespace cocovga::test
{

struct fpga_character_override
{
	std::uint8_t character;
	std::uint8_t rows[12];
};

inline constexpr fpga_character_override FPGA_STANDARD_OVERRIDES[] =
{
	{ 5, { 0x00, 0x00, 0x00, 0x3e, 0x20, 0x20, 0x38, 0x20, 0x20, 0x3e, 0x00, 0x00 } },
	{ 6, { 0x00, 0x00, 0x00, 0x3e, 0x20, 0x20, 0x38, 0x20, 0x20, 0x20, 0x00, 0x00 } },
	{ 51, { 0x00, 0x00, 0x00, 0x1c, 0x22, 0x02, 0x04, 0x02, 0x22, 0x1c, 0x00, 0x00 } }
};

inline constexpr fpga_character_override FPGA_T1_OVERRIDES[] =
{
	{ 0, { 0x00, 0x00, 0x38, 0x44, 0x04, 0x34, 0x4c, 0x4c, 0x38, 0x00, 0x00, 0x00 } },
	{ 7, { 0x00, 0x00, 0x38, 0x44, 0x40, 0x40, 0x4c, 0x44, 0x38, 0x00, 0x00, 0x00 } },
	{ 10, { 0x00, 0x00, 0x04, 0x04, 0x04, 0x04, 0x04, 0x44, 0x38, 0x00, 0x00, 0x00 } },
	{ 35, { 0x00, 0x00, 0x28, 0x28, 0x7c, 0x28, 0x7c, 0x28, 0x28, 0x00, 0x00, 0x00 } },
	{ 36, { 0x00, 0x00, 0x10, 0x3c, 0x50, 0x38, 0x14, 0x78, 0x10, 0x00, 0x00, 0x00 } },
	{ 39, { 0x00, 0x00, 0x10, 0x10, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ 44, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x20, 0x40, 0x00, 0x00 } },
	{ 46, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00 } },
	{ 51, { 0x00, 0x00, 0x38, 0x44, 0x04, 0x08, 0x04, 0x44, 0x38, 0x00, 0x00, 0x00 } },
	{ 54, { 0x00, 0x00, 0x38, 0x40, 0x40, 0x78, 0x44, 0x44, 0x38, 0x00, 0x00, 0x00 } },
	{ 57, { 0x00, 0x00, 0x38, 0x44, 0x44, 0x3c, 0x04, 0x04, 0x38, 0x00, 0x00, 0x00 } },
	{ 58, { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00 } },
	{ 59, { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x10, 0x20, 0x00, 0x00 } }
};

// Expected CoCoVGA text bank row, expressed against the character
// generator ROM fixture and the FPGA substitutions above.
inline std::uint8_t fpga_text_row(
		bool t1_font,
		std::uint8_t character,
		std::uint8_t row)
{
	character &= 0x3f;
	if (row >= 12)
		return 0;

	if (!t1_font)
	{
		for (fpga_character_override const &entry : FPGA_STANDARD_OVERRIDES)
		{
			if (character == entry.character)
				return entry.rows[row];
		}
		return mc6847_charset_test::internal_reference_row(
				mc6847_charset_test::internal_rom(),
				character,
				row);
	}

	for (fpga_character_override const &entry : FPGA_T1_OVERRIDES)
	{
		if (character == entry.character)
			return entry.rows[row];
	}

	// the T1 bank sits one row lower and one pixel further left
	return row == 0
			? 0
			: std::uint8_t(
					mc6847_charset_test::t1_reference_row(
							mc6847_charset_test::t1_rom(),
							character,
							row - 1) << 1);
}

} // namespace cocovga::test

#endif // MAME_TESTS_EMU_VIDEO_COCOVGA_CHARROM_ORACLE_H
