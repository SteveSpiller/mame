// license:BSD-3-Clause
// copyright-holders:Stephen Spiller

#include "catch.hpp"

#include "mc6847_charset_fixture.h"

#include "video/mc6847_charset.h"


TEST_CASE("MC6847 character generator ROM expansion is ROM agnostic", "[emu][video][mc6847]")
{
	using namespace mc6847_charset_test;

	SECTION("packed 5x7 internal font")
	{
		mc6847_charset::text_font font{};
		mc6847_charset::expand_internal_rom(internal_rom().data(), font);

		for (unsigned character = 0; character < mc6847_charset::GLYPH_COUNT; character++)
		{
			for (unsigned row = 0; row < mc6847_charset::GLYPH_HEIGHT; row++)
			{
				INFO("character " << character << " row " << row);
				REQUIRE(font[character][row] == internal_reference_row(internal_rom(), character, row));
			}
		}

		// characters 64-95 mirror 0-31, and glyphs occupy rows 3 through 9
		for (unsigned character = 64; character < mc6847_charset::GLYPH_COUNT; character++)
		{
			for (unsigned row = 0; row < mc6847_charset::GLYPH_HEIGHT; row++)
				REQUIRE(font[character][row] == font[character - 64][row]);
		}
		for (unsigned character = 0; character < mc6847_charset::GLYPH_COUNT; character++)
		{
			REQUIRE(font[character][0] == 0x00);
			REQUIRE(font[character][1] == 0x00);
			REQUIRE(font[character][2] == 0x00);
			REQUIRE(font[character][10] == 0x00);
			REQUIRE(font[character][11] == 0x00);
			for (unsigned row = 3; row < 10; row++)
				REQUIRE((font[character][row] & 0xc1) == 0x00);
		}
	}

	SECTION("expansion overwrites every row of the destination")
	{
		mc6847_charset::text_font font;
		for (auto &glyph : font)
		{
			for (auto &row : glyph)
				row = 0xa5;
		}

		mc6847_charset::expand_internal_rom(internal_rom().data(), font);
		for (unsigned character = 0; character < mc6847_charset::GLYPH_COUNT; character++)
		{
			for (unsigned row = 0; row < mc6847_charset::GLYPH_HEIGHT; row++)
				REQUIRE(font[character][row] == internal_reference_row(internal_rom(), character, row));
		}
	}

	SECTION("T1 font ROM holds ready made glyph rows")
	{
		mc6847_charset::text_font font{};
		mc6847_charset::expand_t1_rom(t1_rom().data(), font);

		for (unsigned character = 0; character < mc6847_charset::GLYPH_COUNT; character++)
		{
			for (unsigned row = 0; row < mc6847_charset::GLYPH_HEIGHT; row++)
			{
				INFO("character " << character << " row " << row);
				REQUIRE(font[character][row] == t1_reference_row(t1_rom(), character, row));
			}
		}
	}
}


TEST_CASE("MC6847 semigraphics fonts are generated, not stored", "[emu][video][mc6847]")
{
	uint8_t semigraphics4[mc6847_charset::SEMIGRAPHICS4_GLYPH_COUNT * mc6847_charset::GLYPH_HEIGHT];
	uint8_t semigraphics6[mc6847_charset::SEMIGRAPHICS6_GLYPH_COUNT * mc6847_charset::GLYPH_HEIGHT];

	mc6847_charset::generate_semigraphics(
			semigraphics4,
			mc6847_charset::SEMIGRAPHICS4_GLYPH_COUNT,
			mc6847_charset::SEMIGRAPHICS4_ROW_HEIGHT);
	mc6847_charset::generate_semigraphics(
			semigraphics6,
			mc6847_charset::SEMIGRAPHICS6_GLYPH_COUNT,
			mc6847_charset::SEMIGRAPHICS6_ROW_HEIGHT);

	SECTION("generated rows match the block layout")
	{
		for (unsigned character = 0; character < mc6847_charset::SEMIGRAPHICS4_GLYPH_COUNT; character++)
		{
			for (unsigned row = 0; row < mc6847_charset::GLYPH_HEIGHT; row++)
			{
				INFO("semigraphics 4 character " << character << " row " << row);
				REQUIRE(semigraphics4[character * mc6847_charset::GLYPH_HEIGHT + row]
						== mc6847_charset_test::semigraphics_reference_row(
								character,
								row,
								mc6847_charset::SEMIGRAPHICS4_ROW_HEIGHT));
			}
		}

		for (unsigned character = 0; character < mc6847_charset::SEMIGRAPHICS6_GLYPH_COUNT; character++)
		{
			for (unsigned row = 0; row < mc6847_charset::GLYPH_HEIGHT; row++)
			{
				INFO("semigraphics 6 character " << character << " row " << row);
				REQUIRE(semigraphics6[character * mc6847_charset::GLYPH_HEIGHT + row]
						== mc6847_charset_test::semigraphics_reference_row(
								character,
								row,
								mc6847_charset::SEMIGRAPHICS6_ROW_HEIGHT));
			}
		}
	}

	SECTION("known rows")
	{
		REQUIRE(semigraphics4[0x09 * mc6847_charset::GLYPH_HEIGHT + 0] == 0xf0);
		REQUIRE(semigraphics4[0x09 * mc6847_charset::GLYPH_HEIGHT + 6] == 0x0f);

		REQUIRE(semigraphics6[0x06 * mc6847_charset::GLYPH_HEIGHT + 3] == 0x00);
		REQUIRE(semigraphics6[0x06 * mc6847_charset::GLYPH_HEIGHT + 4] == 0x0f);
		REQUIRE(semigraphics6[0x06 * mc6847_charset::GLYPH_HEIGHT + 8] == 0xf0);
	}
}


TEST_CASE("MC6847 mode and character decoding preserves stock behavior", "[emu][video][mc6847]")
{
	namespace charset = mc6847_charset;
	using glyph = charset::glyph;

	SECTION("mode simplification")
	{
		REQUIRE(charset::simplify_mode(charset::MODE_AG | charset::MODE_GM1 | charset::MODE_AS | charset::MODE_INV)
				== (charset::MODE_AG | charset::MODE_GM1));
		REQUIRE(charset::simplify_mode(charset::MODE_GM1 | charset::MODE_AS | charset::MODE_INV)
				== (charset::MODE_GM1 | charset::MODE_AS | charset::MODE_INV));
	}

	SECTION("standard character modes")
	{
		auto mode = charset::decode_mode(0x00, false);
		REQUIRE(mode.m_glyph == glyph::TEXT);
		REQUIRE(mode.m_character_mask == 0x3f);
		REQUIRE_FALSE(mode.m_lower_case);
		REQUIRE_FALSE(mode.m_invert);

		auto character = charset::decode_character(charset::MODE_INV, 0x41, false);
		REQUIRE(character.m_glyph == glyph::TEXT);
		REQUIRE(character.m_character == 0x01);
		REQUIRE(character.m_invert);

		character = charset::decode_character(charset::MODE_AS, 0x79, false);
		REQUIRE(character.m_glyph == glyph::SEMIGRAPHICS4);
		REQUIRE(character.m_character == 0x09);

		character = charset::decode_character(charset::MODE_AS | charset::MODE_INTEXT, 0x46, false);
		REQUIRE(character.m_glyph == glyph::SEMIGRAPHICS6);
		REQUIRE(character.m_character == 0x06);

		character = charset::decode_character(charset::MODE_INTEXT, 0xaa, false);
		REQUIRE(character.m_glyph == glyph::STRIPES);
		REQUIRE(character.m_character == 0x2a);
		REQUIRE(charset::stripes_row(character.m_character) == 0xd5);
	}

	SECTION("T1 text modes")
	{
		auto character = charset::decode_character(charset::MODE_GM0, 0x01, true);
		REQUIRE(character.m_glyph == glyph::TEXT);
		REQUIRE(character.m_character == 0x41);
		REQUIRE(character.m_invert);

		character = charset::decode_character(charset::MODE_GM0 | charset::MODE_GM1, 0x01, true);
		REQUIRE(character.m_character == 0x41);
		REQUIRE_FALSE(character.m_invert);

		character = charset::decode_character(charset::MODE_GM0, 0x21, true);
		REQUIRE(character.m_character == 0x21);
		REQUIRE_FALSE(character.m_invert);

		character = charset::decode_character(charset::MODE_GM0 | charset::MODE_INV, 0x01, true);
		REQUIRE(character.m_character == 0x01);
		REQUIRE(character.m_invert);

		REQUIRE(charset::decode_mode(charset::MODE_INTEXT, true).m_glyph == glyph::TEXT);
		REQUIRE(charset::decode_mode(charset::MODE_AS | charset::MODE_INTEXT, true).m_glyph == glyph::SEMIGRAPHICS4);
	}

	SECTION("graphics modes do not decode a glyph")
	{
		auto const character = charset::decode_character(
				charset::MODE_AG | charset::MODE_AS | charset::MODE_INV,
				0xff,
				false);
		REQUIRE(character.m_glyph == glyph::NONE);
		REQUIRE(character.m_character == 0x00);
	}

	SECTION("all mode and character combinations follow the stock character map rules")
	{
		for (bool const is_mc6847t1 : { false, true })
		{
			for (unsigned raw_mode = 0; raw_mode < 0x100; raw_mode++)
			{
				uint8_t const mode = uint8_t(raw_mode);
				uint8_t const simplified = mode & ~((mode & charset::MODE_AG) ? (charset::MODE_AS | charset::MODE_INV) : 0);
				auto const decoded_mode = charset::decode_mode(mode, is_mc6847t1);

				glyph expected_glyph;
				uint8_t expected_mask;
				bool expected_lower_case = false;
				bool expected_invert = false;

				if (simplified & charset::MODE_AG)
				{
					expected_glyph = glyph::NONE;
					expected_mask = 0x00;
				}
				else if (is_mc6847t1)
				{
					if (simplified & charset::MODE_AS)
					{
						expected_glyph = glyph::SEMIGRAPHICS4;
						expected_mask = 0x0f;
					}
					else
					{
						expected_glyph = glyph::TEXT;
						expected_mask = 0x3f;
						expected_lower_case = !(simplified & charset::MODE_INV) && (simplified & charset::MODE_GM0);
						expected_invert = bool(simplified & charset::MODE_INV) != bool(simplified & charset::MODE_GM1);
					}
				}
				else
				{
					switch (simplified & (charset::MODE_INTEXT | charset::MODE_AS))
					{
					case 0:
						expected_glyph = glyph::TEXT;
						expected_mask = 0x3f;
						expected_invert = (simplified & charset::MODE_INV) != 0;
						break;

					case charset::MODE_AS:
						expected_glyph = glyph::SEMIGRAPHICS4;
						expected_mask = 0x0f;
						break;

					case charset::MODE_INTEXT:
						expected_glyph = glyph::STRIPES;
						expected_mask = 0x7f;
						break;

					default:
						expected_glyph = glyph::SEMIGRAPHICS6;
						expected_mask = 0x3f;
						break;
					}
				}

				REQUIRE(decoded_mode.m_glyph == expected_glyph);
				REQUIRE(decoded_mode.m_character_mask == expected_mask);
				REQUIRE(decoded_mode.m_lower_case == expected_lower_case);
				REQUIRE(decoded_mode.m_invert == expected_invert);

				for (unsigned raw_character = 0; raw_character < 0x100; raw_character++)
				{
					uint8_t expected_character = uint8_t(raw_character) & expected_mask;
					bool character_invert = expected_invert;
					if (expected_lower_case && (expected_character < 0x20))
					{
						expected_character += 0x40;
						character_invert = !character_invert;
					}

					auto const character = charset::decode_character(decoded_mode, uint8_t(raw_character));
					REQUIRE(character.m_glyph == expected_glyph);
					REQUIRE(character.m_character == expected_character);
					REQUIRE(character.m_invert == character_invert);
				}
			}
		}
	}
}
