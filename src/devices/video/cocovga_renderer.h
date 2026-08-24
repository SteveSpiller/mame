// license:BSD-3-Clause
// copyright-holders:Stephen Spiller

#ifndef MAME_VIDEO_COCOVGA_RENDERER_H
#define MAME_VIDEO_COCOVGA_RENDERER_H

#pragma once

#include "cocovga_artifact.h"
#include "cocovga_capture.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace cocovga
{

class character_generator;

constexpr std::uint16_t RENDER_WIDTH = 640;
constexpr std::uint16_t RENDER_HEIGHT = 480;
constexpr std::uint16_t VIEWPORT_LEFT = 65;
constexpr std::uint16_t VIEWPORT_TOP = 48;
constexpr std::uint16_t VIEWPORT_WIDTH = 512;
constexpr std::uint16_t VIEWPORT_HEIGHT = 384;
constexpr std::uint16_t VIEWPORT_RIGHT = VIEWPORT_LEFT + VIEWPORT_WIDTH - 1;
constexpr std::uint16_t VIEWPORT_BOTTOM = VIEWPORT_TOP + VIEWPORT_HEIGHT - 1;
constexpr std::uint16_t VIEWPORT_RIGHT_EXCLUSIVE = VIEWPORT_RIGHT + 1;
constexpr std::uint16_t VIEWPORT_BOTTOM_EXCLUSIVE = VIEWPORT_BOTTOM + 1;

// Pixels are opaque 0xAARRGGBB values.
using render_pixel = std::uint32_t;

struct pixel_scanline
{
	render_pixel *pixels = nullptr;
	std::size_t length = 0;
};

struct pixel_surface
{
	render_pixel *pixels = nullptr;
	// Distance between row starts, measured in render_pixel elements.
	std::size_t row_stride = 0;
	std::uint16_t width = 0;
	std::uint16_t height = 0;

	pixel_scanline scanline(std::uint16_t row) const noexcept
	{
		return pixels != nullptr && row < height
				? pixel_scanline{ pixels + std::size_t(row) * row_stride, width }
				: pixel_scanline{};
	}
};

enum class render_status : std::uint8_t
{
	OK,
	INVALID_SURFACE,
	FRAME_UNAVAILABLE,
	FRAME_INCOMPLETE,
	INCONSISTENT_CAPTURE
};

struct render_options
{
	render_effects::border_status_view border_status;
};

struct render_result
{
	render_status status = render_status::OK;
	std::uint64_t generation = 0;
	std::uint32_t pixels_written = 0;
	std::uint16_t scanlines_written = 0;
	std::uint16_t mode_changes = 0;
	mode_kind first_mode = mode_kind::ALPHA_SEMIGRAPHICS;
	mode_kind last_mode = mode_kind::ALPHA_SEMIGRAPHICS;

	bool success() const noexcept { return status == render_status::OK; }
};

struct renderer_state
{
	std::uint64_t render_calls = 0;
	std::uint64_t frames_rendered = 0;
	std::uint64_t scanlines_rendered = 0;
	std::uint64_t last_generation = 0;
	std::uint32_t last_pixels_written = 0;
	std::uint16_t last_mode_changes = 0;
	render_status last_status = render_status::OK;
	mode_kind last_first_mode = mode_kind::ALPHA_SEMIGRAPHICS;
	mode_kind last_last_mode = mode_kind::ALPHA_SEMIGRAPHICS;
};

class renderer
{
public:
	renderer() noexcept = default;

	// The character generator is owned by the device and holds the decoded
	// contents of CoCoVGA's character generator ROM regions.
	void set_character_generator(character_generator const *font) noexcept { m_font = font; }

	void reset() noexcept { m_state = renderer_state{}; }
	renderer_state &state() noexcept { return m_state; }
	renderer_state const &state() const noexcept { return m_state; }
	void restore_state(renderer_state const &saved) noexcept { m_state = saved; }

	render_result render(
			core const &control,
			frame_buffer_state const &frame,
			pixel_surface const &surface,
			render_options const &options = {}) noexcept;

	render_result preflight(
			core const &control,
			frame_buffer_state const &frame) const noexcept;

	// Rendering is transactional: presented is updated only after staging has
	// received a complete successful frame.
	render_result render_preserving(
			core const &control,
			frame_buffer_state const &frame,
			pixel_surface const &presented,
			pixel_surface const &staging,
			render_options const &options = {}) noexcept;

	render_result render_scanline(
			core const &control,
			frame_buffer_state const &frame,
			std::uint16_t output_row,
			pixel_scanline const &scanline,
			render_options const &options = {}) noexcept;

	static render_pixel pack_color(palette_output const &color) noexcept;
	static char const *status_name(render_status status) noexcept;

private:
	character_generator const &font() const noexcept;

	render_status preflight_native(
			frame_buffer_state const &frame) const noexcept;

	void render_native_scanline(
			core const &control,
			frame_buffer_state const &frame,
			std::uint16_t output_row,
			render_pixel *pixels) const noexcept;

	void apply_output_effects(
			core const &control,
			frame_buffer_state const &frame,
			std::uint16_t output_row,
			render_pixel *pixels,
			render_options const &options,
			bool scanlines_active) const noexcept;

	render_result make_result(
			frame_buffer_state const &frame,
			render_status status,
			std::uint16_t scanlines,
			std::uint32_t pixels) const noexcept;

	void record_result(render_result const &result, bool full_frame) noexcept;

	character_generator const *m_font = nullptr;
	renderer_state m_state;
};

static_assert(std::is_trivially_copyable<renderer_state>::value);
static_assert(std::is_standard_layout<renderer_state>::value);

} // namespace cocovga

#endif // MAME_VIDEO_COCOVGA_RENDERER_H
