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
#include <vector>

namespace
{

using namespace cocovga;

void check_doubled_glyph(
		std::array<render_pixel, RENDER_WIDTH> const &line,
		unsigned cell,
		std::uint8_t bits,
		render_pixel foreground,
		render_pixel background)
{
	std::size_t const start = VIEWPORT_LEFT + cell * 16;
	for (unsigned bit = 0; bit < 8; ++bit)
	{
		render_pixel const expected = (bits & (0x80U >> bit)) ? foreground : background;
		CHECK(line[start + bit * 2] == expected);
		CHECK(line[start + bit * 2 + 1] == expected);
	}
}

std::uint8_t fpga_status_character_row(
		bool t1_font,
		std::uint8_t character,
		std::uint8_t row)
{
	return test::fpga_text_row(t1_font, character, row);
}

} // anonymous namespace


TEST_CASE("CoCoVGA renderer packs both DAC widths and names every status", "[cocovga][renderer]")
{
	core modern(model::MODERN);
	core amc2(model::AMC2);
	CHECK(renderer::pack_color(modern.semigraphics_color(0)) == 0xff000000U);
	CHECK(renderer::pack_color(modern.semigraphics_color(5)) == 0xffffffffU);
	CHECK(renderer::pack_color(amc2.semigraphics_color(5)) == 0xffffffffU);
	CHECK(renderer::pack_color(modern.semigraphics_color(4)) == 0xffa50000U);

	std::array<std::pair<render_status, char const *>, 5> const names =
	{{
		{ render_status::OK, "ok" },
		{ render_status::INVALID_SURFACE, "invalid surface" },
		{ render_status::FRAME_UNAVAILABLE, "frame unavailable" },
		{ render_status::FRAME_INCOMPLETE, "frame incomplete" },
		{ render_status::INCONSISTENT_CAPTURE, "inconsistent capture" }
	}};
	for (auto const &entry : names)
		CHECK(std::string(renderer::status_name(entry.first)) == entry.second);
	CHECK(std::string(renderer::status_name(static_cast<render_status>(0xff))) == "unknown");
}

TEST_CASE("CoCoVGA boot status reproduces every FPGA byte case, glyph row, color, and position", "[cocovga][renderer][status][oracle]")
{
	constexpr std::array<std::uint8_t, 32> top =
	{{
		3, 15, 3, 15, 22, 7, 1, 32, 18, 5, 22, 32, 48, 46, 57, 50,
		32, 23, 23, 23, 46, 3, 15, 3, 15, 22, 7, 1, 46, 3, 15, 13
	}};
	constexpr std::array<std::uint8_t, 32> bottom =
	{{
		40, 3, 41, 50, 48, 49, 49, 45, 50, 48, 32, 4, 15, 14, 1, 8,
		5, 47, 19, 14, 9, 4, 5, 18, 47, 19, 16, 9, 12, 12, 5, 18
	}};
	constexpr std::array<std::uint16_t, 2> first_rows =
		{ std::uint16_t(VIEWPORT_TOP - 24), VIEWPORT_BOTTOM_EXCLUSIVE };
	constexpr std::array<std::array<std::uint8_t, 32>, 2> banners = { top, bottom };

	for (model board : { model::MODERN, model::T1 })
	{
		core control(board);
		frame_buffer_state const frame =
				test::make_native_frame(mode_kind::ALPHA_SEMIGRAPHICS, false, 0);
		render_options options;
		options.border_status.kind = render_effects::border_status_kind::BOOT_VERSION;
		options.border_status.visible = true;

		palette_output status_palette;
		status_palette.bits_per_component =
				core::describe_model(control.board_model()).dac_bits_per_component;
		status_palette.color = { 0, 31, 24 };
		render_pixel const foreground = renderer::pack_color(status_palette);
		render_pixel const background = 0xff000000U;

		renderer output = cocovga::test::make_renderer();
		for (std::size_t banner = 0; banner < banners.size(); ++banner)
		{
			for (unsigned glyph_row = 0; glyph_row < W64_GLYPH_HEIGHT; ++glyph_row)
			{
				for (unsigned duplicate = 0; duplicate < 2; ++duplicate)
				{
					std::array<render_pixel, RENDER_WIDTH> line{};
					std::uint16_t const output_row = std::uint16_t(
							first_rows[banner] + glyph_row * 2 + duplicate);
					REQUIRE(output.render_scanline(
							control,
							frame,
							output_row,
							{ line.data(), line.size() },
							options).success());

					for (std::size_t character = 0; character < banners[banner].size(); ++character)
					{
						std::uint8_t const bits = fpga_status_character_row(
								board == model::T1,
								banners[banner][character],
								std::uint8_t(glyph_row));
						INFO("board " << unsigned(board) << " banner " << banner
								<< " character " << character << " glyph row " << glyph_row
								<< " duplicate " << duplicate);
						check_doubled_glyph(
								line,
								unsigned(character),
								bits,
								foreground,
								background);
					}
				}
			}
		}
	}
}


TEST_CASE("CoCoVGA renderer covers every native mode, CSS, and board model", "[cocovga][renderer][modes]")
{
	constexpr std::array<mode_kind, 9> modes =
	{{
		mode_kind::ALPHA_SEMIGRAPHICS,
		mode_kind::CG1,
		mode_kind::RG1,
		mode_kind::CG2,
		mode_kind::RG2,
		mode_kind::CG3,
		mode_kind::RG3,
		mode_kind::CG6,
		mode_kind::RG6
	}};

	for (mode_kind mode : modes)
	{
		for (model board : { model::AMC2, model::MODERN, model::T1 })
		{
			for (bool css : { false, true })
			{
				core control(board);
				REQUIRE(test::write_register(control, register_bank::ARTIFACT, 4, 0));
				frame_buffer_state const frame = test::make_native_frame(
						mode,
						css,
						std::uint8_t(0x25 + unsigned(board) * 31U));
				std::vector<render_pixel> pixels;
				pixel_surface const surface = test::make_surface(pixels);
				renderer output = cocovga::test::make_renderer();
				render_result const result = output.render(control, frame, surface);
				INFO("mode " << unsigned(mode) << " model " << unsigned(board)
						<< " css " << css << " status " << renderer::status_name(result.status));
				REQUIRE(result.success());
				CHECK(result.generation == frame.generation);
				CHECK(result.pixels_written == std::uint32_t(RENDER_WIDTH) * RENDER_HEIGHT);
				CHECK(result.scanlines_written == RENDER_HEIGHT);
				CHECK(result.mode_changes == 0);
				CHECK(result.first_mode == mode);
				CHECK(result.last_mode == mode);
				CHECK(output.state().render_calls == 1);
				CHECK(output.state().frames_rendered == 1);
				CHECK(output.state().scanlines_rendered == RENDER_HEIGHT);
				CHECK(std::all_of(
						pixels.begin(),
						pixels.end(),
						[] (render_pixel pixel) { return (pixel & 0xff000000U) == 0xff000000U; }));
				CHECK(pixels[0] ==
						render_effects::border_color(
								control,
								mode,
								frame.cells[0].pixel()).color.argb);
			}
		}
	}
}


TEST_CASE("CoCoVGA native renderer exercises text, inverse, SG4, and SG6 glyphs", "[cocovga][renderer][glyphs]")
{
	core control;
	REQUIRE(test::write_register(control, register_bank::ARTIFACT, 4, 0));
	REQUIRE(test::write_register(control, register_bank::EXTRAS, 5, 0x50));
	frame_buffer_state frame = test::make_native_frame(mode_kind::ALPHA_SEMIGRAPHICS, false, 0);
	frame.cells[3 * 32 + 0] = test::make_cell(mode_kind::ALPHA_SEMIGRAPHICS, false, 0x01);
	frame.cells[3 * 32 + 1] = test::make_cell(mode_kind::ALPHA_SEMIGRAPHICS, false, 0x01, false, true);
	frame.cells[0 * 32 + 2] = test::make_cell(mode_kind::ALPHA_SEMIGRAPHICS, false, 0x39, true);
	frame.cells[4 * 32 + 3] = test::make_cell(mode_kind::ALPHA_SEMIGRAPHICS, false, 0x06, true, false, true);

	text_palette_descriptor const inverse_text = control.text_palette(frame.cells[3 * 32 + 0].pixel());
	render_pixel const inverse_foreground =
			renderer::pack_color(control.palette_color(inverse_text.foreground));
	render_pixel const inverse_background =
			renderer::pack_color(control.palette_color(inverse_text.background));
	text_palette_descriptor const normal_text = control.text_palette(frame.cells[3 * 32 + 1].pixel());
	render_pixel const normal_foreground =
			renderer::pack_color(control.palette_color(normal_text.foreground));
	render_pixel const normal_background =
			renderer::pack_color(control.palette_color(normal_text.background));
	render_pixel const sg4_foreground = renderer::pack_color(control.semigraphics_color(4));
	render_pixel const sg_background = renderer::pack_color(control.semigraphics_color(0));
	render_pixel const sg6_foreground = renderer::pack_color(control.semigraphics_color(3));

	renderer output = cocovga::test::make_renderer();
	std::array<render_pixel, RENDER_WIDTH> line{};
	std::uint8_t const letter_a_row3 = test::fpga_text_row(false, 0x01, 3);
	render_result result = output.render_scanline(
			control,
			frame,
			VIEWPORT_TOP + 3 * 2,
			{ line.data(), line.size() });
	REQUIRE(result.success());
	check_doubled_glyph(line, 0, letter_a_row3, inverse_foreground, inverse_background);
	check_doubled_glyph(line, 1, letter_a_row3, normal_foreground, normal_background);

	result = output.render_scanline(
			control,
			frame,
			VIEWPORT_TOP,
			{ line.data(), line.size() });
	REQUIRE(result.success());
	check_doubled_glyph(line, 2, 0xf0, sg4_foreground, sg_background);

	result = output.render_scanline(
			control,
			frame,
			VIEWPORT_TOP + 4 * 2,
			{ line.data(), line.size() });
	REQUIRE(result.success());
	check_doubled_glyph(line, 3, 0x0f, sg6_foreground, sg_background);
}

TEST_CASE("CoCoVGA native renderer uses FPGA DECB and custom text-color polarity", "[cocovga][renderer][glyphs][palette][oracle]")
{
	for (bool css : { false, true })
	{
		for (bool inverse : { false, true })
		{
			core control;
			REQUIRE(test::write_register(control, register_bank::ARTIFACT, 4, 0));
			frame_buffer_state frame =
					test::make_native_frame(mode_kind::ALPHA_SEMIGRAPHICS, css, 0);
			frame.cells[3 * 32] = test::make_cell(
					mode_kind::ALPHA_SEMIGRAPHICS,
					css,
					0x01,
					false,
					inverse);

			std::array<render_pixel, RENDER_WIDTH> line{};
			renderer output = cocovga::test::make_renderer();
			REQUIRE(output.render_scanline(
					control,
					frame,
					VIEWPORT_TOP + 3 * 2,
					{ line.data(), line.size() }).success());

			std::uint8_t const bright = css ? 8 : 1;
			std::uint8_t const dark = css ? 10 : 9;
			render_pixel const foreground = renderer::pack_color(
					control.semigraphics_color(inverse ? 0 : bright));
			render_pixel const background = renderer::pack_color(
					control.semigraphics_color(inverse ? bright : dark));
			INFO("CSS " << css << " inverse " << inverse);
			check_doubled_glyph(line, 0, test::fpga_text_row(false, 0x01, 3), foreground, background);
		}
	}

	core custom;
	REQUIRE(test::write_register(custom, register_bank::ARTIFACT, 4, 0));
	REQUIRE(test::write_register(custom, register_bank::EXTRA_PALETTE, 128, 0xfc));
	REQUIRE(test::write_register(custom, register_bank::EXTRA_PALETTE, 129, 0x00));
	REQUIRE(test::write_register(custom, register_bank::EXTRA_PALETTE, 130, 0x80));
	REQUIRE(test::write_register(custom, register_bank::EXTRA_PALETTE, 131, 0x1f));
	REQUIRE(custom.commit_palettes_at_vsync());
	REQUIRE(test::write_register(custom, register_bank::EXTRAS, 5, 0x80));

	for (bool inverse : { false, true })
	{
		frame_buffer_state frame =
				test::make_native_frame(mode_kind::ALPHA_SEMIGRAPHICS, false, 0);
		frame.cells[3 * 32] = test::make_cell(
				mode_kind::ALPHA_SEMIGRAPHICS,
				false,
				0x01,
				false,
				inverse);
		std::array<render_pixel, RENDER_WIDTH> line{};
		renderer output = cocovga::test::make_renderer();
		REQUIRE(output.render_scanline(
				custom,
				frame,
				VIEWPORT_TOP + 3 * 2,
				{ line.data(), line.size() }).success());
		check_doubled_glyph(
				line,
				0,
				test::fpga_text_row(false, 0x01, 3),
				renderer::pack_color(custom.extra_color(
						inverse
								? extra_palette_slot::TEXT_FOREGROUND
								: extra_palette_slot::TEXT_BACKGROUND)),
				renderer::pack_color(custom.extra_color(
						inverse
								? extra_palette_slot::TEXT_BACKGROUND
								: extra_palette_slot::TEXT_FOREGROUND)));
	}
}

TEST_CASE("CoCoVGA MODEDEMO direct GM0 renders both ROM paths on every model", "[cocovga][renderer][glyphs][lowercase][palette][oracle][modedemo]")
{
	for (model board : { model::AMC2, model::MODERN, model::T1 })
	{
		for (unsigned font_bank = 0; font_bank < 2; ++font_bank)
		{
			for (bool css : { false, true })
			{
				core control(board);
				REQUIRE(test::write_register(control, register_bank::ARTIFACT, 4, 0));
				REQUIRE(test::write_register(
						control,
						register_bank::FONT,
						3,
						std::uint8_t(font_bank)));

				frame_buffer_state frame =
						test::make_native_frame(mode_kind::ALPHA_SEMIGRAPHICS, css, 0);
				captured_cell lowercase = test::make_cell(
						mode_kind::ALPHA_SEMIGRAPHICS,
						css,
						0x01,
						false,
						false,
						true);
				captured_cell charrom = test::make_cell(
						mode_kind::ALPHA_SEMIGRAPHICS,
						css,
						0x21,
						false,
						false,
						true);
				lowercase.gm = 1;
				charrom.gm = 1;
				for (std::uint16_t source_row = 0; source_row < HOST_ACTIVE_LINES; ++source_row)
				{
					frame.cells[std::size_t(source_row) * 32] = lowercase;
					frame.cells[std::size_t(source_row) * 32 + 1] = charrom;
				}

				text_palette_descriptor const lowercase_text =
						control.text_palette(lowercase.pixel());
				render_pixel const lowercase_foreground =
						renderer::pack_color(control.palette_color(lowercase_text.foreground));
				render_pixel const lowercase_background =
						renderer::pack_color(control.palette_color(lowercase_text.background));
				CHECK(lowercase_foreground ==
						renderer::pack_color(control.semigraphics_color(0)));
				CHECK(lowercase_background ==
						renderer::pack_color(control.semigraphics_color(css ? 8 : 1)));

				text_palette_descriptor const charrom_text = control.text_palette(charrom.pixel());
				render_pixel const charrom_foreground =
						renderer::pack_color(control.palette_color(charrom_text.foreground));
				render_pixel const charrom_background =
						renderer::pack_color(control.palette_color(charrom_text.background));

				renderer output = cocovga::test::make_renderer();
				for (unsigned glyph_row = 0; glyph_row < W64_GLYPH_HEIGHT; ++glyph_row)
				{
					for (unsigned duplicate = 0; duplicate < 2; ++duplicate)
					{
						std::array<render_pixel, RENDER_WIDTH> line{};
						REQUIRE(output.render_scanline(
								control,
								frame,
								std::uint16_t(VIEWPORT_TOP + glyph_row * 2 + duplicate),
								{ line.data(), line.size() }).success());
						INFO("board " << unsigned(board) << " font bank " << font_bank
								<< " CSS " << css << " glyph row " << glyph_row
								<< " duplicate " << duplicate);
						check_doubled_glyph(
								line,
								0,
								test::FPGA_LOWERCASE_ROWS[font_bank][1][glyph_row],
								lowercase_foreground,
								lowercase_background);
						check_doubled_glyph(
								line,
								1,
								cocovga::test::make_extended_renderer().text_character_row(
										font_bank != 0,
										0x21,
										std::uint8_t(glyph_row)),
								charrom_foreground,
								charrom_background);
					}
				}
			}
		}
	}
}

TEST_CASE("CoCoVGA native and character-RAM semigraphics use public palette slots", "[cocovga][renderer][glyphs][palette][oracle]")
{
	for (bool character_ram : { false, true })
	{
		for (bool css : { false, true })
		{
			for (unsigned code = 0; code < 8; ++code)
			{
				core control;
				REQUIRE(test::write_register(control, register_bank::ARTIFACT, 4, 0));
				if (character_ram)
				{
					REQUIRE(test::write_register(control, register_bank::FONT, 3, 0x04));
					REQUIRE(test::write_register(control, register_bank::ENHANCED_MODES, 8, 0x04));
				}

				frame_buffer_state frame =
						test::make_native_frame(mode_kind::ALPHA_SEMIGRAPHICS, css, 0);
				frame.cells[0] = test::make_cell(
						mode_kind::ALPHA_SEMIGRAPHICS,
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
				renderer output = cocovga::test::make_renderer();
				REQUIRE(output.render_scanline(
						control,
						frame,
						VIEWPORT_TOP,
						{ line.data(), line.size() }).success());
				INFO("SG4 character RAM " << character_ram << " css " << css << " code " << code);
				check_doubled_glyph(
						line,
						0,
						0xf0,
						renderer::pack_color(control.semigraphics_color(1 + code)),
						renderer::pack_color(control.semigraphics_color(0)));
			}
		}

		for (bool css : { false, true })
		{
			for (unsigned code : { 2U, 3U })
			{
				core control;
				REQUIRE(test::write_register(control, register_bank::ARTIFACT, 4, 0));
				REQUIRE(test::write_register(control, register_bank::EXTRAS, 5, 0x50));
				if (character_ram)
				{
					REQUIRE(test::write_register(control, register_bank::FONT, 3, 0x04));
					REQUIRE(test::write_register(control, register_bank::ENHANCED_MODES, 8, 0x04));
				}

				frame_buffer_state frame =
						test::make_native_frame(mode_kind::ALPHA_SEMIGRAPHICS, css, 0);
				frame.cells[4 * 32] = test::make_cell(
						mode_kind::ALPHA_SEMIGRAPHICS,
						css,
						0x06,
						true,
						(code & 1) != 0,
						true);

				std::array<render_pixel, RENDER_WIDTH> line{};
				renderer output = cocovga::test::make_renderer();
				REQUIRE(output.render_scanline(
						control,
						frame,
						VIEWPORT_TOP + 4 * 2,
						{ line.data(), line.size() }).success());
				INFO("SG6 character RAM " << character_ram << " css " << css << " code " << code);
				check_doubled_glyph(
						line,
						0,
						0x0f,
						renderer::pack_color(control.semigraphics_color(
								(css ? 5 : 1) + code)),
						renderer::pack_color(control.semigraphics_color(0)));
			}
		}
	}
}


TEST_CASE("CoCoVGA native renderer tracks mode changes and border selection", "[cocovga][renderer][borders]")
{
	core control;
	REQUIRE(test::write_register(control, register_bank::ARTIFACT, 4, 0));
	frame_buffer_state frame = test::make_native_frame(mode_kind::CG2, false, 0x44);
	for (std::uint16_t row = HOST_ACTIVE_LINES / 2; row < HOST_ACTIVE_LINES; ++row)
	{
		for (std::uint8_t column = 0; column < 32; ++column)
		{
			std::uint16_t const address = std::uint16_t(row * 32 + column);
			frame.cells[address] = test::make_cell(mode_kind::CG6, true, std::uint8_t(address));
		}
	}
	frame.mode_changes = 1;
	frame.last_mode = static_cast<std::uint8_t>(mode_kind::CG6);

	std::vector<render_pixel> pixels;
	pixel_surface const surface = test::make_surface(pixels);
	renderer output = cocovga::test::make_renderer();
	render_result result = output.render(control, frame, surface);
	REQUIRE(result.success());
	CHECK(result.mode_changes == 1);
	CHECK(result.first_mode == mode_kind::CG2);
	CHECK(result.last_mode == mode_kind::CG6);

	render_pixel const css0_border = renderer::pack_color(
			control.palette_color({ palette_source::SEMIGRAPHICS, 1, true }));
	render_pixel const css1_border = renderer::pack_color(
			control.palette_color({ palette_source::SEMIGRAPHICS, 5, true }));
	CHECK(pixels[0] == css0_border);
	CHECK(pixels[std::size_t(RENDER_HEIGHT - 1) * RENDER_WIDTH] == css1_border);
	CHECK(pixels[std::size_t(VIEWPORT_TOP) * RENDER_WIDTH + VIEWPORT_LEFT - 1] == css0_border);
	CHECK(pixels[std::size_t(VIEWPORT_BOTTOM) * RENDER_WIDTH + VIEWPORT_RIGHT + 1] == css1_border);

	REQUIRE(test::write_register(control, register_bank::EXTRAS, 5, 0x42));
	result = output.render(control, frame, surface);
	REQUIRE(result.success());
	CHECK(pixels[0] == 0xff000000U);
	CHECK(pixels[std::size_t(RENDER_HEIGHT - 1) * RENDER_WIDTH] == 0xff000000U);

	REQUIRE(test::write_register(control, register_bank::EXTRA_PALETTE, 132, 0x83));
	REQUIRE(control.write_page00(133, 0xff));
	REQUIRE(control.commit_palettes_at_vsync());
	REQUIRE(test::write_register(control, register_bank::EXTRAS, 5, 0x43));
	result = output.render(control, frame, surface);
	REQUIRE(result.success());
	render_pixel const custom = renderer::pack_color(control.extra_color(extra_palette_slot::BORDER));
	CHECK(pixels[0] == custom);
	CHECK(pixels[std::size_t(RENDER_HEIGHT - 1) * RENDER_WIDTH] == custom);
}

TEST_CASE("CoCoVGA renderer consumes mixed row lengths with deterministic clipping and fill", "[cocovga][renderer][modes][transition]")
{
	core control;
	REQUIRE(test::write_register(control, register_bank::ARTIFACT, 4, 0));
	renderer output = cocovga::test::make_renderer();
	constexpr std::uint16_t row = 37;
	std::array<render_pixel, RENDER_WIDTH> line{};

	frame_buffer_state short_to_long =
			test::make_native_frame(mode_kind::CG6, false, 0x31);
	short_to_long.row_fetch_counts[row] = 16;
	short_to_long.writes -= 16;
	for (std::uint8_t column = 8; column < 16; ++column)
	{
		short_to_long.cells[row * 32 + column] =
				test::make_cell(mode_kind::CG1, true, std::uint8_t(0x40 + column));
	}
	short_to_long.mode_changes = 2;
	REQUIRE(output.preflight(control, short_to_long).success());
	REQUIRE(output.render_scanline(
			control,
			short_to_long,
			VIEWPORT_TOP + row * 2,
			{ line.data(), line.size() }).success());

	captured_cell const &fill_cell = short_to_long.cells[row * 32 + 15];
	render_pixel const fill = render_effects::border_color(
			control,
			fill_cell.mode(),
			fill_cell.pixel()).color.argb;
	CHECK(std::all_of(
			line.begin() + VIEWPORT_LEFT + 384,
			line.begin() + VIEWPORT_RIGHT_EXCLUSIVE,
			[fill] (render_pixel pixel) { return pixel == fill; }));

	frame_buffer_state long_to_short =
			test::make_native_frame(mode_kind::RG3, false, 0x52);
	long_to_short.row_fetch_counts[row] = 32;
	long_to_short.writes += 16;
	for (std::uint8_t column = 8; column < 32; ++column)
	{
		long_to_short.cells[row * 32 + column] =
				test::make_cell(mode_kind::RG6, false, std::uint8_t(0x80 + column));
	}
	long_to_short.mode_changes = 2;
	REQUIRE(output.preflight(control, long_to_short).success());
	REQUIRE(output.render_scanline(
			control,
			long_to_short,
			VIEWPORT_TOP + row * 2,
			{ line.data(), line.size() }).success());
	std::uint64_t const clipped_hash = test::pixel_hash(
			line.data() + VIEWPORT_LEFT,
			VIEWPORT_WIDTH);

	long_to_short.cells[row * 32 + 31].data ^= 0xff;
	REQUIRE(output.render_scanline(
			control,
			long_to_short,
			VIEWPORT_TOP + row * 2,
			{ line.data(), line.size() }).success());
	CHECK(test::pixel_hash(
			line.data() + VIEWPORT_LEFT,
			VIEWPORT_WIDTH) == clipped_hash);
}


TEST_CASE("CoCoVGA renderer rejects invalid and inconsistent paths", "[cocovga][renderer][errors]")
{
	core control;
	REQUIRE(test::write_register(control, register_bank::ARTIFACT, 4, 0));
	frame_buffer_state valid = test::make_native_frame(mode_kind::CG2, false);
	std::vector<render_pixel> pixels;
	pixel_surface surface = test::make_surface(pixels);
	renderer output = cocovga::test::make_renderer();

	pixel_surface invalid_surface = surface;
	invalid_surface.pixels = nullptr;
	CHECK(output.render(control, valid, invalid_surface).status == render_status::INVALID_SURFACE);
	invalid_surface = surface;
	invalid_surface.width = RENDER_WIDTH - 1;
	CHECK(output.render(control, valid, invalid_surface).status == render_status::INVALID_SURFACE);
	invalid_surface = surface;
	invalid_surface.height = RENDER_HEIGHT - 1;
	CHECK(output.render(control, valid, invalid_surface).status == render_status::INVALID_SURFACE);
	invalid_surface = surface;
	invalid_surface.row_stride = RENDER_WIDTH - 1;
	CHECK(output.render(control, valid, invalid_surface).status == render_status::INVALID_SURFACE);
	CHECK(output.render_scanline(control, valid, RENDER_HEIGHT, { pixels.data(), RENDER_WIDTH }).status ==
			render_status::INVALID_SURFACE);
	CHECK(output.render_scanline(control, valid, 0, { nullptr, RENDER_WIDTH }).status ==
			render_status::INVALID_SURFACE);
	CHECK(output.render_scanline(control, valid, 0, { pixels.data(), RENDER_WIDTH - 1 }).status ==
			render_status::INVALID_SURFACE);

	frame_buffer_state frame = valid;
	frame.valid = false;
	CHECK(output.render(control, frame, surface).status == render_status::FRAME_UNAVAILABLE);
	frame = valid;
	frame.complete = false;
	CHECK(output.render(control, frame, surface).status == render_status::FRAME_INCOMPLETE);
	frame = valid;
	frame.programming_suppressed = true;
	CHECK(output.render(control, frame, surface).status == render_status::FRAME_INCOMPLETE);
	frame = valid;
	frame.active_lines = HOST_ACTIVE_LINES - 1;
	CHECK(output.render(control, frame, surface).status == render_status::FRAME_INCOMPLETE);

	for (unsigned mutation = 0; mutation < 8; ++mutation)
	{
		frame = valid;
		switch (mutation)
		{
		case 0:
			frame.first_address = CAPTURE_PAGE_BYTES;
			break;
		case 1:
			frame.last_address = CAPTURE_PAGE_BYTES;
			break;
		case 2:
			frame.first_mode = 0xff;
			break;
		case 3:
			frame.last_mode = 0xff;
			break;
		case 4:
			--frame.writes;
			break;
		case 5:
			frame.cells[0].mode_id = 0xff;
			break;
		case 6:
			frame.row_fetch_counts[0] = 0;
			break;
		case 7:
			frame.cells[0].gm = 7;
			break;
		}
		INFO("inconsistent mutation " << mutation);
		CHECK(output.render(control, frame, surface).status == render_status::INCONSISTENT_CAPTURE);
	}

	frame = test::make_native_frame(mode_kind::ALPHA_SEMIGRAPHICS, false);
	frame.cells[0].flags |= captured_cell::FLAG_GRAPHICS;
	CHECK(output.render(control, frame, surface).status == render_status::INCONSISTENT_CAPTURE);
	frame = valid;
	frame.row_fetch_counts[10] = 33;
	CHECK(output.render(control, frame, surface).status == render_status::INCONSISTENT_CAPTURE);

	std::array<render_pixel, RENDER_WIDTH> scanline{};
	frame = valid;
	frame.cells[10 * 32 + 1] = test::make_cell(mode_kind::CG1, false, 0);
	CHECK(output.render_scanline(
				control,
				frame,
				VIEWPORT_TOP,
				{ scanline.data(), scanline.size() }).success());
	CHECK(output.render(control, frame, surface).status == render_status::INCONSISTENT_CAPTURE);
	frame = valid;
	frame.cells[1] = test::make_cell(mode_kind::CG1, false, 0);
	CHECK(output.render_scanline(
				control,
				frame,
				VIEWPORT_TOP,
				{ scanline.data(), scanline.size() }).success());
	CHECK(output.render(control, frame, surface).status == render_status::INCONSISTENT_CAPTURE);
	frame = valid;
	frame.first_address = INVALID_CAPTURE_ADDRESS;
	CHECK(output.render_scanline(
				control,
				frame,
				0,
				{ scanline.data(), scanline.size() }).status == render_status::INCONSISTENT_CAPTURE);
	frame = valid;
	frame.last_address = INVALID_CAPTURE_ADDRESS;
	CHECK(output.render_scanline(
				control,
				frame,
				RENDER_HEIGHT - 1,
				{ scanline.data(), scanline.size() }).status == render_status::INCONSISTENT_CAPTURE);

	render_options options;
	options.border_status.kind = render_effects::border_status_kind::BOOT_VERSION;
	options.border_status.visible = true;
	CHECK(output.render(control, valid, surface, options).success());

	core artifact_control;
	frame = test::make_native_frame(mode_kind::RG6, false);
	CHECK(output.render(artifact_control, frame, surface).success());

	core w64_control;
	w64_control.state().w64_active = true;
	CHECK(output.render(w64_control, valid, surface).status == render_status::INCONSISTENT_CAPTURE);

	core vg6_control;
	vg6_control.state().vg6_active = true;
	CHECK(output.render(vg6_control, valid, surface).status == render_status::INCONSISTENT_CAPTURE);

	core character_control;
	REQUIRE(test::write_register(character_control, register_bank::ARTIFACT, 4, 0));
	REQUIRE(test::write_register(character_control, register_bank::ENHANCED_MODES, 8, 0x04));
	frame = test::make_native_frame(mode_kind::ALPHA_SEMIGRAPHICS, false);
	CHECK(output.render(character_control, frame, surface).success());

	core scanline_control;
	REQUIRE(test::write_register(scanline_control, register_bank::ARTIFACT, 4, 0));
	REQUIRE(test::write_register(scanline_control, register_bank::EXTRAS, 5, 0x44));
	CHECK(output.render(scanline_control, valid, surface).success());
}


TEST_CASE("CoCoVGA renderer state counts full frames, scanlines, failures, reset, and restore", "[cocovga][renderer][restore]")
{
	core control;
	REQUIRE(test::write_register(control, register_bank::ARTIFACT, 4, 0));
	frame_buffer_state frame = test::make_native_frame(mode_kind::CG3, true);
	std::vector<render_pixel> pixels;
	pixel_surface const surface = test::make_surface(pixels);
	renderer output = cocovga::test::make_renderer();

	render_result full = output.render(control, frame, surface);
	REQUIRE(full.success());
	std::array<render_pixel, RENDER_WIDTH> line{};
	render_result single = output.render_scanline(
			control,
			frame,
			VIEWPORT_TOP,
			{ line.data(), line.size() });
	REQUIRE(single.success());
	frame.valid = false;
	render_result failed = output.render(control, frame, surface);
	CHECK(failed.status == render_status::FRAME_UNAVAILABLE);

	renderer_state const saved = output.state();
	CHECK(saved.render_calls == 3);
	CHECK(saved.frames_rendered == 1);
	CHECK(saved.scanlines_rendered == RENDER_HEIGHT + 1);
	CHECK(saved.last_generation == frame.generation);
	CHECK(saved.last_pixels_written == 0);
	CHECK(saved.last_status == render_status::FRAME_UNAVAILABLE);
	CHECK(saved.last_first_mode == mode_kind::CG3);
	CHECK(saved.last_last_mode == mode_kind::CG3);

	output.reset();
	CHECK(output.state().render_calls == 0);
	CHECK(output.state().frames_rendered == 0);
	CHECK(output.state().scanlines_rendered == 0);
	output.restore_state(saved);
	CHECK(output.state().render_calls == 3);
	CHECK(output.state().frames_rendered == 1);
	CHECK(output.state().scanlines_rendered == RENDER_HEIGHT + 1);
	CHECK(output.state().last_status == render_status::FRAME_UNAVAILABLE);
}
