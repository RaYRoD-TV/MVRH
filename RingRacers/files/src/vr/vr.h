// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vr.h
/// \brief OpenXR VR support.
///
/// Public interface is platform-agnostic and C-linkage so it can be called
/// from both the C and C++ sides of the engine. No OpenXR or GL types leak
/// out of this header. When ENABLE_VR is not defined the whole surface
/// collapses to inline no-ops, so call sites never need their own #ifdef.

#ifndef __VR_VR_H__
#define __VR_VR_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- view modes (shared types) ------------------------------------------------

// VR view modes (vr_viewmode console command; right-stick click cycles). The
// eye-matrix builder and D_Display's render routing branch on this.
typedef enum
{
	VR_VIEW_THIRD_PERSON = 0, // game chase cam, life-size stereo (default)
	VR_VIEW_FIRST_PERSON = 1, // eye at the kart driver's head, life-size stereo
	VR_VIEW_THEATER      = 2, // flat game frame on the big head-locked screen
	VR_VIEW_DIORAMA      = 3, // world shrunk to a tabletop anchored in front of you
} VrViewMode;

// --- motion controllers (shared types) ---------------------------------------

// Controller button mask bits, as reported by vr_controller_buttons(). The
// trigger and grip bits are digital latches over the analog inputs (press
// past 60%, release under 40%) so a resting finger can't flicker them.
enum
{
	VR_BTN_A        = 1<<0,
	VR_BTN_B        = 1<<1,
	VR_BTN_X        = 1<<2,
	VR_BTN_Y        = 1<<3,
	VR_BTN_MENU     = 1<<4,
	VR_BTN_LSTICK   = 1<<5,
	VR_BTN_RSTICK   = 1<<6,
	VR_BTN_LTRIGGER = 1<<7,
	VR_BTN_RTRIGGER = 1<<8,
	VR_BTN_LGRIP    = 1<<9,
	VR_BTN_RGRIP    = 1<<10,
};

// Device id the synthetic VR gamepad uses in game events. INT32_MAX cannot
// collide with a real pad: the SDL layer explicitly drops any gamepad event
// whose device id would compute to this value (see i_video.cpp), and real
// SDL joystick instance ids are small incrementing integers besides.
#define VR_GAMEPAD_DEVICE_ID 0x7fffffff

// First-person drama classes. The class decides the SHAPE the module gives the
// motion; the game only says which one is playing. Defined outside the
// ENABLE_VR guard because the feed sites compile on every platform.
#define VR_DRAMA_NONE       0
#define VR_DRAMA_SPIN       1 // spinout / frozen / carried: thrown into a flat spin
#define VR_DRAMA_TUMBLE     2 // end over end, with the barrel roll under it
#define VR_DRAMA_FAULT      3 // false start: a jolt, not a wreck
#define VR_DRAMA_TRICK_ROLL 4 // a side trick: a barrel roll you pressed for
#define VR_DRAMA_TRICK_FLIP 5 // a forward or back trick: a flip you pressed for

#ifdef ENABLE_VR

// Requested via the -vr command line flag (or auto-enabled when a headset is
// detected at startup). Requested is not active: activation happens once an
// OpenXR session is live.
void vr_request_enable(void);
bool vr_is_requested(void);

// Requested via -vrfake: the full stereo eye loop against a synthesized head
// pose, no OpenXR runtime or headset involved. Exists so stereo geometry and
// eye-pass cost can be measured on a desk; nothing is ever submitted anywhere.
void vr_request_fake(void);

// -vrfakepitch <deg>: tilt the stand-in head DOWN by this many degrees. Ground
// art (shadows, splats, decals) is judged by a player looking at their own
// feet; a rig locked level can never render what they are complaining about.
void vr_set_fake_pitch(float degrees_down);

// Lightweight startup probe: is a VR headset actually connected right now?
// Creates and tears down a throwaway OpenXR instance (no GL context needed)
// and asks for an HMD system. Lets the same exe auto-enable VR when a headset
// is present and stay flat otherwise.
bool vr_headset_present(void);

// Is an OpenXR session live and actively rendering? Everything VR-specific in
// the renderer and game is gated on this; false == stock flat game.
bool vr_is_active(void);

// --- per-frame loop ---------------------------------------------------------

// Run once per rendered frame, before any VR output for that frame. Lazy-boots
// OpenXR on first call (the GL context must be current on this thread), pumps
// the session-state machine, then xrWaitFrame/xrBeginFrame and locates the
// per-eye views. Cheap no-op until VR is requested; xrWaitFrame paces the loop
// at the headset refresh once the session runs.
void vr_begin_frame(void);

// Stereo view info (valid once the session is up; 0 before).
int vr_eye_count(void);
int vr_eye_width(int eye);
int vr_eye_height(int eye);

// Theater panel: blit an already-rendered GL texture (the finished flat game
// frame, sized w x h) into the panel swapchain; vr_submit then shows it on a
// large head-locked quad. Returns false if this frame can't take it.
bool vr_submit_panel_texture(unsigned int glTex, int w, int h);

// Direct-render variant: acquire + bind the panel swapchain image as the
// active GL render target (cleared, viewport set). vr_end_panel releases it.
bool vr_begin_panel(void);
void vr_end_panel(void);
void vr_set_panel_mode(bool on);

// Gameplay HUD overlay: blit the finished 2D frame (the game backbuffer,
// drawn on transparent black so only the HUD/menus carry alpha) into the HUD
// swapchain; vr_submit composites it as an alpha-blended head-locked quad
// above the stereo world. Only takes on frames that rendered eyes.
bool vr_submit_hud_texture(unsigned int glTex, int w, int h);

// True once this frame's eye loop submitted at least one eye: the frame is a
// stereo frame, the flat composite/panel paths must stand down.
bool vr_frame_has_eyes(void);

// xrEndFrame: submit this frame's layers (panel quad, and the stereo
// projection layer once eye rendering exists) to the compositor.
void vr_submit(void);

// --- stereo eye rendering -----------------------------------------------------

// True while D_Display is inside the per-eye render loop; the GL renderer
// substitutes the eye matrices below whenever this is set.
bool vr_stereo_active(void);
int  vr_current_eye(void);

// Is the eye pass rendering into a MULTISAMPLED target right now? The GL
// backend asks before spending samples on alpha-to-coverage: with one sample
// there is no coverage to give, so the cutout would simply lose its threshold.
bool vr_eye_msaa_active(void);

// Column-major, ready for glLoadMatrixf: the eye's NATIVE asymmetric frustum
// from the runtime (never symmetrized - canted-display headsets report
// strongly asymmetric per-eye fovs and forcing them symmetric cross-eyes the
// stereo pair). Clip planes are in game units, matching the flat renderer's.
const float* vr_gl_eye_projection(int eye);

// Column-major modelview PRE-multiply (camera-space -> eye-space): the rigid
// inverse of the head pose relative to the recentered origin, translation in
// game units. While vr_mono_sky(true) is in effect this returns a
// rotation-only variant for the current sky draw, so the sky renders at
// infinity (no IPD on the sky = no depth conflict).
const float* vr_gl_eye_view(int eye);

// World scale: game units per meter of head motion (default 32). Set by the
// vr_worldscale cvar.
float vr_units_per_meter(void);

// Horizontal full-angle (degrees) the BSP culling wedge must cover in VR: the
// widest eye frustum plus the head's current free-look offset, with margin.
// 360 means "cull nothing" (e.g. looking steeply up or down).
float vr_cull_fov_deg(void);

// Begin one eye's render: binds and clears the VR mono render target (sized
// eye swapchain x vr_renderscale, optionally multisampled) over the RHI's
// framebuffer. Returns false when this frame can't render eyes; skip the eye.
bool vr_begin_eye(int eye);

// End one eye's render: resolve MSAA if on, blit the mono target into the eye
// swapchain image (1:1 at full render scale, linear-filtered otherwise),
// record the projection-layer view for vr_submit, and restore the framebuffer
// binding vr_begin_eye replaced.
void vr_end_eye(int eye);

// Raster size of the in-progress eye pass (the VR mono target). While
// vr_stereo_active() the GL backend must size its viewport from these instead
// of the vid.width/height rect its callers computed for the desktop window.
int vr_eye_raster_width(void);
int vr_eye_raster_height(void);

// Sky pass marker: while true the eye view drops the head translation so the
// sky renders at infinity (no IPD on the sky = no depth conflict).
void vr_mono_sky(bool in_sky);

// --- settings (pushed by the cv_vr_* cvars) --------------------------------------
//
// The cvars (declared in cvars.cpp, edited from the VR Options menu, saved to
// config) are the persistent interface; their onchange handlers in vr_cvars.c
// push values here and the module applies them live. The module never reads a
// cvar itself - except vr_viewmode, which it stealth-updates so the menu and
// config always show the live mode (the right-stick cycle changes it too).

// Queue a recenter: the current head yaw/position becomes the neutral origin
// on the next frame. Also the vr_recenter console command / menu item.
void vr_recenter(void);

void vr_set_world_scale(float unitsPerMeter); // 1..4096, default 32
void vr_set_render_scale(float scale);        // 0.4..1.5, default 1 (of eye swapchain)
void vr_set_msaa(int samples);                // 0 / 2 / 4, default 4
void vr_set_stereo_strength(float frac);      // 0..1 IPD scale, default 1
// Distance in game units to the nearest thing permanently on screen (third
// person: your own kart). The separation stands down when that gets close
// enough for a full IPD to cross the eyes. 0 = no limit.
void vr_set_focus_distance(float units);

// The Video "Screen Effect" preset (a cv_scr_effect value), pushed once per
// frame, and whether it also runs on the 3D view. The flat 2D layer - theater
// panel, HUD and menu quads - always gets it from the RHI before the frame
// reaches this module; these two decide the per-eye pass.
void vr_set_screen_effect(int effect);
void vr_set_world_effect(bool on);
void vr_set_effect_strength(float frac); // 0..1 CRT strength (1 = full look)

// Radians the composed eye view adds on top of the game camera's yaw (first
// person's kart heading plus the head's own free-look, one shared value for
// both eyes) - i.e. which way the eyes are POINTING. The renderer's
// "is this behind the view plane" test needs it, because that test is about a
// direction. Turning an object to face the viewer is a different question
// answered from the eye's POSITION (see vr_eye_world_offset), so nothing
// billboards off this any more.
float vr_billboard_yaw_delta(void);
void vr_set_head_motion_scale(float frac);    // 0..1 6DoF damping, default 1
void vr_set_horizon_lock(bool on);            // FP: world stays level (default on)
// FP: how much of the kart's own motion reaches the eye. 0 = off (the original
// drama chase and nothing else), 1 = light, 2 = full. Horizon lock is separate:
// that decides whether the WORLD tilts, this decides how much of the kart the
// DRIVER feels.
void vr_set_immersion(int level);
// FP: a launch-class boost just landed. 0..1, spikes on the tic it fires and
// falls away over about a third of a second; the module turns it into a head
// snap so the shove is felt in the neck rather than only seen on the screen.
void vr_set_boost_kick(float amount);
// Diagnostic: white lines at exact per-eye view angles, drawn straight into
// the eye buffers past the whole game renderer. Full height = infinity (must
// fuse), upper half = 2 m, lower half = 10 m. The vr_calib cvar drives it.
void vr_set_calib(bool on);
void vr_set_depth_layer(bool on);             // submit per-eye depth for reprojection
void vr_set_fp_eyeheight(float units);        // head height above the kart origin
void vr_set_fp_forward(float units);          // seat slide along the kart's heading
void vr_set_tp_eyeheight(float meters);       // third person: eye lift above the chase cam
void vr_set_diorama_scale(float unitsPerMeter);
void vr_set_diorama_dist(float meters);
void vr_set_diorama_height(float meters);

// Diorama wall clearance. The eye parks well behind the chase camera, and
// near walls that spot can land inside geometry. vr_diorama_eye_offset
// reports the world-space offset (SRB2 coords, game units) from the last-fed
// game view to where the knobs would park the eye (false outside diorama
// mode); the renderer traces that segment against the level and feeds back
// the fraction of it that is clear. The module slides the eye in along the
// segment - instantly inward so it never clips, eased back out.
bool vr_diorama_eye_offset(float outOffset[3]);
void vr_set_diorama_clearance(float frac);

// Where the eyes actually are, as a world-space offset (SRB2 coords, game
// units) from the camera the renderer is drawing FROM. False when there is
// nothing to correct: no stereo frame, or third person, where the eyes ARE
// the camera give or take an IPD. First person puts them a whole chase
// length up the track; the diorama parks them behind and above.
//
// Every renderer test written against the camera is wrong by exactly this
// much - what a wall hides, which way a viewer-facing model turns, how far
// away a thing is. Read it there instead of assuming the camera is the eye.
bool vr_eye_world_offset(float outOffset[3]);
void vr_set_hud_scale(float frac);            // 0.1..2.5 of the 2.5 m reference width
void vr_set_hud_dist(float meters);           // 1..5 m (physical size stays; farther reads smaller)
void vr_set_menu_dist(float meters);          // 1..5 m, the pause menu screen's own distance
void vr_set_hud_world(bool on);               // park the HUD in room space (default) vs head-lock

// True while a menu is up this frame: the HUD quad becomes a fixed-size
// menu screen anchored in the world where the player was looking (instead
// of the gameplay HUD), so pause/options layouts read at a real size and
// can be looked around. Fed per frame alongside the HUD submit.
void vr_set_hud_menu(bool menuUp);

// True while the in-progress eye draw should dim the world: a menu quad is
// composited over the stereo scene, and undimmed geometry near the quad's
// depth fights the menu for the eyes (reads as double vision). The GL
// renderer's post pass draws the dim.
bool vr_menu_dim_active(void);

// Mirror the last rendered eye into the currently bound draw framebuffer
// (the desktop window), center-cropped to dstW x dstH's aspect. Stereo
// frames leave the window's own frame empty, so I_FinishUpdate calls this
// inside the window render pass; the 2D backbuffer then alpha-blends the
// HUD over it. False when no eye was rendered this frame. Leaves both
// framebuffer bindings on 0 (the window pass's own target).
bool vr_blit_mirror(int dstW, int dstH);

// --- view modes -----------------------------------------------------------------

// Current VrViewMode. Setting it is clamped to the enum range; switching INTO
// first person is refused while the switch lock (below) holds. The vr_viewmode
// cvar is kept in step on every path through here.
int  vr_get_view_mode(void);
void vr_set_view_mode(int mode);

// Right-stick click: advance to the next mode. Theater is skipped entirely -
// it's a menu-only choice - and first person is skipped, not blocked, while
// the switch lock holds.
void vr_cycle_view_mode(void);

// Fed by the game every main-loop iteration: true while the pre-race
// intro/countdown runs, so cycling INTO first person mid-countdown is blocked
// (staying in it is always fine).
void vr_set_fp_switch_locked(bool locked);

// First person feeds, once per rendered frame (see D_Display / hw_main):
//
// The local kart's interpolated head pose in world space (SRB2 coords, game
// units; z is height, angles in radians CCW). The eye anchors here.
void vr_set_cockpit_pose(const float posWorld[3], float yawRad, float pitchRad, float rollRad);

// The exact interpolated view the render pass below will feed the transform
// chain (same coords/units as above). skyPass marks the skybox-viewpoint
// render, whose miniature space only takes the rotation half of the
// first-person compose.
void vr_set_game_view(const float posWorld[3], float yawRad, float pitchRad, float rollRad, bool skyPass);

// How far the skybox viewpoint moves per unit of camera movement (see
// R_SkyboxFrame: 1/skybox_scalex, or 0 with no centerpoint). Eye/head
// translation is scaled by this during skyPass renders so the miniature
// skybox world gets parallax matching the huge distance it stands in for -
// full-size IPD in there reads as hyperstereo and cross-eyes background
// props. Fed by the renderer right before each skybox pass.
void vr_set_sky_parallax_scale(float scale);

// Spinout / tumble / trick drama, fed once per rendered frame.
//
//   kind - VR_DRAMA_* above; NONE ends it and returns the eye to level.
//   spin - the kart's spin away from its facing, binary angle (0x10000 = a
//          full turn), wrapped to +/-180.
//   roll - the kart's sprite roll, same units.
//
// The raw angles run to 4725 deg/s, which is not something an eye can be
// carried through, so for the wreck classes they only decide DIRECTION: the
// module holds a bounded lean for as long as the state lasts. A TRICK is the
// exception - the player pressed a button for it, so at Full immersion it
// passes through whole. Feed VR_DRAMA_NONE when nothing is playing.
void vr_set_drama(int kind, short spinBinary, short rollBinary);
void vr_set_action_cam(int dramatic, int finished);

// Game units above the kart origin where the driver's head sits (the
// vr_fp_eyeheight console knob; the cockpit-pose feed applies it, scaled by
// the kart's own scale).
float vr_fp_eyeheight_units(void);

// Game units the driver's seat slides along the kart's heading (the
// vr_fp_forward console knob; applied by the same cockpit-pose feed, and by
// the flat first-person camera). Positive is toward the nose.
float vr_fp_forward_units(void);

// True while the in-progress eye pass should skip the local kart's own sprite
// (first person: the driver's body blocks the view). Finishing the race
// un-hides the kart for the post-race camera. The drop shadow always draws.
bool vr_fp_hide_player(void);

// True while the transform chain must not apply the camera's pitch/roll
// (first person + horizon lock: the HMD is the only source of tilt). Consulted
// where atransform/dometransform are built.
bool vr_horizon_locked(void);

// --- motion controllers -------------------------------------------------------

// True while the session is focused with the controller action set attached:
// the only state in which the runtime feeds us controller input.
bool vr_controllers_active(void);

// Current VR_BTN_* mask (0 when controllers aren't active).
unsigned vr_controller_buttons(void);

// Thumbstick state for one hand (0 = left, 1 = right), -1..1 each axis,
// +x right, +y up. Zeroed when controllers aren't active.
void vr_controller_stick(int hand, float out[2]);

// Arm haptic rumble on both hands at the given strength (0..1) for the given
// duration. The VR layer re-arms short bursts each frame while armed, so a
// runtime that drops stop requests (common over wireless) can't strand the
// motors buzzing. Calling again re-times and re-scales the rumble.
void vr_controller_rumble(float strength, float seconds);
void vr_controller_rumble_stop(void);

// Synthetic gamepad bridge: mirrors the controller state above into the
// game's own event queue (device added / key up-down / axis events) once per
// frame, so the OpenXR controllers look like a normal pad to every consumer.
// Call once per main-loop iteration, before events are processed.
void vr_input_bridge_update(void);

// Tear down all OpenXR state. Safe to call when VR never started.
void vr_shutdown(void);

#else // ENABLE_VR

// Non-VR build: the same surface as inline no-ops.
static inline void vr_request_enable(void) {}
static inline void vr_request_fake(void) {}
static inline void vr_set_fake_pitch(float degrees_down) { (void)degrees_down; }
static inline bool vr_is_requested(void) { return false; }
static inline bool vr_headset_present(void) { return false; }
static inline bool vr_is_active(void) { return false; }
static inline void vr_begin_frame(void) {}
static inline int vr_eye_count(void) { return 0; }
static inline int vr_eye_width(int eye) { (void)eye; return 0; }
static inline bool vr_eye_msaa_active(void) { return false; }
static inline int vr_eye_height(int eye) { (void)eye; return 0; }
static inline bool vr_submit_panel_texture(unsigned int glTex, int w, int h) { (void)glTex; (void)w; (void)h; return false; }
static inline bool vr_begin_panel(void) { return false; }
static inline void vr_end_panel(void) {}
static inline void vr_set_panel_mode(bool on) { (void)on; }
static inline bool vr_submit_hud_texture(unsigned int glTex, int w, int h) { (void)glTex; (void)w; (void)h; return false; }
static inline bool vr_frame_has_eyes(void) { return false; }
static inline void vr_submit(void) {}
static inline void vr_recenter(void) {}
static inline void vr_set_world_scale(float unitsPerMeter) { (void)unitsPerMeter; }
static inline void vr_set_render_scale(float scale) { (void)scale; }
static inline void vr_set_msaa(int samples) { (void)samples; }
static inline void vr_set_stereo_strength(float frac) { (void)frac; }
static inline void vr_set_focus_distance(float units) { (void)units; }
static inline void vr_set_screen_effect(int effect) { (void)effect; }
static inline void vr_set_world_effect(bool on) { (void)on; }
static inline void vr_set_effect_strength(float frac) { (void)frac; }
static inline float vr_billboard_yaw_delta(void) { return 0.0f; }
static inline void vr_set_head_motion_scale(float frac) { (void)frac; }
static inline void vr_set_horizon_lock(bool on) { (void)on; }
static inline void vr_set_immersion(int level) { (void)level; }
static inline void vr_set_boost_kick(float amount) { (void)amount; }
static inline void vr_set_calib(bool on) { (void)on; }
static inline void vr_set_depth_layer(bool on) { (void)on; }
static inline void vr_set_fp_eyeheight(float units) { (void)units; }
static inline void vr_set_fp_forward(float units) { (void)units; }
static inline void vr_set_tp_eyeheight(float meters) { (void)meters; }
static inline void vr_set_diorama_scale(float unitsPerMeter) { (void)unitsPerMeter; }
static inline void vr_set_diorama_dist(float meters) { (void)meters; }
static inline void vr_set_diorama_height(float meters) { (void)meters; }
static inline bool vr_diorama_eye_offset(float outOffset[3]) { (void)outOffset; return false; }
static inline bool vr_eye_world_offset(float outOffset[3]) { (void)outOffset; return false; }
static inline void vr_set_diorama_clearance(float frac) { (void)frac; }
static inline void vr_set_hud_scale(float frac) { (void)frac; }
static inline void vr_set_hud_dist(float meters) { (void)meters; }
static inline void vr_set_menu_dist(float meters) { (void)meters; }
static inline void vr_set_hud_world(bool on) { (void)on; }
static inline void vr_set_hud_menu(bool menuUp) { (void)menuUp; }
static inline bool vr_menu_dim_active(void) { return false; }
static inline bool vr_blit_mirror(int dstW, int dstH) { (void)dstW; (void)dstH; return false; }
static inline bool vr_stereo_active(void) { return false; }
static inline int vr_current_eye(void) { return 0; }
static inline const float* vr_gl_eye_projection(int eye) { (void)eye; return 0; }
static inline const float* vr_gl_eye_view(int eye) { (void)eye; return 0; }
static inline float vr_units_per_meter(void) { return 32.0f; }
static inline float vr_cull_fov_deg(void) { return 0.0f; }
static inline bool vr_begin_eye(int eye) { (void)eye; return false; }
static inline void vr_end_eye(int eye) { (void)eye; }
static inline int vr_eye_raster_width(void) { return 0; }
static inline int vr_eye_raster_height(void) { return 0; }
static inline void vr_mono_sky(bool in_sky) { (void)in_sky; }
static inline int vr_get_view_mode(void) { return VR_VIEW_THIRD_PERSON; }
static inline void vr_set_view_mode(int mode) { (void)mode; }
static inline void vr_cycle_view_mode(void) {}
static inline void vr_set_fp_switch_locked(bool locked) { (void)locked; }
static inline void vr_set_cockpit_pose(const float posWorld[3], float yawRad, float pitchRad, float rollRad) { (void)posWorld; (void)yawRad; (void)pitchRad; (void)rollRad; }
static inline void vr_set_game_view(const float posWorld[3], float yawRad, float pitchRad, float rollRad, bool skyPass) { (void)posWorld; (void)yawRad; (void)pitchRad; (void)rollRad; (void)skyPass; }
static inline void vr_set_sky_parallax_scale(float scale) { (void)scale; }
static inline void vr_set_drama(int kind, short spinBinary, short rollBinary) { (void)kind; (void)spinBinary; (void)rollBinary; }
static inline void vr_set_action_cam(int dramatic, int finished) { (void)dramatic; (void)finished; }
static inline float vr_fp_eyeheight_units(void) { return 0.0f; }
static inline float vr_fp_forward_units(void) { return 0.0f; }
static inline bool vr_fp_hide_player(void) { return false; }
static inline bool vr_horizon_locked(void) { return false; }
static inline bool vr_controllers_active(void) { return false; }
static inline unsigned vr_controller_buttons(void) { return 0; }
static inline void vr_controller_stick(int hand, float out[2]) { (void)hand; out[0] = out[1] = 0.0f; }
static inline void vr_controller_rumble(float strength, float seconds) { (void)strength; (void)seconds; }
static inline void vr_controller_rumble_stop(void) {}
static inline void vr_input_bridge_update(void) {}
static inline void vr_shutdown(void) {}

#endif // ENABLE_VR

#ifdef __cplusplus
} // extern "C"
#endif

#endif // __VR_VR_H__
