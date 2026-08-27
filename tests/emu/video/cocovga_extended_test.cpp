// license:BSD-3-Clause
// copyright-holders:Stephen Spiller

#include "catch.hpp"

#include "cocovga_charrom_oracle.h"
#include "cocovga_lowercase_oracle.h"
#include "cocovga_test_helpers.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace cocovga;

captured_cell cell_from_stream(
		mode_kind mode,
		bool css,
		std::uint8_t stream,
		bool internal_external = false)
{
	return test::make_cell(
			mode,
			css,
			stream & 0x3f,
			(stream & 0x80) != 0,
			(stream & 0x40) != 0,
			internal_external);
}

captured_cell direct_lowercase_cell(
		std::uint8_t stream,
		bool css = false,
		bool internal_external = true)
{
	captured_cell result = cell_from_stream(
			mode_kind::ALPHA_SEMIGRAPHICS,
			css,
			stream,
			internal_external);
	result.gm = 1;
	return result;
}

std::uint8_t fpga_semigraphics_row(
		bool sg6,
		std::uint8_t character,
		std::uint8_t row)
{
	unsigned left_bit;
	unsigned right_bit;
	if (!sg6)
	{
		left_bit = row < 6 ? 3 : 1;
		right_bit = left_bit - 1;
	}
	else
	{
		left_bit = row < 4 ? 5 : row < 8 ? 3 : 1;
		right_bit = left_bit - 1;
	}
	return std::uint8_t(
			((character & (1U << left_bit)) != 0 ? 0xf0 : 0x00) |
			((character & (1U << right_bit)) != 0 ? 0x0f : 0x00));
}

} // anonymous namespace


TEST_CASE("CoCoVGA extended mode selection and constants are exact", "[cocovga][extended]")
{
	CHECK(W64_COLUMNS == 64);
	CHECK(W64_ROWS == 32);
	CHECK(W64_GLYPH_WIDTH == 8);
	CHECK(W64_GLYPH_HEIGHT == 12);
	CHECK(W64_LOGICAL_BYTES == 2'048);
	CHECK(W64_CAPTURE_ROW_BYTES == 32);
	CHECK(W64_HOST_LINE_REPEAT == 3);
	CHECK(VG6_LOGICAL_WIDTH == 128);
	CHECK(VG6_LOGICAL_HEIGHT == 96);
	CHECK(VG6_BYTES_PER_ROW == 64);
	CHECK(VG6_SCALE_X == 4);
	CHECK(VG6_SCALE_Y == 4);

	core control;
	CHECK(extended_renderer::selected_mode(control) == extended_mode::NONE);
	control.state().w64_active = true;
	CHECK(extended_renderer::selected_mode(control) == extended_mode::W64);
	control.state().vg6_active = true;
	CHECK(extended_renderer::selected_mode(control) == extended_mode::VG6);
	control.state().vg6_active = false;
	control.state().w64_active = false;
	CHECK(extended_renderer::selected_mode(control) == extended_mode::NONE);
}


TEST_CASE("CoCoVGA character generator expands both character ROM banks", "[cocovga][extended][glyphs][rom]")
{
	std::uint8_t const *const standard = mc6847_charset_test::internal_rom().data();
	std::uint8_t const *const t1 = mc6847_charset_test::t1_rom().data();

	character_generator font;
	font.load(standard, t1);

	// A second generator built from an altered T1 image, to show that the two
	// banks come from separate ROMs and that neither approximates the other.
	mc6847_charset_test::t1_rom_image altered_t1 = mc6847_charset_test::t1_rom();
	for (std::uint8_t &value : altered_t1)
		value = std::uint8_t(~value & 0x3f);
	character_generator altered;
	altered.load(standard, altered_t1.data());

	auto const override_row =
			[] (auto const &overrides, std::uint8_t character, std::uint8_t row, bool &found) -> std::uint8_t
			{
				for (auto const &entry : overrides)
				{
					if (entry.character == character)
					{
						found = true;
						return entry.rows[row];
					}
				}
				found = false;
				return 0;
			};

	for (unsigned character = 0; character < 64; ++character)
	{
		for (unsigned row = 0; row < W64_GLYPH_HEIGHT; ++row)
		{
			std::uint8_t const code = std::uint8_t(character);
			std::uint8_t const line = std::uint8_t(row);
			INFO("character " << character << " row " << row);

			// the standard bank is the MC6847 ROM expansion, with the glyphs
			// the FPGA substitutes for its own bitmaps
			bool substituted = false;
			std::uint8_t const standard_substitute =
					override_row(test::FPGA_STANDARD_OVERRIDES, code, line, substituted);
			std::uint8_t const standard_expected = substituted
					? standard_substitute
					: mc6847_charset_test::internal_reference_row(
							mc6847_charset_test::internal_rom(),
							character,
							row);
			CHECK(font.text_row(false, code, line) == standard_expected);

			// the T1 bank is the MC6847T1 ROM expansion moved one row down and
			// one pixel towards the left edge of the cell
			std::uint8_t const t1_substitute =
					override_row(test::FPGA_T1_OVERRIDES, code, line, substituted);
			std::uint8_t const t1_expected = substituted
					? t1_substitute
					: (row == 0
							? std::uint8_t(0)
							: std::uint8_t(mc6847_charset_test::t1_reference_row(
									mc6847_charset_test::t1_rom(),
									character,
									row - 1) << 1));
			CHECK(font.text_row(true, code, line) == t1_expected);

			// changing one ROM leaves the bank the other ROM feeds alone
			CHECK(altered.text_row(false, code, line) == font.text_row(false, code, line));

			// the FPGA-generated banks never depend on a character ROM
			if (character < 32)
			{
				CHECK(font.lowercase_row(false, code, line) ==
						test::FPGA_LOWERCASE_ROWS[0][character][row]);
				CHECK(font.lowercase_row(true, code, line) ==
						test::FPGA_LOWERCASE_ROWS[1][character][row]);
				CHECK(altered.lowercase_row(true, code, line) ==
						font.lowercase_row(true, code, line));
			}
			CHECK(altered.semigraphics4_row(code, line) == font.semigraphics4_row(code, line));
			CHECK(altered.semigraphics6_row(code, line) == font.semigraphics6_row(code, line));
		}
	}

	// the per-bank digests identify the ROM images without carrying glyph data
	CHECK(font.text_bank_digest(false) != font.text_bank_digest(true));
	CHECK(altered.text_bank_digest(false) == font.text_bank_digest(false));
	CHECK(altered.text_bank_digest(true) != font.text_bank_digest(true));

	character_generator const reloaded_but_identical = [&]
			{
				character_generator result;
				result.load(standard, t1);
				return result;
			}();
	CHECK(reloaded_but_identical.text_bank_digest(false) == font.text_bank_digest(false));
	CHECK(reloaded_but_identical.text_bank_digest(true) == font.text_bank_digest(true));

	// a generator with no ROMs attached draws nothing and is distinguishable
	CHECK(character_generator::blank().text_bank_digest(false) != font.text_bank_digest(false));
	CHECK(character_generator::blank().text_bank_digest(true) != font.text_bank_digest(true));
	for (unsigned character = 0; character < 64; ++character)
	{
		for (unsigned row = 0; row < W64_GLYPH_HEIGHT; ++row)
		{
			INFO("character " << character << " row " << row);
			CHECK(character_generator::blank().text_row(false, std::uint8_t(character), std::uint8_t(row)) == 0);
			CHECK(character_generator::blank().text_row(true, std::uint8_t(character), std::uint8_t(row)) == 0);
		}
	}
}


TEST_CASE("CoCoVGA character RAM lookup covers all 3072 bytes and every selected row", "[cocovga][extended][character-ram]")
{
	core control;
	for (std::uint16_t address = 0; address < CHARACTER_RAM_BYTES; ++address)
		REQUIRE(control.write_character(address, std::uint8_t(address * 43U + (address >> 4))));
	CHECK_FALSE(control.write_character(CHARACTER_RAM_BYTES, 0));

	for (unsigned character = 0; character < 256; ++character)
	{
		for (unsigned row = 0; row < W64_GLYPH_HEIGHT; ++row)
		{
			character_ram_row_result const result = extended_renderer::lookup_character_ram(
					control,
					std::uint8_t(character),
					std::uint8_t(row));
			std::uint16_t const address = std::uint16_t(character * W64_GLYPH_HEIGHT + row);
			INFO("character " << character << " row " << row);
			CHECK(result.valid);
			CHECK(result.character == character);
			CHECK(result.row == row);
			CHECK(result.address == address);
			CHECK(result.bits == std::uint8_t(address * 43U + (address >> 4)));
		}

		character_ram_row_result const invalid = extended_renderer::lookup_character_ram(
				control,
				std::uint8_t(character),
				W64_GLYPH_HEIGHT);
		CHECK_FALSE(invalid.valid);
		CHECK(invalid.address == INVALID_CAPTURE_ADDRESS);
	}

	REQUIRE(test::write_register(control, register_bank::ENHANCED_MODES, 8, 0x04));
	for (unsigned stream = 0; stream < 256; ++stream)
	{
		captured_cell const cell = cell_from_stream(mode_kind::W64, false, std::uint8_t(stream));
		for (unsigned row = 0; row < W64_GLYPH_HEIGHT; ++row)
		{
			character_row_result const result = cocovga::test::make_extended_renderer().resolve_character_row(
					control,
					cell,
					std::uint8_t(row),
					character_layout::W64);
			std::uint16_t const address = std::uint16_t(stream * W64_GLYPH_HEIGHT + row);
			INFO("stream " << stream << " row " << row);
			CHECK(result.valid);
			CHECK(result.character_ram_requested);
			CHECK(result.character_ram_used);
			CHECK(result.source == character_source::UPLOADED_RAM);
			CHECK(result.character == stream);
			CHECK(result.character_ram_address == address);
			CHECK(result.bits == std::uint8_t(address * 43U + (address >> 4)));
			CHECK(result.glyph ==
					((stream & 0x80) ? character_glyph::SEMIGRAPHICS4 : character_glyph::TEXT));
		}
	}
}


TEST_CASE("CoCoVGA character resolver follows pixelGen glyph and RAM priority", "[cocovga][extended][glyphs]")
{
	core control;
	std::uint8_t const letter_a_row3 = test::fpga_text_row(false, 0x01, 3);

	character_row_result result = cocovga::test::make_extended_renderer().resolve_character_row(
			control,
			cell_from_stream(mode_kind::ALPHA_SEMIGRAPHICS, false, 0x01),
			3,
			character_layout::NATIVE_32);
	CHECK(result.valid);
	CHECK(result.glyph == character_glyph::TEXT);
	CHECK(result.source == character_source::CHARACTER_ROM);
	CHECK(result.character == 0x01);
	CHECK_FALSE(result.inverted);
	CHECK(result.bits == letter_a_row3);

	result = cocovga::test::make_extended_renderer().resolve_character_row(
			control,
			cell_from_stream(mode_kind::ALPHA_SEMIGRAPHICS, false, 0x41),
			3,
			character_layout::NATIVE_32);
	CHECK(result.valid);
	CHECK(result.glyph == character_glyph::TEXT);
	CHECK(result.source == character_source::CHARACTER_ROM);
	CHECK_FALSE(result.inverted);
	CHECK(result.bits == letter_a_row3);

	result = cocovga::test::make_extended_renderer().resolve_character_row(
			control,
			cell_from_stream(mode_kind::ALPHA_SEMIGRAPHICS, false, 0xb9),
			0,
			character_layout::NATIVE_32);
	CHECK(result.valid);
	CHECK(result.glyph == character_glyph::SEMIGRAPHICS4);
	CHECK(result.source == character_source::GENERATED);
	CHECK(result.character == 9);
	CHECK(result.bits == 0xf0);

	REQUIRE(test::write_register(control, register_bank::EXTRAS, 5, 0x50));
	result = cocovga::test::make_extended_renderer().resolve_character_row(
			control,
			cell_from_stream(mode_kind::ALPHA_SEMIGRAPHICS, false, 0x86, true),
			4,
			character_layout::NATIVE_32);
	CHECK(result.valid);
	CHECK(result.glyph == character_glyph::SEMIGRAPHICS6);
	CHECK(result.source == character_source::GENERATED);
	CHECK(result.character == 6);
	CHECK(result.bits == 0x0f);

	result = cocovga::test::make_extended_renderer().resolve_character_row(
			control,
			cell_from_stream(mode_kind::ALPHA_SEMIGRAPHICS, false, 0x2a, true),
			7,
			character_layout::NATIVE_32);
	CHECK(result.valid);
	CHECK(result.glyph == character_glyph::TEXT);
	CHECK(result.source == character_source::CHARACTER_ROM);
	CHECK(result.character == 0x2a);
	CHECK(result.bits == cocovga::test::make_extended_renderer().text_character_row(false, 0x2a, 7));

	REQUIRE(test::write_register(control, register_bank::FONT, 3, 0x02));
	result = cocovga::test::make_extended_renderer().resolve_character_row(
			control,
			cell_from_stream(mode_kind::ALPHA_SEMIGRAPHICS, false, 0x41),
			3,
			character_layout::NATIVE_32);
	CHECK(result.valid);
	CHECK(result.glyph == character_glyph::TEXT);
	CHECK(result.source == character_source::CHARACTER_ROM);
	CHECK(result.character == 0x01);
	CHECK_FALSE(result.inverted);
	CHECK(result.bits == letter_a_row3);

	result = cocovga::test::make_extended_renderer().resolve_character_row(
			control,
			cell_from_stream(mode_kind::ALPHA_SEMIGRAPHICS, false, 0x01),
			3,
			character_layout::NATIVE_32);
	CHECK(result.valid);
	CHECK(result.source == character_source::LOWERCASE_CHARACTER_ROM);
	CHECK(result.character == 0x41);
	CHECK_FALSE(result.inverted);
	CHECK(result.bits == 0x00);

	control.clear_character_ram(0x5a);
	REQUIRE(test::write_register(control, register_bank::FONT, 3, 0x04));
	REQUIRE(test::write_register(control, register_bank::ENHANCED_MODES, 8, 0x04));
	result = cocovga::test::make_extended_renderer().resolve_character_row(
			control,
			cell_from_stream(mode_kind::ALPHA_SEMIGRAPHICS, false, 0x21),
			5,
			character_layout::NATIVE_32);
	CHECK(result.valid);
	CHECK(result.character_ram_requested);
	CHECK(result.character_ram_used);
	CHECK(result.source == character_source::UPLOADED_RAM);
	CHECK(result.character_ram_address == 0x21 * 12 + 5);
	CHECK_FALSE(result.inverted);
	CHECK(result.bits == 0x5a);

	REQUIRE(test::write_register(control, register_bank::EXTRAS, 5, 0x40));
	result = cocovga::test::make_extended_renderer().resolve_character_row(
			control,
			cell_from_stream(mode_kind::ALPHA_SEMIGRAPHICS, false, 0x86, true),
			4,
			character_layout::NATIVE_32);
	CHECK(result.valid);
	CHECK(result.character_ram_requested);
	CHECK(result.character_ram_used);
	CHECK(result.source == character_source::UPLOADED_RAM);
	CHECK(result.glyph == character_glyph::SEMIGRAPHICS4);
	CHECK(result.bits == 0x5a);

	REQUIRE(test::write_register(control, register_bank::EXTRAS, 5, 0x50));
	result = cocovga::test::make_extended_renderer().resolve_character_row(
			control,
			cell_from_stream(mode_kind::ALPHA_SEMIGRAPHICS, false, 0x86, true),
			4,
			character_layout::NATIVE_32);
	CHECK(result.valid);
	CHECK(result.character_ram_requested);
	CHECK_FALSE(result.character_ram_used);
	CHECK(result.source == character_source::GENERATED);
	CHECK(result.glyph == character_glyph::SEMIGRAPHICS6);
	CHECK(result.bits == 0x0f);

	result = cocovga::test::make_extended_renderer().resolve_character_row(
			control,
			cell_from_stream(mode_kind::ALPHA_SEMIGRAPHICS, false, 0),
			W64_GLYPH_HEIGHT,
			character_layout::NATIVE_32);
	CHECK_FALSE(result.valid);
	result = cocovga::test::make_extended_renderer().resolve_character_row(
			control,
			test::make_cell(mode_kind::CG2, false, 0),
			0,
			character_layout::NATIVE_32);
	CHECK_FALSE(result.valid);
	result = cocovga::test::make_extended_renderer().resolve_character_row(
			control,
			test::make_cell(mode_kind::ALPHA_SEMIGRAPHICS, false, 0),
			0,
			character_layout::W64);
	CHECK_FALSE(result.valid);
}


TEST_CASE("CoCoVGA MODEDEMO direct GM0 uses exact ROM rows on every board model", "[cocovga][extended][glyphs][lowercase][oracle][modedemo]")
{
	for (model board : { model::AMC2, model::MODERN, model::T1 })
	{
		for (unsigned font_bank = 0; font_bank < 2; ++font_bank)
		{
			core control(board);
			REQUIRE(test::write_register(
					control,
					register_bank::FONT,
					3,
					std::uint8_t(font_bank)));

			for (bool css : { false, true })
			{
				for (unsigned character = 0; character < 32; ++character)
				{
					for (unsigned row = 0; row < W64_GLYPH_HEIGHT; ++row)
					{
						character_row_result const result =
								cocovga::test::make_extended_renderer().resolve_character_row(
										control,
										direct_lowercase_cell(std::uint8_t(character), css),
										std::uint8_t(row),
										character_layout::NATIVE_32);
						INFO("board " << unsigned(board) << " font bank " << font_bank
								<< " CSS " << css << " lowercase character " << character
								<< " row " << row);
						REQUIRE(result.valid);
						CHECK(result.glyph == character_glyph::TEXT);
						CHECK(result.character == character + 0x40);
						CHECK_FALSE(result.inverted);
						CHECK(result.bits ==
								test::FPGA_LOWERCASE_ROWS[font_bank][character][row]);
					}
				}
			}

			for (unsigned character = 0; character < 64; ++character)
			{
				std::uint8_t const stream = std::uint8_t(
						character < 32 ? character | 0x40 : character);
				for (unsigned row = 0; row < W64_GLYPH_HEIGHT; ++row)
				{
					character_row_result const result =
							cocovga::test::make_extended_renderer().resolve_character_row(
									control,
									direct_lowercase_cell(stream),
									std::uint8_t(row),
									character_layout::NATIVE_32);
					INFO("board " << unsigned(board) << " font bank " << font_bank
							<< " CharRom character " << character << " row " << row);
					REQUIRE(result.valid);
					CHECK(result.glyph == character_glyph::TEXT);
					CHECK(result.source == character_source::CHARACTER_ROM);
					CHECK(result.character == character);
					CHECK_FALSE(result.inverted);
					CHECK(result.bits ==
							test::fpga_text_row(
									font_bank != 0,
									std::uint8_t(character),
									std::uint8_t(row)));
				}
			}
		}
	}

	for (model board : { model::AMC2, model::MODERN, model::T1 })
	{
		for (unsigned font_bank = 0; font_bank < 2; ++font_bank)
		{
			core control(board);
			control.state().w64_active = true;
			REQUIRE(test::write_register(
					control,
					register_bank::FONT,
					3,
					std::uint8_t(font_bank | 0x02)));
			for (unsigned character = 0; character < 32; ++character)
			{
				for (unsigned row = 0; row < W64_GLYPH_HEIGHT; ++row)
				{
					character_row_result const result =
							cocovga::test::make_extended_renderer().resolve_character_row(
									control,
									cell_from_stream(
											mode_kind::W64,
											false,
											std::uint8_t(character)),
									std::uint8_t(row),
									character_layout::W64);
					INFO("board " << unsigned(board) << " W64 font bank " << font_bank
							<< " character " << character << " row " << row);
					REQUIRE(result.valid);
					CHECK(result.bits ==
							test::FPGA_LOWERCASE_ROWS[font_bank][character][row]);
				}
			}
		}
	}
}


TEST_CASE("CoCoVGA W64 maps every NitrOS-9 letter to exact FPGA glyph rows without bitmap inversion", "[cocovga][extended][w64][glyphs][lowercase][oracle]")
{
	for (unsigned font_bank = 0; font_bank < 2; ++font_bank)
	{
		core control;
		control.state().w64_active = true;
		REQUIRE(test::write_register(
				control,
				register_bank::FONT,
				3,
				std::uint8_t(0x02 | font_bank)));

		for (bool inverse_text : { false, true })
		{
			REQUIRE(test::write_register(
					control,
					register_bank::EXTRAS,
					5,
					inverse_text ? 0x20 : 0));
			for (unsigned letter = 1; letter <= 26; ++letter)
			{
				for (unsigned row = 0; row < W64_GLYPH_HEIGHT; ++row)
				{
					character_row_result const lower = cocovga::test::make_extended_renderer().resolve_character_row(
							control,
							cell_from_stream(mode_kind::W64, false, std::uint8_t(letter)),
							std::uint8_t(row),
							character_layout::W64);
					character_row_result const upper = cocovga::test::make_extended_renderer().resolve_character_row(
							control,
							cell_from_stream(mode_kind::W64, false, std::uint8_t(0x40 + letter)),
							std::uint8_t(row),
							character_layout::W64);
					INFO("font bank " << font_bank << " inverse text " << inverse_text
							<< " letter " << letter << " row " << row);

					REQUIRE(lower.valid);
					CHECK(lower.glyph == character_glyph::TEXT);
					CHECK(lower.source == character_source::LOWERCASE_CHARACTER_ROM);
					CHECK(lower.character == 0x40 + letter);
					CHECK_FALSE(lower.inverted);
					CHECK(lower.bits == test::FPGA_LOWERCASE_ROWS[font_bank][letter][row]);

					REQUIRE(upper.valid);
					CHECK(upper.glyph == character_glyph::TEXT);
					CHECK(upper.source == character_source::CHARACTER_ROM);
					CHECK(upper.character == letter);
					CHECK_FALSE(upper.inverted);
					CHECK(upper.bits ==
							test::fpga_text_row(
									font_bank != 0,
									std::uint8_t(letter),
									std::uint8_t(row)));
				}
			}
		}
	}
}

TEST_CASE("CoCoVGA W64 keeps pixelGen SG6 wires but suppresses GM0 lowercase", "[cocovga][extended][w64][glyphs][oracle]")
{
	for (model board : { model::AMC2, model::MODERN, model::T1 })
	{
		for (unsigned font_bank = 0; font_bank < 2; ++font_bank)
		{
			for (bool extras_bit4 : { false, true })
			{
				for (bool internal_external : { false, true })
				{
					for (bool gm0 : { false, true })
					{
						core control(board);
						control.state().w64_active = true;
						REQUIRE(test::write_register(
								control,
								register_bank::FONT,
								3,
								std::uint8_t(font_bank)));
						REQUIRE(test::write_register(
								control,
								register_bank::EXTRAS,
								5,
								std::uint8_t(0x40 | (extras_bit4 ? 0x10 : 0))));
						bool const sg6 = (internal_external || gm0) && extras_bit4;

						for (unsigned character = 0; character < 64; ++character)
						{
							captured_cell cell = cell_from_stream(
									mode_kind::W64,
									false,
									std::uint8_t(0x80 | character),
									internal_external);
							cell.gm = gm0 ? 1 : 2;
							for (unsigned row = 0; row < W64_GLYPH_HEIGHT; ++row)
							{
								character_row_result const result =
										cocovga::test::make_extended_renderer().resolve_character_row(
												control,
												cell,
												std::uint8_t(row),
												character_layout::W64);
								std::uint8_t const expected_character =
										std::uint8_t(character & (sg6 ? 0x3f : 0x0f));
								INFO("board " << unsigned(board) << " font bank " << font_bank
										<< " extras bit4 " << extras_bit4
										<< " internal/external " << internal_external
										<< " GM0 " << gm0 << " character " << character
										<< " row " << row);
								REQUIRE(result.valid);
								CHECK(result.glyph ==
										(sg6
												? character_glyph::SEMIGRAPHICS6
												: character_glyph::SEMIGRAPHICS4));
								CHECK(result.source == character_source::GENERATED);
								CHECK(result.character == expected_character);
								CHECK(result.bits ==
										fpga_semigraphics_row(
												sg6,
												expected_character,
												std::uint8_t(row)));
							}
						}
					}
				}
			}

			core control(board);
			control.state().w64_active = true;
			REQUIRE(test::write_register(
					control,
					register_bank::FONT,
					3,
					std::uint8_t(font_bank)));
			for (unsigned character = 0; character < 32; ++character)
			{
				captured_cell cell = cell_from_stream(
						mode_kind::W64,
						false,
						std::uint8_t(character));
				cell.gm = 1;
				for (unsigned row = 0; row < W64_GLYPH_HEIGHT; ++row)
				{
					character_row_result const result =
							cocovga::test::make_extended_renderer().resolve_character_row(
									control,
									cell,
									std::uint8_t(row),
									character_layout::W64);
					INFO("board " << unsigned(board) << " font bank " << font_bank
							<< " W64 GM0 CharRom character " << character << " row " << row);
					REQUIRE(result.valid);
					CHECK(result.glyph == character_glyph::TEXT);
					CHECK(result.source == character_source::CHARACTER_ROM);
					CHECK(result.character == character);
					CHECK(result.bits ==
							cocovga::test::make_extended_renderer().text_character_row(
									font_bank != 0,
									std::uint8_t(character),
									std::uint8_t(row)));
				}
			}
		}
	}
}


TEST_CASE("CoCoVGA character resolver matches pixelGen selection for every alpha byte", "[cocovga][extended][glyphs][oracle]")
{
	for (model board : { model::AMC2, model::MODERN, model::T1 })
	{
		for (unsigned font_bank = 0; font_bank < 2; ++font_bank)
		{
			for (bool extras_bit4 : { false, true })
			{
				for (bool internal_external : { false, true })
				{
					for (bool gm0 : { false, true })
					{
						for (bool force_lowercase : { false, true })
						{
							core control(board);
							REQUIRE(test::write_register(
									control,
									register_bank::FONT,
									3,
									std::uint8_t(
											font_bank |
											(force_lowercase ? 0x02 : 0))));
							REQUIRE(test::write_register(
									control,
									register_bank::EXTRAS,
									5,
									std::uint8_t(0x40 | (extras_bit4 ? 0x10 : 0))));

							for (unsigned stream = 0; stream < 256; ++stream)
							{
								captured_cell cell = cell_from_stream(
										mode_kind::ALPHA_SEMIGRAPHICS,
										(stream & 1) != 0,
										std::uint8_t(stream),
										internal_external);
								cell.gm = gm0 ? 1 : 0;
								bool const alpha_semigraphics = (stream & 0x80) != 0;
								bool const sg6 =
										alpha_semigraphics &&
										(internal_external || gm0) &&
										extras_bit4;
								bool const lowercase =
										!alpha_semigraphics &&
										(force_lowercase || gm0) &&
										(stream & 0x60) == 0;
								character_glyph const expected_glyph =
										alpha_semigraphics
												? (sg6
														? character_glyph::SEMIGRAPHICS6
														: character_glyph::SEMIGRAPHICS4)
												: character_glyph::TEXT;
								character_source const expected_source =
										alpha_semigraphics
												? character_source::GENERATED
												: lowercase
														? character_source::LOWERCASE_CHARACTER_ROM
														: character_source::CHARACTER_ROM;
								std::uint8_t const expected_character =
										alpha_semigraphics
												? std::uint8_t(stream & (sg6 ? 0x3f : 0x0f))
												: lowercase
														? std::uint8_t((stream & 0x1f) + 0x40)
														: std::uint8_t(stream & 0x3f);

								for (unsigned row = 0; row < W64_GLYPH_HEIGHT; ++row)
								{
									character_row_result const result =
											cocovga::test::make_extended_renderer().resolve_character_row(
													control,
													cell,
													std::uint8_t(row),
													character_layout::NATIVE_32);
									std::uint8_t const expected_bits =
											alpha_semigraphics
													? fpga_semigraphics_row(
															sg6,
															expected_character,
															std::uint8_t(row))
													: lowercase
															? test::FPGA_LOWERCASE_ROWS
																	[font_bank][stream & 0x1f][row]
															: cocovga::test::make_extended_renderer().text_character_row(
																	font_bank != 0,
																	expected_character,
																	std::uint8_t(row));
									INFO("model " << unsigned(board) << " font bank " << font_bank
											<< " extras bit4 " << extras_bit4
											<< " internal/external " << internal_external
											<< " GM0 " << gm0
											<< " force lowercase " << force_lowercase
											<< " stream " << stream << " row " << row);
									REQUIRE(result.valid);
									CHECK(result.stream_byte == stream);
									CHECK(result.character == expected_character);
									CHECK(result.row == row);
									CHECK(result.glyph == expected_glyph);
									CHECK(result.source == expected_source);
									CHECK_FALSE(result.inverted);
									CHECK(result.bits == expected_bits);
									CHECK_FALSE(result.character_ram_requested);
									CHECK_FALSE(result.character_ram_used);
								}
							}
						}
					}
				}
			}
		}
	}
}


TEST_CASE("CoCoVGA W64 cell and viewport mapping is exhaustive", "[cocovga][extended][w64]")
{
	for (unsigned row = 0; row < W64_ROWS; ++row)
	{
		for (unsigned column = 0; column < W64_COLUMNS; ++column)
		{
			w64_cell_location const location = extended_renderer::map_w64_cell(
					std::uint8_t(row),
					std::uint8_t(column));
			std::uint8_t const capture_row = std::uint8_t(row * 2 + column / 32);
			std::uint16_t const address = std::uint16_t(row * W64_COLUMNS + column);
			INFO("row " << row << " column " << column);
			CHECK(location.valid);
			CHECK(location.text_row == row);
			CHECK(location.text_column == column);
			CHECK(location.capture_row == capture_row);
			CHECK(location.capture_column == column % 32);
			CHECK(location.capture_address == address);
			CHECK(location.first_host_line == capture_row * W64_HOST_LINE_REPEAT);
			CHECK(location.last_host_line == location.first_host_line + W64_HOST_LINE_REPEAT - 1);
		}
	}
	CHECK_FALSE(extended_renderer::map_w64_cell(W64_ROWS, 0).valid);
	CHECK_FALSE(extended_renderer::map_w64_cell(0, W64_COLUMNS).valid);

	std::array<std::pair<std::uint16_t, std::uint16_t>, 6> const points =
	{{
		{ 0, 0 },
		{ 7, 11 },
		{ 8, 12 },
		{ 255, 191 },
		{ 510, 382 },
		{ 511, 383 }
	}};
	for (auto const &point : points)
	{
		w64_pixel_location const location =
				extended_renderer::map_w64_viewport_pixel(point.first, point.second);
		REQUIRE(location.valid);
		CHECK(location.cell.text_column == point.first / W64_GLYPH_WIDTH);
		CHECK(location.cell.text_row == point.second / W64_GLYPH_HEIGHT);
		CHECK(location.glyph_column == point.first % W64_GLYPH_WIDTH);
		CHECK(location.glyph_row == point.second % W64_GLYPH_HEIGHT);
		CHECK(location.glyph_mask == (0x80U >> location.glyph_column));
	}
	CHECK_FALSE(extended_renderer::map_w64_viewport_pixel(VIEWPORT_WIDTH, 0).valid);
	CHECK_FALSE(extended_renderer::map_w64_viewport_pixel(0, VIEWPORT_HEIGHT).valid);
}


TEST_CASE("CoCoVGA VG6 mapping and decode cover the complete viewport", "[cocovga][extended][vg6]")
{
	for (std::uint16_t y = 0; y < VIEWPORT_HEIGHT; ++y)
	{
		for (std::uint16_t x = 0; x < VIEWPORT_WIDTH; ++x)
		{
			vg6_pixel_location const location = extended_renderer::map_vg6_viewport_pixel(x, y);
			std::uint8_t const logical_x = std::uint8_t(x / VG6_SCALE_X);
			std::uint8_t const logical_y = std::uint8_t(y / VG6_SCALE_Y);
			std::uint16_t const capture_address = std::uint16_t(
					std::uint16_t(logical_y) * VG6_BYTES_PER_ROW + logical_x / 2);
			INFO("x " << x << " y " << y);
			CHECK(location.valid);
			CHECK(location.logical_x == logical_x);
			CHECK(location.logical_y == logical_y);
			CHECK(location.capture_address == capture_address);
			CHECK(location.capture_row == capture_address / W64_CAPTURE_ROW_BYTES);
			CHECK(location.capture_column == capture_address % W64_CAPTURE_ROW_BYTES);
			CHECK(location.nibble_shift == ((logical_x & 1) == 0 ? 4 : 0));
		}
	}
	CHECK_FALSE(extended_renderer::map_vg6_viewport_pixel(VIEWPORT_WIDTH, 0).valid);
	CHECK_FALSE(extended_renderer::map_vg6_viewport_pixel(0, VIEWPORT_HEIGHT).valid);

	core control;
	frame_buffer_state frame = test::make_vg6_frame(false, 0);
	frame.cells[0].data = 0xa5;
	frame.cells[0].flags &= ~captured_cell::FLAG_CSS;
	vg6_pixel_result decoded = extended_renderer::decode_vg6_viewport_pixel(control, frame, 0, 0);
	REQUIRE(decoded.valid);
	CHECK(decoded.location.capture_address == 0);
	CHECK(decoded.location.nibble_shift == 4);
	CHECK(decoded.packed_byte == 0xa5);
	CHECK(decoded.pixel == 0x0a);
	CHECK_FALSE(decoded.css);
	CHECK(decoded.palette_index == 10);
	CHECK(decoded.color == renderer::pack_color(control.artifact_color(10)));

	decoded = extended_renderer::decode_vg6_viewport_pixel(control, frame, 4, 0);
	REQUIRE(decoded.valid);
	CHECK(decoded.location.capture_address == 0);
	CHECK(decoded.location.nibble_shift == 0);
	CHECK(decoded.pixel == 0x05);
	CHECK(decoded.palette_index == 5);

	frame.cells[0].flags |= captured_cell::FLAG_CSS;
	decoded = extended_renderer::decode_vg6_viewport_pixel(control, frame, 0, 0);
	REQUIRE(decoded.valid);
	CHECK(decoded.css);
	CHECK(decoded.palette_index == 26);

	frame.cells[0].mode_id = static_cast<std::uint8_t>(mode_kind::CG6);
	CHECK_FALSE(extended_renderer::decode_vg6_viewport_pixel(control, frame, 0, 0).valid);
	CHECK_FALSE(extended_renderer::decode_vg6_viewport_pixel(control, frame, VIEWPORT_WIDTH, 0).valid);
}


TEST_CASE("CoCoVGA extended renderer covers W64 and VG6 for every model and CSS", "[cocovga][extended][w64][vg6]")
{
	constexpr std::array<extended_mode, 2> modes =
		{ extended_mode::W64, extended_mode::VG6 };

	extended_renderer output = cocovga::test::make_extended_renderer();
	for (extended_mode mode : modes)
	{
		for (model board : { model::AMC2, model::MODERN, model::T1 })
		{
			for (bool css : { false, true })
			{
				core control(board);
				frame_buffer_state frame;
				if (mode == extended_mode::W64)
				{
					control.state().w64_active = true;
					frame = test::make_w64_frame(css, std::uint8_t(0x19 + unsigned(board) * 23U));
				}
				else
				{
					control.state().vg6_active = true;
					frame = test::make_vg6_frame(css, std::uint8_t(0x61 + unsigned(board) * 17U));
				}

				std::vector<render_pixel> pixels;
				pixel_surface const surface = test::make_surface(pixels);
				extended_result const result = output.render(mode, control, frame, surface);
				INFO("mode " << unsigned(mode) << " model " << unsigned(board)
						<< " css " << css << " status " << extended_renderer::status_name(result.status));
				REQUIRE(result.success());
				CHECK(result.mode == mode);
				CHECK(result.generation == frame.generation);
				CHECK(result.pixels_written == std::uint32_t(RENDER_WIDTH) * RENDER_HEIGHT);
				CHECK(result.scanlines_written == RENDER_HEIGHT);
				CHECK(std::all_of(
						pixels.begin(),
						pixels.end(),
						[] (render_pixel pixel) { return (pixel & 0xff000000U) == 0xff000000U; }));

				if (mode == extended_mode::VG6)
				{
					CHECK(pixels[0] == 0xff000000U);
					CHECK(pixels[std::size_t(VIEWPORT_TOP) * RENDER_WIDTH + VIEWPORT_LEFT - 1] == 0xff000000U);
					vg6_pixel_result const first =
							extended_renderer::decode_vg6_viewport_pixel(control, frame, 0, 0);
					REQUIRE(first.valid);
					CHECK(pixels[std::size_t(VIEWPORT_TOP) * RENDER_WIDTH + VIEWPORT_LEFT] ==
							first.color);
				}
				else
				{
					CHECK(pixels[0] == 0xff000000U);
				}
			}
		}
	}
}


TEST_CASE("CoCoVGA W64 renderer selects uploaded character RAM and custom borders", "[cocovga][extended][w64][character-ram][borders]")
{
	core control;
	control.state().w64_active = true;
	for (std::uint16_t address = 0; address < CHARACTER_RAM_BYTES; ++address)
		REQUIRE(control.write_character(address, std::uint8_t(address * 7U + 3U)));
	REQUIRE(test::write_register(control, register_bank::ENHANCED_MODES, 8, 0x04));
	REQUIRE(test::write_register(control, register_bank::EXTRA_PALETTE, 132, 0xfc));
	REQUIRE(control.write_page00(133, 0x00));
	REQUIRE(control.commit_palettes_at_vsync());
	REQUIRE(test::write_register(control, register_bank::EXTRAS, 5, 0x43));

	frame_buffer_state frame = test::make_w64_frame(false, 0x22);
	std::vector<render_pixel> pixels;
	pixel_surface const surface = test::make_surface(pixels);
	extended_renderer output = cocovga::test::make_extended_renderer();
	extended_result const result = output.render(extended_mode::W64, control, frame, surface);
	REQUIRE(result.success());
	render_pixel const border = renderer::pack_color(control.extra_color(extra_palette_slot::BORDER));
	CHECK(pixels[0] == border);
	CHECK(pixels[std::size_t(RENDER_HEIGHT - 1) * RENDER_WIDTH] == border);
}

TEST_CASE("CoCoVGA W64 and character-RAM rendering use the documented SG palette slot", "[cocovga][extended][w64][character-ram][palette][oracle]")
{
	for (bool character_ram : { false, true })
	{
		for (bool css : { false, true })
		{
			for (unsigned code = 0; code < 8; ++code)
			{
				core control;
				control.state().w64_active = true;
				if (character_ram)
					REQUIRE(test::write_register(control, register_bank::ENHANCED_MODES, 8, 0x04));

				frame_buffer_state frame = test::make_w64_frame(css, 0);
				frame.cells[0] = test::make_cell(
						mode_kind::W64,
						css,
						std::uint8_t(((code & 3) << 4) | 0x09),
						true,
						(code & 4) != 0);
				if (character_ram)
				{
					REQUIRE(control.write_character(
							core::character_address(frame.cells[0].stream_byte(), 0),
							0xf0));
				}

				std::array<render_pixel, RENDER_WIDTH> line{};
				extended_renderer output = cocovga::test::make_extended_renderer();
				extended_result const result = output.render_scanline(
						extended_mode::W64,
						control,
						frame,
						VIEWPORT_TOP,
						{ line.data(), line.size() });
				REQUIRE(result.success());

				render_pixel const foreground =
						renderer::pack_color(control.semigraphics_color(1 + code));
				render_pixel const background =
						renderer::pack_color(control.semigraphics_color(0));
				INFO("character RAM " << character_ram << " css " << css << " code " << code);
				for (unsigned bit = 0; bit < W64_GLYPH_WIDTH; ++bit)
				{
					CHECK(line[VIEWPORT_LEFT + bit] ==
							(bit < 4 ? foreground : background));
				}
			}
		}
	}
}


TEST_CASE("CoCoVGA extended renderer rejects every invalid and inconsistent path", "[cocovga][extended][errors]")
{
	extended_renderer output = cocovga::test::make_extended_renderer();
	core w64_control;
	w64_control.state().w64_active = true;
	frame_buffer_state w64 = test::make_w64_frame(false);
	std::vector<render_pixel> pixels;
	pixel_surface surface = test::make_surface(pixels);

	CHECK(output.render(extended_mode::NONE, w64_control, w64, surface).status == extended_status::NO_MODE);
	CHECK(output.render(extended_mode::VG6, w64_control, w64, surface).status == extended_status::MODE_INACTIVE);

	pixel_surface invalid_surface = surface;
	invalid_surface.pixels = nullptr;
	CHECK(output.render(extended_mode::W64, w64_control, w64, invalid_surface).status ==
			extended_status::INVALID_SURFACE);
	invalid_surface = surface;
	invalid_surface.width = RENDER_WIDTH - 1;
	CHECK(output.render(extended_mode::W64, w64_control, w64, invalid_surface).status ==
			extended_status::INVALID_SURFACE);
	invalid_surface = surface;
	invalid_surface.height = RENDER_HEIGHT - 1;
	CHECK(output.render(extended_mode::W64, w64_control, w64, invalid_surface).status ==
			extended_status::INVALID_SURFACE);
	invalid_surface = surface;
	invalid_surface.row_stride = RENDER_WIDTH - 1;
	CHECK(output.render(extended_mode::W64, w64_control, w64, invalid_surface).status ==
			extended_status::INVALID_SURFACE);
	CHECK(output.render_scanline(
				extended_mode::W64,
				w64_control,
				w64,
				RENDER_HEIGHT,
				{ pixels.data(), RENDER_WIDTH }).status == extended_status::INVALID_SURFACE);
	CHECK(output.render_scanline(
				extended_mode::W64,
				w64_control,
				w64,
				0,
				{ nullptr, RENDER_WIDTH }).status == extended_status::INVALID_SURFACE);
	CHECK(output.render_scanline(
				extended_mode::W64,
				w64_control,
				w64,
				0,
				{ pixels.data(), RENDER_WIDTH - 1 }).status == extended_status::INVALID_SURFACE);

	frame_buffer_state frame = w64;
	frame.valid = false;
	CHECK(output.render(extended_mode::W64, w64_control, frame, surface).status ==
			extended_status::FRAME_UNAVAILABLE);
	frame = w64;
	frame.complete = false;
	CHECK(output.render(extended_mode::W64, w64_control, frame, surface).status ==
			extended_status::FRAME_INCOMPLETE);
	frame = w64;
	frame.programming_suppressed = true;
	CHECK(output.render(extended_mode::W64, w64_control, frame, surface).status ==
			extended_status::FRAME_INCOMPLETE);
	frame = w64;
	frame.active_lines = HOST_ACTIVE_LINES - 1;
	CHECK(output.render(extended_mode::W64, w64_control, frame, surface).status ==
			extended_status::FRAME_INCOMPLETE);

	for (unsigned mutation = 0; mutation < 7; ++mutation)
	{
		frame = w64;
		switch (mutation)
		{
		case 0:
			--frame.writes;
			break;
		case 1:
			frame.first_address = 1;
			break;
		case 2:
			--frame.last_address;
			break;
		case 3:
			frame.mode_changes = 1;
			break;
		case 4:
			frame.first_mode = static_cast<std::uint8_t>(mode_kind::ALPHA_SEMIGRAPHICS);
			break;
		case 5:
			frame.last_mode = static_cast<std::uint8_t>(mode_kind::ALPHA_SEMIGRAPHICS);
			break;
		case 6:
			frame.cells[17].mode_id = static_cast<std::uint8_t>(mode_kind::ALPHA_SEMIGRAPHICS);
			break;
		}
		INFO("W64 inconsistent mutation " << mutation);
		CHECK(output.render(extended_mode::W64, w64_control, frame, surface).status ==
				extended_status::INCONSISTENT_CAPTURE);
	}

	core vg6_control;
	vg6_control.state().vg6_active = true;
	frame_buffer_state vg6 = test::make_vg6_frame(false);
	vg6.cells[CAPTURE_PAGE_BYTES - 1].mode_id = static_cast<std::uint8_t>(mode_kind::CG6);
	CHECK(output.render(extended_mode::VG6, vg6_control, vg6, surface).status ==
			extended_status::INCONSISTENT_CAPTURE);
}


TEST_CASE("CoCoVGA extended scanline results and status names are exact", "[cocovga][extended]")
{
	core control;
	control.state().vg6_active = true;
	frame_buffer_state frame = test::make_vg6_frame(true);
	std::array<render_pixel, RENDER_WIDTH> line{};
	extended_renderer output = cocovga::test::make_extended_renderer();
	extended_result const result = output.render_scanline(
			extended_mode::VG6,
			control,
			frame,
			VIEWPORT_TOP,
			{ line.data(), line.size() });
	REQUIRE(result.success());
	CHECK(result.mode == extended_mode::VG6);
	CHECK(result.generation == frame.generation);
	CHECK(result.scanlines_written == 1);
	CHECK(result.pixels_written == RENDER_WIDTH);

	frame.cells[VG6_BYTES_PER_ROW * 5] =
			test::make_cell(mode_kind::CG6, true, 0);
	CHECK(output.render_scanline(
				extended_mode::VG6,
				control,
				frame,
				VIEWPORT_TOP,
				{ line.data(), line.size() }).success());
	frame.cells[0] = test::make_cell(mode_kind::CG6, true, 0);
	CHECK(output.render_scanline(
				extended_mode::VG6,
				control,
				frame,
				VIEWPORT_TOP,
				{ line.data(), line.size() }).status == extended_status::INCONSISTENT_CAPTURE);
	frame = test::make_vg6_frame(true);
	frame.first_address = INVALID_CAPTURE_ADDRESS;
	CHECK(output.render_scanline(
				extended_mode::VG6,
				control,
				frame,
				0,
				{ line.data(), line.size() }).status == extended_status::INCONSISTENT_CAPTURE);
	frame = test::make_vg6_frame(true);
	frame.last_address = INVALID_CAPTURE_ADDRESS;
	CHECK(output.render_scanline(
				extended_mode::VG6,
				control,
				frame,
				RENDER_HEIGHT - 1,
				{ line.data(), line.size() }).status == extended_status::INCONSISTENT_CAPTURE);

	std::array<std::pair<extended_status, char const *>, 7> const names =
	{{
		{ extended_status::OK, "ok" },
		{ extended_status::NO_MODE, "no extended mode" },
		{ extended_status::MODE_INACTIVE, "extended mode inactive" },
		{ extended_status::INVALID_SURFACE, "invalid surface" },
		{ extended_status::FRAME_UNAVAILABLE, "frame unavailable" },
		{ extended_status::FRAME_INCOMPLETE, "frame incomplete" },
		{ extended_status::INCONSISTENT_CAPTURE, "inconsistent capture" }
	}};
	for (auto const &entry : names)
		CHECK(std::string(extended_renderer::status_name(entry.first)) == entry.second);
	CHECK(std::string(extended_renderer::status_name(static_cast<extended_status>(0xff))) == "unknown");
}
