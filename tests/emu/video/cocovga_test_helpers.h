// license:BSD-3-Clause
// copyright-holders:Stephen Spiller

#ifndef MAME_TESTS_EMU_VIDEO_COCOVGA_TEST_HELPERS_H
#define MAME_TESTS_EMU_VIDEO_COCOVGA_TEST_HELPERS_H

#pragma once

#include "mc6847_charset_fixture.h"

#include "video/cocovga_artifact.h"
#include "video/cocovga_extended.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cocovga::test
{

// Character generator loaded from the ROM fixtures, standing in for the
// device's own chargen regions.
inline character_generator const &character_rom()
{
	static character_generator const font = []
			{
				character_generator result;
				result.load(
						mc6847_charset_test::internal_rom().data(),
						mc6847_charset_test::t1_rom().data());
				return result;
			}();
	return font;
}

inline extended_renderer make_extended_renderer()
{
	return extended_renderer(character_rom());
}

inline renderer make_renderer()
{
	renderer result;
	result.set_character_generator(&character_rom());
	return result;
}

constexpr std::uint64_t FNV1A_OFFSET = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t FNV1A_PRIME = 1'099'511'628'211ULL;

inline void hash_byte(std::uint64_t &hash, std::uint8_t value) noexcept
{
	hash = (hash ^ value) * FNV1A_PRIME;
}

inline void hash_u16(std::uint64_t &hash, std::uint16_t value) noexcept
{
	hash_byte(hash, std::uint8_t(value));
	hash_byte(hash, std::uint8_t(value >> 8));
}

inline void hash_u32(std::uint64_t &hash, std::uint32_t value) noexcept
{
	hash_u16(hash, std::uint16_t(value));
	hash_u16(hash, std::uint16_t(value >> 16));
}

inline void hash_u64(std::uint64_t &hash, std::uint64_t value) noexcept
{
	hash_u32(hash, std::uint32_t(value));
	hash_u32(hash, std::uint32_t(value >> 32));
}

inline std::uint64_t pixel_hash(render_pixel const *pixels, std::size_t count) noexcept
{
	std::uint64_t hash = FNV1A_OFFSET;
	for (std::size_t index = 0; index < count; ++index)
		hash_u32(hash, pixels[index]);
	return hash;
}

inline std::uint64_t frame_hash(frame_buffer_state const &frame) noexcept
{
	std::uint64_t hash = FNV1A_OFFSET;
	for (captured_cell const &cell : frame.cells)
	{
		hash_byte(hash, cell.data);
		hash_byte(hash, cell.gm);
		hash_byte(hash, cell.mode_id);
		hash_byte(hash, cell.flags);
	}
	for (std::uint8_t const count : frame.row_fetch_counts)
		hash_byte(hash, count);
	hash_u64(hash, frame.generation);
	hash_u32(hash, frame.writes);
	hash_u16(hash, frame.first_address);
	hash_u16(hash, frame.last_address);
	hash_u16(hash, frame.active_lines);
	hash_u16(hash, frame.mode_changes);
	hash_byte(hash, frame.first_mode);
	hash_byte(hash, frame.last_mode);
	hash_byte(hash, frame.valid);
	hash_byte(hash, frame.complete);
	hash_byte(hash, frame.programming_suppressed);
	return hash;
}

inline std::uint8_t gm_for_mode(mode_kind mode) noexcept
{
	switch (mode)
	{
	case mode_kind::CG1:
		return 0;
	case mode_kind::RG1:
		return 1;
	case mode_kind::CG2:
	case mode_kind::W64:
		return 2;
	case mode_kind::RG2:
		return 3;
	case mode_kind::CG3:
		return 4;
	case mode_kind::RG3:
		return 5;
	case mode_kind::CG6:
	case mode_kind::VG6:
		return 6;
	case mode_kind::RG6:
		return 7;
	case mode_kind::ALPHA_SEMIGRAPHICS:
	default:
		return 0;
	}
}

inline bool graphics_for_mode(mode_kind mode) noexcept
{
	return mode != mode_kind::ALPHA_SEMIGRAPHICS;
}

inline pixel_input input_for_mode(
		mode_kind mode,
		bool css = false,
		std::uint8_t data = 0) noexcept
{
	pixel_input result;
	result.data = data;
	result.gm = gm_for_mode(mode);
	result.graphics = graphics_for_mode(mode);
	result.css = css;
	return result;
}

inline captured_cell make_cell(
		mode_kind mode,
		bool css,
		std::uint8_t data,
		bool alpha_semigraphics = false,
		bool inverse = false,
		bool internal_external = false) noexcept
{
	captured_cell result;
	result.data =
			(mode == mode_kind::ALPHA_SEMIGRAPHICS || mode == mode_kind::W64)
					? std::uint8_t(data & 0x3f)
					: data;
	result.gm = gm_for_mode(mode);
	result.mode_id = static_cast<std::uint8_t>(mode);
	result.flags =
			(graphics_for_mode(mode) ? captured_cell::FLAG_GRAPHICS : 0) |
			(css ? captured_cell::FLAG_CSS : 0) |
			(alpha_semigraphics ? captured_cell::FLAG_ALPHA_SEMIGRAPHICS : 0) |
			(inverse ? captured_cell::FLAG_INVERSE : 0) |
			(internal_external ? captured_cell::FLAG_INTERNAL_EXTERNAL : 0);
	return result;
}

inline frame_buffer_state make_native_frame(
		mode_kind mode,
		bool css,
		std::uint8_t seed = 0x39) noexcept
{
	frame_buffer_state frame;
	std::uint8_t const row_cells = capture_engine::fetches_per_host_line(mode);
	for (std::uint16_t row = 0; row < HOST_ACTIVE_LINES; ++row)
	{
		frame.row_fetch_counts[row] = row_cells;
		for (std::uint8_t column = 0; column < row_cells; ++column)
		{
			std::uint16_t const address = std::uint16_t(row * 32 + column);
			std::uint8_t const data = std::uint8_t(
					seed + row * 13U + column * 29U + (address >> 3));
			if (mode == mode_kind::ALPHA_SEMIGRAPHICS)
			{
				unsigned const selector = (row + column) & 7U;
				frame.cells[address] = make_cell(
						mode,
						css,
						data,
						selector == 1 || selector == 2,
						selector == 3 || selector == 6,
						selector == 2 || selector == 5);
			}
			else
			{
				frame.cells[address] = make_cell(mode, css, data);
			}
		}
	}

	frame.generation = 0x1234;
	frame.writes = std::uint32_t(HOST_ACTIVE_LINES) * row_cells;
	frame.first_address = 0;
	frame.last_address = std::uint16_t((HOST_ACTIVE_LINES - 1) * 32 + row_cells - 1);
	frame.active_lines = HOST_ACTIVE_LINES;
	frame.first_mode = static_cast<std::uint8_t>(mode);
	frame.last_mode = static_cast<std::uint8_t>(mode);
	frame.valid = true;
	frame.complete = true;
	return frame;
}

inline frame_buffer_state make_w64_frame(bool css, std::uint8_t seed = 0x13) noexcept
{
	frame_buffer_state frame;
	frame.row_fetch_counts.fill(32);
	for (std::uint16_t address = 0; address < W64_LOGICAL_BYTES; ++address)
	{
		std::uint8_t const data = std::uint8_t(seed + address * 37U + (address >> 5));
		frame.cells[address] = make_cell(
				mode_kind::W64,
				css,
				data,
				(address & 0x0f) == 1,
				(address & 0x0f) == 3);
	}
	frame.generation = 0x2345;
	frame.writes = CAPTURE_PAGE_BYTES;
	frame.first_address = 0;
	frame.last_address = W64_LOGICAL_BYTES - 1;
	frame.active_lines = HOST_ACTIVE_LINES;
	frame.first_mode = static_cast<std::uint8_t>(mode_kind::W64);
	frame.last_mode = frame.first_mode;
	frame.valid = true;
	frame.complete = true;
	return frame;
}

inline frame_buffer_state make_vg6_frame(bool css, std::uint8_t seed = 0x57) noexcept
{
	frame_buffer_state frame;
	frame.row_fetch_counts.fill(32);
	for (std::uint16_t address = 0; address < CAPTURE_PAGE_BYTES; ++address)
	{
		std::uint8_t const data = std::uint8_t(seed + address * 19U + (address >> 6));
		frame.cells[address] = make_cell(mode_kind::VG6, css, data);
	}
	frame.generation = 0x3456;
	frame.writes = CAPTURE_PAGE_BYTES;
	frame.first_address = 0;
	frame.last_address = CAPTURE_PAGE_BYTES - 1;
	frame.active_lines = HOST_ACTIVE_LINES;
	frame.first_mode = static_cast<std::uint8_t>(mode_kind::VG6);
	frame.last_mode = frame.first_mode;
	frame.valid = true;
	frame.complete = true;
	return frame;
}

inline pixel_surface make_surface(std::vector<render_pixel> &pixels) noexcept
{
	pixels.assign(std::size_t(RENDER_WIDTH) * RENDER_HEIGHT, 0);
	return { pixels.data(), RENDER_WIDTH, RENDER_WIDTH, RENDER_HEIGHT };
}

inline bool write_register(
		core &control,
		register_bank bank,
		std::uint16_t offset,
		std::uint8_t value) noexcept
{
	return control.write_page00(1, core::bank_bit(bank)) &&
			control.write_page00(offset, value);
}

inline combo_step unlock_page(core &control, register_page page) noexcept
{
	control.clock_combo(false, 0);
	control.clock_combo(false, core::COMBO_1);
	control.clock_combo(false, core::COMBO_2);
	control.clock_combo(false, core::COMBO_3);
	control.clock_combo(false, core::COMBO_4);
	control.clock_combo(false, 0);
	control.clock_combo(false, 0);
	return control.clock_combo(false, static_cast<std::uint8_t>(page));
}

inline bool capture_frame(
		capture_engine &capture,
		mode_kind mode,
		bool css,
		std::uint8_t seed,
		host_sync_result &completion,
		bool compare_sam = true) noexcept
{
	if (!capture.state().host.frame_started)
	{
		host_sync_result const started = capture.host_fsync(false);
		if (!started.frame_started)
			return false;
	}

	for (std::uint16_t row = 0; row < HOST_ACTIVE_LINES; ++row)
	{
		capture.host_hsync(false);
		std::uint8_t const count = capture_engine::fetches_per_host_line(mode);
		for (std::uint8_t column = 0; column < count; ++column)
		{
			host_fetch_event event;
			event.data = std::uint8_t(seed + row * 11U + column * 17U);
			event.mode.gm = gm_for_mode(mode);
			event.mode.graphics = graphics_for_mode(mode);
			event.mode.css = css;
			event.mode.alpha_semigraphics =
					mode == mode_kind::ALPHA_SEMIGRAPHICS && ((row + column) & 3U) == 1;
			event.mode.inverse =
					mode == mode_kind::ALPHA_SEMIGRAPHICS && ((row + column) & 3U) == 2;
			event.phase = host_phase::ACTIVE;
			event.sam_address_valid = compare_sam;
			event.sam_address = std::uint16_t(
					0x2000 +
					capture_engine::expected_sam_offset(mode, row, column));
			capture_result const result = capture.host_fetch(event, mode);
			if (!result.sampled || (!capture.programming() && !result.buffer_written))
				return false;
		}
		capture.host_hsync(true);
	}

	capture.host_fsync(true);
	completion = capture.host_fsync(false);
	return completion.frame_completed;
}

} // namespace cocovga::test

#endif // MAME_TESTS_EMU_VIDEO_COCOVGA_TEST_HELPERS_H
