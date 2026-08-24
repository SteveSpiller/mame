// license:BSD-3-Clause
// copyright-holders:Stephen Spiller
/*********************************************************************

    coco_vga.cpp

    CoCoVGA, a video display generator socket board for the Tandy
    Color Computer 1/2.

    The board is installed in the VDG socket, watches the same
    synchronous address multiplexer fetches and mode pins the VDG sees,
    and regenerates the picture on a 640x480 VGA output of its own.  It
    can also blank the machine's composite output, which is what the
    real board does when its composite pass-through is disabled.

    The character generator the board reimplements lives in the
    CoCoVGA device itself, which carries both MC6847 character
    generator ROMs; installing the board is therefore the only thing
    that adds a ROM requirement to a machine.

    http://cocovga.com/documentation/specifications/

*********************************************************************/

#include "emu.h"
#include "coco_vga.h"

#include "coco_vga.lh"


//**************************************************************************
//  GLOBAL VARIABLES
//**************************************************************************

DEFINE_DEVICE_TYPE(COCO_VGA, coco_vga_device, "coco_vga", "CoCoVGA")


//**************************************************************************
//  INPUT PORTS
//**************************************************************************

static INPUT_PORTS_START( coco_vga )
	PORT_START("config")
	PORT_CONFNAME(0x03, 0x00, "CoCoVGA Model (requires reset)")
	PORT_CONFSETTING(0x00, "AMC2")
	PORT_CONFSETTING(0x01, "AMC3/F1/KMC1 15-bit")
	PORT_CONFSETTING(0x02, "T1")
	PORT_CONFNAME(0x0c, 0x08, "CoCoVGA Output (requires reset)")
	PORT_CONFSETTING(0x00, "Composite")
	PORT_CONFSETTING(0x04, "VGA")
	PORT_CONFSETTING(0x08, "Both")

	PORT_START("buttons")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_OTHER) PORT_NAME("CoCoVGA Button 1") PORT_CODE(KEYCODE_F1) PORT_CHANGED_MEMBER(DEVICE_SELF, FUNC(coco_vga_device::button_changed), 1)
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_OTHER) PORT_NAME("CoCoVGA Button 2") PORT_CODE(KEYCODE_F2) PORT_CHANGED_MEMBER(DEVICE_SELF, FUNC(coco_vga_device::button_changed), 2)
INPUT_PORTS_END


//-------------------------------------------------
//  coco_vga_device - constructor
//-------------------------------------------------

coco_vga_device::coco_vga_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
	: device_t(mconfig, COCO_VGA, tag, owner, clock)
	, device_coco_vdg_interface(mconfig, *this)
	, m_vga(*this, "vga")
	, m_vga_screen(*this, "screen")
	, m_config(*this, "config")
	, m_buttons(*this, "buttons")
{
}


//-------------------------------------------------
//  device_input_ports - device-specific input
//  ports
//-------------------------------------------------

ioport_constructor coco_vga_device::device_input_ports() const
{
	return INPUT_PORTS_NAME(coco_vga);
}


//-------------------------------------------------
//  device_add_mconfig - device-specific machine
//  configuration
//-------------------------------------------------

void coco_vga_device::device_add_mconfig(machine_config &config)
{
	SCREEN(config, m_vga_screen);
	m_vga_screen->set_raw(cocovga_device::DEFAULT_PIXEL_CLOCK, 800, 0, 640, 525, 0, 480);
	m_vga_screen->set_screen_update(m_vga, FUNC(cocovga_device::screen_update));

	COCOVGA(config, m_vga, cocovga_device::DEFAULT_PIXEL_CLOCK);
	m_vga->set_screen(m_vga_screen);

	config.set_default_layout(layout_coco_vga);
}


//-------------------------------------------------
//  device_start - device-specific startup
//-------------------------------------------------

void coco_vga_device::device_start()
{
}


//-------------------------------------------------
//  device_reset_after_children - sample the
//  reset-time configuration
//-------------------------------------------------

void coco_vga_device::device_reset_after_children()
{
	// The board is configured by jumpers and by which FPGA image it was
	// programmed with, so this is sampled once the CoCoVGA device itself has
	// reset rather than while the machine is running.
	ioport_value const config = m_config->read();

	cocovga::model board;
	switch (BIT(config, 0, 2))
	{
	case 0:
		board = cocovga::model::AMC2;
		break;

	case 2:
		board = cocovga::model::T1;
		break;

	default:
		board = cocovga::model::MODERN;
		break;
	}

	cocovga::output_policy policy;
	switch (BIT(config, 2, 2))
	{
	case 0:
		policy = cocovga::output_policy::COMPOSITE;
		break;

	case 2:
		policy = cocovga::output_policy::BOTH;
		break;

	default:
		policy = cocovga::output_policy::VGA;
		break;
	}

	m_vga->apply_configuration(board, policy);

	ioport_value const buttons = m_buttons->read();
	m_vga->button_1_w(BIT(buttons, 0));
	m_vga->button_2_w(BIT(buttons, 1));
}


//-------------------------------------------------
//  button_changed - the two board push buttons
//-------------------------------------------------

INPUT_CHANGED_MEMBER(coco_vga_device::button_changed)
{
	if (param == 1)
		m_vga->button_1_w(newval);
	else if (param == 2)
		m_vga->button_2_w(newval);
}


//-------------------------------------------------
//  sam_fetch - the board sees every video fetch
//  the synchronous address multiplexer performs
//-------------------------------------------------

void coco_vga_device::sam_fetch(offs_t offset, u8 data)
{
	if (offset == offs_t(-1))
		return;

	cocovga::host_fetch_event event;
	event.sam_address = u16(offset);
	event.data = data;
	event.mode = m_vga->mode();
	event.mode.alpha_semigraphics = BIT(data, 7);
	event.mode.inverse = BIT(data, 6);
	event.phase = cocovga::host_phase::ACTIVE;
	event.sam_address_valid = offset <= 0xffff;
	event.sam_address_is_offset = true;
	m_vga->capture_fetch(event);
}


//-------------------------------------------------
//  mode_w - PIA 1 port B carries the VDG mode pins
//-------------------------------------------------

void coco_vga_device::mode_w(u8 data)
{
	cocovga::host_mode_pins mode = m_vga->mode();
	mode.gm = BIT(data, 4, 3);
	mode.graphics = BIT(data, 7);
	mode.css = BIT(data, 3);
	mode.internal_external = BIT(data, 4);
	m_vga->mode_update(mode);
}


//-------------------------------------------------
//  hs_w - VDG horizontal sync pin
//-------------------------------------------------

void coco_vga_device::hs_w(int state)
{
	m_vga->hsync_pin_w(state);
}


//-------------------------------------------------
//  fs_w - VDG field sync pin
//-------------------------------------------------

void coco_vga_device::fs_w(int state)
{
	m_vga->fsync_pin_w(state);
}


//-------------------------------------------------
//  composite_output_active
//-------------------------------------------------

bool coco_vga_device::composite_output_active() const
{
	return m_vga->composite_output_active();
}
