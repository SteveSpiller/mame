// license:BSD-3-Clause
// copyright-holders:Stephen Spiller

#ifndef MAME_VIDEO_COCOVGA_CORE_H
#define MAME_VIDEO_COCOVGA_CORE_H

#pragma once

#include "coretmpl.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace cocovga
{

// The core, capture and rendering engines are independent of the emulation
// framework.  Their mutable data is kept in fixed-width, trivially copyable
// state structures that form the save-state boundary for the device wrapper.

constexpr std::size_t SEMIGRAPHICS_PALETTE_ENTRIES = 11;
constexpr std::size_t ARTIFACT_PALETTE_ENTRIES = 32;
constexpr std::size_t EXTRA_PALETTE_ENTRIES = 4;
constexpr std::size_t CHARACTER_RAM_BYTES = 3'072;
constexpr std::uint16_t PAGE00_REGISTER_BYTES = 512;
constexpr std::uint16_t INVALID_CAPTURE_ADDRESS = 0xffff;
constexpr std::uint32_t BUTTON_HOLD_CYCLES = 3'580'000;
constexpr std::uint16_t VGA_HORIZONTAL_TOTAL_MAX = 2'048;
constexpr std::uint16_t VGA_VERTICAL_TOTAL_MAX = 1'023;

enum class model : std::uint8_t
{
	AMC2 = 0,
	AMC3_F1_KMC1 = 1,
	MODERN = AMC3_F1_KMC1,
	T1 = 2
};

enum class register_bank : std::uint8_t
{
	FONT = 0,
	ARTIFACT,
	EXTRAS,
	SEMIGRAPHICS_PALETTE,
	ARTIFACT_PALETTE,
	EXTRA_PALETTE,
	VGA_TIMING,
	ENHANCED_MODES
};

constexpr unsigned REGISTER_BANK_COUNT = unsigned(register_bank::ENHANCED_MODES) + 1;

enum class register_page : std::uint8_t
{
	PAGE_00 = 0x00,
	PAGE_18 = 0x0c, // packed selector for PIA bits 7-3 value 0x18
	UNKNOWN = 0xff
};

enum class combo_state : std::uint8_t
{
	RESET,
	INIT,
	READY,
	UNLOCKED_1,
	UNLOCKED_2,
	UNLOCKED_3,
	WAIT_MODE_1,
	WAIT_MODE_2,
	WAIT_MODE_3,
	UNLOCKED_A,
	UNLOCKED_B
};

enum class border_mode : std::uint8_t
{
	STANDARD,
	T1,
	BLACK,
	CUSTOM
};

enum class artifact_mode : std::uint8_t
{
	STANDARD,
	FAT_BITS,
	SMARTIFACT,
	MESS,
	MONOCHROME
};

enum class text_palette_choice : std::uint8_t
{
	SOFTWARE,
	CSS_GREEN_OR_ORANGE,
	BLACK_ON_RED,
	WHITE_ON_RED,
	BLACK_ON_YELLOW,
	BLACK_ON_BLUE,
	BLUE_ON_WHITE,
	BLACK_ON_WHITE
};

enum class hardware_artifact_choice : std::uint8_t
{
	SOFTWARE,
	STANDARD,
	FAT_BITS,
	SMARTIFACT,
	MESS,
	MONOCHROME
};

enum class mode_kind : std::uint8_t
{
	ALPHA_SEMIGRAPHICS,
	CG1,
	RG1,
	CG2,
	RG2,
	CG3,
	RG3,
	CG6,
	RG6,
	VG6,
	W64
};

enum class mode_family : std::uint8_t
{
	ALPHA_SEMIGRAPHICS,
	COLOR_GRAPHICS,
	RESOLUTION_GRAPHICS,
	ENHANCED_GRAPHICS,
	ENHANCED_TEXT
};

enum class semigraphics_glyph : std::uint8_t
{
	SG4,
	SG6
};

enum class capture_cycle : std::uint8_t
{
	ALPHA,
	LONG_CYCLE,
	SHORT_CYCLE
};

enum class palette_source : std::uint8_t
{
	SEMIGRAPHICS,
	EXTRA
};

enum class extra_palette_slot : std::uint8_t
{
	TEXT_FOREGROUND,
	TEXT_BACKGROUND,
	BORDER,
	SCANLINE
};

struct rgb_color
{
	std::uint8_t red = 0;
	std::uint8_t green = 0;
	std::uint8_t blue = 0;
};

struct decoded_palette_word
{
	bool enabled = false;
	rgb_color color;
	std::uint16_t rgb15 = 0;
	std::uint16_t rgb9 = 0;
};

struct palette_output
{
	bool overridden = false;
	rgb_color color;
	std::uint16_t rgb15 = 0;
	std::uint16_t rgb9 = 0;
	std::uint16_t packed = 0;
	std::uint8_t bits_per_component = 0;
};

struct palette_reference
{
	palette_source source = palette_source::SEMIGRAPHICS;
	std::uint8_t index = 0;
	// Hardware button palettes use the built-in color even if software overrides the slot.
	bool use_default = false;
};

struct text_palette_descriptor
{
	palette_reference foreground;
	palette_reference background;
};

struct font_controls
{
	bool t1_font = false;
	bool force_lowercase = false;
	bool force_character_ram = false;
};

struct artifact_controls
{
	bool color_enabled = false;
	bool swap = false;
	artifact_mode mode = artifact_mode::MONOCHROME;
};

struct extras_controls
{
	border_mode border = border_mode::STANDARD;
	bool scanlines = false;
	bool quiet_status = false;
	bool lowercase = false;
	bool inverse_text = false;
	bool full_semigraphics_palette = false;
	bool custom_text_palette = false;
};

struct enhanced_controls
{
	bool vg6 = false;
	bool w64 = false;
	bool character_ram = false;
};

struct effective_controls
{
	font_controls font;
	artifact_controls artifact;
	extras_controls extras;
	enhanced_controls enhanced;
	text_palette_choice text_palette = text_palette_choice::SOFTWARE;
};

struct vga_timing
{
	std::uint16_t h_active = 640;
	std::uint8_t h_front_porch = 7;
	std::uint8_t h_sync_width = 98;
	std::uint8_t h_back_porch = 55;
	std::uint16_t v_active = 480;
	std::uint8_t v_front_porch = 24;
	std::uint8_t v_sync_width = 2;
	std::uint8_t v_back_porch = 19;
};

struct vga_timing_endpoints
{
	std::uint16_t h_active_end = 640;
	std::uint16_t h_front_end = 647;
	std::uint16_t h_sync_end = 745;
	std::uint16_t h_total = 800;
	std::uint16_t v_active_end = 480;
	std::uint16_t v_front_end = 504;
	std::uint16_t v_sync_end = 506;
	std::uint16_t v_total = 525;
};

constexpr bool vga_timing_equal(vga_timing const &left, vga_timing const &right) noexcept
{
	return left.h_active == right.h_active &&
			left.h_front_porch == right.h_front_porch &&
			left.h_sync_width == right.h_sync_width &&
			left.h_back_porch == right.h_back_porch &&
			left.v_active == right.v_active &&
			left.v_front_porch == right.v_front_porch &&
			left.v_sync_width == right.v_sync_width &&
			left.v_back_porch == right.v_back_porch;
}

constexpr vga_timing_endpoints make_vga_timing_endpoints(vga_timing const &timing) noexcept
{
	vga_timing_endpoints result;
	result.h_active_end = timing.h_active;
	result.h_front_end = std::uint16_t(result.h_active_end + timing.h_front_porch);
	result.h_sync_end = std::uint16_t(result.h_front_end + timing.h_sync_width);
	result.h_total = std::uint16_t(result.h_sync_end + timing.h_back_porch);
	result.v_active_end = timing.v_active;
	result.v_front_end = std::uint16_t(result.v_active_end + timing.v_front_porch);
	result.v_sync_end = std::uint16_t(result.v_front_end + timing.v_sync_width);
	result.v_total = std::uint16_t(result.v_sync_end + timing.v_back_porch);
	return result;
}

struct register_file
{
	std::uint8_t font = 0;
	std::uint8_t artifact = 0;
	std::uint8_t extras = 0;
	std::uint8_t enhanced_modes = 0;
	std::array<std::uint16_t, SEMIGRAPHICS_PALETTE_ENTRIES> semigraphics_palette_shadow{};
	std::array<std::uint16_t, ARTIFACT_PALETTE_ENTRIES> artifact_palette_shadow{};
	std::array<std::uint16_t, EXTRA_PALETTE_ENTRIES> extra_palette_shadow{};
	std::array<std::uint16_t, SEMIGRAPHICS_PALETTE_ENTRIES> semigraphics_palette_visible{};
	std::array<std::uint16_t, ARTIFACT_PALETTE_ENTRIES> artifact_palette_visible{};
	std::array<std::uint16_t, EXTRA_PALETTE_ENTRIES> extra_palette_visible{};
	vga_timing timing;
};

struct pixel_input
{
	std::uint8_t data = 0;
	std::uint8_t gm = 0;
	bool graphics = false;
	bool css = false;
	bool alpha_semigraphics = false;
	bool inverse = false;
	bool internal_external = false;
};

struct mode_descriptor
{
	mode_kind mode = mode_kind::ALPHA_SEMIGRAPHICS;
	mode_family family = mode_family::ALPHA_SEMIGRAPHICS;
	std::uint16_t source_width = 0;
	std::uint16_t source_height = 0;
	std::uint16_t logical_bytes = 0;
	std::uint8_t bits_per_pixel = 0;
	std::uint8_t captured_data_bits = 0;
	std::uint8_t bytes_per_row = 0;
	capture_cycle capture = capture_cycle::ALPHA;
	std::uint8_t scale_x = 1;
	std::uint8_t scale_y = 1;
	std::uint8_t text_columns = 0;
	std::uint8_t text_rows = 0;
	std::uint8_t glyph_width = 0;
	std::uint8_t glyph_height = 0;
	bool artifact_capable = false;
	std::uint8_t palette_entries = 0;
	std::array<std::uint8_t, 16> css0_palette{};
	std::array<std::uint8_t, 16> css1_palette{};
};

struct model_descriptor
{
	model board = model::MODERN;
	std::uint8_t dac_bits_per_component = 5;
	std::uint8_t default_font = 0;
	std::uint8_t default_artifact = 0x0d;
	std::uint8_t default_extras = 0x40;
	std::uint8_t default_enhanced_modes = 0;
};

struct combo_step
{
	combo_state previous = combo_state::INIT;
	combo_state current = combo_state::INIT;
	bool page_captured = false;
	std::uint8_t captured_selector = 0;
	bool programming_released = false;
	bool freeze = false;
};

struct button_events
{
	bool button_1_press = false;
	bool button_1_hold = false;
	bool button_2_press = false;
	bool button_2_hold = false;
};

struct button_runtime
{
	std::uint32_t button_1_hold_ticks = 0;
	std::uint32_t button_2_hold_ticks = 0;
	bool button_1_down = false;
	bool button_2_down = false;
	std::uint8_t text_button_1_cycle = 0;
	std::uint8_t graphics_button_1_cycle = 0;
	std::uint8_t rg6_button_1_cycle = 0;
	std::uint8_t text_button_2_cycle = 0;
	std::uint8_t rg6_button_2_cycle = 0;
	bool force_lowercase_toggle = false;
	bool quiet_status_toggle = false;
	bool w64_toggle = false;
};

struct hardware_controls
{
	bool inverse_text = false;
	bool reduced_palette = false;
	bool scanlines = false;
	bool force_lowercase = false;
	bool disable_border = false;
	bool artifact_swap = false;
	bool quiet_status = false;
	bool w64 = false;
	text_palette_choice text_palette = text_palette_choice::SOFTWARE;
	hardware_artifact_choice artifact = hardware_artifact_choice::SOFTWARE;
};

struct core_state
{
	model board = model::MODERN;
	register_file registers;
	std::array<std::uint8_t, CHARACTER_RAM_BYTES> character_ram{};
	std::uint8_t reset_mask = 0;
	std::uint8_t edit_mask = 0;
	std::uint8_t selected_page = 0;
	bool page_selected = false;
	combo_state lock_state = combo_state::INIT;
	bool character_write_enabled = false;
	button_runtime buttons;
	bool vg6_active = false;
	bool w64_active = false;
};

class core
{
public:
	static constexpr std::uint8_t COMBO_1 = 0x09;
	static constexpr std::uint8_t COMBO_2 = 0x14;
	static constexpr std::uint8_t COMBO_3 = 0x0a;
	static constexpr std::uint8_t COMBO_4 = 0x1f;

	explicit core(model board = model::MODERN) noexcept;

	// Lifetime and mutable state.
	void reset() noexcept;
	void set_model(model board) noexcept;
	model board_model() const noexcept { return m_state.board; }

	core_state &state() noexcept { return m_state; }
	core_state const &state() const noexcept { return m_state; }
	void restore_state(core_state const &saved) noexcept { m_state = saved; }

	register_file const &registers() const noexcept { return m_state.registers; }
	std::array<std::uint8_t, CHARACTER_RAM_BYTES> const &character_ram() const noexcept { return m_state.character_ram; }
	button_runtime const &buttons() const noexcept { return m_state.buttons; }

	std::uint8_t reset_mask() const noexcept { return m_state.reset_mask; }
	std::uint8_t edit_mask() const noexcept { return m_state.edit_mask; }
	std::uint8_t selected_page() const noexcept { return m_state.selected_page; }
	bool page_selected() const noexcept { return m_state.page_selected; }
	register_page selected_register_page() const noexcept;
	combo_state lock_state() const noexcept { return m_state.lock_state; }
	bool freeze() const noexcept;
	bool character_write_enabled() const noexcept { return m_state.character_write_enabled; }

	// Static hardware descriptions and decoding.
	static constexpr std::uint8_t bank_bit(register_bank bank) noexcept
	{
		return std::uint8_t(1U << static_cast<unsigned>(bank));
	}

	static model_descriptor const &describe_model(model board) noexcept;
	static mode_descriptor const &describe_mode(mode_kind mode) noexcept;
	static mode_kind decode_standard_mode(pixel_input const &input) noexcept;
	mode_kind display_mode(pixel_input const &input) const noexcept;
	mode_kind capture_mode(pixel_input const &input) const noexcept;

	// Palette decoding and effective controls.
	static decoded_palette_word decode_palette_word(std::uint16_t word) noexcept;
	static std::uint16_t pack_rgb15(rgb_color color) noexcept;
	static std::uint16_t pack_rgb9(rgb_color color) noexcept;
	static std::uint8_t semigraphics_palette_index(std::uint8_t slot, bool full_palette) noexcept;
	static std::uint8_t semigraphics_glyph_palette_index(
			semigraphics_glyph glyph,
			bool css,
			std::uint8_t color_code) noexcept;
	static std::uint8_t color_graphics_palette_index(bool css, std::uint8_t pixel) noexcept;
	static std::uint8_t resolution_graphics_palette_index(bool css, bool pixel) noexcept;
	static std::uint8_t vg6_palette_index(bool css, std::uint8_t pixel) noexcept;

	palette_output semigraphics_color(std::size_t slot, bool full_palette = true) const noexcept;
	palette_output artifact_color(std::size_t slot) const noexcept;
	palette_output extra_color(extra_palette_slot slot) const noexcept;
	palette_output palette_color(palette_reference reference) const noexcept;
	text_palette_descriptor text_palette(pixel_input const &input) const noexcept;

	font_controls software_font() const noexcept;
	artifact_controls software_artifact() const noexcept;
	extras_controls software_extras() const noexcept;
	enhanced_controls software_enhanced() const noexcept;
	hardware_controls hardware_settings(pixel_input const &input) const noexcept;
	effective_controls controls(pixel_input const &input) const noexcept;
	bool use_character_ram(pixel_input const &input) const noexcept;

	vga_timing_endpoints timing_endpoints() const noexcept;

	// Register and character-memory address translation.
	static constexpr std::uint16_t page00_logical_to_internal(std::uint16_t logical_offset) noexcept
	{
		return logical_offset >= PAGE00_REGISTER_BYTES
				? INVALID_CAPTURE_ADDRESS
				: logical_offset < 32
						? std::uint16_t(32 + logical_offset)
						: std::uint16_t((logical_offset / 32) * 384 + (logical_offset % 32));
	}

	static bool page00_internal_to_logical(std::uint16_t internal_address, std::uint16_t &logical_offset) noexcept;
	static constexpr std::uint16_t character_capture_to_address(std::uint16_t capture_address) noexcept
	{
		return capture_address >= CHARACTER_RAM_BYTES * 2
				? INVALID_CAPTURE_ADDRESS
				: std::uint16_t((util::BIT(capture_address, 6, 10) << 5) | util::BIT(capture_address, 0, 5));
	}

	static constexpr std::uint16_t character_address(std::uint8_t character, std::uint8_t row) noexcept
	{
		return row >= 12
				? INVALID_CAPTURE_ADDRESS
				: std::uint16_t(std::uint16_t(character) * 12 + row);
	}

	static constexpr std::uint8_t pack_alpha_capture_byte(bool alpha_semigraphics, bool inverse, std::uint8_t data) noexcept
	{
		return std::uint8_t((alpha_semigraphics ? 0x80 : 0x00) | (inverse ? 0x40 : 0x00) | util::BIT(data, 0, 6));
	}

	static pixel_input decode_alpha_capture_byte(
			std::uint8_t captured,
			bool css = false,
			bool internal_external = false) noexcept;

	// Programming and live button inputs.
	static constexpr std::uint8_t selector_from_pia_bits(std::uint8_t pia_bits_7_3) noexcept
	{
		return std::uint8_t((util::BIT(pia_bits_7_3, 0) << 4) | (util::BIT(pia_bits_7_3, 4) << 3) | util::BIT(pia_bits_7_3, 1, 3));
	}

	static constexpr std::uint8_t selector_from_pia_register(std::uint8_t pia1b) noexcept
	{
		return selector_from_pia_bits(util::BIT(pia1b, 3, 5));
	}

	bool write_page00(std::uint16_t logical_offset, std::uint8_t data) noexcept;
	bool write_page00_internal(std::uint16_t internal_address, std::uint8_t data) noexcept;
	bool write_character(std::uint16_t logical_offset, std::uint8_t data) noexcept;
	bool write_character_capture(std::uint16_t capture_address, std::uint8_t data) noexcept;
	bool ingest_selected_capture(std::uint16_t capture_address, std::uint8_t data) noexcept;

	void clear_character_ram(std::uint8_t value = 0) noexcept;
	void end_programming_frame() noexcept;
	bool commit_palettes_at_vsync() noexcept;

	// field_sync_pin_high is the raw active-low MC6847 FS pin level; selector is
	// packed as { CSS, A/G, GM[2:0] }.
	combo_step clock_combo(
			bool field_sync_pin_high,
			std::uint8_t selector,
			bool reset_asserted = false,
			bool reset_complete = true) noexcept;

	button_events clock_buttons(bool button_1, bool button_2, pixel_input const &input) noexcept;
	void latch_previous_field_mode(bool previous_field_graphics, std::uint8_t previous_field_gm) noexcept;

private:
	enum class button_context : std::uint8_t
	{
		TEXT,
		GRAPHICS,
		RG6
	};

	button_context context_for(pixel_input const &input) const noexcept;
	palette_output palette_value(std::uint16_t word, std::uint16_t default_rgb15) const noexcept;
	void reset_bank(register_bank bank, bool reset_visible_palette) noexcept;
	void reset_button_controls(register_bank bank) noexcept;
	void apply_button_events(button_events const &events, button_context context) noexcept;
	void write_palette_byte(std::uint16_t &word, bool high_byte, std::uint8_t data) noexcept;

	core_state m_state;
};

static_assert(std::is_trivially_copyable<core_state>::value);
static_assert(std::is_standard_layout<core_state>::value);
static_assert(sizeof(core_state) <= 8 * 1'024);

} // namespace cocovga

#endif // MAME_VIDEO_COCOVGA_CORE_H
