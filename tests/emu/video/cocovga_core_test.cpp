// license:BSD-3-Clause
// copyright-holders:Stephen Spiller

#include "catch.hpp"

#include "cocovga_test_helpers.h"

#include <array>
#include <cstdint>

namespace
{

using namespace cocovga;

bool timing_equal(vga_timing const &left, vga_timing const &right)
{
	return vga_timing_equal(left, right);
}

button_events click_button(core &control, bool first, pixel_input const &input)
{
	control.clock_buttons(first, !first, input);
	return control.clock_buttons(false, false, input);
}

button_events hold_button(core &control, bool first, pixel_input const &input)
{
	control.clock_buttons(first, !first, input);
	std::uint32_t &hold_ticks = first
			? control.state().buttons.button_1_hold_ticks
			: control.state().buttons.button_2_hold_ticks;
	hold_ticks = BUTTON_HOLD_CYCLES - 1;
	return control.clock_buttons(first, !first, input);
}

} // anonymous namespace


TEST_CASE("CoCoVGA core reset applies exact board defaults", "[cocovga][core]")
{
	struct model_case
	{
		model board;
		std::uint8_t dac_bits;
		std::uint8_t font;
		std::uint8_t artifact;
		std::uint8_t extras;
		std::uint8_t enhanced;
	};

	constexpr std::array<model_case, 3> cases =
	{{
		{ model::AMC2, 3, 0x00, 0x0d, 0x40, 0x00 },
		{ model::MODERN, 5, 0x00, 0x0d, 0x40, 0x00 },
		{ model::T1, 5, 0x01, 0x0d, 0x41, 0x00 }
	}};

	for (model_case const &expected : cases)
	{
		INFO("model " << unsigned(expected.board));
		core control(expected.board);
		model_descriptor const &descriptor = core::describe_model(expected.board);
		core_state const &state = control.state();

		CHECK(descriptor.board == expected.board);
		CHECK(descriptor.dac_bits_per_component == expected.dac_bits);
		CHECK(descriptor.default_font == expected.font);
		CHECK(descriptor.default_artifact == expected.artifact);
		CHECK(descriptor.default_extras == expected.extras);
		CHECK(descriptor.default_enhanced_modes == expected.enhanced);

		CHECK(control.board_model() == expected.board);
		CHECK(state.registers.font == expected.font);
		CHECK(state.registers.artifact == expected.artifact);
		CHECK(state.registers.extras == expected.extras);
		CHECK(state.registers.enhanced_modes == expected.enhanced);
		CHECK(timing_equal(state.registers.timing, vga_timing{}));
		CHECK(state.reset_mask == 0);
		CHECK(state.edit_mask == 0);
		CHECK(state.selected_page == 0);
		CHECK_FALSE(state.page_selected);
		CHECK(control.selected_register_page() == register_page::UNKNOWN);
		CHECK(state.lock_state == combo_state::INIT);
		CHECK_FALSE(control.freeze());
		CHECK_FALSE(state.character_write_enabled);
		CHECK_FALSE(state.vg6_active);
		CHECK_FALSE(state.w64_active);

		for (std::uint16_t word : state.registers.semigraphics_palette_shadow)
			CHECK(word == 0);
		for (std::uint16_t word : state.registers.semigraphics_palette_visible)
			CHECK(word == 0);
		for (std::uint16_t word : state.registers.artifact_palette_shadow)
			CHECK(word == 0);
		for (std::uint16_t word : state.registers.artifact_palette_visible)
			CHECK(word == 0);
		for (std::uint16_t word : state.registers.extra_palette_shadow)
			CHECK(word == 0);
		for (std::uint16_t word : state.registers.extra_palette_visible)
			CHECK(word == 0);
		for (std::uint8_t value : state.character_ram)
			CHECK(value == 0);

		button_runtime const &buttons = state.buttons;
		CHECK(buttons.button_1_hold_ticks == 0);
		CHECK(buttons.button_2_hold_ticks == 0);
		CHECK_FALSE(buttons.button_1_down);
		CHECK_FALSE(buttons.button_2_down);
		CHECK(buttons.text_button_1_cycle == 0);
		CHECK(buttons.graphics_button_1_cycle == 0);
		CHECK(buttons.rg6_button_1_cycle == 0);
		CHECK(buttons.text_button_2_cycle == 0);
		CHECK(buttons.rg6_button_2_cycle == 0);
		CHECK_FALSE(buttons.force_lowercase_toggle);
		CHECK_FALSE(buttons.quiet_status_toggle);
		CHECK_FALSE(buttons.w64_toggle);

		font_controls const font = control.software_font();
		CHECK(font.t1_font == (expected.font & 0x01));
		CHECK_FALSE(font.force_lowercase);
		CHECK_FALSE(font.force_character_ram);
		artifact_controls const artifact = control.software_artifact();
		CHECK(artifact.color_enabled);
		CHECK_FALSE(artifact.swap);
		CHECK(artifact.mode == artifact_mode::MESS);
		extras_controls const extras = control.software_extras();
		CHECK(extras.border == (expected.board == model::T1 ? border_mode::T1 : border_mode::STANDARD));
		CHECK_FALSE(extras.scanlines);
		CHECK_FALSE(extras.quiet_status);
		CHECK_FALSE(extras.lowercase);
		CHECK_FALSE(extras.inverse_text);
		CHECK(extras.full_semigraphics_palette);
		CHECK_FALSE(extras.custom_text_palette);
	}

	CHECK(core::describe_model(static_cast<model>(0xff)).board == model::MODERN);
}


TEST_CASE("CoCoVGA core reset preserves uploaded character RAM only", "[cocovga][core]")
{
	core control;
	REQUIRE(control.write_character(0x000, 0x12));
	REQUIRE(control.write_character(0x5a5, 0x34));
	REQUIRE(control.write_character(CHARACTER_RAM_BYTES - 1, 0x56));
	control.state().buttons.text_button_1_cycle = 7;
	control.state().registers.font = 7;
	control.state().vg6_active = true;

	control.reset();
	CHECK(control.character_ram()[0x000] == 0x12);
	CHECK(control.character_ram()[0x5a5] == 0x34);
	CHECK(control.character_ram()[CHARACTER_RAM_BYTES - 1] == 0x56);
	CHECK_FALSE(control.character_write_enabled());
	CHECK(control.registers().font == 0);
	CHECK(control.buttons().text_button_1_cycle == 0);
	CHECK_FALSE(control.state().vg6_active);

	control.set_model(model::T1);
	CHECK(control.board_model() == model::T1);
	CHECK(control.character_ram()[0x000] == 0x12);
	CHECK(control.character_ram()[0x5a5] == 0x34);
	CHECK(control.character_ram()[CHARACTER_RAM_BYTES - 1] == 0x56);
	CHECK(control.registers().font == 1);
	CHECK(control.registers().extras == 0x41);
}


TEST_CASE("CoCoVGA mode descriptors and native decoding are exact", "[cocovga][core][modes]")
{
	struct mode_case
	{
		mode_kind mode;
		mode_family family;
		std::uint16_t width;
		std::uint16_t height;
		std::uint16_t bytes;
		std::uint8_t bits;
		std::uint8_t captured_bits;
		std::uint8_t bytes_per_row;
		capture_cycle capture;
		std::uint8_t scale_x;
		std::uint8_t scale_y;
		std::uint8_t columns;
		std::uint8_t rows;
		std::uint8_t glyph_width;
		std::uint8_t glyph_height;
		bool artifact;
		std::uint8_t palette_entries;
	};

	constexpr std::array<mode_case, 11> cases =
	{{
		{ mode_kind::ALPHA_SEMIGRAPHICS, mode_family::ALPHA_SEMIGRAPHICS, 256, 192, 512, 0, 6, 32, capture_cycle::ALPHA, 2, 2, 32, 16, 8, 12, false, 0 },
		{ mode_kind::CG1, mode_family::COLOR_GRAPHICS, 64, 64, 1024, 2, 8, 16, capture_cycle::LONG_CYCLE, 8, 6, 0, 0, 0, 0, false, 4 },
		{ mode_kind::RG1, mode_family::RESOLUTION_GRAPHICS, 128, 64, 1024, 1, 8, 16, capture_cycle::LONG_CYCLE, 4, 6, 0, 0, 0, 0, false, 2 },
		{ mode_kind::CG2, mode_family::COLOR_GRAPHICS, 128, 64, 2048, 2, 8, 32, capture_cycle::SHORT_CYCLE, 4, 6, 0, 0, 0, 0, false, 4 },
		{ mode_kind::RG2, mode_family::RESOLUTION_GRAPHICS, 128, 96, 1536, 1, 8, 16, capture_cycle::LONG_CYCLE, 4, 4, 0, 0, 0, 0, false, 2 },
		{ mode_kind::CG3, mode_family::COLOR_GRAPHICS, 128, 96, 3072, 2, 8, 32, capture_cycle::SHORT_CYCLE, 4, 4, 0, 0, 0, 0, false, 4 },
		{ mode_kind::RG3, mode_family::RESOLUTION_GRAPHICS, 128, 192, 3072, 1, 8, 16, capture_cycle::LONG_CYCLE, 4, 2, 0, 0, 0, 0, false, 2 },
		{ mode_kind::CG6, mode_family::COLOR_GRAPHICS, 128, 192, 6144, 2, 8, 32, capture_cycle::SHORT_CYCLE, 4, 2, 0, 0, 0, 0, false, 4 },
		{ mode_kind::RG6, mode_family::RESOLUTION_GRAPHICS, 256, 192, 6144, 1, 8, 32, capture_cycle::SHORT_CYCLE, 2, 2, 0, 0, 0, 0, true, 2 },
		{ mode_kind::VG6, mode_family::ENHANCED_GRAPHICS, 128, 96, 6144, 4, 8, 64, capture_cycle::SHORT_CYCLE, 4, 4, 0, 0, 0, 0, false, 16 },
		{ mode_kind::W64, mode_family::ENHANCED_TEXT, 512, 384, 2048, 0, 6, 64, capture_cycle::SHORT_CYCLE, 1, 1, 64, 32, 8, 12, false, 0 }
	}};

	for (mode_case const &expected : cases)
	{
		INFO("mode " << unsigned(expected.mode));
		mode_descriptor const &descriptor = core::describe_mode(expected.mode);
		CHECK(descriptor.mode == expected.mode);
		CHECK(descriptor.family == expected.family);
		CHECK(descriptor.source_width == expected.width);
		CHECK(descriptor.source_height == expected.height);
		CHECK(descriptor.logical_bytes == expected.bytes);
		CHECK(descriptor.bits_per_pixel == expected.bits);
		CHECK(descriptor.captured_data_bits == expected.captured_bits);
		CHECK(descriptor.bytes_per_row == expected.bytes_per_row);
		CHECK(descriptor.capture == expected.capture);
		CHECK(descriptor.scale_x == expected.scale_x);
		CHECK(descriptor.scale_y == expected.scale_y);
		CHECK(descriptor.text_columns == expected.columns);
		CHECK(descriptor.text_rows == expected.rows);
		CHECK(descriptor.glyph_width == expected.glyph_width);
		CHECK(descriptor.glyph_height == expected.glyph_height);
		CHECK(descriptor.artifact_capable == expected.artifact);
		CHECK(descriptor.palette_entries == expected.palette_entries);
	}

	CHECK(core::describe_mode(static_cast<mode_kind>(0xff)).mode == mode_kind::ALPHA_SEMIGRAPHICS);

	for (unsigned graphics = 0; graphics < 2; ++graphics)
	{
		for (unsigned gm = 0; gm < 8; ++gm)
		{
			pixel_input input;
			input.graphics = graphics != 0;
			input.gm = std::uint8_t(gm | 0xf8);
			mode_kind const expected = graphics
					? static_cast<mode_kind>(unsigned(mode_kind::CG1) + gm)
					: mode_kind::ALPHA_SEMIGRAPHICS;
			CHECK(core::decode_standard_mode(input) == expected);
		}
	}

	for (bool css : { false, true })
	{
		for (unsigned pixel = 0; pixel < 16; ++pixel)
		{
			CHECK(core::color_graphics_palette_index(css, std::uint8_t(pixel)) ==
					std::uint8_t((css ? 5 : 1) + (pixel & 3)));
			CHECK(core::vg6_palette_index(css, std::uint8_t(pixel)) ==
					std::uint8_t((css ? 16 : 0) + pixel));
		}
		CHECK(core::resolution_graphics_palette_index(css, false) == (css ? 0 : 16));
		CHECK(core::resolution_graphics_palette_index(css, true) == (css ? 15 : 31));
	}
}

TEST_CASE("CoCoVGA semigraphics glyph colors follow public MC6847 palette order", "[cocovga][core][palette][oracle]")
{
	// Public source: http://cocovga.com/documentation/software-mode-control/
	// Public CoCoVGA palette slots are black, green, yellow, blue, red, buff,
	// cyan, magenta, orange.  MC6847 SG color codes select the latter eight.
	constexpr std::array<std::uint8_t, 8> sg4_slots = { 1, 2, 3, 4, 5, 6, 7, 8 };
	constexpr std::array<std::array<std::uint8_t, 4>, 2> sg6_slots =
	{{
		{ 1, 2, 3, 4 },
		{ 5, 6, 7, 8 }
	}};

	for (bool css : { false, true })
	{
		for (std::size_t code = 0; code < sg4_slots.size(); ++code)
		{
			INFO("SG4 css " << css << " color code " << code);
			CHECK(core::semigraphics_glyph_palette_index(
						semigraphics_glyph::SG4,
						css,
						std::uint8_t(code)) == sg4_slots[code]);
		}
		for (std::size_t code = 0; code < sg6_slots[css].size(); ++code)
		{
			INFO("SG6 css " << css << " color code " << code);
			CHECK(core::semigraphics_glyph_palette_index(
						semigraphics_glyph::SG6,
						css,
						std::uint8_t(code)) == sg6_slots[css][code]);
		}
	}
}


TEST_CASE("CoCoVGA palette words convert exactly for 9-bit and 15-bit DACs", "[cocovga][core][palette]")
{
	struct color_case
	{
		rgb_color color;
		std::uint16_t rgb15;
		std::uint16_t rgb9;
	};

	constexpr std::array<color_case, 5> cases =
	{{
		{ { 0, 0, 0 }, 0x0000, 0x000 },
		{ { 31, 31, 31 }, 0x7fff, 0x1ff },
		{ { 31, 18, 5 }, 0x7e45, 0x1e1 },
		{ { 4, 8, 12 }, 0x110c, 0x053 },
		{ { 0xff, 0xfe, 0xfd }, 0x7fdd, 0x1ff }
	}};

	for (color_case const &expected : cases)
	{
		CHECK(core::pack_rgb15(expected.color) == expected.rgb15);
		CHECK(core::pack_rgb9(expected.color) == expected.rgb9);
	}

	decoded_palette_word const disabled = core::decode_palette_word(0x7e45);
	CHECK_FALSE(disabled.enabled);
	CHECK(disabled.rgb15 == 0x7e45);
	CHECK(disabled.rgb9 == 0x1e1);
	CHECK(disabled.color.red == 31);
	CHECK(disabled.color.green == 18);
	CHECK(disabled.color.blue == 5);

	decoded_palette_word const enabled = core::decode_palette_word(0xfe45);
	CHECK(enabled.enabled);
	CHECK(enabled.rgb15 == disabled.rgb15);
	CHECK(enabled.rgb9 == disabled.rgb9);
	CHECK(enabled.color.red == disabled.color.red);
	CHECK(enabled.color.green == disabled.color.green);
	CHECK(enabled.color.blue == disabled.color.blue);

	for (model board : { model::AMC2, model::MODERN, model::T1 })
	{
		core control(board);
		REQUIRE(test::write_register(control, register_bank::SEMIGRAPHICS_PALETTE, 32, 0xfe));
		REQUIRE(control.write_page00(33, 0x45));

		palette_output before = control.semigraphics_color(0);
		CHECK_FALSE(before.overridden);
		REQUIRE(control.commit_palettes_at_vsync());

		palette_output const after = control.semigraphics_color(0);
		CHECK(after.overridden);
		CHECK(after.color.red == 31);
		CHECK(after.color.green == 18);
		CHECK(after.color.blue == 5);
		CHECK(after.rgb15 == 0x7e45);
		CHECK(after.rgb9 == 0x1e1);
		CHECK(after.bits_per_component == (board == model::AMC2 ? 3 : 5));
		CHECK(after.packed == (board == model::AMC2 ? 0x1e1 : 0x7e45));
	}
}


TEST_CASE("CoCoVGA built-in palettes cover every CSS and board model", "[cocovga][core][palette]")
{
	constexpr std::array<std::uint16_t, SEMIGRAPHICS_PALETTE_ENTRIES> semigraphics =
	{
		0x0000, 0x03e0, 0x7fe8, 0x0018, 0x5000, 0x7fff, 0x03f8, 0x6094, 0x7e00, 0x0100, 0x2080
	};
	constexpr std::array<std::uint16_t, ARTIFACT_PALETTE_ENTRIES> artifact =
	{
		0x0000, 0x1004, 0x0080, 0x731c, 0x6398, 0x0094, 0x3080, 0x321c,
		0x7308, 0x7108, 0x021c, 0x639c, 0x738c, 0x0004, 0x1000, 0x7fff,
		0x0100, 0x0100, 0x0100, 0x0300, 0x0380, 0x0100, 0x0100, 0x0200,
		0x0280, 0x0300, 0x0200, 0x0380, 0x0380, 0x0100, 0x0100, 0x03e0
	};
	constexpr std::array<std::uint16_t, EXTRA_PALETTE_ENTRIES> extra =
	{
		0x0000, 0x03e0, 0x0000, 0x0000
	};

	for (model board : { model::AMC2, model::MODERN, model::T1 })
	{
		core control(board);
		for (std::size_t slot = 0; slot < semigraphics.size(); ++slot)
		{
			palette_output const color = control.semigraphics_color(slot);
			INFO("model " << unsigned(board) << " semigraphics " << slot);
			CHECK_FALSE(color.overridden);
			CHECK(color.rgb15 == semigraphics[slot]);
			CHECK(color.rgb9 == core::pack_rgb9(core::decode_palette_word(semigraphics[slot]).color));
			CHECK(color.bits_per_component == (board == model::AMC2 ? 3 : 5));
		}
		for (std::size_t slot = 0; slot < artifact.size(); ++slot)
		{
			palette_output const color = control.artifact_color(slot);
			INFO("model " << unsigned(board) << " artifact " << slot);
			CHECK_FALSE(color.overridden);
			CHECK(color.rgb15 == artifact[slot]);
		}
		for (std::size_t slot = 0; slot < extra.size(); ++slot)
		{
			palette_output const color = control.extra_color(static_cast<extra_palette_slot>(slot));
			INFO("model " << unsigned(board) << " extra " << slot);
			CHECK_FALSE(color.overridden);
			CHECK(color.rgb15 == extra[slot]);
		}

		CHECK(control.semigraphics_color(SEMIGRAPHICS_PALETTE_ENTRIES).rgb15 == 0);
		CHECK(control.artifact_color(ARTIFACT_PALETTE_ENTRIES).rgb15 == 0);
		CHECK(control.extra_color(static_cast<extra_palette_slot>(0xff)).rgb15 == 0);
	}

	for (unsigned slot = 0; slot < 16; ++slot)
	{
		CHECK(core::semigraphics_palette_index(std::uint8_t(slot), true) ==
				(slot < SEMIGRAPHICS_PALETTE_ENTRIES ? slot : 0));
		CHECK(core::semigraphics_palette_index(std::uint8_t(slot), false) ==
				(slot < 9 ? slot : 0));
	}
}


TEST_CASE("CoCoVGA Page 00 address translation is exhaustive and reversible", "[cocovga][core][registers]")
{
	std::array<bool, CAPTURE_PAGE_BYTES> used{};
	for (std::uint16_t logical = 0; logical < PAGE00_REGISTER_BYTES; ++logical)
	{
		std::uint16_t const internal = core::page00_logical_to_internal(logical);
		INFO("logical " << logical);
		REQUIRE(internal < CAPTURE_PAGE_BYTES);
		CHECK_FALSE(used[internal]);
		used[internal] = true;
		std::uint16_t round_trip = INVALID_CAPTURE_ADDRESS;
		REQUIRE(core::page00_internal_to_logical(internal, round_trip));
		CHECK(round_trip == logical);
	}

	CHECK(core::page00_logical_to_internal(PAGE00_REGISTER_BYTES) == INVALID_CAPTURE_ADDRESS);
	CHECK(core::page00_logical_to_internal(0xffff) == INVALID_CAPTURE_ADDRESS);

	unsigned valid_internal = 0;
	for (std::uint16_t internal = 0; internal < CAPTURE_PAGE_BYTES; ++internal)
	{
		std::uint16_t logical = INVALID_CAPTURE_ADDRESS;
		bool const valid = core::page00_internal_to_logical(internal, logical);
		INFO("internal " << internal);
		CHECK(valid == used[internal]);
		if (valid)
		{
			++valid_internal;
			CHECK(logical < PAGE00_REGISTER_BYTES);
			CHECK(core::page00_logical_to_internal(logical) == internal);
		}
	}
	CHECK(valid_internal == PAGE00_REGISTER_BYTES);
}


TEST_CASE("CoCoVGA Page 00 writes honor edit masks, reset masks, and field widths", "[cocovga][core][registers]")
{
	core control;

	CHECK_FALSE(control.write_page00(3, 0xff));
	CHECK_FALSE(control.write_page00(32, 0xff));
	CHECK_FALSE(control.write_page00(480, 0xff));
	REQUIRE(control.write_page00(0, 0xa5));
	CHECK(control.reset_mask() == 0xa5);
	REQUIRE(control.write_page00(1, 0xff));
	CHECK(control.edit_mask() == 0xff);

	REQUIRE(control.write_page00(3, 0xff));
	REQUIRE(control.write_page00(4, 0xff));
	REQUIRE(control.write_page00(5, 0xa5));
	REQUIRE(control.write_page00(8, 0xff));
	CHECK(control.registers().font == 0x07);
	CHECK(control.registers().artifact == 0x0f);
	CHECK(control.registers().extras == 0xa5);
	CHECK(control.registers().enhanced_modes == 0x07);

	for (std::uint16_t offset = 32; offset <= 53; ++offset)
		REQUIRE(control.write_page00(offset, std::uint8_t(offset ^ 0xa5)));
	for (std::uint16_t offset = 64; offset <= 127; ++offset)
		REQUIRE(control.write_page00(offset, std::uint8_t(offset ^ 0x5a)));
	for (std::uint16_t offset = 128; offset <= 135; ++offset)
		REQUIRE(control.write_page00(offset, std::uint8_t(offset + 3)));

	for (std::size_t slot = 0; slot < SEMIGRAPHICS_PALETTE_ENTRIES; ++slot)
	{
		std::uint16_t const offset = std::uint16_t(32 + slot * 2);
		std::uint16_t const expected = std::uint16_t(
				(std::uint16_t(std::uint8_t(offset ^ 0xa5)) << 8) |
				std::uint8_t((offset + 1) ^ 0xa5));
		CHECK(control.registers().semigraphics_palette_shadow[slot] == expected);
	}
	for (std::size_t slot = 0; slot < ARTIFACT_PALETTE_ENTRIES; ++slot)
	{
		std::uint16_t const offset = std::uint16_t(64 + slot * 2);
		std::uint16_t const expected = std::uint16_t(
				(std::uint16_t(std::uint8_t(offset ^ 0x5a)) << 8) |
				std::uint8_t((offset + 1) ^ 0x5a));
		CHECK(control.registers().artifact_palette_shadow[slot] == expected);
	}

	REQUIRE(control.write_page00(480, 0xff));
	REQUIRE(control.write_page00(481, 0x34));
	REQUIRE(control.write_page00(482, 8));
	REQUIRE(control.write_page00(483, 99));
	REQUIRE(control.write_page00(484, 56));
	REQUIRE(control.write_page00(486, 0xff));
	REQUIRE(control.write_page00(487, 0x67));
	REQUIRE(control.write_page00(488, 25));
	REQUIRE(control.write_page00(489, 3));
	REQUIRE(control.write_page00(490, 20));
	CHECK(control.registers().timing.h_active == 0x734);
	CHECK(control.registers().timing.h_front_porch == 8);
	CHECK(control.registers().timing.h_sync_width == 99);
	CHECK(control.registers().timing.h_back_porch == 56);
	CHECK(control.registers().timing.v_active == 0x367);
	CHECK(control.registers().timing.v_front_porch == 25);
	CHECK(control.registers().timing.v_sync_width == 3);
	CHECK(control.registers().timing.v_back_porch == 20);

	CHECK_FALSE(control.write_page00(2, 0));
	CHECK_FALSE(control.write_page00(6, 0));
	CHECK_FALSE(control.write_page00(54, 0));
	CHECK_FALSE(control.write_page00(136, 0));
	CHECK_FALSE(control.write_page00(479, 0));
	CHECK_FALSE(control.write_page00(485, 0));
	CHECK_FALSE(control.write_page00(491, 0));
	CHECK_FALSE(control.write_page00(PAGE00_REGISTER_BYTES, 0));

	REQUIRE(control.write_page00_internal(core::page00_logical_to_internal(3), 0x05));
	CHECK(control.registers().font == 0x05);
	CHECK_FALSE(control.write_page00_internal(0, 0xff));
}


TEST_CASE("CoCoVGA palette reset and VSYNC commit keep shadow and visible banks distinct", "[cocovga][core][palette][registers]")
{
	core control;
	REQUIRE(test::write_register(control, register_bank::SEMIGRAPHICS_PALETTE, 32, 0x92));
	REQUIRE(control.write_page00(33, 0x34));
	REQUIRE(test::write_register(control, register_bank::ARTIFACT_PALETTE, 64, 0xd6));
	REQUIRE(control.write_page00(65, 0x78));
	REQUIRE(test::write_register(control, register_bank::EXTRA_PALETTE, 128, 0xba));
	REQUIRE(control.write_page00(129, 0xbc));

	CHECK(control.registers().semigraphics_palette_visible[0] == 0);
	CHECK(control.registers().artifact_palette_visible[0] == 0);
	CHECK(control.registers().extra_palette_visible[0] == 0);
	REQUIRE(control.commit_palettes_at_vsync());
	CHECK(control.registers().semigraphics_palette_visible[0] == 0x9234);
	CHECK(control.registers().artifact_palette_visible[0] == 0xd678);
	CHECK(control.registers().extra_palette_visible[0] == 0xbabc);

	REQUIRE(control.write_page00(
			0,
			core::bank_bit(register_bank::SEMIGRAPHICS_PALETTE) |
					core::bank_bit(register_bank::ARTIFACT_PALETTE) |
					core::bank_bit(register_bank::EXTRA_PALETTE)));
	REQUIRE(control.write_page00(1, 0));
	CHECK(control.registers().semigraphics_palette_shadow[0] == 0);
	CHECK(control.registers().artifact_palette_shadow[0] == 0);
	CHECK(control.registers().extra_palette_shadow[0] == 0);
	CHECK(control.registers().semigraphics_palette_visible[0] == 0x9234);
	CHECK(control.registers().artifact_palette_visible[0] == 0xd678);
	CHECK(control.registers().extra_palette_visible[0] == 0xbabc);

	REQUIRE(control.commit_palettes_at_vsync());
	CHECK(control.registers().semigraphics_palette_visible[0] == 0);
	CHECK(control.registers().artifact_palette_visible[0] == 0);
	CHECK(control.registers().extra_palette_visible[0] == 0);

	REQUIRE(test::unlock_page(control, register_page::PAGE_00).freeze);
	CHECK_FALSE(control.commit_palettes_at_vsync());
	control.clock_combo(true, 0);
	control.clock_combo(false, 0);
	REQUIRE(control.commit_palettes_at_vsync());
}


TEST_CASE("CoCoVGA combination lock covers positive, negative, and no-timeout paths", "[cocovga][core][lock]")
{
	core control;
	CHECK(control.lock_state() == combo_state::INIT);

	combo_step step = control.clock_combo(true, core::COMBO_1);
	CHECK(step.current == combo_state::INIT);
	step = control.clock_combo(false, 0);
	CHECK(step.current == combo_state::READY);
	step = control.clock_combo(false, 0x1e);
	CHECK(step.current == combo_state::READY);
	step = control.clock_combo(false, core::COMBO_1);
	CHECK(step.current == combo_state::UNLOCKED_1);

	for (unsigned delay = 0; delay < 1'000; ++delay)
	{
		step = control.clock_combo(false, 0);
		CHECK(step.current == combo_state::UNLOCKED_1);
	}

	CHECK(control.clock_combo(false, core::COMBO_2).current == combo_state::UNLOCKED_2);
	CHECK(control.clock_combo(false, core::COMBO_3).current == combo_state::UNLOCKED_3);
	CHECK(control.clock_combo(false, core::COMBO_4).current == combo_state::WAIT_MODE_1);
	for (unsigned delay = 0; delay < 1'000; ++delay)
		CHECK(control.clock_combo(false, core::COMBO_4).current == combo_state::WAIT_MODE_1);
	CHECK(control.clock_combo(false, 0).current == combo_state::WAIT_MODE_2);
	CHECK(control.clock_combo(false, 0x1f).current == combo_state::WAIT_MODE_3);
	step = control.clock_combo(false, static_cast<std::uint8_t>(register_page::PAGE_18));
	CHECK(step.page_captured);
	CHECK(step.captured_selector == static_cast<std::uint8_t>(register_page::PAGE_18));
	CHECK(step.current == combo_state::UNLOCKED_A);
	CHECK(step.freeze);
	CHECK(control.freeze());
	CHECK(control.page_selected());
	CHECK(control.selected_register_page() == register_page::PAGE_18);

	step = control.clock_combo(false, 0);
	CHECK(step.current == combo_state::UNLOCKED_A);
	step = control.clock_combo(true, 0);
	CHECK(step.current == combo_state::UNLOCKED_B);
	CHECK(step.freeze);
	step = control.clock_combo(true, 0);
	CHECK(step.current == combo_state::UNLOCKED_B);
	step = control.clock_combo(false, 0);
	CHECK(step.current == combo_state::READY);
	CHECK(step.programming_released);
	CHECK_FALSE(step.freeze);

	control.reset();
	control.clock_combo(false, 0);
	control.clock_combo(false, core::COMBO_1);
	CHECK(control.clock_combo(true, core::COMBO_2).current == combo_state::INIT);
	control.clock_combo(false, 0);
	control.clock_combo(false, core::COMBO_1);
	CHECK(control.clock_combo(false, 0, true).current == combo_state::RESET);
	CHECK(control.clock_combo(false, 0, false, false).current == combo_state::RESET);
	CHECK(control.clock_combo(false, 0).current == combo_state::INIT);
}


TEST_CASE("CoCoVGA combination lock restores at every partial sequence step", "[cocovga][core][lock][restore]")
{
	constexpr std::array<std::uint8_t, 8> sequence =
	{{
		0,
		core::COMBO_1,
		core::COMBO_2,
		core::COMBO_3,
		core::COMBO_4,
		0,
		0,
		static_cast<std::uint8_t>(register_page::PAGE_18)
	}};

	for (std::size_t split = 1; split < sequence.size(); ++split)
	{
		core uninterrupted;
		for (std::size_t index = 0; index < split; ++index)
			uninterrupted.clock_combo(false, sequence[index]);

		core restored;
		restored.restore_state(uninterrupted.state());
		CHECK(restored.lock_state() == uninterrupted.lock_state());
		CHECK(restored.freeze() == uninterrupted.freeze());
		CHECK(restored.page_selected() == uninterrupted.page_selected());

		for (std::size_t index = split; index < sequence.size(); ++index)
		{
			combo_step const expected = uninterrupted.clock_combo(false, sequence[index]);
			combo_step const actual = restored.clock_combo(false, sequence[index]);
			INFO("split " << split << ", sequence index " << index);
			CHECK(actual.previous == expected.previous);
			CHECK(actual.current == expected.current);
			CHECK(actual.captured_selector == expected.captured_selector);
			CHECK(actual.page_captured == expected.page_captured);
			CHECK(actual.programming_released == expected.programming_released);
			CHECK(actual.freeze == expected.freeze);
		}

		CHECK(restored.lock_state() == combo_state::UNLOCKED_A);
		CHECK(restored.freeze());
		CHECK(restored.page_selected());
		CHECK(restored.selected_register_page() == register_page::PAGE_18);
	}
}


TEST_CASE("CoCoVGA Page 00 and Page 18 selected capture writes are exact", "[cocovga][core][lock][registers]")
{
	core page00;
	REQUIRE(test::unlock_page(page00, register_page::PAGE_00).freeze);
	REQUIRE(page00.ingest_selected_capture(core::page00_logical_to_internal(0), 0));
	REQUIRE(page00.ingest_selected_capture(
			core::page00_logical_to_internal(1),
			core::bank_bit(register_bank::FONT)));
	REQUIRE(page00.ingest_selected_capture(core::page00_logical_to_internal(3), 0x07));
	CHECK(page00.registers().font == 0x07);
	CHECK_FALSE(page00.ingest_selected_capture(0, 0xff));
	page00.clock_combo(true, 0);
	page00.clock_combo(false, 0);
	CHECK_FALSE(page00.ingest_selected_capture(core::page00_logical_to_internal(3), 0));
	CHECK(page00.reset_mask() == 0);
	CHECK(page00.edit_mask() == 0);

	core page18;
	REQUIRE(test::unlock_page(page18, register_page::PAGE_18).freeze);
	for (std::uint16_t capture = 0; capture < CHARACTER_RAM_BYTES * 2; ++capture)
	{
		std::uint8_t const value = std::uint8_t(capture * 17U + (capture >> 5));
		INFO("capture address " << capture);
		REQUIRE(page18.ingest_selected_capture(capture, value));
		CHECK(page18.character_ram()[core::character_capture_to_address(capture)] == value);
	}
	CHECK_FALSE(page18.ingest_selected_capture(CHARACTER_RAM_BYTES * 2, 0));
	CHECK(page18.character_write_enabled());
	page18.end_programming_frame();
	CHECK_FALSE(page18.character_write_enabled());

	for (std::uint16_t address = 0; address < CHARACTER_RAM_BYTES; ++address)
	{
		std::uint16_t const second_capture = std::uint16_t(
				((address >> 5) << 6) | 0x20 | (address & 0x1f));
		std::uint8_t const expected = std::uint8_t(second_capture * 17U + (second_capture >> 5));
		CHECK(page18.character_ram()[address] == expected);
	}

	for (unsigned character = 0; character < 256; ++character)
	{
		for (unsigned row = 0; row < 12; ++row)
			CHECK(core::character_address(std::uint8_t(character), std::uint8_t(row)) == character * 12 + row);
		CHECK(core::character_address(std::uint8_t(character), 12) == INVALID_CAPTURE_ADDRESS);
	}
	CHECK(core::character_capture_to_address(CHARACTER_RAM_BYTES * 2) == INVALID_CAPTURE_ADDRESS);
}


TEST_CASE("CoCoVGA alpha capture and PIA selectors preserve every input bit", "[cocovga][core]")
{
	for (unsigned data = 0; data < 256; ++data)
	{
		for (bool semigraphics : { false, true })
		{
			for (bool inverse : { false, true })
			{
				std::uint8_t const packed = core::pack_alpha_capture_byte(
						semigraphics,
						inverse,
						std::uint8_t(data));
				pixel_input const decoded = core::decode_alpha_capture_byte(packed, true, true);
				CHECK(decoded.data == (data & 0x3f));
				CHECK(decoded.alpha_semigraphics == semigraphics);
				CHECK(decoded.inverse == inverse);
				CHECK(decoded.css);
				CHECK(decoded.internal_external);
				CHECK_FALSE(decoded.graphics);
			}
		}
	}

	for (unsigned pins = 0; pins < 32; ++pins)
	{
		std::uint8_t const expected = std::uint8_t(
				((pins & 0x01) << 4) |
				((pins & 0x10) >> 1) |
				((pins >> 1) & 0x07));
		CHECK(core::selector_from_pia_bits(std::uint8_t(pins)) == expected);
		for (unsigned low = 0; low < 8; ++low)
			CHECK(core::selector_from_pia_register(std::uint8_t((pins << 3) | low)) == expected);
	}
}


TEST_CASE("CoCoVGA buttons implement every context, press, hold, and reset path", "[cocovga][core][buttons][oracle]")
{
	core control;
	pixel_input const text = test::input_for_mode(mode_kind::ALPHA_SEMIGRAPHICS);
	pixel_input const graphics = test::input_for_mode(mode_kind::CG2);
	pixel_input const rg6 = test::input_for_mode(mode_kind::RG6);
	pixel_input const w64 = test::input_for_mode(mode_kind::W64);

	for (unsigned cycle = 1; cycle <= 8; ++cycle)
	{
		button_events const events = click_button(control, true, text);
		CHECK(events.button_1_press);
		CHECK(control.buttons().text_button_1_cycle == (cycle & 7));
		hardware_controls const settings = control.hardware_settings(text);
		CHECK(settings.inverse_text == ((cycle & 1) != 0));
		CHECK(settings.reduced_palette == ((cycle & 2) != 0));
		CHECK(settings.scanlines == ((cycle & 4) != 0));
	}
	for (unsigned cycle = 1; cycle <= 8; ++cycle)
	{
		button_events const events = click_button(control, false, text);
		CHECK(events.button_2_press);
		CHECK(control.buttons().text_button_2_cycle == (cycle & 7));
		CHECK(control.hardware_settings(text).text_palette ==
				static_cast<text_palette_choice>(cycle & 7));
	}

	for (unsigned cycle = 1; cycle <= 4; ++cycle)
	{
		click_button(control, true, graphics);
		CHECK(control.buttons().graphics_button_1_cycle == (cycle & 3));
		CHECK(control.hardware_settings(graphics).disable_border == ((cycle & 1) != 0));
		CHECK(control.hardware_settings(graphics).scanlines == ((cycle & 2) != 0));
	}

	for (unsigned cycle = 1; cycle <= 8; ++cycle)
	{
		click_button(control, true, rg6);
		CHECK(control.buttons().rg6_button_1_cycle == (cycle & 7));
		hardware_controls const settings = control.hardware_settings(rg6);
		CHECK(settings.artifact_swap == ((cycle & 1) != 0));
		CHECK(settings.disable_border == ((cycle & 2) != 0));
		CHECK(settings.scanlines == ((cycle & 4) != 0));
	}

	// CoCoVGA User Manual, "Hardware Mode Control": RG6 Button 2 starts under
	// software control, then selects Standard, Fat Bits, Smart, MESS, and
	// Monochrome before wrapping to Standard.
	struct artifact_button_vector
	{
		std::uint8_t cycle;
		hardware_artifact_choice choice;
		artifact_mode mode;
		bool color_enabled;
	};
	constexpr std::array<artifact_button_vector, 7> artifact_vectors =
	{{
		{ 0, hardware_artifact_choice::SOFTWARE, artifact_mode::MESS, true },
		{ 1, hardware_artifact_choice::STANDARD, artifact_mode::STANDARD, true },
		{ 2, hardware_artifact_choice::FAT_BITS, artifact_mode::FAT_BITS, true },
		{ 3, hardware_artifact_choice::SMARTIFACT, artifact_mode::SMARTIFACT, true },
		{ 4, hardware_artifact_choice::MESS, artifact_mode::MESS, true },
		{ 5, hardware_artifact_choice::MONOCHROME, artifact_mode::MONOCHROME, false },
		{ 1, hardware_artifact_choice::STANDARD, artifact_mode::STANDARD, true }
	}};

	for (std::size_t step = 0; step < artifact_vectors.size(); ++step)
	{
		INFO("documented RG6 Button 2 step " << step);
		if (step != 0)
		{
			click_button(control, false, rg6);
			CHECK(control.registers().artifact == 0);
		}
		artifact_button_vector const &expected = artifact_vectors[step];
		CHECK(control.buttons().rg6_button_2_cycle == expected.cycle);
		CHECK(control.hardware_settings(rg6).artifact == expected.choice);
		effective_controls const settings = control.controls(rg6);
		CHECK(settings.artifact.mode == expected.mode);
		CHECK(settings.artifact.color_enabled == expected.color_enabled);
	}

	button_events events = hold_button(control, true, text);
	CHECK(events.button_1_hold);
	CHECK(control.buttons().force_lowercase_toggle);
	CHECK_FALSE(control.clock_buttons(false, false, text).button_1_press);
	events = hold_button(control, false, text);
	CHECK(events.button_2_hold);
	CHECK(control.buttons().quiet_status_toggle);
	CHECK_FALSE(control.clock_buttons(false, false, text).button_2_press);

	control.state().w64_active = true;
	events = hold_button(control, true, w64);
	CHECK(events.button_1_hold);
	CHECK(control.buttons().force_lowercase_toggle);
	CHECK(control.buttons().w64_toggle);
	control.clock_buttons(false, false, w64);

	control.state().w64_active = false;
	events = hold_button(control, true, graphics);
	CHECK(events.button_1_hold);
	CHECK_FALSE(control.buttons().w64_toggle);
	control.clock_buttons(false, false, graphics);

	control.state().buttons.force_lowercase_toggle = true;
	control.state().buttons.quiet_status_toggle = true;
	control.state().buttons.w64_toggle = true;
	control.state().buttons.text_button_1_cycle = 7;
	control.state().buttons.graphics_button_1_cycle = 3;
	control.state().buttons.rg6_button_1_cycle = 7;
	control.state().buttons.text_button_2_cycle = 7;
	control.state().buttons.rg6_button_2_cycle = 4;
	REQUIRE(control.write_page00(0, 0xff));
	REQUIRE(control.write_page00(1, 0));
	CHECK_FALSE(control.buttons().force_lowercase_toggle);
	CHECK_FALSE(control.buttons().quiet_status_toggle);
	CHECK_FALSE(control.buttons().w64_toggle);
	CHECK(control.buttons().text_button_1_cycle == 0);
	CHECK(control.buttons().graphics_button_1_cycle == 0);
	CHECK(control.buttons().rg6_button_1_cycle == 0);
	CHECK(control.buttons().text_button_2_cycle == 0);
	CHECK(control.buttons().rg6_button_2_cycle == 0);
}

TEST_CASE("CoCoVGA button releases are independent and W64 uses the graphics pin context", "[cocovga][core][buttons][oracle]")
{
	pixel_input const text = test::input_for_mode(mode_kind::ALPHA_SEMIGRAPHICS);
	pixel_input const w64 = test::input_for_mode(mode_kind::W64);

	core simultaneous;
	simultaneous.clock_buttons(true, true, text);
	button_events events = simultaneous.clock_buttons(false, false, text);
	CHECK(events.button_1_press);
	CHECK(events.button_2_press);
	CHECK(simultaneous.buttons().text_button_1_cycle == 1);
	CHECK(simultaneous.buttons().text_button_2_cycle == 1);

	core independent;
	independent.clock_buttons(true, true, text);
	events = independent.clock_buttons(false, true, text);
	CHECK(events.button_1_press);
	CHECK_FALSE(events.button_2_press);
	CHECK(independent.buttons().text_button_1_cycle == 1);
	CHECK(independent.buttons().text_button_2_cycle == 0);
	events = independent.clock_buttons(false, false, text);
	CHECK_FALSE(events.button_1_press);
	CHECK(events.button_2_press);
	CHECK(independent.buttons().text_button_2_cycle == 1);

	core held;
	held.clock_buttons(true, true, text);
	held.state().buttons.button_1_hold_ticks = BUTTON_HOLD_CYCLES - 1;
	held.state().buttons.button_2_hold_ticks = BUTTON_HOLD_CYCLES - 1;
	events = held.clock_buttons(true, true, text);
	CHECK(events.button_1_hold);
	CHECK_FALSE(events.button_2_hold);
	CHECK(held.buttons().button_1_hold_ticks == BUTTON_HOLD_CYCLES);
	CHECK(held.buttons().button_2_hold_ticks == BUTTON_HOLD_CYCLES);
	CHECK(held.buttons().force_lowercase_toggle);
	CHECK_FALSE(held.buttons().quiet_status_toggle);
	events = held.clock_buttons(false, false, text);
	CHECK_FALSE(events.button_1_press);
	CHECK_FALSE(events.button_2_press);

	core overlapping;
	overlapping.clock_buttons(true, false, text);
	overlapping.state().buttons.button_1_hold_ticks = BUTTON_HOLD_CYCLES - 1;
	events = overlapping.clock_buttons(true, true, text);
	CHECK(events.button_1_hold);
	CHECK_FALSE(events.button_2_hold);
	CHECK(overlapping.buttons().button_1_hold_ticks == BUTTON_HOLD_CYCLES);
	CHECK(overlapping.buttons().button_2_hold_ticks == 1);
	events = overlapping.clock_buttons(false, true, text);
	CHECK_FALSE(events.button_1_press);
	CHECK_FALSE(events.button_2_press);
	CHECK(overlapping.buttons().button_1_hold_ticks == 0);
	CHECK(overlapping.buttons().button_2_hold_ticks == 2);
	overlapping.state().buttons.button_2_hold_ticks = BUTTON_HOLD_CYCLES - 1;
	events = overlapping.clock_buttons(false, true, text);
	CHECK_FALSE(events.button_1_hold);
	CHECK(events.button_2_hold);
	events = overlapping.clock_buttons(false, false, text);
	CHECK_FALSE(events.button_1_press);
	CHECK_FALSE(events.button_2_press);

	core reverse_overlap;
	reverse_overlap.clock_buttons(false, true, text);
	reverse_overlap.state().buttons.button_2_hold_ticks = BUTTON_HOLD_CYCLES - 1;
	events = reverse_overlap.clock_buttons(true, true, text);
	CHECK_FALSE(events.button_1_hold);
	CHECK(events.button_2_hold);
	CHECK(reverse_overlap.buttons().button_1_hold_ticks == 1);
	CHECK(reverse_overlap.buttons().button_2_hold_ticks == BUTTON_HOLD_CYCLES);
	events = reverse_overlap.clock_buttons(true, false, text);
	CHECK_FALSE(events.button_1_press);
	CHECK_FALSE(events.button_2_press);
	reverse_overlap.state().buttons.button_1_hold_ticks = BUTTON_HOLD_CYCLES - 1;
	events = reverse_overlap.clock_buttons(true, false, text);
	CHECK(events.button_1_hold);
	CHECK_FALSE(events.button_2_hold);
	events = reverse_overlap.clock_buttons(false, false, text);
	CHECK_FALSE(events.button_1_press);
	CHECK_FALSE(events.button_2_press);

	core wide;
	wide.state().w64_active = true;
	events = click_button(wide, true, w64);
	CHECK(events.button_1_press);
	CHECK(wide.buttons().graphics_button_1_cycle == 1);
	CHECK(wide.buttons().text_button_1_cycle == 0);
	events = hold_button(wide, true, w64);
	CHECK(events.button_1_hold);
	CHECK(wide.buttons().w64_toggle);
	CHECK_FALSE(wide.buttons().force_lowercase_toggle);
}


TEST_CASE("CoCoVGA Page 00 artifact control remains effective before the first RG6 Button 2 press", "[cocovga][core][registers][buttons][artifact]")
{
	core control;
	pixel_input const rg6 = test::input_for_mode(mode_kind::RG6, false);

	REQUIRE(control.write_page00(1, core::bank_bit(register_bank::ARTIFACT)));
	CHECK(control.buttons().rg6_button_2_cycle == 0);
	CHECK(control.hardware_settings(rg6).artifact == hardware_artifact_choice::SOFTWARE);

	REQUIRE(control.write_page00(4, 0x00));
	CHECK(control.hardware_settings(rg6).artifact == hardware_artifact_choice::SOFTWARE);
	effective_controls settings = control.controls(rg6);
	CHECK_FALSE(settings.artifact.color_enabled);
	CHECK(settings.artifact.mode == artifact_mode::MONOCHROME);

	REQUIRE(control.write_page00(4, 0x01));
	CHECK(control.hardware_settings(rg6).artifact == hardware_artifact_choice::SOFTWARE);
	settings = control.controls(rg6);
	CHECK(settings.artifact.color_enabled);
	CHECK(settings.artifact.mode == artifact_mode::STANDARD);
}


TEST_CASE("CoCoVGA effective controls and enhanced mode latches combine software and buttons", "[cocovga][core][modes][buttons]")
{
	core control;
	pixel_input text = test::input_for_mode(mode_kind::ALPHA_SEMIGRAPHICS, true);
	pixel_input rg6 = test::input_for_mode(mode_kind::RG6, false);

	REQUIRE(test::write_register(control, register_bank::FONT, 3, 0x07));
	REQUIRE(test::write_register(control, register_bank::ARTIFACT, 4, 0x07));
	REQUIRE(test::write_register(control, register_bank::EXTRAS, 5, 0xff));
	REQUIRE(test::write_register(control, register_bank::ENHANCED_MODES, 8, 0x07));

	control.state().buttons.force_lowercase_toggle = true;
	control.state().buttons.quiet_status_toggle = true;
	control.state().buttons.text_button_1_cycle = 0x07;
	control.state().buttons.text_button_2_cycle =
			static_cast<std::uint8_t>(text_palette_choice::BLUE_ON_WHITE);

	effective_controls settings = control.controls(text);
	CHECK(settings.font.t1_font);
	CHECK_FALSE(settings.font.force_lowercase);
	CHECK(settings.font.force_character_ram);
	CHECK(settings.artifact.color_enabled);
	CHECK(settings.artifact.swap);
	CHECK(settings.artifact.mode == artifact_mode::FAT_BITS);
	CHECK(settings.extras.border == border_mode::CUSTOM);
	CHECK_FALSE(settings.extras.scanlines);
	CHECK_FALSE(settings.extras.quiet_status);
	CHECK(settings.extras.lowercase);
	CHECK_FALSE(settings.extras.inverse_text);
	CHECK_FALSE(settings.extras.full_semigraphics_palette);
	CHECK(settings.extras.custom_text_palette);
	CHECK(settings.enhanced.vg6);
	CHECK(settings.enhanced.w64);
	CHECK(settings.enhanced.character_ram);
	CHECK(settings.text_palette == text_palette_choice::BLUE_ON_WHITE);

	text_palette_descriptor palette = control.text_palette(text);
	CHECK(palette.foreground.source == palette_source::SEMIGRAPHICS);
	CHECK(palette.foreground.index == 3);
	CHECK(palette.foreground.use_default);
	CHECK(palette.background.index == 5);
	CHECK(palette.background.use_default);

	control.state().buttons.rg6_button_2_cycle =
			static_cast<std::uint8_t>(hardware_artifact_choice::MONOCHROME);
	settings = control.controls(rg6);
	CHECK_FALSE(settings.artifact.color_enabled);
	CHECK(settings.artifact.mode == artifact_mode::MONOCHROME);

	control.latch_previous_field_mode(true, 6);
	CHECK(control.state().vg6_active);
	CHECK_FALSE(control.state().w64_active);
	CHECK(control.display_mode(rg6) == mode_kind::VG6);
	CHECK(control.capture_mode(rg6) == mode_kind::VG6);

	control.latch_previous_field_mode(true, 2);
	CHECK_FALSE(control.state().vg6_active);
	CHECK(control.state().w64_active);
	CHECK(control.display_mode(text) == mode_kind::W64);
	CHECK(control.capture_mode(text) == mode_kind::W64);
	CHECK(control.use_character_ram(text));

	REQUIRE(test::unlock_page(control, register_page::PAGE_00).freeze);
	control.latch_previous_field_mode(true, 6);
	CHECK(control.state().w64_active);
	CHECK(control.display_mode(text) == mode_kind::W64);
	CHECK(control.capture_mode(text) == mode_kind::ALPHA_SEMIGRAPHICS);
}
