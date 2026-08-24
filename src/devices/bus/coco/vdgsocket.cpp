// license:BSD-3-Clause
// copyright-holders:Stephen Spiller
/*********************************************************************

    vdgsocket.cpp

    Video display generator socket of a Tandy Color Computer 1/2.

*********************************************************************/

#include "emu.h"
#include "vdgsocket.h"

#include "coco_vga.h"


//**************************************************************************
//  GLOBAL VARIABLES
//**************************************************************************

DEFINE_DEVICE_TYPE(COCO_VDG_SOCKET, coco_vdg_socket_device, "coco_vdg_socket", "Color Computer VDG socket")


//-------------------------------------------------
//  coco_vdg_socket_device - constructor
//-------------------------------------------------

coco_vdg_socket_device::coco_vdg_socket_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
	: device_t(mconfig, COCO_VDG_SOCKET, tag, owner, clock)
	, device_single_card_slot_interface<device_coco_vdg_interface>(mconfig, *this)
	, m_board(nullptr)
{
}


//-------------------------------------------------
//  device_start - device-specific startup
//-------------------------------------------------

void coco_vdg_socket_device::device_start()
{
	m_board = get_card_device();
}


//-------------------------------------------------
//  sam_fetch - report a video fetch the
//  synchronous address multiplexer performed
//-------------------------------------------------

void coco_vdg_socket_device::sam_fetch(offs_t offset, u8 data)
{
	if (m_board)
		m_board->sam_fetch(offset, data);
}


//-------------------------------------------------
//  mode_w - report PIA 1 port B, which carries
//  the VDG mode pins
//-------------------------------------------------

void coco_vdg_socket_device::mode_w(u8 data)
{
	if (m_board)
		m_board->mode_w(data);
}


//-------------------------------------------------
//  hs_w - report the VDG horizontal sync pin
//-------------------------------------------------

void coco_vdg_socket_device::hs_w(int state)
{
	if (m_board)
		m_board->hs_w(state);
}


//-------------------------------------------------
//  fs_w - report the VDG field sync pin
//-------------------------------------------------

void coco_vdg_socket_device::fs_w(int state)
{
	if (m_board)
		m_board->fs_w(state);
}


//-------------------------------------------------
//  composite_output_active - an empty socket
//  leaves the machine's own output alone
//-------------------------------------------------

bool coco_vdg_socket_device::composite_output_active() const
{
	return !m_board || m_board->composite_output_active();
}


//**************************************************************************
//  DEVICE COCO VDG INTERFACE - implemented by boards that install in the
//  video display generator socket
//**************************************************************************

template class device_finder<device_coco_vdg_interface, false>;
template class device_finder<device_coco_vdg_interface, true>;


//-------------------------------------------------
//  device_coco_vdg_interface - constructor
//-------------------------------------------------

device_coco_vdg_interface::device_coco_vdg_interface(const machine_config &mconfig, device_t &device)
	: device_interface(device, "cocovdg")
{
}


//-------------------------------------------------
//  ~device_coco_vdg_interface - destructor
//-------------------------------------------------

device_coco_vdg_interface::~device_coco_vdg_interface()
{
}


//-------------------------------------------------
//  coco_vdg_socket_devices
//-------------------------------------------------

void coco_vdg_socket_devices(device_slot_interface &device)
{
	device.option_add("cocovga", COCO_VGA);
}
