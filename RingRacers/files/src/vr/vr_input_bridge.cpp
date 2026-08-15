// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vr_input_bridge.cpp
/// \brief VR motion controllers as a synthetic gamepad.
///
/// Mirrors the OpenXR controller state (vr_controller_buttons / _stick) into
/// the game's own event queue as one synthetic gamepad device, posting the
/// exact same events the SDL gamepad path produces (see sdl/i_video.cpp:
/// Impl_HandleControllerButtonEvent / AxisEvent / DeviceAddedEvent). From
/// there the stock input pipeline takes over unmodified: the device registers
/// as an available gamepad, navigates menus as player 1, gets assigned
/// through the normal device-assignment flows, and drives gameplay through
/// the player's own control profile. No game action is hardcoded here - this
/// layer only decides which PAD control each physical VR control impersonates.
///
/// Without ENABLE_VR this file compiles to nothing (vr.h provides an inline
/// no-op for vr_input_bridge_update).

#include "vr.h"

#ifdef ENABLE_VR

#include "../doomdef.h"
#include "../d_event.h"
#include "../d_main.h"  // D_PostEvent
#include "../doomstat.h" // gamestate / starttime / players / consoleplayer
#include "../d_player.h" // trickstate_t / PF_TRICKDELAY (the trick arming)
#include "../p_tick.h"  // leveltime
#include "../g_input.h" // KEY_JOY1 / KEY_HAT1 virtual key layout
#include "../i_joy.h"   // JOYAXISRANGE

#include <math.h>

// Physical VR control -> pad button (the same SDL_GamepadButton-derived
// virtual keys a real pad posts, so binding menus and profiles see no
// difference). Triggers are not in this table: they go out as the trigger
// AXIS pair, exactly like a real pad's analog triggers.
struct VrPadButton
{
	unsigned bit;  // VR_BTN_* mask bit
	INT32 key;     // virtual key code posted for it
};

// The right stick click is absent on purpose: it cycles the VR view mode and
// is consumed in vr_input_bridge_update before this table runs, so the game
// never sees it as a pad button.
static const VrPadButton kPadButtons[] =
{
	{ VR_BTN_A,      KEY_JOY1 + 0 },  // right hand A       -> pad A  (accel)
	{ VR_BTN_B,      KEY_JOY1 + 2 },  // right hand B       -> pad X  (brake/reverse)
	{ VR_BTN_X,      KEY_JOY1 + 10 }, // left hand X        -> pad RB (vote)
	{ VR_BTN_Y,      KEY_JOY1 + 1 },  // left hand Y        -> pad B  (look back / menu back)
	{ VR_BTN_MENU,   KEY_JOY1 + 6 },  // menu button        -> pad Start
	{ VR_BTN_LSTICK, KEY_JOY1 + 1 },  // left stick click   -> pad B  (look back; stock LS click is unbound)
	{ VR_BTN_LGRIP,  KEY_JOY1 + 9 },  // left grip squeeze  -> pad LB (respawn)
	{ VR_BTN_RGRIP,  KEY_JOY1 + 3 },  // right grip squeeze -> pad Y  (spindash)
};

static bool     sDeviceAnnounced = false; // ev_gamepad_device_added posted
static bool     sWasActive       = false; // controllers were live last frame
static bool     sRStickHeld      = false; // right stick click edge (view-mode cycling)
static unsigned sPrevButtons     = 0;     // VR_BTN_* mask as of the last posted events

// Last posted value per axis set [left stick, right stick, triggers][x, y],
// so steady input posts nothing (the SDL path also only posts on change).
static INT32 sPrevAxis[3][2];

static void post_key(INT32 key, bool down)
{
	event_t ev = {};
	ev.type = down ? ev_keydown : ev_keyup;
	ev.data1 = key;
	ev.device = VR_GAMEPAD_DEVICE_ID;
	D_PostEvent(&ev);
}

// Post one axis set (both of its axes at once; the handler takes data2 and
// data3 together) if either value changed since the last post.
static void post_axis_pair(INT32 set, INT32 x, INT32 y)
{
	if (sPrevAxis[set][0] == x && sPrevAxis[set][1] == y)
		return;
	sPrevAxis[set][0] = x;
	sPrevAxis[set][1] = y;

	event_t ev = {};
	ev.type = ev_gamepad_axis;
	ev.device = VR_GAMEPAD_DEVICE_ID;
	ev.data1 = set;
	ev.data2 = x;
	ev.data3 = y;
	D_PostEvent(&ev);
}

// OpenXR stick float (-1..1) -> game axis units. The game applies its own
// per-player deadzone (cv_deadzone in G_PlayerInputAnalog), so none is added
// here.
static INT32 axis_units(float f)
{
	if (f < -1.0f)
		f = -1.0f;
	if (f > 1.0f)
		f = 1.0f;
	return (INT32)lroundf(f * (float)JOYAXISRANGE);
}

// Post events for every difference between the given controller state and
// what was last posted. Passing zeroes releases everything.
static void post_state(unsigned buttons, const float ls[2], const float rs[2])
{
	const unsigned changed = buttons ^ sPrevButtons;
	for (size_t i = 0; i < sizeof(kPadButtons) / sizeof(kPadButtons[0]); i++)
	{
		if (changed & kPadButtons[i].bit)
			post_key(kPadButtons[i].key, !!(buttons & kPadButtons[i].bit));
	}
	sPrevButtons = buttons;

	// Sticks. Game axis convention (from the SDL path): +x right, +y DOWN;
	// OpenXR sticks are +y up, so y flips.
	post_axis_pair(0, axis_units(ls[0]), -axis_units(ls[1]));
	post_axis_pair(1, axis_units(rs[0]), -axis_units(rs[1]));

	// Triggers ride the VR layer's hysteresis latch (press >60%, release
	// <40%), posted as the full-or-nothing trigger axis pair - the same shape
	// a real pad in "gamepad style" axis mode produces.
	post_axis_pair(2,
		(buttons & VR_BTN_LTRIGGER) ? JOYAXISRANGE : 0,
		(buttons & VR_BTN_RTRIGGER) ? JOYAXISRANGE : 0);
}

extern "C" void vr_input_bridge_update(void)
{
	// First person is disorienting to ENTER mid-countdown: lock switching
	// into it while the pre-race intro/countdown runs. Staying in it (or
	// cycling between the other modes) is always allowed.
	vr_set_fp_switch_locked(gamestate == GS_LEVEL && leveltime < starttime);

	if (!vr_controllers_active())
	{
		// Focus loss (headset doffed, runtime dashboard up): release every
		// button and center the sticks so nothing stays held down. The
		// device itself stays registered - posting a device-removed event
		// would kick the game back to the title screen mid-session.
		if (sWasActive)
		{
			const float zero[2] = { 0.0f, 0.0f };
			post_state(0, zero, zero);
			sWasActive = false;
			sRStickHeld = false;
		}
		return;
	}

	if (!sDeviceAnnounced)
	{
		// First activation: register with the game as a plugged-in gamepad.
		// The id is reserved (see VR_GAMEPAD_DEVICE_ID) so it can never
		// collide with a real SDL pad.
		event_t ev = {};
		ev.type = ev_gamepad_device_added;
		ev.device = VR_GAMEPAD_DEVICE_ID;
		D_PostEvent(&ev);
		sDeviceAnnounced = true;
	}
	sWasActive = true;

	float ls[2], rs[2];
	vr_controller_stick(0, ls);
	vr_controller_stick(1, rs);

	// Right stick click cycles the VR view mode. Consumed here - masked out
	// before the pad mirror below - so the game never sees the press.
	unsigned buttons = vr_controller_buttons();
	const bool rstick = (buttons & VR_BTN_RSTICK) != 0;
	if (rstick && !sRStickHeld)
		vr_cycle_view_mode();
	sRStickHeld = rstick;
	buttons &= ~VR_BTN_RSTICK;

	// Tricks used to be assisted here, for the headset only. They aren't any
	// more: the arming problem this was working around belongs to everyone who
	// holds accelerate, not just VR players, so the assist moved into the
	// ticcmd builder where every device passes through it (trick_assist in
	// g_build_ticcmd.cpp). Doing it in both places would leave the two fighting
	// over the same buttons. The right trigger still reaches the game as drift,
	// which is exactly what the assist reads as "trick now".

	post_state(buttons, ls, rs);
}

#endif // ENABLE_VR
