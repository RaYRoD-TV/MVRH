// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  menus/options-video-vr.c
/// \brief VR Options

#include "../k_menu.h"
#include "../screen.h" // cv_scr_effect
#include "../hardware/hw_main.h" // cv_glsupersample
#include "../k_hazard.h" // handling / landing dash / trick + hazard knobs
#include "../vr/vr.h"

extern consvar_t cv_vr_viewmode, cv_vr_worldscale, cv_vr_stereo, cv_vr_headscale,
	cv_vr_horizonlock, cv_vr_immersion, cv_vr_fp_eyeheight, cv_vr_fp_forward, cv_vr_fp_shadow,
	cv_vr_shadowlift, cv_vr_shadowinterp, cv_vr_shadowtest, cv_vr_tp_forward,
	cv_vr_tp_eyeheight, cv_vr_renderscale,
	cv_vr_msaa, cv_vr_mirror, cv_vr_hud, cv_vr_hudscale, cv_vr_huddist,
	cv_vr_menudist, cv_vr_hudlock, cv_vr_diorama_scale, cv_vr_diorama_dist,
	cv_vr_diorama_height, cv_vr_worldeffect, cv_vr_depthlayer,
	cv_vr_effectstrength;

static void M_VrRecenter(INT32 choice)
{
	(void)choice;
	vr_recenter();
}

// Every VR cvar on this page, for the reset item below. Screen Effect and
// Texture Supersampling are on the page for reach - they're worth a knob you
// can find without taking the headset off - but they belong to the flat game
// too, so resetting the VR settings leaves them alone.
static consvar_t *const vr_option_cvars[] =
{
	&cv_vr_viewmode, &cv_vr_worldscale, &cv_vr_stereo, &cv_vr_headscale,
	&cv_vr_horizonlock, &cv_vr_immersion, &cv_vr_fp_eyeheight, &cv_vr_fp_forward, &cv_vr_fp_shadow,
	&cv_vr_shadowlift, &cv_vr_shadowinterp,
	&cv_vr_tp_forward, &cv_vr_tp_eyeheight,
	&cv_vr_worldeffect, &cv_vr_effectstrength,
	&cv_vr_renderscale, &cv_vr_msaa, &cv_vr_mirror, &cv_vr_depthlayer, &cv_vr_hud,
	&cv_vr_hudscale, &cv_vr_huddist, &cv_vr_menudist, &cv_vr_hudlock,
	&cv_vr_diorama_scale, &cv_vr_diorama_dist, &cv_vr_diorama_height,
};

static void M_VrResetDefaults(INT32 choice)
{
	size_t i;

	(void)choice;

	for (i = 0; i < sizeof (vr_option_cvars) / sizeof (vr_option_cvars[0]); i++)
		CV_Set(vr_option_cvars[i], vr_option_cvars[i]->defaultvalue);
}

// Sections run alphabetically - Diorama, Driving, HUD, Performance, Picture,
// View - with Reset always last so it can't be tripped over while browsing.
menuitem_t OPTIONS_VideoVR[] =
{
	{IT_HEADER, "Diorama...", NULL,
		NULL, {NULL}, 0, 0},

	{IT_STRING | IT_CVAR, "Diorama Scale", "Units per meter on the tabletop. Higher shrinks the world.",
		NULL, {.cvar = &cv_vr_diorama_scale}, 0, 0},

	{IT_STRING | IT_CVAR, "Diorama Distance", "How far away the tabletop sits, in centimeters.",
		NULL, {.cvar = &cv_vr_diorama_dist}, 0, 0},

	{IT_STRING | IT_CVAR, "Diorama Height", "Tabletop height above or below eye level, in centimeters.",
		NULL, {.cvar = &cv_vr_diorama_height}, 0, 0},


	{IT_HEADER, "Driving...", NULL,
		NULL, {NULL}, 0, 0},

	{IT_STRING | IT_CVAR, "Handling", "Grip versus slide. Higher holds your line, lower lets the kart drift wide.",
		NULL, {.cvar = &cv_handling}, 0, 0},

	{IT_STRING | IT_CVAR, "Drift Grip", "How much the kart holds on while drifting. Higher is less slidey.",
		NULL, {.cvar = &cv_drifthandling}, 0, 0},

	{IT_STRING | IT_CVAR, "Landing Dash", "Hit gas as you land from a respawn for a boost.",
		NULL, {.cvar = &cv_landingdash}, 0, 0},

	{IT_STRING | IT_CVAR, "Trick Assist", "Tricks in any air, without letting go of gas. Buttons Only: the drift button, straight forward.",
		NULL, {.cvar = &cv_trickassist}, 0, 0},

	{IT_STRING | IT_CVAR, "Hazardous Surfaces", "Lava burns you, and takes you out once your rings are gone.",
		NULL, {.cvar = &cv_hazardsurfaces}, 0, 0},

	{IT_STRING | IT_CVAR, "Hazards Tumble You", "Burning throws you into a tumble instead of a spinout.",
		NULL, {.cvar = &cv_hazardtumble}, 0, 0},


	{IT_HEADER, "HUD...", NULL,
		NULL, {NULL}, 0, 0},

	{IT_STRING | IT_CVAR, "HUD Display", "Everything, or just the item box to keep the view clear.",
		NULL, {.cvar = &cv_vr_hud}, 0, 0},

	{IT_STRING | IT_CVAR, "HUD Scale", "Physical size of the HUD panel. Takes effect once you resume.",
		NULL, {.cvar = &cv_vr_hudscale}, 0, 0},

	{IT_STRING | IT_CVAR, "HUD Distance", "How far the HUD floats, in centimeters. Nearer reads bigger.",
		NULL, {.cvar = &cv_vr_huddist}, 0, 0},

	{IT_STRING | IT_CVAR, "Menu Distance", "How far the pause menu screen floats, in centimeters.",
		NULL, {.cvar = &cv_vr_menudist}, 0, 0},

	{IT_STRING | IT_CVAR, "HUD Anchor", "World parks the HUD in your space. Head glues it to your view.",
		NULL, {.cvar = &cv_vr_hudlock}, 0, 0},


	{IT_HEADER, "Performance...", NULL,
		NULL, {NULL}, 0, 0},

	{IT_STRING | IT_CVAR, "Render Scale", "Per-eye resolution scale. Lower it if frames drop.",
		NULL, {.cvar = &cv_vr_renderscale}, 0, 0},

	{IT_STRING | IT_CVAR, "Anti-Aliasing", "Smooths jagged edges in the headset. Costs GPU time.",
		NULL, {.cvar = &cv_vr_msaa}, 0, 0},

	{IT_STRING | IT_CVAR, "Desktop Mirror", "How often the monitor window redraws. Off is worth about double the frame rate.",
		NULL, {.cvar = &cv_vr_mirror}, 0, 0},

	{IT_STRING | IT_CVAR, "Depth Reprojection", "Sends depth to the headset so filled-in frames keep near things steady.",
		NULL, {.cvar = &cv_vr_depthlayer}, 0, 0},


	{IT_HEADER, "Picture...", NULL,
		NULL, {NULL}, 0, 0},

	{IT_STRING | IT_CVAR, "Screen Effect", "The picture filter on the HUD, the menus, and the world below.",
		NULL, {.cvar = &cv_scr_effect}, 0, 0},

	// The headset's own dial. Every VR consumer - the eye pass and the
	// HUD/panel blit - reads vr_effectstrength while a session is live; the
	// flat game's scr_effectstrength lives on the flat Video page. This row
	// pointed at the flat dial once, which a VR session ignores entirely, so
	// the slider read as dead in the headset.
	{IT_STRING | IT_CVAR, "CRT Strength", "How much of the CRT look lands. Only the CRT effects listen.",
		NULL, {.cvar = &cv_vr_effectstrength}, 0, 0},

	{IT_STRING | IT_CVAR, "Screen Effect in VR", "Run that effect on the world too, not just the HUD.",
		NULL, {.cvar = &cv_vr_worldeffect}, 0, 0},

	{IT_STRING | IT_CVAR, "Texture Supersampling", "Extra texture samples per pixel. Stops distant pixel art from crawling.",
		NULL, {.cvar = &cv_glsupersample}, 0, 0},


	{IT_HEADER, "View...", NULL,
		NULL, {NULL}, 0, 0},

	{IT_STRING | IT_CVAR, "View Mode", "Third person, first person, diorama - or the theater screen, only from here.",
		NULL, {.cvar = &cv_vr_viewmode}, 0, 0},

	{IT_STRING | IT_CVAR, "World Scale", "Game units per meter of head motion. Lower feels bigger.",
		NULL, {.cvar = &cv_vr_worldscale}, 0, 0},

	{IT_STRING | IT_CVAR, "Stereo Strength", "3D depth strength. Turn it down if your eyes strain.",
		NULL, {.cvar = &cv_vr_stereo}, 0, 0},

	{IT_STRING | IT_CVAR, "Head Motion", "How much leaning moves the camera. Turn it down for comfort.",
		NULL, {.cvar = &cv_vr_headscale}, 0, 0},

	{IT_STRING | IT_CVAR, "Horizon Lock", "First person: keep the world level on ramps and banked turns.",
		NULL, {.cvar = &cv_vr_horizonlock}, 0, 0},

	{IT_STRING | IT_CVAR, "Motion Feel", "First person: how much of the kart's own spin, tumble and tricks you feel. Full lets a trick roll you all the way over.",
		NULL, {.cvar = &cv_vr_immersion}, 0, 0},

	{IT_STRING | IT_CVAR, "FP Eye Height", "First person: how high the driver's head sits above the kart.",
		NULL, {.cvar = &cv_vr_fp_eyeheight}, 0, 0},

	{IT_STRING | IT_CVAR, "FP Camera Forward", "First person: slide your seat toward the kart's nose, or back off it.",
		NULL, {.cvar = &cv_vr_fp_forward}, 0, 0},

	{IT_STRING | IT_CVAR, "FP Own Shadow", "First person: draw your own kart's ground shadow under you, with no kart above it.",
		NULL, {.cvar = &cv_vr_fp_shadow}, 0, 0},

	{IT_STRING | IT_CVAR, "Shadow Motion", "Off draws shadows exactly the way the pause menu does: stepping with the tic, not gliding.",
		NULL, {.cvar = &cv_vr_shadowinterp}, 0, 0},

	{IT_STRING | IT_CVAR, "Shadow Lift", "How far shadows float above the ground. Higher stops flicker, lower stops them sliding as you drive.",
		NULL, {.cvar = &cv_vr_shadowlift}, 0, 0},

	{IT_STRING | IT_CVAR, "Shadow Test", "Plain strips the texture off shadows; Flat Parity draws them exactly as the flatscreen game does.",
		NULL, {.cvar = &cv_vr_shadowtest}, 0, 0},

	{IT_STRING | IT_CVAR, "TP Camera Forward", "Third person: pull the camera in toward the kart - keep going to sit inside it.",
		NULL, {.cvar = &cv_vr_tp_forward}, 0, 0},

	{IT_STRING | IT_CVAR, "TP Camera Height", "Third person: raise or lower where you sit, in centimeters. The view stays level.",
		NULL, {.cvar = &cv_vr_tp_eyeheight}, 0, 0},

	{IT_STRING | IT_CALL, "Recenter View", "Face forward, then select this to reset the view's center.",
		NULL, {.routine = M_VrRecenter}, 0, 0},


	{IT_HEADER, "Reset...", NULL,
		NULL, {NULL}, 0, 0},

	{IT_STRING | IT_CALL, "Reset to Defaults", "Set every VR option back to its default value.",
		NULL, {.routine = M_VrResetDefaults}, 0, 0},
};

menu_t OPTIONS_VideoVRDef = {
	sizeof (OPTIONS_VideoVR) / sizeof (menuitem_t),
	&OPTIONS_VideoDef,
	0,
	OPTIONS_VideoVR,
	48, 80,
	SKINCOLOR_PLAGUE, 0,
	MBF_DRAWBGWHILEPLAYING,
	NULL,
	2, 5,
	M_DrawGenericOptions,
	M_DrawOptionsCogs,
	M_OptionsTick,
	NULL,
	NULL,
	NULL,
};
