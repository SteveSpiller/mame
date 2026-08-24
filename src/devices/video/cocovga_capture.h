// license:BSD-3-Clause
// copyright-holders:Stephen Spiller

#ifndef MAME_VIDEO_COCOVGA_CAPTURE_H
#define MAME_VIDEO_COCOVGA_CAPTURE_H

#pragma once

#include "cocovga_core.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace cocovga
{

constexpr std::size_t CAPTURE_PAGE_COUNT = 2;
constexpr std::size_t CAPTURE_PAGE_BYTES = 6'144;
constexpr std::uint16_t HOST_ACTIVE_LINES = 192;
constexpr std::uint32_t SAM_MISMATCH_WARNING_THRESHOLD = 8;

// MAME publishes raw MC6847 electrical pin levels, where low is asserted.
// Capture consumes logical blanking assertions, while the combination lock
// consumes the unmodified FS pin level.
constexpr bool hblank_from_hs_pin(bool hs_pin_high) noexcept { return !hs_pin_high; }
constexpr bool vblank_from_fs_pin(bool fs_pin_high) noexcept { return !fs_pin_high; }

enum class host_phase : std::uint8_t
{
	ACTIVE,
	BORDER,
	BLANKING
};

enum class capture_reject_reason : std::uint8_t
{
	NONE,
	NO_HOST_FRAME,
	FIELD_SYNC,
	HORIZONTAL_SYNC,
	INACTIVE_PHASE,
	LINE_OUT_OF_RANGE,
	CADENCE_EXHAUSTED,
	FRAME_ALIGNMENT
};

enum class output_reject_reason : std::uint8_t
{
	NONE,
	PERSISTENT_MODE_TRANSITION,
	RENDER_PREFLIGHT
};

struct host_mode_pins
{
	std::uint8_t gm = 0;
	bool graphics = false;
	bool css = false;
	bool alpha_semigraphics = false;
	bool inverse = false;
	bool internal_external = false;

	pixel_input pixel(std::uint8_t data) const noexcept;
};

struct host_fetch_event
{
	// MAME's VDG callback supplies a frame-relative offset.  Other callers may
	// supply an absolute SAM address and let the diagnostic latch its base.
	std::uint16_t sam_address = 0;
	std::uint8_t data = 0;
	host_mode_pins mode;
	host_phase phase = host_phase::ACTIVE;
	bool sam_address_valid = false;
	bool sam_address_is_offset = false;
};

struct captured_cell
{
	static constexpr std::uint8_t FLAG_GRAPHICS = 0x01;
	static constexpr std::uint8_t FLAG_CSS = 0x02;
	static constexpr std::uint8_t FLAG_ALPHA_SEMIGRAPHICS = 0x04;
	static constexpr std::uint8_t FLAG_INVERSE = 0x08;
	static constexpr std::uint8_t FLAG_INTERNAL_EXTERNAL = 0x10;

	std::uint8_t data = 0;
	std::uint8_t gm = 0;
	std::uint8_t mode_id = static_cast<std::uint8_t>(mode_kind::ALPHA_SEMIGRAPHICS);
	std::uint8_t flags = 0;

	mode_kind mode() const noexcept;
	bool graphics() const noexcept { return (flags & FLAG_GRAPHICS) != 0; }
	bool css() const noexcept { return (flags & FLAG_CSS) != 0; }
	bool alpha_semigraphics() const noexcept { return (flags & FLAG_ALPHA_SEMIGRAPHICS) != 0; }
	bool inverse() const noexcept { return (flags & FLAG_INVERSE) != 0; }
	bool internal_external() const noexcept { return (flags & FLAG_INTERNAL_EXTERNAL) != 0; }
	bool alpha_format() const noexcept;
	std::uint8_t stream_byte() const noexcept;
	pixel_input pixel() const noexcept;
};

struct capture_result
{
	bool sampled = false;
	bool buffer_written = false;
	bool sam_compared = false;
	bool sam_matched = true;
	bool sam_warning = false;
	bool frame_started = false;
	bool frame_completed = false;
	capture_reject_reason reject_reason = capture_reject_reason::NONE;
	std::uint8_t input_page = 0;
	std::uint8_t completed_page = 0;
	std::uint16_t input_address = 0;
	std::uint16_t source_offset = 0;
	std::uint16_t observed_sam_offset = 0;
	std::uint8_t stream_byte = 0;
	std::uint64_t completed_generation = 0;
	captured_cell cell;
};

struct frame_buffer_state
{
	std::array<captured_cell, CAPTURE_PAGE_BYTES> cells{};
	// The row length is explicit; each retained cell's mode_id records the
	// cadence that produced that fetch.
	std::array<std::uint8_t, HOST_ACTIVE_LINES> row_fetch_counts{};
	std::uint64_t generation = 0;
	std::uint32_t writes = 0;
	std::uint16_t first_address = 0;
	std::uint16_t last_address = 0;
	std::uint16_t active_lines = 0;
	std::uint16_t mode_changes = 0;
	std::uint8_t first_mode = static_cast<std::uint8_t>(mode_kind::ALPHA_SEMIGRAPHICS);
	std::uint8_t last_mode = static_cast<std::uint8_t>(mode_kind::ALPHA_SEMIGRAPHICS);
	bool valid = false;
	bool complete = false;
	bool programming_suppressed = false;
};

struct host_capture_state
{
	std::array<std::uint8_t, HOST_ACTIVE_LINES> working_row_fetch_counts{};
	std::uint64_t frames_completed = 0;
	std::uint32_t working_writes = 0;
	std::uint16_t working_first_address = 0;
	std::uint16_t working_last_address = 0;
	std::uint16_t working_active_lines = 0;
	std::uint16_t working_mode_changes = 0;
	std::uint16_t active_line = 0;
	std::uint8_t column = 0;
	std::uint8_t input_page = 1;
	std::uint8_t working_first_mode = static_cast<std::uint8_t>(mode_kind::ALPHA_SEMIGRAPHICS);
	std::uint8_t working_last_mode = static_cast<std::uint8_t>(mode_kind::ALPHA_SEMIGRAPHICS);
	bool frame_started = false;
	bool fsync_asserted = true;
	bool hsync_asserted = true;
	bool line_had_active_fetch = false;
	bool frame_programming_suppressed = false;
	bool frame_boundary_pending = false;
	bool line_boundary_pending = false;
	bool late_burst_active = false;
};

struct vga_capture_state
{
	vga_timing active_timing;
	vga_timing pending_timing;
	std::uint64_t pixel_clocks = 0;
	std::uint64_t frames = 0;
	std::uint64_t selections = 0;
	std::uint64_t repeated_frames = 0;
	std::uint64_t held_frames = 0;
	std::uint64_t rejected_candidates = 0;
	std::uint64_t output_generation = 0;
	std::uint64_t rejected_generation = 0;
	std::uint16_t hcounter = 0;
	std::uint16_t vcounter = 0;
	std::uint8_t output_page = 0;
	std::uint8_t rejected_page = 0;
	output_reject_reason rejected_reason = output_reject_reason::NONE;
	bool pending_timing_valid = false;
	bool hsync_asserted = false;
	bool vsync_asserted = false;
	bool blanking = false;
	bool output_valid = false;
	bool rejected_candidate_valid = false;
};

struct capture_diagnostic_state
{
	std::uint64_t sam_comparisons = 0;
	std::uint64_t sam_mismatches = 0;
	std::uint32_t frame_sam_comparisons = 0;
	std::uint32_t frame_sam_mismatches = 0;
	std::uint32_t completed_frame_sam_comparisons = 0;
	std::uint32_t completed_frame_sam_mismatches = 0;
	std::uint16_t sam_base = 0;
	std::uint16_t last_sam_address = 0;
	std::uint16_t last_source_offset = 0;
	std::uint16_t last_observed_offset = 0;
	bool sam_base_valid = false;
	bool sam_warning_issued = false;
};

struct capture_engine_state
{
	std::array<frame_buffer_state, CAPTURE_PAGE_COUNT> buffers{};
	host_capture_state host;
	vga_capture_state vga;
	capture_diagnostic_state diagnostics;
	bool programming = false;
};

struct host_sync_result
{
	bool frame_started = false;
	bool frame_completed = false;
	bool input_page_changed = false;
	bool boundary_pending = false;
	std::uint8_t completed_page = 0;
	std::uint8_t input_page = 0;
	std::uint64_t completed_generation = 0;
};

struct vga_tick_result
{
	bool frame_started = false;
	bool vertical_sync_started = false;
	bool output_changed = false;
	bool output_rejected = false;
	bool frame_repeated = false;
	bool output_held = false;
	bool timing_applied = false;
	output_reject_reason reject_reason = output_reject_reason::NONE;
};

class capture_engine
{
public:
	capture_engine() noexcept;
	explicit capture_engine(vga_timing const &timing) noexcept;

	// Lifetime and mutable state.
	void reset() noexcept;
	void reset(vga_timing const &timing) noexcept;

	// Call rebuild_derived_state after a save-state loader writes state() directly.
	capture_engine_state &state() noexcept { return m_state; }
	capture_engine_state const &state() const noexcept { return m_state; }
	void restore_state(capture_engine_state const &saved) noexcept;
	void rebuild_derived_state() noexcept;

	void set_programming(bool programming) noexcept;
	bool programming() const noexcept { return m_state.programming; }

	// Timing configuration and host capture.
	// Valid requests take effect only at a VGA frame boundary.
	bool request_vga_timing(vga_timing const &timing) noexcept;
	bool update_from_core(core const &control) noexcept;

	// These inputs are logical capture blanking assertions, not raw MC6847 pins.
	void host_hblank(bool asserted) noexcept;
	host_sync_result host_vblank(bool asserted) noexcept;
	void host_hsync(bool asserted) noexcept { host_hblank(asserted); }
	host_sync_result host_fsync(bool asserted) noexcept { return host_vblank(asserted); }
	capture_result host_fetch(host_fetch_event const &event, mode_kind mode) noexcept;
	capture_result host_fetch(host_fetch_event const &event, core const &control) noexcept;

	vga_tick_result clock_vga() noexcept;

	// Captured frame access and output selection.
	frame_buffer_state const &buffer(std::size_t page) const noexcept;
	captured_cell read_cell(std::size_t page, std::uint16_t address) const noexcept;
	captured_cell output_cell(std::uint16_t address) const noexcept;
	bool output_candidate(
			std::uint8_t &page,
			frame_buffer_state const *&frame) const noexcept;
	bool reject_output_candidate(
			std::uint8_t page,
			std::uint64_t generation,
			output_reject_reason reason) noexcept;
	void allow_output_candidate(
			std::uint8_t page,
			std::uint64_t generation) noexcept;

	std::uint8_t input_page() const noexcept { return m_state.host.input_page; }
	std::uint8_t output_page() const noexcept { return m_state.vga.output_page; }
	bool output_valid() const noexcept { return m_state.vga.output_valid; }
	std::uint64_t output_generation() const noexcept { return m_state.vga.output_generation; }
	std::uint64_t output_latency_frames() const noexcept;

	std::uint16_t hcounter() const noexcept { return m_state.vga.hcounter; }
	std::uint16_t vcounter() const noexcept { return m_state.vga.vcounter; }
	bool hsync_asserted() const noexcept { return m_state.vga.hsync_asserted; }
	bool vsync_asserted() const noexcept { return m_state.vga.vsync_asserted; }
	bool blanking() const noexcept { return m_state.vga.blanking; }
	vga_timing_endpoints const &timing_endpoints() const noexcept { return m_timing_endpoints; }

	// Stateless validation and address helpers.
	static bool valid_timing(vga_timing const &timing) noexcept;
	static std::uint8_t fetches_per_host_line(mode_kind mode) noexcept;
	static std::uint16_t reconstruct_input_address(
			mode_kind mode,
			std::uint16_t active_line,
			std::uint8_t column) noexcept;
	static std::uint16_t expected_sam_offset(
			mode_kind mode,
			std::uint16_t active_line,
			std::uint8_t column) noexcept;
	static bool persistent_mode_transition(frame_buffer_state const &frame) noexcept;
	static char const *output_reject_name(output_reject_reason reason) noexcept;

private:
	struct output_selection_result
	{
		bool changed = false;
		bool rejected = false;
		output_reject_reason reject_reason = output_reject_reason::NONE;
	};

	static mode_kind normalize_mode(mode_kind mode) noexcept;
	static captured_cell make_cell(host_fetch_event const &event, mode_kind mode) noexcept;

	void begin_host_frame(bool accepting_late_burst = false) noexcept;
	void finish_host_frame() noexcept;
	host_sync_result transition_host_frame(bool accepting_late_burst) noexcept;
	bool pending_frame_starts_with(host_fetch_event const &event) const noexcept;
	void apply_pending_line_boundary() noexcept;
	void compare_sam(capture_result &result, host_fetch_event const &event) noexcept;
	output_selection_result select_output_page() noexcept;

	capture_engine_state m_state;
	vga_timing_endpoints m_timing_endpoints;
};

static_assert(sizeof(captured_cell) == 4);
static_assert(std::is_trivially_copyable<capture_engine_state>::value);
static_assert(std::is_standard_layout<capture_engine_state>::value);
static_assert(sizeof(capture_engine_state) <= 64 * 1'024);

} // namespace cocovga

#endif // MAME_VIDEO_COCOVGA_CAPTURE_H
