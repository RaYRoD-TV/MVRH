/*
 * vr_flat_state.c -- the view modes, without a headset.
 *
 * The flat build compiles no OpenXR layer, and vr.h used to collapse this
 * whole group to inline no-ops: vr_get_view_mode() answered THIRD_PERSON
 * forever, vr_set_view_mode() and vr_cycle_view_mode() did nothing, and the
 * seat knobs all read zero. So on a monitor the D-pad cycled nothing and the
 * VIEW row moved nothing -- reported, correctly, as "the view modes stopped
 * working on flatscreen".
 *
 * They are real here instead. The flat view modes are a genuine port feature
 * (camera.c: mdkr_flat_view_mode and mdkr_flat_eye_compute build a first
 * person eye and a diorama eye from the game's own geometry), and they need
 * exactly this much state: which mode, and where the seat sits.
 *
 * Compiled ONLY when ENABLE_VR is absent, so the VR build keeps the layer's
 * own definitions and there is one owner of each symbol in each lane. The
 * defaults are kept in step with vr_openxr.cpp's initialisers by hand; they
 * are few, and a shared header for four numbers would cost more than it saves.
 */

#include "vr.h"

#ifndef ENABLE_VR

/* Theater is deliberately unreachable here: it is a screen floating in a
 * headset, and on a monitor it would be a screen inside a screen. The menu
 * hides the row and this cycle skips it, so neither route can land on it. */
static int   s_flatViewMode   = VR_VIEW_THIRD_PERSON;
/* 32 on a monitor, 26 in the headset (vr_openxr.cpp): the flat first person
 * has no head tracking to look down over the kart with, so the seat sits a
 * little higher to give the fixed camera the same view of the road. Both numbers were settled in visual tests. */
static float s_flatFpEyeHeight = 32.0f;
static float s_flatFpForward   = -12.0f;  /* negative = back; same seat as the headset default */
static float s_flatTpEyeHeight = 0.0f;
static float s_flatTpForward   = 0.0f;

int vr_get_view_mode(void) {
    return s_flatViewMode;
}

void vr_set_view_mode(int mode) {
    if (mode == VR_VIEW_THIRD_PERSON || mode == VR_VIEW_FIRST_PERSON ||
        mode == VR_VIEW_DIORAMA) {
        s_flatViewMode = mode;
    }
}

void vr_cycle_view_mode(void) {
    switch (s_flatViewMode) {
        case VR_VIEW_THIRD_PERSON: s_flatViewMode = VR_VIEW_FIRST_PERSON; break;
        case VR_VIEW_FIRST_PERSON: s_flatViewMode = VR_VIEW_DIORAMA;      break;
        default:                   s_flatViewMode = VR_VIEW_THIRD_PERSON; break;
    }
}

/* The same clamps the VR layer applies, so a value tuned on one lane cannot
 * be out of range on the other. */
static float flat_clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void  vr_set_fp_eyeheight(float units) { s_flatFpEyeHeight = flat_clamp(units, -64.0f, 256.0f); }
void  vr_set_fp_forward(float units)   { s_flatFpForward   = flat_clamp(units, -128.0f, 256.0f); }
float vr_fp_eyeheight_units(void)      { return s_flatFpEyeHeight; }
float vr_fp_forward_units(void)        { return s_flatFpForward; }

void  vr_set_tp_eyeheight(float units) { s_flatTpEyeHeight = flat_clamp(units, -64.0f, 256.0f); }
void  vr_set_tp_forward(float units)   { s_flatTpForward   = flat_clamp(units, -128.0f, 256.0f); }
float vr_tp_eyeheight_units(void)      { return s_flatTpEyeHeight; }
float vr_tp_forward_units(void)        { return s_flatTpForward; }

#endif /* !ENABLE_VR */
