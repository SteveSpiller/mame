// license:BSD-3-Clause
// copyright-holders:Stephen Spiller

#include "catch.hpp"

#include "cocovga_charrom_oracle.h"
#include "cocovga_test_helpers.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <utility>
#include <vector>

namespace
{

using namespace cocovga;

vga_tick_result clock_at_vsync_start(capture_engine &capture)
{
	vga_capture_state &vga = capture.state().vga;
	vga.hcounter = 0;
	vga.vcounter = capture.timing_endpoints().v_front_end;
	vga.vsync_asserted = false;
	return capture.clock_vga();
}

vga_tick_result clock_at_frame_boundary(capture_engine &capture)
{
	vga_capture_state &vga = capture.state().vga;
	vga.hcounter = std::uint16_t(capture.timing_endpoints().h_total - 1);
	vga.vcounter = std::uint16_t(capture.timing_endpoints().v_total - 1);
	return capture.clock_vga();
}

void release_programming(core &control, capture_engine &capture)
{
	control.clock_combo(true, 0);
	control.clock_combo(false, 0);
	REQUIRE_FALSE(control.freeze());
	REQUIRE(capture.update_from_core(control));
	REQUIRE_FALSE(capture.programming());
}

void selected_page00_write(
		core &control,
		capture_engine &capture,
		std::uint16_t logical_offset,
		std::uint8_t value)
{
	std::uint16_t const address = core::page00_logical_to_internal(logical_offset);
	REQUIRE(address != INVALID_CAPTURE_ADDRESS);
	REQUIRE(control.ingest_selected_capture(address, value));
	REQUIRE(capture.update_from_core(control));
}

std::uint16_t palette_word(unsigned seed)
{
	rgb_color const color
	{
		std::uint8_t((seed * 7U + 3U) & 0x1f),
		std::uint8_t((seed * 11U + 5U) & 0x1f),
		std::uint8_t((seed * 13U + 9U) & 0x1f)
	};
	return std::uint16_t(0x8000 | core::pack_rgb15(color));
}

void write_palette_word(core &control, std::uint16_t offset, std::uint16_t word)
{
	REQUIRE(control.write_page00(offset, std::uint8_t(word >> 8)));
	REQUIRE(control.write_page00(std::uint16_t(offset + 1), std::uint8_t(word)));
}

std::uint64_t rendered_hash(
		renderer &output,
		core const &control,
		frame_buffer_state const &frame,
		render_options const &options = {})
{
	std::vector<render_pixel> pixels;
	pixel_surface const surface = test::make_surface(pixels);
	render_result const result = output.render(control, frame, surface, options);
	INFO("render status " << renderer::status_name(result.status));
	REQUIRE(result.success());
	REQUIRE(result.generation == frame.generation);
	REQUIRE(result.pixels_written == std::uint32_t(RENDER_WIDTH) * RENDER_HEIGHT);
	REQUIRE(result.scanlines_written == RENDER_HEIGHT);
	return test::pixel_hash(pixels.data(), pixels.size());
}

std::uint8_t character_stream_value(std::uint16_t capture_address)
{
	return std::uint8_t(capture_address * 29U + (capture_address >> 4) + 0x53U);
}

std::uint8_t ordered_alpha_data(
		std::uint16_t row,
		std::uint8_t column,
		std::uint8_t seed) noexcept
{
	return std::uint8_t((seed + row * 13U + column * 5U) & 0x3f);
}

bool mame_ordered_alpha_burst(
		capture_engine &capture,
		std::uint16_t row,
		bool css,
		std::uint8_t seed,
		capture_result *first_result = nullptr) noexcept
{
	capture.host_hblank(true);
	for (std::uint8_t column = 0; column < 32; ++column)
	{
		host_fetch_event event;
		event.sam_address = capture_engine::expected_sam_offset(
				mode_kind::ALPHA_SEMIGRAPHICS,
				row,
				column);
		event.data = ordered_alpha_data(row, column, seed);
		event.mode.css = css;
		event.phase = host_phase::ACTIVE;
		event.sam_address_valid = true;
		event.sam_address_is_offset = true;
		capture_result const result =
				capture.host_fetch(event, mode_kind::ALPHA_SEMIGRAPHICS);
		if (column == 0 && first_result != nullptr)
			*first_result = result;
		if (!result.sampled || !result.buffer_written)
			return false;
	}
	capture.host_hblank(false);
	return true;
}

std::uint8_t programming_stream_value(
		register_page page,
		std::uint16_t address,
		std::uint8_t seed) noexcept
{
	if (page == register_page::PAGE_00)
	{
		std::uint16_t logical_offset;
		if (!core::page00_internal_to_logical(address, logical_offset))
			return 0;

		switch (logical_offset)
		{
		case 0:
			return 0;
		case 1:
			return core::bank_bit(register_bank::FONT);
		case 3:
			return 0x05;
		default:
			return 0;
		}
	}

	return character_stream_value(std::uint16_t(address + seed));
}

bool mame_ordered_device_burst(
		core &control,
		capture_engine &capture,
		std::uint16_t row,
		std::uint8_t seed) noexcept
{
	capture.host_hblank(true);
	for (std::uint8_t column = 0; column < 32; ++column)
	{
		std::uint16_t const address = std::uint16_t(row * 32 + column);
		host_fetch_event event;
		event.sam_address = address;
		event.data = programming_stream_value(
				control.selected_register_page(),
				address,
				seed);
		event.mode.gm = 7;
		event.mode.graphics = true;
		event.phase = host_phase::ACTIVE;
		event.sam_address_valid = true;
		event.sam_address_is_offset = true;

		capture_result const result = capture.host_fetch(event, control);
		if (!result.sampled ||
				(!capture.programming() &&
						!capture.state().host.frame_programming_suppressed &&
						!result.buffer_written))
			return false;

		if (control.freeze())
		{
			control.ingest_selected_capture(result.input_address, result.stream_byte);
			if (!capture.update_from_core(control))
				return false;
		}
	}
	capture.host_hblank(false);
	return true;
}

void check_repeated_output(
		capture_engine &capture,
		std::uint8_t held_page,
		std::uint64_t held_generation,
		std::uint64_t held_hash,
		bool programming)
{
	vga_tick_result const tick = clock_at_vsync_start(capture);
	REQUIRE(tick.vertical_sync_started);
	CHECK(tick.output_held == programming);
	CHECK(tick.frame_repeated);
	CHECK_FALSE(tick.output_changed);
	CHECK(capture.output_page() == held_page);
	CHECK(capture.output_generation() == held_generation);
	CHECK(test::frame_hash(capture.buffer(capture.output_page())) == held_hash);
}

void exercise_programming_freeze(register_page page, unsigned whole_programming_frames)
{
	core control;
	capture_engine capture;
	REQUIRE(capture.host_vblank(false).frame_started);
	capture.host_hblank(false);

	for (std::uint16_t row = 0; row < HOST_ACTIVE_LINES - 1; ++row)
		REQUIRE(mame_ordered_device_burst(control, capture, row, 0x19));
	REQUIRE(capture.host_vblank(true).boundary_pending);
	REQUIRE(mame_ordered_device_burst(control, capture, HOST_ACTIVE_LINES - 1, 0x19));
	REQUIRE(capture.host_vblank(false).frame_completed);
	REQUIRE(clock_at_vsync_start(capture).output_changed);

	std::uint8_t const held_page = capture.output_page();
	std::uint64_t const held_generation = capture.output_generation();
	std::uint64_t const held_hash = test::frame_hash(capture.buffer(held_page));

	for (std::uint16_t row = 0; row < 48; ++row)
		REQUIRE(mame_ordered_device_burst(control, capture, row, 0x43));
	REQUIRE(test::unlock_page(control, page).freeze);
	REQUIRE(capture.update_from_core(control));
	REQUIRE(capture.programming());

	for (std::uint16_t row = 48; row < HOST_ACTIVE_LINES - 1; ++row)
	{
		REQUIRE(mame_ordered_device_burst(control, capture, row, 0x62));
		if ((row & 0x0f) == 0)
			check_repeated_output(
					capture,
					held_page,
					held_generation,
					held_hash,
					true);
	}
	REQUIRE(capture.host_vblank(true).boundary_pending);
	REQUIRE(mame_ordered_device_burst(control, capture, HOST_ACTIVE_LINES - 1, 0x62));
	host_sync_result completed = capture.host_vblank(false);
	REQUIRE(completed.frame_completed);
	frame_buffer_state const &entry_partial = capture.buffer(completed.completed_page);
	CHECK(entry_partial.valid);
	CHECK_FALSE(entry_partial.complete);
	CHECK(entry_partial.programming_suppressed);
	CHECK(entry_partial.writes == 48 * 32);
	CHECK(entry_partial.active_lines == HOST_ACTIVE_LINES);
	CHECK(std::all_of(
			entry_partial.row_fetch_counts.begin(),
			entry_partial.row_fetch_counts.end(),
			[] (std::uint8_t count) { return count == 32; }));

	for (unsigned frame = 0; frame < whole_programming_frames; ++frame)
	{
		for (std::uint16_t row = 0; row < HOST_ACTIVE_LINES - 1; ++row)
		{
			REQUIRE(mame_ordered_device_burst(
					control,
					capture,
					row,
					std::uint8_t(0x80 + frame)));
			if ((row & 0x0f) == 0)
				check_repeated_output(
						capture,
						held_page,
						held_generation,
						held_hash,
						true);
		}
		REQUIRE(capture.host_vblank(true).boundary_pending);
		REQUIRE(mame_ordered_device_burst(
				control,
				capture,
				HOST_ACTIVE_LINES - 1,
				std::uint8_t(0x80 + frame)));
		REQUIRE(capture.host_vblank(false).frame_completed);
	}

	if (page == register_page::PAGE_00)
		CHECK(control.registers().font == 0x05);
	else
		CHECK(control.character_ram()[0] == character_stream_value(
				std::uint16_t(32 + 0x80 + whole_programming_frames - 1)));

	for (std::uint16_t row = 0; row < 64; ++row)
		REQUIRE(mame_ordered_device_burst(control, capture, row, 0xb4));
	std::uint32_t const writes_before_release = capture.state().host.working_writes;
	release_programming(control, capture);

	for (std::uint16_t row = 64; row < HOST_ACTIVE_LINES - 1; ++row)
	{
		REQUIRE(mame_ordered_device_burst(control, capture, row, 0xd7));
		CHECK(capture.state().host.working_writes == writes_before_release);
		if ((row & 0x07) == 0)
			check_repeated_output(
					capture,
					held_page,
					held_generation,
					held_hash,
					false);
	}
	REQUIRE(capture.host_vblank(true).boundary_pending);
	REQUIRE(mame_ordered_device_burst(control, capture, HOST_ACTIVE_LINES - 1, 0xd7));
	completed = capture.host_vblank(false);
	REQUIRE(completed.frame_completed);
	CHECK(capture.input_page() == (held_page ^ 1));
	check_repeated_output(
			capture,
			held_page,
			held_generation,
			held_hash,
			false);

	for (std::uint16_t row = 0; row < HOST_ACTIVE_LINES - 1; ++row)
	{
		REQUIRE(mame_ordered_device_burst(control, capture, row, 0xee));
		if ((row & 0x07) == 0)
			check_repeated_output(
					capture,
					held_page,
					held_generation,
					held_hash,
					false);
	}
	REQUIRE(capture.host_vblank(true).boundary_pending);
	REQUIRE(mame_ordered_device_burst(control, capture, HOST_ACTIVE_LINES - 1, 0xee));
	completed = capture.host_vblank(false);
	REQUIRE(completed.frame_completed);
	frame_buffer_state const &clean = capture.buffer(completed.completed_page);
	REQUIRE(clean.valid);
	REQUIRE(clean.complete);
	CHECK_FALSE(clean.programming_suppressed);
	CHECK(clean.active_lines == HOST_ACTIVE_LINES);

	vga_tick_result const switched = clock_at_vsync_start(capture);
	CHECK(switched.output_changed);
	CHECK_FALSE(switched.frame_repeated);
	CHECK_FALSE(switched.output_held);
	CHECK(capture.output_page() == completed.completed_page);
	CHECK(capture.output_generation() == completed.completed_generation);
	CHECK(test::frame_hash(capture.buffer(capture.output_page())) != held_hash);
}

// Expected rendered pixels for one doubled alphanumeric cell, derived from the
// captured cell and the character generator ROM fixture.
void check_rendered_cell(
		std::vector<render_pixel> const &pixels,
		core const &control,
		captured_cell const &cell,
		unsigned column,
		std::uint16_t output_row,
		std::uint8_t glyph_row)
{
	text_palette_descriptor const palette = control.text_palette(cell.pixel());
	render_pixel const foreground =
			renderer::pack_color(control.palette_color(palette.foreground));
	render_pixel const background =
			renderer::pack_color(control.palette_color(palette.background));
	std::uint8_t const bits = test::fpga_text_row(false, cell.data, glyph_row);
	render_pixel const *const line =
			pixels.data() + std::size_t(output_row) * RENDER_WIDTH + VIEWPORT_LEFT + column * 16;

	for (unsigned bit = 0; bit < 8; ++bit)
	{
		render_pixel const expected = (bits & (0x80U >> bit)) ? foreground : background;
		CHECK(line[bit * 2] == expected);
		CHECK(line[bit * 2 + 1] == expected);
	}
}

} // anonymous namespace


TEST_CASE("CoCoVGA raw FS and capture blanking contracts unlock during vertical blanking", "[cocovga][integration][sync][lock]")
{
	CHECK(vblank_from_fs_pin(false));
	CHECK_FALSE(vblank_from_fs_pin(true));
	CHECK(hblank_from_hs_pin(false));
	CHECK_FALSE(hblank_from_hs_pin(true));

	core control;
	capture_engine capture;
	auto clock_contract = [&control, &capture] (bool fs_pin_high, std::uint8_t selector)
	{
		capture.host_vblank(vblank_from_fs_pin(fs_pin_high));
		combo_step step = control.clock_combo(fs_pin_high, selector);
		for (unsigned clocks = 0;
				clocks < 2 &&
						(step.current == combo_state::WAIT_MODE_2 ||
								step.current == combo_state::WAIT_MODE_3);
				++clocks)
		{
			step = control.clock_combo(fs_pin_high, selector);
		}
		capture.update_from_core(control);
		return step;
	};

	CHECK(clock_contract(true, 0).current == combo_state::INIT);
	CHECK(clock_contract(false, 0).current == combo_state::READY);
	CHECK(clock_contract(false, core::COMBO_1).current == combo_state::UNLOCKED_1);
	CHECK(clock_contract(false, core::COMBO_2).current == combo_state::UNLOCKED_2);
	CHECK(clock_contract(false, core::COMBO_3).current == combo_state::UNLOCKED_3);
	CHECK(clock_contract(false, core::COMBO_4).current == combo_state::WAIT_MODE_1);
	combo_step const selected =
			clock_contract(false, static_cast<std::uint8_t>(register_page::PAGE_00));
	CHECK(selected.current == combo_state::UNLOCKED_A);
	CHECK(selected.page_captured);
	CHECK(selected.freeze);
	CHECK(control.freeze());
	CHECK(capture.programming());

	CHECK(clock_contract(true, 0).current == combo_state::UNLOCKED_B);
	combo_step const released = clock_contract(false, 0);
	CHECK(released.current == combo_state::READY);
	CHECK(released.programming_released);
	CHECK_FALSE(control.freeze());
	CHECK_FALSE(capture.programming());
}


TEST_CASE("CoCoVGA active mode tracking latches W64 after programming", "[cocovga][integration][lock][w64]")
{
	core control;
	capture_engine capture;

	REQUIRE(control.clock_combo(true, 10).current == combo_state::INIT);
	REQUIRE(test::unlock_page(control, register_page::PAGE_00).freeze);
	REQUIRE(capture.update_from_core(control));
	selected_page00_write(control, capture, 0, 0);
	selected_page00_write(
			control,
			capture,
			1,
			core::bank_bit(register_bank::ENHANCED_MODES));
	selected_page00_write(control, capture, 8, 0x02);

	control.latch_previous_field_mode(true, 2);
	CHECK_FALSE(control.state().w64_active);

	CHECK(control.clock_combo(true, 0).current == combo_state::UNLOCKED_B);
	combo_step const released = control.clock_combo(false, 0);
	REQUIRE(released.programming_released);
	CHECK_FALSE(control.state().w64_active);
	REQUIRE(control.clock_combo(true, 10).current == combo_state::INIT);
	REQUIRE(capture.update_from_core(control));

	CHECK(control.state().w64_active);
	CHECK_FALSE(control.state().vg6_active);
	CHECK_FALSE(capture.programming());
}


TEST_CASE("CoCoVGA MAME-ordered alpha capture stores and renders the fetched characters", "[cocovga][integration][capture][mame-order][oracle]")
{
	capture_engine capture;
	REQUIRE(capture.host_vblank(false).frame_started);
	capture.host_hblank(false);
	for (std::uint16_t row = 0; row < HOST_ACTIVE_LINES - 1; ++row)
		REQUIRE(mame_ordered_alpha_burst(capture, row, false, 0x19));

	REQUIRE(capture.host_vblank(true).boundary_pending);
	REQUIRE(mame_ordered_alpha_burst(
			capture,
			HOST_ACTIVE_LINES - 1,
			false,
			0x19));
	host_sync_result const boundary = capture.host_vblank(false);
	REQUIRE(boundary.frame_completed);
	REQUIRE(mame_ordered_alpha_burst(capture, 0, true, 0xa7));

	frame_buffer_state const &frame = capture.buffer(boundary.completed_page);
	REQUIRE(frame.complete);
	REQUIRE(frame.active_lines == HOST_ACTIVE_LINES);
	REQUIRE(capture.state().diagnostics.completed_frame_sam_mismatches == 0);

	// every captured cell holds the byte the host fetched for it
	for (std::uint16_t row = 0; row < HOST_ACTIVE_LINES; ++row)
	{
		for (std::uint8_t column = 0; column < 32; ++column)
		{
			captured_cell const &cell = frame.cells[row * 32 + column];
			captured_cell const expected = test::make_cell(
					mode_kind::ALPHA_SEMIGRAPHICS,
					false,
					ordered_alpha_data(row, column, 0x19));
			INFO("row " << row << " column " << unsigned(column));
			CHECK(cell.data == expected.data);
			CHECK(cell.gm == expected.gm);
			CHECK(cell.mode_id == expected.mode_id);
			CHECK(cell.flags == expected.flags);
		}
	}

	core control;
	renderer output = cocovga::test::make_renderer();
	std::vector<render_pixel> pixels;
	pixel_surface const surface = test::make_surface(pixels);
	render_result const rendered = output.render(control, frame, surface);
	REQUIRE(rendered.success());

	// and every viewport pixel follows from that cell and the character
	// generator ROM fixture rather than from a recorded frame hash
	for (std::uint16_t text_row = 0; text_row < HOST_ACTIVE_LINES / W64_GLYPH_HEIGHT; ++text_row)
	{
		for (std::uint8_t glyph_row = 0; glyph_row < W64_GLYPH_HEIGHT; ++glyph_row)
		{
			std::uint16_t const capture_row =
					std::uint16_t(text_row * W64_GLYPH_HEIGHT + glyph_row);
			for (unsigned duplicate = 0; duplicate < 2; ++duplicate)
			{
				std::uint16_t const output_row =
						std::uint16_t(VIEWPORT_TOP + capture_row * 2 + duplicate);
				for (unsigned column = 0; column < 32; ++column)
				{
					INFO("text row " << text_row << " glyph row " << unsigned(glyph_row)
							<< " duplicate " << duplicate << " column " << column);
					check_rendered_cell(
							pixels,
							control,
							frame.cells[capture_row * 32 + column],
							column,
							output_row,
							glyph_row);
				}
			}
		}
	}
}


TEST_CASE("CoCoVGA integrated RG6 rendering covers every effect, palette, model, CSS, and swap", "[cocovga][integration][artifact][hash]")
{
	constexpr std::array<artifact_mode, 5> modes =
	{{
		artifact_mode::STANDARD,
		artifact_mode::FAT_BITS,
		artifact_mode::SMARTIFACT,
		artifact_mode::MESS,
		artifact_mode::MONOCHROME
	}};

	renderer output = cocovga::test::make_renderer();
	for (model board : { model::AMC2, model::MODERN, model::T1 })
	{
		for (artifact_mode mode : modes)
		{
			for (bool css : { false, true })
			{
				for (bool swap : { false, true })
				{
					core control(board);
					std::uint8_t const artifact =
							mode == artifact_mode::MONOCHROME
									? std::uint8_t(swap ? 0x02 : 0x00)
									: std::uint8_t(
											0x01 |
											(swap ? 0x02 : 0x00) |
											(static_cast<unsigned>(mode) << 2));
					REQUIRE(test::write_register(control, register_bank::ARTIFACT, 4, artifact));
					REQUIRE(test::write_register(control, register_bank::EXTRAS, 5, 0x47));

					REQUIRE(control.write_page00(
							1,
							core::bank_bit(register_bank::ARTIFACT_PALETTE) |
							core::bank_bit(register_bank::EXTRA_PALETTE)));
					for (unsigned slot = 0; slot < ARTIFACT_PALETTE_ENTRIES; slot += 3)
						write_palette_word(control, std::uint16_t(64 + slot * 2), palette_word(slot + unsigned(board)));
					write_palette_word(
							control,
							132,
							core::pack_rgb15({ 31, std::uint8_t(4 + unsigned(board) * 5), 2 }) | 0x8000);
					write_palette_word(
							control,
							134,
							core::pack_rgb15({ 2, 9, std::uint8_t(20 + unsigned(board) * 3) }) | 0x8000);
					REQUIRE(control.commit_palettes_at_vsync());

					frame_buffer_state const frame = test::make_native_frame(
							mode_kind::RG6,
							css,
							std::uint8_t(
									0x21 +
									unsigned(mode) * 19U +
									unsigned(board) * 37U +
									(swap ? 7U : 0U)));
					render_options options;
					options.border_status.kind = render_effects::border_status_kind::MODE;
					options.border_status.visible = true;
					options.border_status.graphics = true;
					options.border_status.gm = 7;

					std::vector<render_pixel> pixels;
					pixel_surface const surface = test::make_surface(pixels);
					render_result const result = output.render(control, frame, surface, options);
					INFO("model " << unsigned(board) << " mode " << unsigned(mode)
							<< " css " << css << " swap " << swap);
					REQUIRE(result.success());
					CHECK(pixels[0] ==
							render_effects::border_color(
									control,
									mode_kind::RG6,
									frame.cells[0].pixel()).color.argb);
					CHECK(pixels[RENDER_WIDTH] == render_effects::scanline_color(control).argb);
				}
			}
		}
	}

	CHECK(output.state().frames_rendered == 60);
}


TEST_CASE("CoCoVGA integrated renderer combines native character RAM, W64, VG6, overlays, and buttons", "[cocovga][integration][extended][hash]")
{
	renderer output = cocovga::test::make_renderer();

	core native(model::MODERN);
	for (std::uint16_t address = 0; address < CHARACTER_RAM_BYTES; ++address)
		REQUIRE(native.write_character(address, std::uint8_t(address * 17U + (address >> 3))));
	REQUIRE(test::write_register(native, register_bank::FONT, 3, 0x06));
	REQUIRE(test::write_register(native, register_bank::ENHANCED_MODES, 8, 0x04));
	REQUIRE(test::write_register(native, register_bank::EXTRAS, 5, 0xf7));
	REQUIRE(native.write_page00(
			1,
			core::bank_bit(register_bank::EXTRA_PALETTE) |
			core::bank_bit(register_bank::SEMIGRAPHICS_PALETTE)));
	write_palette_word(native, 32, palette_word(41));
	write_palette_word(native, 128, palette_word(42));
	write_palette_word(native, 130, palette_word(43));
	write_palette_word(native, 132, palette_word(44));
	write_palette_word(native, 134, palette_word(45));
	REQUIRE(native.commit_palettes_at_vsync());
	frame_buffer_state const native_frame =
			test::make_native_frame(mode_kind::ALPHA_SEMIGRAPHICS, true, 0x17);
	render_options boot;
	boot.border_status.kind = render_effects::border_status_kind::BOOT_VERSION;
	boot.border_status.visible = true;
	std::uint64_t const native_hash = rendered_hash(output, native, native_frame, boot);

	core w64(model::T1);
	for (std::uint16_t address = 0; address < CHARACTER_RAM_BYTES; ++address)
		REQUIRE(w64.write_character(address, std::uint8_t(address * 31U + 0x69U)));
	REQUIRE(test::write_register(w64, register_bank::FONT, 3, 0x06));
	REQUIRE(test::write_register(w64, register_bank::ENHANCED_MODES, 8, 0x06));
	REQUIRE(test::write_register(w64, register_bank::EXTRAS, 5, 0xc3));
	w64.state().buttons.text_button_1_cycle = 5;
	w64.state().buttons.text_button_2_cycle =
			static_cast<std::uint8_t>(text_palette_choice::BLUE_ON_WHITE);
	w64.state().buttons.force_lowercase_toggle = true;
	w64.state().w64_active = true;
	frame_buffer_state const w64_frame = test::make_w64_frame(false, 0x4d);
	render_options w64_status;
	w64_status.border_status.kind = render_effects::border_status_kind::MODE;
	w64_status.border_status.visible = true;
	w64_status.border_status.graphics = true;
	w64_status.border_status.gm = 2;
	std::uint64_t const w64_hash = rendered_hash(output, w64, w64_frame, w64_status);

	core vg6(model::AMC2);
	REQUIRE(test::write_register(vg6, register_bank::ENHANCED_MODES, 8, 0x01));
	REQUIRE(test::write_register(vg6, register_bank::EXTRAS, 5, 0x44));
	REQUIRE(vg6.write_page00(
			1,
			core::bank_bit(register_bank::ARTIFACT_PALETTE) |
			core::bank_bit(register_bank::EXTRA_PALETTE)));
	for (unsigned slot = 0; slot < ARTIFACT_PALETTE_ENTRIES; ++slot)
		write_palette_word(vg6, std::uint16_t(64 + slot * 2), palette_word(slot + 73));
	write_palette_word(vg6, 134, palette_word(109));
	REQUIRE(vg6.commit_palettes_at_vsync());
	vg6.state().buttons.graphics_button_1_cycle = 2;
	vg6.state().vg6_active = true;
	frame_buffer_state const vg6_frame = test::make_vg6_frame(true, 0xb1);
	render_options vg6_status;
	vg6_status.border_status.kind = render_effects::border_status_kind::MODE;
	vg6_status.border_status.visible = true;
	vg6_status.border_status.graphics = true;
	vg6_status.border_status.gm = 6;
	std::uint64_t const vg6_hash = rendered_hash(output, vg6, vg6_frame, vg6_status);

	CHECK(output.state().frames_rendered == 3);
	CHECK(output.state().last_first_mode == mode_kind::VG6);
	CHECK(output.state().last_last_mode == mode_kind::VG6);
	CHECK(native_hash != w64_hash);
	CHECK(native_hash != vg6_hash);
	CHECK(w64_hash != vg6_hash);
}


TEST_CASE("CoCoVGA capture output page and latency feed current renderer controls through programming", "[cocovga][integration][capture][latency][hash]")
{
	core control;
	capture_engine capture;
	renderer output = cocovga::test::make_renderer();
	host_sync_result completion;

	REQUIRE(test::capture_frame(capture, mode_kind::RG6, false, 0x21, completion));
	REQUIRE(clock_at_vsync_start(capture).output_changed);
	CHECK(capture.output_page() == 0);
	CHECK(capture.output_generation() == 1);
	CHECK(capture.output_latency_frames() == 1);
	std::uint64_t const first_hash = rendered_hash(
			output,
			control,
			capture.buffer(capture.output_page()));

	REQUIRE(test::capture_frame(capture, mode_kind::RG6, true, 0x91, completion));
	CHECK(capture.output_generation() == 1);
	CHECK(rendered_hash(output, control, capture.buffer(capture.output_page())) == first_hash);
	REQUIRE(clock_at_vsync_start(capture).output_changed);
	CHECK(capture.output_page() == 1);
	CHECK(capture.output_generation() == 2);
	CHECK(capture.output_latency_frames() == 1);
	std::uint64_t const second_hash = rendered_hash(
			output,
			control,
			capture.buffer(capture.output_page()));
	CHECK(second_hash != first_hash);

	REQUIRE(test::unlock_page(control, register_page::PAGE_00).freeze);
	REQUIRE(capture.update_from_core(control));
	std::uint8_t const held_page = capture.output_page();
	std::uint64_t const held_generation = capture.output_generation();
	selected_page00_write(control, capture, 0, 0);
	selected_page00_write(
			control,
			capture,
			1,
			core::bank_bit(register_bank::ARTIFACT) |
			core::bank_bit(register_bank::EXTRAS) |
			core::bank_bit(register_bank::EXTRA_PALETTE));
	selected_page00_write(control, capture, 4, 0);
	selected_page00_write(control, capture, 5, 0x47);
	std::uint16_t const scanline = palette_word(127);
	selected_page00_write(control, capture, 134, std::uint8_t(scanline >> 8));
	selected_page00_write(control, capture, 135, std::uint8_t(scanline));
	vga_tick_result const held = clock_at_vsync_start(capture);
	CHECK(held.output_held);
	CHECK(capture.output_page() == held_page);
	CHECK(capture.output_generation() == held_generation);

	release_programming(control, capture);
	REQUIRE(control.commit_palettes_at_vsync());
	std::uint64_t const programmed_hash = rendered_hash(
			output,
			control,
			capture.buffer(capture.output_page()));
	CHECK(programmed_hash != second_hash);

}

TEST_CASE("CoCoVGA MAME-ordered register streams hold output through programming release", "[cocovga][integration][capture][lock][mame-order][hash]")
{
	SECTION("Page 00 partial and whole streams")
	{
		exercise_programming_freeze(register_page::PAGE_00, 2);
	}

	SECTION("Page 18 partial and whole streams")
	{
		exercise_programming_freeze(register_page::PAGE_18, 1);
	}
}

TEST_CASE("CoCoVGA render preflight rejection holds page generation and capture hash", "[cocovga][integration][capture][renderer][fallback][hash]")
{
	core control;
	capture_engine capture;
	renderer output = cocovga::test::make_renderer();
	host_sync_result completed;

	REQUIRE(test::capture_frame(
			capture,
			mode_kind::ALPHA_SEMIGRAPHICS,
			false,
			0x31,
			completed));
	REQUIRE(clock_at_vsync_start(capture).output_changed);
	std::uint8_t const held_page = capture.output_page();
	std::uint64_t const held_generation = capture.output_generation();
	std::uint64_t const held_hash = test::frame_hash(capture.buffer(held_page));

	REQUIRE(test::capture_frame(
			capture,
			mode_kind::ALPHA_SEMIGRAPHICS,
			true,
			0x72,
			completed));
	std::uint8_t candidate_page = 0;
	frame_buffer_state const *candidate = nullptr;
	REQUIRE(capture.output_candidate(candidate_page, candidate));
	REQUIRE(candidate != nullptr);
	REQUIRE(candidate->generation == completed.completed_generation);

	control.state().w64_active = true;
	render_result const failed_preflight = output.preflight(control, *candidate);
	REQUIRE(failed_preflight.status == render_status::INCONSISTENT_CAPTURE);
	REQUIRE(capture.reject_output_candidate(
			candidate_page,
			candidate->generation,
			output_reject_reason::RENDER_PREFLIGHT));

	vga_tick_result rejected = clock_at_vsync_start(capture);
	CHECK(rejected.output_rejected);
	CHECK(rejected.reject_reason == output_reject_reason::RENDER_PREFLIGHT);
	CHECK(rejected.frame_repeated);
	CHECK_FALSE(rejected.output_changed);
	CHECK(capture.output_page() == held_page);
	CHECK(capture.output_generation() == held_generation);
	CHECK(test::frame_hash(capture.buffer(capture.output_page())) == held_hash);

	control.state().w64_active = false;
	REQUIRE(output.preflight(control, *candidate).success());
	capture.allow_output_candidate(candidate_page, candidate->generation);
	vga_tick_result const switched = clock_at_vsync_start(capture);
	CHECK(switched.output_changed);
	CHECK_FALSE(switched.output_rejected);
	CHECK(capture.output_page() == candidate_page);
	CHECK(capture.output_generation() == candidate->generation);
}

TEST_CASE("CoCoVGA transactional rendering repeats the last successful bitmap on failure", "[cocovga][integration][renderer][fallback][hash]")
{
	core control;
	REQUIRE(test::write_register(control, register_bank::ARTIFACT, 4, 0));
	renderer output = cocovga::test::make_renderer();
	std::vector<render_pixel> presented_pixels;
	std::vector<render_pixel> staging_pixels;
	pixel_surface const presented = test::make_surface(presented_pixels);
	pixel_surface const staging = test::make_surface(staging_pixels);
	std::fill(presented_pixels.begin(), presented_pixels.end(), 0xff123456U);

	frame_buffer_state first = test::make_native_frame(mode_kind::CG2, false, 0x21);
	first.generation = 1;
	render_result result = output.render_preserving(
			control,
			first,
			presented,
			staging);
	REQUIRE(result.success());
	std::uint64_t const first_hash =
			test::pixel_hash(presented_pixels.data(), presented_pixels.size());

	frame_buffer_state failed = test::make_native_frame(mode_kind::CG2, true, 0x72);
	failed.generation = 2;
	failed.cells[0].gm = 7;
	result = output.render_preserving(
			control,
			failed,
			presented,
			staging);
	CHECK(result.status == render_status::INCONSISTENT_CAPTURE);
	CHECK(result.generation == 2);
	CHECK(test::pixel_hash(presented_pixels.data(), presented_pixels.size()) == first_hash);

	frame_buffer_state clean = test::make_native_frame(mode_kind::CG2, true, 0xb4);
	clean.generation = 3;
	result = output.render_preserving(
			control,
			clean,
			presented,
			staging);
	REQUIRE(result.success());
	CHECK(result.generation == 3);
	CHECK(test::pixel_hash(presented_pixels.data(), presented_pixels.size()) != first_hash);
	CHECK(output.state().render_calls == 3);
	CHECK(output.state().frames_rendered == 2);
}


TEST_CASE("CoCoVGA combined state restores exactly during a register upload", "[cocovga][integration][restore][registers][hash]")
{
	core uninterrupted;
	capture_engine uninterrupted_capture;
	renderer uninterrupted_renderer = cocovga::test::make_renderer();
	render_effects::border_status_overlay uninterrupted_status;
	host_sync_result completion;
	REQUIRE(test::capture_frame(
			uninterrupted_capture,
			mode_kind::ALPHA_SEMIGRAPHICS,
			false,
			0x35,
			completion));
	REQUIRE(clock_at_vsync_start(uninterrupted_capture).output_changed);
	rendered_hash(
			uninterrupted_renderer,
			uninterrupted,
			uninterrupted_capture.buffer(uninterrupted_capture.output_page()));
	uninterrupted_status.observe_mode(
			uninterrupted_capture.output_cell(13 * 32).pixel());
	uninterrupted_status.advance(12'345'678);

	REQUIRE(test::unlock_page(uninterrupted, register_page::PAGE_00).freeze);
	REQUIRE(uninterrupted_capture.update_from_core(uninterrupted));
	std::uint8_t const edit_mask =
			core::bank_bit(register_bank::FONT) |
			core::bank_bit(register_bank::ARTIFACT) |
			core::bank_bit(register_bank::EXTRAS) |
			core::bank_bit(register_bank::SEMIGRAPHICS_PALETTE) |
			core::bank_bit(register_bank::ARTIFACT_PALETTE) |
			core::bank_bit(register_bank::EXTRA_PALETTE) |
			core::bank_bit(register_bank::VGA_TIMING) |
			core::bank_bit(register_bank::ENHANCED_MODES);
	const std::array<std::pair<std::uint16_t, std::uint8_t>, 22> writes =
	{{
		{ 0, 0x00 },
		{ 1, edit_mask },
		{ 3, 0x06 },
		{ 4, 0x09 },
		{ 5, 0xf3 },
		{ 8, 0x04 },
		{ 32, std::uint8_t(palette_word(151) >> 8) },
		{ 33, std::uint8_t(palette_word(151)) },
		{ 82, std::uint8_t(palette_word(152) >> 8) },
		{ 83, std::uint8_t(palette_word(152)) },
		{ 128, std::uint8_t(palette_word(153) >> 8) },
		{ 129, std::uint8_t(palette_word(153)) },
		{ 130, std::uint8_t(palette_word(154) >> 8) },
		{ 131, std::uint8_t(palette_word(154)) },
		{ 132, std::uint8_t(palette_word(155) >> 8) },
		{ 133, std::uint8_t(palette_word(155)) },
		{ 480, 0x01 },
		{ 481, 0x40 },
		{ 482, 8 },
		{ 483, 96 },
		{ 484, 56 },
		{ 486, 0x01 }
	}};
	for (std::size_t index = 0; index <= 16; ++index)
		selected_page00_write(
				uninterrupted,
				uninterrupted_capture,
				writes[index].first,
				writes[index].second);
	CHECK_FALSE(uninterrupted_capture.state().vga.pending_timing_valid);

	core_state const saved_core = uninterrupted.state();
	capture_engine_state const saved_capture = uninterrupted_capture.state();
	renderer_state const saved_renderer = uninterrupted_renderer.state();
	render_effects::border_status_state const saved_status = uninterrupted_status.state();

	core restored;
	restored.restore_state(saved_core);
	capture_engine restored_capture;
	restored_capture.restore_state(saved_capture);
	renderer restored_renderer = cocovga::test::make_renderer();
	restored_renderer.restore_state(saved_renderer);
	render_effects::border_status_overlay restored_status;
	restored_status.restore_state(saved_status);

	for (std::size_t index = 17; index < writes.size(); ++index)
	{
		selected_page00_write(
				uninterrupted,
				uninterrupted_capture,
				writes[index].first,
				writes[index].second);
		selected_page00_write(
				restored,
				restored_capture,
				writes[index].first,
				writes[index].second);
	}
	for (core *const current : { &uninterrupted, &restored })
	{
		capture_engine &current_capture =
				current == &uninterrupted ? uninterrupted_capture : restored_capture;
		selected_page00_write(*current, current_capture, 487, 0xe0);
		selected_page00_write(*current, current_capture, 488, 24);
		selected_page00_write(*current, current_capture, 489, 2);
		selected_page00_write(*current, current_capture, 490, 19);
		release_programming(*current, current_capture);
		REQUIRE(current->commit_palettes_at_vsync());
		REQUIRE(clock_at_frame_boundary(current_capture).timing_applied);
	}

	CHECK(vga_timing_equal(
			uninterrupted_capture.state().vga.active_timing,
			restored_capture.state().vga.active_timing));
	CHECK(uninterrupted.registers().font == restored.registers().font);
	CHECK(uninterrupted.registers().artifact == restored.registers().artifact);
	CHECK(uninterrupted.registers().extras == restored.registers().extras);
	CHECK(uninterrupted.registers().enhanced_modes == restored.registers().enhanced_modes);
	CHECK(uninterrupted.registers().semigraphics_palette_visible ==
			restored.registers().semigraphics_palette_visible);
	CHECK(uninterrupted.registers().artifact_palette_visible ==
			restored.registers().artifact_palette_visible);
	CHECK(uninterrupted.registers().extra_palette_visible ==
			restored.registers().extra_palette_visible);

	uninterrupted_status.advance(98'765);
	restored_status.advance(98'765);
	CHECK(uninterrupted_status.state().boot_remaining == restored_status.state().boot_remaining);
	CHECK(uninterrupted_status.state().mode_remaining == restored_status.state().mode_remaining);

	render_options uninterrupted_options;
	uninterrupted_options.border_status = uninterrupted_status.view();
	render_options restored_options;
	restored_options.border_status = restored_status.view();
	std::uint64_t const uninterrupted_hash = rendered_hash(
			uninterrupted_renderer,
			uninterrupted,
			uninterrupted_capture.buffer(uninterrupted_capture.output_page()),
			uninterrupted_options);
	std::uint64_t const restored_hash = rendered_hash(
			restored_renderer,
			restored,
			restored_capture.buffer(restored_capture.output_page()),
			restored_options);
	CHECK(restored_hash == uninterrupted_hash);
	CHECK(restored_renderer.state().render_calls == uninterrupted_renderer.state().render_calls);
	CHECK(restored_renderer.state().frames_rendered == uninterrupted_renderer.state().frames_rendered);
}


TEST_CASE("CoCoVGA Page 18 upload and W64 rendering restore exactly mid-character", "[cocovga][integration][restore][character-ram][w64][hash]")
{
	core uninterrupted;
	capture_engine uninterrupted_capture;
	REQUIRE(test::unlock_page(uninterrupted, register_page::PAGE_18).freeze);
	REQUIRE(uninterrupted_capture.update_from_core(uninterrupted));

	constexpr std::uint16_t split = 3'077;
	for (std::uint16_t address = 0; address < split; ++address)
	{
		REQUIRE(uninterrupted.ingest_selected_capture(address, character_stream_value(address)));
		REQUIRE(uninterrupted_capture.update_from_core(uninterrupted));
	}

	core restored;
	restored.restore_state(uninterrupted.state());
	capture_engine restored_capture;
	restored_capture.restore_state(uninterrupted_capture.state());

	for (std::uint16_t address = split; address < CHARACTER_RAM_BYTES * 2; ++address)
	{
		std::uint8_t const value = character_stream_value(address);
		REQUIRE(uninterrupted.ingest_selected_capture(address, value));
		REQUIRE(restored.ingest_selected_capture(address, value));
		REQUIRE(uninterrupted_capture.update_from_core(uninterrupted));
		REQUIRE(restored_capture.update_from_core(restored));
	}
	release_programming(uninterrupted, uninterrupted_capture);
	release_programming(restored, restored_capture);

	for (std::uint16_t address = 0; address < CHARACTER_RAM_BYTES; ++address)
	{
		std::uint16_t const final_capture_address = std::uint16_t(
				((address >> 5) << 6) |
				0x20 |
				(address & 0x1f));
		std::uint8_t const expected = character_stream_value(final_capture_address);
		INFO("character RAM address " << address);
		CHECK(uninterrupted.character_ram()[address] == expected);
		CHECK(restored.character_ram()[address] == expected);
	}

	uninterrupted.state().registers.enhanced_modes = 0x04;
	restored.state().registers.enhanced_modes = 0x04;
	uninterrupted.state().w64_active = true;
	restored.state().w64_active = true;
	frame_buffer_state const frame = test::make_w64_frame(true, 0x6b);
	render_options options;
	options.border_status.kind = render_effects::border_status_kind::MODE;
	options.border_status.visible = true;
	options.border_status.graphics = true;
	options.border_status.gm = 2;
	renderer uninterrupted_renderer = cocovga::test::make_renderer();
	renderer restored_renderer = cocovga::test::make_renderer();
	std::uint64_t const uninterrupted_hash =
			rendered_hash(uninterrupted_renderer, uninterrupted, frame, options);
	std::uint64_t const restored_hash =
			rendered_hash(restored_renderer, restored, frame, options);
	CHECK(restored_hash == uninterrupted_hash);
}
