// license:BSD-3-Clause
// copyright-holders:Stephen Spiller
/*********************************************************************

    vdgsocket.h

    Video display generator socket of a Tandy Color Computer 1/2.

    The MC6847 sits in a socket on the official Color Computer 1 and 2
    main boards, and video enhancement boards are installed by lifting
    the VDG and plugging into the socket underneath it.  Such a board
    watches the same synchronous address multiplexer fetches and mode
    pins the VDG sees, and may take over the machine's video output.

    The socket is empty by default, so a machine that fits no board
    behaves exactly as it does without this bus.

*********************************************************************/

#ifndef MAME_BUS_COCO_VDGSOCKET_H
#define MAME_BUS_COCO_VDGSOCKET_H

#pragma once


//**************************************************************************
//  TYPE DEFINITIONS
//**************************************************************************

class device_coco_vdg_interface;


// ======================> coco_vdg_socket_device

class coco_vdg_socket_device final
	: public device_t
	, public device_single_card_slot_interface<device_coco_vdg_interface>
{
public:
	// construction/destruction
	template <typename T>
	coco_vdg_socket_device(const machine_config &mconfig, const char *tag, device_t *owner, T &&opts, const char *dflt)
		: coco_vdg_socket_device(mconfig, tag, owner, 0U)
	{
		option_reset();
		opts(*this);
		set_default_option(dflt);
		set_fixed(false);
	}
	coco_vdg_socket_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0U);

	// host signals; every entry point is safe to call with an empty socket
	void sam_fetch(offs_t offset, u8 data);
	void mode_w(u8 data);
	void hs_w(int state);
	void fs_w(int state);

	// true while the machine's own composite output should still be displayed
	bool composite_output_active() const;

protected:
	// device_t implementation
	virtual void device_start() override ATTR_COLD;

private:
	device_coco_vdg_interface *m_board;
};


// ======================> device_coco_vdg_interface

class device_coco_vdg_interface : public device_interface
{
public:
	// destruction
	virtual ~device_coco_vdg_interface();

	// a fetch the synchronous address multiplexer performed for the VDG
	virtual void sam_fetch(offs_t offset, u8 data) = 0;

	// PIA 1 port B, which carries the VDG mode pins
	virtual void mode_w(u8 data) = 0;

	// raw VDG sync pins
	virtual void hs_w(int state) = 0;
	virtual void fs_w(int state) = 0;

	virtual bool composite_output_active() const = 0;

protected:
	device_coco_vdg_interface(const machine_config &mconfig, device_t &device);
};


// device type declaration
DECLARE_DEVICE_TYPE(COCO_VDG_SOCKET, coco_vdg_socket_device)


void coco_vdg_socket_devices(device_slot_interface &device) ATTR_COLD;

#endif // MAME_BUS_COCO_VDGSOCKET_H
