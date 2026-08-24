// license:BSD-3-Clause
// copyright-holders:Stephen Spiller

#include "catch.hpp"

#include "cocovga_artifact_vectors.h"
#include "cocovga_test_helpers.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <string>

namespace
{

using namespace cocovga;
using namespace cocovga::render_effects;

constexpr std::uint8_t swap_pair(std::uint8_t index)
{
	if (index >= 1 && index <= 13 && (index & 1))
		return std::uint8_t(index + 1);
	if (index >= 2 && index <= 14 && !(index & 1))
		return std::uint8_t(index - 1);
	return index;
}

constexpr std::uint8_t absolute_index(bool css, std::uint8_t relative)
{
	return std::uint8_t((css ? 0 : 16) + relative);
}

void set_source_bit(std::array<std::uint8_t, RG6_SOURCE_BYTES> &source, unsigned bit)
{
	source[bit / 8] |= std::uint8_t(0x80U >> (bit & 7));
}

bool oracle_source_bit(
		std::array<std::uint8_t, RG6_SOURCE_BYTES> const &source,
		std::ptrdiff_t position)
{
	return position >= 0 &&
			position < std::ptrdiff_t(RG6_SOURCE_PIXELS) &&
			(source[std::size_t(position) / 8] & (0x80U >> (std::size_t(position) & 7))) != 0;
}

std::uint8_t oracle_pair_pattern(
		std::array<std::uint8_t, RG6_SOURCE_BYTES> const &source,
		std::size_t pair_start)
{
	return std::uint8_t(
			(oracle_source_bit(source, std::ptrdiff_t(pair_start)) ? 2 : 0) |
			(oracle_source_bit(source, std::ptrdiff_t(pair_start + 1)) ? 1 : 0));
}

std::uint8_t oracle_six_tap_pattern(
		std::array<std::uint8_t, RG6_SOURCE_BYTES> const &source,
		std::size_t pair_start)
{
	std::uint8_t result = 0;
	for (std::ptrdiff_t offset = -2; offset <= 3; ++offset)
	{
		result = std::uint8_t(
				(result << 1) |
				(oracle_source_bit(source, std::ptrdiff_t(pair_start) + offset) ? 1 : 0));
	}
	return result;
}

// Expected RG6 palette index for one output pixel, built from the documented
// RG6 contract and the externally recorded FPGA palette vectors rather than
// from the palette helpers under test.
std::uint8_t documented_rg6_palette_index(
		artifact_controls const &settings,
		bool css,
		std::array<std::uint8_t, RG6_SOURCE_BYTES> const &source,
		std::size_t output_position)
{
	unsigned const css_index = css ? 1 : 0;
	unsigned const swap_index = settings.swap ? 1 : 0;
	std::size_t const logical_position = output_position / 2;
	bool const pixel = oracle_source_bit(source, std::ptrdiff_t(logical_position));
	if (!settings.color_enabled || settings.mode == artifact_mode::MONOCHROME)
		return absolute_index(css, pixel ? 15 : 0);

	std::size_t const pair_start = logical_position & ~std::size_t(1);
	std::uint8_t const pair = oracle_pair_pattern(source, pair_start);
	std::uint8_t const window = oracle_six_tap_pattern(source, pair_start);
	switch (settings.mode)
	{
	case artifact_mode::STANDARD:
		return test::STANDARD_PALETTE_INDICES[css_index][swap_index][pair][logical_position & 1];

	case artifact_mode::FAT_BITS:
		return test::SIMPLE_PALETTE_INDICES[css_index][swap_index][pair];

	case artifact_mode::SMARTIFACT:
		return test::SMARTIFACT_PALETTE_INDICES[css_index][swap_index][window]
				[((logical_position & 1) * 2) + (output_position & 1)];

	case artifact_mode::MESS:
		return test::MESS_PALETTE_INDICES[css_index][swap_index][window][logical_position & 1];

	case artifact_mode::MONOCHROME:
	default:
		return absolute_index(css, pixel ? 15 : 0);
	}
}

std::array<std::uint8_t, RG6_SOURCE_BYTES> source_for_pattern(
		std::uint8_t pattern,
		unsigned pair_start = 8)
{
	std::array<std::uint8_t, RG6_SOURCE_BYTES> source{};
	for (unsigned index = 0; index < 6; ++index)
	{
		if (pattern & (0x20U >> index))
			set_source_bit(source, pair_start - 2 + index);
	}
	return source;
}

} // anonymous namespace


TEST_CASE("CoCoVGA standard and fat-bit artifact selections follow public palette rules", "[cocovga][artifact][oracle]")
{
	// Public source: http://cocovga.com/documentation/artifact-emulation/
	// The documented pair order is 00, 01, 10, 11 -> 0, 9, 10, 15.
	constexpr std::array<std::uint8_t, 4> documented = { 0, 9, 10, 15 };

	for (bool css : { false, true })
	{
		for (bool swap : { false, true })
		{
			for (unsigned pattern = 0; pattern < 4; ++pattern)
			{
				std::uint8_t relative = documented[pattern];
				if (swap)
					relative = swap_pair(relative);
				std::uint8_t const expected = absolute_index(css, relative);
				INFO("css " << css << " swap " << swap << " pattern " << pattern);
				CHECK(simple_artifact_palette_index(css, swap, std::uint8_t(pattern)) == expected);
				CHECK(fat_bits_artifact_palette_index(css, swap, std::uint8_t(pattern)) == expected);

				for (unsigned position = 0; position < 2; ++position)
				{
					bool const source_pixel = (pattern & (2U >> position)) != 0;
					CHECK(standard_artifact_palette_index(
								css,
								swap,
								std::uint8_t(pattern),
								std::uint8_t(position)) ==
							(source_pixel ? expected : absolute_index(css, 0)));
				}
			}
		}

		CHECK(monochrome_palette_index(css, false) == absolute_index(css, 0));
		CHECK(monochrome_palette_index(css, true) == absolute_index(css, 15));
	}

	CHECK(simple_artifact_palette_index(true, false, 0xff) == 15);
	CHECK(standard_artifact_palette_index(false, false, 0xff, 0xff) == 31);
}


TEST_CASE("CoCoVGA artifact palette selectors match the FPGA vectors", "[cocovga][artifact][oracle]")
{
	// cocovga_artifact_vectors.h carries the palette index the FPGA selects
	// for every documented input combination, recorded from a simulation of
	// the pinned CoCoVGA sources.  None of it is produced by MAME, so every
	// check below compares an implementation result against external data.
	std::array<bool, ARTIFACT_PALETTE_ENTRIES> simple_colors{};
	std::array<bool, ARTIFACT_PALETTE_ENTRIES> mess_colors{};
	std::array<bool, ARTIFACT_PALETTE_ENTRIES> smart_colors{};
	unsigned checked = 0;

	for (unsigned css = 0; css < 2; ++css)
	{
		for (unsigned swap = 0; swap < 2; ++swap)
		{
			for (unsigned pattern = 0; pattern < 4; ++pattern)
			{
				std::uint8_t const expected = test::SIMPLE_PALETTE_INDICES[css][swap][pattern];
				INFO("css " << css << " swap " << swap << " pair pattern " << pattern);
				CHECK(simple_artifact_palette_index(css != 0, swap != 0, std::uint8_t(pattern)) == expected);
				CHECK(fat_bits_artifact_palette_index(css != 0, swap != 0, std::uint8_t(pattern)) == expected);
				simple_colors[expected] = true;
				checked += 2;

				for (unsigned pixel = 0; pixel < 2; ++pixel)
				{
					// standard artifacting only paints the lit pixel of the pair
					INFO("pixel " << pixel);
					CHECK(standard_artifact_palette_index(
								css != 0,
								swap != 0,
								std::uint8_t(pattern),
								std::uint8_t(pixel)) ==
							test::STANDARD_PALETTE_INDICES[css][swap][pattern][pixel]);
					++checked;
				}
			}

			for (unsigned window = 0; window < 64; ++window)
			{
				for (unsigned phase = 0; phase < 2; ++phase)
				{
					std::uint8_t const expected = test::MESS_PALETTE_INDICES[css][swap][window][phase];
					INFO("css " << css << " swap " << swap
							<< " MESS window " << window << " phase " << phase);
					CHECK(mess_artifact_palette_index(
								css != 0,
								swap != 0,
								std::uint8_t(window),
								std::uint8_t(phase)) == expected);
					mess_colors[expected] = true;
					++checked;
				}

				for (unsigned phase = 0; phase < 4; ++phase)
				{
					std::uint8_t const expected = test::SMARTIFACT_PALETTE_INDICES[css][swap][window][phase];
					INFO("css " << css << " swap " << swap
							<< " smArtifact window " << window << " phase " << phase);
					CHECK(smartifact_palette_index(
								css != 0,
								swap != 0,
								std::uint8_t(window),
								std::uint8_t(phase)) == expected);
					smart_colors[expected] = true;
					++checked;
				}
			}
		}
	}

	// 1,568 recorded FPGA vectors plus the 32 standard-mode pixel positions
	CHECK(checked == 1'600);

	// the vectors reach every color the hardware can select in each mode
	CHECK(std::count(simple_colors.begin(), simple_colors.end(), true) == 8);
	CHECK(std::count(mess_colors.begin(), mess_colors.end(), true) == ARTIFACT_PALETTE_ENTRIES);
	CHECK(std::count(smart_colors.begin(), smart_colors.end(), true) == 12);
}


TEST_CASE("CoCoVGA RG6 scanline integration covers every artifact mode", "[cocovga][artifact][pixels]")
{
	std::array<std::uint8_t, RG6_SOURCE_BYTES> source{};
	for (std::size_t index = 0; index < source.size(); ++index)
		source[index] = std::uint8_t(index * 73U + 0x5d);

	for (artifact_mode mode : {
			artifact_mode::STANDARD,
			artifact_mode::FAT_BITS,
			artifact_mode::SMARTIFACT,
			artifact_mode::MESS,
			artifact_mode::MONOCHROME })
	{
		for (bool css : { false, true })
		{
			for (bool swap : { false, true })
			{
				artifact_controls settings;
				settings.color_enabled = mode != artifact_mode::MONOCHROME;
				settings.swap = swap;
				settings.mode = mode;
				std::array<std::uint8_t, RG6_OUTPUT_PIXELS> indices{};
				REQUIRE(render_rg6_palette_indices(
						settings,
						css,
						source.data(),
						source.size(),
						indices.data(),
						indices.size()) == effect_status::OK);

				for (std::size_t position = 0; position < indices.size(); ++position)
				{
					std::uint8_t single = 0xff;
					REQUIRE(rg6_palette_index(
							settings,
							css,
							source.data(),
							source.size(),
							position,
							single));
					CHECK(single == indices[position]);
					CHECK(single < ARTIFACT_PALETTE_ENTRIES);
				}

				if (mode == artifact_mode::STANDARD || mode == artifact_mode::MESS || mode == artifact_mode::MONOCHROME)
				{
					for (std::size_t position = 0; position < indices.size(); position += 2)
						CHECK(indices[position] == indices[position + 1]);
				}
			}
		}
	}

	artifact_controls settings;
	std::uint8_t index = 0xff;
	CHECK_FALSE(rg6_palette_index(settings, false, nullptr, source.size(), 0, index));
	CHECK(index == 16);
	CHECK_FALSE(rg6_palette_index(settings, true, source.data(), source.size() - 1, 0, index));
	CHECK(index == 0);
	CHECK_FALSE(rg6_palette_index(settings, false, source.data(), source.size(), RG6_OUTPUT_PIXELS, index));
	CHECK(index == 16);

	std::array<std::uint8_t, RG6_OUTPUT_PIXELS> indices{};
	CHECK(render_rg6_palette_indices(settings, false, nullptr, source.size(), indices.data(), indices.size()) ==
			effect_status::INVALID_SOURCE);
	CHECK(render_rg6_palette_indices(settings, false, source.data(), source.size() - 1, indices.data(), indices.size()) ==
			effect_status::INVALID_SOURCE);
	CHECK(render_rg6_palette_indices(settings, false, source.data(), source.size(), nullptr, indices.size()) ==
			effect_status::INVALID_DESTINATION);
	CHECK(render_rg6_palette_indices(settings, false, source.data(), source.size(), indices.data(), indices.size() - 1) ==
			effect_status::INVALID_DESTINATION);
}


TEST_CASE("CoCoVGA RG6 six-tap windows zero-pad both source edges", "[cocovga][artifact][pixels]")
{
	artifact_controls smart;
	smart.color_enabled = true;
	smart.mode = artifact_mode::SMARTIFACT;

	std::array<std::uint8_t, RG6_SOURCE_BYTES> first{};
	set_source_bit(first, 0);
	std::uint8_t index = 0;
	REQUIRE(rg6_palette_index(smart, true, first.data(), first.size(), 0, index));
	CHECK(index == test::SMARTIFACT_PALETTE_INDICES[1][0][0b001000][0]);
	REQUIRE(rg6_palette_index(smart, true, first.data(), first.size(), 1, index));
	CHECK(index == test::SMARTIFACT_PALETTE_INDICES[1][0][0b001000][1]);

	std::array<std::uint8_t, RG6_SOURCE_BYTES> last{};
	set_source_bit(last, RG6_SOURCE_PIXELS - 1);
	REQUIRE(rg6_palette_index(smart, true, last.data(), last.size(), RG6_OUTPUT_PIXELS - 2, index));
	CHECK(index == test::SMARTIFACT_PALETTE_INDICES[1][0][0b000100][2]);
	REQUIRE(rg6_palette_index(smart, true, last.data(), last.size(), RG6_OUTPUT_PIXELS - 1, index));
	CHECK(index == test::SMARTIFACT_PALETTE_INDICES[1][0][0b000100][3]);

	std::array<std::uint8_t, RG6_SOURCE_BYTES> centered = source_for_pattern(0b101101);
	for (unsigned phase = 0; phase < 4; ++phase)
	{
		REQUIRE(rg6_palette_index(
				smart,
				true,
				centered.data(),
				centered.size(),
				16 + phase,
				index));
		CHECK(index == test::SMARTIFACT_PALETTE_INDICES[1][0][0b101101][phase]);
	}
}


TEST_CASE("CoCoVGA artifact pixels follow documented RG6 positioning and palette colors", "[cocovga][artifact][pixels][oracle]")
{
	// The public RG6 contract doubles each source bit and applies pair or
	// six-tap palette selection, so every output position follows from the
	// externally recorded palette vectors instead of a frame hash.
	std::array<std::uint8_t, RG6_SOURCE_BYTES> source{};
	for (std::size_t index = 0; index < source.size(); ++index)
		source[index] = std::uint8_t(0xa7U + index * 41U);

	for (artifact_mode mode : {
			artifact_mode::STANDARD,
			artifact_mode::FAT_BITS,
			artifact_mode::SMARTIFACT,
			artifact_mode::MESS,
			artifact_mode::MONOCHROME })
	{
		for (bool css : { false, true })
		{
			for (bool swap : { false, true })
			{
				core control;
				std::uint8_t const artifact =
						mode == artifact_mode::MONOCHROME
								? std::uint8_t(swap ? 0x02 : 0)
								: std::uint8_t(
										0x01 |
										(swap ? 0x02 : 0) |
										(static_cast<unsigned>(mode) << 2));
				REQUIRE(test::write_register(control, register_bank::ARTIFACT, 4, artifact));
				pixel_input input;
				input.graphics = true;
				input.gm = 7;
				input.css = css;
				artifact_controls const settings = control.controls(input).artifact;
				std::array<effect_pixel, RG6_OUTPUT_PIXELS> pixels{};
				REQUIRE(render_rg6_pixels(
						control,
						input,
						source.data(),
						source.size(),
						pixels.data(),
						pixels.size()) == effect_status::OK);

				for (std::size_t position = 0; position < pixels.size(); ++position)
				{
					std::uint8_t const index = documented_rg6_palette_index(
							settings,
							css,
							source,
							position);
					INFO("mode " << unsigned(mode) << " css " << css
							<< " swap " << swap << " position " << position);
					CHECK(pixels[position] == pack_color(control.artifact_color(index)));
				}
			}
		}
	}

	core control;
	pixel_input input;
	effect_pixel pixel = 0;
	CHECK_FALSE(rg6_pixel(control, input, nullptr, source.size(), 0, pixel));
	CHECK(pixel == OPAQUE_BLACK);
	std::array<effect_pixel, RG6_OUTPUT_PIXELS> pixels{};
	CHECK(render_rg6_pixels(control, input, nullptr, source.size(), pixels.data(), pixels.size()) ==
			effect_status::INVALID_SOURCE);
	CHECK(render_rg6_pixels(control, input, source.data(), source.size(), nullptr, pixels.size()) ==
			effect_status::INVALID_DESTINATION);
	CHECK(render_rg6_pixels(control, input, source.data(), source.size(), pixels.data(), pixels.size() - 1) ==
			effect_status::INVALID_DESTINATION);
}


TEST_CASE("CoCoVGA DECB text polarity follows the FPGA mux for CSS, INV, GM, lowercase, RAM, and custom colors", "[cocovga][artifact][palette][text][oracle]")
{
	auto check_indices = [] (
			core const &control,
			pixel_input const &input,
			std::uint8_t foreground,
			std::uint8_t background)
	{
		text_color_pair const text = text_colors(control, input);
		CHECK(text.foreground.index == foreground);
		CHECK(text.background.index == background);
	};

	for (bool css : { false, true })
	{
		for (bool inverse : { false, true })
		{
			for (bool force_lowercase : { false, true })
			{
				for (bool character_ram : { false, true })
				{
					for (unsigned gm = 0; gm < 8; ++gm)
					{
						for (bool dd5 : { false, true })
						{
							for (bool w64 : { false, true })
							{
								for (bool inverse_text : { false, true })
								{
									core control;
									control.state().w64_active = w64;
									REQUIRE(test::write_register(
											control,
											register_bank::FONT,
											3,
											std::uint8_t(
													(force_lowercase ? 0x02 : 0) |
													(character_ram ? 0x04 : 0))));
									REQUIRE(test::write_register(
											control,
											register_bank::ENHANCED_MODES,
											8,
											character_ram ? 0x04 : 0));
									REQUIRE(test::write_register(
											control,
											register_bank::EXTRAS,
											5,
											inverse_text ? 0x20 : 0));

									pixel_input input;
									input.css = css;
									input.data = dd5 ? 0x21 : 0x01;
									input.inverse = inverse;
									input.gm = std::uint8_t(gm);

									bool const dd65_clear = !inverse && !dd5;
									bool const gm_polarity =
											((((gm & 0x01) != 0) && dd65_clear) !=
													((gm & 0x02) != 0)) &&
											!w64;
									bool const black_text =
											(inverse || force_lowercase || character_ram || gm_polarity) !=
											inverse_text;
									std::uint8_t const bright = css ? 8 : 1;
									std::uint8_t const dark = css ? 10 : 9;
									INFO("CSS " << css << " INV " << inverse
											<< " force lowercase " << force_lowercase
											<< " character RAM " << character_ram
											<< " GM " << gm << " DD5 " << dd5
											<< " W64 " << w64 << " inverse text " << inverse_text);
									check_indices(
											control,
											input,
											black_text ? 0 : bright,
											black_text ? bright : dark);
								}
							}
						}
					}
				}
			}
		}
	}

	pixel_input input;
	input.data = 0x01;

	core custom;
	REQUIRE(test::write_register(custom, register_bank::EXTRA_PALETTE, 128, 0xfc));
	REQUIRE(test::write_register(custom, register_bank::EXTRA_PALETTE, 129, 0x00));
	REQUIRE(test::write_register(custom, register_bank::EXTRA_PALETTE, 130, 0x80));
	REQUIRE(test::write_register(custom, register_bank::EXTRA_PALETTE, 131, 0x1f));
	REQUIRE(custom.commit_palettes_at_vsync());
	REQUIRE(test::write_register(custom, register_bank::EXTRAS, 5, 0x80));

	input = pixel_input{};
	input.data = 0x01;
	text_color_pair inverse = text_colors(custom, input);
	CHECK(inverse.foreground.source == color_source::EXTRA);
	CHECK(inverse.foreground.index == static_cast<std::uint8_t>(extra_palette_slot::TEXT_BACKGROUND));
	CHECK(inverse.background.index == static_cast<std::uint8_t>(extra_palette_slot::TEXT_FOREGROUND));
	CHECK(inverse.foreground.argb == renderer::pack_color(custom.extra_color(extra_palette_slot::TEXT_BACKGROUND)));
	CHECK(inverse.background.argb == renderer::pack_color(custom.extra_color(extra_palette_slot::TEXT_FOREGROUND)));

	input.inverse = true;
	text_color_pair normal = text_colors(custom, input);
	CHECK(normal.foreground.index == static_cast<std::uint8_t>(extra_palette_slot::TEXT_FOREGROUND));
	CHECK(normal.background.index == static_cast<std::uint8_t>(extra_palette_slot::TEXT_BACKGROUND));
	CHECK(normal.foreground.argb == renderer::pack_color(custom.extra_color(extra_palette_slot::TEXT_FOREGROUND)));
	CHECK(normal.background.argb == renderer::pack_color(custom.extra_color(extra_palette_slot::TEXT_BACKGROUND)));
}


TEST_CASE("CoCoVGA color effects resolve palettes, borders, and scanlines", "[cocovga][artifact][palette]")
{
	core modern(model::MODERN);
	core amc2(model::AMC2);

	CHECK(pack_color(modern.semigraphics_color(5)) == 0xffffffffU);
	CHECK(pack_color(amc2.semigraphics_color(5)) == 0xffffffffU);
	CHECK(pack_color(modern.semigraphics_color(0)) == OPAQUE_BLACK);
	palette_output invalid;
	invalid.bits_per_component = 4;
	CHECK_FALSE(valid_palette_output(invalid));
	CHECK(pack_color(invalid, 0x12345678U) == 0x12345678U);
	invalid.bits_per_component = 5;
	invalid.color.red = 32;
	CHECK_FALSE(valid_palette_output(invalid));

	CHECK(darken_pixel(0xff804020U, 0) == 0xff804020U);
	CHECK(darken_pixel(0xff804020U, 255) == OPAQUE_BLACK);
	CHECK((darken_pixel(0x7f804020U, 128) & 0xff000000U) == 0x7f000000U);

	resolved_color black = fixed_black();
	CHECK(black.valid);
	CHECK_FALSE(black.programmable());
	CHECK(black.argb == OPAQUE_BLACK);
	resolved_color invalid_semigraphics = semigraphics_color(modern, 0xff);
	CHECK_FALSE(invalid_semigraphics.valid);
	CHECK(invalid_semigraphics.argb == OPAQUE_BLACK);
	resolved_color invalid_artifact = artifact_color(modern, 0xff);
	CHECK_FALSE(invalid_artifact.valid);

	pixel_input input;
	input.css = false;
	core t1(model::T1);
	border_color_result const standard_border =
			border_color(modern, mode_kind::ALPHA_SEMIGRAPHICS, input);
	border_color_result const t1_border =
			border_color(t1, mode_kind::ALPHA_SEMIGRAPHICS, input);
	CHECK(t1_border.requested == border_mode::T1);
	CHECK(t1_border.effective == border_mode::STANDARD);
	CHECK(t1_border.t1_fallback);
	CHECK(t1_border.color.argb == standard_border.color.argb);

	text_color_pair text = text_colors(modern, input);
	CHECK(text.foreground.index == 1);
	CHECK(text.background.index == 9);
	CHECK(background_color(modern, mode_kind::ALPHA_SEMIGRAPHICS, input).index == 9);
	input.alpha_semigraphics = true;
	CHECK(background_color(modern, mode_kind::ALPHA_SEMIGRAPHICS, input).index == 0);
	input.alpha_semigraphics = false;
	CHECK(background_color(modern, mode_kind::CG2, input).index == 1);
	CHECK(background_color(modern, mode_kind::RG6, input).index == 16);
	CHECK(background_color(modern, mode_kind::VG6, input).index == 0);
	input.css = true;
	CHECK(background_color(modern, mode_kind::CG2, input).index == 5);
	CHECK(background_color(modern, mode_kind::RG6, input).index == 0);
	CHECK(background_color(modern, mode_kind::VG6, input).index == 16);

	border_color_result border = border_color(modern, mode_kind::ALPHA_SEMIGRAPHICS, input);
	CHECK(border.requested == border_mode::STANDARD);
	CHECK(border.effective == border_mode::STANDARD);
	CHECK(border.color.source == color_source::SEMIGRAPHICS_DEFAULT);
	CHECK(border.color.index == 0);

	REQUIRE(test::write_register(modern, register_bank::EXTRAS, 5, 0x43));
	REQUIRE(test::write_register(modern, register_bank::EXTRA_PALETTE, 132, 0xfc));
	REQUIRE(modern.write_page00(133, 0x00));
	REQUIRE(modern.commit_palettes_at_vsync());
	border = border_color(modern, mode_kind::CG2, input);
	CHECK(border.requested == border_mode::CUSTOM);
	CHECK(border.effective == border_mode::CUSTOM);
	CHECK(border.color.source == color_source::EXTRA);
	CHECK(border.color.index == static_cast<std::uint8_t>(extra_palette_slot::BORDER));
	CHECK(border.color.overridden());
	CHECK(border.color.argb == 0xffff0000U);

	REQUIRE(test::write_register(modern, register_bank::EXTRAS, 5, 0x41));
	border = border_color(modern, mode_kind::CG2, input);
	CHECK(border.requested == border_mode::T1);
	CHECK(border.effective == border_mode::STANDARD);
	CHECK(border.t1_fallback);
	border = border_color(modern, mode_kind::VG6, input);
	CHECK(border.effective == border_mode::BLACK);
	CHECK(border.mode_forced_black);
	CHECK(border.color.argb == OPAQUE_BLACK);

	REQUIRE(test::write_register(modern, register_bank::EXTRA_PALETTE, 134, 0x83));
	REQUIRE(modern.write_page00(135, 0xe0));
	REQUIRE(modern.commit_palettes_at_vsync());
	REQUIRE(test::write_register(modern, register_bank::EXTRAS, 5, 0x44));
	CHECK_FALSE(scanline_row(true, 0));
	CHECK(scanline_row(true, 1));
	CHECK_FALSE(scanline_row(false, 1));
	effect_pixel const source = 0xff123456U;
	CHECK(scanline_pixel(modern, input, 0, source) == source);
	CHECK(scanline_pixel(modern, input, 1, source) == scanline_color(modern).argb);
	std::array<effect_pixel, 8> line;
	line.fill(source);
	REQUIRE(apply_scanline(modern, input, 0, line.data(), line.size()) == effect_status::OK);
	for (effect_pixel value : line)
		CHECK(value == source);
	REQUIRE(apply_scanline(modern, input, 1, line.data(), line.size()) == effect_status::OK);
	for (effect_pixel value : line)
		CHECK(value == scanline_color(modern).argb);
	CHECK(apply_scanline(modern, input, 1, nullptr, line.size()) == effect_status::INVALID_DESTINATION);
}


TEST_CASE("CoCoVGA VGA timing validation reports every boundary and fallback", "[cocovga][artifact][timing]")
{
	vga_timing const standard;
	CHECK(valid_vga_timing(standard));
	CHECK(vga_timing_errors(standard) == timing_error::NONE);

	struct error_case
	{
		timing_error error;
		vga_timing timing;
	};

	std::array<error_case, 12> cases;
	for (error_case &entry : cases)
		entry.timing = standard;
	cases[0].error = timing_error::H_ACTIVE_ZERO;
	cases[0].timing.h_active = 0;
	cases[1].error = timing_error::H_ACTIVE_RANGE;
	cases[1].timing.h_active = 0x0800;
	cases[2].error = timing_error::H_FRONT_ZERO;
	cases[2].timing.h_front_porch = 0;
	cases[3].error = timing_error::H_SYNC_ZERO;
	cases[3].timing.h_sync_width = 0;
	cases[4].error = timing_error::H_BACK_ZERO;
	cases[4].timing.h_back_porch = 0;
	cases[5].error = timing_error::H_TOTAL_OVERFLOW;
	cases[5].timing.h_active = std::numeric_limits<std::uint16_t>::max();
	cases[5].timing.h_front_porch = 255;
	cases[6].error = timing_error::V_ACTIVE_ZERO;
	cases[6].timing.v_active = 0;
	cases[7].error = timing_error::V_ACTIVE_RANGE;
	cases[7].timing.v_active = 0x0400;
	cases[8].error = timing_error::V_FRONT_ZERO;
	cases[8].timing.v_front_porch = 0;
	cases[9].error = timing_error::V_SYNC_ZERO;
	cases[9].timing.v_sync_width = 0;
	cases[10].error = timing_error::V_BACK_ZERO;
	cases[10].timing.v_back_porch = 0;
	cases[11].error = timing_error::V_TOTAL_OVERFLOW;
	cases[11].timing.v_active = std::numeric_limits<std::uint16_t>::max();
	cases[11].timing.v_front_porch = 255;

	for (error_case const &entry : cases)
	{
		timing_error const errors = vga_timing_errors(entry.timing);
		INFO("error bit " << static_cast<unsigned>(entry.error));
		CHECK(has_timing_error(errors, entry.error));
		CHECK_FALSE(valid_vga_timing(entry.timing));
	}

	vga_timing horizontal_limit{ 2'045, 1, 1, 1, 1, 1, 1, 1 };
	CHECK(valid_vga_timing(horizontal_limit));
	horizontal_limit.h_active = 2'046;
	CHECK(has_timing_error(vga_timing_errors(horizontal_limit), timing_error::H_TOTAL_OVERFLOW));
	CHECK_FALSE(valid_vga_timing(horizontal_limit));
	vga_timing vertical_limit{ 1, 1, 1, 1, 1'020, 1, 1, 1 };
	CHECK(valid_vga_timing(vertical_limit));
	vertical_limit.v_active = 1'021;
	CHECK(has_timing_error(vga_timing_errors(vertical_limit), timing_error::V_TOTAL_OVERFLOW));
	CHECK_FALSE(valid_vga_timing(vertical_limit));

	vga_timing invalid = standard;
	invalid.h_active = 0;
	vga_timing fallback = standard;
	fallback.h_active = 320;
	validated_vga_timing validated = validate_vga_timing(invalid, fallback);
	CHECK_FALSE(validated.requested_valid);
	CHECK(validated.used_fallback);
	CHECK(vga_timing_equal(validated.effective, fallback));
	CHECK(validated.endpoints.h_total == 480);
	CHECK_FALSE(has_timing_error(validated.errors, timing_error::FALLBACK_INVALID));

	fallback.v_sync_width = 0;
	validated = validate_vga_timing(invalid, fallback);
	CHECK(validated.used_fallback);
	CHECK(vga_timing_equal(validated.effective, standard));
	CHECK(has_timing_error(validated.errors, timing_error::FALLBACK_INVALID));

	validated = validate_vga_timing(standard);
	CHECK(validated.requested_valid);
	CHECK_FALSE(validated.used_fallback);
	vga_timing_sample sample = sample_vga_timing(validated, 0, 0);
	CHECK(sample.in_range);
	CHECK(sample.active);
	CHECK_FALSE(sample.blank);
	CHECK_FALSE(sample.hsync_asserted);
	CHECK_FALSE(sample.vsync_asserted);
	sample = sample_vga_timing(validated, 647, 504);
	CHECK(sample.in_range);
	CHECK_FALSE(sample.active);
	CHECK(sample.blank);
	CHECK(sample.hsync_asserted);
	CHECK(sample.vsync_asserted);
	sample = sample_vga_timing(validated, 745, 506);
	CHECK(sample.in_range);
	CHECK_FALSE(sample.hsync_asserted);
	CHECK_FALSE(sample.vsync_asserted);
	sample = sample_vga_timing(validated, 800, 525);
	CHECK_FALSE(sample.in_range);
	CHECK(sample.blank);
}


TEST_CASE("CoCoVGA border status overlay handles boot, mode, quiet, and restore", "[cocovga][artifact][overlay]")
{
	border_status_overlay overlay;
	border_status_view view = overlay.view();
	CHECK(view.kind == border_status_kind::BOOT_VERSION);
	CHECK(view.remaining == BOOT_STATUS_PIXEL_CLOCKS);
	CHECK(view.visible);
	CHECK_FALSE(view.suppressed);

	pixel_input input;
	CHECK_FALSE(overlay.observe_mode(input));
	overlay.advance(BOOT_STATUS_PIXEL_CLOCKS);
	view = overlay.view();
	CHECK(view.kind == border_status_kind::MODE);
	CHECK(view.remaining == MODE_STATUS_PIXEL_CLOCKS);
	CHECK_FALSE(view.graphics);
	CHECK(view.gm == 0);
	overlay.advance(MODE_STATUS_PIXEL_CLOCKS);
	CHECK(overlay.view().kind == border_status_kind::NONE);
	input.graphics = true;
	input.gm = 6;
	CHECK(overlay.observe_mode(input));
	CHECK_FALSE(overlay.observe_mode(input));
	CHECK(overlay.state().mode_remaining == MODE_STATUS_PIXEL_CLOCKS);

	view = overlay.view();
	CHECK(view.kind == border_status_kind::MODE);
	CHECK(view.remaining == MODE_STATUS_PIXEL_CLOCKS);
	CHECK(view.graphics);
	CHECK(view.gm == 6);

	border_status_state const saved = overlay.state();
	overlay.set_quiet(true);
	view = overlay.view();
	CHECK_FALSE(view.visible);
	CHECK_FALSE(view.suppressed);
	CHECK(view.kind == border_status_kind::NONE);

	overlay.advance(MODE_STATUS_PIXEL_CLOCKS);
	CHECK(overlay.view().kind == border_status_kind::NONE);
	overlay.restore_state(saved);
	CHECK(overlay.view().kind == border_status_kind::MODE);

	core control;
	control.state().buttons.quiet_status_toggle = true;
	overlay.set_quiet(control, input);
	CHECK(overlay.state().quiet);
	overlay.reset(false);
	CHECK_FALSE(overlay.state().quiet);
	CHECK(overlay.view().kind == border_status_kind::BOOT_VERSION);

	overlay.reset(true);
	overlay.observe_mode(input);
	overlay.advance(BOOT_STATUS_PIXEL_CLOCKS + MODE_STATUS_PIXEL_CLOCKS);
	CHECK(overlay.view().kind == border_status_kind::BOOT_VERSION);
	CHECK_FALSE(overlay.view().visible);
	CHECK(overlay.view().remaining == BOOT_STATUS_PIXEL_CLOCKS);
	overlay.set_quiet(false);
	CHECK(overlay.view().kind == border_status_kind::BOOT_VERSION);
	CHECK(overlay.view().visible);
	overlay.advance(BOOT_STATUS_PIXEL_CLOCKS);
	CHECK(overlay.view().kind == border_status_kind::MODE);

	overlay.reset(false);
	overlay.observe_mode(input);
	overlay.advance(1);
	overlay.set_quiet(true);
	CHECK(overlay.view().kind == border_status_kind::NONE);
	overlay.set_quiet(false);
	CHECK(overlay.view().kind == border_status_kind::NONE);
	pixel_input changed_input;
	overlay.observe_mode(changed_input);
	CHECK(overlay.view().kind == border_status_kind::MODE);
}


TEST_CASE("CoCoVGA artifact status names cover supported and unknown values", "[cocovga][artifact][errors]")
{
	CHECK(std::string(status_name(effect_status::OK)) == "ok");
	CHECK(std::string(status_name(effect_status::INVALID_SOURCE)) == "invalid source");
	CHECK(std::string(status_name(effect_status::INVALID_DESTINATION)) == "invalid destination");
	CHECK(std::string(status_name(effect_status::INVALID_OUTPUT_POSITION)) == "invalid output position");
	CHECK(std::string(status_name(static_cast<effect_status>(0xff))) == "unknown");
}
