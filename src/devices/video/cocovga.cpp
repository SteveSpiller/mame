// license:BSD-3-Clause
// copyright-holders:Stephen Spiller

#include "emu.h"

#include "cocovga.h"

#include <algorithm>

#define LOG_LOCK      (1U << 1)
#define LOG_REGISTER  (1U << 2)
#define LOG_CAPTURE   (1U << 3)
#define LOG_TIMING    (1U << 4)
#define LOG_RENDER    (1U << 5)
#define LOG_FONT      (1U << 6)

#define VERBOSE (0)
#include "logmacro.h"

#define LOG_LOCK_EVENT(...)      LOGMASKED(LOG_LOCK, __VA_ARGS__)
#define LOG_REGISTER_ACCESS(...) LOGMASKED(LOG_REGISTER, __VA_ARGS__)
#define LOG_CAPTURE_EVENT(...)   LOGMASKED(LOG_CAPTURE, __VA_ARGS__)
#define LOG_TIMING_EVENT(...)    LOGMASKED(LOG_TIMING, __VA_ARGS__)
#define LOG_RENDER_EVENT(...)    LOGMASKED(LOG_RENDER, __VA_ARGS__)
#define LOG_FONT_SOURCE(...)     LOGMASKED(LOG_FONT, __VA_ARGS__)

ALLOW_SAVE_TYPE(cocovga::model)
ALLOW_SAVE_TYPE(cocovga::combo_state)
ALLOW_SAVE_TYPE(cocovga::mode_kind)
ALLOW_SAVE_TYPE(cocovga::render_status)
ALLOW_SAVE_TYPE(cocovga::capture_reject_reason)
ALLOW_SAVE_TYPE(cocovga::output_reject_reason)

DEFINE_DEVICE_TYPE(COCOVGA, cocovga_device, "cocovga", "CoCoVGA video generator")

// The board regenerates the VDG character set inside its FPGA from both
// Motorola character generator ROMs, and the font register selects between the
// resulting banks, so it carries a copy of each.  These are the same images
// the MC6847 and MC6847T1 devices declare in mc6847.cpp; keep the names and
// hashes identical so that a redump updates both together.
ROM_START( cocovga )
	ROM_REGION( 0x0118, "chargen", 0 ) // extracted from decap imagery
	ROM_LOAD( "mc6847_charset.bin", 0x0000, 0x0118, CRC(3b22d071) SHA1(5e9d68e55e73cae3d28adaff34fe115e00029009) )

	ROM_REGION( 0x0480, "chargen_t1", 0 ) // reconstructed from video display, needs redump
	ROM_LOAD( "mc6847t1_charset_needredump.bin", 0x0000, 0x0480, CRC(42e62f8d) SHA1(1f09a076732a1e4b132cf298f0d1747df817e1c6) )
ROM_END

static_assert(mc6847_charset::INTERNAL_ROM_BYTES == 0x0118, "chargen region no longer matches the MC6847 ROM layout");
static_assert(mc6847_charset::T1_ROM_BYTES == 0x0480, "chargen_t1 region no longer matches the MC6847T1 ROM layout");

namespace
{

constexpr std::uint32_t OPAQUE_BLACK = 0xff00'0000U;

cocovga::output_policy normalize_output_policy(cocovga::output_policy policy) noexcept
{
	switch (policy)
	{
	case cocovga::output_policy::COMPOSITE:
	case cocovga::output_policy::VGA:
	case cocovga::output_policy::BOTH:
		return policy;

	default:
		return cocovga::output_policy::BOTH;
	}
}

bool same_mode(cocovga::host_mode_pins const &left, cocovga::host_mode_pins const &right) noexcept
{
	return left.gm == right.gm &&
			left.graphics == right.graphics &&
			left.css == right.css &&
			left.alpha_semigraphics == right.alpha_semigraphics &&
			left.inverse == right.inverse &&
			left.internal_external == right.internal_external;
}

std::uint8_t mode_selector(cocovga::host_mode_pins const &mode) noexcept
{
	return std::uint8_t(
			(mode.css ? 0x10 : 0x00) |
			(mode.graphics ? 0x08 : 0x00) |
			BIT(mode.gm, 0, 3));
}

} // anonymous namespace


cocovga_device::cocovga_device(
		machine_config const &mconfig,
		char const *tag,
		device_t *owner,
		std::uint32_t clock) :
	device_t(mconfig, COCOVGA, tag, owner, clock),
	device_video_interface(mconfig, *this),
	m_chargen(*this, "chargen"),
	m_chargen_t1(*this, "chargen_t1"),
	m_core(cocovga::model::MODERN),
	m_capture(m_core.registers().timing)
{
}

//-------------------------------------------------
//  device_rom_region - device-specific ROM region
//-------------------------------------------------

tiny_rom_entry const *cocovga_device::device_rom_region() const
{
	return ROM_NAME( cocovga );
}

void cocovga_device::apply_configuration(
		cocovga::model board,
		cocovga::output_policy policy)
{
	bool const model_changed = m_core.board_model() != board;
	if (model_changed)
		m_core.set_model(board);

	m_output_policy = normalize_output_policy(policy);

	if (model_changed)
	{
		reset_runtime();
		queue_screen_timing();
	}
}

bool cocovga_device::composite_output_active() const noexcept
{
	return m_output_policy != cocovga::output_policy::VGA;
}

bool cocovga_device::vga_output_active() const noexcept
{
	return m_output_policy != cocovga::output_policy::COMPOSITE;
}

void cocovga_device::device_start()
{
	// The board reimplements the VDG character generator from its own copies
	// of both Motorola character generator ROMs, and the font register selects
	// between the resulting banks independently of the VDG the host fits.
	m_charset.load(m_chargen->base(), m_chargen_t1->base());
	m_renderer.set_character_generator(&m_charset);

	m_render_bitmap.allocate(cocovga::RENDER_WIDTH, cocovga::RENDER_HEIGHT);
	m_render_candidate_bitmap.allocate(cocovga::RENDER_WIDTH, cocovga::RENDER_HEIGHT);
	m_render_bitmap.fill(OPAQUE_BLACK);
	m_render_candidate_bitmap.fill(OPAQUE_BLACK);
	m_button_1_hold_timer = timer_alloc(FUNC(cocovga_device::button_hold), this);
	m_button_2_hold_timer = timer_alloc(FUNC(cocovga_device::button_hold), this);

	screen().register_vblank_callback(vblank_state_delegate(&cocovga_device::vblank, this));

	register_core_save_state();
	register_capture_save_state();
	register_renderer_save_state();
	register_device_save_state();

	// Initial configuration happens before screen timers begin running.
	apply_screen_timing(m_capture.state().vga.active_timing);

	LOG_FONT_SOURCE(
			"%s: character generator expanded from both banks (%u + %u bytes)\n",
			tag(),
			unsigned(m_chargen->bytes()),
			unsigned(m_chargen_t1->bytes()));
}

void cocovga_device::device_reset()
{
	reset_runtime();
	queue_screen_timing();
}

void cocovga_device::device_post_load()
{
	m_mode.gm = BIT(m_mode.gm, 0, 3);
	m_status_overlay.state().gm = BIT(m_status_overlay.state().gm, 0, 3);
	m_capture.rebuild_derived_state();
	m_render_bitmap.fill(OPAQUE_BLACK);
	m_render_candidate_bitmap.fill(OPAQUE_BLACK);
	m_have_rendered_frame = false;
	sync_capture_control();
	queue_screen_timing();
}

void cocovga_device::device_clock_changed()
{
	if (started())
		queue_screen_timing();
}

void cocovga_device::reset_runtime()
{
	m_core.reset();
	m_capture.reset(m_core.registers().timing);
	m_renderer.reset();
	m_status_overlay.reset();
	m_render_bitmap.fill(OPAQUE_BLACK);
	m_render_candidate_bitmap.fill(OPAQUE_BLACK);

	m_mode = cocovga::host_mode_pins{};
	m_applied_timing = m_capture.state().vga.active_timing;
	m_last_requested_timing = m_core.registers().timing;
	m_last_render_status = cocovga::render_status::FRAME_UNAVAILABLE;
	m_last_capture_reject = cocovga::capture_reject_reason::NONE;
	m_frame_boundaries = 0;
	m_capture_fetches = 0;
	m_register_writes = 0;
	m_rejected_timing_requests = 0;
	m_rejected_render_candidates = 0;
	m_render_failures = 0;
	m_render_repeats = 0;
	m_last_render_generation = 0;
	m_last_successful_generation = 0;
	m_hsync_pin = false;
	m_fsync_pin = false;
	m_button_1 = false;
	m_button_2 = false;
	m_screen_timing_valid = false;
	m_screen_timing_pending = true;
	m_timing_request_seen = true;
	m_last_timing_request_valid = true;
	m_have_rendered_frame = false;

	if (m_button_1_hold_timer != nullptr)
		m_button_1_hold_timer->adjust(attotime::never);
	if (m_button_2_hold_timer != nullptr)
		m_button_2_hold_timer->adjust(attotime::never);
}

void cocovga_device::register_core_save_state()
{
	cocovga::core_state &state = m_core.state();
	cocovga::register_file &registers = state.registers;
	cocovga::button_runtime &buttons = state.buttons;

	save_item(state.board, "core.board");
	save_item(registers.font, "core.registers.font");
	save_item(registers.artifact, "core.registers.artifact");
	save_item(registers.extras, "core.registers.extras");
	save_item(registers.enhanced_modes, "core.registers.enhanced_modes");
	save_item(registers.semigraphics_palette_shadow, "core.registers.semigraphics_palette_shadow");
	save_item(registers.artifact_palette_shadow, "core.registers.artifact_palette_shadow");
	save_item(registers.extra_palette_shadow, "core.registers.extra_palette_shadow");
	save_item(registers.semigraphics_palette_visible, "core.registers.semigraphics_palette_visible");
	save_item(registers.artifact_palette_visible, "core.registers.artifact_palette_visible");
	save_item(registers.extra_palette_visible, "core.registers.extra_palette_visible");
	save_item(registers.timing.h_active, "core.registers.timing.h_active");
	save_item(registers.timing.h_front_porch, "core.registers.timing.h_front_porch");
	save_item(registers.timing.h_sync_width, "core.registers.timing.h_sync_width");
	save_item(registers.timing.h_back_porch, "core.registers.timing.h_back_porch");
	save_item(registers.timing.v_active, "core.registers.timing.v_active");
	save_item(registers.timing.v_front_porch, "core.registers.timing.v_front_porch");
	save_item(registers.timing.v_sync_width, "core.registers.timing.v_sync_width");
	save_item(registers.timing.v_back_porch, "core.registers.timing.v_back_porch");
	save_item(state.character_ram, "core.character_ram");
	save_item(state.reset_mask, "core.reset_mask");
	save_item(state.edit_mask, "core.edit_mask");
	save_item(state.selected_page, "core.selected_page");
	save_item(state.page_selected, "core.page_selected");
	save_item(state.lock_state, "core.lock_state");
	save_item(state.character_write_enabled, "core.character_write_enabled");
	save_item(buttons.button_1_hold_ticks, "core.buttons.button_1_hold_ticks");
	save_item(buttons.button_2_hold_ticks, "core.buttons.button_2_hold_ticks");
	save_item(buttons.button_1_down, "core.buttons.button_1_down");
	save_item(buttons.button_2_down, "core.buttons.button_2_down");
	save_item(buttons.text_button_1_cycle, "core.buttons.text_button_1_cycle");
	save_item(buttons.graphics_button_1_cycle, "core.buttons.graphics_button_1_cycle");
	save_item(buttons.rg6_button_1_cycle, "core.buttons.rg6_button_1_cycle");
	save_item(buttons.text_button_2_cycle, "core.buttons.text_button_2_cycle");
	save_item(buttons.rg6_button_2_cycle, "core.buttons.rg6_button_2_cycle");
	save_item(buttons.force_lowercase_toggle, "core.buttons.force_lowercase_toggle");
	save_item(buttons.quiet_status_toggle, "core.buttons.quiet_status_toggle");
	save_item(buttons.w64_toggle, "core.buttons.w64_toggle");
	save_item(state.vg6_active, "core.vg6_active");
	save_item(state.w64_active, "core.w64_active");
}

void cocovga_device::register_capture_save_state()
{
	cocovga::capture_engine_state &state = m_capture.state();

	for (unsigned page = 0; page < cocovga::CAPTURE_PAGE_COUNT; ++page)
	{
		auto &cells = state.buffers[page].cells;
		save_item(STRUCT_MEMBER(cells, data), page);
		save_item(STRUCT_MEMBER(cells, gm), page);
		save_item(STRUCT_MEMBER(cells, mode_id), page);
		save_item(STRUCT_MEMBER(cells, flags), page);
		save_item(state.buffers[page].row_fetch_counts, "capture.buffers.row_fetch_counts", page);
	}

	save_item(STRUCT_MEMBER(state.buffers, generation));
	save_item(STRUCT_MEMBER(state.buffers, writes));
	save_item(STRUCT_MEMBER(state.buffers, first_address));
	save_item(STRUCT_MEMBER(state.buffers, last_address));
	save_item(STRUCT_MEMBER(state.buffers, active_lines));
	save_item(STRUCT_MEMBER(state.buffers, mode_changes));
	save_item(STRUCT_MEMBER(state.buffers, first_mode));
	save_item(STRUCT_MEMBER(state.buffers, last_mode));
	save_item(STRUCT_MEMBER(state.buffers, valid));
	save_item(STRUCT_MEMBER(state.buffers, complete));
	save_item(STRUCT_MEMBER(state.buffers, programming_suppressed));

	save_item(state.host.working_row_fetch_counts, "capture.host.working_row_fetch_counts");
	save_item(state.host.frames_completed, "capture.host.frames_completed");
	save_item(state.host.working_writes, "capture.host.working_writes");
	save_item(state.host.working_first_address, "capture.host.working_first_address");
	save_item(state.host.working_last_address, "capture.host.working_last_address");
	save_item(state.host.working_active_lines, "capture.host.working_active_lines");
	save_item(state.host.working_mode_changes, "capture.host.working_mode_changes");
	save_item(state.host.active_line, "capture.host.active_line");
	save_item(state.host.column, "capture.host.column");
	save_item(state.host.input_page, "capture.host.input_page");
	save_item(state.host.working_first_mode, "capture.host.working_first_mode");
	save_item(state.host.working_last_mode, "capture.host.working_last_mode");
	save_item(state.host.frame_started, "capture.host.frame_started");
	save_item(state.host.fsync_asserted, "capture.host.fsync_asserted");
	save_item(state.host.hsync_asserted, "capture.host.hsync_asserted");
	save_item(state.host.line_had_active_fetch, "capture.host.line_had_active_fetch");
	save_item(state.host.frame_programming_suppressed, "capture.host.frame_programming_suppressed");
	save_item(state.host.frame_boundary_pending, "capture.host.frame_boundary_pending");
	save_item(state.host.line_boundary_pending, "capture.host.line_boundary_pending");
	save_item(state.host.late_burst_active, "capture.host.late_burst_active");

	save_item(state.vga.active_timing.h_active, "capture.vga.active_timing.h_active");
	save_item(state.vga.active_timing.h_front_porch, "capture.vga.active_timing.h_front_porch");
	save_item(state.vga.active_timing.h_sync_width, "capture.vga.active_timing.h_sync_width");
	save_item(state.vga.active_timing.h_back_porch, "capture.vga.active_timing.h_back_porch");
	save_item(state.vga.active_timing.v_active, "capture.vga.active_timing.v_active");
	save_item(state.vga.active_timing.v_front_porch, "capture.vga.active_timing.v_front_porch");
	save_item(state.vga.active_timing.v_sync_width, "capture.vga.active_timing.v_sync_width");
	save_item(state.vga.active_timing.v_back_porch, "capture.vga.active_timing.v_back_porch");
	save_item(state.vga.pending_timing.h_active, "capture.vga.pending_timing.h_active");
	save_item(state.vga.pending_timing.h_front_porch, "capture.vga.pending_timing.h_front_porch");
	save_item(state.vga.pending_timing.h_sync_width, "capture.vga.pending_timing.h_sync_width");
	save_item(state.vga.pending_timing.h_back_porch, "capture.vga.pending_timing.h_back_porch");
	save_item(state.vga.pending_timing.v_active, "capture.vga.pending_timing.v_active");
	save_item(state.vga.pending_timing.v_front_porch, "capture.vga.pending_timing.v_front_porch");
	save_item(state.vga.pending_timing.v_sync_width, "capture.vga.pending_timing.v_sync_width");
	save_item(state.vga.pending_timing.v_back_porch, "capture.vga.pending_timing.v_back_porch");
	save_item(state.vga.pixel_clocks, "capture.vga.pixel_clocks");
	save_item(state.vga.frames, "capture.vga.frames");
	save_item(state.vga.selections, "capture.vga.selections");
	save_item(state.vga.repeated_frames, "capture.vga.repeated_frames");
	save_item(state.vga.held_frames, "capture.vga.held_frames");
	save_item(state.vga.rejected_candidates, "capture.vga.rejected_candidates");
	save_item(state.vga.output_generation, "capture.vga.output_generation");
	save_item(state.vga.rejected_generation, "capture.vga.rejected_generation");
	save_item(state.vga.hcounter, "capture.vga.hcounter");
	save_item(state.vga.vcounter, "capture.vga.vcounter");
	save_item(state.vga.output_page, "capture.vga.output_page");
	save_item(state.vga.rejected_page, "capture.vga.rejected_page");
	save_item(state.vga.rejected_reason, "capture.vga.rejected_reason");
	save_item(state.vga.pending_timing_valid, "capture.vga.pending_timing_valid");
	save_item(state.vga.hsync_asserted, "capture.vga.hsync_asserted");
	save_item(state.vga.vsync_asserted, "capture.vga.vsync_asserted");
	save_item(state.vga.blanking, "capture.vga.blanking");
	save_item(state.vga.output_valid, "capture.vga.output_valid");
	save_item(state.vga.rejected_candidate_valid, "capture.vga.rejected_candidate_valid");

	save_item(state.diagnostics.sam_comparisons, "capture.diagnostics.sam_comparisons");
	save_item(state.diagnostics.sam_mismatches, "capture.diagnostics.sam_mismatches");
	save_item(state.diagnostics.frame_sam_comparisons, "capture.diagnostics.frame_sam_comparisons");
	save_item(state.diagnostics.frame_sam_mismatches, "capture.diagnostics.frame_sam_mismatches");
	save_item(state.diagnostics.completed_frame_sam_comparisons, "capture.diagnostics.completed_frame_sam_comparisons");
	save_item(state.diagnostics.completed_frame_sam_mismatches, "capture.diagnostics.completed_frame_sam_mismatches");
	save_item(state.diagnostics.sam_base, "capture.diagnostics.sam_base");
	save_item(state.diagnostics.last_sam_address, "capture.diagnostics.last_sam_address");
	save_item(state.diagnostics.last_source_offset, "capture.diagnostics.last_source_offset");
	save_item(state.diagnostics.last_observed_offset, "capture.diagnostics.last_observed_offset");
	save_item(state.diagnostics.sam_base_valid, "capture.diagnostics.sam_base_valid");
	save_item(state.diagnostics.sam_warning_issued, "capture.diagnostics.sam_warning_issued");
	save_item(state.programming, "capture.programming");
}

void cocovga_device::register_renderer_save_state()
{
	cocovga::renderer_state &state = m_renderer.state();

	save_item(state.render_calls, "renderer.render_calls");
	save_item(state.frames_rendered, "renderer.frames_rendered");
	save_item(state.scanlines_rendered, "renderer.scanlines_rendered");
	save_item(state.last_generation, "renderer.last_generation");
	save_item(state.last_pixels_written, "renderer.last_pixels_written");
	save_item(state.last_mode_changes, "renderer.last_mode_changes");
	save_item(state.last_status, "renderer.last_status");
	save_item(state.last_first_mode, "renderer.last_first_mode");
	save_item(state.last_last_mode, "renderer.last_last_mode");
}

void cocovga_device::register_device_save_state()
{
	cocovga::render_effects::border_status_state &status = m_status_overlay.state();

	// Enable and output policy are reset-required machine configuration, not
	// runtime state; loading a state must not override the current configuration.
	save_item(m_mode.gm, "device.mode.gm");
	save_item(m_mode.graphics, "device.mode.graphics");
	save_item(m_mode.css, "device.mode.css");
	save_item(m_mode.alpha_semigraphics, "device.mode.alpha_semigraphics");
	save_item(m_mode.inverse, "device.mode.inverse");
	save_item(m_mode.internal_external, "device.mode.internal_external");
	save_item(m_applied_timing.h_active, "device.applied_timing.h_active");
	save_item(m_applied_timing.h_front_porch, "device.applied_timing.h_front_porch");
	save_item(m_applied_timing.h_sync_width, "device.applied_timing.h_sync_width");
	save_item(m_applied_timing.h_back_porch, "device.applied_timing.h_back_porch");
	save_item(m_applied_timing.v_active, "device.applied_timing.v_active");
	save_item(m_applied_timing.v_front_porch, "device.applied_timing.v_front_porch");
	save_item(m_applied_timing.v_sync_width, "device.applied_timing.v_sync_width");
	save_item(m_applied_timing.v_back_porch, "device.applied_timing.v_back_porch");
	save_item(m_last_requested_timing.h_active, "device.last_requested_timing.h_active");
	save_item(m_last_requested_timing.h_front_porch, "device.last_requested_timing.h_front_porch");
	save_item(m_last_requested_timing.h_sync_width, "device.last_requested_timing.h_sync_width");
	save_item(m_last_requested_timing.h_back_porch, "device.last_requested_timing.h_back_porch");
	save_item(m_last_requested_timing.v_active, "device.last_requested_timing.v_active");
	save_item(m_last_requested_timing.v_front_porch, "device.last_requested_timing.v_front_porch");
	save_item(m_last_requested_timing.v_sync_width, "device.last_requested_timing.v_sync_width");
	save_item(m_last_requested_timing.v_back_porch, "device.last_requested_timing.v_back_porch");
	save_item(m_last_render_status, "device.last_render_status");
	save_item(m_last_capture_reject, "device.last_capture_reject");
	save_item(m_frame_boundaries, "device.frame_boundaries");
	save_item(m_capture_fetches, "device.capture_fetches");
	save_item(m_register_writes, "device.register_writes");
	save_item(m_rejected_timing_requests, "device.rejected_timing_requests");
	save_item(m_rejected_render_candidates, "device.rejected_render_candidates");
	save_item(m_render_failures, "device.render_failures");
	save_item(m_render_repeats, "device.render_repeats");
	save_item(m_last_render_generation, "device.last_render_generation");
	save_item(m_last_successful_generation, "device.last_successful_generation");
	save_item(status.boot_remaining, "device.status.boot_remaining");
	save_item(status.mode_remaining, "device.status.mode_remaining");
	save_item(status.quiet, "device.status.quiet");
	save_item(status.have_mode, "device.status.have_mode");
	save_item(status.graphics, "device.status.graphics");
	save_item(status.gm, "device.status.gm");
	save_item(m_hsync_pin, "device.hsync_pin");
	save_item(m_fsync_pin, "device.fsync_pin");
	save_item(m_button_1, "device.button_1");
	save_item(m_button_2, "device.button_2");
	save_item(m_screen_timing_valid, "device.screen_timing_valid");
	save_item(m_screen_timing_pending, "device.screen_timing_pending");
	save_item(m_timing_request_seen, "device.timing_request_seen");
	save_item(m_last_timing_request_valid, "device.last_timing_request_valid");
}

void cocovga_device::update_lock()
{
	auto const clock_once = [this] ()
	{
		cocovga::combo_step const step = m_core.clock_combo(
				m_fsync_pin,
				mode_selector(m_mode));

		if (step.previous != step.current || step.page_captured || step.programming_released)
		{
			LOG_LOCK_EVENT(
					"%s: lock %u -> %u selector=%02x page=%d release=%d freeze=%d\n",
					tag(),
					unsigned(step.previous),
					unsigned(step.current),
					mode_selector(m_mode),
					step.page_captured,
					step.programming_released,
					step.freeze);
		}
		return step;
	};

	cocovga::combo_step step = clock_once();
	// WAIT2 and WAIT3 advance unconditionally on consecutive 3.58 MHz CoCo
	// clocks.  Settle those two clocks here rather than waiting for unrelated
	// sync/fetch callbacks, which may not occur during vertical blanking.
	for (unsigned clocks = 0;
			clocks < 2 &&
					(step.current == cocovga::combo_state::WAIT_MODE_2 ||
							step.current == cocovga::combo_state::WAIT_MODE_3);
			++clocks)
	{
		step = clock_once();
	}

	sync_capture_control();
}

void cocovga_device::sync_capture_control()
{
	bool const programming = m_core.freeze();
	if (programming != m_capture.programming())
		preflight_output_candidate();
	m_capture.set_programming(programming);
	m_status_overlay.set_quiet(m_core, status_input());
	if (programming)
		return;

	cocovga::vga_timing const &timing = m_core.registers().timing;
	if (m_timing_request_seen && cocovga::vga_timing_equal(timing, m_last_requested_timing))
		return;

	m_last_requested_timing = timing;
	m_timing_request_seen = true;
	m_last_timing_request_valid = m_capture.request_vga_timing(timing);
	if (m_last_timing_request_valid)
	{
		LOG_TIMING_EVENT(
				"%s: queued timing %ux%u totals %ux%u\n",
				tag(),
				timing.h_active,
				timing.v_active,
				cocovga::make_vga_timing_endpoints(timing).h_total,
				cocovga::make_vga_timing_endpoints(timing).v_total);
	}
	else
	{
		++m_rejected_timing_requests;
		LOG_TIMING_EVENT("%s: rejected invalid programmed timing\n", tag());
	}
}

cocovga::pixel_input cocovga_device::status_input() const noexcept
{
	if (!m_capture.output_valid())
		return m_mode.pixel(0);

	cocovga::frame_buffer_state const &frame =
			m_capture.buffer(m_capture.output_page());
	constexpr std::uint16_t STATUS_SAMPLE_ADDRESS = 13 * 32;
	std::uint16_t const unclamped_address =
			STATUS_SAMPLE_ADDRESS <= frame.last_address
					? STATUS_SAMPLE_ADDRESS
					: frame.first_address;
	std::uint16_t const address =
			std::min<std::uint16_t>(unclamped_address, cocovga::CAPTURE_PAGE_BYTES - 1);
	return frame.cells[address].pixel();
}

void cocovga_device::sync_status_overlay()
{
	cocovga::pixel_input const input = status_input();
	m_status_overlay.set_quiet(m_core, input);
	if (m_capture.output_valid())
		m_status_overlay.observe_mode(input);
}

void cocovga_device::mode_update(cocovga::host_mode_pins const &mode)
{
	cocovga::host_mode_pins normalized = mode;
	normalized.gm = BIT(mode.gm, 0, 3);

	if (!same_mode(m_mode, normalized))
		m_mode = normalized;

	// A PIA write or fetch is an observable CoCo clock for the event-driven
	// lock approximation, including repeated selector values during blanking.
	update_lock();
}

void cocovga_device::hsync_pin_w(int state)
{
	bool const pin_high = state != 0;
	if (m_hsync_pin == pin_high)
		return;

	m_hsync_pin = pin_high;
	m_capture.host_hblank(cocovga::hblank_from_hs_pin(pin_high));
}

void cocovga_device::fsync_pin_w(int state)
{
	bool const pin_high = state != 0;
	if (m_fsync_pin == pin_high)
		return;

	m_fsync_pin = pin_high;
	update_lock();

	cocovga::host_sync_result const result =
			m_capture.host_vblank(cocovga::vblank_from_fs_pin(pin_high));
	if (result.frame_started || result.frame_completed || result.boundary_pending)
	{
		LOG_CAPTURE_EVENT(
				"%s: host frame start=%d complete=%d pending=%d page=%u generation=%llu\n",
				tag(),
				result.frame_started,
				result.frame_completed,
				result.boundary_pending,
				result.completed_page,
				(long long unsigned)result.completed_generation);
	}
}

cocovga::capture_result cocovga_device::capture_fetch(cocovga::host_fetch_event const &event)
{
	mode_update(event.mode);
	++m_capture_fetches;

	cocovga::capture_result result = m_capture.host_fetch(event, m_core);
	m_last_capture_reject = result.reject_reason;

	if (result.sampled && m_core.freeze())
	{
		bool const written = m_core.ingest_selected_capture(result.input_address, result.stream_byte);
		if (written)
		{
			++m_register_writes;
			LOG_REGISTER_ACCESS(
					"%s: page=%02x address=%04x data=%02x\n",
					tag(),
					m_core.selected_page(),
					result.input_address,
					result.stream_byte);
			sync_capture_control();
		}
	}

	if (result.sam_compared && !result.sam_matched)
	{
		LOG_CAPTURE_EVENT(
				"%s: SAM mismatch address=%04x observed-offset=%04x expected-offset=%04x\n",
				tag(),
				event.sam_address,
				result.observed_sam_offset,
				result.source_offset);
		if (result.sam_warning)
		{
			// The host can legitimately re-point the SAM mid-frame; capture
			// recovers on the next frame boundary, so this is a developer
			// diagnostic rather than an emulation failure.
			LOG_CAPTURE_EVENT(
					"%s: CoCoVGA capture has %u SAM offset mismatches in the current frame\n",
					tag(),
					m_capture.state().diagnostics.frame_sam_mismatches);
		}
	}
	else if (!result.sampled && result.reject_reason != cocovga::capture_reject_reason::NONE)
	{
		LOG_CAPTURE_EVENT("%s: rejected fetch reason=%u\n", tag(), unsigned(result.reject_reason));
	}

	return result;
}

void cocovga_device::schedule_button_hold(bool first)
{
	emu_timer *const timer = first ? m_button_1_hold_timer : m_button_2_hold_timer;
	if (timer == nullptr)
		return;

	cocovga::button_runtime const &buttons = m_core.buttons();
	bool const pressed = first ? m_button_1 : m_button_2;
	std::uint32_t const hold_ticks =
			first ? buttons.button_1_hold_ticks : buttons.button_2_hold_ticks;
	if (pressed && hold_ticks != 0 && hold_ticks < cocovga::BUTTON_HOLD_CYCLES)
	{
		std::uint32_t const remaining = cocovga::BUTTON_HOLD_CYCLES - hold_ticks;
		timer->adjust(attotime::from_ticks(remaining, BUTTON_CLOCK), first ? 1 : 2);
	}
	else
	{
		timer->adjust(attotime::never);
	}
}

void cocovga_device::clock_buttons()
{
	cocovga::button_events const events =
			m_core.clock_buttons(m_button_1, m_button_2, m_mode.pixel(0));

	if (events.button_1_press || events.button_1_hold ||
			events.button_2_press || events.button_2_hold)
	{
		LOG_REGISTER_ACCESS(
				"%s: buttons press=%d/%d hold=%d/%d\n",
				tag(),
				events.button_1_press,
				events.button_2_press,
				events.button_1_hold,
				events.button_2_hold);
		sync_capture_control();
	}
}

void cocovga_device::button_1_w(int state)
{
	bool const pressed = state != 0;
	if (m_button_1 == pressed)
		return;

	m_button_1 = pressed;
	clock_buttons();
	schedule_button_hold(true);
}

void cocovga_device::button_2_w(int state)
{
	bool const pressed = state != 0;
	if (m_button_2 == pressed)
		return;

	m_button_2 = pressed;
	clock_buttons();
	schedule_button_hold(false);
}

TIMER_CALLBACK_MEMBER(cocovga_device::button_hold)
{
	bool const first = param == 1;
	bool const pressed = first ? m_button_1 : m_button_2;
	emu_timer *const timer = first ? m_button_1_hold_timer : m_button_2_hold_timer;
	if (!pressed)
	{
		timer->adjust(attotime::never);
		return;
	}

	cocovga::button_runtime &buttons = m_core.state().buttons;
	std::uint32_t &hold_ticks =
			first ? buttons.button_1_hold_ticks : buttons.button_2_hold_ticks;
	if (hold_ticks < cocovga::BUTTON_HOLD_CYCLES)
	{
		// The timer represents the intervening button clock cycles.
		hold_ticks = cocovga::BUTTON_HOLD_CYCLES - 1;
		clock_buttons();
	}
	timer->adjust(attotime::never);
}

void cocovga_device::vblank(screen_device &screen, bool state)
{
	if (!state || &screen != &this->screen())
		return;

	advance_vga_frame();
}

void cocovga_device::advance_vga_frame()
{
	// The screen device supplies the independent 25 MHz VGA cadence.  To avoid
	// scheduling every pixel, internal counters are advanced one line at a time
	// at VGA vblank; host capture continues independently on MC6847 callbacks.
	++m_frame_boundaries;
	preflight_output_candidate();
	m_capture.set_programming(m_core.freeze());
	std::uint64_t const pixel_clocks_before = m_capture.state().vga.pixel_clocks;

	bool frame_started = false;
	bool vertical_sync_started = false;
	bool output_changed = false;
	bool output_rejected = false;
	bool frame_repeated = false;
	bool output_held = false;
	bool timing_applied = false;

	for (unsigned line = 0; line < 2'048 && !frame_started; ++line)
	{
		cocovga::vga_timing_endpoints const endpoints = m_capture.timing_endpoints();
		if (endpoints.h_total == 0)
			break;

		// Preserve the capture engine's boundary transitions without scheduling every VGA pixel.
		cocovga::vga_capture_state &vga = m_capture.state().vga;
		std::uint16_t const last_pixel = std::uint16_t(endpoints.h_total - 1);
		if (vga.hcounter < last_pixel)
			vga.pixel_clocks += last_pixel - vga.hcounter;
		vga.hcounter = last_pixel;

		cocovga::vga_tick_result const tick = m_capture.clock_vga();
		frame_started = tick.frame_started;
		vertical_sync_started = vertical_sync_started || tick.vertical_sync_started;
		output_changed = output_changed || tick.output_changed;
		output_rejected = output_rejected || tick.output_rejected;
		frame_repeated = frame_repeated || tick.frame_repeated;
		output_held = output_held || tick.output_held;
		timing_applied = timing_applied || tick.timing_applied;
	}

	cocovga::vga_capture_state &vga = m_capture.state().vga;
	if (frame_started)
	{
		vga.hsync_asserted = false;
		vga.vsync_asserted = false;
		vga.blanking = false;
	}

	m_status_overlay.advance(vga.pixel_clocks - pixel_clocks_before);
	sync_status_overlay();

	if (vertical_sync_started && m_core.commit_palettes_at_vsync())
		LOG_REGISTER_ACCESS("%s: committed palette shadows at VGA sync\n", tag());

	cocovga::vga_timing const &active_timing = m_capture.state().vga.active_timing;
	if (timing_applied ||
			m_screen_timing_pending ||
			!m_screen_timing_valid ||
			!cocovga::vga_timing_equal(active_timing, m_applied_timing))
	{
		apply_screen_timing(active_timing);
	}

	if (!frame_started)
	{
		LOG_TIMING_EVENT("%s: VGA frame advance did not reach a frame boundary\n", tag());
	}
	else if (timing_applied)
	{
		LOG_TIMING_EVENT("%s: applied programmed timing at frame boundary\n", tag());
	}

	if (output_changed || output_rejected || frame_repeated || output_held)
	{
		LOG_CAPTURE_EVENT(
				"%s: output changed=%d rejected=%d repeated=%d held=%d page=%u valid=%d\n",
				tag(),
				output_changed,
				output_rejected,
				frame_repeated,
				output_held,
				m_capture.output_page(),
				m_capture.output_valid());
	}
}

void cocovga_device::preflight_output_candidate()
{
	std::uint8_t page = 0;
	cocovga::frame_buffer_state const *candidate = nullptr;
	if (!m_capture.output_candidate(page, candidate))
		return;

	cocovga::output_reject_reason reason = cocovga::output_reject_reason::NONE;
	cocovga::render_result const result = m_renderer.preflight(m_core, *candidate);
	if (result.status == cocovga::render_status::INCONSISTENT_CAPTURE)
		reason = cocovga::output_reject_reason::RENDER_PREFLIGHT;

	if (reason == cocovga::output_reject_reason::NONE)
	{
		m_capture.allow_output_candidate(page, candidate->generation);
		return;
	}

	if (m_capture.reject_output_candidate(page, candidate->generation, reason))
	{
		++m_rejected_render_candidates;
		// Holding the previous frame through a mode transition is the designed
		// behaviour, so this only goes to the render log channel.
		LOG_RENDER_EVENT(
				"%s: CoCoVGA rejected VGA candidate page=%u generation=%llu "
				"reason=%s render=%s modes=%u->%u changes=%u; repeating generation=%llu\n",
				tag(),
				page,
				(long long unsigned)candidate->generation,
				cocovga::capture_engine::output_reject_name(reason),
				cocovga::renderer::status_name(result.status),
				unsigned(candidate->first_mode),
				unsigned(candidate->last_mode),
				candidate->mode_changes,
				(long long unsigned)m_capture.output_generation());
	}
}

void cocovga_device::queue_screen_timing() noexcept
{
	m_screen_timing_pending = true;
}

void cocovga_device::apply_screen_timing(cocovga::vga_timing const &timing)
{
	if (!cocovga::capture_engine::valid_timing(timing))
		return;
	if (!m_screen_timing_pending &&
			m_screen_timing_valid &&
			cocovga::vga_timing_equal(timing, m_applied_timing))
	{
		return;
	}

	cocovga::vga_timing_endpoints const endpoints = cocovga::make_vga_timing_endpoints(timing);
	rectangle const visible(
			0,
			int(timing.h_active) - 1,
			0,
			int(timing.v_active) - 1);
	std::uint32_t const pixel_clock = clock() != 0 ? clock() : DEFAULT_PIXEL_CLOCK;
	std::uint64_t const frame_clocks =
			std::uint64_t(endpoints.h_total) * endpoints.v_total;

	screen().configure(
			endpoints.h_total,
			endpoints.v_total,
			visible,
			attotime::from_ticks(frame_clocks, pixel_clock));

	m_applied_timing = timing;
	m_screen_timing_valid = true;
	m_screen_timing_pending = false;

	LOG_TIMING_EVENT(
			"%s: screen %ux%u active, %ux%u total at %u Hz pixel clock\n",
			tag(),
			timing.h_active,
			timing.v_active,
			endpoints.h_total,
			endpoints.v_total,
			pixel_clock);
}

void cocovga_device::record_render_result(cocovga::render_result const &result)
{
	cocovga::render_status const previous = m_last_render_status;
	std::uint64_t const previous_generation = m_last_render_generation;
	m_last_render_status = result.status;
	m_last_render_generation = result.generation;

	if (result.status != previous || result.status != cocovga::render_status::OK)
	{
		LOG_RENDER_EVENT(
				"%s: render %s generation=%llu writes=%u lines=%u\n",
				tag(),
				cocovga::renderer::status_name(result.status),
				(long long unsigned)result.generation,
				result.pixels_written,
				result.scanlines_written);
	}

	if (!result.success() &&
			(result.status != previous || result.generation != previous_generation))
	{
		// An unusable capture or an incomplete frame is recoverable: the last
		// good bitmap is presented again.  Only a surface the renderer cannot
		// write to means the device was handed something invalid.
		if (result.status == cocovga::render_status::INVALID_SURFACE)
		{
			logerror(
					"%s: CoCoVGA render target is unusable at generation=%llu\n",
					tag(),
					(long long unsigned)result.generation);
		}
		else
		{
			LOG_RENDER_EVENT(
					"%s: CoCoVGA render rejected generation=%llu status=%s; %s\n",
					tag(),
					(long long unsigned)result.generation,
					cocovga::renderer::status_name(result.status),
					m_have_rendered_frame
							? "repeating last successful VGA bitmap"
							: "no successful VGA bitmap is available");
		}
	}
}

std::uint32_t cocovga_device::screen_update(
		screen_device &screen,
		bitmap_rgb32 &bitmap,
		rectangle const &cliprect)
{
	bitmap.fill(OPAQUE_BLACK, cliprect);
	if (&screen != &this->screen() || !vga_output_active())
		return 0;

	auto const copy_presented = [this, &bitmap, &cliprect] ()
	{
		int const left = std::max(cliprect.left(), 0);
		int const top = std::max(cliprect.top(), 0);
		int const right = std::min(
				cliprect.right(),
				std::min(bitmap.width() - 1, int(cocovga::RENDER_WIDTH) - 1));
		int const bottom = std::min(
				cliprect.bottom(),
				std::min(bitmap.height() - 1, int(cocovga::RENDER_HEIGHT) - 1));

		if (left <= right && top <= bottom)
		{
			for (int y = top; y <= bottom; ++y)
			{
				std::copy_n(
						&m_render_bitmap.pix(y, left),
						right - left + 1,
						&bitmap.pix(y, left));
			}
		}
	};

	if (!m_capture.output_valid())
	{
		cocovga::render_result unavailable;
		unavailable.status = cocovga::render_status::FRAME_UNAVAILABLE;
		unavailable.generation = m_capture.output_generation();
		record_render_result(unavailable);
		if (m_have_rendered_frame)
		{
			++m_render_repeats;
			copy_presented();
		}
		return 0;
	}

	cocovga::pixel_surface const presented
	{
		&m_render_bitmap.pix(0),
		std::size_t(m_render_bitmap.rowpixels()),
		cocovga::RENDER_WIDTH,
		cocovga::RENDER_HEIGHT
	};
	cocovga::pixel_surface const staging
	{
		&m_render_candidate_bitmap.pix(0),
		std::size_t(m_render_candidate_bitmap.rowpixels()),
		cocovga::RENDER_WIDTH,
		cocovga::RENDER_HEIGHT
	};
	m_status_overlay.set_quiet(m_core, status_input());
	cocovga::render_options options;
	options.border_status = m_status_overlay.view();
	cocovga::render_result const result = m_renderer.render_preserving(
			m_core,
			m_capture.buffer(m_capture.output_page()),
			presented,
			staging,
			options);
	record_render_result(result);
	if (!result.success())
	{
		++m_render_failures;
		if (m_have_rendered_frame)
			++m_render_repeats;
	}
	else
	{
		m_have_rendered_frame = true;
		m_last_successful_generation = result.generation;
	}

	if (!m_have_rendered_frame)
		return 0;
	copy_presented();

	return 0;
}
