// SRB2 OpenXR VR support -- public API. No OpenXR headers leak out of i_openxr.c.
#ifndef __I_OPENXR_H__
#define __I_OPENXR_H__

#include "../doomtype.h"
#include "../command.h" // consvar_t
#include "../d_event.h" // event_t (gamepad responder)

#ifdef __cplusplus
extern "C" {
#endif

// vr_mode: 0 = off (flatscreen), 1 = OpenXR VR, 2 = stereo preview (no HMD)
extern consvar_t cv_vrmode;

// VR view modes (vr_viewmode cvar; right-stick click cycles in-game).
enum
{
	VR_VIEW_THIRD_PERSON = 0, // game chase cam, life-size stereo (default)
	VR_VIEW_FIRST_PERSON = 1, // chase cam off, eye at the player's view
	VR_VIEW_THEATER      = 2, // the flat game frame on the floating screen
	VR_VIEW_DIORAMA      = 3, // world miniaturized (scaled head motion + IPD)
};
extern consvar_t cv_vrviewmode;

void VR_CycleViewMode(void); // next in-game view mode (skips Theater -- menu-only)

// Movement/turn comfort, read where the game builds the local ticcmd. Both
// answer "stock behaviour" (0 / false) whenever VR is off, the eyes aren't
// showing the world, or the option is Off, so the call sites need no gate.
float   VR_MoveYaw(void);            // degrees to rotate the move stick by (0 = body-relative)
boolean VR_SnapTurnActive(void);     // snap turning owns the look yaw: the smooth turn stands down
INT16   VR_SnapTurnStep(INT32 lookaxis); // one latched step's angleturn delta (0 while held or centered)

// Gamepad shortcut, called from D_ProcessEvents before other responders:
// Select (Back) or D-pad Up cycles the VR view in play. Returns true when the
// event was consumed; always false outside a live VR session.
boolean VR_GamepadResponder(event_t *ev);

void VR_RegisterCvars(void);   // register cvars (call during cvar init)
void VR_Init(void);            // boot OpenXR; safe no-op if VR is off or unavailable
void VR_Shutdown(void);        // tear down OpenXR; safe to call always
boolean VR_IsActive(void);     // true only when a live OpenXR session is running
boolean VR_HMDPresent(void);   // one-time headset probe (no GL needed; cached)
boolean VR_WantVR(void);       // vr_mode resolved: VR, or Auto with a headset present
boolean VR_WorldVisible(void); // the 3D world surrounds the player (level or title flyby)
boolean VR_TheaterActive(void); // Theater view mode live in a level: D_Display keeps the vanilla flat draw
float   VR_WorldScaleMul(void); // view-mode multiplier on vr_scale (Diorama shrinks the world)
void    VR_DioramaPark(float *backM, float *upM); // Diorama camera pull-back/raise (meters; zeros otherwise)

void VR_BeginFrame(void);      // xrWaitFrame/BeginFrame + pose update; no-op when inactive
void VR_EndFrame(void);        // xrEndFrame (no layers in Phase 0); no-op when inactive

// Floating UI screen: menus/title/console/HUD on a world-locked quad layer.
// Begin/End bracket the in-level 2D overlay phase in D_Display (redirects it into a
// transparent buffer, then puts it back on the desktop window so the mirror is unchanged).
// Capture runs right before the window swap and feeds the quad. All no-ops when inactive.
void VR_UIOverlayBegin(void);
void VR_UIOverlayEnd(void);
void VR_CaptureUIFrame(void);
void VR_DrawDesktopMirror(void); // in-level: window shows the headset view (recording); no-op when inactive
boolean VR_MirrorWorldLayer(void); // in-level: blit the mirror as the frame's world layer, replacing the flat world render
void VR_RecenterScreen(void);  // re-place the screen in front of the current head pose

// Controller rumble, both hands. Strength 0..1, duration in seconds; the burst
// is re-armed per frame while it lasts. Safe no-ops without VR or controllers.
void VR_Rumble(float strength, float seconds);
void VR_RumbleStop(void);

// VR render state, read by the GL renderer (hw_main/r_opengl) while an eye is rendering.
// g_vrActive is true only between per-eye setup and that eye's world render.
extern boolean g_vrActive;
extern INT32 g_vrEyeIndex; // eye being rendered while g_vrActive (0 = left, first)
extern boolean g_vrWaterMuffle; // the view is underwater: the audio mixer low-passes the mix
extern float   g_vrFov[4];     // current eye FOV angles (rad): left, right, up, down
extern float   g_vrYaw, g_vrPitch; // head yaw/pitch (degrees) fed into SRB2's camera
extern float   g_vrStickPitch;     // opt-in stick-look pitch offset (degrees, +up; 0 unless vr_sticklook)
extern float   g_vrEyeOff[3];      // eye position vs the recenter anchor (meters): lean/step + IPD
extern consvar_t cv_vrscale;   // SRB2 world units per meter (live-tunable world scale)

// Floating screen tuning (distances/sizes in real-world meters).
extern consvar_t cv_vrscreendist;  // screen distance from the head at (re)center
extern consvar_t cv_vrscreensize;  // screen width; height follows the capture aspect
extern consvar_t cv_vrhud;         // show the in-level HUD on the screen
extern consvar_t cv_vrhudalpha;    // in-level overlay opacity, 0-10
extern consvar_t cv_vrcrosshair;   // opt back in to the crosshair in VR (default off)
extern consvar_t cv_vrmirror;      // desktop window follows the headset in-level (default on)
extern consvar_t cv_vrmsaa;        // eye-render anti-aliasing: Off / 2x / 4x / 8x
extern consvar_t cv_vrrenderscale; // eye render resolution, % of the swapchain (supersample > 100)
extern consvar_t cv_vrculling;     // angle culling in VR (default Off: the clipper mis-culls on lean)
extern consvar_t cv_vrunlockall;   // opt-in: everything unlocked, in memory only (medals/records pause; saves still work)
extern consvar_t cv_vrwateratmo;   // underwater haze + veil in the eyes (default on)
extern consvar_t cv_vrbubbles;     // ambient bubbles around the view underwater, single-player (default on)
extern consvar_t cv_vrwatermuffle; // low-pass the audio mix while the view is underwater (default on)
extern consvar_t cv_vrwatersway;   // underwater world sway strength, 0-10 (default 5)
extern consvar_t cv_vrripple;      // water-surface ripple boost in VR, 0-10 (default 5)
extern consvar_t cv_vrwaterfog;    // underwater absorption fog strength, 0-10 (default 5)
extern consvar_t cv_vrcaustics;    // caustic dapple strength, 0-10 (default 5)
extern consvar_t cv_vrwaterdebug;  // trace per-eye water state to the console
extern consvar_t cv_vrgodrays;     // god-ray shaft strength, 0-10 (default 5)
extern consvar_t cv_vrsticklook;   // opt-in right-stick pitch glance (default off)
extern consvar_t cv_vrheadmove;    // move where you look: Off / First Person / All Views
extern consvar_t cv_vrsnapturn;    // turn in fixed steps instead of sweeping: Off / 30 / 45 / 90
extern consvar_t cv_vrmenudim;     // menu backdrop dim strength in VR, 0-16 (default 4)
extern consvar_t cv_vrstereo;      // stereo depth, % of real eye separation (0 = flat)
extern consvar_t cv_vrheadscale;   // head-motion damping, % of real head translation
extern consvar_t cv_vrthirddist;    // Third Person: chase distance, % of the game's own (100 = stock)
extern consvar_t cv_vrdioramascale; // Diorama: multiplier on vr_scale (bigger = smaller world)
extern consvar_t cv_vrdioramadist;  // Diorama: camera pull-back, meters
extern consvar_t cv_vrdioramaheight; // Diorama: camera raise (miniature sits below eyes), meters

#ifdef __cplusplus
}
#endif

#endif // __I_OPENXR_H__
