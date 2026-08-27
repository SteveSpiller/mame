// license:BSD-3-Clause
// copyright-holders:Stephen Spiller

#include "emu.h"

#include "cocovga_extended.h"

#include "cocovga_charrom.h"

#include <algorithm>

namespace cocovga
{

namespace
{

constexpr render_pixel OPAQUE_BLACK = 0xff00'0000U;

// FNV-1a, used to identify an expanded text bank without carrying glyph data
constexpr std::uint64_t FNV1A_OFFSET_BASIS = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t FNV1A_PRIME = 1'099'511'628'211ULL;

pixel_input character_input(captured_cell const &cell, character_layout layout) noexcept
{
	pixel_input result = cell.pixel();
	if (layout == character_layout::W64)
	{
		// W64 uses CG2 only as its capture envelope, but pixelGen still feeds
		// the buffered GM and INT/EXT bits to its SG6 selector.
		result.graphics = false;
	}
	return result;
}

struct character_colors
{
	render_pixel foreground = OPAQUE_BLACK;
	render_pixel background = OPAQUE_BLACK;
};

character_colors colors_for_character(
		core const &control,
		captured_cell const &cell,
		character_layout layout,
		character_glyph glyph) noexcept
{
	pixel_input const input = character_input(cell, layout);
	effective_controls const settings = control.controls(input);
	character_colors result;

	switch (glyph)
	{
	case character_glyph::SEMIGRAPHICS4:
		result.foreground = renderer::pack_color(control.semigraphics_color(
				core::semigraphics_glyph_palette_index(
						semigraphics_glyph::SG4,
						input.css,
						BIT(cell.stream_byte(), 4, 4)),
				settings.extras.full_semigraphics_palette));
		result.background = renderer::pack_color(
				control.semigraphics_color(0, settings.extras.full_semigraphics_palette));
		break;

	case character_glyph::SEMIGRAPHICS6:
		result.foreground = renderer::pack_color(control.semigraphics_color(
				core::semigraphics_glyph_palette_index(
						semigraphics_glyph::SG6,
						input.css,
						BIT(cell.stream_byte(), 6, 2)),
				settings.extras.full_semigraphics_palette));
		result.background = renderer::pack_color(
				control.semigraphics_color(0, settings.extras.full_semigraphics_palette));
		break;

	case character_glyph::TEXT:
		{
			text_palette_descriptor const text = control.text_palette(input);
			result.foreground = renderer::pack_color(control.palette_color(text.foreground));
			result.background = renderer::pack_color(control.palette_color(text.background));
		}
		break;

	case character_glyph::NONE:
	default:
		break;
	}

	return result;
}

render_pixel w64_border_pixel(core const &control, captured_cell const &cell) noexcept
{
	pixel_input const input = character_input(cell, character_layout::W64);
	effective_controls const settings = control.controls(input);

	switch (settings.extras.border)
	{
	case border_mode::CUSTOM:
		return renderer::pack_color(control.extra_color(extra_palette_slot::BORDER));

	case border_mode::BLACK:
		return OPAQUE_BLACK;

	case border_mode::T1:
		[[fallthrough]];

	case border_mode::STANDARD:
	default:
		return renderer::pack_color(
				control.palette_color({ palette_source::SEMIGRAPHICS, 0, true }));
	}
}

constexpr mode_kind captured_mode(extended_mode mode) noexcept
{
	switch (mode)
	{
	case extended_mode::W64:
		return mode_kind::W64;

	case extended_mode::VG6:
		return mode_kind::VG6;

	case extended_mode::NONE:
	default:
		return mode_kind::ALPHA_SEMIGRAPHICS;
	}
}

constexpr std::uint16_t last_capture_address(extended_mode mode) noexcept
{
	return mode == extended_mode::W64
			? std::uint16_t(W64_LOGICAL_BYTES - 1)
			: std::uint16_t(CAPTURE_PAGE_BYTES - 1);
}

extended_status extended_frame_status(
		extended_mode mode,
		core const &control,
		frame_buffer_state const &frame) noexcept
{
	if (mode == extended_mode::NONE)
		return extended_status::NO_MODE;
	if (extended_renderer::selected_mode(control) != mode)
		return extended_status::MODE_INACTIVE;
	if (!frame.valid)
		return extended_status::FRAME_UNAVAILABLE;
	if (!frame.complete ||
			frame.programming_suppressed ||
			frame.active_lines != HOST_ACTIVE_LINES)
	{
		return extended_status::FRAME_INCOMPLETE;
	}

	mode_kind const expected_mode = captured_mode(mode);
	std::uint16_t const last_address = last_capture_address(mode);
	if (frame.writes != CAPTURE_PAGE_BYTES ||
			frame.first_address != 0 ||
			frame.last_address != last_address ||
			frame.mode_changes != 0 ||
			frame.first_mode != static_cast<std::uint8_t>(expected_mode) ||
			frame.last_mode != static_cast<std::uint8_t>(expected_mode) ||
			frame.cells[0].mode() != expected_mode ||
			frame.cells[last_address].mode() != expected_mode)
	{
		return extended_status::INCONSISTENT_CAPTURE;
	}
	return extended_status::OK;
}

} // anonymous namespace

character_generator::character_generator() noexcept
{
	mc6847_charset::generate_semigraphics(
			m_semigraphics4,
			SEMIGRAPHICS4_GLYPHS,
			mc6847_charset::SEMIGRAPHICS4_ROW_HEIGHT);
	mc6847_charset::generate_semigraphics(
			m_semigraphics6,
			SEMIGRAPHICS6_GLYPHS,
			mc6847_charset::SEMIGRAPHICS6_ROW_HEIGHT);
}

void character_generator::load(
		std::uint8_t const *standard_rom,
		std::uint8_t const *t1_rom) noexcept
{
	// The board carries both character generator ROMs.  Neither bank can stand
	// in for the other, so both images are required rather than approximated.
	assert(standard_rom != nullptr);
	assert(t1_rom != nullptr);

	mc6847_charset::text_font expanded{};

	mc6847_charset::expand_internal_rom(standard_rom, expanded);
	for (std::size_t character = 0; character < TEXT_GLYPHS; ++character)
	{
		for (std::size_t row = 0; row < mc6847_charset::GLYPH_HEIGHT; ++row)
			m_text[0][character][row] = expanded[character][row];
	}

	for (fpga_character_override const &entry : FPGA_STANDARD_OVERRIDES)
	{
		for (std::size_t row = 0; row < mc6847_charset::GLYPH_HEIGHT; ++row)
			m_text[0][entry.character][row] = entry.rows[row];
	}

	// The T1 bank is drawn one row lower than the MC6847T1 ROM stores it, and
	// one pixel towards the left edge of the cell: glyph bytes are most
	// significant bit first, so the left shift moves every pixel one column.
	mc6847_charset::expand_t1_rom(t1_rom, expanded);
	for (std::size_t character = 0; character < TEXT_GLYPHS; ++character)
	{
		m_text[1][character][0] = 0;
		for (std::size_t row = 1; row < mc6847_charset::GLYPH_HEIGHT; ++row)
			m_text[1][character][row] = std::uint8_t(expanded[character][row - 1] << 1);
	}

	for (fpga_character_override const &entry : FPGA_T1_OVERRIDES)
	{
		for (std::size_t row = 0; row < mc6847_charset::GLYPH_HEIGHT; ++row)
			m_text[1][entry.character][row] = entry.rows[row];
	}
}

std::uint64_t character_generator::text_bank_digest(bool t1_font) const noexcept
{
	std::uint8_t const *const bank = &m_text[t1_font ? 1 : 0][0][0];
	std::uint64_t digest = FNV1A_OFFSET_BASIS;
	for (std::size_t index = 0; index < TEXT_GLYPHS * mc6847_charset::GLYPH_HEIGHT; ++index)
		digest = (digest ^ bank[index]) * FNV1A_PRIME;
	return digest;
}

std::uint8_t character_generator::text_row(
		bool t1_font,
		std::uint8_t character,
		std::uint8_t row) const noexcept
{
	return row < mc6847_charset::GLYPH_HEIGHT
			? m_text[t1_font ? 1 : 0][BIT(character, 0, 6)][row]
			: 0;
}

std::uint8_t character_generator::lowercase_row(
		bool t1_font,
		std::uint8_t character,
		std::uint8_t row) const noexcept
{
	return row < mc6847_charset::GLYPH_HEIGHT
			? FPGA_LOWERCASE_ROWS[t1_font ? 1 : 0][BIT(character, 0, 5)][row]
			: 0;
}

std::uint8_t character_generator::semigraphics4_row(
		std::uint8_t character,
		std::uint8_t row) const noexcept
{
	return row < mc6847_charset::GLYPH_HEIGHT
			? m_semigraphics4[BIT(character, 0, 4) * mc6847_charset::GLYPH_HEIGHT + row]
			: 0;
}

std::uint8_t character_generator::semigraphics6_row(
		std::uint8_t character,
		std::uint8_t row) const noexcept
{
	return row < mc6847_charset::GLYPH_HEIGHT
			? m_semigraphics6[BIT(character, 0, 6) * mc6847_charset::GLYPH_HEIGHT + row]
			: 0;
}

character_generator const &character_generator::blank() noexcept
{
	static character_generator const empty;
	return empty;
}

extended_mode extended_renderer::selected_mode(core const &control) noexcept
{
	if (control.state().vg6_active)
		return extended_mode::VG6;
	if (control.state().w64_active)
		return extended_mode::W64;
	return extended_mode::NONE;
}

character_ram_row_result extended_renderer::lookup_character_ram(
		core const &control,
		std::uint8_t character,
		std::uint8_t row) noexcept
{
	character_ram_row_result result;
	result.character = character;
	result.row = row;
	result.address = core::character_address(character, row);
	if (result.address == INVALID_CAPTURE_ADDRESS)
		return result;

	result.valid = true;
	result.bits = control.character_ram()[result.address];
	return result;
}

std::uint8_t extended_renderer::text_character_row(
		bool t1_font,
		std::uint8_t character,
		std::uint8_t row) const noexcept
{
	return m_font->text_row(t1_font, character, row);
}

character_row_result extended_renderer::resolve_character_row(
		core const &control,
		captured_cell const &cell,
		std::uint8_t row,
		character_layout layout) const noexcept
{
	character_row_result result;
	result.layout = layout;
	result.stream_byte = cell.stream_byte();
	result.row = row;

	if (row >= W64_GLYPH_HEIGHT ||
			(layout == character_layout::NATIVE_32 && cell.mode() != mode_kind::ALPHA_SEMIGRAPHICS) ||
			(layout == character_layout::W64 && cell.mode() != mode_kind::W64))
	{
		return result;
	}

	pixel_input const input = character_input(cell, layout);
	effective_controls const settings = control.controls(input);
	bool const semigraphics6 =
			input.alpha_semigraphics &&
			(input.internal_external || BIT(input.gm, 0)) &&
			settings.extras.lowercase;
	bool const lowercase_character =
			(settings.font.force_lowercase ||
					(BIT(input.gm, 0) && !control.state().w64_active)) &&
			BIT(result.stream_byte, 5, 2) == 0 &&
			!input.alpha_semigraphics;

	result.character_ram_requested =
			settings.enhanced.character_ram &&
			(layout == character_layout::W64 ||
					settings.font.force_character_ram ||
					input.internal_external);

	// The HDL gives character RAM priority over text and SG4, but SG6 bypasses it.
	if (result.character_ram_requested && !semigraphics6)
	{
		character_ram_row_result const uploaded =
				lookup_character_ram(control, result.stream_byte, row);
		result.valid = uploaded.valid;
		result.character_ram_used = uploaded.valid;
		result.glyph = input.alpha_semigraphics
				? character_glyph::SEMIGRAPHICS4
				: character_glyph::TEXT;
		result.source = character_source::UPLOADED_RAM;
		result.character = result.stream_byte;
		result.character_ram_address = uploaded.address;
		result.inverted = false;
		result.bits = uploaded.bits;
		return result;
	}

	if (input.alpha_semigraphics)
	{
		result.valid = true;
		result.glyph = semigraphics6
				? character_glyph::SEMIGRAPHICS6
				: character_glyph::SEMIGRAPHICS4;
		result.source = character_source::GENERATED;
		result.character = semigraphics6
				? BIT(result.stream_byte, 0, 6)
				: BIT(result.stream_byte, 0, 4);
		result.inverted = false;
		result.bits = semigraphics6
				? m_font->semigraphics6_row(result.character, row)
				: m_font->semigraphics4_row(result.character, row);
		return result;
	}

	if (lowercase_character)
	{
		result.valid = true;
		result.glyph = character_glyph::TEXT;
		result.source = character_source::LOWERCASE_CHARACTER_ROM;
		result.character = std::uint8_t(BIT(input.data, 0, 5) + 0x40);
		result.inverted = false;
		result.bits = m_font->lowercase_row(
				settings.font.t1_font,
				input.data,
				row);
		return result;
	}

	result.valid = true;
	result.glyph = character_glyph::TEXT;
	result.source = character_source::CHARACTER_ROM;
	result.character = BIT(input.data, 0, 6);
	result.inverted = false;
	result.bits = m_font->text_row(
			settings.font.t1_font,
			result.character,
			row);
	return result;
}

w64_cell_location extended_renderer::map_w64_cell(
		std::uint8_t text_row,
		std::uint8_t text_column) noexcept
{
	w64_cell_location result;
	result.text_row = text_row;
	result.text_column = text_column;
	if (text_row >= W64_ROWS || text_column >= W64_COLUMNS)
		return result;

	result.valid = true;
	result.capture_row = std::uint8_t(text_row * 2 + text_column / W64_CAPTURE_ROW_BYTES);
	result.capture_column = std::uint8_t(text_column % W64_CAPTURE_ROW_BYTES);
	result.capture_address = std::uint16_t(
			std::uint16_t(result.capture_row) * W64_CAPTURE_ROW_BYTES +
			result.capture_column);
	result.first_host_line = std::uint16_t(result.capture_row * W64_HOST_LINE_REPEAT);
	result.last_host_line = std::uint16_t(result.first_host_line + W64_HOST_LINE_REPEAT - 1);
	return result;
}

w64_pixel_location extended_renderer::map_w64_viewport_pixel(
		std::uint16_t viewport_x,
		std::uint16_t viewport_y) noexcept
{
	w64_pixel_location result;
	if (viewport_x >= VIEWPORT_WIDTH || viewport_y >= VIEWPORT_HEIGHT)
		return result;

	result.cell = map_w64_cell(
			std::uint8_t(viewport_y / W64_GLYPH_HEIGHT),
			std::uint8_t(viewport_x / W64_GLYPH_WIDTH));
	result.valid = result.cell.valid;
	result.glyph_row = std::uint8_t(viewport_y % W64_GLYPH_HEIGHT);
	result.glyph_column = std::uint8_t(viewport_x % W64_GLYPH_WIDTH);
	result.glyph_mask = std::uint8_t(0x80U >> result.glyph_column);
	return result;
}

vg6_pixel_location extended_renderer::map_vg6_viewport_pixel(
		std::uint16_t viewport_x,
		std::uint16_t viewport_y) noexcept
{
	vg6_pixel_location result;
	if (viewport_x >= VIEWPORT_WIDTH || viewport_y >= VIEWPORT_HEIGHT)
		return result;

	result.valid = true;
	result.logical_x = std::uint8_t(viewport_x / VG6_SCALE_X);
	result.logical_y = std::uint8_t(viewport_y / VG6_SCALE_Y);
	std::uint8_t const byte_column = std::uint8_t(result.logical_x / 2);
	result.capture_address = std::uint16_t(
			std::uint16_t(result.logical_y) * VG6_BYTES_PER_ROW +
			byte_column);
	result.capture_row = std::uint16_t(result.capture_address / W64_CAPTURE_ROW_BYTES);
	result.capture_column = std::uint8_t(result.capture_address % W64_CAPTURE_ROW_BYTES);
	result.nibble_shift = BIT(result.logical_x, 0) ? 0 : 4;
	return result;
}

vg6_pixel_result extended_renderer::decode_vg6_viewport_pixel(
		core const &control,
		frame_buffer_state const &frame,
		std::uint16_t viewport_x,
		std::uint16_t viewport_y) noexcept
{
	vg6_pixel_result result;
	result.location = map_vg6_viewport_pixel(viewport_x, viewport_y);
	if (!result.location.valid)
		return result;

	captured_cell const &cell = frame.cells[result.location.capture_address];
	if (cell.mode() != mode_kind::VG6)
		return result;

	result.valid = true;
	result.packed_byte = cell.data;
	result.pixel = std::uint8_t(BIT(cell.data, result.location.nibble_shift, 4));
	result.css = cell.css();
	result.palette_index = core::vg6_palette_index(result.css, result.pixel);
	result.color = renderer::pack_color(control.artifact_color(result.palette_index));
	return result;
}

extended_status extended_renderer::preflight(
		extended_mode mode,
		core const &control,
		frame_buffer_state const &frame) noexcept
{
	extended_status const frame_status = extended_frame_status(mode, control, frame);
	if (frame_status != extended_status::OK)
		return frame_status;
	mode_kind const expected_mode = captured_mode(mode);
	std::uint16_t const last_address = last_capture_address(mode);
	for (std::uint16_t address = 0; address <= last_address; ++address)
	{
		if (frame.cells[address].mode() != expected_mode)
			return extended_status::INCONSISTENT_CAPTURE;
	}

	return extended_status::OK;
}

extended_status extended_renderer::preflight_scanline(
		extended_mode mode,
		core const &control,
		frame_buffer_state const &frame,
		std::uint16_t output_row) noexcept
{
	extended_status const frame_status = extended_frame_status(mode, control, frame);
	if (frame_status != extended_status::OK)
		return frame_status;
	if (output_row < VIEWPORT_TOP || output_row >= VIEWPORT_BOTTOM_EXCLUSIVE)
		return extended_status::OK;

	std::uint16_t const viewport_row = std::uint16_t(output_row - VIEWPORT_TOP);
	std::uint16_t const row_address =
			mode == extended_mode::W64
					? std::uint16_t((viewport_row / W64_GLYPH_HEIGHT) * W64_COLUMNS)
					: std::uint16_t((viewport_row / VG6_SCALE_Y) * VG6_BYTES_PER_ROW);
	mode_kind const expected_mode = captured_mode(mode);
	for (std::uint8_t column = 0; column < 64; ++column)
	{
		if (frame.cells[std::uint16_t(row_address + column)].mode() != expected_mode)
			return extended_status::INCONSISTENT_CAPTURE;
	}
	return extended_status::OK;
}

void extended_renderer::render_w64_scanline(
		core const &control,
		frame_buffer_state const &frame,
		std::uint16_t output_row,
		render_pixel *pixels) const noexcept
{
	if (output_row < VIEWPORT_TOP)
	{
		std::fill_n(pixels, RENDER_WIDTH, w64_border_pixel(control, frame.cells[0]));
		return;
	}
	if (output_row >= VIEWPORT_BOTTOM_EXCLUSIVE)
	{
		std::fill_n(
				pixels,
				RENDER_WIDTH,
				w64_border_pixel(control, frame.cells[W64_LOGICAL_BYTES - 1]));
		return;
	}

	std::uint16_t const viewport_row = std::uint16_t(output_row - VIEWPORT_TOP);
	std::uint8_t const text_row = std::uint8_t(viewport_row / W64_GLYPH_HEIGHT);
	std::uint8_t const glyph_row = std::uint8_t(viewport_row % W64_GLYPH_HEIGHT);
	std::uint16_t const row_address = std::uint16_t(text_row * W64_COLUMNS);

	std::fill_n(
			pixels,
			VIEWPORT_LEFT,
			w64_border_pixel(control, frame.cells[row_address]));

	std::uint16_t output_column = VIEWPORT_LEFT;
	for (std::uint8_t column = 0; column < W64_COLUMNS; ++column)
	{
		captured_cell const &cell = frame.cells[std::uint16_t(row_address + column)];
		character_row_result const character = resolve_character_row(
				control,
				cell,
				glyph_row,
				character_layout::W64);
		character_colors const colors = colors_for_character(
				control,
				cell,
				character_layout::W64,
				character.glyph);

		for (unsigned bit = 0; bit < W64_GLYPH_WIDTH; ++bit)
			pixels[output_column++] =
					BIT(character.bits, W64_GLYPH_WIDTH - 1 - bit) != 0
							? colors.foreground
							: colors.background;
	}

	std::fill(
			pixels + VIEWPORT_RIGHT_EXCLUSIVE,
			pixels + RENDER_WIDTH,
			w64_border_pixel(
					control,
					frame.cells[std::uint16_t(row_address + W64_COLUMNS - 1)]));
}

void extended_renderer::render_vg6_scanline(
		core const &control,
		frame_buffer_state const &frame,
		std::uint16_t output_row,
		render_pixel *pixels) noexcept
{
	if (output_row < VIEWPORT_TOP || output_row >= VIEWPORT_BOTTOM_EXCLUSIVE)
	{
		std::fill_n(pixels, RENDER_WIDTH, OPAQUE_BLACK);
		return;
	}

	std::fill_n(pixels, VIEWPORT_LEFT, OPAQUE_BLACK);
	std::uint8_t const logical_row =
			std::uint8_t((output_row - VIEWPORT_TOP) / VG6_SCALE_Y);
	std::uint16_t const row_address =
			std::uint16_t(std::uint16_t(logical_row) * VG6_BYTES_PER_ROW);
	std::uint16_t output_column = VIEWPORT_LEFT;

	for (std::uint8_t byte_column = 0; byte_column < VG6_BYTES_PER_ROW; ++byte_column)
	{
		captured_cell const &cell = frame.cells[std::uint16_t(row_address + byte_column)];
		for (std::uint8_t shift : { std::uint8_t(4), std::uint8_t(0) })
		{
			std::uint8_t const pixel = std::uint8_t(BIT(cell.data, shift, 4));
			render_pixel const color = renderer::pack_color(
					control.artifact_color(core::vg6_palette_index(cell.css(), pixel)));
			for (unsigned repeat = 0; repeat < VG6_SCALE_X; ++repeat)
				pixels[output_column++] = color;
		}
	}

	std::fill(
			pixels + VIEWPORT_RIGHT_EXCLUSIVE,
			pixels + RENDER_WIDTH,
			OPAQUE_BLACK);
}

extended_result extended_renderer::render(
		extended_mode mode,
		core const &control,
		frame_buffer_state const &frame,
		pixel_surface const &surface) const noexcept
{
	extended_result result;
	result.mode = mode;
	result.generation = frame.generation;
	result.status =
			surface.pixels != nullptr &&
			surface.width == RENDER_WIDTH &&
			surface.height == RENDER_HEIGHT &&
			surface.row_stride >= RENDER_WIDTH
					? preflight(mode, control, frame)
					: extended_status::INVALID_SURFACE;

	if (result.status == extended_status::OK)
	{
		for (std::uint16_t row = 0; row < RENDER_HEIGHT; ++row)
		{
			render_pixel *const pixels = surface.scanline(row).pixels;
			if (mode == extended_mode::W64)
				render_w64_scanline(control, frame, row, pixels);
			else
				render_vg6_scanline(control, frame, row, pixels);
		}
		result.scanlines_written = RENDER_HEIGHT;
		result.pixels_written = std::uint32_t(RENDER_WIDTH) * RENDER_HEIGHT;
	}

	return result;
}

extended_result extended_renderer::render_scanline(
		extended_mode mode,
		core const &control,
		frame_buffer_state const &frame,
		std::uint16_t output_row,
		pixel_scanline const &scanline) const noexcept
{
	extended_result result;
	result.mode = mode;
	result.generation = frame.generation;
	result.status =
			output_row < RENDER_HEIGHT &&
			scanline.pixels != nullptr &&
			scanline.length >= RENDER_WIDTH
					? preflight_scanline(mode, control, frame, output_row)
					: extended_status::INVALID_SURFACE;

	if (result.status == extended_status::OK)
	{
		if (mode == extended_mode::W64)
			render_w64_scanline(control, frame, output_row, scanline.pixels);
		else
			render_vg6_scanline(control, frame, output_row, scanline.pixels);
		result.scanlines_written = 1;
		result.pixels_written = RENDER_WIDTH;
	}

	return result;
}

char const *extended_renderer::status_name(extended_status status) noexcept
{
	switch (status)
	{
	case extended_status::OK:
		return "ok";
	case extended_status::NO_MODE:
		return "no extended mode";
	case extended_status::MODE_INACTIVE:
		return "extended mode inactive";
	case extended_status::INVALID_SURFACE:
		return "invalid surface";
	case extended_status::FRAME_UNAVAILABLE:
		return "frame unavailable";
	case extended_status::FRAME_INCOMPLETE:
		return "frame incomplete";
	case extended_status::INCONSISTENT_CAPTURE:
		return "inconsistent capture";
	}

	return "unknown";
}

} // namespace cocovga
