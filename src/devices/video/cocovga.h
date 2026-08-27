// license:BSD-3-Clause
// copyright-holders:Stephen Spiller

#ifndef MAME_VIDEO_COCOVGA_H
#define MAME_VIDEO_COCOVGA_H

#pragma once

#include "cocovga_extended.h"

#include "screen.h"

namespace cocovga
{

enum class output_policy : std::uint8_t
{
	COMPOSITE,
	VGA,
	BOTH
};

} // namespace cocovga


// MAME-facing device wrapper for the framework-independent CoCoVGA engines.
// It owns framework resources and explicitly registers each engine's mutable
// state structure with the save-state system.
class cocovga_device : public device_t, public device_video_interface
{
public:
	static constexpr std::uint32_t DEFAULT_PIXEL_CLOCK = 25'000'000;

	cocovga_device(machine_config const &mconfig, char const *tag, device_t *owner, std::uint32_t clock);

	// Reset-time configuration sampled from input ports.
	void apply_configuration(
			cocovga::model board,
			cocovga::output_policy policy);

	bool composite_output_active() const noexcept;
	bool vga_output_active() const noexcept;

	// Live host signals.
	// These are raw MC6847 electrical pin levels, where low is asserted.  FS is
	// passed unchanged to the lock and converted to capture blanking only here.
	cocovga::capture_result capture_fetch(cocovga::host_fetch_event const &event);
	void mode_update(cocovga::host_mode_pins const &mode);
	void hsync_pin_w(int state);
	void fsync_pin_w(int state);
	void button_1_w(int state);
	void button_2_w(int state);

	std::uint32_t screen_update(screen_device &screen, bitmap_rgb32 &bitmap, rectangle const &cliprect);

	cocovga::host_mode_pins const &mode() const noexcept { return m_mode; }

protected:
	virtual void device_start() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;
	virtual void device_post_load() override ATTR_COLD;
	virtual void device_clock_changed() override;
	virtual tiny_rom_entry const *device_rom_region() const override ATTR_COLD;

private:
	static constexpr std::uint32_t BUTTON_CLOCK = 3'580'000;

	void register_core_save_state() ATTR_COLD;
	void register_capture_save_state() ATTR_COLD;
	void register_renderer_save_state() ATTR_COLD;
	void register_device_save_state() ATTR_COLD;

	void reset_runtime();
	void update_lock();
	void sync_capture_control();
	cocovga::pixel_input status_input() const noexcept;
	void sync_status_overlay();
	void schedule_button_hold(bool first);
	void clock_buttons();
	void vblank(screen_device &screen, bool state);
	void advance_vga_frame();
	void preflight_output_candidate();
	void queue_screen_timing() noexcept;
	void apply_screen_timing(cocovga::vga_timing const &timing);
	void record_render_result(cocovga::render_result const &result);

	TIMER_CALLBACK_MEMBER(button_hold);

	// CoCoVGA reimplements the VDG character generator and can select either
	// the MC6847 or the MC6847T1 bank regardless of which VDG the host machine
	// fits, so the board carries both character generator ROMs itself.  A
	// machine only needs them once this device is present.
	required_memory_region m_chargen;
	required_memory_region m_chargen_t1;

	cocovga::character_generator m_charset;
	cocovga::core m_core;
	cocovga::capture_engine m_capture;
	cocovga::renderer m_renderer;
	cocovga::render_effects::border_status_overlay m_status_overlay;
	bitmap_rgb32 m_render_bitmap;
	bitmap_rgb32 m_render_candidate_bitmap;
	emu_timer *m_button_1_hold_timer = nullptr;
	emu_timer *m_button_2_hold_timer = nullptr;

	cocovga::host_mode_pins m_mode;
	cocovga::output_policy m_output_policy = cocovga::output_policy::BOTH;
	cocovga::vga_timing m_applied_timing;
	cocovga::vga_timing m_last_requested_timing;
	cocovga::render_status m_last_render_status = cocovga::render_status::FRAME_UNAVAILABLE;
	cocovga::capture_reject_reason m_last_capture_reject = cocovga::capture_reject_reason::NONE;

	std::uint64_t m_frame_boundaries = 0;
	std::uint64_t m_capture_fetches = 0;
	std::uint64_t m_register_writes = 0;
	std::uint64_t m_rejected_timing_requests = 0;
	std::uint64_t m_rejected_render_candidates = 0;
	std::uint64_t m_render_failures = 0;
	std::uint64_t m_render_repeats = 0;
	std::uint64_t m_last_render_generation = 0;
	std::uint64_t m_last_successful_generation = 0;

	bool m_hsync_pin = false;
	bool m_fsync_pin = false;
	bool m_button_1 = false;
	bool m_button_2 = false;
	bool m_screen_timing_valid = false;
	bool m_screen_timing_pending = false;
	bool m_timing_request_seen = false;
	bool m_last_timing_request_valid = true;
	bool m_have_rendered_frame = false;
};


DECLARE_DEVICE_TYPE(COCOVGA, cocovga_device)

#endif // MAME_VIDEO_COCOVGA_H
