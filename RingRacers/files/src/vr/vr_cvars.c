// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vr_cvars.c
/// \brief VR console variable bridge.
///
/// The cv_vr_* consvars (declared in cvars.cpp, edited from the VR Options
/// menu, saved to config like any other setting) are the persistent interface
/// to the VR module. Each onchange handler here converts the cvar's stored
/// integer into the module's native unit and pushes it in; the module applies
/// it live. Registration and config execution both run these handlers, so the
/// module is in sync with the saved settings before VR even boots. Without
/// ENABLE_VR the setters are inline no-ops and this whole bridge is inert.

#include "vr.h"

#include "../command.h"

extern consvar_t cv_vr_effectstrength;
extern consvar_t cv_vr_depthlayer;
extern consvar_t cv_vr_diorama_dist;
extern consvar_t cv_vr_diorama_height;
extern consvar_t cv_vr_diorama_scale;
extern consvar_t cv_vr_fp_eyeheight;
extern consvar_t cv_vr_fp_forward;
extern consvar_t cv_vr_headscale;
extern consvar_t cv_vr_horizonlock;
extern consvar_t cv_vr_calib;
extern consvar_t cv_vr_immersion;
extern consvar_t cv_vr_huddist;
extern consvar_t cv_vr_hudlock;
extern consvar_t cv_vr_hudscale;
extern consvar_t cv_vr_menudist;
extern consvar_t cv_vr_msaa;
extern consvar_t cv_vr_renderscale;
extern consvar_t cv_vr_stereo;
extern consvar_t cv_vr_tp_eyeheight;
extern consvar_t cv_vr_viewmode;
extern consvar_t cv_vr_worldeffect;
extern consvar_t cv_vr_worldscale;

// p_local.h: re-place the chase camera on the spot. The view knobs are edited
// from the pause menu, where the camera is frozen, so anything the GAME places
// (rather than the module) has to be pushed by hand or the slider looks dead.
void P_UpdateCamerasNow(void);

void VrDepthLayer_OnChange(void)
{
	vr_set_depth_layer(cv_vr_depthlayer.value != 0);
}

void VrDiorama_OnChange(void)
{
	vr_set_diorama_scale((float)cv_vr_diorama_scale.value);
	vr_set_diorama_dist(cv_vr_diorama_dist.value / 100.0f); // cm -> m
	vr_set_diorama_height(cv_vr_diorama_height.value / 100.0f); // cm -> m
}

void VrFpEyeHeight_OnChange(void)
{
	vr_set_fp_eyeheight((float)cv_vr_fp_eyeheight.value);
	P_UpdateCamerasNow(); // the flat first-person camera reads it game-side
}

void VrFpForward_OnChange(void)
{
	vr_set_fp_forward((float)cv_vr_fp_forward.value);
	P_UpdateCamerasNow();
}

// Third person's camera distance is placed by the game's own chase camera, not
// by the module, so this is the one view knob with nothing to push - it just
// needs the camera re-run.
void VrTpForward_OnChange(void)
{
	P_UpdateCamerasNow();
}

void VrHeadScale_OnChange(void)
{
	vr_set_head_motion_scale(cv_vr_headscale.value / 100.0f); // percent
}

void VrHorizonLock_OnChange(void)
{
	vr_set_horizon_lock(cv_vr_horizonlock.value != 0);
}

void VrImmersion_OnChange(void)
{
	vr_set_immersion(cv_vr_immersion.value);
}

void VrCalib_OnChange(void)
{
	vr_set_calib(cv_vr_calib.value != 0);
}

void VrHudDist_OnChange(void)
{
	vr_set_hud_dist(cv_vr_huddist.value / 100.0f); // cm -> m
}

void VrMenuDist_OnChange(void)
{
	vr_set_menu_dist(cv_vr_menudist.value / 100.0f); // cm -> m
}

void VrTpEyeHeight_OnChange(void)
{
	vr_set_tp_eyeheight(cv_vr_tp_eyeheight.value / 100.0f); // cm -> m
}

void VrHudLock_OnChange(void)
{
	vr_set_hud_world(cv_vr_hudlock.value != 0);
}

void VrHudScale_OnChange(void)
{
	vr_set_hud_scale(cv_vr_hudscale.value / 100.0f); // percent
}

void VrMsaa_OnChange(void)
{
	vr_set_msaa(cv_vr_msaa.value);
}

void VrRenderScale_OnChange(void)
{
	vr_set_render_scale(cv_vr_renderscale.value / 100.0f); // percent
}

void VrStereo_OnChange(void)
{
	vr_set_stereo_strength(cv_vr_stereo.value / 100.0f); // percent
}

void VrViewMode_OnChange(void)
{
	vr_set_view_mode(cv_vr_viewmode.value);
	P_UpdateCamerasNow(); // flat first person / diorama reshape the chase cam
}

void VrWorldEffect_OnChange(void)
{
	vr_set_world_effect(cv_vr_worldeffect.value != 0);
}

void VrEffectStrength_OnChange(void)
{
	vr_set_effect_strength((float)cv_vr_effectstrength.value / 100.0f);
}

void VrWorldScale_OnChange(void)
{
	vr_set_world_scale((float)cv_vr_worldscale.value);
}
