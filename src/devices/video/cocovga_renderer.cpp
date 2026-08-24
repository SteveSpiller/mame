// license:BSD-3-Clause
// copyright-holders:Stephen Spiller

#include "emu.h"

#include "cocovga_extended.h"
#include "cocovga_renderer.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace cocovga
{

namespace
{

constexpr render_pixel OPAQUE_BLACK = 0xff00'0000U;

bool valid_surface(pixel_surface const &surface) noexcept
{
	return surface.pixels != nullptr &&
			surface.width == RENDER_WIDTH &&
			surface.height == RENDER_HEIGHT &&
			surface.row_stride >= RENDER_WIDTH;
}

constexpr bool valid_mode_id(std::uint8_t mode) noexcept
{
	return mode <= static_cast<std::uint8_t>(mode_kind::W64);
}

constexpr mode_kind mode_from_id(std::uint8_t mode) noexcept
{
	return valid_mode_id(mode)
			? static_cast<mode_kind>(mode)
			: mode_kind::ALPHA_SEMIGRAPHICS;
}

constexpr bool native_graphics_mode(mode_kind mode) noexcept
{
	return mode >= mode_kind::CG1 && mode <= mode_kind::RG6;
}

constexpr bool native_mode(mode_kind mode) noexcept
{
	return mode <= mode_kind::RG6;
}

render_pixel border_pixel(core const &control, captured_cell const &cell) noexcept
{
	return render_effects::border_color(control, cell.mode(), cell.pixel()).color.argb;
}

struct character_colors
{
	render_pixel foreground = OPAQUE_BLACK;
	render_pixel background = OPAQUE_BLACK;
};

character_colors colors_for_character(
		core const &control,
		captured_cell const &cell,
		character_glyph glyph) noexcept
{
	pixel_input const input = cell.pixel();
	effective_controls const settings = control.controls(input);
	character_colors result;

	switch (glyph)
	{
	case character_glyph::SEMIGRAPHICS4:
		result.foreground = render_effects::semigraphics_color(
				control,
				core::semigraphics_glyph_palette_index(
						semigraphics_glyph::SG4,
						input.css,
						cell.stream_byte() >> 4),
				settings.extras.full_semigraphics_palette).argb;
		result.background = render_effects::semigraphics_color(
				control,
				0,
				settings.extras.full_semigraphics_palette).argb;
		break;

	case character_glyph::SEMIGRAPHICS6:
		result.foreground = render_effects::semigraphics_color(
				control,
				core::semigraphics_glyph_palette_index(
						semigraphics_glyph::SG6,
						input.css,
						cell.stream_byte() >> 6),
				settings.extras.full_semigraphics_palette).argb;
		result.background = render_effects::semigraphics_color(
				control,
				0,
				settings.extras.full_semigraphics_palette).argb;
		break;

	case character_glyph::TEXT:
		{
			render_effects::text_color_pair const text =
					render_effects::text_colors(control, input);
			result.foreground = text.foreground.argb;
			result.background = text.background.argb;
		}
		break;

	case character_glyph::NONE:
	default:
		break;
	}

	return result;
}

void render_alpha_cell(
		character_generator const &font,
		core const &control,
		captured_cell const &cell,
		std::uint16_t source_row,
		render_pixel *pixels) noexcept
{
	character_row_result const character = extended_renderer(font).resolve_character_row(
			control,
			cell,
			std::uint8_t(source_row % W64_GLYPH_HEIGHT),
			character_layout::NATIVE_32);
	character_colors const colors = colors_for_character(control, cell, character.glyph);

	for (unsigned bit = 0; bit < 8; ++bit)
	{
		render_pixel const color =
				BIT(character.bits, 7 - bit)
						? colors.foreground
						: colors.background;
		pixels[bit * 2] = color;
		pixels[bit * 2 + 1] = color;
	}
}

void render_graphics_cell(
		core const &control,
		mode_kind mode,
		captured_cell const &cell,
		render_pixel *pixels) noexcept
{
	mode_descriptor const &descriptor = core::describe_mode(mode);
	pixel_input const input = cell.pixel();
	effective_controls const settings = control.controls(input);
	std::size_t output = 0;

	if (descriptor.family == mode_family::COLOR_GRAPHICS)
	{
		for (unsigned source_pixel = 0; source_pixel < 4; ++source_pixel)
		{
			std::uint8_t const value = BIT(cell.data, 6 - source_pixel * 2, 2);
			render_pixel const color = render_effects::color_graphics_color(
					control,
					input.css,
					value,
					settings.extras.full_semigraphics_palette).argb;
			for (unsigned repeat = 0; repeat < descriptor.scale_x; ++repeat)
				pixels[output++] = color;
		}
	}
	else
	{
		for (unsigned source_pixel = 0; source_pixel < 8; ++source_pixel)
		{
			bool const value = BIT(cell.data, 7 - source_pixel) != 0;
			render_pixel const color =
					render_effects::resolution_graphics_color(control, input.css, value).argb;
			for (unsigned repeat = 0; repeat < descriptor.scale_x; ++repeat)
				pixels[output++] = color;
		}
	}
}

void render_rg6_cell(
		core const &control,
		captured_cell const &cell,
		std::array<std::uint8_t, render_effects::RG6_SOURCE_BYTES> const &source,
		std::uint8_t column,
		render_pixel *pixels) noexcept
{
	std::size_t const first = std::size_t(column) * 16;
	pixel_input const input = cell.pixel();
	artifact_controls const settings = control.controls(input).artifact;
	for (std::size_t offset = 0; offset < 16; ++offset)
	{
		if (!render_effects::rg6_pixel(
					control,
					settings,
					input.css,
					source.data(),
					source.size(),
					first + offset,
					pixels[offset]))
		{
			pixels[offset] = OPAQUE_BLACK;
		}
	}
}

void render_cell(
		character_generator const &font,
		core const &control,
		mode_kind mode,
		captured_cell const &cell,
		std::uint16_t source_row,
		render_pixel *pixels) noexcept
{
	if (mode == mode_kind::ALPHA_SEMIGRAPHICS)
		render_alpha_cell(font, control, cell, source_row, pixels);
	else
		render_graphics_cell(control, mode, cell, pixels);
}

constexpr std::uint16_t STATUS_HEIGHT = 24;
constexpr std::uint16_t STATUS_TOP = VIEWPORT_TOP - STATUS_HEIGHT;
constexpr std::uint16_t STATUS_BOTTOM = VIEWPORT_BOTTOM_EXCLUSIVE;
// Reproduce the exact hardware-visible borderStatus.v byte cases with
// rights-holder authorization from CoCoVGA co-creator Stephen Spiller.
constexpr char BOOT_STATUS_TOP[] = "COCOVGA REV 0.92 WWW.COCOVGA.COM";
constexpr char BOOT_STATUS_BOTTOM[] = "(C)2011-20 DONAHE/SNIDER/SPILLER";

static_assert(sizeof(BOOT_STATUS_TOP) - 1 <= 32);
static_assert(sizeof(BOOT_STATUS_BOTTOM) - 1 <= 32);

std::uint8_t status_character_code(char value) noexcept
{
	if (value >= 'A' && value <= 'Z')
		return std::uint8_t(value - 'A' + 1);
	if (value >= ' ' && value <= '?')
		return std::uint8_t(value);
	return std::uint8_t(' ');
}

render_pixel status_foreground(core const &control) noexcept
{
	palette_output color;
	color.bits_per_component = core::describe_model(control.board_model()).dac_bits_per_component;
	color.color = { 0, 31, 24 };
	return render_effects::pack_color(color);
}

std::string_view mode_status_text(
		core const &control,
		render_effects::border_status_view const &status) noexcept
{
	if (!status.graphics)
		return "ALPHA/SG";
	if (control.state().w64_active)
		return "W64";
	if (control.state().vg6_active)
		return "VG6";

	switch (status.gm & 0x07)
	{
	case 0:
		return "CG1";
	case 1:
		return "RG1";
	case 2:
		return "CG2";
	case 3:
		return "RG2";
	case 4:
		return "CG3";
	case 5:
		return "RG3";
	case 6:
		return "CG6";
	case 7:
	default:
		return "RG6";
	}
}

constexpr std::size_t clamped_capture_address(std::uint16_t address) noexcept
{
	return std::min<std::size_t>(address, CAPTURE_PAGE_BYTES - 1);
}

bool valid_native_cell(captured_cell const &cell) noexcept
{
	if (!valid_mode_id(cell.mode_id))
		return false;

	mode_kind const mode = cell.mode();
	if (!native_mode(mode))
		return false;

	pixel_input const input = cell.pixel();
	if (mode == mode_kind::ALPHA_SEMIGRAPHICS)
		return !input.graphics;
	return !native_graphics_mode(mode) ||
			(input.graphics && core::decode_standard_mode(input) == mode);
}

std::uint16_t native_cell_width(mode_kind mode) noexcept
{
	return std::uint16_t(
			VIEWPORT_WIDTH / capture_engine::fetches_per_host_line(mode));
}

bool valid_native_row(
		frame_buffer_state const &frame,
		std::uint16_t row) noexcept
{
	if (row >= HOST_ACTIVE_LINES)
		return false;

	std::uint16_t const row_address = std::uint16_t(row * 32);
	std::uint8_t const row_cells = frame.row_fetch_counts[row];
	if (row_cells == 0 || row_cells > 32)
		return false;

	std::uint16_t produced_pixels = 0;
	for (std::uint8_t column = 0; column < row_cells; ++column)
	{
		captured_cell const &cell = frame.cells[std::uint16_t(row_address + column)];
		if (!valid_native_cell(cell))
			return false;
		produced_pixels = std::uint16_t(produced_pixels + native_cell_width(cell.mode()));
	}
	return produced_pixels != 0 &&
			produced_pixels <= VIEWPORT_WIDTH * 2;
}

render_status native_frame_status(frame_buffer_state const &frame) noexcept
{
	if (!frame.valid)
		return render_status::FRAME_UNAVAILABLE;
	if (!frame.complete ||
			frame.programming_suppressed ||
			frame.active_lines != HOST_ACTIVE_LINES)
	{
		return render_status::FRAME_INCOMPLETE;
	}
	if (frame.first_address >= CAPTURE_PAGE_BYTES ||
			frame.last_address >= CAPTURE_PAGE_BYTES ||
			!valid_mode_id(frame.first_mode) ||
			!valid_mode_id(frame.last_mode) ||
			!native_mode(mode_from_id(frame.first_mode)) ||
			!native_mode(mode_from_id(frame.last_mode)))
	{
		return render_status::INCONSISTENT_CAPTURE;
	}
	return render_status::OK;
}

render_status native_scanline_status(
		frame_buffer_state const &frame,
		std::uint16_t output_row) noexcept
{
	render_status const status = native_frame_status(frame);
	if (status != render_status::OK)
		return status;

	if (frame.writes < HOST_ACTIVE_LINES ||
			frame.writes > CAPTURE_PAGE_BYTES ||
			frame.first_address != 0 ||
			frame.first_address > frame.last_address ||
			frame.mode_changes >= frame.writes)
	{
		return render_status::INCONSISTENT_CAPTURE;
	}

	captured_cell const &first = frame.cells[clamped_capture_address(frame.first_address)];
	captured_cell const &last = frame.cells[clamped_capture_address(frame.last_address)];
	if (!valid_native_cell(first) ||
			!valid_native_cell(last) ||
			first.mode_id != frame.first_mode ||
			last.mode_id != frame.last_mode)
	{
		return render_status::INCONSISTENT_CAPTURE;
	}

	std::uint16_t const last_row_address = std::uint16_t((HOST_ACTIVE_LINES - 1) * 32);
	std::uint8_t const last_row_cells = frame.row_fetch_counts[HOST_ACTIVE_LINES - 1];
	if (last_row_cells == 0 || last_row_cells > 32)
		return render_status::INCONSISTENT_CAPTURE;
	std::uint16_t const expected_last = std::uint16_t(
			last_row_address + last_row_cells - 1);
	if (frame.last_address != expected_last)
		return render_status::INCONSISTENT_CAPTURE;

	if (output_row < VIEWPORT_TOP || output_row >= VIEWPORT_BOTTOM_EXCLUSIVE)
		return render_status::OK;

	std::uint16_t const source_row = std::uint16_t((output_row - VIEWPORT_TOP) / 2);
	return valid_native_row(frame, source_row)
			? render_status::OK
			: render_status::INCONSISTENT_CAPTURE;
}

bool scanlines_may_be_active(core const &control) noexcept
{
	bool const software = control.software_extras().scanlines;
	button_runtime const &buttons = control.buttons();
	return (software ^ ((buttons.text_button_1_cycle & 0x04) != 0)) ||
			(software ^ ((buttons.graphics_button_1_cycle & 0x02) != 0)) ||
			(software ^ ((buttons.rg6_button_1_cycle & 0x04) != 0));
}

bool status_overlay_active(render_options const &options) noexcept
{
	return options.border_status.visible &&
			options.border_status.kind != render_effects::border_status_kind::NONE;
}

captured_cell const &scanline_control_cell(
		frame_buffer_state const &frame,
		extended_mode extended,
		std::uint16_t output_row) noexcept
{
	if (output_row < VIEWPORT_TOP)
		return frame.cells[clamped_capture_address(frame.first_address)];
	if (output_row >= VIEWPORT_BOTTOM_EXCLUSIVE)
		return frame.cells[clamped_capture_address(frame.last_address)];

	std::uint16_t const viewport_row = std::uint16_t(output_row - VIEWPORT_TOP);
	std::uint16_t address = 0;
	switch (extended)
	{
	case extended_mode::W64:
		address = std::uint16_t((viewport_row / W64_GLYPH_HEIGHT) * W64_COLUMNS);
		break;

	case extended_mode::VG6:
		address = std::uint16_t((viewport_row / VG6_SCALE_Y) * VG6_BYTES_PER_ROW);
		break;

	case extended_mode::NONE:
	default:
		address = std::uint16_t((viewport_row / 2) * 32);
		break;
	}

	return frame.cells[std::min<std::size_t>(address, frame.cells.size() - 1)];
}

void render_status_scanline(
		character_generator const &font,
		core const &control,
		render_effects::border_status_view const &status,
		std::uint16_t output_row,
		render_pixel *pixels) noexcept
{
	if (!status.visible)
		return;

	std::string_view text;
	std::uint16_t first_row = 0;
	if (status.kind == render_effects::border_status_kind::BOOT_VERSION)
	{
		if (output_row >= STATUS_TOP && output_row < VIEWPORT_TOP)
		{
			text = BOOT_STATUS_TOP;
			first_row = STATUS_TOP;
		}
		else if (output_row >= STATUS_BOTTOM && output_row < STATUS_BOTTOM + STATUS_HEIGHT)
		{
			text = BOOT_STATUS_BOTTOM;
			first_row = STATUS_BOTTOM;
		}
		else
		{
			return;
		}
	}
	else if (status.kind == render_effects::border_status_kind::MODE &&
			output_row >= STATUS_BOTTOM &&
			output_row < STATUS_BOTTOM + STATUS_HEIGHT)
	{
		text = mode_status_text(control, status);
		first_row = STATUS_BOTTOM;
	}
	else
	{
		return;
	}

	std::array<char, 32> characters;
	characters.fill(' ');
	std::copy(
			text.begin(),
			text.end(),
			characters.begin() + (characters.size() - text.size()));

	std::uint8_t const glyph_row = std::uint8_t((output_row - first_row) / 2);
	render_pixel const foreground = status_foreground(control);
	std::fill_n(pixels + VIEWPORT_LEFT, VIEWPORT_WIDTH, OPAQUE_BLACK);

	for (std::size_t character = 0; character < characters.size(); ++character)
	{
		std::uint8_t const bits = extended_renderer(font).text_character_row(
				control.software_font().t1_font,
				status_character_code(characters[character]),
				glyph_row);
		render_pixel *const destination = pixels + VIEWPORT_LEFT + character * 16;
		for (unsigned bit = 0; bit < 8; ++bit)
		{
			render_pixel const color =
					BIT(bits, 7 - bit) != 0
							? foreground
							: OPAQUE_BLACK;
			destination[bit * 2] = color;
			destination[bit * 2 + 1] = color;
		}
	}
}

render_status extended_render_status(extended_status status) noexcept
{
	switch (status)
	{
	case extended_status::OK:
		return render_status::OK;
	case extended_status::INVALID_SURFACE:
		return render_status::INVALID_SURFACE;
	case extended_status::FRAME_UNAVAILABLE:
		return render_status::FRAME_UNAVAILABLE;
	case extended_status::FRAME_INCOMPLETE:
		return render_status::FRAME_INCOMPLETE;
	case extended_status::NO_MODE:
	case extended_status::MODE_INACTIVE:
	case extended_status::INCONSISTENT_CAPTURE:
	default:
		return render_status::INCONSISTENT_CAPTURE;
	}
}

} // anonymous namespace

render_pixel renderer::pack_color(palette_output const &color) noexcept
{
	return render_effects::pack_color(color);
}

character_generator const &renderer::font() const noexcept
{
	return m_font != nullptr ? *m_font : character_generator::blank();
}

char const *renderer::status_name(render_status status) noexcept
{
	switch (status)
	{
	case render_status::OK:
		return "ok";
	case render_status::INVALID_SURFACE:
		return "invalid surface";
	case render_status::FRAME_UNAVAILABLE:
		return "frame unavailable";
	case render_status::FRAME_INCOMPLETE:
		return "frame incomplete";
	case render_status::INCONSISTENT_CAPTURE:
		return "inconsistent capture";
	}

	return "unknown";
}

render_status renderer::preflight_native(frame_buffer_state const &frame) const noexcept
{
	render_status const frame_status = native_frame_status(frame);
	if (frame_status != render_status::OK)
		return frame_status;

	std::uint32_t writes = 0;
	std::uint16_t mode_changes = 0;
	std::uint16_t first_address = 0;
	std::uint16_t last_address = 0;
	std::uint8_t first_mode = 0;
	std::uint8_t last_mode = 0;
	bool have_cell = false;

	for (std::uint16_t row = 0; row < HOST_ACTIVE_LINES; ++row)
	{
		std::uint16_t const row_address = std::uint16_t(row * 32);
		std::uint8_t const row_cells = frame.row_fetch_counts[row];
		if (!valid_native_row(frame, row))
			return render_status::INCONSISTENT_CAPTURE;

		for (std::uint8_t column = 0; column < row_cells; ++column)
		{
			std::uint16_t const address = std::uint16_t(row_address + column);
			captured_cell const &cell = frame.cells[address];

			if (!have_cell)
			{
				have_cell = true;
				first_address = address;
				first_mode = cell.mode_id;
				last_mode = cell.mode_id;
			}
			else if (last_mode != cell.mode_id)
			{
				++mode_changes;
				last_mode = cell.mode_id;
			}

			last_address = address;
			++writes;
		}
	}

	if (!have_cell ||
			frame.writes != writes ||
			frame.first_address != first_address ||
			frame.last_address != last_address ||
			frame.mode_changes != mode_changes ||
			frame.first_mode != first_mode ||
			frame.last_mode != last_mode)
	{
		return render_status::INCONSISTENT_CAPTURE;
	}

	return render_status::OK;
}

render_result renderer::preflight(
		core const &control,
		frame_buffer_state const &frame) const noexcept
{
	extended_mode const extended = extended_renderer::selected_mode(control);
	render_status const status =
			extended == extended_mode::NONE
					? preflight_native(frame)
					: extended_render_status(
							extended_renderer::preflight(extended, control, frame));
	return make_result(frame, status, 0, 0);
}

void renderer::render_native_scanline(
		core const &control,
		frame_buffer_state const &frame,
		std::uint16_t output_row,
		render_pixel *pixels) const noexcept
{
	if (output_row < VIEWPORT_TOP)
	{
		render_pixel const color = border_pixel(
				control,
				frame.cells[clamped_capture_address(frame.first_address)]);
		std::fill_n(pixels, RENDER_WIDTH, color);
		return;
	}
	if (output_row >= VIEWPORT_BOTTOM_EXCLUSIVE)
	{
		render_pixel const color = border_pixel(
				control,
				frame.cells[clamped_capture_address(frame.last_address)]);
		std::fill_n(pixels, RENDER_WIDTH, color);
		return;
	}

	std::uint16_t const source_row = std::uint16_t((output_row - VIEWPORT_TOP) / 2);
	std::uint16_t const row_address = std::uint16_t(source_row * 32);
	std::uint8_t const row_cells = frame.row_fetch_counts[source_row];
	std::array<std::uint8_t, render_effects::RG6_SOURCE_BYTES> rg6_source{};
	for (std::uint8_t column = 0; column < row_cells; ++column)
		rg6_source[column] = frame.cells[std::uint16_t(row_address + column)].data;

	std::fill_n(pixels, VIEWPORT_LEFT, border_pixel(control, frame.cells[row_address]));

	std::uint16_t output_column = VIEWPORT_LEFT;
	for (std::uint8_t column = 0; column < row_cells; ++column)
	{
		captured_cell const &cell = frame.cells[std::uint16_t(row_address + column)];
		std::array<render_pixel, 32> cell_pixels{};
		std::uint16_t const cell_width = native_cell_width(cell.mode());
		if (cell.mode() == mode_kind::RG6)
			render_rg6_cell(control, cell, rg6_source, column, cell_pixels.data());
		else
			render_cell(font(), control, cell.mode(), cell, source_row, cell_pixels.data());

		std::uint16_t const remaining =
				output_column < VIEWPORT_RIGHT_EXCLUSIVE
						? std::uint16_t(VIEWPORT_RIGHT_EXCLUSIVE - output_column)
						: 0;
		std::uint16_t const copied = std::min(cell_width, remaining);
		std::copy_n(cell_pixels.data(), copied, pixels + output_column);
		output_column = std::uint16_t(output_column + copied);
	}

	captured_cell const &row_last =
			frame.cells[std::uint16_t(row_address + row_cells - 1)];
	std::fill(
			pixels + output_column,
			pixels + VIEWPORT_RIGHT_EXCLUSIVE,
			border_pixel(control, row_last));
	std::fill(
			pixels + VIEWPORT_RIGHT_EXCLUSIVE,
			pixels + RENDER_WIDTH,
			border_pixel(control, row_last));
}

void renderer::apply_output_effects(
		core const &control,
		frame_buffer_state const &frame,
		std::uint16_t output_row,
		render_pixel *pixels,
		render_options const &options,
		bool scanlines_active) const noexcept
{
	if (status_overlay_active(options))
		render_status_scanline(font(), control, options.border_status, output_row, pixels);

	if (scanlines_active)
	{
		extended_mode const extended = extended_renderer::selected_mode(control);
		captured_cell const &cell = scanline_control_cell(frame, extended, output_row);
		render_effects::apply_scanline(
				control,
				cell.pixel(),
				output_row,
				pixels,
				RENDER_WIDTH);
	}
}

render_result renderer::make_result(
		frame_buffer_state const &frame,
		render_status status,
		std::uint16_t scanlines,
		std::uint32_t pixels) const noexcept
{
	render_result result;
	result.status = status;
	result.generation = frame.generation;
	result.pixels_written = pixels;
	result.scanlines_written = scanlines;
	result.mode_changes = frame.mode_changes;
	result.first_mode = mode_from_id(frame.first_mode);
	result.last_mode = mode_from_id(frame.last_mode);
	return result;
}

void renderer::record_result(render_result const &result, bool full_frame) noexcept
{
	++m_state.render_calls;
	m_state.last_generation = result.generation;
	m_state.last_pixels_written = result.pixels_written;
	m_state.last_mode_changes = result.mode_changes;
	m_state.last_status = result.status;
	m_state.last_first_mode = result.first_mode;
	m_state.last_last_mode = result.last_mode;

	if (result.success())
	{
		m_state.scanlines_rendered += result.scanlines_written;
		if (full_frame)
			++m_state.frames_rendered;
	}
}

render_result renderer::render(
		core const &control,
		frame_buffer_state const &frame,
		pixel_surface const &surface,
		render_options const &options) noexcept
{
	bool const surface_valid = valid_surface(surface);
	extended_mode const extended = extended_renderer::selected_mode(control);
	render_status status = render_status::INVALID_SURFACE;
	extended_result extended_frame;

	if (surface_valid)
	{
		if (extended == extended_mode::NONE)
		{
			status = preflight_native(frame);
		}
		else
		{
			extended_frame = extended_renderer(font()).render(extended, control, frame, surface);
			status = extended_render_status(extended_frame.status);
		}
	}

	render_result result = make_result(frame, status, 0, 0);
	if (status == render_status::OK)
	{
		if (extended == extended_mode::NONE)
		{
			for (std::uint16_t row = 0; row < RENDER_HEIGHT; ++row)
				render_native_scanline(control, frame, row, surface.scanline(row).pixels);
			result.scanlines_written = RENDER_HEIGHT;
			result.pixels_written = std::uint32_t(RENDER_WIDTH) * RENDER_HEIGHT;
		}
		else
		{
			result.scanlines_written = extended_frame.scanlines_written;
			result.pixels_written = extended_frame.pixels_written;
		}

		bool const scanlines_active = scanlines_may_be_active(control);
		if (scanlines_active || status_overlay_active(options))
		{
			for (std::uint16_t row = 0; row < RENDER_HEIGHT; ++row)
			{
				apply_output_effects(
						control,
						frame,
						row,
						surface.scanline(row).pixels,
						options,
						scanlines_active);
			}
		}
	}

	record_result(result, true);
	return result;
}

render_result renderer::render_preserving(
		core const &control,
		frame_buffer_state const &frame,
		pixel_surface const &presented,
		pixel_surface const &staging,
		render_options const &options) noexcept
{
	if (!valid_surface(presented) ||
			!valid_surface(staging) ||
			presented.pixels == staging.pixels)
	{
		render_result const result =
				make_result(frame, render_status::INVALID_SURFACE, 0, 0);
		record_result(result, true);
		return result;
	}

	render_result const result = render(control, frame, staging, options);
	if (result.success())
	{
		for (std::uint16_t row = 0; row < RENDER_HEIGHT; ++row)
		{
			std::copy_n(
					staging.scanline(row).pixels,
					RENDER_WIDTH,
					presented.scanline(row).pixels);
		}
	}
	return result;
}

render_result renderer::render_scanline(
		core const &control,
		frame_buffer_state const &frame,
		std::uint16_t output_row,
		pixel_scanline const &scanline,
		render_options const &options) noexcept
{
	bool const valid_surface =
			output_row < RENDER_HEIGHT &&
			scanline.pixels != nullptr &&
			scanline.length >= RENDER_WIDTH;
	extended_mode const extended = extended_renderer::selected_mode(control);
	render_status status = render_status::INVALID_SURFACE;
	extended_result extended_line;

	if (valid_surface)
	{
		if (extended == extended_mode::NONE)
		{
			status = native_scanline_status(frame, output_row);
		}
		else
		{
			extended_line = extended_renderer(font()).render_scanline(
					extended,
					control,
					frame,
					output_row,
					scanline);
			status = extended_render_status(extended_line.status);
		}
	}

	render_result result = make_result(frame, status, 0, 0);
	if (status == render_status::OK)
	{
		if (extended == extended_mode::NONE)
		{
			render_native_scanline(control, frame, output_row, scanline.pixels);
			result.scanlines_written = 1;
			result.pixels_written = RENDER_WIDTH;
		}
		else
		{
			result.scanlines_written = extended_line.scanlines_written;
			result.pixels_written = extended_line.pixels_written;
		}

		bool const scanlines_active = scanlines_may_be_active(control);
		if (scanlines_active || status_overlay_active(options))
		{
			apply_output_effects(
					control,
					frame,
					output_row,
					scanline.pixels,
					options,
					scanlines_active);
		}
	}

	record_result(result, false);
	return result;
}

} // namespace cocovga
