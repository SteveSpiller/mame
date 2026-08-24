// license:BSD-3-Clause
// copyright-holders:Stephen Spiller

#include "emu.h"

#include "cocovga_capture.h"

#include <algorithm>
#include <numeric>

namespace cocovga
{

namespace
{

constexpr bool alpha_capture_format(mode_kind mode) noexcept
{
	return mode == mode_kind::ALPHA_SEMIGRAPHICS || mode == mode_kind::W64;
}

std::uint16_t capture_input_address(
		mode_kind mode,
		std::uint16_t active_line,
		std::uint8_t column) noexcept
{
	if (active_line >= HOST_ACTIVE_LINES || column >= 32)
		return INVALID_CAPTURE_ADDRESS;
	if (mode == mode_kind::W64)
		return std::uint16_t((active_line / 3) * 32 + column);
	return std::uint16_t(active_line * 32 + column);
}

} // anonymous namespace

pixel_input host_mode_pins::pixel(std::uint8_t data) const noexcept
{
	pixel_input result;
	result.data = data;
	result.gm = BIT(gm, 0, 3);
	result.graphics = graphics;
	result.css = css;
	result.alpha_semigraphics = alpha_semigraphics;
	result.inverse = inverse;
	result.internal_external = internal_external;
	return result;
}

mode_kind captured_cell::mode() const noexcept
{
	mode_kind const value = static_cast<mode_kind>(mode_id);
	return static_cast<unsigned>(value) <= static_cast<unsigned>(mode_kind::W64)
			? value
			: mode_kind::ALPHA_SEMIGRAPHICS;
}

bool captured_cell::alpha_format() const noexcept
{
	return alpha_capture_format(mode());
}

std::uint8_t captured_cell::stream_byte() const noexcept
{
	return alpha_format()
			? core::pack_alpha_capture_byte(alpha_semigraphics(), inverse(), data)
			: data;
}

pixel_input captured_cell::pixel() const noexcept
{
	pixel_input result;
	result.data = data;
	result.gm = BIT(gm, 0, 3);
	result.graphics = graphics();
	result.css = css();
	result.alpha_semigraphics = alpha_semigraphics();
	result.inverse = inverse();
	result.internal_external = internal_external();
	return result;
}

capture_engine::capture_engine() noexcept
{
	reset();
}

capture_engine::capture_engine(vga_timing const &timing) noexcept
{
	reset(timing);
}

void capture_engine::reset() noexcept
{
	reset(vga_timing{});
}

void capture_engine::reset(vga_timing const &timing) noexcept
{
	m_state = capture_engine_state{};
	m_state.host.input_page = 1;
	m_state.host.fsync_asserted = true;
	m_state.host.hsync_asserted = true;
	m_state.vga.active_timing = valid_timing(timing) ? timing : vga_timing{};
	m_state.vga.pending_timing = m_state.vga.active_timing;
	rebuild_derived_state();
}

void capture_engine::restore_state(capture_engine_state const &saved) noexcept
{
	m_state = saved;
	rebuild_derived_state();
}

void capture_engine::rebuild_derived_state() noexcept
{
	if (!valid_timing(m_state.vga.active_timing))
		m_state.vga.active_timing = vga_timing{};

	if (m_state.vga.pending_timing_valid && !valid_timing(m_state.vga.pending_timing))
		m_state.vga.pending_timing_valid = false;

	m_timing_endpoints = make_vga_timing_endpoints(m_state.vga.active_timing);
	m_state.host.input_page &= 1;
	m_state.vga.output_page &= 1;
	m_state.vga.rejected_page &= 1;
	if (m_state.vga.rejected_reason == output_reject_reason::PERSISTENT_MODE_TRANSITION ||
			m_state.vga.rejected_reason > output_reject_reason::RENDER_PREFLIGHT)
	{
		m_state.vga.rejected_reason = output_reject_reason::NONE;
		m_state.vga.rejected_candidate_valid = false;
	}

	for (frame_buffer_state &buffer : m_state.buffers)
	{
		bool row_metadata_valid = true;
		std::uint32_t row_writes = 0;
		bool all_rows_present = true;
		for (std::uint8_t &count : buffer.row_fetch_counts)
		{
			if (count > 32)
			{
				count = 0;
				row_metadata_valid = false;
			}
			row_writes += count;
			all_rows_present = all_rows_present && count != 0;
		}
		buffer.writes = std::min<std::uint32_t>(buffer.writes, CAPTURE_PAGE_BYTES);
		buffer.first_address = std::min<std::uint16_t>(buffer.first_address, CAPTURE_PAGE_BYTES - 1);
		buffer.last_address = std::min<std::uint16_t>(buffer.last_address, CAPTURE_PAGE_BYTES - 1);
		buffer.active_lines = std::min<std::uint16_t>(buffer.active_lines, HOST_ACTIVE_LINES);
		buffer.first_mode = static_cast<std::uint8_t>(normalize_mode(static_cast<mode_kind>(buffer.first_mode)));
		buffer.last_mode = static_cast<std::uint8_t>(normalize_mode(static_cast<mode_kind>(buffer.last_mode)));
		if (!row_metadata_valid ||
				(buffer.complete &&
						(!all_rows_present || row_writes != buffer.writes)))
		{
			buffer.complete = false;
		}
	}

	bool working_row_metadata_valid = true;
	std::uint32_t working_row_fetches = 0;
	for (std::uint8_t &count : m_state.host.working_row_fetch_counts)
	{
		if (count > 32)
		{
			count = 0;
			working_row_metadata_valid = false;
		}
		working_row_fetches += count;
	}
	m_state.host.working_writes =
			std::min<std::uint32_t>(m_state.host.working_writes, CAPTURE_PAGE_BYTES);
	m_state.host.working_first_address =
			std::min<std::uint16_t>(m_state.host.working_first_address, CAPTURE_PAGE_BYTES - 1);
	m_state.host.working_last_address =
			std::min<std::uint16_t>(m_state.host.working_last_address, CAPTURE_PAGE_BYTES - 1);
	m_state.host.working_active_lines =
			std::min<std::uint16_t>(m_state.host.working_active_lines, HOST_ACTIVE_LINES);
	m_state.host.working_first_mode = static_cast<std::uint8_t>(
			normalize_mode(static_cast<mode_kind>(m_state.host.working_first_mode)));
	m_state.host.working_last_mode = static_cast<std::uint8_t>(
			normalize_mode(static_cast<mode_kind>(m_state.host.working_last_mode)));
	if (!working_row_metadata_valid ||
			(!m_state.host.frame_programming_suppressed &&
					working_row_fetches < m_state.host.working_writes))
	{
		m_state.host.frame_programming_suppressed = true;
	}

	if (m_state.host.active_line > HOST_ACTIVE_LINES)
		m_state.host.active_line = HOST_ACTIVE_LINES;
	if (m_state.host.column > 32)
		m_state.host.column = 32;
	if (!m_state.host.frame_started)
	{
		m_state.host.frame_boundary_pending = false;
		m_state.host.line_boundary_pending = false;
		m_state.host.late_burst_active = false;
	}
	else if (!m_state.host.hsync_asserted)
	{
		m_state.host.late_burst_active = false;
	}
	if (m_state.vga.hcounter >= m_timing_endpoints.h_total)
		m_state.vga.hcounter = 0;
	if (m_state.vga.vcounter >= m_timing_endpoints.v_total)
		m_state.vga.vcounter = 0;

	if (m_state.diagnostics.frame_sam_mismatches < SAM_MISMATCH_WARNING_THRESHOLD)
		m_state.diagnostics.sam_warning_issued = false;
}

void capture_engine::set_programming(bool programming) noexcept
{
	if (programming &&
			!m_state.programming &&
			m_state.vga.output_valid &&
			m_state.host.input_page == m_state.vga.output_page)
	{
		std::uint8_t const latest_page = m_state.vga.output_page ^ 1;
		frame_buffer_state const &latest = m_state.buffers[latest_page];
		if (latest.valid &&
				latest.complete &&
				!latest.programming_suppressed &&
				latest.generation > m_state.vga.output_generation &&
				(!m_state.vga.rejected_candidate_valid ||
						m_state.vga.rejected_page != latest_page ||
						m_state.vga.rejected_generation != latest.generation))
		{
			m_state.vga.output_page = latest_page;
			m_state.vga.output_generation = latest.generation;
		}
	}

	m_state.programming = programming;
	if (programming && m_state.host.frame_started)
		m_state.host.frame_programming_suppressed = true;
}

bool capture_engine::request_vga_timing(vga_timing const &timing) noexcept
{
	if (!valid_timing(timing))
		return false;

	if (m_state.vga.pending_timing_valid && vga_timing_equal(timing, m_state.vga.pending_timing))
		return true;

	if (vga_timing_equal(timing, m_state.vga.active_timing))
	{
		m_state.vga.pending_timing_valid = false;
		return true;
	}

	m_state.vga.pending_timing = timing;
	m_state.vga.pending_timing_valid = true;
	return true;
}

bool capture_engine::update_from_core(core const &control) noexcept
{
	set_programming(control.freeze());
	if (m_state.programming)
		return true;
	return request_vga_timing(control.registers().timing);
}

void capture_engine::host_hblank(bool asserted) noexcept
{
	host_capture_state &host = m_state.host;
	if (host.hsync_asserted == asserted)
		return;

	host.hsync_asserted = asserted;
	if (asserted)
	{
		host.line_boundary_pending = true;
		host.late_burst_active = false;
	}
	else
	{
		host.late_burst_active = false;
	}
}

host_sync_result capture_engine::host_vblank(bool asserted) noexcept
{
	host_capture_state &host = m_state.host;
	host_sync_result result;
	result.input_page = host.input_page;

	if (host.fsync_asserted == asserted)
		return result;

	host.fsync_asserted = asserted;
	if (asserted)
	{
		// MC6847 records the final body scanline before publishing the HS edge,
		// and that burst can arrive just after capture blanking asserts.
		if (host.frame_started)
		{
			host.frame_boundary_pending = true;
			result.boundary_pending = true;
		}
		return result;
	}

	if (!host.frame_started)
		return transition_host_frame(false);

	// Complete immediately when the late final burst has arrived.  If a caller
	// deasserts blanking first, retain the boundary until SAM offset zero.
	if (host.active_line == HOST_ACTIVE_LINES - 1 && host.line_had_active_fetch)
		return transition_host_frame(false);

	host.frame_boundary_pending = true;
	result.boundary_pending = true;
	return result;
}

capture_result capture_engine::host_fetch(host_fetch_event const &event, mode_kind mode) noexcept
{
	host_capture_state &host = m_state.host;
	capture_result result;
	result.input_page = host.input_page;
	mode = normalize_mode(mode);

	if (!host.fsync_asserted &&
			host.frame_boundary_pending &&
			pending_frame_starts_with(event))
	{
		host_sync_result const sync = transition_host_frame(host.hsync_asserted);
		result.frame_started = sync.frame_started;
		result.frame_completed = sync.frame_completed;
		result.completed_page = sync.completed_page;
		result.completed_generation = sync.completed_generation;
		result.input_page = sync.input_page;
	}

	if (!host.frame_started)
	{
		result.reject_reason = capture_reject_reason::NO_HOST_FRAME;
		return result;
	}
	bool const accepting_late_field_burst =
			host.frame_boundary_pending &&
			(host.active_line == HOST_ACTIVE_LINES - 1 ||
					(host.active_line == HOST_ACTIVE_LINES - 2 &&
							host.line_boundary_pending &&
							host.line_had_active_fetch));
	if (host.fsync_asserted && !accepting_late_field_burst)
	{
		result.reject_reason = capture_reject_reason::FIELD_SYNC;
		return result;
	}
	if (host.hsync_asserted && !host.line_boundary_pending && !host.late_burst_active)
	{
		result.reject_reason = capture_reject_reason::HORIZONTAL_SYNC;
		return result;
	}
	if (host.line_boundary_pending)
		apply_pending_line_boundary();
	if (event.sam_address_valid &&
			event.sam_address_is_offset &&
			host.working_active_lines == 0 &&
			!host.line_had_active_fetch &&
			host.column == 0 &&
			host.active_line == 0 &&
			event.sam_address != 0)
	{
		host.frame_boundary_pending = true;
		result.reject_reason = capture_reject_reason::FRAME_ALIGNMENT;
		return result;
	}
	if (event.phase != host_phase::ACTIVE)
	{
		result.reject_reason = capture_reject_reason::INACTIVE_PHASE;
		return result;
	}
	if (host.active_line >= HOST_ACTIVE_LINES)
	{
		result.reject_reason = capture_reject_reason::LINE_OUT_OF_RANGE;
		return result;
	}

	std::uint8_t const column = host.column;
	if (column >= 32)
	{
		result.reject_reason = capture_reject_reason::CADENCE_EXHAUSTED;
		return result;
	}
	++host.column;

	if (!host.line_had_active_fetch)
	{
		host.line_had_active_fetch = true;
		if (host.working_active_lines < HOST_ACTIVE_LINES)
			++host.working_active_lines;
	}

	result.sampled = true;
	result.input_address = capture_input_address(mode, host.active_line, column);
	result.source_offset = expected_sam_offset(mode, host.active_line, column);
	result.cell = make_cell(event, mode);
	result.stream_byte = result.cell.stream_byte();
	if (host.working_row_fetch_counts[host.active_line] < 32)
		++host.working_row_fetch_counts[host.active_line];
	compare_sam(result, event);

	if (!m_state.programming && !host.frame_programming_suppressed)
	{
		frame_buffer_state &page = m_state.buffers[host.input_page];
		page.cells[result.input_address] = result.cell;
		result.buffer_written = true;

		if (host.working_writes == 0)
		{
			host.working_first_address = result.input_address;
			host.working_first_mode = result.cell.mode_id;
			host.working_last_mode = result.cell.mode_id;
		}
		else if (host.working_last_mode != result.cell.mode_id)
		{
			if (host.working_mode_changes != UINT16_MAX)
				++host.working_mode_changes;
			host.working_last_mode = result.cell.mode_id;
		}

		host.working_last_address = result.input_address;
		if (host.working_writes != UINT32_MAX)
			++host.working_writes;
	}
	else
	{
		host.frame_programming_suppressed = true;
	}

	return result;
}

capture_result capture_engine::host_fetch(host_fetch_event const &event, core const &control) noexcept
{
	set_programming(control.freeze());
	return host_fetch(event, control.capture_mode(event.mode.pixel(event.data)));
}

vga_tick_result capture_engine::clock_vga() noexcept
{
	vga_capture_state &vga = m_state.vga;
	vga_tick_result result;

	bool const hsync_now =
			vga.hcounter >= m_timing_endpoints.h_front_end &&
			vga.hcounter < m_timing_endpoints.h_sync_end;
	bool const vsync_now =
			vga.vcounter >= m_timing_endpoints.v_front_end &&
			vga.vcounter < m_timing_endpoints.v_sync_end;
	bool const blanking_now =
			vga.hcounter >= m_timing_endpoints.h_active_end ||
			vga.vcounter >= m_timing_endpoints.v_active_end;
	bool const vertical_sync_started = vsync_now && !vga.vsync_asserted;

	if (vertical_sync_started)
	{
		result.vertical_sync_started = true;
		++vga.selections;

		if (m_state.programming)
		{
			result.output_held = true;
			++vga.held_frames;
			if (vga.output_valid)
			{
				result.frame_repeated = true;
				++vga.repeated_frames;
			}
		}
		else
		{
			bool const old_valid = vga.output_valid;
			std::uint64_t const old_generation = vga.output_generation;
			output_selection_result const selection = select_output_page();
			result.output_changed = selection.changed;
			result.output_rejected = selection.rejected;
			result.reject_reason = selection.reject_reason;

			if (old_valid && vga.output_valid && old_generation == vga.output_generation)
			{
				result.frame_repeated = true;
				++vga.repeated_frames;
			}
		}
	}

	vga.hsync_asserted = hsync_now;
	vga.vsync_asserted = vsync_now;
	vga.blanking = blanking_now;

	bool const horizontal_rollover = vga.hcounter == std::uint16_t(m_timing_endpoints.h_total - 1);
	bool const vertical_rollover =
			horizontal_rollover &&
			vga.vcounter == std::uint16_t(m_timing_endpoints.v_total - 1);

	// Use the conventional h_total * v_total frame length.  The HDL evidence
	// has a documented one-clock vertical-rollover ambiguity.
	if (horizontal_rollover)
		vga.hcounter = 0;
	else
		++vga.hcounter;

	if (vertical_rollover)
	{
		vga.vcounter = 0;
		++vga.frames;
		result.frame_started = true;

		if (vga.pending_timing_valid)
		{
			vga.active_timing = vga.pending_timing;
			vga.pending_timing_valid = false;
			m_timing_endpoints = make_vga_timing_endpoints(vga.active_timing);
			result.timing_applied = true;
		}
	}
	else if (horizontal_rollover)
	{
		++vga.vcounter;
	}

	++vga.pixel_clocks;
	return result;
}

frame_buffer_state const &capture_engine::buffer(std::size_t page) const noexcept
{
	return m_state.buffers[page & 1];
}

captured_cell capture_engine::read_cell(std::size_t page, std::uint16_t address) const noexcept
{
	if (address >= CAPTURE_PAGE_BYTES)
		return captured_cell{};
	return m_state.buffers[page & 1].cells[address];
}

captured_cell capture_engine::output_cell(std::uint16_t address) const noexcept
{
	return read_cell(m_state.vga.output_page, address);
}

std::uint64_t capture_engine::output_latency_frames() const noexcept
{
	if (!m_state.vga.output_valid)
		return 0;
	if (m_state.host.frames_completed < m_state.vga.output_generation)
		return 1;
	std::uint64_t const age = m_state.host.frames_completed - m_state.vga.output_generation;
	return age == UINT64_MAX ? UINT64_MAX : age + 1;
}

bool capture_engine::output_candidate(
		std::uint8_t &page,
		frame_buffer_state const *&frame) const noexcept
{
	page = m_state.host.input_page ^ 1;
	frame = &m_state.buffers[page];
	return frame->valid &&
			frame->complete &&
			!frame->programming_suppressed &&
			(!m_state.vga.output_valid ||
					frame->generation > m_state.vga.output_generation);
}

bool capture_engine::reject_output_candidate(
		std::uint8_t page,
		std::uint64_t generation,
		output_reject_reason reason) noexcept
{
	if (reason == output_reject_reason::NONE)
		return false;

	vga_capture_state &vga = m_state.vga;
	bool const changed =
			!vga.rejected_candidate_valid ||
			vga.rejected_page != (page & 1) ||
			vga.rejected_generation != generation ||
			vga.rejected_reason != reason;
	vga.rejected_page = page & 1;
	vga.rejected_generation = generation;
	vga.rejected_reason = reason;
	vga.rejected_candidate_valid = true;
	if (changed)
		++vga.rejected_candidates;
	return changed;
}

void capture_engine::allow_output_candidate(
		std::uint8_t page,
		std::uint64_t generation) noexcept
{
	vga_capture_state &vga = m_state.vga;
	if (vga.rejected_candidate_valid &&
			vga.rejected_page == (page & 1) &&
			vga.rejected_generation == generation)
	{
		vga.rejected_candidate_valid = false;
		vga.rejected_reason = output_reject_reason::NONE;
	}
}

bool capture_engine::valid_timing(vga_timing const &timing) noexcept
{
	std::uint32_t const horizontal_total =
			std::uint32_t(timing.h_active) +
			timing.h_front_porch +
			timing.h_sync_width +
			timing.h_back_porch;
	std::uint32_t const vertical_total =
			std::uint32_t(timing.v_active) +
			timing.v_front_porch +
			timing.v_sync_width +
			timing.v_back_porch;

	return timing.h_active != 0 &&
			timing.h_front_porch != 0 &&
			timing.h_sync_width != 0 &&
			timing.h_back_porch != 0 &&
			timing.v_active != 0 &&
			timing.v_front_porch != 0 &&
			timing.v_sync_width != 0 &&
			timing.v_back_porch != 0 &&
			horizontal_total <= VGA_HORIZONTAL_TOTAL_MAX &&
			vertical_total <= VGA_VERTICAL_TOTAL_MAX;
}

std::uint8_t capture_engine::fetches_per_host_line(mode_kind mode) noexcept
{
	mode_descriptor const &descriptor = core::describe_mode(normalize_mode(mode));
	return descriptor.capture == capture_cycle::LONG_CYCLE ? 16 : 32;
}

std::uint16_t capture_engine::reconstruct_input_address(
		mode_kind mode,
		std::uint16_t active_line,
		std::uint8_t column) noexcept
{
	mode = normalize_mode(mode);
	if (active_line >= HOST_ACTIVE_LINES || column >= fetches_per_host_line(mode))
		return INVALID_CAPTURE_ADDRESS;
	if (mode == mode_kind::W64)
		return std::uint16_t((active_line / 3) * 32 + column);
	return std::uint16_t(active_line * 32 + column);
}

std::uint16_t capture_engine::expected_sam_offset(
		mode_kind mode,
		std::uint16_t active_line,
		std::uint8_t column) noexcept
{
	mode = normalize_mode(mode);
	if (active_line >= HOST_ACTIVE_LINES || column >= fetches_per_host_line(mode))
		return INVALID_CAPTURE_ADDRESS;
	if (mode == mode_kind::ALPHA_SEMIGRAPHICS)
		return std::uint16_t((active_line / 12) * 32 + column);
	if (mode == mode_kind::W64)
		return std::uint16_t((active_line / 3) * 32 + column);
	if (mode == mode_kind::VG6)
		return std::uint16_t(active_line * 32 + column);

	mode_descriptor const &descriptor = core::describe_mode(mode);
	std::uint16_t const row_repeat = std::uint16_t(HOST_ACTIVE_LINES / descriptor.source_height);
	std::uint16_t const source_row = std::uint16_t(active_line / row_repeat);
	return std::uint16_t(source_row * descriptor.bytes_per_row + column);
}

bool capture_engine::persistent_mode_transition(frame_buffer_state const &frame) noexcept
{
	return frame.mode_changes != 0 && frame.first_mode != frame.last_mode;
}

char const *capture_engine::output_reject_name(output_reject_reason reason) noexcept
{
	switch (reason)
	{
	case output_reject_reason::NONE:
		return "none";
	case output_reject_reason::PERSISTENT_MODE_TRANSITION:
		return "persistent mode transition";
	case output_reject_reason::RENDER_PREFLIGHT:
		return "render preflight";
	}

	return "unknown";
}

mode_kind capture_engine::normalize_mode(mode_kind mode) noexcept
{
	return static_cast<unsigned>(mode) <= static_cast<unsigned>(mode_kind::W64)
			? mode
			: mode_kind::ALPHA_SEMIGRAPHICS;
}

captured_cell capture_engine::make_cell(host_fetch_event const &event, mode_kind mode) noexcept
{
	captured_cell result;
	bool const alpha_format = alpha_capture_format(mode);
	result.data = alpha_format ? BIT(event.data, 0, 6) : event.data;
	result.gm = BIT(event.mode.gm, 0, 3);
	result.mode_id = static_cast<std::uint8_t>(mode);
	result.flags =
			(event.mode.graphics ? captured_cell::FLAG_GRAPHICS : 0) |
			(event.mode.css ? captured_cell::FLAG_CSS : 0) |
			(alpha_format && event.mode.alpha_semigraphics ? captured_cell::FLAG_ALPHA_SEMIGRAPHICS : 0) |
			(alpha_format && event.mode.inverse ? captured_cell::FLAG_INVERSE : 0) |
			(event.mode.internal_external ? captured_cell::FLAG_INTERNAL_EXTERNAL : 0);
	return result;
}

void capture_engine::begin_host_frame(bool accepting_late_burst) noexcept
{
	host_capture_state &host = m_state.host;
	host.frame_started = true;
	host.active_line = 0;
	host.column = 0;
	host.line_had_active_fetch = false;
	host.working_writes = 0;
	host.working_row_fetch_counts.fill(0);
	host.working_first_address = 0;
	host.working_last_address = 0;
	host.working_active_lines = 0;
	host.working_mode_changes = 0;
	host.working_first_mode = static_cast<std::uint8_t>(mode_kind::ALPHA_SEMIGRAPHICS);
	host.working_last_mode = static_cast<std::uint8_t>(mode_kind::ALPHA_SEMIGRAPHICS);
	host.frame_programming_suppressed = m_state.programming;
	host.frame_boundary_pending = false;
	host.line_boundary_pending = false;
	host.late_burst_active = accepting_late_burst;

	capture_diagnostic_state &diagnostics = m_state.diagnostics;
	diagnostics.frame_sam_comparisons = 0;
	diagnostics.frame_sam_mismatches = 0;
	diagnostics.sam_base_valid = false;
	diagnostics.sam_warning_issued = false;
}

void capture_engine::finish_host_frame() noexcept
{
	host_capture_state &host = m_state.host;
	capture_diagnostic_state &diagnostics = m_state.diagnostics;
	diagnostics.completed_frame_sam_comparisons = diagnostics.frame_sam_comparisons;
	diagnostics.completed_frame_sam_mismatches = diagnostics.frame_sam_mismatches;
	++host.frames_completed;

	if (host.working_writes != 0)
	{
		frame_buffer_state &page = m_state.buffers[host.input_page];
		page.row_fetch_counts = host.working_row_fetch_counts;
		page.generation = host.frames_completed;
		page.writes = host.working_writes;
		page.first_address = host.working_first_address;
		page.last_address = host.working_last_address;
		page.active_lines = host.working_active_lines;
		page.mode_changes = host.working_mode_changes;
		page.first_mode = host.working_first_mode;
		page.last_mode = host.working_last_mode;
		std::uint32_t const accepted_fetches = std::accumulate(
				host.working_row_fetch_counts.begin(),
				host.working_row_fetch_counts.end(),
				std::uint32_t(0));
		bool const all_rows_present = std::all_of(
				host.working_row_fetch_counts.begin(),
				host.working_row_fetch_counts.end(),
				[] (std::uint8_t count) { return count != 0 && count <= 32; });
		bool const complete =
				!host.frame_programming_suppressed &&
				host.working_first_address == 0 &&
				host.working_active_lines == HOST_ACTIVE_LINES &&
				all_rows_present &&
				host.working_writes == accepted_fetches;

		page.valid = true;
		page.complete = complete;
		page.programming_suppressed = host.frame_programming_suppressed;
	}
}

host_sync_result capture_engine::transition_host_frame(bool accepting_late_burst) noexcept
{
	host_capture_state &host = m_state.host;
	host_sync_result result;
	result.input_page = host.input_page;

	if (host.frame_started &&
			host.working_writes == 0 &&
			!host.frame_programming_suppressed &&
			host.frame_boundary_pending)
	{
		result.frame_started = true;
		begin_host_frame(accepting_late_burst);
		return result;
	}

	bool const resume_after_programming =
			host.frame_started &&
			host.frame_programming_suppressed &&
			!m_state.programming;

	if (host.frame_started)
	{
		result.frame_completed = true;
		result.completed_page = host.input_page;
		finish_host_frame();
		result.completed_generation = host.frames_completed;
	}

	std::uint8_t const previous_input_page = host.input_page;
	if (resume_after_programming && m_state.vga.output_valid)
		host.input_page = m_state.vga.output_page ^ 1;
	else
		host.input_page ^= 1;
	result.input_page_changed = host.input_page != previous_input_page;
	result.input_page = host.input_page;
	result.frame_started = true;
	begin_host_frame(accepting_late_burst);
	return result;
}

bool capture_engine::pending_frame_starts_with(host_fetch_event const &event) const noexcept
{
	if (!event.sam_address_valid)
		return !m_state.host.line_had_active_fetch;
	if (event.sam_address_is_offset)
		return event.sam_address == 0;

	capture_diagnostic_state const &diagnostics = m_state.diagnostics;
	if (!diagnostics.sam_base_valid)
		return true;

	return std::uint16_t(event.sam_address - diagnostics.sam_base) == 0;
}

void capture_engine::apply_pending_line_boundary() noexcept
{
	host_capture_state &host = m_state.host;
	if (host.line_had_active_fetch && host.active_line < HOST_ACTIVE_LINES)
		++host.active_line;
	host.column = 0;
	host.line_had_active_fetch = false;
	host.line_boundary_pending = false;
	host.late_burst_active = host.hsync_asserted;
}

void capture_engine::compare_sam(capture_result &result, host_fetch_event const &event) noexcept
{
	if (!event.sam_address_valid || result.source_offset == INVALID_CAPTURE_ADDRESS)
		return;

	capture_diagnostic_state &diagnostics = m_state.diagnostics;
	if (event.sam_address_is_offset)
	{
		diagnostics.sam_base = 0;
		diagnostics.sam_base_valid = true;
	}
	else if (!diagnostics.sam_base_valid)
	{
		diagnostics.sam_base = std::uint16_t(event.sam_address - result.source_offset);
		diagnostics.sam_base_valid = true;
	}

	std::uint16_t const observed_offset = std::uint16_t(event.sam_address - diagnostics.sam_base);
	result.sam_compared = true;
	result.sam_matched = observed_offset == result.source_offset;
	result.observed_sam_offset = observed_offset;
	++diagnostics.sam_comparisons;
	++diagnostics.frame_sam_comparisons;
	diagnostics.last_sam_address = event.sam_address;
	diagnostics.last_source_offset = result.source_offset;
	diagnostics.last_observed_offset = observed_offset;

	if (!result.sam_matched)
	{
		++diagnostics.sam_mismatches;
		++diagnostics.frame_sam_mismatches;
		if (!diagnostics.sam_warning_issued &&
				diagnostics.frame_sam_mismatches >= SAM_MISMATCH_WARNING_THRESHOLD)
		{
			diagnostics.sam_warning_issued = true;
			result.sam_warning = true;
		}
	}
}

capture_engine::output_selection_result capture_engine::select_output_page() noexcept
{
	vga_capture_state &vga = m_state.vga;
	output_selection_result result;
	std::uint8_t selected_page = 0;
	frame_buffer_state const *selected_pointer = nullptr;
	if (!output_candidate(selected_page, selected_pointer))
		return result;
	frame_buffer_state const &selected = *selected_pointer;

	if (vga.rejected_candidate_valid &&
			vga.rejected_page == selected_page &&
			vga.rejected_generation == selected.generation)
	{
		result.rejected = true;
		result.reject_reason = vga.rejected_reason;
		return result;
	}

	vga.rejected_candidate_valid = false;
	vga.rejected_reason = output_reject_reason::NONE;
	result.changed =
			vga.output_page != selected_page ||
			vga.output_generation != selected.generation;

	vga.output_page = selected_page;
	vga.output_valid = true;
	vga.output_generation = selected.generation;
	return result;
}

} // namespace cocovga
