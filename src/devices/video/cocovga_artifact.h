// license:BSD-3-Clause
// copyright-holders:Nathan Woods,Stephen Spiller

#ifndef MAME_VIDEO_COCOVGA_ARTIFACT_H
#define MAME_VIDEO_COCOVGA_ARTIFACT_H

#pragma once

#include "cocovga_core.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace cocovga
{

namespace render_effects
{

using effect_pixel = std::uint32_t;

constexpr effect_pixel OPAQUE_BLACK = 0xff00'0000U;
constexpr std::size_t RG6_SOURCE_BYTES = 32;
constexpr std::size_t RG6_SOURCE_PIXELS = RG6_SOURCE_BYTES * 8;
constexpr std::size_t RG6_OUTPUT_PIXELS = RG6_SOURCE_PIXELS * 2;

enum class effect_status : std::uint8_t
{
	OK,
	INVALID_SOURCE,
	INVALID_DESTINATION,
	INVALID_OUTPUT_POSITION
};

enum class color_source : std::uint8_t
{
	FIXED,
	SEMIGRAPHICS,
	SEMIGRAPHICS_DEFAULT,
	ARTIFACT,
	EXTRA
};

struct resolved_color
{
	palette_output palette;
	effect_pixel argb = OPAQUE_BLACK;
	color_source source = color_source::FIXED;
	std::uint8_t index = 0;
	bool valid = true;

	bool programmable() const noexcept { return source != color_source::FIXED; }
	bool overridden() const noexcept { return programmable() && palette.overridden; }
	bool using_fallback() const noexcept { return programmable() && !palette.overridden; }
};

struct text_color_pair
{
	resolved_color foreground;
	resolved_color background;
};

struct border_color_result
{
	resolved_color color;
	border_mode requested = border_mode::STANDARD;
	border_mode effective = border_mode::STANDARD;
	bool t1_fallback = false;
	bool mode_forced_black = false;
};

// Patterns are ordered left-to-right, most significant bit first.  MESS
// phases select the two logical pixels; smArtifact phases select their four
// doubled VGA pixels.
std::uint8_t monochrome_palette_index(bool css, bool pixel) noexcept;
std::uint8_t simple_artifact_palette_index(bool css, bool swap, std::uint8_t pattern) noexcept;
std::uint8_t standard_artifact_palette_index(
		bool css,
		bool swap,
		std::uint8_t pattern,
		std::uint8_t source_bit_position) noexcept;
std::uint8_t fat_bits_artifact_palette_index(bool css, bool swap, std::uint8_t pattern) noexcept;
std::uint8_t mess_artifact_palette_index(
		bool css,
		bool swap,
		std::uint8_t pattern,
		std::uint8_t phase) noexcept;
std::uint8_t smartifact_palette_index(
		bool css,
		bool swap,
		std::uint8_t pattern,
		std::uint8_t phase) noexcept;

// RG6 source bytes are MSB-first.  The pixel API safely pads both ends with
// zero bits; scanline APIs write exactly RG6_OUTPUT_PIXELS entries.
bool rg6_palette_index(
		artifact_controls const &settings,
		bool css,
		std::uint8_t const *source,
		std::size_t source_bytes,
		std::size_t output_position,
		std::uint8_t &palette_index) noexcept;

effect_status render_rg6_palette_indices(
		artifact_controls const &settings,
		bool css,
		std::uint8_t const *source,
		std::size_t source_bytes,
		std::uint8_t *palette_indices,
		std::size_t palette_index_count) noexcept;

bool rg6_pixel(
		core const &control,
		pixel_input const &input,
		std::uint8_t const *source,
		std::size_t source_bytes,
		std::size_t output_position,
		effect_pixel &pixel) noexcept;

bool rg6_pixel(
		core const &control,
		artifact_controls const &settings,
		bool css,
		std::uint8_t const *source,
		std::size_t source_bytes,
		std::size_t output_position,
		effect_pixel &pixel) noexcept;

effect_status render_rg6_pixels(
		core const &control,
		pixel_input const &input,
		std::uint8_t const *source,
		std::size_t source_bytes,
		effect_pixel *pixels,
		std::size_t pixel_count) noexcept;

char const *status_name(effect_status status) noexcept;

bool valid_palette_output(palette_output const &color) noexcept;
effect_pixel pack_color(
		palette_output const &color,
		effect_pixel invalid_fallback = OPAQUE_BLACK) noexcept;
effect_pixel darken_pixel(effect_pixel pixel, std::uint8_t amount = 128) noexcept;

resolved_color fixed_black() noexcept;
resolved_color semigraphics_color(
		core const &control,
		std::uint8_t slot,
		bool full_palette = true) noexcept;
resolved_color color_graphics_color(
		core const &control,
		bool css,
		std::uint8_t pixel,
		bool full_palette = true) noexcept;
resolved_color resolution_graphics_color(core const &control, bool css, bool pixel) noexcept;
resolved_color artifact_color(core const &control, std::uint8_t palette_index) noexcept;
text_color_pair text_colors(core const &control, pixel_input const &input) noexcept;
resolved_color background_color(
		core const &control,
		mode_kind mode,
		pixel_input const &input) noexcept;
border_color_result border_color(
		core const &control,
		mode_kind mode,
		pixel_input const &input) noexcept;
resolved_color scanline_color(core const &control) noexcept;

// active_row is relative to the active VGA region, not the CoCo viewport.
bool scanline_row(bool enabled, std::uint32_t active_row) noexcept;
effect_pixel scanline_pixel(
		core const &control,
		pixel_input const &input,
		std::uint32_t active_row,
		effect_pixel source_pixel) noexcept;
effect_status apply_scanline(
		core const &control,
		pixel_input const &input,
		std::uint32_t active_row,
		effect_pixel *pixels,
		std::size_t pixel_count) noexcept;

enum class timing_error : std::uint16_t
{
	NONE = 0,
	H_ACTIVE_ZERO = 1U << 0,
	H_ACTIVE_RANGE = 1U << 1,
	H_FRONT_ZERO = 1U << 2,
	H_SYNC_ZERO = 1U << 3,
	H_BACK_ZERO = 1U << 4,
	H_TOTAL_OVERFLOW = 1U << 5,
	V_ACTIVE_ZERO = 1U << 6,
	V_ACTIVE_RANGE = 1U << 7,
	V_FRONT_ZERO = 1U << 8,
	V_SYNC_ZERO = 1U << 9,
	V_BACK_ZERO = 1U << 10,
	V_TOTAL_OVERFLOW = 1U << 11,
	FALLBACK_INVALID = 1U << 12
};

constexpr timing_error operator|(timing_error left, timing_error right) noexcept
{
	return static_cast<timing_error>(
			static_cast<std::uint16_t>(left) |
			static_cast<std::uint16_t>(right));
}

constexpr timing_error &operator|=(timing_error &left, timing_error right) noexcept
{
	left = left | right;
	return left;
}

constexpr bool has_timing_error(timing_error errors, timing_error error) noexcept
{
	return (static_cast<std::uint16_t>(errors) & static_cast<std::uint16_t>(error)) != 0;
}

struct validated_vga_timing
{
	vga_timing requested;
	vga_timing effective;
	vga_timing_endpoints endpoints;
	timing_error errors = timing_error::NONE;
	bool requested_valid = true;
	bool used_fallback = false;
};

struct vga_timing_sample
{
	bool in_range = false;
	bool active = false;
	bool blank = true;
	bool hsync_asserted = false;
	bool vsync_asserted = false;
};

timing_error vga_timing_errors(vga_timing const &timing) noexcept;
bool valid_vga_timing(vga_timing const &timing) noexcept;
// Invalid requests use a validated caller fallback, then the 640x480 default.
validated_vga_timing validate_vga_timing(
		vga_timing const &requested,
		vga_timing const &fallback = vga_timing{}) noexcept;
vga_timing_sample sample_vga_timing(
		validated_vga_timing const &timing,
		std::uint32_t horizontal,
		std::uint32_t vertical) noexcept;

constexpr std::uint64_t BOOT_STATUS_PIXEL_CLOCKS = 300'000'000;
constexpr std::uint64_t MODE_STATUS_PIXEL_CLOCKS = 150'000'000;

enum class border_status_kind : std::uint8_t
{
	NONE,
	BOOT_VERSION,
	MODE
};

struct border_status_state
{
	std::uint64_t boot_remaining = BOOT_STATUS_PIXEL_CLOCKS;
	std::uint64_t mode_remaining = 0;
	bool quiet = false;
	bool have_mode = false;
	bool graphics = false;
	std::uint8_t gm = 0;
};

struct border_status_view
{
	border_status_kind kind = border_status_kind::NONE;
	std::uint64_t remaining = 0;
	bool visible = false;
	bool suppressed = false;
	bool graphics = false;
	std::uint8_t gm = 0;
};

class border_status_overlay
{
public:
	explicit border_status_overlay(bool quiet = false) noexcept { reset(quiet); }

	void reset(bool quiet = false) noexcept;
	void set_quiet(bool quiet) noexcept;
	void set_quiet(core const &control, pixel_input const &input) noexcept;
	bool observe_mode(pixel_input const &input) noexcept;
	void advance(std::uint64_t pixel_clocks) noexcept;

	border_status_view view() const noexcept;
	border_status_state &state() noexcept { return m_state; }
	border_status_state const &state() const noexcept { return m_state; }
	void restore_state(border_status_state const &saved) noexcept { m_state = saved; }

private:
	border_status_state m_state;
};

static_assert(std::is_trivially_copyable<border_status_state>::value);
static_assert(std::is_standard_layout<border_status_state>::value);

} // namespace render_effects

} // namespace cocovga

#endif // MAME_VIDEO_COCOVGA_ARTIFACT_H
