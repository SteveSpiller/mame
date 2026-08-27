// license:BSD-3-Clause
// copyright-holders:Nathan Woods,Stephen Spiller

#include "emu.h"

#include "cocovga_artifact.h"

#include <algorithm>
#include <array>

namespace cocovga
{

namespace render_effects
{

namespace
{

constexpr std::array<std::uint8_t, 4> SIMPLE_ARTIFACT =
{
	0, 9, 10, 15
};

// MESS-compatible six-tap truth table regenerated from the pinned CoCoVGA
// oracle vectors.  Its orientation matches the oracle's swap=1 rows; swap=0
// applies the documented pair exchange noted by the oracle.
constexpr std::array<std::uint8_t, 128> MESS_ARTIFACT =
{
	0,  0,       0,  0,      0,  6,      0,  2,
	5,  7,       5,  7,      1,  3,      1, 11,
	8,  6,       8, 14,      8,  9,      8,  9,
	4,  4,       4, 15,     12, 12,     12, 15,

	5, 13,       5, 13,     13,  0,     13,  2,
	10, 10,     10, 10,     10, 15,     10, 11,
	3,  1,       3,  1,     15,  9,     15,  9,
	11, 11,     11, 11,     15, 15,     15, 15,

	14,  0,     14,  0,     14,  6,     14,  2,
	0,  7,       0,  7,      1,  3,      1, 11,
	9,  6,       9, 14,      9,  9,      9,  9,
	15,  4,     15, 15,     12, 12,     12, 15,

	2, 13,       2, 13,      2,  0,      2,  2,
	10, 10,     10, 10,     10, 15,     10, 11,
	12,  1,     12,  1,     12,  9,     12,  9,
	15, 11,     15, 11,     15, 15,     15, 15
};

// Flat smArtifact truth table generated from the pinned oracle vectors.
// Entries are pattern-major, with four output phases per six-bit pattern.
constexpr std::array<std::uint8_t, 256> SMARTIFACT_SWAP0 =
{
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	 0,  0,  8,  8,  0,  0,  8,  9,  0,  0, 15, 15,  0,  0, 15, 15,
	 7,  7,  0,  0,  7,  7,  0,  0,  7, 10, 10, 10,  7, 10, 10, 10,
	15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	 9,  9,  9,  8,  9,  9,  8,  9,  9,  9,  9, 15,  9,  9,  9, 15,
	15, 15,  0,  0, 15, 15,  0,  0, 15, 10, 10, 10, 15, 10, 10, 10,
	15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	 0,  0,  8,  8,  0,  0,  8,  9,  0,  0, 15, 15,  0,  0, 15, 15,
	 7, 10,  0,  0,  7, 10,  0,  0,  7, 10, 10, 10,  7, 10, 10, 10,
	15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	 9,  9,  9,  8,  9,  9,  8,  9,  9,  9,  9, 15,  9,  9,  9, 15,
	15, 10, 10,  0, 15, 10, 10,  0, 15, 10, 10, 10, 15, 10, 10, 10,
	15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15
};

constexpr std::uint8_t artifact_base(bool css) noexcept
{
	return css ? 0 : 16;
}

constexpr std::uint8_t swap_color_pair(std::uint8_t index) noexcept
{
	if (index >= 1 && index <= 13 && (index & 1))
		return std::uint8_t(index + 1);
	if (index >= 2 && index <= 14 && !(index & 1))
		return std::uint8_t(index - 1);
	return index;
}

constexpr std::uint8_t absolute_artifact_index(bool css, std::uint8_t relative) noexcept
{
	return std::uint8_t(artifact_base(css) + (relative & 0x0f));
}

bool source_bit(std::uint8_t const *source, std::ptrdiff_t position) noexcept
{
	if (position < 0 || position >= std::ptrdiff_t(RG6_SOURCE_PIXELS))
		return false;

	std::size_t const bit = static_cast<std::size_t>(position);
	return BIT(source[bit / 8], 7 - (bit % 8)) != 0;
}

std::uint8_t pair_pattern(std::uint8_t const *source, std::size_t pair_start) noexcept
{
	return std::uint8_t(
			(source_bit(source, std::ptrdiff_t(pair_start)) ? 0x02 : 0x00) |
			(source_bit(source, std::ptrdiff_t(pair_start + 1)) ? 0x01 : 0x00));
}

std::uint8_t six_tap_pattern(std::uint8_t const *source, std::size_t pair_start) noexcept
{
	std::uint8_t result = 0;
	for (int offset = -2; offset <= 3; ++offset)
	{
		result = std::uint8_t(result << 1);
		if (source_bit(source, std::ptrdiff_t(pair_start) + offset))
			result |= 0x01;
	}
	return result;
}

bool valid_artifact_mode(artifact_mode mode) noexcept
{
	return mode == artifact_mode::STANDARD ||
			mode == artifact_mode::FAT_BITS ||
			mode == artifact_mode::SMARTIFACT ||
			mode == artifact_mode::MESS ||
			mode == artifact_mode::MONOCHROME;
}

std::uint8_t expand_component(std::uint8_t component, std::uint8_t bits) noexcept
{
	if (bits == 3)
	{
		std::uint8_t const value = BIT(component, 2, 3);
		return std::uint8_t((value << 5) | (value << 2) | (value >> 1));
	}

	std::uint8_t const value = BIT(component, 0, 5);
	return std::uint8_t((value << 3) | (value >> 2));
}

resolved_color make_color(
		palette_output const &palette,
		color_source source,
		std::uint8_t index,
		bool valid = true) noexcept
{
	resolved_color result;
	result.palette = palette;
	result.argb = pack_color(palette);
	result.source = source;
	result.index = index;
	result.valid = valid && valid_palette_output(palette);
	if (!result.valid)
		result.argb = OPAQUE_BLACK;
	return result;
}

resolved_color default_semigraphics_color(core const &control, std::uint8_t index) noexcept
{
	if (index >= SEMIGRAPHICS_PALETTE_ENTRIES)
		return fixed_black();
	return make_color(
			control.palette_color({ palette_source::SEMIGRAPHICS, index, true }),
			color_source::SEMIGRAPHICS_DEFAULT,
			index);
}

resolved_color reference_color(core const &control, palette_reference reference) noexcept
{
	color_source source = color_source::FIXED;
	switch (reference.source)
	{
	case palette_source::SEMIGRAPHICS:
		source = reference.use_default
				? color_source::SEMIGRAPHICS_DEFAULT
				: color_source::SEMIGRAPHICS;
		break;

	case palette_source::EXTRA:
		source = color_source::EXTRA;
		break;
	}

	return make_color(
			control.palette_color(reference),
			source,
			reference.index);
}

constexpr bool alpha_family(mode_kind mode) noexcept
{
	return mode == mode_kind::ALPHA_SEMIGRAPHICS || mode == mode_kind::W64;
}

constexpr bool color_graphics_family(mode_kind mode) noexcept
{
	return mode == mode_kind::CG1 ||
			mode == mode_kind::CG2 ||
			mode == mode_kind::CG3 ||
			mode == mode_kind::CG6;
}

constexpr bool resolution_graphics_family(mode_kind mode) noexcept
{
	return mode == mode_kind::RG1 ||
			mode == mode_kind::RG2 ||
			mode == mode_kind::RG3 ||
			mode == mode_kind::RG6;
}

vga_timing_endpoints safe_endpoints(vga_timing const &timing) noexcept
{
	vga_timing_endpoints result;
	result.h_active_end = timing.h_active;
	result.h_front_end = std::uint16_t(std::uint32_t(result.h_active_end) + timing.h_front_porch);
	result.h_sync_end = std::uint16_t(std::uint32_t(result.h_front_end) + timing.h_sync_width);
	result.h_total = std::uint16_t(std::uint32_t(result.h_sync_end) + timing.h_back_porch);
	result.v_active_end = timing.v_active;
	result.v_front_end = std::uint16_t(std::uint32_t(result.v_active_end) + timing.v_front_porch);
	result.v_sync_end = std::uint16_t(std::uint32_t(result.v_front_end) + timing.v_sync_width);
	result.v_total = std::uint16_t(std::uint32_t(result.v_sync_end) + timing.v_back_porch);
	return result;
}

void subtract_saturating(std::uint64_t &value, std::uint64_t amount) noexcept
{
	value = amount >= value ? 0 : value - amount;
}

} // anonymous namespace

std::uint8_t monochrome_palette_index(bool css, bool pixel) noexcept
{
	return absolute_artifact_index(css, pixel ? 15 : 0);
}

std::uint8_t simple_artifact_palette_index(bool css, bool swap, std::uint8_t pattern) noexcept
{
	std::uint8_t relative = SIMPLE_ARTIFACT[pattern & 0x03];
	if (swap)
		relative = swap_color_pair(relative);
	return absolute_artifact_index(css, relative);
}

std::uint8_t standard_artifact_palette_index(
		bool css,
		bool swap,
		std::uint8_t pattern,
		std::uint8_t source_bit_position) noexcept
{
	pattern &= 0x03;
	std::uint8_t const position = source_bit_position & 0x01;
	bool const pixel = BIT(pattern, 1 - position) != 0;
	return pixel
			? simple_artifact_palette_index(css, swap, pattern)
			: monochrome_palette_index(css, false);
}

std::uint8_t fat_bits_artifact_palette_index(bool css, bool swap, std::uint8_t pattern) noexcept
{
	return simple_artifact_palette_index(css, swap, pattern);
}

std::uint8_t mess_artifact_palette_index(
		bool css,
		bool swap,
		std::uint8_t pattern,
		std::uint8_t phase) noexcept
{
	std::size_t const offset = std::size_t(pattern & 0x3f) * 2 + (phase & 0x01);
	std::uint8_t relative = MESS_ARTIFACT[offset];
	if (!swap)
		relative = swap_color_pair(relative);
	return absolute_artifact_index(css, relative);
}

std::uint8_t smartifact_palette_index(
		bool css,
		bool swap,
		std::uint8_t pattern,
		std::uint8_t phase) noexcept
{
	std::size_t const offset = std::size_t(pattern & 0x3f) * 4 + (phase & 0x03);
	std::uint8_t relative = SMARTIFACT_SWAP0[offset];
	if (swap)
		relative = swap_color_pair(relative);
	return absolute_artifact_index(css, relative);
}

bool rg6_palette_index(
		artifact_controls const &settings,
		bool css,
		std::uint8_t const *source,
		std::size_t source_bytes,
		std::size_t output_position,
		std::uint8_t &palette_index) noexcept
{
	palette_index = monochrome_palette_index(css, false);
	if (source == nullptr || source_bytes < RG6_SOURCE_BYTES || output_position >= RG6_OUTPUT_PIXELS)
		return false;

	std::size_t const logical_position = output_position / 2;
	bool const pixel = source_bit(source, std::ptrdiff_t(logical_position));
	artifact_mode const mode =
			settings.color_enabled && valid_artifact_mode(settings.mode)
					? settings.mode
					: artifact_mode::MONOCHROME;

	if (mode == artifact_mode::MONOCHROME)
	{
		palette_index = monochrome_palette_index(css, pixel);
		return true;
	}

	std::size_t const pair_start = logical_position & ~std::size_t(1);
	std::uint8_t const pair = pair_pattern(source, pair_start);

	switch (mode)
	{
	case artifact_mode::STANDARD:
		palette_index = standard_artifact_palette_index(
				css,
				settings.swap,
				pair,
				std::uint8_t(logical_position & 1));
		break;

	case artifact_mode::FAT_BITS:
		palette_index = fat_bits_artifact_palette_index(css, settings.swap, pair);
		break;

	case artifact_mode::SMARTIFACT:
		palette_index = smartifact_palette_index(
				css,
				settings.swap,
				six_tap_pattern(source, pair_start),
				std::uint8_t(((logical_position & 1) * 2) + (output_position & 1)));
		break;

	case artifact_mode::MESS:
		palette_index = mess_artifact_palette_index(
				css,
				settings.swap,
				six_tap_pattern(source, pair_start),
				std::uint8_t(logical_position & 1));
		break;

	default:
		palette_index = monochrome_palette_index(css, pixel);
		break;
	}

	return true;
}

effect_status render_rg6_palette_indices(
		artifact_controls const &settings,
		bool css,
		std::uint8_t const *source,
		std::size_t source_bytes,
		std::uint8_t *palette_indices,
		std::size_t palette_index_count) noexcept
{
	if (source == nullptr || source_bytes < RG6_SOURCE_BYTES)
		return effect_status::INVALID_SOURCE;
	if (palette_indices == nullptr || palette_index_count < RG6_OUTPUT_PIXELS)
		return effect_status::INVALID_DESTINATION;

	for (std::size_t position = 0; position < RG6_OUTPUT_PIXELS; ++position)
	{
		if (!rg6_palette_index(
					settings,
					css,
					source,
					source_bytes,
					position,
					palette_indices[position]))
		{
			return effect_status::INVALID_OUTPUT_POSITION;
		}
	}
	return effect_status::OK;
}

bool rg6_pixel(
		core const &control,
		pixel_input const &input,
		std::uint8_t const *source,
		std::size_t source_bytes,
		std::size_t output_position,
		effect_pixel &pixel) noexcept
{
	return rg6_pixel(
			control,
			control.controls(input).artifact,
			input.css,
			source,
			source_bytes,
			output_position,
			pixel);
}

bool rg6_pixel(
		core const &control,
		artifact_controls const &settings,
		bool css,
		std::uint8_t const *source,
		std::size_t source_bytes,
		std::size_t output_position,
		effect_pixel &pixel) noexcept
{
	std::uint8_t index = 0;
	if (!rg6_palette_index(
				settings,
				css,
				source,
				source_bytes,
				output_position,
				index))
	{
		pixel = OPAQUE_BLACK;
		return false;
	}

	pixel = artifact_color(control, index).argb;
	return true;
}

effect_status render_rg6_pixels(
		core const &control,
		pixel_input const &input,
		std::uint8_t const *source,
		std::size_t source_bytes,
		effect_pixel *pixels,
		std::size_t pixel_count) noexcept
{
	if (source == nullptr || source_bytes < RG6_SOURCE_BYTES)
		return effect_status::INVALID_SOURCE;
	if (pixels == nullptr || pixel_count < RG6_OUTPUT_PIXELS)
		return effect_status::INVALID_DESTINATION;

	std::array<std::uint8_t, RG6_OUTPUT_PIXELS> indices{};
	effect_status const status = render_rg6_palette_indices(
			control.controls(input).artifact,
			input.css,
			source,
			source_bytes,
			indices.data(),
			indices.size());
	if (status != effect_status::OK)
		return status;

	for (std::size_t position = 0; position < RG6_OUTPUT_PIXELS; ++position)
		pixels[position] = artifact_color(control, indices[position]).argb;
	return effect_status::OK;
}

char const *status_name(effect_status status) noexcept
{
	switch (status)
	{
	case effect_status::OK:
		return "ok";
	case effect_status::INVALID_SOURCE:
		return "invalid source";
	case effect_status::INVALID_DESTINATION:
		return "invalid destination";
	case effect_status::INVALID_OUTPUT_POSITION:
		return "invalid output position";
	}

	return "unknown";
}

bool valid_palette_output(palette_output const &color) noexcept
{
	return (color.bits_per_component == 3 || color.bits_per_component == 5) &&
			color.color.red <= 31 &&
			color.color.green <= 31 &&
			color.color.blue <= 31;
}

effect_pixel pack_color(palette_output const &color, effect_pixel invalid_fallback) noexcept
{
	if (!valid_palette_output(color))
		return invalid_fallback;

	std::uint32_t const red = expand_component(color.color.red, color.bits_per_component);
	std::uint32_t const green = expand_component(color.color.green, color.bits_per_component);
	std::uint32_t const blue = expand_component(color.color.blue, color.bits_per_component);
	return 0xff00'0000U | (red << 16) | (green << 8) | blue;
}

effect_pixel darken_pixel(effect_pixel pixel, std::uint8_t amount) noexcept
{
	std::uint32_t const factor = 255U - amount;
	auto const darken = [factor] (std::uint32_t component)
	{
		return (component * factor + 127U) / 255U;
	};

	return (pixel & 0xff00'0000U) |
			(darken(BIT(pixel, 16, 8)) << 16) |
			(darken(BIT(pixel, 8, 8)) << 8) |
			darken(BIT(pixel, 0, 8));
}

resolved_color fixed_black() noexcept
{
	palette_output palette;
	palette.bits_per_component = 5;
	return make_color(palette, color_source::FIXED, 0);
}

resolved_color semigraphics_color(
		core const &control,
		std::uint8_t slot,
		bool full_palette) noexcept
{
	bool const valid = slot < SEMIGRAPHICS_PALETTE_ENTRIES;
	std::uint8_t const selected = core::semigraphics_palette_index(slot, full_palette);
	return make_color(
			control.semigraphics_color(slot, full_palette),
			color_source::SEMIGRAPHICS,
			selected,
			valid);
}

resolved_color color_graphics_color(
		core const &control,
		bool css,
		std::uint8_t pixel,
		bool full_palette) noexcept
{
	std::uint8_t const index = core::color_graphics_palette_index(css, pixel);
	return semigraphics_color(control, index, full_palette);
}

resolved_color resolution_graphics_color(core const &control, bool css, bool pixel) noexcept
{
	return artifact_color(control, core::resolution_graphics_palette_index(css, pixel));
}

resolved_color artifact_color(core const &control, std::uint8_t palette_index) noexcept
{
	bool const valid = palette_index < ARTIFACT_PALETTE_ENTRIES;
	return make_color(
			control.artifact_color(palette_index),
			color_source::ARTIFACT,
			valid ? palette_index : 0,
			valid);
}

text_color_pair text_colors(core const &control, pixel_input const &input) noexcept
{
	text_palette_descriptor const descriptor = control.text_palette(input);
	return
	{
		reference_color(control, descriptor.foreground),
		reference_color(control, descriptor.background)
	};
}

resolved_color background_color(
		core const &control,
		mode_kind mode,
		pixel_input const &input) noexcept
{
	if (alpha_family(mode))
	{
		return input.alpha_semigraphics
				? semigraphics_color(control, 0, control.controls(input).extras.full_semigraphics_palette)
				: text_colors(control, input).background;
	}
	if (color_graphics_family(mode))
	{
		return color_graphics_color(
				control,
				input.css,
				0,
				control.controls(input).extras.full_semigraphics_palette);
	}
	if (resolution_graphics_family(mode))
		return resolution_graphics_color(control, input.css, false);
	if (mode == mode_kind::VG6)
		return artifact_color(control, core::vg6_palette_index(input.css, 0));
	return fixed_black();
}

border_color_result border_color(
		core const &control,
		mode_kind mode,
		pixel_input const &input) noexcept
{
	border_color_result result;
	result.requested = control.controls(input).extras.border;
	result.effective = result.requested;

	// The pinned pixel path forces VG6 borders to black for every selector.
	if (mode == mode_kind::VG6)
	{
		result.color = fixed_black();
		result.effective = border_mode::BLACK;
		result.mode_forced_black = true;
		return result;
	}

	switch (result.requested)
	{
	case border_mode::BLACK:
		result.color = fixed_black();
		break;

	case border_mode::CUSTOM:
		result.color = make_color(
				control.extra_color(extra_palette_slot::BORDER),
				color_source::EXTRA,
				static_cast<std::uint8_t>(extra_palette_slot::BORDER));
		break;

	case border_mode::T1:
		// The pinned CoCoVGA pixel path generates but does not consume T1 select.
		result.t1_fallback = true;
		result.effective = border_mode::STANDARD;
		[[fallthrough]];

	case border_mode::STANDARD:
	default:
		result.color = alpha_family(mode)
				? default_semigraphics_color(control, 0)
				: default_semigraphics_color(control, input.css ? 5 : 1);
		break;
	}

	return result;
}

resolved_color scanline_color(core const &control) noexcept
{
	return make_color(
			control.extra_color(extra_palette_slot::SCANLINE),
			color_source::EXTRA,
			static_cast<std::uint8_t>(extra_palette_slot::SCANLINE));
}

bool scanline_row(bool enabled, std::uint32_t active_row) noexcept
{
	return enabled && (active_row & 1U) != 0;
}

effect_pixel scanline_pixel(
		core const &control,
		pixel_input const &input,
		std::uint32_t active_row,
		effect_pixel source_pixel) noexcept
{
	return scanline_row(control.controls(input).extras.scanlines, active_row)
			? scanline_color(control).argb
			: source_pixel;
}

effect_status apply_scanline(
		core const &control,
		pixel_input const &input,
		std::uint32_t active_row,
		effect_pixel *pixels,
		std::size_t pixel_count) noexcept
{
	if (pixels == nullptr)
		return effect_status::INVALID_DESTINATION;
	if (!scanline_row(control.controls(input).extras.scanlines, active_row))
		return effect_status::OK;

	std::fill_n(pixels, pixel_count, scanline_color(control).argb);
	return effect_status::OK;
}

timing_error vga_timing_errors(vga_timing const &timing) noexcept
{
	timing_error errors = timing_error::NONE;

	if (timing.h_active == 0)
		errors |= timing_error::H_ACTIVE_ZERO;
	if (timing.h_active > 0x07ff)
		errors |= timing_error::H_ACTIVE_RANGE;
	if (timing.h_front_porch == 0)
		errors |= timing_error::H_FRONT_ZERO;
	if (timing.h_sync_width == 0)
		errors |= timing_error::H_SYNC_ZERO;
	if (timing.h_back_porch == 0)
		errors |= timing_error::H_BACK_ZERO;

	std::uint32_t const horizontal_total =
			std::uint32_t(timing.h_active) +
			timing.h_front_porch +
			timing.h_sync_width +
			timing.h_back_porch;
	if (horizontal_total > VGA_HORIZONTAL_TOTAL_MAX)
		errors |= timing_error::H_TOTAL_OVERFLOW;

	if (timing.v_active == 0)
		errors |= timing_error::V_ACTIVE_ZERO;
	if (timing.v_active > 0x03ff)
		errors |= timing_error::V_ACTIVE_RANGE;
	if (timing.v_front_porch == 0)
		errors |= timing_error::V_FRONT_ZERO;
	if (timing.v_sync_width == 0)
		errors |= timing_error::V_SYNC_ZERO;
	if (timing.v_back_porch == 0)
		errors |= timing_error::V_BACK_ZERO;

	std::uint32_t const vertical_total =
			std::uint32_t(timing.v_active) +
			timing.v_front_porch +
			timing.v_sync_width +
			timing.v_back_porch;
	if (vertical_total > VGA_VERTICAL_TOTAL_MAX)
		errors |= timing_error::V_TOTAL_OVERFLOW;

	return errors;
}

bool valid_vga_timing(vga_timing const &timing) noexcept
{
	return vga_timing_errors(timing) == timing_error::NONE;
}

validated_vga_timing validate_vga_timing(
		vga_timing const &requested,
		vga_timing const &fallback) noexcept
{
	validated_vga_timing result;
	result.requested = requested;
	result.errors = vga_timing_errors(requested);
	result.requested_valid = result.errors == timing_error::NONE;
	result.used_fallback = !result.requested_valid;

	if (result.requested_valid)
	{
		result.effective = requested;
	}
	else if (valid_vga_timing(fallback))
	{
		result.effective = fallback;
	}
	else
	{
		result.effective = vga_timing{};
		result.errors |= timing_error::FALLBACK_INVALID;
	}

	result.endpoints = safe_endpoints(result.effective);
	return result;
}

vga_timing_sample sample_vga_timing(
		validated_vga_timing const &timing,
		std::uint32_t horizontal,
		std::uint32_t vertical) noexcept
{
	vga_timing_sample result;
	vga_timing_endpoints const &endpoints = timing.endpoints;
	result.in_range = horizontal < endpoints.h_total && vertical < endpoints.v_total;
	result.active =
			result.in_range &&
			horizontal < endpoints.h_active_end &&
			vertical < endpoints.v_active_end;
	result.blank = !result.active;
	result.hsync_asserted =
			result.in_range &&
			horizontal >= endpoints.h_front_end &&
			horizontal < endpoints.h_sync_end;
	result.vsync_asserted =
			result.in_range &&
			vertical >= endpoints.v_front_end &&
			vertical < endpoints.v_sync_end;
	return result;
}

void border_status_overlay::reset(bool quiet) noexcept
{
	m_state = border_status_state{};
	m_state.quiet = quiet;
}

void border_status_overlay::set_quiet(bool quiet) noexcept
{
	if (quiet && !m_state.quiet)
	{
		// Hardware holds in INIT while quiet is already asserted at boot.  Once
		// either timed display has started, asserting quiet moves directly to
		// DONE and releasing it must not resume the interrupted display.
		if (m_state.boot_remaining != BOOT_STATUS_PIXEL_CLOCKS)
			m_state.boot_remaining = 0;
		m_state.mode_remaining = 0;
	}
	m_state.quiet = quiet;
}

void border_status_overlay::set_quiet(core const &control, pixel_input const &input) noexcept
{
	set_quiet(control.controls(input).extras.quiet_status);
}

bool border_status_overlay::observe_mode(pixel_input const &input) noexcept
{
	bool const graphics = input.graphics;
	std::uint8_t const gm = input.gm & 0x07;

	if (!m_state.have_mode)
	{
		m_state.have_mode = true;
		m_state.graphics = graphics;
		m_state.gm = gm;
		return false;
	}

	if (m_state.graphics == graphics && m_state.gm == gm)
		return false;

	m_state.graphics = graphics;
	m_state.gm = gm;
	m_state.mode_remaining = MODE_STATUS_PIXEL_CLOCKS;
	return true;
}

void border_status_overlay::advance(std::uint64_t pixel_clocks) noexcept
{
	if (m_state.quiet)
		return;

	subtract_saturating(m_state.mode_remaining, pixel_clocks);
	bool const boot_was_active = m_state.boot_remaining != 0;
	subtract_saturating(m_state.boot_remaining, pixel_clocks);
	if (boot_was_active &&
			m_state.boot_remaining == 0 &&
			m_state.have_mode &&
			!m_state.quiet)
	{
		m_state.mode_remaining = MODE_STATUS_PIXEL_CLOCKS;
	}
}

border_status_view border_status_overlay::view() const noexcept
{
	border_status_view result;
	result.graphics = m_state.graphics;
	result.gm = m_state.gm;

	if (m_state.boot_remaining != 0)
	{
		result.kind = border_status_kind::BOOT_VERSION;
		result.remaining = m_state.boot_remaining;
	}
	else if (m_state.have_mode && m_state.mode_remaining != 0)
	{
		result.kind = border_status_kind::MODE;
		result.remaining = m_state.mode_remaining;
	}

	result.suppressed = m_state.quiet && result.kind != border_status_kind::NONE;
	result.visible = !m_state.quiet && result.kind != border_status_kind::NONE;
	return result;
}

} // namespace render_effects

} // namespace cocovga
