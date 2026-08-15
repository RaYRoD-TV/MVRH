// SRB2 OpenXR VR support. All OpenXR state lives here.
#include "i_openxr.h"
#include "../console.h" // CONS_Printf, con_destlines (console visible -> show the UI screen)
#include "../screen.h" // viddef_t vid (eye buffer is sized to the desktop render)
#include "../m_fixed.h" // FixedToFloat (VR view matrix + vr_scale)
#include "../doomstat.h" // gamestate, menuactive, paused (UI screen visibility)
#include "../f_finale.h" // curbghide, hidetitlemap (title flyby renders in the headset)
#include "../d_event.h" // event_t (VR controller -> game events)
#include "../d_main.h"  // D_PostEvent
#include "../g_input.h" // KEY_JOY1 (gamepad button keycodes)
#include "../i_joy.h"   // JOYAXISRANGE (snap-turn deflection thresholds)
#include "../keys.h"    // KEY_ESCAPE
#include "../r_main.h"  // cv_chasecam (the view modes drive it), ps_vr_eyetime
#include "../i_system.h" // I_GetPreciseTime (the VR eye-render perfstats row)
#include "../g_game.h"  // players/displayplayer (viewpoint-cut detection for the stereo ramp)
#include "../m_menu.h"  // M_VROptionsShortcut, M_MenuSectionJump (controller menu hooks)
#include "../hu_stuff.h" // HU_DoCEcho (view-mode name on cycle)
#include <math.h>       // sinf / cosf (VR view matrix)
#include <stdlib.h>     // atof (vr_scale parsing)

// The synthetic stick axes must pass G_BuildTiccmd's usejoystick gate even with no
// physical pad plugged in (I_InitJoystick zeroes the cvar when no SDL device opens).
extern consvar_t cv_usejoystick;

// Defined in hw_main.c: renders the local player's 3D view into the bound FBO + viewport.
extern void HWR_RenderPlayerEye(void);
// Defined in hw_main.c: draws the finished frame (made by HWR_MakeScreenFinalTexture each
// present) stretched to the given size into the currently bound framebuffer.
extern void HWR_DrawScreenFinalTexture(int width, int height);
extern void HWR_DrawScreenFinalTextureNoEffect(int width, int height);
extern boolean HWR_VREyeEffectActive(void);
extern void HWR_DrawVREyeEffect(UINT32 tex, int width, int height);
// Defined in r_opengl.c: sets SRB2's GL render dimensions for the current eye (no texture flush).
extern void SetVRViewport(int w, int h);
// SRB2 view window (r_state.h) -- set to the eye buffer size so the 3D view fills the eye.
extern INT32 viewwidth, viewheight, viewwindowx, viewwindowy;

// VR render state shared with the GL renderer (declared in i_openxr.h).
boolean g_vrActive = false;
INT32 g_vrEyeIndex = 0; // which eye is rendering (0 = left, renders first)
// Read by the audio mixer's post-mix hook (any thread; a stale frame is fine).
boolean g_vrWaterMuffle = false;
// Stick-look pitch offset (degrees, +up). Springs toward the right stick's
// vertical deflection while vr_sticklook is On, back to level otherwise.
float g_vrStickPitch = 0.0f;
float   g_vrFov[4] = {0, 0, 0, 0};
float   g_vrYaw = 0.0f, g_vrPitch = 0.0f;   // head yaw/pitch (degrees) fed into SRB2's camera
float   g_vrEyeOff[3] = {0, 0, 0};          // this eye's position relative to the recenter
                                            // anchor (meters, tracking space) â€" drives
                                            // positional tracking + stereo in HWR_SetupView
static float g_vrEyePos[3]  = {0, 0, 0};    // current eye position (meters, OpenXR local space)
static float g_vrEyeQuat[4] = {0, 0, 0, 1}; // current eye orientation (x,y,z,w)

// vr_scale: SRB2 world units per real-world meter (tunable live; bigger = world feels smaller).
static CV_PossibleValue_t vrscale_cons_t[] = {{4*FRACUNIT, "MIN"}, {128*FRACUNIT, "MAX"}, {0, NULL}};
consvar_t cv_vrscale = CVAR_INIT("vr_scale", "32.0", CV_SAVE|CV_FLOAT, vrscale_cons_t, NULL);

// Floating UI screen tuning (meters). Read with atof(cv.string) like vr_scale.
static CV_PossibleValue_t vrscreendist_cons_t[] = {{FRACUNIT/2, "MIN"}, {5*FRACUNIT, "MAX"}, {0, NULL}};
consvar_t cv_vrscreendist = CVAR_INIT("vr_screendist", "3.0", CV_SAVE|CV_FLOAT, vrscreendist_cons_t, NULL);
static CV_PossibleValue_t vrscreensize_cons_t[] = {{FRACUNIT, "MIN"}, {8*FRACUNIT, "MAX"}, {0, NULL}};
consvar_t cv_vrscreensize = CVAR_INIT("vr_screensize", "4.0", CV_SAVE|CV_FLOAT, vrscreensize_cons_t, NULL);
consvar_t cv_vrhud = CVAR_INIT("vr_hud", "On", CV_SAVE, CV_OnOff, NULL);
// Floor of 5: the blend runs in gamma space so low values read far thinner than
// their number suggests (2/10 is invisible in-headset over a bright sky), and
// turning the HUD off is the "HUD in Headset" toggle's job, not the slider's.
static CV_PossibleValue_t vrhudalpha_cons_t[] = {{5, "MIN"}, {10, "MAX"}, {0, NULL}};
consvar_t cv_vrhudalpha = CVAR_INIT("vr_hudalpha", "8", CV_SAVE, vrhudalpha_cons_t, NULL);
// The crosshair sits glued to the center of the floating screen in VR and says
// nothing about where shots go, so it defaults off there (flatscreen untouched).
consvar_t cv_vrcrosshair = CVAR_INIT("vr_crosshair", "Off", CV_SAVE, CV_OnOff, NULL);
// In-level the desktop window follows the headset view (for recording); Off
// restores the flat body-camera view the window showed before.
consvar_t cv_vrmirror = CVAR_INIT("vr_mirror", "On", CV_SAVE, CV_OnOff, NULL);
// Eye-render MSAA. The request is clamped to the driver's max at build time.
static CV_PossibleValue_t vrmsaa_cons_t[] = {{0, "Off"}, {2, "2x"}, {4, "4x"}, {8, "8x"}, {0, NULL}};
consvar_t cv_vrmsaa = CVAR_INIT("vr_msaa", "4x", CV_SAVE, vrmsaa_cons_t, NULL);
// View mode (the standard VR set): right-stick click cycles, the cvar persists.
static void VR_ViewModeChanged(void);
static CV_PossibleValue_t vrviewmode_cons_t[] = {
	{VR_VIEW_THIRD_PERSON, "Third Person"}, {VR_VIEW_FIRST_PERSON, "First Person"},
	{VR_VIEW_THEATER, "Theater"}, {VR_VIEW_DIORAMA, "Diorama"}, {0, NULL}
};
consvar_t cv_vrviewmode = CVAR_INIT("vr_viewmode", "Third Person", CV_SAVE|CV_CALL, vrviewmode_cons_t, VR_ViewModeChanged);
// Third Person view: how close the chase camera sits to the player, as a
// percent of the game's own chase distance (100 = stock; lower = closer).
// Applied world-space in the VR eye setup, so it never touches the flat
// game's camera cvars.
static CV_PossibleValue_t vrthirddist_cons_t[] = {{25, "MIN"}, {250, "MAX"}, {0, NULL}};
consvar_t cv_vrthirddist = CVAR_INIT("vr_thirddist", "115", CV_SAVE, vrthirddist_cons_t, NULL);
// Diorama view: how many times more world a meter of head motion covers than
// vr_scale says (which also scales the IPD by the same factor - that
// hyperstereo is what makes the level read as a tabletop miniature).
static CV_PossibleValue_t vrdiorama_cons_t[] = {{2*FRACUNIT, "MIN"}, {32*FRACUNIT, "MAX"}, {0, NULL}};
consvar_t cv_vrdioramascale = CVAR_INIT("vr_dioramascale", "8.0", CV_SAVE|CV_FLOAT, vrdiorama_cons_t, NULL);
// Diorama placement (meters): pulling the camera back and up parks the
// miniature in front of and below the eyes, like a table you stand over.
static CV_PossibleValue_t vrdioramadist_cons_t[] = {{0, "MIN"}, {3*FRACUNIT, "MAX"}, {0, NULL}};
consvar_t cv_vrdioramadist = CVAR_INIT("vr_dioramadist", "0.5", CV_SAVE|CV_FLOAT, vrdioramadist_cons_t, NULL);
static CV_PossibleValue_t vrdioramaheight_cons_t[] = {{0, "MIN"}, {2*FRACUNIT, "MAX"}, {0, NULL}};
consvar_t cv_vrdioramaheight = CVAR_INIT("vr_dioramaheight", "0.4", CV_SAVE|CV_FLOAT, vrdioramaheight_cons_t, NULL);
// Comfort/quality knobs, all live. Stereo depth and head motion are percent of
// the real tracked values; render scale is percent of the swapchain resolution.
static CV_PossibleValue_t vrpercent_cons_t[] = {{0, "MIN"}, {100, "MAX"}, {0, NULL}};
consvar_t cv_vrstereo = CVAR_INIT("vr_stereo", "100", CV_SAVE, vrpercent_cons_t, NULL);
consvar_t cv_vrheadscale = CVAR_INIT("vr_headscale", "100", CV_SAVE, vrpercent_cons_t, NULL);
static CV_PossibleValue_t vrrenderscale_cons_t[] = {{40, "MIN"}, {150, "MAX"}, {0, NULL}};
consvar_t cv_vrrenderscale = CVAR_INIT("vr_renderscale", "100", CV_SAVE, vrrenderscale_cons_t, NULL);
// Angle culling in VR. On (default) = smart culling: the accept wedge follows
// the head's yaw and the eye's real FOV, and the eye render's view position is
// the true eye position, so the seg occlusion clipper is exact under lean (the
// mis-culling that forced the old Off default is gone at the root). Off drops
// all angle culling and draws the whole BSP traversal -- the escape hatch if a
// map ever shows culling pop-in.
// On: head-aware frustum wedge + exact solid-wall occlusion. Off: occlusion
// only -- the wedge (the one piece that can pop geometry at the view's
// edges) is skipped, but walls still hide the rooms behind them, so Off no
// longer means "draw the entire map per eye" the way the old full bypass
// did. Open areas still cost more with Off than On; walled maps barely do.
consvar_t cv_vrculling = CVAR_INIT("vr_culling", "On", CV_SAVE, CV_OnOff, NULL);
// Underwater feel in the eyes (the flat screen's wavy warp can't be stereo-correct):
// vr_wateratmo = the distance haze + full-eye veil, vr_bubbles = ambient bubbles
// drifting past the face (single-player only -- they are real synced objects),
// vr_watermuffle = low-pass on the mix while the view is submerged.
consvar_t cv_vrwateratmo = CVAR_INIT("vr_wateratmo", "On", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_vrbubbles = CVAR_INIT("vr_bubbles", "On", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_vrwatermuffle = CVAR_INIT("vr_watermuffle", "On", CV_SAVE, CV_OnOff, NULL);
// Underwater world sway strength, 0 (off) to 10 -- drives the world vertex
// shader's wave amplitude while a submerged eye renders.
static CV_PossibleValue_t vrten_cons_t[] = {{0, "MIN"}, {10, "MAX"}, {0, NULL}};
consvar_t cv_vrwatersway = CVAR_INIT("vr_watersway", "5", CV_SAVE, vrten_cons_t, NULL);
// Water-surface ripple boost, 0 (the stock surface) to 10 -- applies whenever
// a VR eye renders, above or below the water.
consvar_t cv_vrripple = CVAR_INIT("vr_ripple", "10", CV_SAVE, vrten_cons_t, NULL);
// The rest of the underwater look, one slider per layer (0 = off). Defaults
// are the in-headset picks from the field: fog a whisper, caustics and rays
// maxed, ripple maxed, sway moderate.
consvar_t cv_vrwaterfog = CVAR_INIT("vr_waterfog", "2", CV_SAVE, vrten_cons_t, NULL);
consvar_t cv_vrcaustics = CVAR_INIT("vr_caustics", "10", CV_SAVE, vrten_cons_t, NULL);
// Diagnostic only, deliberately not CV_SAVE: prints the per-eye waterline
// state so a left/right disagreement can be seen rather than guessed at.
consvar_t cv_vrwaterdebug = CVAR_INIT("vr_waterdebug", "Off", 0, CV_OnOff, NULL);
consvar_t cv_vrgodrays = CVAR_INIT("vr_godrays", "10", CV_SAVE, vrten_cons_t, NULL);
// Opt-in: hold the right stick up/down to pitch the view without craning the
// neck (seated comfort). Rotation the head didn't make is vection, so the
// offset eases in, springs back to level on release, and defaults Off.
consvar_t cv_vrsticklook = CVAR_INIT("vr_sticklook", "Off", CV_SAVE, CV_OnOff, NULL);
// Move where you look. The game thrusts forward/side along the BODY angle, the
// one the turn stick moves, so in first person -- where the head IS the camera
// -- "forward" drifts away from wherever you happen to be facing. On, the move
// stick is rotated by the head yaw before the game reads it, and the two
// rotations compose onto the heading you are actually looking down. Third
// person and Diorama default to the stock feel: their chase camera already
// gives you a fixed frame of reference, which is the thing first person lacks.
static CV_PossibleValue_t vrheadmove_cons_t[] = {{0, "Off"}, {1, "First Person"}, {2, "All Views"}, {0, NULL}};
consvar_t cv_vrheadmove = CVAR_INIT("vr_headmove", "First Person", CV_SAVE, vrheadmove_cons_t, NULL);
// Snap turning: the look stick's yaw becomes discrete steps, so turning carries
// no sweep to be sick from. Off keeps the shipped smooth analog turn.
static CV_PossibleValue_t vrsnapturn_cons_t[] = {{0, "Off"}, {30, "30"}, {45, "45"}, {90, "90"}, {0, NULL}};
consvar_t cv_vrsnapturn = CVAR_INIT("vr_snapturn", "Off", CV_SAVE, vrsnapturn_cons_t, NULL);
// How dark the WORLD dims behind an open menu in VR, 0 (none) to 16. This is
// a wash over the eye render -- the floating screen's pane stays clear glass
// -- so it can afford real strength. Flatscreen keeps the vanilla fade.
static CV_PossibleValue_t vrmenudim_cons_t[] = {{0, "MIN"}, {16, "MAX"}, {0, NULL}};
consvar_t cv_vrmenudim = CVAR_INIT("vr_menudim", "8", CV_SAVE, vrmenudim_cons_t, NULL);
// Opt-in progression skip: forces every unlockable and map open, in memory only
// (m_cond.c does the work). Gamedata.dat writes stay locked while the forced
// arrays exist (until a restart), so real progress is never overwritten -- but
// the session is NOT marked cheated: save slots, continues, and level saving
// keep working like normal play. The config loads before the gamedata does, so
// d_main re-applies it once the gamedata exists.
extern void M_ApplyUnlockEverything(void);
static void VR_UnlockAllChanged(void)
{
	M_ApplyUnlockEverything();
}
consvar_t cv_vrunlockall = CVAR_INIT("vr_unlockall", "Off", CV_SAVE|CV_CALL, CV_OnOff, VR_UnlockAllChanged);
// Live A/B for the projection-pose correction (the eye images render without
// head roll, so the submitted pose strips it too). On is the correct physics;
// the toggle exists so a headset verdict can isolate it in seconds if fusion
// ever feels off: flip it mid-game and compare.
consvar_t cv_vrposefix = CVAR_INIT("vr_posefix", "On", CV_SAVE, CV_OnOff, NULL);

#ifdef SRB2_HAVE_OPENXR

#define XR_USE_GRAPHICS_API_OPENGL
#define XR_USE_PLATFORM_WIN32
#include <windows.h>
#include <GL/gl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_MAX_SAMPLES
#define GL_MAX_SAMPLES 0x8D57
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif

// GL 3.0 framebuffer entry points, loaded via wglGetProcAddress at session start,
// so we can clear the eye swapchain textures into a valid composition frame.
typedef void (APIENTRY *MV_PFNGLGENFRAMEBUFFERS)(GLsizei, GLuint*);
typedef void (APIENTRY *MV_PFNGLBINDFRAMEBUFFER)(GLenum, GLuint);
typedef void (APIENTRY *MV_PFNGLFRAMEBUFFERTEXTURE2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (APIENTRY *MV_PFNGLDELETEFRAMEBUFFERS)(GLsizei, const GLuint*);
static MV_PFNGLGENFRAMEBUFFERS      p_glGenFramebuffers;
static MV_PFNGLBINDFRAMEBUFFER      p_glBindFramebuffer;
static MV_PFNGLFRAMEBUFFERTEXTURE2D p_glFramebufferTexture2D;
static MV_PFNGLDELETEFRAMEBUFFERS   p_glDeleteFramebuffers;
static GLuint s_fbo = 0;

typedef void (APIENTRY *MV_PFNGLGENRENDERBUFFERS)(GLsizei, GLuint*);
typedef void (APIENTRY *MV_PFNGLBINDRENDERBUFFER)(GLenum, GLuint);
typedef void (APIENTRY *MV_PFNGLRENDERBUFFERSTORAGE)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *MV_PFNGLFRAMEBUFFERRENDERBUFFER)(GLenum, GLenum, GLenum, GLuint);
typedef void (APIENTRY *MV_PFNGLDELETERENDERBUFFERS)(GLsizei, const GLuint*);
static MV_PFNGLGENRENDERBUFFERS        p_glGenRenderbuffers;
static MV_PFNGLBINDRENDERBUFFER        p_glBindRenderbuffer;
static MV_PFNGLRENDERBUFFERSTORAGE     p_glRenderbufferStorage;
static MV_PFNGLFRAMEBUFFERRENDERBUFFER p_glFramebufferRenderbuffer;
static MV_PFNGLDELETERENDERBUFFERS     p_glDeleteRenderbuffers;
static GLuint s_depthRB = 0;

// Optional GL 3.0 entry points for the multisampled eye target; MSAA is simply
// unavailable (renders go straight into the swapchain image) when these are
// missing on the driver.
typedef void (APIENTRY *MV_PFNGLRENDERBUFFERSTORAGEMULTISAMPLE)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *MV_PFNGLBLITFRAMEBUFFER)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef GLenum (APIENTRY *MV_PFNGLCHECKFRAMEBUFFERSTATUS)(GLenum);
static MV_PFNGLRENDERBUFFERSTORAGEMULTISAMPLE p_glRenderbufferStorageMultisample;
static MV_PFNGLBLITFRAMEBUFFER                p_glBlitFramebuffer;
static MV_PFNGLCHECKFRAMEBUFFERSTATUS         p_glCheckFramebufferStatus;
static GLuint s_msFBO = 0, s_msColorRB = 0, s_msDepthRB = 0; // multisampled eye target (both eyes, sequential)
static GLuint s_rtFBO = 0, s_rtTex = 0, s_rtDepthRB = 0;     // single-sample eye target (render scale / MSAA hop)
static int    s_msSamples = -1;   // sample count the targets were built for (-1 = never built)
static INT32  s_rtScalePct = -1;  // render-scale percent the targets were built for
static INT32  s_renderW = 0, s_renderH = 0; // active per-eye render size (equals the swapchain when unscaled)

static boolean VR_LoadGLFunctions(void)
{
	p_glGenFramebuffers      = (MV_PFNGLGENFRAMEBUFFERS) wglGetProcAddress("glGenFramebuffers");
	p_glBindFramebuffer      = (MV_PFNGLBINDFRAMEBUFFER) wglGetProcAddress("glBindFramebuffer");
	p_glFramebufferTexture2D = (MV_PFNGLFRAMEBUFFERTEXTURE2D) wglGetProcAddress("glFramebufferTexture2D");
	p_glDeleteFramebuffers   = (MV_PFNGLDELETEFRAMEBUFFERS) wglGetProcAddress("glDeleteFramebuffers");
	p_glGenRenderbuffers        = (MV_PFNGLGENRENDERBUFFERS) wglGetProcAddress("glGenRenderbuffers");
	p_glBindRenderbuffer        = (MV_PFNGLBINDRENDERBUFFER) wglGetProcAddress("glBindRenderbuffer");
	p_glRenderbufferStorage     = (MV_PFNGLRENDERBUFFERSTORAGE) wglGetProcAddress("glRenderbufferStorage");
	p_glFramebufferRenderbuffer = (MV_PFNGLFRAMEBUFFERRENDERBUFFER) wglGetProcAddress("glFramebufferRenderbuffer");
	p_glDeleteRenderbuffers     = (MV_PFNGLDELETERENDERBUFFERS) wglGetProcAddress("glDeleteRenderbuffers");
	p_glRenderbufferStorageMultisample = (MV_PFNGLRENDERBUFFERSTORAGEMULTISAMPLE) wglGetProcAddress("glRenderbufferStorageMultisample");
	if (!p_glRenderbufferStorageMultisample)
		p_glRenderbufferStorageMultisample = (MV_PFNGLRENDERBUFFERSTORAGEMULTISAMPLE) wglGetProcAddress("glRenderbufferStorageMultisampleEXT");
	p_glBlitFramebuffer        = (MV_PFNGLBLITFRAMEBUFFER) wglGetProcAddress("glBlitFramebuffer");
	p_glCheckFramebufferStatus = (MV_PFNGLCHECKFRAMEBUFFERSTATUS) wglGetProcAddress("glCheckFramebufferStatus");
	return (p_glGenFramebuffers && p_glBindFramebuffer && p_glFramebufferTexture2D && p_glDeleteFramebuffers
		&& p_glGenRenderbuffers && p_glBindRenderbuffer && p_glRenderbufferStorage && p_glFramebufferRenderbuffer && p_glDeleteRenderbuffers);
}

// ---- OpenXR state ----
static XrInstance     s_xrInstance   = XR_NULL_HANDLE;
static XrSystemId     s_xrSystem     = XR_NULL_SYSTEM_ID;
static XrSession      s_xrSession    = XR_NULL_HANDLE;
static XrSpace        s_xrSpace      = XR_NULL_HANDLE; // LOCAL (world) reference space
static XrSpace        s_viewRefSpace = XR_NULL_HANDLE; // VIEW (head) space
static XrSessionState s_xrState      = XR_SESSION_STATE_UNKNOWN;
static boolean        s_xrRunning    = false;
static XrFrameState   s_frameState;

static uint32_t s_viewCount = 0;
static XrViewConfigurationView s_viewConfigs[2];

typedef struct { XrSwapchain handle; uint32_t w, h, imgCount; XrSwapchainImageOpenGLKHR *images; } vr_swapchain_t;
static vr_swapchain_t s_eye[2];
static int64_t s_swapFmt = 0; // chosen at boot; the UI swapchain reuses it

// ---- floating UI screen (menus / title / console / HUD) ----
static vr_swapchain_t s_ui;                // quad-layer swapchain (window aspect, capped)
static GLuint  s_uiCopyFBO = 0;            // color-only FBO for writing into UI swapchain images
static GLuint  s_uiFBO = 0, s_uiTex = 0;   // in-level 2D overlay redirect target
static INT32   s_uiTexW = 0, s_uiTexH = 0;
static boolean s_uiOverlayActive = false;  // between VR_UIOverlayBegin/End
static boolean s_uiHaveOverlay = false;    // this frame drew the 2D overlay into s_uiTex
static boolean s_uiCaptured = false;       // a UI image was written this frame -> submit the quad
static boolean s_uiOpaque = true;          // captured image is a full frame (ignore its alpha)
// The screen is world-locked to an anchor (head position + yaw captured at placement);
// the pose derives from the anchor + live cvars each frame, so Screen Distance/Size
// changes apply immediately without re-centering.
static float   s_uiAnchorPos[3];
static float   s_uiAnchorYaw = 0.0f;
static boolean s_uiAnchorValid = false;    // false -> re-place in front of the head next frame
static XrTime  s_uiOffscreenSince = 0;     // head turned far away from the screen since (0 = looking at it)

static PFN_xrGetOpenGLGraphicsRequirementsKHR s_pfnGLReq = NULL;

// ---- helper ----
static boolean XR_Ok(XrResult r, const char *what)
{
	if (XR_SUCCEEDED(r)) return true;
	char buf[XR_MAX_RESULT_STRING_SIZE] = {0};
	if (s_xrInstance != XR_NULL_HANDLE) xrResultToString(s_xrInstance, r, buf);
	else snprintf(buf, sizeof buf, "%d", (int)r);
	CONS_Printf("[VR] %s failed: %s\n", what, buf);
	return false;
}

// ---- boot steps ----
// Optional controller-profile extensions, probed each boot. The Quest 3 / Pro
// native Touch Plus profile and the HP Reverb G2 profile only exist behind
// these; without the native profile the runtime auto-translates the old Touch
// bindings and that translation can land buttons on the wrong hand.
static boolean s_hasTouchPlus = false; // XR_META_touch_controller_plus
static boolean s_hasHpMR = false;      // XR_EXT_hp_mixed_reality_controller

static void VR_ProbeExtensions(void)
{
	uint32_t n = 0;
	XrExtensionProperties *props;
	s_hasTouchPlus = s_hasHpMR = false;
	if (!XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL)) || n == 0)
		return;
	props = calloc(n, sizeof(XrExtensionProperties));
	if (!props)
		return;
	for (uint32_t i = 0; i < n; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, n, &n, props)))
	{
		for (uint32_t i = 0; i < n; i++)
		{
			if (!strcmp(props[i].extensionName, "XR_META_touch_controller_plus"))
				s_hasTouchPlus = true;
			if (!strcmp(props[i].extensionName, "XR_EXT_hp_mixed_reality_controller"))
				s_hasHpMR = true;
		}
	}
	free(props);
}

static boolean VR_CreateInstance(void)
{
	const char *exts[3];
	uint32_t nexts = 0;
	XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
	VR_ProbeExtensions();
	exts[nexts++] = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
	if (s_hasTouchPlus) exts[nexts++] = "XR_META_touch_controller_plus";
	if (s_hasHpMR)      exts[nexts++] = "XR_EXT_hp_mixed_reality_controller";
	ci.enabledExtensionCount = nexts;
	ci.enabledExtensionNames = exts;
	strncpy(ci.applicationInfo.applicationName, "SRB2", XR_MAX_APPLICATION_NAME_SIZE - 1);
	ci.applicationInfo.applicationVersion = 1;
	strncpy(ci.applicationInfo.engineName, "SRB2-DoomLegacy", XR_MAX_ENGINE_NAME_SIZE - 1);
	ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	if (!XR_Ok(xrCreateInstance(&ci, &s_xrInstance), "xrCreateInstance")) return false;

	XrInstanceProperties props = { XR_TYPE_INSTANCE_PROPERTIES };
	if (XR_SUCCEEDED(xrGetInstanceProperties(s_xrInstance, &props)))
		CONS_Printf("[VR] OpenXR runtime: %s\n", props.runtimeName);
	return true;
}

static boolean VR_GetSystem(void)
{
	XrSystemGetInfo gi = { XR_TYPE_SYSTEM_GET_INFO };
	gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (!XR_Ok(xrGetSystem(s_xrInstance, &gi, &s_xrSystem), "xrGetSystem")) return false;

	if (!XR_Ok(xrGetInstanceProcAddr(s_xrInstance, "xrGetOpenGLGraphicsRequirementsKHR",
			(PFN_xrVoidFunction*)&s_pfnGLReq), "get xrGetOpenGLGraphicsRequirementsKHR")) return false;

	XrGraphicsRequirementsOpenGLKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
	if (!XR_Ok(s_pfnGLReq(s_xrInstance, s_xrSystem, &req), "xrGetOpenGLGraphicsRequirementsKHR")) return false;
	CONS_Printf("[VR] GL min version %u.%u\n",
		XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported));
	return true;
}

static boolean VR_CreateSession(void)
{
	XrGraphicsBindingOpenGLWin32KHR gb = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
	gb.hDC   = wglGetCurrentDC();
	gb.hGLRC = wglGetCurrentContext();
	if (!gb.hDC || !gb.hGLRC) { CONS_Printf("[VR] No current WGL context\n"); return false; }

	XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
	sci.next = &gb;
	sci.systemId = s_xrSystem;
	if (!XR_Ok(xrCreateSession(s_xrInstance, &sci, &s_xrSession), "xrCreateSession")) return false;

	XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rs.poseInReferenceSpace.orientation.w = 1.0f;
	if (!XR_Ok(xrCreateReferenceSpace(s_xrSession, &rs, &s_xrSpace), "xrCreateReferenceSpace(LOCAL)")) return false;

	XrReferenceSpaceCreateInfo vs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	vs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	vs.poseInReferenceSpace.orientation.w = 1.0f;
	if (!XR_Ok(xrCreateReferenceSpace(s_xrSession, &vs, &s_viewRefSpace), "xrCreateReferenceSpace(VIEW)")) return false;

	CONS_Printf("[VR] Session + reference spaces created.\n");
	return true;
}

// ---- controller input (OpenXR action system) ----
// Every physical control is its own action so the input bridge can mirror the
// whole controller into SRB2's synthetic gamepad (see VR_ProcessInput).
static XrActionSet s_actionSet = XR_NULL_HANDLE;
static XrAction s_actMove     = XR_NULL_HANDLE; // left stick  (vec2): walk/strafe, menu d-pad
static XrAction s_actLook     = XR_NULL_HANDLE; // right stick (vec2): turn/look, menu d-pad too
static XrAction s_actBtnA     = XR_NULL_HANDLE; // A: jump, menu accept
static XrAction s_actBtnB     = XR_NULL_HANDLE; // B: spin, menu back
static XrAction s_actBtnX     = XR_NULL_HANDLE; // X: custom 1, menu back
static XrAction s_actBtnY     = XR_NULL_HANDLE; // Y: custom 2
static XrAction s_actMenuBtn  = XR_NULL_HANDLE; // menu button: open/close the game menu
static XrAction s_actLStick   = XR_NULL_HANDLE; // left stick click: custom 3; recenter in menus
static XrAction s_actRStick   = XR_NULL_HANDLE; // right stick click: first/third person toggle
static XrAction s_actLTrigger = XR_NULL_HANDLE; // analog: fire normal / menu back
static XrAction s_actRTrigger = XR_NULL_HANDLE; // analog: fire / menu accept
static XrAction s_actLGrip    = XR_NULL_HANDLE; // analog: toss flag (hold)
static XrAction s_actRGrip    = XR_NULL_HANDLE; // analog: center view / lock-on (hold)
static XrAction s_actHaptic   = XR_NULL_HANDLE; // vibration output, per-hand subactions
static XrPath   s_handPath[2] = { XR_NULL_PATH, XR_NULL_PATH }; // left, right

static XrPath VR_Path(const char *s)
{
	XrPath p = XR_NULL_PATH;
	xrStringToPath(s_xrInstance, s, &p);
	return p;
}

static boolean VR_MakeAction(XrAction *out, XrActionType type, const char *name, const char *label, boolean perHand)
{
	XrActionCreateInfo ai = { XR_TYPE_ACTION_CREATE_INFO };
	ai.actionType = type;
	if (perHand)
	{
		ai.countSubactionPaths = 2;
		ai.subactionPaths = s_handPath;
	}
	strncpy(ai.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
	strncpy(ai.localizedActionName, label, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
	return XR_Ok(xrCreateAction(s_actionSet, &ai, out), name);
}

// One binding row: an action tied to one input path on one hand.
typedef struct { XrAction action; const char *path; } vr_bind_t;

// Suggest one profile's bindings; a profile or path the runtime doesn't know
// just doesn't apply (it picks whichever suggested profile matches the hardware).
static void VR_SuggestProfile(const char *profilePath, const vr_bind_t *binds, int count)
{
	XrActionSuggestedBinding sb[32];
	XrInteractionProfileSuggestedBinding spb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	XrPath profile = VR_Path(profilePath);
	uint32_t n = 0;
	int i;

	if (profile == XR_NULL_PATH)
		return;
	for (i = 0; i < count && n < 32; i++)
	{
		XrPath p;
		if (binds[i].action == XR_NULL_HANDLE)
			continue;
		p = VR_Path(binds[i].path);
		if (p == XR_NULL_PATH)
			continue;
		sb[n].action  = binds[i].action;
		sb[n].binding = p;
		n++;
	}
	if (n == 0)
		return;
	spb.interactionProfile = profile;
	spb.suggestedBindings = sb;
	spb.countSuggestedBindings = n;
	if (XR_SUCCEEDED(xrSuggestInteractionProfileBindings(s_xrInstance, &spb)))
		CONS_Printf("[VR] suggested bindings for %s\n", profilePath);
}

// Create the action set, suggest the per-device bindings and attach to the
// session. Any failure leaves VR fully functional, just without controllers.
static boolean VR_CreateActions(void)
{
	XrActionSetCreateInfo si = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strncpy(si.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
	strncpy(si.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
	if (!XR_Ok(xrCreateActionSet(s_xrInstance, &si, &s_actionSet), "xrCreateActionSet")) return false;

	xrStringToPath(s_xrInstance, "/user/hand/left",  &s_handPath[0]);
	xrStringToPath(s_xrInstance, "/user/hand/right", &s_handPath[1]);

	if (!VR_MakeAction(&s_actMove,     XR_ACTION_TYPE_VECTOR2F_INPUT,   "move",          "Move", false)) return false;
	if (!VR_MakeAction(&s_actLook,     XR_ACTION_TYPE_VECTOR2F_INPUT,   "look",          "Look", false)) return false;
	if (!VR_MakeAction(&s_actBtnA,     XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_a",      "Jump / Accept", false)) return false;
	if (!VR_MakeAction(&s_actBtnB,     XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_b",      "Spin / Back", false)) return false;
	if (!VR_MakeAction(&s_actBtnX,     XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_x",      "Custom 1 / Back", false)) return false;
	if (!VR_MakeAction(&s_actBtnY,     XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_y",      "Custom 2", false)) return false;
	if (!VR_MakeAction(&s_actMenuBtn,  XR_ACTION_TYPE_BOOLEAN_INPUT,    "menu",          "Game Menu", false)) return false;
	if (!VR_MakeAction(&s_actLStick,   XR_ACTION_TYPE_BOOLEAN_INPUT,    "left_stick",    "Custom 3 / Recenter", false)) return false;
	if (!VR_MakeAction(&s_actRStick,   XR_ACTION_TYPE_BOOLEAN_INPUT,    "right_stick",   "Change View", false)) return false;
	if (!VR_MakeAction(&s_actLTrigger, XR_ACTION_TYPE_FLOAT_INPUT,      "left_trigger",  "Fire Normal", false)) return false;
	if (!VR_MakeAction(&s_actRTrigger, XR_ACTION_TYPE_FLOAT_INPUT,      "right_trigger", "Fire", false)) return false;
	if (!VR_MakeAction(&s_actLGrip,    XR_ACTION_TYPE_FLOAT_INPUT,      "left_grip",     "Toss Flag", false)) return false;
	if (!VR_MakeAction(&s_actRGrip,    XR_ACTION_TYPE_FLOAT_INPUT,      "right_grip",    "Center View / Lock-on", false)) return false;
	if (!VR_MakeAction(&s_actHaptic,   XR_ACTION_TYPE_VIBRATION_OUTPUT, "rumble",        "Rumble", true)) return false;

	{ // Quest/Rift Touch controllers (the right Oculus button is system-reserved)
		const vr_bind_t touch[] = {
			{ s_actMove,     "/user/hand/left/input/thumbstick" },
			{ s_actLook,     "/user/hand/right/input/thumbstick" },
			{ s_actBtnA,     "/user/hand/right/input/a/click" },
			{ s_actBtnB,     "/user/hand/right/input/b/click" },
			{ s_actBtnX,     "/user/hand/left/input/x/click" },
			{ s_actBtnY,     "/user/hand/left/input/y/click" },
			{ s_actMenuBtn,  "/user/hand/left/input/menu/click" },
			{ s_actLStick,   "/user/hand/left/input/thumbstick/click" },
			{ s_actRStick,   "/user/hand/right/input/thumbstick/click" },
			{ s_actLTrigger, "/user/hand/left/input/trigger/value" },
			{ s_actRTrigger, "/user/hand/right/input/trigger/value" },
			{ s_actLGrip,    "/user/hand/left/input/squeeze/value" },
			{ s_actRGrip,    "/user/hand/right/input/squeeze/value" },
			{ s_actHaptic,   "/user/hand/left/output/haptic" },
			{ s_actHaptic,   "/user/hand/right/output/haptic" },
		};
		VR_SuggestProfile("/interaction_profiles/oculus/touch_controller", touch, (int)(sizeof(touch)/sizeof(touch[0])));

		// Quest 3 / Quest Pro native profile: identical layout to Touch, so the
		// same table applies. Suggesting it explicitly matters: with only the
		// older Touch bindings suggested, the runtime auto-translates them onto
		// Touch Plus and that translation can land buttons on the wrong hand.
		if (s_hasTouchPlus)
			VR_SuggestProfile("/interaction_profiles/meta/touch_controller_plus", touch, (int)(sizeof(touch)/sizeof(touch[0])));

		// HP Reverb G2: same control set as Touch (a/b right, x/y left, menu on
		// both hands, analog squeeze).
		if (s_hasHpMR)
			VR_SuggestProfile("/interaction_profiles/hp/mixed_reality_controller", touch, (int)(sizeof(touch)/sizeof(touch[0])));
	}
	{ // Valve Index knuckles: A/B on both hands, no menu button (system button is
	  // reserved), so the left B stands in for the game menu and Custom 2 goes unbound.
		const vr_bind_t index[] = {
			{ s_actMove,     "/user/hand/left/input/thumbstick" },
			{ s_actLook,     "/user/hand/right/input/thumbstick" },
			{ s_actBtnA,     "/user/hand/right/input/a/click" },
			{ s_actBtnB,     "/user/hand/right/input/b/click" },
			{ s_actBtnX,     "/user/hand/left/input/a/click" },
			{ s_actMenuBtn,  "/user/hand/left/input/b/click" },
			{ s_actLStick,   "/user/hand/left/input/thumbstick/click" },
			{ s_actRStick,   "/user/hand/right/input/thumbstick/click" },
			{ s_actLTrigger, "/user/hand/left/input/trigger/value" },
			{ s_actRTrigger, "/user/hand/right/input/trigger/value" },
			{ s_actLGrip,    "/user/hand/left/input/squeeze/value" },
			{ s_actRGrip,    "/user/hand/right/input/squeeze/value" },
			{ s_actHaptic,   "/user/hand/left/output/haptic" },
			{ s_actHaptic,   "/user/hand/right/output/haptic" },
		};
		VR_SuggestProfile("/interaction_profiles/valve/index_controller", index, (int)(sizeof(index)/sizeof(index[0])));
	}
	{ // Windows Mixed Reality wands: no face buttons, so the trackpad clicks stand
	  // in for jump (right) and spin (left). Squeeze is a click on these, not analog.
		const vr_bind_t wmr[] = {
			{ s_actMove,     "/user/hand/left/input/thumbstick" },
			{ s_actLook,     "/user/hand/right/input/thumbstick" },
			{ s_actBtnA,     "/user/hand/right/input/trackpad/click" },
			{ s_actBtnB,     "/user/hand/left/input/trackpad/click" },
			{ s_actMenuBtn,  "/user/hand/left/input/menu/click" },
			{ s_actLStick,   "/user/hand/left/input/thumbstick/click" },
			{ s_actRStick,   "/user/hand/right/input/thumbstick/click" },
			{ s_actLTrigger, "/user/hand/left/input/trigger/value" },
			{ s_actRTrigger, "/user/hand/right/input/trigger/value" },
			{ s_actLGrip,    "/user/hand/left/input/squeeze/click" },
			{ s_actRGrip,    "/user/hand/right/input/squeeze/click" },
			{ s_actHaptic,   "/user/hand/left/output/haptic" },
			{ s_actHaptic,   "/user/hand/right/output/haptic" },
		};
		VR_SuggestProfile("/interaction_profiles/microsoft/motion_controller", wmr, (int)(sizeof(wmr)/sizeof(wmr[0])));
	}
	{ // Vive wands: no sticks at all, so the trackpads move and look and their
	  // clicks stand in for jump/spin. Best-effort.
		const vr_bind_t vive[] = {
			{ s_actMove,     "/user/hand/left/input/trackpad" },
			{ s_actLook,     "/user/hand/right/input/trackpad" },
			{ s_actBtnA,     "/user/hand/right/input/trackpad/click" },
			{ s_actBtnB,     "/user/hand/left/input/trackpad/click" },
			{ s_actMenuBtn,  "/user/hand/left/input/menu/click" },
			{ s_actLTrigger, "/user/hand/left/input/trigger/value" },
			{ s_actRTrigger, "/user/hand/right/input/trigger/value" },
			{ s_actLGrip,    "/user/hand/left/input/squeeze/click" },
			{ s_actRGrip,    "/user/hand/right/input/squeeze/click" },
			{ s_actHaptic,   "/user/hand/left/output/haptic" },
			{ s_actHaptic,   "/user/hand/right/output/haptic" },
		};
		VR_SuggestProfile("/interaction_profiles/htc/vive_controller", vive, (int)(sizeof(vive)/sizeof(vive[0])));
	}
	{ // Generic fallback: any runtime supports this two-button profile
		const vr_bind_t simple[] = {
			{ s_actBtnA,    "/user/hand/right/input/select/click" },
			{ s_actBtnB,    "/user/hand/left/input/select/click" },
			{ s_actMenuBtn, "/user/hand/left/input/menu/click" },
			{ s_actHaptic,  "/user/hand/left/output/haptic" },
			{ s_actHaptic,  "/user/hand/right/output/haptic" },
		};
		VR_SuggestProfile("/interaction_profiles/khr/simple_controller", simple, (int)(sizeof(simple)/sizeof(simple[0])));
	}

	{
		XrSessionActionSetsAttachInfo at = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
		at.countActionSets = 1;
		at.actionSets = &s_actionSet;
		if (!XR_Ok(xrAttachSessionActionSets(s_xrSession, &at), "xrAttachSessionActionSets")) return false;
	}
	CONS_Printf("[VR] Controller actions ready (sticks, buttons, triggers, grips, rumble).\n");
	return true;
}

// Print which interaction profile the runtime actually bound for each hand.
// Fires on focus and whenever the runtime reports a profile change; this line
// is the first thing to check when a controller behaves oddly (wrong hand,
// dead buttons), since it shows what the runtime matched us to.
static void VR_LogActiveProfiles(void)
{
	static const char *handName[2] = { "left", "right" };
	int h;
	if (s_actionSet == XR_NULL_HANDLE || s_xrSession == XR_NULL_HANDLE)
		return;
	for (h = 0; h < 2; h++)
	{
		char buf[XR_MAX_PATH_LENGTH];
		XrInteractionProfileState ips = { XR_TYPE_INTERACTION_PROFILE_STATE };
		snprintf(buf, sizeof buf, "none (not bound)");
		if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(s_xrSession, s_handPath[h], &ips))
			&& ips.interactionProfile != XR_NULL_PATH)
		{
			uint32_t len = 0;
			xrPathToString(s_xrInstance, ips.interactionProfile, sizeof buf, &len, buf);
		}
		CONS_Printf("[VR] %s controller profile: %s\n", handName[h], buf);
	}
}

static boolean VR_EnumViews(void)
{
	XrViewConfigurationType vt = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	uint32_t n = 0;
	if (!XR_Ok(xrEnumerateViewConfigurationViews(s_xrInstance, s_xrSystem, vt, 0, &n, NULL), "enum view count")) return false;
	if (n > 2) n = 2;
	for (uint32_t i = 0; i < n; i++) s_viewConfigs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	if (!XR_Ok(xrEnumerateViewConfigurationViews(s_xrInstance, s_xrSystem, vt, n, &n, s_viewConfigs), "enum views")) return false;
	s_viewCount = n;
	CONS_Printf("[VR] %u eyes, recommended %ux%u per eye\n", n,
		s_viewConfigs[0].recommendedImageRectWidth, s_viewConfigs[0].recommendedImageRectHeight);
	return true;
}

// The runtime dictates which GL swapchain formats are valid (SteamVR rejects plain
// GL_RGBA8), so enumerate and pick a supported one, sRGB preferred for correct gamma.
static int64_t VR_ChooseSwapchainFormat(void)
{
	uint32_t n = 0;
	if (!XR_Ok(xrEnumerateSwapchainFormats(s_xrSession, 0, &n, NULL), "enum swapchain formats") || n == 0)
		return GL_RGBA8;
	int64_t *fmts = calloc(n, sizeof(int64_t));
	xrEnumerateSwapchainFormats(s_xrSession, n, &n, fmts);
	const int64_t prefs[] = { GL_SRGB8_ALPHA8, GL_RGBA8, GL_RGBA16F };
	int64_t chosen = fmts[0]; // fallback: the runtime's most-preferred format
	boolean found = false;
	for (uint32_t p = 0; p < sizeof(prefs)/sizeof(prefs[0]) && !found; p++)
		for (uint32_t i = 0; i < n; i++)
			if (fmts[i] == prefs[p]) { chosen = prefs[p]; found = true; break; }
	free(fmts);
	CONS_Printf("[VR] swapchain format 0x%llx (%u supported)\n", (unsigned long long)chosen, n);
	return chosen;
}

static boolean VR_CreateSwapchains(void)
{
	int64_t fmt = s_swapFmt = VR_ChooseSwapchainFormat();
	for (uint32_t e = 0; e < s_viewCount; e++)
	{
		XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		ci.format = fmt;
		ci.sampleCount = 1;
		// Eye buffer at the runtime's recommended resolution (capped, aspect preserved).
		// SRB2 is told to render at exactly this size per eye (see VR_EndFrame's vid swap).
		{
			uint32_t rw = s_viewConfigs[e].recommendedImageRectWidth;
			uint32_t rh = s_viewConfigs[e].recommendedImageRectHeight;
			const uint32_t cap = 2048;
			uint32_t mx = (rw > rh) ? rw : rh;
			if (mx > cap) { rw = (uint32_t)((float)rw * cap / mx); rh = (uint32_t)((float)rh * cap / mx); }
			ci.width  = s_eye[e].w = rw;
			ci.height = s_eye[e].h = rh;
		}
		ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
		if (!XR_Ok(xrCreateSwapchain(s_xrSession, &ci, &s_eye[e].handle), "xrCreateSwapchain")) return false;

		uint32_t n = 0;
		xrEnumerateSwapchainImages(s_eye[e].handle, 0, &n, NULL);
		s_eye[e].images = calloc(n, sizeof(XrSwapchainImageOpenGLKHR));
		for (uint32_t i = 0; i < n; i++) s_eye[e].images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
		xrEnumerateSwapchainImages(s_eye[e].handle, n, &n, (XrSwapchainImageBaseHeader*)s_eye[e].images);
		s_eye[e].imgCount = n;
		CONS_Printf("[VR] eye %u swapchain: %ux%u, %u images\n", e, s_eye[e].w, s_eye[e].h, n);
	}
	return true;
}

// Draw the u0..u1 x v0..v1 sub-rect of tex as a fullscreen quad into the currently
// bound framebuffer. The overlay is accumulated over transparent black, so its RGB is
// premultiplied by alpha: blending uses (ONE, ONE_MINUS_SRC_ALPHA) and `scale` dims
// RGB and alpha together. Fixed pipeline, wrapped in push/pop of every touched state
// so SRB2's cached GL state stays truthful.
static void VR_BlitTexRect(GLuint tex, float u0, float v0, float u1, float v1, float scale, boolean blend)
{
	glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TEXTURE_BIT | GL_CURRENT_BIT | GL_DEPTH_BUFFER_BIT);
	glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_FOG);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	if (blend) { glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); }
	else glDisable(GL_BLEND);
	glColor4f(scale, scale, scale, scale);
	glBegin(GL_TRIANGLE_FAN);
	glTexCoord2f(u0, v0); glVertex2f(-1, -1);
	glTexCoord2f(u1, v0); glVertex2f( 1, -1);
	glTexCoord2f(u1, v1); glVertex2f( 1,  1);
	glTexCoord2f(u0, v1); glVertex2f(-1,  1);
	glEnd();
	glMatrixMode(GL_MODELVIEW);  glPopMatrix();
	glMatrixMode(GL_PROJECTION); glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
	glPopAttrib();
}

static void VR_BlitTexFullscreen(GLuint tex, float scale, boolean blend)
{
	VR_BlitTexRect(tex, 0.0f, 0.0f, 1.0f, 1.0f, scale, blend);
}

// (Re)create the transparent overlay buffer at the current render size.
static boolean VR_EnsureOverlayFBO(void)
{
	if (!p_glGenFramebuffers) return false;
	if (s_uiFBO && s_uiTex && s_uiTexW == vid.width && s_uiTexH == vid.height) return true;
	if (vid.width < 64 || vid.height < 64) return false;
	glPushAttrib(GL_TEXTURE_BIT); // keep SRB2's cached texture binding truthful
	if (s_uiTex) glDeleteTextures(1, &s_uiTex);
	glGenTextures(1, &s_uiTex);
	glBindTexture(GL_TEXTURE_2D, s_uiTex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, vid.width, vid.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glPopAttrib();
	if (!s_uiFBO) p_glGenFramebuffers(1, &s_uiFBO);
	s_uiTexW = vid.width; s_uiTexH = vid.height;
	return true;
}

// ---- desktop mirror: in-level, the window follows the headset (for recording) ----
static GLuint s_mirrorTex = 0;
static int s_mirrorW = 0, s_mirrorH = 0;
static boolean s_mirrorValid = false;   // the mirror texture holds an eye frame
static boolean s_mirrorUIValid = false; // s_uiTex holds this frame's HUD overlay
static boolean s_mirrorWorldLayer = false; // this frame's world layer IS the mirror already

// Stereo ease-in (the pattern every reference port ships): separation fades up
// over ~half a second whenever eye rendering (re)starts or the view cuts, and
// decays gently through brief blips instead of collapsing -- a cut lands flat
// and fades to depth instead of momentarily cross-eyed.
static float s_stereoRamp = 0.0f;
static fixed_t s_lastMoX, s_lastMoY, s_lastMoZ;
static boolean s_lastMoValid = false;

// Snapshot the left eye. Called from VR_EndFrame while s_fbo (with the eye's
// swapchain image attached) is still the bound framebuffer, so a plain
// CopyTexSubImage grabs it. FRAMEBUFFER_SRGB is off and the stored values are
// display-ready; RGBA8 keeps them from being re-decoded when drawn.
static void VR_MirrorGrab(void)
{
	const int w = (int)s_eye[0].w, h = (int)s_eye[0].h;
	glPushAttrib(GL_TEXTURE_BIT); // keep SRB2's cached texture binding truthful
	if (!s_mirrorTex || s_mirrorW != w || s_mirrorH != h)
	{
		if (s_mirrorTex) glDeleteTextures(1, &s_mirrorTex);
		glGenTextures(1, &s_mirrorTex);
		glBindTexture(GL_TEXTURE_2D, s_mirrorTex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		s_mirrorW = w; s_mirrorH = h;
	}
	else
		glBindTexture(GL_TEXTURE_2D, s_mirrorTex);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
	glPopAttrib();
	s_mirrorValid = true;
}

// Create the quad-layer swapchain on first use (vid isn't sized until video boot).
// Kept for the session once created; the aspect follows the render size at that moment.
static boolean VR_EnsureUISwapchain(void)
{
	uint32_t w, h, mx, n;
	const uint32_t cap = 2048;

	if (s_ui.handle != XR_NULL_HANDLE) return true;
	if (s_xrSession == XR_NULL_HANDLE) return false;
	w = (uint32_t)vid.width; h = (uint32_t)vid.height;
	if (w < 64 || h < 64) return false;
	mx = (w > h) ? w : h;
	if (mx > cap) { w = (uint32_t)((float)w * cap / mx); h = (uint32_t)((float)h * cap / mx); }

	{
		XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		ci.format = s_swapFmt;
		ci.sampleCount = 1;
		ci.width = w; ci.height = h;
		ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
		if (!XR_Ok(xrCreateSwapchain(s_xrSession, &ci, &s_ui.handle), "xrCreateSwapchain(UI)")) return false;
	}
	s_ui.w = w; s_ui.h = h;

	n = 0;
	xrEnumerateSwapchainImages(s_ui.handle, 0, &n, NULL);
	s_ui.images = calloc(n, sizeof(XrSwapchainImageOpenGLKHR));
	for (uint32_t i = 0; i < n; i++) s_ui.images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
	xrEnumerateSwapchainImages(s_ui.handle, n, &n, (XrSwapchainImageBaseHeader*)s_ui.images);
	s_ui.imgCount = n;
	if (!s_uiCopyFBO) p_glGenFramebuffers(1, &s_uiCopyFBO);
	CONS_Printf("[VR] UI screen swapchain: %ux%u, %u images\n", w, h, n);
	return true;
}

// Anchor the screen to the current head pose: it hangs at head height,
// vr_screendist meters along the head's horizontal forward, yaw-only facing
// the viewer. World-locked until re-anchored (recenter, headset put on, or
// the head turned way off it for a while).
static void VR_AnchorUIScreen(void)
{
	XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
	float fx, fz, len;

	if (!XR_SUCCEEDED(xrLocateSpace(s_viewRefSpace, s_xrSpace, s_frameState.predictedDisplayTime, &loc)))
		return;
	if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
		|| !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
		return;

	{ // horizontal head-forward from the orientation quaternion (same math as the eye yaw)
		const float qx = loc.pose.orientation.x, qy = loc.pose.orientation.y;
		const float qz = loc.pose.orientation.z, qw = loc.pose.orientation.w;
		fx = -2.0f*(qx*qz + qy*qw);
		fz = -(1.0f - 2.0f*(qx*qx + qy*qy));
	}
	len = sqrtf(fx*fx + fz*fz);
	if (len < 0.01f) return; // looking straight up/down; keep the previous anchor
	fx /= len; fz /= len;

	s_uiAnchorPos[0] = loc.pose.position.x;
	s_uiAnchorPos[1] = loc.pose.position.y;
	s_uiAnchorPos[2] = loc.pose.position.z;
	s_uiAnchorYaw = atan2f(-fx, -fz);
	s_uiAnchorValid = true;
	s_uiOffscreenSince = 0;
}

// Build the quad pose from the anchor + live cvars (distance changes apply instantly).
static XrPosef VR_UIScreenPose(void)
{
	XrPosef pose;
	float dist = (float)atof(cv_vrscreendist.string);
	if (dist < 0.5f) dist = 3.0f;

	pose.position.x = s_uiAnchorPos[0] - sinf(s_uiAnchorYaw)*dist;
	pose.position.y = s_uiAnchorPos[1];
	pose.position.z = s_uiAnchorPos[2] - cosf(s_uiAnchorYaw)*dist;
	pose.orientation.x = 0.0f;
	pose.orientation.y = sinf(s_uiAnchorYaw * 0.5f);
	pose.orientation.z = 0.0f;
	pose.orientation.w = cosf(s_uiAnchorYaw * 0.5f);
	return pose;
}

// Findability net: if the head has pointed >120 degrees away from the screen for a
// couple of seconds (headset was put on facing elsewhere, chair swiveled...), snap
// the screen back in front rather than leaving the user in an empty black void.
static void VR_UIScreenFindability(void)
{
	XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
	float fx, fz, len, headYaw, diff;

	if (!s_uiAnchorValid) return;
	if (!XR_SUCCEEDED(xrLocateSpace(s_viewRefSpace, s_xrSpace, s_frameState.predictedDisplayTime, &loc))
		|| !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		return;

	{
		const float qx = loc.pose.orientation.x, qy = loc.pose.orientation.y;
		const float qz = loc.pose.orientation.z, qw = loc.pose.orientation.w;
		fx = -2.0f*(qx*qz + qy*qw);
		fz = -(1.0f - 2.0f*(qx*qx + qy*qy));
	}
	len = sqrtf(fx*fx + fz*fz);
	if (len < 0.01f) return;
	headYaw = atan2f(-fx, -fz);

	diff = headYaw - s_uiAnchorYaw;
	while (diff >  3.14159265f) diff -= 6.28318531f;
	while (diff < -3.14159265f) diff += 6.28318531f;

	if (fabsf(diff) > 2.094f) // ~120 degrees
	{
		if (s_uiOffscreenSince == 0)
			s_uiOffscreenSince = s_frameState.predictedDisplayTime;
		else if (s_frameState.predictedDisplayTime - s_uiOffscreenSince > 2000000000LL) // 2s
			s_uiAnchorValid = false; // re-anchor in front of the head next frame
	}
	else
		s_uiOffscreenSince = 0;
}

static void VR_PollEvents(void)
{
	XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
	while (xrPollEvent(s_xrInstance, &ev) == XR_SUCCESS)
	{
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			const XrEventDataSessionStateChanged *e = (const XrEventDataSessionStateChanged*)&ev;
			s_xrState = e->state;
			// The headset was just put on / regained attention: the screen anchored during
			// boot points wherever the HMD lay, so re-place it in front of the user's eyes.
			if (e->state == XR_SESSION_STATE_FOCUSED)
			{
				s_uiAnchorValid = false;
				VR_LogActiveProfiles();
			}
			if (e->state == XR_SESSION_STATE_READY)
			{
				XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
				bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if (XR_Ok(xrBeginSession(s_xrSession, &bi), "xrBeginSession"))
				{ s_xrRunning = true; CONS_Printf("[VR] Session running.\n"); }
			}
			else if (e->state == XR_SESSION_STATE_STOPPING)
			{
				xrEndSession(s_xrSession);
				s_xrRunning = false;
				CONS_Printf("[VR] Session stopped.\n");
			}
		}
		else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED)
			VR_LogActiveProfiles(); // controllers woke up / switched profile
		ev.type = XR_TYPE_EVENT_DATA_BUFFER; // reset for next poll
	}
}

static void VR_ReleaseAllKeys(void); // defined with the input bridge below

#endif // SRB2_HAVE_OPENXR

// ---------------- cvar (always present, even without the loader) ----------------
static void VR_OnModeChange(void);
static void Command_VRRecenter_f(void);

static CV_PossibleValue_t vrmode_cons_t[] = {
	{0, "Off"}, {1, "VR"}, {2, "StereoPreview"}, {3, "Auto"}, {0, NULL}
};
// CV_SAVE persists it to config; CV_CALL fires VR_OnModeChange when it changes.
// Auto (the default) is plug & play: a live headset enables VR, otherwise flatscreen.
consvar_t cv_vrmode = CVAR_INIT ("vr_mode", "Auto", CV_SAVE|CV_CALL, vrmode_cons_t, VR_OnModeChange);

// One-time probe: is a headset actually available right now? Drives vr_mode Auto
// and the renderer pick in I_StartupGraphics (which runs before the config loads,
// so it can't consult the cvar). Needs no GL context. Cached for the session.
boolean VR_HMDPresent(void)
{
#ifdef SRB2_HAVE_OPENXR
	static int cached = -1; // -1 unknown, 0 absent, 1 present
	if (cached < 0)
	{
		XrInstance probe = s_xrInstance;
		cached = 0;
		if (probe == XR_NULL_HANDLE)
		{
			const char *exts[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
			XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
			ci.enabledExtensionCount = 1;
			ci.enabledExtensionNames = exts;
			strncpy(ci.applicationInfo.applicationName, "SRB2", XR_MAX_APPLICATION_NAME_SIZE - 1);
			ci.applicationInfo.applicationVersion = 1;
			strncpy(ci.applicationInfo.engineName, "SRB2-DoomLegacy", XR_MAX_ENGINE_NAME_SIZE - 1);
			ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
			if (!XR_SUCCEEDED(xrCreateInstance(&ci, &probe)))
				probe = XR_NULL_HANDLE;
		}
		if (probe != XR_NULL_HANDLE)
		{
			XrSystemGetInfo gi = { XR_TYPE_SYSTEM_GET_INFO };
			XrSystemId sys = XR_NULL_SYSTEM_ID;
			gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
			if (XR_SUCCEEDED(xrGetSystem(probe, &gi, &sys)))
			{
				cached = 1;
				CONS_Printf("[VR] Headset detected - VR enabled (vr_mode Auto).\n");
			}
			if (probe != s_xrInstance)
				xrDestroyInstance(probe);
		}
	}
	return cached == 1;
#else
	return false;
#endif
}

boolean VR_WantVR(void)
{
	if (cv_vrmode.value == 1) return true;
	if (cv_vrmode.value == 3) return VR_HMDPresent();
	return false;
}

// Does the 3D world render around the player in the headset this frame?
// True in a level, and on the title screen while its map flyby is up (the same
// condition D_Display uses for the desktop). Everything else (intermission,
// cutscenes) stays a full flat frame on the floating screen.
boolean VR_WorldVisible(void)
{
	// Theater mode watches the flat frame on the floating screen instead of
	// standing inside the world; the eye renders drop to the calm backdrop and
	// the capture takes the whole finished frame (both key off this).
	if (gamestate == GS_LEVEL) return !VR_TheaterActive();
	if (gamestate == GS_TITLESCREEN && titlemapinaction && curbghide && !hidetitlemap) return true;
	return false;
}

boolean VR_TheaterActive(void)
{
	return VR_IsActive() && gamestate == GS_LEVEL && cv_vrviewmode.value == VR_VIEW_THEATER;
}

float VR_WorldScaleMul(void)
{
	if (VR_IsActive() && cv_vrviewmode.value == VR_VIEW_DIORAMA)
	{
		float m = (float)atof(cv_vrdioramascale.string);
		return (m < 1.0f) ? 1.0f : m;
	}
	return 1.0f;
}

void VR_DioramaPark(float *backM, float *upM)
{
	*backM = *upM = 0.0f;
	if (!VR_IsActive() || cv_vrviewmode.value != VR_VIEW_DIORAMA)
		return;
	*backM = (float)atof(cv_vrdioramadist.string);
	*upM   = (float)atof(cv_vrdioramaheight.string);
}

// The stereo modes own the camera: Third Person and Diorama want the chase
// cam, First Person wants it off, Theater leaves the flat frame's camera
// alone. Ran on every mode change AND per input frame, so a mode picked in
// the menus applies once a level starts and console chasecam flips fold back.
static void VR_EnforceViewCamera(void)
{
	int want = -1;
	if (cv_vrviewmode.value == VR_VIEW_FIRST_PERSON) want = 0;
	else if (cv_vrviewmode.value != VR_VIEW_THEATER) want = 1;
	if (want >= 0 && gamestate == GS_LEVEL && cv_chasecam.value != want)
		CV_SetValue(&cv_chasecam, want);
}

static void VR_ViewModeChanged(void)
{
	static const char *names[] = { "Third Person", "First Person", "Theater", "Diorama" };
	if (!VR_IsActive())
		return; // config load / flat play: apply when a session is up
	VR_EnforceViewCamera();
#ifdef SRB2_HAVE_OPENXR
	s_stereoRamp = 0.0f; // new framing is a cut: fade the depth back in
#endif
	if (gamestate == GS_LEVEL)
	{
		// Name the new mode center-screen: the gamepad cycle has no rumble,
		// and Third vs First can look alike for a beat -- without this a
		// working press reads as a dead button.
		HU_SetCEchoDuration(2);
		HU_DoCEcho(names[cv_vrviewmode.value & 3]);
	}
	CONS_Printf("[VR] view: %s\n", names[cv_vrviewmode.value & 3]);
}

// The in-game cycle skips Theater: it's a deliberate menu pick, and landing on
// a flat screen mid-run reads as "VR broke". The menu row still offers it.
void VR_CycleViewMode(void)
{
	INT32 next = (cv_vrviewmode.value + 1) & 3;
	if (next == VR_VIEW_THEATER)
		next = (next + 1) & 3;
	CV_SetValue(&cv_vrviewmode, next); // fires VR_ViewModeChanged
}

// How far the movement stick should be rotated out of body space, in degrees.
// Zero whenever the option is Off, VR isn't running, the eyes aren't showing
// the world (Theater is the flat frame), or the current view mode isn't one the
// setting covers -- so G_BuildTiccmd can rotate unconditionally by whatever
// comes back. This is the same head yaw HWR_SetupView adds to the camera, so
// the movement and the picture can never disagree about where "forward" is.
float VR_MoveYaw(void)
{
	if (!cv_vrheadmove.value || !VR_IsActive() || VR_TheaterActive())
		return 0.0f;
	if (cv_vrheadmove.value == 1 && cv_vrviewmode.value != VR_VIEW_FIRST_PERSON)
		return 0.0f;
	return g_vrYaw;
}

boolean VR_SnapTurnActive(void)
{
	return cv_vrsnapturn.value != 0 && VR_IsActive() && !VR_TheaterActive();
}

// One tic of the look stick under snap turning. Press past 60% of the axis to
// fire a step; the stick must fall back under 40% before the next one -- the
// same hysteresis the triggers use, so a thumb parked at the edge turns once
// instead of spinning. Stick right is a right turn and angleturn counts the
// other way, hence the negation.
INT16 VR_SnapTurnStep(INT32 lookaxis)
{
	static boolean armed = true; // false while the stick is still deflected
	INT32 mag = abs(lookaxis);
	INT32 step;

	if (!VR_SnapTurnActive())
	{
		armed = true;
		return 0;
	}
	if (mag < (JOYAXISRANGE*2)/5) // under 40%: ready for the next step
		armed = true;
	if (!armed || mag < (JOYAXISRANGE*3)/5) // past 60%: fire
		return 0;
	armed = false;
	VR_Rumble(0.30f, 0.05f); // felt tick: the turn landed
	step = (cv_vrsnapturn.value * 65536) / 360; // degrees -> angleturn units
	return (INT16)(lookaxis > 0 ? -step : step);
}

// Gamepad shortcut, checked before anything else consumes the event: Select
// (Back) or D-pad Up cycles the VR view while playing. Both keys keep their
// stock jobs whenever this passes on the event -- Select stays screenshot on
// flatscreen, D-pad Up stays Toss Flag in the gametypes that throw things.
boolean VR_GamepadResponder(event_t *ev)
{
	boolean dpadup;

	if (!VR_IsActive() || gamestate != GS_LEVEL || menuactive || CON_Ready())
		return false;
	if (ev->type != ev_keydown && ev->type != ev_keyup)
		return false;
	dpadup = (ev->key == KEY_HAT1 + 0);
	if (ev->key != KEY_JOY1 + 6 && !dpadup)
		return false;
	if (dpadup && (gametyperules & (GTR_TEAMFLAGS|GTR_POWERSTONES)))
		return false;
	if (ev->type == ev_keydown)
		VR_CycleViewMode();
	return true; // eat the keyup too so screenshot/tossflag never trigger
}

static void VR_OnModeChange(void)
{
	CONS_Printf("[VR] vr_mode = %d\n", cv_vrmode.value);
	// Live toggle for convenience: booting needs a live GL context, so an early
	// (pre-graphics) callback during config load is a safe no-op (VR_Init bails),
	// and the i_video startup hook boots it once the GL context exists.
	if (VR_WantVR()) VR_Init();
	else VR_Shutdown();
}

void VR_RegisterCvars(void)
{
	CV_RegisterVar(&cv_vrmode);
	CV_RegisterVar(&cv_vrscale);
	CV_RegisterVar(&cv_vrscreendist);
	CV_RegisterVar(&cv_vrscreensize);
	CV_RegisterVar(&cv_vrhud);
	CV_RegisterVar(&cv_vrhudalpha);
	CV_RegisterVar(&cv_vrcrosshair);
	CV_RegisterVar(&cv_vrmirror);
	CV_RegisterVar(&cv_vrmsaa);
	CV_RegisterVar(&cv_vrviewmode);
	CV_RegisterVar(&cv_vrthirddist);
	CV_RegisterVar(&cv_vrdioramascale);
	CV_RegisterVar(&cv_vrdioramadist);
	CV_RegisterVar(&cv_vrdioramaheight);
	CV_RegisterVar(&cv_vrstereo);
	CV_RegisterVar(&cv_vrposefix);
	CV_RegisterVar(&cv_vrheadscale);
	CV_RegisterVar(&cv_vrrenderscale);
	CV_RegisterVar(&cv_vrculling);
	CV_RegisterVar(&cv_vrwateratmo);
	CV_RegisterVar(&cv_vrbubbles);
	CV_RegisterVar(&cv_vrwatermuffle);
	CV_RegisterVar(&cv_vrwatersway);
	CV_RegisterVar(&cv_vrripple);
	CV_RegisterVar(&cv_vrwaterfog);
	CV_RegisterVar(&cv_vrcaustics);
	CV_RegisterVar(&cv_vrwaterdebug);
	CV_RegisterVar(&cv_vrgodrays);
	CV_RegisterVar(&cv_vrsticklook);
	CV_RegisterVar(&cv_vrheadmove);
	CV_RegisterVar(&cv_vrsnapturn);
	CV_RegisterVar(&cv_vrmenudim);
	CV_RegisterVar(&cv_vrunlockall);
	COM_AddCommand("vr_recenter", Command_VRRecenter_f, COM_LUA);
	CONS_Printf("[VR] cvars registered (vr_mode, vr_scale, vr_screen*, vr_hud*)\n");
}

#ifdef SRB2_HAVE_OPENXR
static void VR_DestroyEyeTargets(void)
{
	if (s_msFBO && p_glDeleteFramebuffers) { p_glDeleteFramebuffers(1, &s_msFBO); s_msFBO = 0; }
	if (s_msColorRB && p_glDeleteRenderbuffers) { p_glDeleteRenderbuffers(1, &s_msColorRB); s_msColorRB = 0; }
	if (s_msDepthRB && p_glDeleteRenderbuffers) { p_glDeleteRenderbuffers(1, &s_msDepthRB); s_msDepthRB = 0; }
	if (s_rtFBO && p_glDeleteFramebuffers) { p_glDeleteFramebuffers(1, &s_rtFBO); s_rtFBO = 0; }
	if (s_rtTex) { glDeleteTextures(1, &s_rtTex); s_rtTex = 0; }
	if (s_rtDepthRB && p_glDeleteRenderbuffers) { p_glDeleteRenderbuffers(1, &s_rtDepthRB); s_rtDepthRB = 0; }
}

// (Re)build the off-swapchain eye targets to match vr_msaa and vr_renderscale.
// Both eyes use them sequentially. Three shapes:
//   MSAA at full scale     -> multisampled target, resolve straight into the eye image
//   MSAA at another scale  -> multisampled target + single-sample hop (a multisample
//                             resolve blit cannot rescale, so resolve 1:1 then scale)
//   scaled without MSAA    -> single-sample target (with depth: it IS the render
//                             target), scale-blit into the eye image
// Anything missing on the driver just leaves the FBOs at 0 and the eyes render
// straight into the swapchain like before - degraded, never a failure.
static void VR_EnsureEyeTargets(void)
{
	int want = cv_vrmsaa.value;
	INT32 pct = cv_vrrenderscale.value;
	boolean scaled;
	if (want == s_msSamples && pct == s_rtScalePct)
		return;
	VR_DestroyEyeTargets();
	s_msSamples = want;
	s_rtScalePct = pct;
	s_renderW = (INT32)s_eye[0].w;
	s_renderH = (INT32)s_eye[0].h;
	if (!p_glBlitFramebuffer || !p_glCheckFramebufferStatus || s_eye[0].w == 0)
		return;
	if (pct != 100)
	{
		s_renderW = (INT32)(s_eye[0].w * pct / 100);
		s_renderH = (INT32)(s_eye[0].h * pct / 100);
		if (s_renderW < 64) s_renderW = 64;
		if (s_renderH < 64) s_renderH = 64;
	}
	scaled = (s_renderW != (INT32)s_eye[0].w || s_renderH != (INT32)s_eye[0].h);

	if (want > 0 && p_glRenderbufferStorageMultisample)
	{
		GLint maxs = 0;
		glGetIntegerv(GL_MAX_SAMPLES, &maxs);
		while (glGetError() != GL_NO_ERROR) {} // enum absent pre-GL3: don't leave the error queued
		if (maxs > 0 && want > maxs) want = maxs;

		p_glGenRenderbuffers(1, &s_msColorRB);
		p_glBindRenderbuffer(GL_RENDERBUFFER, s_msColorRB);
		// Plain RGBA8, NOT the swapchain's (usually sRGB) format: it must
		// match the hop texture below -- mismatched formats make the
		// multisample resolve blit a GL_INVALID_OPERATION -- and the hop's
		// comment explains why neither of them may be sRGB.
		p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, want, GL_RGBA8,
			(GLsizei)s_renderW, (GLsizei)s_renderH);
		p_glGenRenderbuffers(1, &s_msDepthRB);
		p_glBindRenderbuffer(GL_RENDERBUFFER, s_msDepthRB);
		p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, want, GL_DEPTH_COMPONENT24,
			(GLsizei)s_renderW, (GLsizei)s_renderH);
		p_glBindRenderbuffer(GL_RENDERBUFFER, 0);
		p_glGenFramebuffers(1, &s_msFBO);
		p_glBindFramebuffer(GL_FRAMEBUFFER, s_msFBO);
		p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s_msColorRB);
		p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_msDepthRB);
		if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		{
			p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
			VR_DestroyEyeTargets();
			CONS_Printf("[VR] MSAA %dx eye buffer unavailable on this driver\n", want);
		}
		else
			CONS_Printf("[VR] MSAA %dx eye buffer ready\n", want);
	}

	// Single-sample hop/render target. Created unconditionally: it is the
	// render-scale surface, the MSAA resolve destination, AND the sampleable
	// source the eye-pass screen effect (CRT) draws from -- a swapchain
	// image can be blitted into but not sampled while bound as the target.
	// Costs one eye-sized texture; the no-effect unscaled path pays one
	// extra 1:1 blit, which is noise next to the eye render itself.
	{
		glGenTextures(1, &s_rtTex);
		glBindTexture(GL_TEXTURE_2D, s_rtTex);
		// Plain RGBA8, never the swapchain's sRGB format. A blit moves raw
		// bytes, but the eye-pass screen effect SAMPLES this texture -- and
		// sampling an sRGB texture makes the driver decode sRGB to linear in
		// hardware. The game's output is already gamma-encoded, so that
		// stealth decode (on top of the shader's own) crushed the whole
		// image dark in-headset whenever the effect was on. As a byte
		// container the sample returns exactly what the blit used to move,
		// and the effect path matches the flat screen bit for bit.
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)s_renderW, (GLsizei)s_renderH,
			0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_2D, 0);
		p_glGenRenderbuffers(1, &s_rtDepthRB);
		p_glBindRenderbuffer(GL_RENDERBUFFER, s_rtDepthRB);
		p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)s_renderW, (GLsizei)s_renderH);
		p_glBindRenderbuffer(GL_RENDERBUFFER, 0);
		p_glGenFramebuffers(1, &s_rtFBO);
		p_glBindFramebuffer(GL_FRAMEBUFFER, s_rtFBO);
		p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_rtTex, 0);
		p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_rtDepthRB);
		if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		{
			// No hop buffer: fall back to rendering straight into the
			// swapchain at its native size (no scale, no eye-pass effect).
			p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
			VR_DestroyEyeTargets();
			s_renderW = (INT32)s_eye[0].w;
			s_renderH = (INT32)s_eye[0].h;
			CONS_Printf("[VR] eye hop buffer unavailable on this driver\n");
			return;
		}
		if (scaled)
			CONS_Printf("[VR] eye render %dx%d (%d%% of %ux%u)\n",
				s_renderW, s_renderH, pct, s_eye[0].w, s_eye[0].h);
	}
	p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
#endif

// ---------------- public API ----------------
void VR_Init(void)
{
#ifdef SRB2_HAVE_OPENXR
	if (!VR_WantVR()) return;
	if (s_xrInstance != XR_NULL_HANDLE) return; // already booted (idempotent)
	if (!wglGetCurrentContext()) return;        // too early; the i_video hook retries after GL is ready

	CONS_Printf("[VR] Initializing OpenXR...\n");
	if (!VR_CreateInstance())   { VR_Shutdown(); return; }
	if (!VR_GetSystem())        { VR_Shutdown(); return; }
	if (!VR_CreateSession())    { VR_Shutdown(); return; }
	if (!VR_CreateActions())    // controllers are optional: keep VR alive without them
		CONS_Printf("[VR] controller input unavailable (head tracking still active)\n");
	if (!VR_EnumViews())        { VR_Shutdown(); return; }
	if (!VR_CreateSwapchains()) { VR_Shutdown(); return; }
	if (!VR_LoadGLFunctions()) { CONS_Printf("[VR] failed to load GL framebuffer functions\n"); VR_Shutdown(); return; }
	p_glGenFramebuffers(1, &s_fbo);
	// Depth renderbuffer for the eye FBO (the 3D world render needs depth). Both eyes share it (drawn sequentially).
	p_glGenRenderbuffers(1, &s_depthRB);
	p_glBindRenderbuffer(GL_RENDERBUFFER, s_depthRB);
	p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)s_eye[0].w, (GLsizei)s_eye[0].h);
	p_glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
	p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_depthRB);
	p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
	p_glBindRenderbuffer(GL_RENDERBUFFER, 0);
	CONS_Printf("[VR] OpenXR initialized; waiting for session to start.\n");
#endif
}

void VR_Shutdown(void)
{
#ifdef SRB2_HAVE_OPENXR
	VR_DestroyEyeTargets();
	s_msSamples = -1;
	s_rtScalePct = -1;
	if (s_depthRB && p_glDeleteRenderbuffers) { p_glDeleteRenderbuffers(1, &s_depthRB); s_depthRB = 0; }
	if (s_fbo && p_glDeleteFramebuffers) { p_glDeleteFramebuffers(1, &s_fbo); s_fbo = 0; }
	if (s_uiCopyFBO && p_glDeleteFramebuffers) { p_glDeleteFramebuffers(1, &s_uiCopyFBO); s_uiCopyFBO = 0; }
	if (s_uiFBO && p_glDeleteFramebuffers) { p_glDeleteFramebuffers(1, &s_uiFBO); s_uiFBO = 0; }
	if (s_uiTex) { glDeleteTextures(1, &s_uiTex); s_uiTex = 0; s_uiTexW = s_uiTexH = 0; }
	if (s_mirrorTex) { glDeleteTextures(1, &s_mirrorTex); s_mirrorTex = 0; }
	s_mirrorW = s_mirrorH = 0;
	s_mirrorValid = s_mirrorUIValid = false;
	if (s_ui.handle != XR_NULL_HANDLE) { xrDestroySwapchain(s_ui.handle); s_ui.handle = XR_NULL_HANDLE; }
	if (s_ui.images) { free(s_ui.images); s_ui.images = NULL; }
	s_ui.imgCount = 0; s_ui.w = s_ui.h = 0;
	s_uiOverlayActive = s_uiHaveOverlay = s_uiCaptured = false;
	s_uiAnchorValid = false;
	s_uiOffscreenSince = 0;
	for (int e = 0; e < 2; e++)
	{
		if (s_eye[e].handle != XR_NULL_HANDLE) { xrDestroySwapchain(s_eye[e].handle); s_eye[e].handle = XR_NULL_HANDLE; }
		if (s_eye[e].images) { free(s_eye[e].images); s_eye[e].images = NULL; }
		s_eye[e].imgCount = 0;
	}
	if (s_actionSet != XR_NULL_HANDLE)
	{
		VR_ReleaseAllKeys(); // no keyup will ever come from a destroyed action set
		xrDestroyActionSet(s_actionSet);
		s_actionSet = XR_NULL_HANDLE;
	}
	s_actMove = s_actLook = s_actBtnA = s_actBtnB = s_actBtnX = s_actBtnY = s_actMenuBtn
		= s_actLStick = s_actRStick = s_actLTrigger = s_actRTrigger = s_actLGrip = s_actRGrip
		= s_actHaptic = XR_NULL_HANDLE;
	s_handPath[0] = s_handPath[1] = XR_NULL_PATH;
	if (s_viewRefSpace != XR_NULL_HANDLE) { xrDestroySpace(s_viewRefSpace); s_viewRefSpace = XR_NULL_HANDLE; }
	if (s_xrSpace      != XR_NULL_HANDLE) { xrDestroySpace(s_xrSpace); s_xrSpace = XR_NULL_HANDLE; }
	if (s_xrSession    != XR_NULL_HANDLE) { xrDestroySession(s_xrSession); s_xrSession = XR_NULL_HANDLE; }
	if (s_xrInstance   != XR_NULL_HANDLE) { xrDestroyInstance(s_xrInstance); s_xrInstance = XR_NULL_HANDLE; }
	s_xrRunning = false;
	s_viewCount = 0;
	CONS_Printf("[VR] Shutdown.\n");
#endif
}

boolean VR_IsActive(void)
{
#ifdef SRB2_HAVE_OPENXR
	return s_xrRunning;
#else
	return false;
#endif
}

#ifdef SRB2_HAVE_OPENXR
// ---- controller -> SRB2 events ----
// VR controller state becomes ordinary SRB2 input events, so every existing
// responder (menus, console, gameplay bindings) just works. Buttons mirror the
// synthetic gamepad SRB2 already has defaults for (g_input.c):
//   sticks   -> ev_joystick axis pairs 0 (move; doubles as the menu d-pad via
//               M_Responder's built-in pair-0 handling) and 1 (turn/look)
//   A        -> KEY_JOY1+0 (GC_JUMP; menu accept)
//   B        -> KEY_JOY1+2 (GC_SPIN) in game, KEY_JOY1+1 (back) in menus, so
//               jump and spin share the right hand like the pad layout intends
//   X        -> KEY_JOY1+1 (GC_CUSTOM1; menu back)
//   Y        -> KEY_JOY1+3 (GC_CUSTOM2)
//   menu     -> KEY_ESCAPE in menus / KEY_JOY1+7 in game (GC_SYSTEMMENU)
//   triggers -> ev_joystick pair 2: fire (right, Z-Rudder = joyaxis_fire default)
//               and fire normal (left, Y-Rudder); in menus they double as accept/back
//   L grip   -> KEY_HAT1+0 ("D-Pad Up": GC_TOSSFLAG)
//   R grip   -> KEY_JOY1+5 ("RB": GC_CENTERVIEW, hold for lock-on in simple mode)
//   L stick click -> KEY_JOY1+8 (GC_CUSTOM3); in menus it recenters the screen
//   R stick click -> cycles the VR view mode (Third/First Person, Theater, Diorama)
static INT32 s_keySentA = 0, s_keySentB = 0, s_keySentX = 0, s_keySentY = 0,
	s_keySentMenu = 0, s_keySentLStick = 0,
	s_keySentLTrig = 0, s_keySentRTrig = 0, s_keySentLGrip = 0, s_keySentRGrip = 0;
static boolean s_prevStickClick = false;
static boolean s_prevRStickClick = false;
static XrTime s_rstickClickAt = 0; // first click of the view-cycle double click
static boolean s_prevRTrigMenu = false;  // edge state: right trigger's VR Options jump
static boolean s_prevLGripMenu = false, s_prevRGripMenu = false; // edge state: grip section paging
static boolean s_trigHeld[2] = { false, false }; // analog latch state [left, right]
static boolean s_gripHeld[2] = { false, false };
static INT8   s_menuDigX = 0, s_menuDigY = 0; // menu d-pad digital state (-1/0/1)
static XrTime s_menuDigSince = 0;             // when that state was last entered
static boolean s_stickWasLive[3] = { false, false, false }; // posted a nonzero pair last frame
static float  s_rumbleAmp = 0.0f;  // armed rumble amplitude (0 = off)
static XrTime s_rumbleUntil = 0;   // stop re-arming past this time

static boolean VR_GetBool(XrAction a)
{
	XrActionStateBoolean st = { XR_TYPE_ACTION_STATE_BOOLEAN };
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	if (a == XR_NULL_HANDLE) return false;
	gi.action = a;
	if (!XR_SUCCEEDED(xrGetActionStateBoolean(s_xrSession, &gi, &st))) return false;
	return st.isActive && st.currentState;
}

static void VR_GetVec2(XrAction a, float *x, float *y)
{
	XrActionStateVector2f st = { XR_TYPE_ACTION_STATE_VECTOR2F };
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	*x = *y = 0.0f;
	if (a == XR_NULL_HANDLE) return;
	gi.action = a;
	if (!XR_SUCCEEDED(xrGetActionStateVector2f(s_xrSession, &gi, &st))) return;
	if (st.isActive) { *x = st.currentState.x; *y = st.currentState.y; }
}

static float VR_GetFloat(XrAction a)
{
	XrActionStateFloat st = { XR_TYPE_ACTION_STATE_FLOAT };
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	if (a == XR_NULL_HANDLE) return 0.0f;
	gi.action = a;
	if (!XR_SUCCEEDED(xrGetActionStateFloat(s_xrSession, &gi, &st))) return 0.0f;
	return st.isActive ? st.currentState : 0.0f;
}

// Analog trigger/grip to a digital button with hysteresis: press past 60%,
// release under 40%, so a finger resting lightly on the trigger can't flicker
// the bound action.
static boolean VR_AnalogLatch(float v, boolean held)
{
	return held ? (v > 0.4f) : (v > 0.6f);
}

static float VR_StickDeadzone(float v)
{
	return (v > -0.15f && v < 0.15f) ? 0.0f : v;
}

// One axis of the menu d-pad: press past 60%, release under 40% (the trigger
// hysteresis), so a thumb resting near the edge can't flutter the cursor.
static INT8 VR_MenuDigital(float v, INT8 held)
{
	if (held > 0) return (v >  0.4f) ? 1 : 0;
	if (held < 0) return (v < -0.4f) ? -1 : 0;
	if (v >  0.6f) return 1;
	if (v < -0.6f) return -1;
	return 0;
}

static void VR_PostKey(evtype_t type, INT32 key)
{
	event_t ev;
	ev.type = type; ev.key = key; ev.x = ev.y = 0; ev.repeated = false;
	D_PostEvent(&ev);
}

// Edge-translate a boolean action into a key press; the release always releases
// whatever key the press sent, even if the menu/game context flipped meanwhile.
static void VR_ButtonToKey(boolean down, INT32 wantKey, INT32 *sentKey)
{
	if (down && *sentKey == 0)       { VR_PostKey(ev_keydown, wantKey); *sentKey = wantKey; }
	else if (!down && *sentKey != 0) { VR_PostKey(ev_keyup, *sentKey); *sentKey = 0; }
}

// Release every synthetic key and zero the posted axis pairs. Runs when input
// focus is lost (headset off, runtime menu up) and at shutdown, so nothing a
// hand was holding at that moment can stay stuck down in the game.
static void VR_ReleaseAllKeys(void)
{
	INT32 *sent[] = {
		&s_keySentA, &s_keySentB, &s_keySentX, &s_keySentY, &s_keySentMenu,
		&s_keySentLStick, &s_keySentLTrig, &s_keySentRTrig,
		&s_keySentLGrip, &s_keySentRGrip
	};
	size_t i;
	event_t ev;
	for (i = 0; i < sizeof(sent)/sizeof(sent[0]); i++)
		VR_ButtonToKey(false, 0, sent[i]);
	ev.type = ev_joystick; ev.repeated = false;
	ev.x = ev.y = 0;
	for (i = 0; i < sizeof(s_stickWasLive)/sizeof(s_stickWasLive[0]); i++)
	{
		if (!s_stickWasLive[i]) continue;
		ev.key = (INT32)i;
		D_PostEvent(&ev);
		s_stickWasLive[i] = false;
	}
	s_trigHeld[0] = s_trigHeld[1] = s_gripHeld[0] = s_gripHeld[1] = false;
	s_prevStickClick = false;
	s_prevRStickClick = false;
	s_rstickClickAt = 0;
	s_prevRTrigMenu = false;
	s_prevLGripMenu = s_prevRGripMenu = false;
	s_menuDigX = s_menuDigY = 0;
	s_rumbleAmp = 0.0f;
}

static void VR_ProcessInput(void)
{
	XrActiveActionSet aas;
	XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
	float mx, my, lx, ly, lt, rt;
	boolean menuish;

	if (s_actionSet == XR_NULL_HANDLE)
		return;
	if (s_xrState != XR_SESSION_STATE_FOCUSED)
	{
		VR_ReleaseAllKeys();
		return;
	}
	aas.actionSet = s_actionSet;
	aas.subactionPath = XR_NULL_PATH;
	si.countActiveActionSets = 1;
	si.activeActionSets = &aas;
	// XR_SESSION_NOT_FOCUSED is a success code that means "no input for you"
	// (headset off, runtime menu up); anything but a full success releases
	// everything so a button held at that moment can't stay stuck down.
	if (xrSyncActions(s_xrSession, &si) != XR_SUCCESS)
	{
		VR_ReleaseAllKeys();
		return;
	}

	menuish = menuactive || gamestate != GS_LEVEL || paused;

	VR_GetVec2(s_actMove, &mx, &my);
	VR_GetVec2(s_actLook, &lx, &ly);
	if (menuish)
	{
		INT8 digX, digY;
		// Either stick navigates menus; fold the right stick into pair 0.
		if (fabsf(lx) > fabsf(mx)) mx = lx;
		if (fabsf(ly) > fabsf(my)) my = ly;
		lx = ly = 0.0f;
		// One flick = one item. M_Responder repeats fast while it keeps
		// receiving a held axis (NEWTICRATE/17 left/right), so a ~150 ms human
		// tap lands 2-3 items. Digitize the stick, post the value once on the
		// edge, stay silent through a hold delay, then stream the held value
		// again and let M_Responder's own repeat sweep lists and sliders.
		digX = VR_MenuDigital(mx, s_menuDigX);
		digY = VR_MenuDigital(my, s_menuDigY);
		if (digX != s_menuDigX || digY != s_menuDigY)
		{
			s_menuDigX = digX; s_menuDigY = digY;
			s_menuDigSince = s_frameState.predictedDisplayTime;
		}
		else if (s_frameState.predictedDisplayTime - s_menuDigSince < 400000000LL) // 0.4 s
			digX = digY = 0;
		mx = (float)digX; my = (float)digY;
	}

	// Analog triggers and grips; the latched state drives their digital keys.
	lt = VR_GetFloat(s_actLTrigger);
	rt = VR_GetFloat(s_actRTrigger);
	s_trigHeld[0] = VR_AnalogLatch(lt, s_trigHeld[0]);
	s_trigHeld[1] = VR_AnalogLatch(rt, s_trigHeld[1]);
	s_gripHeld[0] = VR_AnalogLatch(VR_GetFloat(s_actLGrip), s_gripHeld[0]);
	s_gripHeld[1] = VR_AnalogLatch(VR_GetFloat(s_actRGrip), s_gripHeld[1]);

	// Opt-in stick pitch: hold the right stick up/down to glance without
	// craning. The offset eases toward the deflection and springs back to
	// level the moment the stick (or the option) lets go.
	{
		float pitchTgt = 0.0f;
		if (cv_vrsticklook.value && !menuish && gamestate == GS_LEVEL)
			pitchTgt = VR_StickDeadzone(ly) * 50.0f;
		g_vrStickPitch += (pitchTgt - g_vrStickPitch) * 0.10f;
	}

	{
		// Post a pair only while the VR stick is live (plus one zero event on
		// release) so an idle VR controller can't clobber a real gamepad's axes.
		event_t ev;
		ev.type = ev_joystick; ev.repeated = false;
		ev.key = 0; // pair 0: move (menu d-pad); OpenXR +y is forward, SRB2 wants it negative
		ev.x = (INT32)(VR_StickDeadzone(mx) * 1023.0f);
		ev.y = (INT32)(-VR_StickDeadzone(my) * 1023.0f);
		if (ev.x || ev.y || s_stickWasLive[0])
		{
			s_stickWasLive[0] = (ev.x || ev.y);
			D_PostEvent(&ev);
		}
		ev.key = 1; // pair 1: turn (cv_turnaxis default Z-Axis) / look
		ev.x = (INT32)(VR_StickDeadzone(lx) * 1023.0f);
		ev.y = (INT32)(-VR_StickDeadzone(ly) * 1023.0f);
		if (ev.x || ev.y || s_stickWasLive[1])
		{
			s_stickWasLive[1] = (ev.x || ev.y);
			D_PostEvent(&ev);
		}
		ev.key = 2; // pair 2: fire (right trigger, Z-Rudder) / fire normal (left, Y-Rudder)
		ev.x = menuish ? 0 : (INT32)(lt * 1023.0f);
		ev.y = menuish ? 0 : (INT32)(rt * 1023.0f);
		if (ev.x || ev.y || s_stickWasLive[2])
		{
			s_stickWasLive[2] = (ev.x || ev.y);
			D_PostEvent(&ev);
		}
	}

	VR_ButtonToKey(VR_GetBool(s_actBtnA), KEY_JOY1 + 0, &s_keySentA); // jump / accept
	VR_ButtonToKey(VR_GetBool(s_actBtnB), menuish ? (KEY_JOY1 + 1) : (KEY_JOY1 + 2), &s_keySentB); // spin / back
	VR_ButtonToKey(VR_GetBool(s_actBtnX), KEY_JOY1 + 1, &s_keySentX); // custom 1 / back
	VR_ButtonToKey(VR_GetBool(s_actBtnY), KEY_JOY1 + 3, &s_keySentY); // custom 2
	// The menu button is the pad's Start: on the save-slot screen it deletes
	// the hovered save (B backs out); everywhere else it closes/backs the menu.
	VR_ButtonToKey(VR_GetBool(s_actMenuBtn),
		menuish ? (M_SaveSelectActive() ? KEY_BACKSPACE : KEY_ESCAPE) : (KEY_JOY1 + 7), &s_keySentMenu);
	{ // right stick click: cycle the view mode in play, first press, every
	  // press. (A double-click guard lived here briefly against gamepad
	  // bridges landing shoulder buttons on the stick click -- the real
	  // culprit turned out to be the pad's old cam-toggle binding, long
	  // since unbound, and the guard only made the click feel dead.)
		boolean click = VR_GetBool(s_actRStick);
		if (!menuish && click && !s_prevRStickClick)
		{
			VR_CycleViewMode(); // Third -> First -> Diorama (Theater is a menu pick)
			VR_Rumble(0.25f, 0.06f); // felt tick: the cycle registered
		}
		s_prevRStickClick = click;
	}
	VR_EnforceViewCamera(); // camera follows the mode across level entry / console flips
	// Grip gameplay actions release when a menu opens mid-hold; in menus the
	// grips page between menu sections instead (left = up, right = down).
	// The right grip mirrors the X button's game action (squeezing beats
	// reaching for X mid-motion); center view / lock-on lives on the left
	// grip so the tutorial's hold-to-lock stays reachable -- except in the
	// gametypes where that grip throws the flag/emeralds.
	VR_ButtonToKey(!menuish && s_gripHeld[1], KEY_JOY1 + 1, &s_keySentRGrip); // custom 1, as X
	{
		boolean flagrules = (gametyperules & (GTR_TEAMFLAGS|GTR_POWERSTONES)) != 0;
		VR_ButtonToKey(!menuish && s_gripHeld[0],
			flagrules ? (KEY_HAT1 + 0) : (KEY_JOY1 + 5), &s_keySentLGrip); // toss flag / center view
	}
	if (menuish)
	{
		if (s_gripHeld[1] && !s_prevRGripMenu) M_MenuSectionJump(1);
		if (s_gripHeld[0] && !s_prevLGripMenu) M_MenuSectionJump(-1);
	}
	s_prevRGripMenu = menuish && s_gripHeld[1];
	s_prevLGripMenu = menuish && s_gripHeld[0];
	// The left trigger doubles as menu back so navigation doesn't demand the
	// face buttons. The right trigger: in the in-level pause menu it jumps
	// straight into VR Options (the live tuning panel); on the title menus it
	// stays menu accept. In game both are the fire axes above.
	{
		boolean vrOptsJump = menuish && gamestate == GS_LEVEL && s_trigHeld[1];
		if (vrOptsJump && !s_prevRTrigMenu)
			M_VROptionsShortcut();
		s_prevRTrigMenu = vrOptsJump;
	}
	VR_ButtonToKey(menuish && gamestate != GS_LEVEL && s_trigHeld[1], KEY_JOY1 + 0, &s_keySentRTrig);
	VR_ButtonToKey(menuish && s_trigHeld[0], KEY_JOY1 + 1, &s_keySentLTrig);

	{ // left stick click: custom 3 in play; in the UI it re-places the screen
		boolean click = VR_GetBool(s_actLStick);
		VR_ButtonToKey(!menuish && click, KEY_JOY1 + 8, &s_keySentLStick);
		if (menuish && click && !s_prevStickClick) VR_RecenterScreen();
		s_prevStickClick = click;
	}

	// Haptics: while armed, re-arm a SHORT burst every frame instead of ever
	// submitting one long vibration. Runtimes don't all honor stop requests
	// promptly (or at all, over wireless) - a long one-shot buzz that misses
	// its stop can never be cancelled. Short bursts die on their own right
	// after the last re-arm, so a lost stop can't strand the motor.
	if (s_rumbleAmp > 0.0f && s_actHaptic != XR_NULL_HANDLE
		&& s_frameState.predictedDisplayTime < s_rumbleUntil)
	{
		XrHapticVibration vib = { XR_TYPE_HAPTIC_VIBRATION };
		int h;
		vib.duration  = 60000000LL; // 60 ms: outlasts one frame, dies fast once re-arming stops
		vib.frequency = XR_FREQUENCY_UNSPECIFIED;
		vib.amplitude = s_rumbleAmp;
		for (h = 0; h < 2; h++)
		{
			XrHapticActionInfo hai = { XR_TYPE_HAPTIC_ACTION_INFO };
			hai.action = s_actHaptic;
			hai.subactionPath = s_handPath[h];
			xrApplyHapticFeedback(s_xrSession, &hai, (const XrHapticBaseHeader*)&vib);
		}
	}

	if (!menuish && cv_usejoystick.value == 0)
		cv_usejoystick.value = 1; // direct poke: CV_Set would re-probe SDL devices
}
#endif // SRB2_HAVE_OPENXR

void VR_BeginFrame(void)
{
#ifdef SRB2_HAVE_OPENXR
	// The audio mixer's underwater low-pass keys off this once per frame:
	// postimg_water is the game's own "the view is underwater" verdict (the
	// same one that triggers the flat screen's warp). Flatscreen gets the
	// muffle too -- it's the water talking, not the headset.
	g_vrWaterMuffle = (cv_vrwatermuffle.value
		&& gamestate == GS_LEVEL && postimgtype == postimg_water);
	if (s_xrSession == XR_NULL_HANDLE) return;
	VR_PollEvents();
	if (!s_xrRunning) return;
	VR_ProcessInput();

	XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
	s_frameState.type = XR_TYPE_FRAME_STATE;
	s_frameState.next = NULL;
	if (!XR_Ok(xrWaitFrame(s_xrSession, &wi, &s_frameState), "xrWaitFrame")) return;

	XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
	XR_Ok(xrBeginFrame(s_xrSession, &bi), "xrBeginFrame");
#endif
}

void VR_EndFrame(void)
{
#ifdef SRB2_HAVE_OPENXR
	if (!s_xrRunning) return;

	ps_vr_eyetime.value.p = 0; // perfstats "VR eye renders" row: fresh total each frame

	// The mirror only ever shows a LIVE eye frame: D_Display consumed last
	// frame's grab already (it runs before us), so drop validity here and let
	// this frame's grab re-arm it. If the eyes don't render (headset dozing,
	// Theater, menus), the desktop falls back to the flat render instead of
	// freezing on a stale eye image.
	s_mirrorValid = false;

	XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerProjectionView projViews[2];
	XrCompositionLayerQuad uiLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	const XrCompositionLayerBaseHeader *layers[2];
	uint32_t layerCount = 0;

	// The recenter anchor doubles as the body's neutral position: positional
	// tracking and the floating screen both measure from it, so make sure it
	// exists before any eye renders (vr_recenter re-zeros both together).
	if (!s_uiAnchorValid) VR_AnchorUIScreen();

	// A big viewpoint jump in one frame (death, starpost, teleporter) is a cut:
	// restart the stereo ramp so the new place fades to depth.
	if (gamestate == GS_LEVEL && players[displayplayer].mo)
	{
		mobj_t *mo = players[displayplayer].mo;
		if (s_lastMoValid
			&& (abs(mo->x - s_lastMoX) > 128*FRACUNIT
			 || abs(mo->y - s_lastMoY) > 128*FRACUNIT
			 || abs(mo->z - s_lastMoZ) > 128*FRACUNIT))
			s_stereoRamp = 0.0f;
		s_lastMoX = mo->x; s_lastMoY = mo->y; s_lastMoZ = mo->z;
		s_lastMoValid = true;
	}
	else
		s_lastMoValid = false;

	if (s_frameState.shouldRender && VR_WorldVisible())
		s_stereoRamp += 1.0f/45.0f;
	else
		s_stereoRamp -= 3.0f/45.0f;
	if (s_stereoRamp > 1.0f) s_stereoRamp = 1.0f;
	if (s_stereoRamp < 0.0f) s_stereoRamp = 0.0f;

	// Render a valid frame into the eye swapchains. SteamVR's compositor crashes if
	// it gets begun-but-empty frames, so we must submit a real projection layer.
	if (s_frameState.shouldRender && s_viewCount == 2 && s_fbo != 0)
	{
		XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
		vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		vli.displayTime = s_frameState.predictedDisplayTime;
		vli.space = s_xrSpace;
		XrViewState vstate = { XR_TYPE_VIEW_STATE };
		XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
		uint32_t nv = 0;
		float hcx = 0.0f, hcy = 0.0f, hcz = 0.0f; // head center (midpoint of the eyes)
		boolean ok = XR_SUCCEEDED(xrLocateViews(s_xrSession, &vli, &vstate, 2, &nv, views)) && nv == 2;

		if (ok)
		{
			hcx = 0.5f * (views[0].pose.position.x + views[1].pose.position.x);
			hcy = 0.5f * (views[0].pose.position.y + views[1].pose.position.y);
			hcz = 0.5f * (views[0].pose.position.z + views[1].pose.position.z);
		}

		VR_EnsureEyeTargets(); // rebuilds only when vr_msaa / vr_renderscale changed

		for (uint32_t e = 0; ok && e < 2; e++)
		{
			uint32_t idx = 0;
			XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
			if (!XR_SUCCEEDED(xrAcquireSwapchainImage(s_eye[e].handle, &ai, &idx))) { ok = false; break; }
			XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			swi.timeout = XR_INFINITE_DURATION;
			xrWaitSwapchainImage(s_eye[e].handle, &swi);

			// The eye renders into the multisampled target when MSAA is on, the
			// single-sample target when only the render scale differs, and
			// straight into the swapchain image otherwise.
			if (s_msFBO)
				p_glBindFramebuffer(GL_FRAMEBUFFER, s_msFBO);
			else if (s_rtFBO)
				p_glBindFramebuffer(GL_FRAMEBUFFER, s_rtFBO);
			else
			{
				p_glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
				p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_eye[e].images[idx].image, 0);
			}
			glViewport(0, 0, (GLsizei)s_renderW, (GLsizei)s_renderH);
			// Feed this eye's headset FOV to the renderer so SRB2 draws with the correct
			// per-eye projection (kills the "zoomed in" look) instead of its own FOV.
			g_vrFov[0] = views[e].fov.angleLeft;
			g_vrFov[1] = views[e].fov.angleRight;
			g_vrFov[2] = views[e].fov.angleUp;
			g_vrFov[3] = views[e].fov.angleDown;
			// Store this eye's pose (position incl. IPD + orientation) for the view matrix.
			g_vrEyePos[0] = views[e].pose.position.x;
			g_vrEyePos[1] = views[e].pose.position.y;
			g_vrEyePos[2] = views[e].pose.position.z;
			// Eye position relative to the recenter anchor: leaning/stepping moves the
			// camera, and the per-eye IPD difference gives real stereo depth. The two
			// halves scale separately: vr_headscale damps the lean/step (comfort),
			// vr_stereo scales the eye separation (0 = flat).
			if (s_uiAnchorValid)
			{
				const float hs = cv_vrheadscale.value / 100.0f;
				const float ss = (cv_vrstereo.value / 100.0f) * s_stereoRamp;
				g_vrEyeOff[0] = (hcx - s_uiAnchorPos[0])*hs + (views[e].pose.position.x - hcx)*ss;
				g_vrEyeOff[1] = (hcy - s_uiAnchorPos[1])*hs + (views[e].pose.position.y - hcy)*ss;
				g_vrEyeOff[2] = (hcz - s_uiAnchorPos[2])*hs + (views[e].pose.position.z - hcz)*ss;
			}
			else
				g_vrEyeOff[0] = g_vrEyeOff[1] = g_vrEyeOff[2] = 0.0f;
			g_vrEyeQuat[0] = views[e].pose.orientation.x;
			g_vrEyeQuat[1] = views[e].pose.orientation.y;
			g_vrEyeQuat[2] = views[e].pose.orientation.z;
			g_vrEyeQuat[3] = views[e].pose.orientation.w;
			{ // head yaw + pitch (degrees) from the orientation, fed into SRB2's own camera build
				const float qx=g_vrEyeQuat[0], qy=g_vrEyeQuat[1], qz=g_vrEyeQuat[2], qw=g_vrEyeQuat[3];
				const float fx = -2.0f*(qx*qz + qy*qw);          // head forward vector
				const float fy =  2.0f*(qx*qw - qy*qz);
				const float fz = -(1.0f - 2.0f*(qx*qx + qy*qy));
				float fyc = fy; if (fyc > 1.0f) fyc = 1.0f; if (fyc < -1.0f) fyc = -1.0f;
				g_vrYaw   = atan2f(-fx, -fz) * 57.29578f;
				g_vrPitch = asinf(fyc) * 57.29578f;
			}
			g_vrActive = true;
			g_vrEyeIndex = (INT32)e;
			{ // make SRB2 render at the eye target's resolution (it caches the desktop view size)
				INT32 vw = vid.width, vh = vid.height;
				INT32 oww = viewwidth, owh = viewheight, owx = viewwindowx, owy = viewwindowy;
				vid.width = s_renderW; vid.height = s_renderH;
				viewwidth = s_renderW; viewheight = s_renderH;
				viewwindowx = 0; viewwindowy = 0;
				SetVRViewport((int)s_renderW, (int)s_renderH);
				{ // the built-in render timer only sees the flat pass; this row is the eyes
					const precise_t t0 = I_GetPreciseTime();
					HWR_RenderPlayerEye();
					ps_vr_eyetime.value.p += I_GetPreciseTime() - t0;
				}
				vid.width = vw; vid.height = vh;
				viewwidth = oww; viewheight = owh; viewwindowx = owx; viewwindowy = owy;
				SetVRViewport((int)vw, (int)vh);
			}
			if (s_msFBO || s_rtFBO)
			{
				// Move the finished render into the eye's swapchain image.
				// Scissor off first: a blit honors it and would clip the copy.
				const boolean scaled = (s_renderW != (INT32)s_eye[e].w || s_renderH != (INT32)s_eye[e].h);
				glDisable(GL_SCISSOR_TEST);
				if (s_msFBO && s_rtFBO)
				{
					// A multisample resolve cannot rescale in one blit: resolve
					// 1:1 into the single-sample hop, then scale from there.
					p_glBindFramebuffer(GL_READ_FRAMEBUFFER, s_msFBO);
					p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_rtFBO);
					p_glBlitFramebuffer(0, 0, s_renderW, s_renderH,
						0, 0, s_renderW, s_renderH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
					p_glBindFramebuffer(GL_READ_FRAMEBUFFER, s_rtFBO);
				}
				else
					p_glBindFramebuffer(GL_READ_FRAMEBUFFER, s_msFBO ? s_msFBO : s_rtFBO);
				if (s_rtTex && HWR_VREyeEffectActive())
				{
					// Screen effect (CRT) on the eye itself: a blit can run
					// no shader, so draw the hop texture through the effect
					// shader instead -- the same filter the flat final blit
					// applies, at true per-eye resolution. (If MSAA is on,
					// the resolve above already landed the frame in the hop.)
					p_glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
					p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_eye[e].images[idx].image, 0);
					HWR_DrawVREyeEffect(s_rtTex, (int)s_eye[e].w, (int)s_eye[e].h);
					// The effect draw set the GL viewport to the eye and a
					// blit never would have. Everything after this point that
					// draws a fullscreen quad -- the desktop mirror, the HUD
					// overlay on it -- trusts the ambient viewport, and an
					// eye-sized viewport on a window-sized target scales the
					// whole UI huge and cropped. Put the desktop's back.
					SetVRViewport(vid.width, vid.height);
				}
				else
				{
					p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
					p_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_eye[e].images[idx].image, 0);
					p_glBlitFramebuffer(0, 0, s_renderW, s_renderH,
						0, 0, (GLint)s_eye[e].w, (GLint)s_eye[e].h, GL_COLOR_BUFFER_BIT,
						scaled ? GL_LINEAR : GL_NEAREST);
				}
				// Leave the finished image bound: the mirror grab below reads it.
				p_glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
			}
			// Snapshot the left eye for the desktop window while the image is still ours.
			// (Theater renders no world into the eyes; the window keeps its flat frame.)
			if (e == 0 && gamestate == GS_LEVEL && cv_vrmirror.value && !VR_TheaterActive())
				VR_MirrorGrab();
			g_vrActive = false;
			p_glBindFramebuffer(GL_FRAMEBUFFER, 0);

			XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			xrReleaseSwapchainImage(s_eye[e].handle, &ri);

			memset(&projViews[e], 0, sizeof(projViews[e]));
			projViews[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			projViews[e].pose = views[e].pose;
			if (cv_vrposefix.value)
			{
				// The eye image was rendered through SRB2's camera with the
				// head's yaw and pitch only (plus the opt-in stick glance) --
				// never roll. Submit THAT orientation, not the raw tracked
				// pose: claiming a roll the image doesn't carry makes the
				// compositor reproject every frame the head tilts. The frame
				// swims (sharpest as the menu wobbling against the pinned
				// quad), and with the head held at any natural slight tilt
				// the counter-rotated eye images pick up rotational/vertical
				// disparity -- the cross-eyed strain. With the honest pose
				// the compositor rolls the image itself: horizon, menu, and
				// fusion all hold. (vr_posefix Off = raw pose, for live A/B.)
				const float hy = (g_vrYaw * 0.5f) / 57.29578f;
				const float hp = ((g_vrPitch + g_vrStickPitch) * 0.5f) / 57.29578f;
				const float cy = cosf(hy), sy = sinf(hy), cp = cosf(hp), sp = sinf(hp);
				projViews[e].pose.orientation.x = cy * sp;
				projViews[e].pose.orientation.y = sy * cp;
				projViews[e].pose.orientation.z = -sy * sp;
				projViews[e].pose.orientation.w = cy * cp;
			}
			projViews[e].fov  = views[e].fov;
			projViews[e].subImage.swapchain = s_eye[e].handle;
			projViews[e].subImage.imageRect.extent.width  = (int32_t)s_eye[e].w;
			projViews[e].subImage.imageRect.extent.height = (int32_t)s_eye[e].h;
		}

		if (ok)
		{
			layer.space = s_xrSpace;
			layer.viewCount = 2;
			layer.views = projViews;
			layers[0] = (const XrCompositionLayerBaseHeader*)&layer;
			layerCount = 1;
		}
	}

	// The floating UI screen goes on top of the world layer (layers compose back-to-front).
	if (s_uiCaptured && s_ui.handle != XR_NULL_HANDLE)
	{
		float wm = (float)atof(cv_vrscreensize.string);
		if (wm < 0.5f) wm = 4.0f;
		VR_UIScreenFindability();
		if (!s_uiAnchorValid) VR_AnchorUIScreen();
		if (s_uiAnchorValid)
		{
			// Overlay content is premultiplied (accumulated over transparent black),
			// which is the compositor default: source-alpha bit only.
			uiLayer.layerFlags = s_uiOpaque ? 0 : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			uiLayer.space = s_xrSpace;
			uiLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			uiLayer.subImage.swapchain = s_ui.handle;
			uiLayer.subImage.imageRect.extent.width  = (int32_t)s_ui.w;
			uiLayer.subImage.imageRect.extent.height = (int32_t)s_ui.h;
			uiLayer.pose = VR_UIScreenPose();
			uiLayer.size.width  = wm;
			uiLayer.size.height = wm * (float)s_ui.h / (float)s_ui.w;
			layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&uiLayer;
		}
	}
	s_uiCaptured = false;

	XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
	ei.displayTime = s_frameState.predictedDisplayTime;
	ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	ei.layerCount = layerCount;
	ei.layers = layerCount ? layers : NULL;
	XR_Ok(xrEndFrame(s_xrSession, &ei), "xrEndFrame");
#endif
}

// ---------------- floating UI screen (public hooks) ----------------

// Bracket the in-level 2D overlay phase of D_Display: everything drawn between
// Begin and End (HUD, pause menu, console) lands in a transparent buffer.
void VR_UIOverlayBegin(void)
{
#ifdef SRB2_HAVE_OPENXR
	if (!s_xrRunning) return;
	// Theater: the vanilla flat frame (world + HUD + menus) IS the content;
	// skipping the redirect makes the capture take the whole frame opaque.
	if (VR_TheaterActive()) return;
	if (!VR_EnsureOverlayFBO()) return;
	p_glBindFramebuffer(GL_FRAMEBUFFER, s_uiFBO);
	p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_uiTex, 0);
	glPushAttrib(GL_COLOR_BUFFER_BIT);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glPopAttrib();
	s_uiOverlayActive = true;
#endif
}

void VR_UIOverlayEnd(void)
{
#ifdef SRB2_HAVE_OPENXR
	if (!s_uiOverlayActive) return;
	p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
	// Put the overlay back onto the desktop window so the mirror looks unchanged.
	VR_BlitTexFullscreen(s_uiTex, 1.0f, true);
	s_uiOverlayActive = false;
	s_uiHaveOverlay = true;
	s_mirrorUIValid = true;
#endif
}

// Called right before the window swap (ogl_sdl.c), after HWR_MakeScreenFinalTexture:
// copy this frame's UI into a swapchain image for the quad layer. Two sources:
// the transparent in-level overlay, or (outside a level) the whole finished frame.
void VR_CaptureUIFrame(void)
{
#ifdef SRB2_HAVE_OPENXR
	boolean overlay;
	uint32_t idx = 0;
	GLint vp[4];

	if (!s_xrRunning || !p_glGenFramebuffers)
	{ s_uiHaveOverlay = false; return; }

	overlay = s_uiHaveOverlay;
	s_uiHaveOverlay = false;
	// Boot frames are plain black; leaving them off the screen keeps the void
	// clean until the title actually has something to show.
	if (gamestate == GS_NULL)
		return;
	// While the world renders around the player (level or title flyby), only
	// transparent overlays belong on the panel: stray full frames (wipe loops
	// bypass the overlay) would flash it opaque black over the world.
	if (!overlay && VR_WorldVisible())
		return;
	// In a level the screen only shows when there's something to read on it.
	// (Title-flyby overlays always show: the logo and menu ARE the content.)
	if (overlay && gamestate == GS_LEVEL
		&& !(cv_vrhud.value || menuactive || paused || con_destlines > 0))
		return;
	if (!VR_EnsureUISwapchain()) return;

	{
		XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		if (!XR_SUCCEEDED(xrAcquireSwapchainImage(s_ui.handle, &ai, &idx))) return;
		swi.timeout = XR_INFINITE_DURATION;
		xrWaitSwapchainImage(s_ui.handle, &swi);
	}

	glGetIntegerv(GL_VIEWPORT, vp);
	p_glBindFramebuffer(GL_FRAMEBUFFER, s_uiCopyFBO);
	p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_ui.images[idx].image, 0);
	glViewport(0, 0, (GLsizei)s_ui.w, (GLsizei)s_ui.h);
	if (overlay)
	{
		glPushAttrib(GL_COLOR_BUFFER_BIT);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glPopAttrib();
		// Direct copy with the opacity setting folded into the texture alpha.
		// HUD opacity only dims the in-level HUD; title menus stay fully readable.
		VR_BlitTexFullscreen(s_uiTex,
			(gamestate == GS_LEVEL) ? (float)cv_vrhudalpha.value / 10.0f : 1.0f, false);
	}
	else
	{
		HWR_DrawScreenFinalTextureNoEffect((int)s_ui.w, (int)s_ui.h); // full frame incl. titlemap flyby; clean -- the quad gets resampled
	}
	p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(vp[0], vp[1], vp[2], vp[3]);

	{
		XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(s_ui.handle, &ri);
	}
	s_uiOpaque = !overlay;
	s_uiCaptured = true;
#endif
}

// Called right before the window swap, after VR_CaptureUIFrame: in-level, cover the
// flat body-camera frame with the last head-tracked eye render (center-cropped to
// fill the window) and put the HUD back on top the way the headset shows it. The
// mirror runs one frame behind the headset, which recording can't notice. Menus and
// the title screen keep the flat frame -- that's what the floating screen shows.
// Center-crop the mirror texture to the given aspect and blit it fullscreen
// into the currently bound framebuffer.
#ifdef SRB2_HAVE_OPENXR
static void VR_BlitMirrorCropped(float dstAspect)
{
	float u0 = 0.0f, u1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
	const float eyeAspect = (float)s_mirrorW / (float)s_mirrorH;
	if (eyeAspect > dstAspect)
	{ // eye image wider than the target: crop the sides
		const float band = dstAspect / eyeAspect;
		u0 = 0.5f - band*0.5f; u1 = 0.5f + band*0.5f;
	}
	else
	{ // target wider (the usual case): crop top/bottom, keeping the center
		const float band = eyeAspect / dstAspect;
		v0 = 0.5f - band*0.5f; v1 = 0.5f + band*0.5f;
	}
	VR_BlitTexRect(s_mirrorTex, u0, v0, u1, v1, 1.0f, false);
}
#endif

// In-level with the mirror on, the flat body-camera world render is dead work:
// the mirror covers it entirely every frame. D_Display calls this in place of
// that render; true = last frame's eye grab was just blitted as this frame's
// world layer (postprocessing, the intermission capture, and the HUD overlay
// all land on top of it normally). False = render the world the usual way
// (mirror off, Theater, splitscreen, no eye frame grabbed yet).
boolean VR_MirrorWorldLayer(void)
{
#ifdef SRB2_HAVE_OPENXR
	if (!s_xrRunning || !s_mirrorValid || !cv_vrmirror.value || gamestate != GS_LEVEL
		|| VR_TheaterActive() || splitscreen || !VR_WorldVisible()
		|| vid.width < 1 || vid.height < 1)
		return false;
	VR_BlitMirrorCropped((float)vid.width / (float)vid.height);
	s_mirrorWorldLayer = true;
	return true;
#else
	return false;
#endif
}

void VR_DrawDesktopMirror(void)
{
#ifdef SRB2_HAVE_OPENXR
	GLint vp[4];
	boolean hadUI = s_mirrorUIValid;
	boolean worldLayerDone = s_mirrorWorldLayer;

	s_mirrorUIValid = false;
	s_mirrorWorldLayer = false;
	if (worldLayerDone)
		return; // the frame already is the mirror (drawn as the world layer)
	if (!s_xrRunning || !s_mirrorValid || !cv_vrmirror.value || gamestate != GS_LEVEL
		|| VR_TheaterActive())
		return;

	glGetIntegerv(GL_VIEWPORT, vp);
	if (vp[2] < 1 || vp[3] < 1)
		return;
	VR_BlitMirrorCropped((float)vp[2] / (float)vp[3]);
	// Menus, pause, and the console draw at full strength on the monitor --
	// only the plain gameplay HUD takes the opacity slider.
	if (hadUI && s_uiTex && (cv_vrhud.value || menuactive || paused || con_destlines > 0))
		VR_BlitTexFullscreen(s_uiTex,
			(menuactive || paused || con_destlines > 0) ? 1.0f : (float)cv_vrhudalpha.value / 10.0f, true);
#endif
}

void VR_RecenterScreen(void)
{
#ifdef SRB2_HAVE_OPENXR
	s_uiAnchorValid = false;
#endif
}

static void Command_VRRecenter_f(void)
{
	VR_RecenterScreen();
}

// Arm controller rumble on both hands. No vibration is submitted here:
// VR_ProcessInput re-arms a short burst each frame while armed, so a runtime
// that mishandles stop requests can't strand the motor on.
void VR_Rumble(float strength, float seconds)
{
#ifdef SRB2_HAVE_OPENXR
	if (!s_xrRunning || s_actHaptic == XR_NULL_HANDLE)
		return;
	if (strength < 0.0f) strength = 0.0f;
	if (strength > 1.0f) strength = 1.0f;
	s_rumbleAmp = strength;
	s_rumbleUntil = s_frameState.predictedDisplayTime + (XrTime)(seconds * 1e9);
#else
	(void)strength;
	(void)seconds;
#endif
}

void VR_RumbleStop(void)
{
#ifdef SRB2_HAVE_OPENXR
	s_rumbleAmp = 0.0f;
	s_rumbleUntil = 0;
#endif
}
