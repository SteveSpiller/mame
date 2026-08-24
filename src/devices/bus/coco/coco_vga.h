// license:BSD-3-Clause
// copyright-holders:Stephen Spiller
/*********************************************************************

    coco_vga.h

    CoCoVGA, a video display generator socket board for the Tandy
    Color Computer 1/2.

*********************************************************************/

#ifndef MAME_BUS_COCO_COCO_VGA_H
#define MAME_BUS_COCO_COCO_VGA_H

#pragma once

#include "vdgsocket.h"

#include "video/cocovga.h"

#include "screen.h"


//**************************************************************************
//  TYPE DEFINITIONS
//**************************************************************************

// ======================> coco_vga_device

class coco_vga_device final
	: public device_t
	, public device_coco_vdg_interface
{
public:
	// construction/destruction
	coco_vga_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0U);

	DECLARE_INPUT_CHANGED_MEMBER(button_changed);

protected:
	// device_t implementation
	virtual void device_start() override ATTR_COLD;
	virtual void device_reset_after_children() override ATTR_COLD;
	virtual void device_add_mconfig(machine_config &config) override ATTR_COLD;
	virtual ioport_constructor device_input_ports() const override ATTR_COLD;

	// device_coco_vdg_interface implementation
	virtual void sam_fetch(offs_t offset, u8 data) override;
	virtual void mode_w(u8 data) override;
	virtual void hs_w(int state) override;
	virtual void fs_w(int state) override;
	virtual bool composite_output_active() const override;

private:
	required_device<cocovga_device> m_vga;
	required_device<screen_device> m_vga_screen;
	required_ioport m_config;
	required_ioport m_buttons;
};


// device type declaration
DECLARE_DEVICE_TYPE(COCO_VGA, coco_vga_device)

#endif // MAME_BUS_COCO_COCO_VGA_H
