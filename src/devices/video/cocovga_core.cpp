// license:BSD-3-Clause
// copyright-holders:Stephen Spiller

#include "emu.h"

#include "cocovga_core.h"

namespace cocovga
{

namespace
{

using palette_map = std::array<std::uint8_t, 16>;

constexpr palette_map EMPTY_PALETTE = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
constexpr palette_map CG_CSS0_PALETTE = { 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
constexpr palette_map CG_CSS1_PALETTE = { 5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
constexpr palette_map RG_CSS0_PALETTE = { 16, 31, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
constexpr palette_map RG_CSS1_PALETTE = { 0, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
constexpr palette_map VG6_CSS0_PALETTE = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
constexpr palette_map VG6_CSS1_PALETTE = { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };

constexpr std::array<std::uint16_t, SEMIGRAPHICS_PALETTE_ENTRIES> SEMIGRAPHICS_DEFAULTS =
{
	0x0000, 0x03e0, 0x7fe8, 0x0018, 0x5000, 0x7fff, 0x03f8, 0x6094, 0x7e00, 0x0100, 0x2080
};

constexpr std::array<std::uint16_t, ARTIFACT_PALETTE_ENTRIES> ARTIFACT_DEFAULTS =
{
	0x0000, 0x1004, 0x0080, 0x731c, 0x6398, 0x0094, 0x3080, 0x321c,
	0x7308, 0x7108, 0x021c, 0x639c, 0x738c, 0x0004, 0x1000, 0x7fff,
	0x0100, 0x0100, 0x0100, 0x0300, 0x0380, 0x0100, 0x0100, 0x0200,
	0x0280, 0x0300, 0x0200, 0x0380, 0x0380, 0x0100, 0x0100, 0x03e0
};

constexpr std::array<std::uint16_t, EXTRA_PALETTE_ENTRIES> EXTRA_DEFAULTS =
{
	0x0000, 0x03e0, 0x0000, 0x0000
};

constexpr std::array<model_descriptor, 3> MODEL_DESCRIPTORS =
{{
	{ model::AMC2, 3, 0x00, 0x0d, 0x40, 0x00 },
	{ model::AMC3_F1_KMC1, 5, 0x00, 0x0d, 0x40, 0x00 },
	{ model::T1, 5, 0x01, 0x0d, 0x41, 0x00 }
}};

constexpr std::array<mode_descriptor, 11> MODE_DESCRIPTORS =
{{
	{
		mode_kind::ALPHA_SEMIGRAPHICS, mode_family::ALPHA_SEMIGRAPHICS,
		256, 192, 512, 0, 6, 32, capture_cycle::ALPHA, 2, 2,
		32, 16, 8, 12, false, 0, EMPTY_PALETTE, EMPTY_PALETTE
	},
	{
		mode_kind::CG1, mode_family::COLOR_GRAPHICS,
		64, 64, 1024, 2, 8, 16, capture_cycle::LONG_CYCLE, 8, 6,
		0, 0, 0, 0, false, 4, CG_CSS0_PALETTE, CG_CSS1_PALETTE
	},
	{
		mode_kind::RG1, mode_family::RESOLUTION_GRAPHICS,
		128, 64, 1024, 1, 8, 16, capture_cycle::LONG_CYCLE, 4, 6,
		0, 0, 0, 0, false, 2, RG_CSS0_PALETTE, RG_CSS1_PALETTE
	},
	{
		mode_kind::CG2, mode_family::COLOR_GRAPHICS,
		128, 64, 2048, 2, 8, 32, capture_cycle::SHORT_CYCLE, 4, 6,
		0, 0, 0, 0, false, 4, CG_CSS0_PALETTE, CG_CSS1_PALETTE
	},
	{
		mode_kind::RG2, mode_family::RESOLUTION_GRAPHICS,
		128, 96, 1536, 1, 8, 16, capture_cycle::LONG_CYCLE, 4, 4,
		0, 0, 0, 0, false, 2, RG_CSS0_PALETTE, RG_CSS1_PALETTE
	},
	{
		mode_kind::CG3, mode_family::COLOR_GRAPHICS,
		128, 96, 3072, 2, 8, 32, capture_cycle::SHORT_CYCLE, 4, 4,
		0, 0, 0, 0, false, 4, CG_CSS0_PALETTE, CG_CSS1_PALETTE
	},
	{
		mode_kind::RG3, mode_family::RESOLUTION_GRAPHICS,
		128, 192, 3072, 1, 8, 16, capture_cycle::LONG_CYCLE, 4, 2,
		0, 0, 0, 0, false, 2, RG_CSS0_PALETTE, RG_CSS1_PALETTE
	},
	{
		mode_kind::CG6, mode_family::COLOR_GRAPHICS,
		128, 192, 6144, 2, 8, 32, capture_cycle::SHORT_CYCLE, 4, 2,
		0, 0, 0, 0, false, 4, CG_CSS0_PALETTE, CG_CSS1_PALETTE
	},
	{
		mode_kind::RG6, mode_family::RESOLUTION_GRAPHICS,
		256, 192, 6144, 1, 8, 32, capture_cycle::SHORT_CYCLE, 2, 2,
		0, 0, 0, 0, true, 2, RG_CSS0_PALETTE, RG_CSS1_PALETTE
	},
	{
		mode_kind::VG6, mode_family::ENHANCED_GRAPHICS,
		128, 96, 6144, 4, 8, 64, capture_cycle::SHORT_CYCLE, 4, 4,
		0, 0, 0, 0, false, 16, VG6_CSS0_PALETTE, VG6_CSS1_PALETTE
	},
	{
		mode_kind::W64, mode_family::ENHANCED_TEXT,
		512, 384, 2048, 0, 6, 64, capture_cycle::SHORT_CYCLE, 1, 1,
		64, 32, 8, 12, false, 0, EMPTY_PALETTE, EMPTY_PALETTE
	}
}};

} // anonymous namespace

core::core(model board) noexcept
{
	m_state.board = board;
	reset();
}

void core::reset() noexcept
{
	model const board = m_state.board;
	// Uploaded character data has no hardware reset initialization contract.
	auto const character_ram = m_state.character_ram;
	m_state = core_state{};
	m_state.board = board;
	m_state.character_ram = character_ram;

	for (unsigned bank = 0; bank < REGISTER_BANK_COUNT; ++bank)
		reset_bank(static_cast<register_bank>(bank), true);

	m_state.lock_state = combo_state::INIT;
}

void core::set_model(model board) noexcept
{
	m_state.board = board;
	reset();
}

register_page core::selected_register_page() const noexcept
{
	if (!m_state.page_selected)
		return register_page::UNKNOWN;

	switch (m_state.selected_page)
	{
	case static_cast<std::uint8_t>(register_page::PAGE_00):
		return register_page::PAGE_00;

	case static_cast<std::uint8_t>(register_page::PAGE_18):
		return register_page::PAGE_18;

	default:
		return register_page::UNKNOWN;
	}
}

bool core::freeze() const noexcept
{
	return m_state.lock_state == combo_state::UNLOCKED_A || m_state.lock_state == combo_state::UNLOCKED_B;
}

model_descriptor const &core::describe_model(model board) noexcept
{
	switch (board)
	{
	case model::AMC2:
		return MODEL_DESCRIPTORS[0];

	case model::T1:
		return MODEL_DESCRIPTORS[2];

	case model::MODERN:
	default:
		return MODEL_DESCRIPTORS[1];
	}
}

mode_descriptor const &core::describe_mode(mode_kind mode) noexcept
{
	std::size_t const index = static_cast<std::size_t>(mode);
	return MODE_DESCRIPTORS[index < MODE_DESCRIPTORS.size() ? index : 0];
}

mode_kind core::decode_standard_mode(pixel_input const &input) noexcept
{
	if (!input.graphics)
		return mode_kind::ALPHA_SEMIGRAPHICS;

	switch (BIT(input.gm, 0, 3))
	{
	case 0:
		return mode_kind::CG1;
	case 1:
		return mode_kind::RG1;
	case 2:
		return mode_kind::CG2;
	case 3:
		return mode_kind::RG2;
	case 4:
		return mode_kind::CG3;
	case 5:
		return mode_kind::RG3;
	case 6:
		return mode_kind::CG6;
	case 7:
	default:
		return mode_kind::RG6;
	}
}

mode_kind core::display_mode(pixel_input const &input) const noexcept
{
	if (m_state.vg6_active)
		return mode_kind::VG6;
	if (m_state.w64_active)
		return mode_kind::W64;
	return decode_standard_mode(input);
}

mode_kind core::capture_mode(pixel_input const &input) const noexcept
{
	if (m_state.vg6_active)
		return mode_kind::VG6;
	if (m_state.w64_active && !freeze())
		return mode_kind::W64;
	return decode_standard_mode(input);
}

std::uint16_t core::pack_rgb15(rgb_color color) noexcept
{
	return std::uint16_t(
			(std::uint16_t(BIT(color.red, 0, 5)) << 10) |
			(std::uint16_t(BIT(color.green, 0, 5)) << 5) |
			std::uint16_t(BIT(color.blue, 0, 5)));
}

std::uint16_t core::pack_rgb9(rgb_color color) noexcept
{
	return std::uint16_t(
			(std::uint16_t(BIT(color.red, 2, 3)) << 6) |
			(std::uint16_t(BIT(color.green, 2, 3)) << 3) |
			std::uint16_t(BIT(color.blue, 2, 3)));
}

decoded_palette_word core::decode_palette_word(std::uint16_t word) noexcept
{
	decoded_palette_word result;
	result.enabled = BIT(word, 15);
	result.rgb15 = BIT(word, 0, 15);
	result.color.red = std::uint8_t(BIT(result.rgb15, 10, 5));
	result.color.green = std::uint8_t(BIT(result.rgb15, 5, 5));
	result.color.blue = std::uint8_t(BIT(result.rgb15, 0, 5));
	result.rgb9 = pack_rgb9(result.color);
	return result;
}

std::uint8_t core::semigraphics_palette_index(std::uint8_t slot, bool full_palette) noexcept
{
	if (slot >= SEMIGRAPHICS_PALETTE_ENTRIES)
		return 0;
	return !full_palette && slot >= 9 ? 0 : slot;
}

std::uint8_t core::semigraphics_glyph_palette_index(
		semigraphics_glyph glyph,
		bool css,
		std::uint8_t color_code) noexcept
{
	return glyph == semigraphics_glyph::SG6
			? std::uint8_t((css ? 5 : 1) + BIT(color_code, 0, 2))
			: std::uint8_t(1 + BIT(color_code, 0, 3));
}

std::uint8_t core::color_graphics_palette_index(bool css, std::uint8_t pixel) noexcept
{
	return std::uint8_t((css ? 5 : 1) + BIT(pixel, 0, 2));
}

std::uint8_t core::resolution_graphics_palette_index(bool css, bool pixel) noexcept
{
	if (css)
		return pixel ? 15 : 0;
	return pixel ? 31 : 16;
}

std::uint8_t core::vg6_palette_index(bool css, std::uint8_t pixel) noexcept
{
	return std::uint8_t((css ? 16 : 0) + BIT(pixel, 0, 4));
}

palette_output core::palette_value(std::uint16_t word, std::uint16_t default_rgb15) const noexcept
{
	decoded_palette_word const decoded = decode_palette_word(word);
	std::uint16_t const rgb15 = decoded.enabled ? decoded.rgb15 : default_rgb15;
	decoded_palette_word const effective = decode_palette_word(rgb15);
	model_descriptor const &board = describe_model(m_state.board);

	palette_output result;
	result.overridden = decoded.enabled;
	result.color = effective.color;
	result.rgb15 = effective.rgb15;
	result.rgb9 = effective.rgb9;
	result.bits_per_component = board.dac_bits_per_component;
	result.packed = board.dac_bits_per_component == 3 ? result.rgb9 : result.rgb15;
	return result;
}

palette_output core::semigraphics_color(std::size_t slot, bool full_palette) const noexcept
{
	if (slot >= SEMIGRAPHICS_PALETTE_ENTRIES)
		return palette_value(0, 0);

	std::uint8_t const selected = semigraphics_palette_index(std::uint8_t(slot), full_palette);
	return palette_value(
			m_state.registers.semigraphics_palette_visible[selected],
			SEMIGRAPHICS_DEFAULTS[selected]);
}

palette_output core::artifact_color(std::size_t slot) const noexcept
{
	if (slot >= ARTIFACT_PALETTE_ENTRIES)
		return palette_value(0, 0);
	return palette_value(m_state.registers.artifact_palette_visible[slot], ARTIFACT_DEFAULTS[slot]);
}

palette_output core::extra_color(extra_palette_slot slot) const noexcept
{
	std::size_t const index = static_cast<std::size_t>(slot);
	if (index >= EXTRA_PALETTE_ENTRIES)
		return palette_value(0, 0);
	return palette_value(m_state.registers.extra_palette_visible[index], EXTRA_DEFAULTS[index]);
}

palette_output core::palette_color(palette_reference reference) const noexcept
{
	switch (reference.source)
	{
	case palette_source::SEMIGRAPHICS:
		if (reference.index >= SEMIGRAPHICS_PALETTE_ENTRIES)
			return palette_value(0, 0);
		return reference.use_default
				? palette_value(0, SEMIGRAPHICS_DEFAULTS[reference.index])
				: semigraphics_color(reference.index);

	case palette_source::EXTRA:
		if (reference.index >= EXTRA_PALETTE_ENTRIES)
			return palette_value(0, 0);
		return reference.use_default
				? palette_value(0, EXTRA_DEFAULTS[reference.index])
				: extra_color(static_cast<extra_palette_slot>(reference.index));
	}

	return palette_value(0, 0);
}

font_controls core::software_font() const noexcept
{
	font_controls result;
	result.t1_font = BIT(m_state.registers.font, 0);
	result.force_lowercase = BIT(m_state.registers.font, 1);
	result.force_character_ram = BIT(m_state.registers.font, 2);
	return result;
}

artifact_controls core::software_artifact() const noexcept
{
	artifact_controls result;
	result.color_enabled = BIT(m_state.registers.artifact, 0);
	result.swap = BIT(m_state.registers.artifact, 1);
	result.mode = result.color_enabled
			? static_cast<artifact_mode>(BIT(m_state.registers.artifact, 2, 2))
			: artifact_mode::MONOCHROME;
	return result;
}

extras_controls core::software_extras() const noexcept
{
	extras_controls result;
	result.border = static_cast<border_mode>(BIT(m_state.registers.extras, 0, 2));
	result.scanlines = BIT(m_state.registers.extras, 2);
	result.quiet_status = BIT(m_state.registers.extras, 3);
	result.lowercase = BIT(m_state.registers.extras, 4);
	result.inverse_text = BIT(m_state.registers.extras, 5);
	result.full_semigraphics_palette = BIT(m_state.registers.extras, 6);
	result.custom_text_palette = BIT(m_state.registers.extras, 7);
	return result;
}

enhanced_controls core::software_enhanced() const noexcept
{
	enhanced_controls result;
	result.vg6 = BIT(m_state.registers.enhanced_modes, 0);
	result.w64 = BIT(m_state.registers.enhanced_modes, 1);
	result.character_ram = BIT(m_state.registers.enhanced_modes, 2);
	return result;
}

core::button_context core::context_for(pixel_input const &input) const noexcept
{
	if (!input.graphics)
		return button_context::TEXT;
	return BIT(input.gm, 0, 3) == 7 ? button_context::RG6 : button_context::GRAPHICS;
}

hardware_controls core::hardware_settings(pixel_input const &input) const noexcept
{
	hardware_controls result;
	button_runtime const &buttons = m_state.buttons;

	switch (context_for(input))
	{
	case button_context::TEXT:
		result.inverse_text = BIT(buttons.text_button_1_cycle, 0);
		result.reduced_palette = BIT(buttons.text_button_1_cycle, 1);
		result.scanlines = BIT(buttons.text_button_1_cycle, 2);
		result.text_palette = static_cast<text_palette_choice>(BIT(buttons.text_button_2_cycle, 0, 3));
		break;

	case button_context::GRAPHICS:
		result.disable_border = BIT(buttons.graphics_button_1_cycle, 0);
		result.scanlines = BIT(buttons.graphics_button_1_cycle, 1);
		break;

	case button_context::RG6:
		result.artifact_swap = BIT(buttons.rg6_button_1_cycle, 0);
		result.disable_border = BIT(buttons.rg6_button_1_cycle, 1);
		result.scanlines = BIT(buttons.rg6_button_1_cycle, 2);
		switch (buttons.rg6_button_2_cycle)
		{
		case static_cast<std::uint8_t>(hardware_artifact_choice::STANDARD):
			result.artifact = hardware_artifact_choice::STANDARD;
			break;
		case static_cast<std::uint8_t>(hardware_artifact_choice::FAT_BITS):
			result.artifact = hardware_artifact_choice::FAT_BITS;
			break;
		case static_cast<std::uint8_t>(hardware_artifact_choice::SMARTIFACT):
			result.artifact = hardware_artifact_choice::SMARTIFACT;
			break;
		case static_cast<std::uint8_t>(hardware_artifact_choice::MONOCHROME):
			result.artifact = hardware_artifact_choice::MONOCHROME;
			break;
		case static_cast<std::uint8_t>(hardware_artifact_choice::SOFTWARE):
			result.artifact = hardware_artifact_choice::SOFTWARE;
			break;
		case static_cast<std::uint8_t>(hardware_artifact_choice::MESS):
		default:
			result.artifact = hardware_artifact_choice::MESS;
			break;
		}
		break;
	}

	result.force_lowercase = buttons.force_lowercase_toggle;
	result.quiet_status = buttons.quiet_status_toggle;
	result.w64 = buttons.w64_toggle;
	return result;
}

effective_controls core::controls(pixel_input const &input) const noexcept
{
	effective_controls result;
	result.font = software_font();
	result.artifact = software_artifact();
	result.extras = software_extras();
	result.enhanced = software_enhanced();

	hardware_controls const hardware = hardware_settings(input);
	result.font.force_lowercase ^= hardware.force_lowercase;
	result.artifact.swap ^= hardware.artifact_swap;
	result.extras.scanlines ^= hardware.scanlines;
	result.extras.quiet_status ^= hardware.quiet_status;
	result.extras.inverse_text ^= hardware.inverse_text;
	result.extras.full_semigraphics_palette ^= hardware.reduced_palette;
	result.enhanced.w64 ^= hardware.w64;

	if (hardware.disable_border)
		result.extras.border = border_mode::BLACK;

	result.text_palette = hardware.text_palette;
	if (hardware.text_palette != text_palette_choice::SOFTWARE)
		result.extras.custom_text_palette = true;

	switch (hardware.artifact)
	{
	case hardware_artifact_choice::STANDARD:
		result.artifact.color_enabled = true;
		result.artifact.mode = artifact_mode::STANDARD;
		break;

	case hardware_artifact_choice::FAT_BITS:
		result.artifact.color_enabled = true;
		result.artifact.mode = artifact_mode::FAT_BITS;
		break;

	case hardware_artifact_choice::SMARTIFACT:
		result.artifact.color_enabled = true;
		result.artifact.mode = artifact_mode::SMARTIFACT;
		break;

	case hardware_artifact_choice::MESS:
		result.artifact.color_enabled = true;
		result.artifact.mode = artifact_mode::MESS;
		break;

	case hardware_artifact_choice::MONOCHROME:
		result.artifact.color_enabled = false;
		result.artifact.mode = artifact_mode::MONOCHROME;
		break;

	case hardware_artifact_choice::SOFTWARE:
	default:
		break;
	}

	return result;
}

text_palette_descriptor core::text_palette(pixel_input const &input) const noexcept
{
	effective_controls const settings = controls(input);
	text_palette_descriptor result;
	bool swap_palette_polarity = false;
	bool const character_ram =
			settings.enhanced.character_ram &&
			(m_state.w64_active || settings.font.force_character_ram || input.internal_external);
	bool const dd65_clear = !input.inverse && !BIT(input.data, 5);
	bool black_text_on_color_background =
			input.inverse ||
			settings.font.force_lowercase ||
			character_ram ||
			(!m_state.w64_active &&
					((BIT(input.gm, 0) && dd65_clear) !=
							BIT(input.gm, 1)));
	black_text_on_color_background ^= settings.extras.inverse_text;

	if (settings.text_palette == text_palette_choice::SOFTWARE)
	{
		if (settings.extras.custom_text_palette)
		{
			result.foreground = { palette_source::EXTRA, static_cast<std::uint8_t>(extra_palette_slot::TEXT_FOREGROUND) };
			result.background = { palette_source::EXTRA, static_cast<std::uint8_t>(extra_palette_slot::TEXT_BACKGROUND) };
			swap_palette_polarity = true;
		}
		else if (black_text_on_color_background)
		{
			result.foreground = { palette_source::SEMIGRAPHICS, 0 };
			result.background = { palette_source::SEMIGRAPHICS, std::uint8_t(input.css ? 8 : 1) };
		}
		else
		{
			result.foreground = { palette_source::SEMIGRAPHICS, std::uint8_t(input.css ? 8 : 1) };
			result.background = { palette_source::SEMIGRAPHICS, std::uint8_t(input.css ? 10 : 9) };
		}
	}
	else
	{
		swap_palette_polarity = true;
		switch (settings.text_palette)
		{
		case text_palette_choice::CSS_GREEN_OR_ORANGE:
			result.foreground = { palette_source::SEMIGRAPHICS, 0, true };
			result.background = { palette_source::SEMIGRAPHICS, std::uint8_t(input.css ? 8 : 1), true };
			break;

		case text_palette_choice::BLACK_ON_RED:
			result.foreground = { palette_source::SEMIGRAPHICS, 0, true };
			result.background = { palette_source::SEMIGRAPHICS, 4, true };
			break;

		case text_palette_choice::WHITE_ON_RED:
			result.foreground = { palette_source::SEMIGRAPHICS, 5, true };
			result.background = { palette_source::SEMIGRAPHICS, 4, true };
			break;

		case text_palette_choice::BLACK_ON_YELLOW:
			result.foreground = { palette_source::SEMIGRAPHICS, 0, true };
			result.background = { palette_source::SEMIGRAPHICS, 2, true };
			break;

		case text_palette_choice::BLACK_ON_BLUE:
			result.foreground = { palette_source::SEMIGRAPHICS, 0, true };
			result.background = { palette_source::SEMIGRAPHICS, 3, true };
			break;

		case text_palette_choice::BLUE_ON_WHITE:
			result.foreground = { palette_source::SEMIGRAPHICS, 3, true };
			result.background = { palette_source::SEMIGRAPHICS, 5, true };
			break;

		case text_palette_choice::BLACK_ON_WHITE:
			result.foreground = { palette_source::SEMIGRAPHICS, 0, true };
			result.background = { palette_source::SEMIGRAPHICS, 5, true };
			break;

		case text_palette_choice::SOFTWARE:
		default:
			break;
		}
	}

	if (swap_palette_polarity && !black_text_on_color_background)
		std::swap(result.foreground, result.background);

	return result;
}

bool core::use_character_ram(pixel_input const &input) const noexcept
{
	effective_controls const settings = controls(input);
	if (!settings.enhanced.character_ram)
		return false;
	if (m_state.w64_active)
		return true;
	return settings.font.force_character_ram || input.internal_external;
}

vga_timing_endpoints core::timing_endpoints() const noexcept
{
	return make_vga_timing_endpoints(m_state.registers.timing);
}

bool core::page00_internal_to_logical(std::uint16_t internal_address, std::uint16_t &logical_offset) noexcept
{
	if (internal_address >= 32 && internal_address < 64)
	{
		logical_offset = std::uint16_t(internal_address - 32);
		return true;
	}

	std::uint16_t const row = std::uint16_t(internal_address / 384);
	std::uint16_t const column = std::uint16_t(internal_address % 384);
	if (row >= 1 && row < 16 && column < 32)
	{
		logical_offset = std::uint16_t(row * 32 + column);
		return true;
	}

	return false;
}

pixel_input core::decode_alpha_capture_byte(
		std::uint8_t captured,
		bool css,
		bool internal_external) noexcept
{
	pixel_input result;
	result.data = BIT(captured, 0, 6);
	result.graphics = false;
	result.css = css;
	result.alpha_semigraphics = BIT(captured, 7);
	result.inverse = BIT(captured, 6);
	result.internal_external = internal_external;
	return result;
}

void core::reset_button_controls(register_bank bank) noexcept
{
	button_runtime &buttons = m_state.buttons;

	switch (bank)
	{
	case register_bank::FONT:
		buttons.force_lowercase_toggle = false;
		break;

	case register_bank::ARTIFACT:
		buttons.rg6_button_1_cycle &= 0x06;
		buttons.rg6_button_2_cycle = 0;
		break;

	case register_bank::EXTRAS:
		buttons.text_button_1_cycle = 0;
		buttons.graphics_button_1_cycle = 0;
		buttons.rg6_button_1_cycle &= 0x01;
		buttons.text_button_2_cycle = 0;
		buttons.quiet_status_toggle = false;
		break;

	case register_bank::ENHANCED_MODES:
		buttons.w64_toggle = false;
		break;

	default:
		break;
	}
}

void core::reset_bank(register_bank bank, bool reset_visible_palette) noexcept
{
	register_file &registers = m_state.registers;
	model_descriptor const &board = describe_model(m_state.board);

	switch (bank)
	{
	case register_bank::FONT:
		registers.font = board.default_font;
		break;

	case register_bank::ARTIFACT:
		registers.artifact = board.default_artifact;
		break;

	case register_bank::EXTRAS:
		registers.extras = board.default_extras;
		break;

	case register_bank::SEMIGRAPHICS_PALETTE:
		registers.semigraphics_palette_shadow.fill(0);
		if (reset_visible_palette)
			registers.semigraphics_palette_visible.fill(0);
		break;

	case register_bank::ARTIFACT_PALETTE:
		registers.artifact_palette_shadow.fill(0);
		if (reset_visible_palette)
			registers.artifact_palette_visible.fill(0);
		break;

	case register_bank::EXTRA_PALETTE:
		registers.extra_palette_shadow.fill(0);
		if (reset_visible_palette)
			registers.extra_palette_visible.fill(0);
		break;

	case register_bank::VGA_TIMING:
		registers.timing = vga_timing{};
		break;

	case register_bank::ENHANCED_MODES:
		registers.enhanced_modes = board.default_enhanced_modes;
		break;
	}

	reset_button_controls(bank);
}

void core::write_palette_byte(std::uint16_t &word, bool high_byte, std::uint8_t data) noexcept
{
	if (high_byte)
		word = std::uint16_t((std::uint16_t(data) << 8) | (word & 0x00ff));
	else
		word = std::uint16_t((word & 0xff00) | data);
}

bool core::write_page00(std::uint16_t logical_offset, std::uint8_t data) noexcept
{
	if (logical_offset == 0)
	{
		m_state.reset_mask = data;
		return true;
	}

	if (logical_offset == 1)
	{
		for (unsigned bank = 0; bank < REGISTER_BANK_COUNT; ++bank)
		{
			register_bank const selected = static_cast<register_bank>(bank);
			if (m_state.reset_mask & bank_bit(selected))
				reset_bank(selected, false);
			if (data & bank_bit(selected))
				reset_button_controls(selected);
		}
		m_state.edit_mask = data;
		return true;
	}

	register_file &registers = m_state.registers;

	switch (logical_offset)
	{
	case 3:
		if (!(m_state.edit_mask & bank_bit(register_bank::FONT)))
			return false;
		reset_button_controls(register_bank::FONT);
		registers.font = BIT(data, 0, 3);
		return true;

	case 4:
		if (!(m_state.edit_mask & bank_bit(register_bank::ARTIFACT)))
			return false;
		reset_button_controls(register_bank::ARTIFACT);
		registers.artifact = BIT(data, 0, 4);
		return true;

	case 5:
		if (!(m_state.edit_mask & bank_bit(register_bank::EXTRAS)))
			return false;
		reset_button_controls(register_bank::EXTRAS);
		registers.extras = data;
		return true;

	case 8:
		if (!(m_state.edit_mask & bank_bit(register_bank::ENHANCED_MODES)))
			return false;
		reset_button_controls(register_bank::ENHANCED_MODES);
		registers.enhanced_modes = BIT(data, 0, 3);
		return true;

	default:
		break;
	}

	if (logical_offset >= 32 && logical_offset <= 53)
	{
		if (!(m_state.edit_mask & bank_bit(register_bank::SEMIGRAPHICS_PALETTE)))
			return false;
		std::size_t const slot = (logical_offset - 32) / 2;
		write_palette_byte(registers.semigraphics_palette_shadow[slot], !BIT(logical_offset, 0), data);
		return true;
	}

	if (logical_offset >= 64 && logical_offset <= 127)
	{
		if (!(m_state.edit_mask & bank_bit(register_bank::ARTIFACT_PALETTE)))
			return false;
		std::size_t const slot = (logical_offset - 64) / 2;
		write_palette_byte(registers.artifact_palette_shadow[slot], !BIT(logical_offset, 0), data);
		return true;
	}

	if (logical_offset >= 128 && logical_offset <= 135)
	{
		if (!(m_state.edit_mask & bank_bit(register_bank::EXTRA_PALETTE)))
			return false;
		std::size_t const slot = (logical_offset - 128) / 2;
		write_palette_byte(registers.extra_palette_shadow[slot], !BIT(logical_offset, 0), data);
		return true;
	}

	if (!(m_state.edit_mask & bank_bit(register_bank::VGA_TIMING)))
		return false;

	switch (logical_offset)
	{
	case 480:
		registers.timing.h_active = std::uint16_t(
				(std::uint16_t(BIT(data, 0, 3)) << 8) |
				BIT(registers.timing.h_active, 0, 8));
		return true;
	case 481:
		registers.timing.h_active = std::uint16_t((registers.timing.h_active & 0x0700) | data);
		return true;
	case 482:
		registers.timing.h_front_porch = data;
		return true;
	case 483:
		registers.timing.h_sync_width = data;
		return true;
	case 484:
		registers.timing.h_back_porch = data;
		return true;
	case 486:
		registers.timing.v_active = std::uint16_t(
				(std::uint16_t(BIT(data, 0, 2)) << 8) |
				BIT(registers.timing.v_active, 0, 8));
		return true;
	case 487:
		registers.timing.v_active = std::uint16_t((registers.timing.v_active & 0x0300) | data);
		return true;
	case 488:
		registers.timing.v_front_porch = data;
		return true;
	case 489:
		registers.timing.v_sync_width = data;
		return true;
	case 490:
		registers.timing.v_back_porch = data;
		return true;
	default:
		return false;
	}
}

bool core::write_page00_internal(std::uint16_t internal_address, std::uint8_t data) noexcept
{
	std::uint16_t logical_offset = 0;
	return page00_internal_to_logical(internal_address, logical_offset) && write_page00(logical_offset, data);
}

bool core::write_character(std::uint16_t logical_offset, std::uint8_t data) noexcept
{
	if (logical_offset >= CHARACTER_RAM_BYTES)
		return false;

	m_state.character_ram[logical_offset] = data;
	m_state.character_write_enabled = true;
	return true;
}

bool core::write_character_capture(std::uint16_t capture_address, std::uint8_t data) noexcept
{
	if (capture_address >= CHARACTER_RAM_BYTES * 2)
		return false;
	return write_character(character_capture_to_address(capture_address), data);
}

bool core::ingest_selected_capture(std::uint16_t capture_address, std::uint8_t data) noexcept
{
	if (!freeze())
		return false;

	switch (selected_register_page())
	{
	case register_page::PAGE_00:
		return write_page00_internal(capture_address, data);

	case register_page::PAGE_18:
		return write_character_capture(capture_address, data);

	case register_page::UNKNOWN:
	default:
		return false;
	}
}

void core::clear_character_ram(std::uint8_t value) noexcept
{
	m_state.character_ram.fill(value);
}

void core::end_programming_frame() noexcept
{
	m_state.reset_mask = 0;
	m_state.edit_mask = 0;
	m_state.character_write_enabled = false;
}

bool core::commit_palettes_at_vsync() noexcept
{
	if (freeze())
		return false;

	m_state.registers.semigraphics_palette_visible = m_state.registers.semigraphics_palette_shadow;
	m_state.registers.artifact_palette_visible = m_state.registers.artifact_palette_shadow;
	m_state.registers.extra_palette_visible = m_state.registers.extra_palette_shadow;
	return true;
}

combo_step core::clock_combo(
		bool field_sync_pin_high,
		std::uint8_t selector,
		bool reset_asserted,
		bool reset_complete) noexcept
{
	combo_step result;
	result.previous = m_state.lock_state;
	combo_state next = result.previous;
	selector = BIT(selector, 0, 5);

	if (reset_asserted || !reset_complete)
	{
		next = combo_state::RESET;
	}
	else
	{
		switch (result.previous)
		{
		case combo_state::RESET:
			next = combo_state::INIT;
			break;

		case combo_state::INIT:
			if (!field_sync_pin_high)
				next = combo_state::READY;
			break;

		case combo_state::READY:
			if (field_sync_pin_high)
				next = combo_state::INIT;
			else if (selector == COMBO_1)
				next = combo_state::UNLOCKED_1;
			break;

		case combo_state::UNLOCKED_1:
			if (field_sync_pin_high)
				next = combo_state::INIT;
			else if (selector == COMBO_2)
				next = combo_state::UNLOCKED_2;
			break;

		case combo_state::UNLOCKED_2:
			if (field_sync_pin_high)
				next = combo_state::INIT;
			else if (selector == COMBO_3)
				next = combo_state::UNLOCKED_3;
			break;

		case combo_state::UNLOCKED_3:
			if (field_sync_pin_high)
				next = combo_state::INIT;
			else if (selector == COMBO_4)
				next = combo_state::WAIT_MODE_1;
			break;

		case combo_state::WAIT_MODE_1:
			if (field_sync_pin_high)
				next = combo_state::INIT;
			else if (selector != COMBO_4)
				next = combo_state::WAIT_MODE_2;
			break;

		case combo_state::WAIT_MODE_2:
			next = combo_state::WAIT_MODE_3;
			break;

		case combo_state::WAIT_MODE_3:
			next = combo_state::UNLOCKED_A;
			result.page_captured = true;
			result.captured_selector = selector;
			m_state.selected_page = selector;
			m_state.page_selected = true;
			break;

		case combo_state::UNLOCKED_A:
			if (field_sync_pin_high)
				next = combo_state::UNLOCKED_B;
			break;

		case combo_state::UNLOCKED_B:
			if (!field_sync_pin_high)
			{
				next = combo_state::READY;
				result.programming_released = true;
			}
			break;
		}
	}

	if (next == combo_state::RESET)
		end_programming_frame();
	if (result.programming_released)
		end_programming_frame();

	m_state.lock_state = next;
	if (field_sync_pin_high && next == combo_state::INIT)
		latch_previous_field_mode(BIT(selector, 3), BIT(selector, 0, 3));
	result.current = next;
	result.freeze = freeze();
	return result;
}

button_events core::clock_buttons(bool button_1, bool button_2, pixel_input const &input) noexcept
{
	button_runtime &buttons = m_state.buttons;
	button_events events;
	bool const button_1_rising = button_1 && !buttons.button_1_down;
	bool const button_2_rising = button_2 && !buttons.button_2_down;
	bool const button_1_falling = !button_1 && buttons.button_1_down;
	bool const button_2_falling = !button_2 && buttons.button_2_down;
	bool const button_1_held_long = buttons.button_1_hold_ticks >= BUTTON_HOLD_CYCLES;
	bool const button_2_held_long = buttons.button_2_hold_ticks >= BUTTON_HOLD_CYCLES;

	if (button_1_rising)
	{
		buttons.button_1_hold_ticks = 1;
	}
	else if (button_1 && buttons.button_1_hold_ticks < BUTTON_HOLD_CYCLES)
	{
		++buttons.button_1_hold_ticks;
		if (buttons.button_1_hold_ticks == BUTTON_HOLD_CYCLES)
			events.button_1_hold = true;
	}

	if (button_2_rising)
	{
		buttons.button_2_hold_ticks = 1;
	}
	else if (button_2 && buttons.button_2_hold_ticks < BUTTON_HOLD_CYCLES)
	{
		++buttons.button_2_hold_ticks;
		if (buttons.button_2_hold_ticks == BUTTON_HOLD_CYCLES)
			events.button_2_hold = true;
	}

	// Counters remain independent, but Button 1 owns the hold action while both are held.
	if (events.button_2_hold && button_1 && buttons.button_1_hold_ticks >= BUTTON_HOLD_CYCLES)
		events.button_2_hold = false;

	if (button_1_falling)
	{
		events.button_1_press = !button_1_held_long;
		buttons.button_1_hold_ticks = 0;
	}
	if (button_2_falling)
	{
		events.button_2_press = !button_2_held_long;
		buttons.button_2_hold_ticks = 0;
	}

	buttons.button_1_down = button_1;
	buttons.button_2_down = button_2;

	apply_button_events(events, context_for(input));
	return events;
}

void core::apply_button_events(button_events const &events, button_context context) noexcept
{
	button_runtime &buttons = m_state.buttons;

	if (events.button_1_press)
	{
		switch (context)
		{
		case button_context::TEXT:
			buttons.text_button_1_cycle = std::uint8_t((buttons.text_button_1_cycle + 1) & 0x07);
			break;
		case button_context::GRAPHICS:
			buttons.graphics_button_1_cycle = std::uint8_t((buttons.graphics_button_1_cycle + 1) & 0x03);
			break;
		case button_context::RG6:
			buttons.rg6_button_1_cycle = std::uint8_t((buttons.rg6_button_1_cycle + 1) & 0x07);
			break;
		}
	}

	if (events.button_1_hold)
	{
		switch (context)
		{
		case button_context::TEXT:
			buttons.force_lowercase_toggle = !buttons.force_lowercase_toggle;
			break;
		case button_context::GRAPHICS:
			buttons.w64_toggle = !buttons.w64_toggle;
			break;
		case button_context::RG6:
			buttons.w64_toggle = !buttons.w64_toggle;
			break;
		}
	}

	if (events.button_2_press)
	{
		switch (context)
		{
		case button_context::TEXT:
			buttons.text_button_2_cycle = std::uint8_t((buttons.text_button_2_cycle + 1) & 0x07);
			break;
		case button_context::GRAPHICS:
			break;
		case button_context::RG6:
			m_state.registers.artifact = 0;
			buttons.rg6_button_2_cycle =
					buttons.rg6_button_2_cycle >= static_cast<std::uint8_t>(hardware_artifact_choice::MONOCHROME)
						? static_cast<std::uint8_t>(hardware_artifact_choice::STANDARD)
						: std::uint8_t(buttons.rg6_button_2_cycle + 1);
			break;
		}
	}

	if (events.button_2_hold)
		buttons.quiet_status_toggle = !buttons.quiet_status_toggle;
}

void core::latch_previous_field_mode(bool previous_field_graphics, std::uint8_t previous_field_gm) noexcept
{
	if (freeze())
		return;

	enhanced_controls const enhanced = software_enhanced();
	bool const w64 = enhanced.w64 ^ m_state.buttons.w64_toggle;
	previous_field_gm = BIT(previous_field_gm, 0, 3);
	m_state.vg6_active = previous_field_graphics && previous_field_gm == 6 && enhanced.vg6;
	m_state.w64_active = previous_field_graphics && previous_field_gm == 2 && w64;
}

} // namespace cocovga
