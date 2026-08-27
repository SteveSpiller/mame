// license:BSD-3-Clause
// copyright-holders:Stephen Spiller

#include "catch.hpp"

#include "cocovga_test_helpers.h"

#include <array>
#include <cstdint>
#include <limits>
#include <tuple>
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

std::uint8_t ordered_data(std::uint16_t row, std::uint8_t column, std::uint8_t seed) noexcept
{
	return std::uint8_t(seed + row * 29U + column * 7U);
}

host_fetch_event ordered_event(
		mode_kind mode,
		std::uint16_t row,
		std::uint8_t column,
		bool css,
		std::uint8_t seed) noexcept
{
	host_fetch_event event;
	event.sam_address = capture_engine::expected_sam_offset(mode, row, column);
	event.data = ordered_data(row, column, seed);
	event.mode.gm = test::gm_for_mode(mode);
	event.mode.graphics = test::graphics_for_mode(mode);
	event.mode.css = css;
	event.phase = host_phase::ACTIVE;
	event.sam_address_valid = true;
	event.sam_address_is_offset = true;
	return event;
}

bool mame_ordered_burst(
		capture_engine &capture,
		mode_kind mode,
		std::uint16_t row,
		bool css,
		std::uint8_t seed,
		capture_result *first_result = nullptr) noexcept
{
	capture.host_hblank(true);
	std::uint8_t const count = capture_engine::fetches_per_host_line(mode);
	for (std::uint8_t column = 0; column < count; ++column)
	{
		capture_result const result =
				capture.host_fetch(ordered_event(mode, row, column, css, seed), mode);
		if (column == 0 && first_result != nullptr)
			*first_result = result;
		if (!result.sampled || (!capture.programming() && !result.buffer_written))
			return false;
	}
	capture.host_hblank(false);
	return true;
}

bool capture_mode_transition_frame(
		capture_engine &capture,
		mode_kind first_mode,
		mode_kind last_mode,
		std::uint16_t transition_row,
		std::uint8_t seed,
		host_sync_result &completion) noexcept
{
	if (!capture.state().host.frame_started)
	{
		if (!capture.host_vblank(false).frame_started)
			return false;
	}
	capture.host_hblank(false);

	for (std::uint16_t row = 0; row < HOST_ACTIVE_LINES; ++row)
	{
		mode_kind const mode = row < transition_row ? first_mode : last_mode;
		if (!mame_ordered_burst(capture, mode, row, false, seed))
			return false;
	}

	if (!capture.host_vblank(true).boundary_pending)
		return false;
	completion = capture.host_vblank(false);
	return completion.frame_completed;
}

bool capture_with_mixed_row(
		capture_engine &capture,
		mode_kind first_mode,
		mode_kind last_mode,
		std::uint16_t mixed_row,
		std::uint8_t transition_column,
		std::uint8_t row_fetches,
		std::uint8_t seed,
		host_sync_result &completion) noexcept
{
	if (!capture.state().host.frame_started)
	{
		if (!capture.host_vblank(false).frame_started)
			return false;
	}
	capture.host_hblank(false);

	for (std::uint16_t row = 0; row < HOST_ACTIVE_LINES; ++row)
	{
		if (row != mixed_row)
		{
			mode_kind const mode = row < mixed_row ? first_mode : last_mode;
			if (!mame_ordered_burst(capture, mode, row, false, seed))
				return false;
			continue;
		}

		capture.host_hblank(true);
		for (std::uint8_t column = 0; column < row_fetches; ++column)
		{
			mode_kind const mode =
					column < transition_column ? first_mode : last_mode;
			host_fetch_event event = ordered_event(mode, row, column, false, seed);
			event.sam_address_valid = false;
			capture_result const result = capture.host_fetch(event, mode);
			if (!result.sampled || !result.buffer_written)
				return false;
		}
		capture.host_hblank(false);
	}

	if (!capture.host_vblank(true).boundary_pending)
		return false;
	completion = capture.host_vblank(false);
	return completion.frame_completed;
}

} // anonymous namespace


TEST_CASE("CoCoVGA capture reset and derived state defaults are exact", "[cocovga][capture]")
{
	capture_engine capture;
	capture_engine_state const &state = capture.state();
	vga_timing_endpoints const expected = make_vga_timing_endpoints(vga_timing{});

	CHECK(state.host.frames_completed == 0);
	CHECK(state.host.working_writes == 0);
	CHECK(std::all_of(
			state.host.working_row_fetch_counts.begin(),
			state.host.working_row_fetch_counts.end(),
			[] (std::uint8_t count) { return count == 0; }));
	CHECK(state.host.active_line == 0);
	CHECK(state.host.column == 0);
	CHECK(state.host.input_page == 1);
	CHECK_FALSE(state.host.frame_started);
	CHECK(state.host.fsync_asserted);
	CHECK(state.host.hsync_asserted);
	CHECK_FALSE(state.host.line_had_active_fetch);
	CHECK_FALSE(state.host.frame_programming_suppressed);
	CHECK_FALSE(state.host.frame_boundary_pending);
	CHECK_FALSE(state.host.line_boundary_pending);
	CHECK_FALSE(state.host.late_burst_active);

	CHECK(vga_timing_equal(state.vga.active_timing, vga_timing{}));
	CHECK(vga_timing_equal(state.vga.pending_timing, vga_timing{}));
	CHECK(state.vga.pixel_clocks == 0);
	CHECK(state.vga.frames == 0);
	CHECK(state.vga.selections == 0);
	CHECK(state.vga.repeated_frames == 0);
	CHECK(state.vga.held_frames == 0);
	CHECK(state.vga.rejected_candidates == 0);
	CHECK(state.vga.output_generation == 0);
	CHECK(state.vga.rejected_generation == 0);
	CHECK(state.vga.hcounter == 0);
	CHECK(state.vga.vcounter == 0);
	CHECK(state.vga.output_page == 0);
	CHECK(state.vga.rejected_page == 0);
	CHECK(state.vga.rejected_reason == output_reject_reason::NONE);
	CHECK_FALSE(state.vga.pending_timing_valid);
	CHECK_FALSE(state.vga.hsync_asserted);
	CHECK_FALSE(state.vga.vsync_asserted);
	CHECK_FALSE(state.vga.blanking);
	CHECK_FALSE(state.vga.output_valid);
	CHECK_FALSE(state.vga.rejected_candidate_valid);
	CHECK(state.diagnostics.frame_sam_comparisons == 0);
	CHECK(state.diagnostics.frame_sam_mismatches == 0);
	CHECK(state.diagnostics.completed_frame_sam_comparisons == 0);
	CHECK(state.diagnostics.completed_frame_sam_mismatches == 0);
	CHECK_FALSE(state.diagnostics.sam_warning_issued);
	CHECK_FALSE(state.programming);
	CHECK(capture.timing_endpoints().h_total == expected.h_total);
	CHECK(capture.timing_endpoints().v_total == expected.v_total);

	for (frame_buffer_state const &buffer : state.buffers)
	{
		CHECK(std::all_of(
				buffer.row_fetch_counts.begin(),
				buffer.row_fetch_counts.end(),
				[] (std::uint8_t count) { return count == 0; }));
		CHECK(buffer.generation == 0);
		CHECK(buffer.writes == 0);
		CHECK_FALSE(buffer.valid);
		CHECK_FALSE(buffer.complete);
		CHECK_FALSE(buffer.programming_suppressed);
	}

	vga_timing invalid{};
	invalid.h_active = 0;
	capture.reset(invalid);
	CHECK(vga_timing_equal(capture.state().vga.active_timing, vga_timing{}));

	vga_timing custom{ 8, 2, 3, 4, 6, 1, 2, 3 };
	capture.reset(custom);
	CHECK(vga_timing_equal(capture.state().vga.active_timing, custom));
	CHECK(capture.timing_endpoints().h_total == 17);
	CHECK(capture.timing_endpoints().v_total == 12);
}


TEST_CASE("CoCoVGA captured cells preserve alpha and graphics wire formats", "[cocovga][capture]")
{
	host_mode_pins pins;
	pins.gm = 0xff;
	pins.graphics = true;
	pins.css = true;
	pins.alpha_semigraphics = true;
	pins.inverse = true;
	pins.internal_external = true;
	pixel_input const input = pins.pixel(0xa5);
	CHECK(input.data == 0xa5);
	CHECK(input.gm == 7);
	CHECK(input.graphics);
	CHECK(input.css);
	CHECK(input.alpha_semigraphics);
	CHECK(input.inverse);
	CHECK(input.internal_external);

	captured_cell alpha = test::make_cell(
			mode_kind::ALPHA_SEMIGRAPHICS,
			true,
			0xff,
			true,
			true,
			true);
	CHECK(alpha.mode() == mode_kind::ALPHA_SEMIGRAPHICS);
	CHECK(alpha.alpha_format());
	CHECK(alpha.data == 0x3f);
	CHECK(alpha.stream_byte() == 0xff);
	CHECK(alpha.pixel().alpha_semigraphics);
	CHECK(alpha.pixel().inverse);
	CHECK(alpha.pixel().internal_external);

	captured_cell graphics = test::make_cell(
			mode_kind::CG6,
			true,
			0xa5,
			true,
			true,
			true);
	CHECK(graphics.mode() == mode_kind::CG6);
	CHECK_FALSE(graphics.alpha_format());
	CHECK(graphics.data == 0xa5);
	CHECK(graphics.stream_byte() == 0xa5);
	CHECK(graphics.pixel().graphics);
	CHECK(graphics.pixel().css);
	CHECK(graphics.pixel().internal_external);

	graphics.mode_id = 0xff;
	CHECK(graphics.mode() == mode_kind::ALPHA_SEMIGRAPHICS);
	CHECK(graphics.alpha_format());
}


TEST_CASE("CoCoVGA capture address and cadence rules cover every mode", "[cocovga][capture][modes]")
{
	struct address_case
	{
		mode_kind mode;
		std::uint8_t fetches;
		std::uint16_t source_height;
		std::uint8_t bytes_per_row;
	};

	constexpr std::array<address_case, 11> cases =
	{{
		{ mode_kind::ALPHA_SEMIGRAPHICS, 32, 16, 32 },
		{ mode_kind::CG1, 16, 64, 16 },
		{ mode_kind::RG1, 16, 64, 16 },
		{ mode_kind::CG2, 32, 64, 32 },
		{ mode_kind::RG2, 16, 96, 16 },
		{ mode_kind::CG3, 32, 96, 32 },
		{ mode_kind::RG3, 16, 192, 16 },
		{ mode_kind::CG6, 32, 192, 32 },
		{ mode_kind::RG6, 32, 192, 32 },
		{ mode_kind::VG6, 32, 192, 32 },
		{ mode_kind::W64, 32, 64, 32 }
	}};

	for (address_case const &expected : cases)
	{
		INFO("mode " << unsigned(expected.mode));
		CHECK(capture_engine::fetches_per_host_line(expected.mode) == expected.fetches);
		for (std::uint16_t line : { std::uint16_t(0), std::uint16_t(1), std::uint16_t(11), std::uint16_t(63), std::uint16_t(191) })
		{
			for (std::uint8_t column : { std::uint8_t(0), std::uint8_t(expected.fetches - 1) })
			{
				std::uint16_t const input_address =
						expected.mode == mode_kind::W64
								? std::uint16_t((line / 3) * 32 + column)
								: std::uint16_t(line * 32 + column);
				std::uint16_t source_offset;
				if (expected.mode == mode_kind::ALPHA_SEMIGRAPHICS)
					source_offset = std::uint16_t((line / 12) * 32 + column);
				else if (expected.mode == mode_kind::W64)
					source_offset = std::uint16_t((line / 3) * 32 + column);
				else if (expected.mode == mode_kind::VG6)
					source_offset = std::uint16_t(line * 32 + column);
				else
					source_offset = std::uint16_t(
							(line / (HOST_ACTIVE_LINES / expected.source_height)) *
									expected.bytes_per_row +
							column);

				CHECK(capture_engine::reconstruct_input_address(expected.mode, line, column) == input_address);
				CHECK(capture_engine::expected_sam_offset(expected.mode, line, column) == source_offset);
			}
		}

		CHECK(capture_engine::reconstruct_input_address(expected.mode, HOST_ACTIVE_LINES, 0) == INVALID_CAPTURE_ADDRESS);
		CHECK(capture_engine::expected_sam_offset(expected.mode, HOST_ACTIVE_LINES, 0) == INVALID_CAPTURE_ADDRESS);
		CHECK(capture_engine::reconstruct_input_address(expected.mode, 0, expected.fetches) == INVALID_CAPTURE_ADDRESS);
		CHECK(capture_engine::expected_sam_offset(expected.mode, 0, expected.fetches) == INVALID_CAPTURE_ADDRESS);
	}

	CHECK(capture_engine::fetches_per_host_line(static_cast<mode_kind>(0xff)) == 32);
	CHECK(capture_engine::expected_sam_offset(static_cast<mode_kind>(0xff), 12, 3) == 35);
}


TEST_CASE("CoCoVGA host capture reports every reject path and SAM diagnostic", "[cocovga][capture]")
{
	capture_engine capture;
	host_fetch_event event;
	event.data = 0xff;
	event.mode.alpha_semigraphics = true;
	event.mode.inverse = true;
	event.mode.css = true;
	event.mode.internal_external = true;
	event.phase = host_phase::ACTIVE;
	event.sam_address_valid = true;
	event.sam_address = 0x4000;

	capture_result result = capture.host_fetch(event, mode_kind::ALPHA_SEMIGRAPHICS);
	CHECK_FALSE(result.sampled);
	CHECK(result.reject_reason == capture_reject_reason::NO_HOST_FRAME);

	REQUIRE(capture.host_fsync(false).frame_started);
	result = capture.host_fetch(event, mode_kind::ALPHA_SEMIGRAPHICS);
	CHECK(result.reject_reason == capture_reject_reason::HORIZONTAL_SYNC);

	capture.host_hsync(false);
	event.phase = host_phase::BORDER;
	result = capture.host_fetch(event, mode_kind::ALPHA_SEMIGRAPHICS);
	CHECK(result.reject_reason == capture_reject_reason::INACTIVE_PHASE);

	event.phase = host_phase::ACTIVE;
	result = capture.host_fetch(event, mode_kind::ALPHA_SEMIGRAPHICS);
	REQUIRE(result.sampled);
	CHECK(result.buffer_written);
	CHECK(result.reject_reason == capture_reject_reason::NONE);
	CHECK(result.input_page == 0);
	CHECK(result.input_address == 0);
	CHECK(result.source_offset == 0);
	CHECK(result.stream_byte == 0xff);
	CHECK(result.cell.data == 0x3f);
	CHECK(result.cell.css());
	CHECK(result.cell.alpha_semigraphics());
	CHECK(result.cell.inverse());
	CHECK(result.cell.internal_external());
	CHECK(result.sam_compared);
	CHECK(result.sam_matched);

	event.data = 0x12;
	event.sam_address = 0x4001;
	result = capture.host_fetch(event, mode_kind::ALPHA_SEMIGRAPHICS);
	CHECK(result.sampled);
	CHECK(result.sam_matched);
	CHECK(result.source_offset == 1);

	event.sam_address = 0x4012;
	result = capture.host_fetch(event, mode_kind::ALPHA_SEMIGRAPHICS);
	CHECK(result.sampled);
	CHECK_FALSE(result.sam_matched);
	CHECK(capture.state().diagnostics.sam_comparisons == 3);
	CHECK(capture.state().diagnostics.sam_mismatches == 1);
	CHECK(capture.state().diagnostics.sam_base == 0x4000);
	CHECK(capture.state().diagnostics.last_sam_address == 0x4012);
	CHECK(capture.state().diagnostics.last_source_offset == 2);
	CHECK(capture.state().diagnostics.last_observed_offset == 0x12);

	for (unsigned mismatch = 1; mismatch < SAM_MISMATCH_WARNING_THRESHOLD; ++mismatch)
	{
		event.sam_address = std::uint16_t(0x4100 + mismatch);
		result = capture.host_fetch(event, mode_kind::ALPHA_SEMIGRAPHICS);
		REQUIRE(result.sampled);
		CHECK_FALSE(result.sam_matched);
		CHECK(result.sam_warning == (mismatch == SAM_MISMATCH_WARNING_THRESHOLD - 1));
	}
	CHECK(capture.state().diagnostics.frame_sam_mismatches == SAM_MISMATCH_WARNING_THRESHOLD);
	CHECK(capture.state().diagnostics.sam_warning_issued);

	capture.host_hsync(true);
	capture.host_fsync(true);
	result = capture.host_fetch(event, mode_kind::ALPHA_SEMIGRAPHICS);
	CHECK(result.reject_reason == capture_reject_reason::FIELD_SYNC);

	capture.host_fsync(false);
	capture.host_hsync(false);
	for (unsigned column = 0; column < 32; ++column)
		REQUIRE(capture.host_fetch(event, mode_kind::CG1).sampled);
	result = capture.host_fetch(event, mode_kind::CG1);
	CHECK_FALSE(result.sampled);
	CHECK(result.reject_reason == capture_reject_reason::CADENCE_EXHAUSTED);

	capture.state().host.active_line = HOST_ACTIVE_LINES;
	capture.state().host.column = 0;
	result = capture.host_fetch(event, mode_kind::CG1);
	CHECK(result.reject_reason == capture_reject_reason::LINE_OUT_OF_RANGE);

	capture_engine aligned;
	REQUIRE(aligned.host_vblank(false).frame_started);
	aligned.host_hblank(false);
	aligned.host_hblank(true);
	result = aligned.host_fetch(
			ordered_event(mode_kind::RG6, HOST_ACTIVE_LINES - 1, 0, false, 0x41),
			mode_kind::RG6);
	CHECK_FALSE(result.sampled);
	CHECK(result.reject_reason == capture_reject_reason::FRAME_ALIGNMENT);
	CHECK(aligned.state().host.frame_boundary_pending);
	aligned.host_hblank(false);
	aligned.host_hblank(true);
	result = aligned.host_fetch(
			ordered_event(mode_kind::RG6, 0, 0, false, 0x41),
			mode_kind::RG6);
	CHECK(result.sampled);
	CHECK(result.frame_started);
	CHECK_FALSE(result.frame_completed);
	CHECK(aligned.state().host.active_line == 0);
	CHECK(aligned.state().diagnostics.sam_mismatches == 0);
}


TEST_CASE("CoCoVGA full host frames preserve cadence, metadata, and page flips", "[cocovga][capture][modes]")
{
	for (unsigned raw_mode = 0; raw_mode <= unsigned(mode_kind::W64); ++raw_mode)
	{
		mode_kind const mode = static_cast<mode_kind>(raw_mode);
		capture_engine capture;
		host_sync_result first;
		INFO("first frame mode " << raw_mode);
		REQUIRE(test::capture_frame(capture, mode, false, 0x21, first));
		CHECK(first.completed_page == 0);
		CHECK(first.completed_generation == 1);
		CHECK(first.input_page_changed);
		CHECK(first.input_page == 1);
		CHECK(capture.input_page() == 1);

		frame_buffer_state const &page0 = capture.buffer(0);
		std::uint8_t const row_cells = capture_engine::fetches_per_host_line(mode);
		CHECK(page0.generation == 1);
		CHECK(page0.writes == std::uint32_t(HOST_ACTIVE_LINES) * row_cells);
		CHECK(std::all_of(
				page0.row_fetch_counts.begin(),
				page0.row_fetch_counts.end(),
				[row_cells] (std::uint8_t count) { return count == row_cells; }));
		CHECK(page0.first_address == 0);
		CHECK(page0.last_address ==
				(mode == mode_kind::W64
						? W64_LOGICAL_BYTES - 1
						: std::uint16_t((HOST_ACTIVE_LINES - 1) * 32 + row_cells - 1)));
		CHECK(page0.active_lines == HOST_ACTIVE_LINES);
		CHECK(page0.mode_changes == 0);
		CHECK(page0.first_mode == raw_mode);
		CHECK(page0.last_mode == raw_mode);
		CHECK(page0.valid);
		CHECK(page0.complete);
		CHECK_FALSE(page0.programming_suppressed);
		CHECK(capture.state().diagnostics.sam_comparisons == page0.writes);
		CHECK(capture.state().diagnostics.sam_mismatches == 0);
		CHECK(capture.state().diagnostics.completed_frame_sam_comparisons == page0.writes);
		CHECK(capture.state().diagnostics.completed_frame_sam_mismatches == 0);

		host_sync_result second;
		INFO("second frame mode " << raw_mode);
		REQUIRE(test::capture_frame(capture, mode, true, 0x87, second));
		CHECK(second.completed_page == 1);
		CHECK(second.completed_generation == 2);
		CHECK(second.input_page == 0);
		CHECK(capture.input_page() == 0);
		CHECK(capture.buffer(1).generation == 2);
		CHECK(capture.buffer(1).cells[0].css());
		CHECK_FALSE(capture.buffer(0).cells[0].css());
		CHECK(capture.state().host.frames_completed == 2);
	}
}

TEST_CASE("CoCoVGA capture accepts MAME-ordered late bursts without shifting scanlines", "[cocovga][capture][mame-order]")
{
	for (unsigned raw_mode = 0; raw_mode <= unsigned(mode_kind::W64); ++raw_mode)
	{
		mode_kind const mode = static_cast<mode_kind>(raw_mode);
		capture_engine capture;
		INFO("mode " << raw_mode);
		REQUIRE(capture.host_vblank(false).frame_started);
		capture.host_hblank(false);

		for (std::uint16_t row = 0; row < HOST_ACTIVE_LINES - 1; ++row)
			REQUIRE(mame_ordered_burst(capture, mode, row, false, 0x31));

		host_sync_result const pending = capture.host_vblank(true);
		CHECK(pending.boundary_pending);
		CHECK_FALSE(pending.frame_completed);
		REQUIRE(mame_ordered_burst(capture, mode, HOST_ACTIVE_LINES - 1, false, 0x31));
		CHECK(capture.state().host.frames_completed == 0);

		host_sync_result const boundary = capture.host_vblank(false);
		CHECK(boundary.frame_started);
		CHECK(boundary.frame_completed);
		CHECK(boundary.completed_page == 0);
		CHECK(boundary.completed_generation == 1);
		REQUIRE(mame_ordered_burst(capture, mode, 0, true, 0x92));

		frame_buffer_state const &frame = capture.buffer(0);
		CHECK(frame.generation == 1);
		CHECK(frame.writes ==
				std::uint32_t(HOST_ACTIVE_LINES) *
						capture_engine::fetches_per_host_line(mode));
		CHECK(frame.active_lines == HOST_ACTIVE_LINES);
		CHECK(frame.first_address == 0);
		CHECK(frame.complete);
		CHECK(capture.state().diagnostics.completed_frame_sam_comparisons == frame.writes);
		CHECK(capture.state().diagnostics.completed_frame_sam_mismatches == 0);
	}

	capture_engine capture;
	REQUIRE(capture.host_vblank(false).frame_started);
	capture.host_hblank(false);
	for (std::uint16_t row = 0; row < HOST_ACTIVE_LINES - 1; ++row)
		REQUIRE(mame_ordered_burst(capture, mode_kind::RG6, row, false, 0x31));
	REQUIRE(capture.host_vblank(true).boundary_pending);
	REQUIRE(mame_ordered_burst(capture, mode_kind::RG6, HOST_ACTIVE_LINES - 1, false, 0x31));
	REQUIRE(capture.host_vblank(false).frame_completed);
	REQUIRE(mame_ordered_burst(capture, mode_kind::RG6, 0, true, 0x92));

	frame_buffer_state const &frame = capture.buffer(0);
	CHECK(frame.cells[0].data == ordered_data(0, 0, 0x31));
	CHECK(frame.cells[31].data == ordered_data(0, 31, 0x31));
	CHECK(frame.cells[(HOST_ACTIVE_LINES - 1) * 32].data ==
			ordered_data(HOST_ACTIVE_LINES - 1, 0, 0x31));
	CHECK(frame.cells[CAPTURE_PAGE_BYTES - 1].data ==
			ordered_data(HOST_ACTIVE_LINES - 1, 31, 0x31));

	// every captured cell holds the byte the host fetched for it, in the order
	// the host drove them
	for (std::uint16_t row = 0; row < HOST_ACTIVE_LINES; ++row)
	{
		for (std::uint8_t column = 0; column < 32; ++column)
		{
			captured_cell const expected =
					test::make_cell(mode_kind::RG6, false, ordered_data(row, column, 0x31));
			captured_cell const &cell = frame.cells[row * 32 + column];
			INFO("row " << row << " column " << unsigned(column));
			CHECK(cell.data == expected.data);
			CHECK(cell.gm == expected.gm);
			CHECK(cell.mode_id == expected.mode_id);
			CHECK(cell.flags == expected.flags);
		}
	}
}

TEST_CASE("CoCoVGA pending late-burst state restores without changing the completed frame", "[cocovga][capture][mame-order][restore]")
{
	capture_engine uninterrupted;
	REQUIRE(uninterrupted.host_vblank(false).frame_started);
	uninterrupted.host_hblank(false);
	for (std::uint16_t row = 0; row < HOST_ACTIVE_LINES - 1; ++row)
		REQUIRE(mame_ordered_burst(uninterrupted, mode_kind::RG6, row, false, 0x47));
	REQUIRE(uninterrupted.host_vblank(true).boundary_pending);
	uninterrupted.host_hblank(true);
	REQUIRE(uninterrupted.state().host.frame_boundary_pending);
	REQUIRE(uninterrupted.state().host.line_boundary_pending);

	capture_engine restored;
	restored.restore_state(uninterrupted.state());
	CHECK(restored.state().host.frame_boundary_pending);
	CHECK(restored.state().host.line_boundary_pending);

	for (capture_engine *const current : { &uninterrupted, &restored })
	{
		REQUIRE(mame_ordered_burst(
				*current,
				mode_kind::RG6,
				HOST_ACTIVE_LINES - 1,
				false,
				0x47));
		REQUIRE(current->host_vblank(false).frame_completed);
		REQUIRE(mame_ordered_burst(*current, mode_kind::RG6, 0, true, 0xb3));
		CHECK(current->state().diagnostics.completed_frame_sam_mismatches == 0);
	}

	CHECK(test::frame_hash(restored.buffer(0)) == test::frame_hash(uninterrupted.buffer(0)));
	CHECK(restored.state().host.frames_completed == uninterrupted.state().host.frames_completed);
	CHECK(restored.state().diagnostics.sam_comparisons ==
			uninterrupted.state().diagnostics.sam_comparisons);
}

TEST_CASE("CoCoVGA programming accepts every MAME offset without false realignment", "[cocovga][capture][mame-order][lock]")
{
	capture_engine capture;
	REQUIRE(capture.host_vblank(false).frame_started);
	capture.host_hblank(false);
	capture.set_programming(true);

	for (std::uint16_t row = 0; row < HOST_ACTIVE_LINES - 1; ++row)
		REQUIRE(mame_ordered_burst(capture, mode_kind::ALPHA_SEMIGRAPHICS, row, false, 0x26));

	REQUIRE(capture.host_vblank(true).boundary_pending);
	REQUIRE(mame_ordered_burst(
			capture,
			mode_kind::ALPHA_SEMIGRAPHICS,
			HOST_ACTIVE_LINES - 1,
			false,
			0x26));
	host_sync_result const completed = capture.host_vblank(false);
	REQUIRE(completed.frame_completed);
	CHECK(capture.state().host.frames_completed == 1);
	CHECK(capture.state().diagnostics.completed_frame_sam_comparisons ==
			std::uint32_t(HOST_ACTIVE_LINES) * 32);
	CHECK(capture.state().diagnostics.completed_frame_sam_mismatches == 0);
	CHECK_FALSE(capture.buffer(completed.completed_page).valid);
}


TEST_CASE("CoCoVGA capture selection repeats, holds, and advances generations", "[cocovga][capture]")
{
	capture_engine capture;
	host_sync_result completed;
	REQUIRE(test::capture_frame(capture, mode_kind::CG6, false, 0x11, completed));

	vga_tick_result tick = clock_at_vsync_start(capture);
	CHECK(tick.vertical_sync_started);
	CHECK(tick.output_changed);
	CHECK_FALSE(tick.frame_repeated);
	CHECK_FALSE(tick.output_held);
	CHECK(capture.output_valid());
	CHECK(capture.output_page() == 0);
	CHECK(capture.output_generation() == 1);
	CHECK(capture.output_latency_frames() == 1);
	CHECK(capture.state().vga.selections == 1);

	tick = clock_at_vsync_start(capture);
	CHECK(tick.vertical_sync_started);
	CHECK_FALSE(tick.output_changed);
	CHECK(tick.frame_repeated);
	CHECK_FALSE(tick.output_held);
	CHECK(capture.state().vga.repeated_frames == 1);

	REQUIRE(test::capture_frame(capture, mode_kind::CG6, true, 0x22, completed));
	tick = capture.clock_vga();
	CHECK_FALSE(tick.vertical_sync_started);
	CHECK_FALSE(tick.output_changed);
	CHECK_FALSE(tick.frame_repeated);
	CHECK_FALSE(tick.output_held);
	CHECK(capture.output_page() == 0);
	CHECK(capture.output_generation() == 1);

	tick = clock_at_vsync_start(capture);
	CHECK(tick.output_changed);
	CHECK_FALSE(tick.frame_repeated);
	CHECK(capture.output_page() == 1);
	CHECK(capture.output_generation() == 2);
	CHECK(capture.output_cell(0).css());
	CHECK(capture.output_latency_frames() == 1);

	capture.set_programming(true);
	tick = clock_at_vsync_start(capture);
	CHECK(tick.vertical_sync_started);
	CHECK(tick.output_held);
	CHECK(tick.frame_repeated);
	CHECK_FALSE(tick.output_changed);
	CHECK(capture.output_page() == 1);
	CHECK(capture.output_generation() == 2);
	CHECK(capture.state().vga.held_frames == 1);
	CHECK(capture.state().vga.repeated_frames == 2);

	capture.state().host.frames_completed = std::numeric_limits<std::uint64_t>::max();
	capture.state().vga.output_generation = 0;
	CHECK(capture.output_latency_frames() == std::numeric_limits<std::uint64_t>::max());
	capture.state().vga.output_valid = false;
	CHECK(capture.output_latency_frames() == 0);
}

TEST_CASE("CoCoVGA selection rejects partial, invalid, suppressed, and stale pages", "[cocovga][capture][lock][mame-order]")
{
	capture_engine partial;
	REQUIRE(partial.host_vblank(false).frame_started);
	partial.host_hblank(false);
	REQUIRE(mame_ordered_burst(partial, mode_kind::RG6, 0, false, 0x21));
	REQUIRE(partial.host_vblank(true).boundary_pending);
	REQUIRE(partial.host_vblank(false).boundary_pending);
	capture_result first_result;
	REQUIRE(mame_ordered_burst(
			partial,
			mode_kind::RG6,
			0,
			true,
			0x81,
			&first_result));
	REQUIRE(first_result.frame_completed);
	CHECK(partial.buffer(0).valid);
	CHECK_FALSE(partial.buffer(0).complete);
	CHECK_FALSE(partial.buffer(0).programming_suppressed);
	CHECK(partial.buffer(0).active_lines == 1);

	capture_engine capture;
	host_sync_result completed;
	REQUIRE(test::capture_frame(capture, mode_kind::RG6, false, 0x31, completed));
	REQUIRE(clock_at_vsync_start(capture).output_changed);
	std::uint8_t const held_page = capture.output_page();
	std::uint64_t const held_generation = capture.output_generation();
	std::uint64_t const held_hash = test::frame_hash(capture.buffer(held_page));

	capture.state().host.input_page = 0;
	frame_buffer_state &candidate = capture.state().buffers[1];
	candidate.generation = held_generation + 1;
	candidate.valid = false;
	candidate.complete = true;
	candidate.programming_suppressed = false;

	vga_tick_result tick = clock_at_vsync_start(capture);
	CHECK_FALSE(tick.output_changed);
	CHECK(tick.frame_repeated);
	CHECK(capture.output_page() == held_page);
	CHECK(test::frame_hash(capture.buffer(capture.output_page())) == held_hash);

	candidate.valid = true;
	candidate.complete = false;
	tick = clock_at_vsync_start(capture);
	CHECK_FALSE(tick.output_changed);
	CHECK(tick.frame_repeated);
	CHECK(capture.output_page() == held_page);

	candidate.complete = true;
	candidate.programming_suppressed = true;
	tick = clock_at_vsync_start(capture);
	CHECK_FALSE(tick.output_changed);
	CHECK(tick.frame_repeated);
	CHECK(capture.output_page() == held_page);

	candidate.programming_suppressed = false;
	candidate.generation = held_generation;
	tick = clock_at_vsync_start(capture);
	CHECK_FALSE(tick.output_changed);
	CHECK(tick.frame_repeated);
	CHECK(capture.output_page() == held_page);

	candidate.generation = held_generation + 1;
	tick = clock_at_vsync_start(capture);
	CHECK(tick.output_changed);
	CHECK_FALSE(tick.frame_repeated);
	CHECK(capture.output_page() == 1);
	CHECK(capture.output_generation() == held_generation + 1);
}

TEST_CASE("CoCoVGA alpha and graphics transitions advance at every field position", "[cocovga][capture][renderer][modes][transition][hash]")
{
	for (auto const modes : {
			std::pair{ mode_kind::ALPHA_SEMIGRAPHICS, mode_kind::CG6 },
			std::pair{ mode_kind::CG6, mode_kind::ALPHA_SEMIGRAPHICS },
			std::pair{ mode_kind::ALPHA_SEMIGRAPHICS, mode_kind::RG3 },
			std::pair{ mode_kind::RG3, mode_kind::ALPHA_SEMIGRAPHICS } })
	{
		for (std::uint16_t const transition_row :
				{ std::uint16_t(1), std::uint16_t(96), std::uint16_t(191) })
		{
			INFO("modes " << unsigned(modes.first) << " -> " << unsigned(modes.second)
					<< " transition row " << transition_row);
			capture_engine capture;
			host_sync_result completed;
			REQUIRE(test::capture_frame(
					capture,
					modes.first,
					false,
					0x31,
					completed));
			REQUIRE(clock_at_vsync_start(capture).output_changed);

			REQUIRE(capture_mode_transition_frame(
					capture,
					modes.first,
					modes.second,
					transition_row,
					0x72,
					completed));
			frame_buffer_state const &mixed = capture.buffer(completed.completed_page);
			REQUIRE(mixed.valid);
			REQUIRE(mixed.complete);
			CHECK(mixed.first_mode == static_cast<std::uint8_t>(modes.first));
			CHECK(mixed.last_mode == static_cast<std::uint8_t>(modes.second));
			CHECK(mixed.mode_changes == 1);
			CHECK(capture_engine::persistent_mode_transition(mixed));
			CHECK(mixed.row_fetch_counts[transition_row - 1] ==
					capture_engine::fetches_per_host_line(modes.first));
			CHECK(mixed.row_fetch_counts[transition_row] ==
					capture_engine::fetches_per_host_line(modes.second));

			core control;
			REQUIRE(test::write_register(control, register_bank::ARTIFACT, 4, 0));
			renderer output = cocovga::test::make_renderer();
			REQUIRE(output.preflight(control, mixed).success());
			vga_tick_result const switched = clock_at_vsync_start(capture);
			CHECK(switched.output_changed);
			CHECK_FALSE(switched.output_rejected);
			CHECK_FALSE(switched.frame_repeated);
			CHECK(capture.output_page() == completed.completed_page);
			CHECK(capture.output_generation() == completed.completed_generation);
			CHECK_FALSE(capture.state().vga.rejected_candidate_valid);
		}
	}
}

TEST_CASE("CoCoVGA capture retains within-row cadence changes in both directions", "[cocovga][capture][renderer][modes][transition]")
{
	for (auto const transition : {
			std::tuple{ mode_kind::ALPHA_SEMIGRAPHICS, mode_kind::CG1, std::uint8_t(16) },
			std::tuple{ mode_kind::RG3, mode_kind::RG6, std::uint8_t(32) } })
	{
		mode_kind const first_mode = std::get<0>(transition);
		mode_kind const last_mode = std::get<1>(transition);
		std::uint8_t const row_fetches = std::get<2>(transition);
		capture_engine capture;
		host_sync_result completed;
		REQUIRE(capture_with_mixed_row(
				capture,
				first_mode,
				last_mode,
				96,
				8,
				row_fetches,
				0x49,
				completed));

		frame_buffer_state const &frame = capture.buffer(completed.completed_page);
		REQUIRE(frame.complete);
		CHECK(frame.row_fetch_counts[96] == row_fetches);
		CHECK(frame.cells[96 * 32 + 7].mode() == first_mode);
		CHECK(frame.cells[96 * 32 + 8].mode() == last_mode);
		CHECK(frame.mode_changes == 1);

		core control;
		REQUIRE(test::write_register(control, register_bank::ARTIFACT, 4, 0));
		renderer output = cocovga::test::make_renderer();
		REQUIRE(output.preflight(control, frame).success());
		std::array<render_pixel, RENDER_WIDTH> line{};
		CHECK(output.render_scanline(
				control,
				frame,
				VIEWPORT_TOP + 96 * 2,
				{ line.data(), line.size() }).success());

		vga_tick_result const selected = clock_at_vsync_start(capture);
		CHECK(selected.output_changed);
		CHECK_FALSE(selected.output_rejected);
		CHECK(capture.output_generation() == completed.completed_generation);
	}
}

TEST_CASE("CoCoVGA continuous one-transition fields advance for more than thirty selections", "[cocovga][capture][renderer][modes][transition][hash]")
{
	core control;
	REQUIRE(test::write_register(control, register_bank::ARTIFACT, 4, 0));
	capture_engine capture;
	renderer output = cocovga::test::make_renderer();
	std::vector<std::uint64_t> hashes;

	mode_kind first_mode = mode_kind::ALPHA_SEMIGRAPHICS;
	mode_kind last_mode = mode_kind::CG6;
	for (unsigned field = 0; field < 36; ++field)
	{
		std::uint16_t const transition_row =
				std::array<std::uint16_t, 3>{ 1, 96, 191 }[field % 3];
		host_sync_result completed;
		REQUIRE(capture_mode_transition_frame(
				capture,
				first_mode,
				last_mode,
				transition_row,
				std::uint8_t(0x20 + field * 7U),
				completed));
		frame_buffer_state const &frame = capture.buffer(completed.completed_page);
		REQUIRE(frame.complete);
		REQUIRE(output.preflight(control, frame).success());

		vga_tick_result const selected = clock_at_vsync_start(capture);
		REQUIRE(selected.output_changed);
		CHECK_FALSE(selected.output_rejected);
		CHECK(capture.output_generation() == completed.completed_generation);

		std::vector<render_pixel> pixels;
		pixel_surface const surface = test::make_surface(pixels);
		REQUIRE(output.render(control, frame, surface).success());
		CHECK(std::any_of(
				pixels.begin(),
				pixels.end(),
				[] (render_pixel pixel) { return pixel != 0xff000000U; }));
		std::uint64_t const hash = test::pixel_hash(pixels.data(), pixels.size());
		if (!hashes.empty())
			CHECK(hash != hashes.back());
		hashes.push_back(hash);
		std::swap(first_mode, last_mode);
	}
	CHECK(hashes.size() == 36);
}

TEST_CASE("CoCoVGA nonpersistent mixed-mode fields remain selectable", "[cocovga][capture][modes][transition]")
{
	capture_engine capture;
	host_sync_result completed;
	REQUIRE(test::capture_frame(
			capture,
			mode_kind::ALPHA_SEMIGRAPHICS,
			false,
			0x21,
			completed));
	REQUIRE(clock_at_vsync_start(capture).output_changed);

	REQUIRE(capture.state().host.frame_started);
	capture.host_hblank(false);
	for (std::uint16_t row = 0; row < HOST_ACTIVE_LINES; ++row)
	{
		mode_kind const mode =
				row >= 64 && row < 128
						? mode_kind::CG6
						: mode_kind::ALPHA_SEMIGRAPHICS;
		REQUIRE(mame_ordered_burst(capture, mode, row, false, 0x93));
	}
	REQUIRE(capture.host_vblank(true).boundary_pending);
	completed = capture.host_vblank(false);
	REQUIRE(completed.frame_completed);

	frame_buffer_state const &mixed = capture.buffer(completed.completed_page);
	REQUIRE(mixed.complete);
	CHECK(mixed.mode_changes == 2);
	CHECK(mixed.first_mode == mixed.last_mode);
	CHECK_FALSE(capture_engine::persistent_mode_transition(mixed));

	vga_tick_result const selected = clock_at_vsync_start(capture);
	CHECK(selected.output_changed);
	CHECK_FALSE(selected.output_rejected);
	CHECK(capture.output_generation() == completed.completed_generation);
}

TEST_CASE("CoCoVGA programming preserves the latest clean page when capture overlaps output", "[cocovga][capture][lock][mame-order][hash]")
{
	capture_engine capture;
	host_sync_result completed;
	REQUIRE(test::capture_frame(capture, mode_kind::RG6, false, 0x21, completed));
	REQUIRE(clock_at_vsync_start(capture).output_changed);
	CHECK(capture.output_page() == 0);
	CHECK(capture.output_generation() == 1);

	REQUIRE(test::capture_frame(capture, mode_kind::RG6, true, 0x73, completed));
	REQUIRE(completed.completed_page == 1);
	REQUIRE(capture.buffer(1).complete);
	std::uint64_t const latest_hash = test::frame_hash(capture.buffer(1));
	CHECK(capture.input_page() == capture.output_page());

	std::uint64_t const displayed_hash = test::frame_hash(capture.buffer(0));
	capture.host_hblank(false);
	for (std::uint16_t row = 0; row < 8; ++row)
		REQUIRE(mame_ordered_burst(capture, mode_kind::RG6, row, false, 0xb5));
	CHECK(test::frame_hash(capture.buffer(0)) != displayed_hash);

	capture.set_programming(true);
	CHECK(capture.programming());
	CHECK(capture.output_page() == 1);
	CHECK(capture.output_generation() == 2);
	CHECK(test::frame_hash(capture.buffer(capture.output_page())) == latest_hash);
	CHECK(capture.state().host.frame_programming_suppressed);
}


TEST_CASE("CoCoVGA programming freeze suppresses host writes and holds VGA output", "[cocovga][capture][lock]")
{
	core control;
	capture_engine capture;
	host_sync_result completed;
	REQUIRE(test::capture_frame(capture, mode_kind::ALPHA_SEMIGRAPHICS, false, 0x31, completed));
	REQUIRE(clock_at_vsync_start(capture).output_changed);
	std::uint64_t const generation = capture.output_generation();
	std::uint8_t const page = capture.output_page();

	REQUIRE(test::unlock_page(control, register_page::PAGE_00).freeze);
	REQUIRE(capture.update_from_core(control));
	CHECK(capture.programming());

	REQUIRE(test::capture_frame(capture, mode_kind::ALPHA_SEMIGRAPHICS, true, 0x72, completed));
	CHECK(completed.completed_generation == 2);
	CHECK(capture.buffer(completed.completed_page).generation == 0);
	CHECK(capture.state().host.frame_programming_suppressed);

	vga_tick_result const held = clock_at_vsync_start(capture);
	CHECK(held.output_held);
	CHECK(held.frame_repeated);
	CHECK(capture.output_generation() == generation);
	CHECK(capture.output_page() == page);

	control.clock_combo(true, 0);
	control.clock_combo(false, 0);
	REQUIRE(capture.update_from_core(control));
	CHECK_FALSE(capture.programming());
}


TEST_CASE("CoCoVGA capture timing requests apply only at a frame boundary", "[cocovga][capture][timing]")
{
	vga_timing const initial{ 8, 2, 3, 4, 6, 1, 2, 3 };
	vga_timing const requested{ 10, 3, 2, 5, 7, 2, 1, 4 };
	capture_engine capture(initial);

	CHECK(capture_engine::valid_timing(initial));
	REQUIRE(capture.request_vga_timing(requested));
	CHECK(capture.state().vga.pending_timing_valid);
	CHECK(vga_timing_equal(capture.state().vga.active_timing, initial));
	CHECK(vga_timing_equal(capture.state().vga.pending_timing, requested));

	capture.state().vga.hcounter = std::uint16_t(capture.timing_endpoints().h_total - 2);
	capture.state().vga.vcounter = std::uint16_t(capture.timing_endpoints().v_total - 1);
	CHECK_FALSE(capture.clock_vga().frame_started);
	vga_tick_result const boundary = capture.clock_vga();
	CHECK(boundary.frame_started);
	CHECK(boundary.timing_applied);
	CHECK(capture.state().vga.frames == 1);
	CHECK_FALSE(capture.state().vga.pending_timing_valid);
	CHECK(vga_timing_equal(capture.state().vga.active_timing, requested));
	CHECK(capture.timing_endpoints().h_total == 20);
	CHECK(capture.timing_endpoints().v_total == 14);

	REQUIRE(capture.request_vga_timing(requested));
	CHECK_FALSE(capture.state().vga.pending_timing_valid);

	vga_timing invalid = requested;
	invalid.h_front_porch = 0;
	CHECK_FALSE(capture.request_vga_timing(invalid));
	CHECK_FALSE(capture.state().vga.pending_timing_valid);

	vga_timing later{ 12, 2, 2, 2, 8, 1, 1, 2 };
	REQUIRE(capture.request_vga_timing(later));
	CHECK(capture.state().vga.pending_timing_valid);
	CHECK_FALSE(capture.request_vga_timing(invalid));
	CHECK(capture.state().vga.pending_timing_valid);
	CHECK(vga_timing_equal(capture.state().vga.pending_timing, later));

	core control;
	REQUIRE(test::unlock_page(control, register_page::PAGE_00).freeze);
	control.state().registers.timing = requested;
	REQUIRE(capture.update_from_core(control));
	CHECK(capture.programming());
	CHECK(vga_timing_equal(capture.state().vga.pending_timing, later));
	control.clock_combo(true, 0);
	control.clock_combo(false, 0);
	REQUIRE(capture.update_from_core(control));
	CHECK_FALSE(capture.programming());
	CHECK_FALSE(capture.state().vga.pending_timing_valid);
	CHECK(vga_timing_equal(capture.state().vga.active_timing, requested));

	vga_timing horizontal_limit{ 2045, 1, 1, 1, 1, 1, 1, 1 };
	CHECK(capture_engine::valid_timing(horizontal_limit));
	horizontal_limit.h_active = 2046;
	CHECK_FALSE(capture_engine::valid_timing(horizontal_limit));
	vga_timing vertical_limit{ 1, 1, 1, 1, 1020, 1, 1, 1 };
	CHECK(capture_engine::valid_timing(vertical_limit));
	vertical_limit.v_active = 1021;
	CHECK_FALSE(capture_engine::valid_timing(vertical_limit));
}


TEST_CASE("CoCoVGA capture restore sanitizes derived and bounded state", "[cocovga][capture][restore]")
{
	capture_engine capture;
	capture_engine_state saved = capture.state();
	saved.vga.active_timing.h_active = 0;
	saved.vga.pending_timing = saved.vga.active_timing;
	saved.vga.pending_timing_valid = true;
	saved.host.input_page = 0xff;
	saved.vga.output_page = 0xfe;
	saved.vga.rejected_reason = output_reject_reason::PERSISTENT_MODE_TRANSITION;
	saved.vga.rejected_candidate_valid = true;
	saved.host.active_line = 0xffff;
	saved.host.column = 0xff;
	saved.host.working_writes = std::numeric_limits<std::uint32_t>::max();
	saved.host.working_row_fetch_counts[7] = 0xff;
	saved.host.working_first_address = 0xffff;
	saved.host.working_last_address = 0xffff;
	saved.host.working_active_lines = 0xffff;
	saved.host.working_first_mode = 0xff;
	saved.host.working_last_mode = 0xff;
	saved.vga.hcounter = 0xffff;
	saved.vga.vcounter = make_vga_timing_endpoints(vga_timing{}).v_total;
	saved.buffers[0].writes = std::numeric_limits<std::uint32_t>::max();
	saved.buffers[0].row_fetch_counts.fill(32);
	saved.buffers[0].row_fetch_counts[11] = 0xff;
	saved.buffers[0].complete = true;
	saved.buffers[0].first_address = 0xffff;
	saved.buffers[0].last_address = 0xffff;
	saved.buffers[0].active_lines = 0xffff;
	saved.buffers[0].first_mode = 0xff;
	saved.buffers[0].last_mode = 0xff;
	saved.buffers[1].cells[17].data = 0xa5;
	saved.buffers[1].generation = 42;

	capture.restore_state(saved);
	CHECK(vga_timing_equal(capture.state().vga.active_timing, vga_timing{}));
	CHECK_FALSE(capture.state().vga.pending_timing_valid);
	CHECK(capture.state().host.input_page == 1);
	CHECK(capture.state().vga.output_page == 0);
	CHECK(capture.state().vga.rejected_reason == output_reject_reason::NONE);
	CHECK_FALSE(capture.state().vga.rejected_candidate_valid);
	CHECK(capture.state().host.active_line == HOST_ACTIVE_LINES);
	CHECK(capture.state().host.column == 32);
	CHECK(capture.state().host.working_writes == CAPTURE_PAGE_BYTES);
	CHECK(capture.state().host.working_row_fetch_counts[7] == 0);
	CHECK(capture.state().host.frame_programming_suppressed);
	CHECK(capture.state().host.working_first_address == CAPTURE_PAGE_BYTES - 1);
	CHECK(capture.state().host.working_last_address == CAPTURE_PAGE_BYTES - 1);
	CHECK(capture.state().host.working_active_lines == HOST_ACTIVE_LINES);
	CHECK(capture.state().host.working_first_mode ==
			static_cast<std::uint8_t>(mode_kind::ALPHA_SEMIGRAPHICS));
	CHECK(capture.state().host.working_last_mode ==
			static_cast<std::uint8_t>(mode_kind::ALPHA_SEMIGRAPHICS));
	CHECK(capture.state().vga.hcounter == 0);
	CHECK(capture.state().vga.vcounter == 0);
	CHECK(capture.buffer(0).writes == CAPTURE_PAGE_BYTES);
	CHECK(capture.buffer(0).row_fetch_counts[11] == 0);
	CHECK_FALSE(capture.buffer(0).complete);
	CHECK(capture.buffer(0).first_address == CAPTURE_PAGE_BYTES - 1);
	CHECK(capture.buffer(0).last_address == CAPTURE_PAGE_BYTES - 1);
	CHECK(capture.buffer(0).active_lines == HOST_ACTIVE_LINES);
	CHECK(capture.buffer(0).first_mode ==
			static_cast<std::uint8_t>(mode_kind::ALPHA_SEMIGRAPHICS));
	CHECK(capture.buffer(0).last_mode ==
			static_cast<std::uint8_t>(mode_kind::ALPHA_SEMIGRAPHICS));
	CHECK(capture.buffer(1).cells[17].data == 0xa5);
	CHECK(capture.buffer(1).generation == 42);
	CHECK(capture.timing_endpoints().h_total == 800);
	CHECK(capture.timing_endpoints().v_total == 525);

	CHECK(capture.read_cell(3, 17).data == 0xa5);
	CHECK(capture.read_cell(0, CAPTURE_PAGE_BYTES).data == 0);
}


TEST_CASE("CoCoVGA capture stores every mode's fetches at the documented addresses", "[cocovga][capture][oracle]")
{
	// The buffer holds one cell per host fetch, so the expected frame follows
	// from the documented VDG cadence: the long-cycle modes fetch 16 bytes per
	// host line and every other mode fetches 32, and only W64 folds three host
	// lines onto one 32 byte row.
	auto const documented_fetches = [] (mode_kind mode) -> std::uint8_t
	{
		switch (mode)
		{
		case mode_kind::CG1:
		case mode_kind::RG1:
		case mode_kind::RG2:
		case mode_kind::RG3:
			return 16;
		default:
			return 32;
		}
	};
	auto const documented_address =
			[] (mode_kind mode, std::uint16_t row, std::uint8_t column) -> std::uint16_t
	{
		return mode == mode_kind::W64
				? std::uint16_t((row / 3) * 32 + column)
				: std::uint16_t(row * 32 + column);
	};

	for (mode_kind mode : {
			mode_kind::ALPHA_SEMIGRAPHICS,
			mode_kind::CG1,
			mode_kind::RG1,
			mode_kind::CG2,
			mode_kind::RG2,
			mode_kind::CG3,
			mode_kind::RG3,
			mode_kind::CG6,
			mode_kind::RG6,
			mode_kind::VG6,
			mode_kind::W64 })
	{
		capture_engine first_capture;
		capture_engine second_capture;
		host_sync_result completion;
		REQUIRE(test::capture_frame(first_capture, mode, true, 0x5b, completion));
		REQUIRE(test::capture_frame(second_capture, mode, true, 0x5b, completion));

		// identical fetch sequences capture identical frames
		CHECK(test::frame_hash(first_capture.buffer(0)) ==
				test::frame_hash(second_capture.buffer(0)));

		frame_buffer_state const &frame = first_capture.buffer(0);
		std::uint8_t const fetches = documented_fetches(mode);
		CHECK(frame.valid);
		CHECK(frame.complete);
		CHECK(frame.active_lines == HOST_ACTIVE_LINES);

		for (std::uint16_t row = 0; row < HOST_ACTIVE_LINES; ++row)
		{
			INFO("mode " << unsigned(mode) << " row " << row);
			CHECK(frame.row_fetch_counts[row] == fetches);

			for (std::uint8_t column = 0; column < fetches; ++column)
			{
				// W64 folds three host lines onto one row, so the last of the
				// three fetches is the one left in the buffer
				std::uint16_t const source_row = mode == mode_kind::W64
						? std::uint16_t((row / 3) * 3 + 2)
						: row;
				captured_cell const expected = test::make_cell(
						mode,
						true,
						std::uint8_t(0x5b + source_row * 11U + column * 17U),
						mode == mode_kind::ALPHA_SEMIGRAPHICS && ((source_row + column) & 3U) == 1,
						mode == mode_kind::ALPHA_SEMIGRAPHICS && ((source_row + column) & 3U) == 2);
				captured_cell const &cell = frame.cells[documented_address(mode, row, column)];
				INFO("column " << unsigned(column));
				CHECK(cell.data == expected.data);
				CHECK(cell.gm == expected.gm);
				CHECK(cell.mode_id == expected.mode_id);
				CHECK(cell.flags == expected.flags);
			}

			// long-cycle modes leave the second half of the row untouched
			for (std::uint8_t column = fetches; column < 32; ++column)
			{
				INFO("unfetched column " << unsigned(column));
				CHECK(frame.cells[documented_address(mode, row, column)].data == 0);
			}
		}
	}
}
