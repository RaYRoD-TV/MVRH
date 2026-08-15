// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  vr_openxr.cpp
/// \brief OpenXR VR support.
///
/// Boots an OpenXR session bound to the game's SDL OpenGL context
/// (XR_USE_GRAPHICS_API_OPENGL + WGL on Windows), creates the swapchains and
/// runs the per-frame xrWaitFrame/xrBeginFrame/xrEndFrame loop. The first
/// output mode is the theater panel: the finished flat frame is blitted into
/// a swapchain and presented on a large head-locked quad, which proves the
/// whole instance/session/GL-binding/swapchain/submit spine with the simplest
/// possible image source. Per-eye stereo rendering builds on top of this.
///
/// Without ENABLE_VR this file compiles to nothing (vr.h provides the inline
/// no-ops instead).

#include "vr.h"

#ifdef ENABLE_VR

// --- request flag (platform-agnostic, set by the -vr CLI handler) ----------

static bool sRequested = false;
static bool sFakeRequested = false;
// Head pitch for the stand-in rig, radians (negative pitches the view down).
static float sFakePitchRad = 0.0f;

extern "C" void vr_request_enable(void)
{
	sRequested = true;
}

extern "C" void vr_request_fake(void)
{
	sFakeRequested = true;
}

extern "C" void vr_set_fake_pitch(float degrees_down)
{
	// Taken as degrees DOWN, because the command line cannot carry a leading
	// minus: M_IsNextParm rejects any value starting with '-' or '+', so a
	// signed argument silently parses as "absent" and the rig stays level.
	sFakePitchRad = -degrees_down * 0.01745329252f;
}

extern "C" bool vr_is_requested(void)
{
	return sRequested;
}

#if defined(_WIN32) || defined(__ANDROID__)

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h> // IUnknown - referenced by openxr_platform.h's XR_USE_PLATFORM_WIN32 structs

// The VR module drives GL directly (FBOs + blits) on the same context the
// engine renders with. The engine's RHI owns its own GladGLContext privately,
// so load a second one here from the same SDL proc loader - glad multi-context
// makes that free (two structs of function pointers into one driver).
#include <glad/gl.h>
#include <SDL3/SDL_video.h>

#define XR_USE_GRAPHICS_API_OPENGL
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#else // __ANDROID__

// ON THE HEADSET THE GAME'S GL IS NOT THE GL THIS FILE USES, and that is the
// whole trick to the Android arm.
//
// The renderer reaches GL only through function pointers it fetches from gl4es,
// which emulates desktop GL 2.1 (fixed function and all) on top of GLES. This
// file needs none of that emulation: it draws two quads and moves pixels
// between framebuffers, which GLES 3 does natively. So it calls the real driver
// straight, and both sit on the SAME EGL context, which is what makes the
// runtime's swapchain textures visible to each of them. Routing this file
// through gl4es instead would hand a texture the runtime created to a layer
// that only knows about textures it created itself.
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <jni.h> // before openxr_platform.h: its Android structs are declared in terms of jobject
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_system.h> // SDL_GetAndroidJNIEnv / SDL_GetAndroidActivity

#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_PLATFORM_ANDROID
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// The swapchain image struct differs only in name between the two bindings, so
// the frame path keeps one spelling and this maps it.
typedef XrSwapchainImageOpenGLESKHR XrSwapchainImageOpenGLKHR;
#define XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR

// GLES has no glDrawBuffer; the plural form does the same job for the one call
// that wants it (detaching colour while probing a depth-only attachment).
static inline void vr_gles_draw_buffer(GLenum buf)
{
	if (buf == GL_NONE)
	{
		const GLenum none = GL_NONE;
		glDrawBuffers(1, &none);
	}
	else
	{
		glDrawBuffers(1, &buf);
	}
}

// TWO CAPABILITIES GLES SIMPLY DOES NOT HAVE, and the eye pass saves and
// restores both around its work. Passing either enum to a GLES driver raises
// INVALID_ENUM, which would then be reported by the next error check as though
// something real had gone wrong. Filtering them here keeps the save/restore
// code identical on both platforms and keeps the error log honest: alpha
// testing is a shader concern here, and the sRGB write switch does not exist
// because the swapchain format already decides it.
#ifndef GL_ALPHA_TEST
#define GL_ALPHA_TEST 0x0BC0
#endif
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
// Named only so the two unreachable client-array draws still compile; see the
// note on the no-op members in the struct below.
#ifndef GL_VERTEX_ARRAY
#define GL_VERTEX_ARRAY 0x8074
#endif
#ifndef GL_TEXTURE_COORD_ARRAY
#define GL_TEXTURE_COORD_ARRAY 0x8078
#endif

static inline bool vr_gles_absent_cap(GLenum cap)
{
	return cap == GL_ALPHA_TEST || cap == GL_FRAMEBUFFER_SRGB;
}
static inline void vr_gles_enable(GLenum cap)
{
	if (!vr_gles_absent_cap(cap)) glEnable(cap);
}
static inline void vr_gles_disable(GLenum cap)
{
	if (!vr_gles_absent_cap(cap)) glDisable(cap);
}
static inline GLboolean vr_gles_is_enabled(GLenum cap)
{
	return vr_gles_absent_cap(cap) ? GL_FALSE : glIsEnabled(cap);
}

// glad is a desktop-GL loader: it parses GL_VERSION expecting "4.6", and a
// headset answers "OpenGL ES 3.2 v1.r47", so it would fail to load and take the
// whole VR path down with it. The entry points are linked in directly here
// instead, and this struct keeps every call site in this file reading the same
// on both platforms.
struct VrGlesContext
{
	#define VR_GL(name) decltype(&::gl##name) name = &::gl##name;
	VR_GL(ActiveTexture)      VR_GL(AttachShader)        VR_GL(BindFramebuffer)
	VR_GL(BindRenderbuffer)   VR_GL(BindTexture)         VR_GL(BlitFramebuffer)
	VR_GL(CheckFramebufferStatus) VR_GL(Clear)           VR_GL(ClearColor)
	VR_GL(ColorMask)          VR_GL(CompileShader)       VR_GL(CreateProgram)
	VR_GL(CreateShader)       VR_GL(DeleteFramebuffers)  VR_GL(DeleteProgram)
	VR_GL(DeleteRenderbuffers) VR_GL(DeleteShader)       VR_GL(DeleteTextures)
	VR_GL(DepthMask)          VR_GL(DrawArrays)
	VR_GL(FramebufferRenderbuffer)
	VR_GL(FramebufferTexture2D) VR_GL(GenFramebuffers)   VR_GL(GenRenderbuffers)
	VR_GL(GenTextures)        VR_GL(GetError)            VR_GL(GetIntegerv)
	VR_GL(GetProgramInfoLog)  VR_GL(GetProgramiv)        VR_GL(GetShaderInfoLog)
	VR_GL(GetShaderiv)        VR_GL(GetUniformLocation)
	VR_GL(LinkProgram)        VR_GL(PixelStorei)         VR_GL(ReadBuffer)
	VR_GL(ReadPixels)         VR_GL(RenderbufferStorage)
	VR_GL(RenderbufferStorageMultisample)
	VR_GL(ShaderSource)       VR_GL(TexImage2D)          VR_GL(TexParameteri)
	VR_GL(Uniform1i)          VR_GL(Uniform2f)           VR_GL(UseProgram)
	VR_GL(Viewport)
	#undef VR_GL

	// The odd ones out: no EXT suffix on a GLES 3 driver, no glDrawBuffer, and
	// the three capability calls routed through the filter above.
	decltype(&::glRenderbufferStorageMultisample) RenderbufferStorageMultisampleEXT =
		&::glRenderbufferStorageMultisample;
	void (*DrawBuffer)(GLenum) = &vr_gles_draw_buffer;
	void (*Enable)(GLenum) = &vr_gles_enable;
	void (*Disable)(GLenum) = &vr_gles_disable;
	GLboolean (*IsEnabled)(GLenum) = &vr_gles_is_enabled;

	// THE FIXED-FUNCTION FOUR, PRESENT SO THE FILE COMPILES AND NOTHING MORE.
	// Only the screen-effect and calibration quads use client arrays, and both
	// of those draw through shaders written in GLSL 1.20 against gl_Vertex,
	// which no GLES driver will compile. Both are switched off on this platform
	// (see vr_eye_effect and vr_calib_draw), so the eye takes the plain
	// framebuffer blit, which is core GLES 3 and needs none of this. They stay
	// declared rather than the call sites being carved up, because the file is
	// shared with the desktop build and carving it would fork the frame path.
	static void ClientStateNoop(GLenum) {}
	static void PointerNoop(GLint, GLenum, GLsizei, const void *) {}
	void (*EnableClientState)(GLenum) = &ClientStateNoop;
	void (*DisableClientState)(GLenum) = &ClientStateNoop;
	void (*VertexPointer)(GLint, GLenum, GLsizei, const void *) = &PointerNoop;
	void (*TexCoordPointer)(GLint, GLenum, GLsizei, const void *) = &PointerNoop;
};
typedef VrGlesContext GladGLContext;

#endif // _WIN32 / __ANDROID__

#include "../doomdef.h" // CONS_Printf
#include "../command.h" // COM_AddCommand (vr_recenter / vr_worldscale)
#include "../i_system.h" // I_GetPreciseTime - the frame-cost stamps
#include "../core/string.h"
#include "../core/vector.hpp"
#include "../hwr2/crt_dot_pattern.hpp" // the CRT shaders' shadow mask
#include "../rhi/shader_load_context.hpp" // read_glsllist_sources

// The engine's glad header is generated for GL 2.1 and doesn't carry the sRGB
// internal formats; the OpenXR runtime still enumerates them, so name the one
// we prefer ourselves.
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif

// ---- OpenXR state -----------------------------------------------------------
static XrInstance     sInstance   = XR_NULL_HANDLE;
static XrSystemId     sSystemId   = XR_NULL_SYSTEM_ID;
static XrSession      sSession    = XR_NULL_HANDLE;
static XrSpace        sLocalSpace = XR_NULL_HANDLE; // world-locked reference space
static XrSpace        sViewSpace  = XR_NULL_HANDLE; // VIEW reference space (head-locked) for the panel
static XrSessionState sState      = XR_SESSION_STATE_UNKNOWN;

// Optional controller-profile extensions, probed at boot so the native Quest 3
// / Pro (Touch Plus) and HP Reverb G2 profiles can be suggested when motion
// controller input lands. Enabling them early keeps the instance stable.
static bool sHasTouchPlus = false;
static bool sHasHpMR      = false;

// Boot lifecycle. Boot happens lazily on the first vr_begin_frame (the SDL GL
// context is guaranteed current on the main thread there). A permanent failure
// is logged once and never retried; the one transient case is
// XR_ERROR_FORM_FACTOR_UNAVAILABLE (runtime up, headset asleep / Link not yet
// streaming), which retries on a slow cadence for a while.
static bool sBootFailed      = false; // permanent: VR stays off for this run
static bool sBootTransient   = false; // set by vr_boot: retry may succeed later
static int  sBootRetryFrames = 0;     // frames until the next transient retry
static int  sBootRetries     = 0;
static const int kBootRetryInterval = 120; // ~2s between attempts (vsync still on pre-boot)
static const int kBootRetryMax      = 30;  // give up after ~a minute of trying

static bool sRunning    = false;
static bool sFrameBegun = false;
static bool sViewsValid = false;

static XrFrameState            sFrameState;
static uint32_t                sViewCount = 0;
static XrViewConfigurationView sViewConfigs[2];
static XrView                  sViews[2];

typedef struct
{
	XrSwapchain handle;
	uint32_t w, h, imgCount;
	XrSwapchainImageOpenGLKHR* images;
} VrSwapchain;
static VrSwapchain sEye[2];

// Per-eye DEPTH swapchains, handed to the compositor beside the colour ones.
//
// The compositor almost never gets to show the frame we drew. It runs at the
// headset's refresh and we do not - so most of what reaches the panels is the
// last pair we submitted, warped forward to where the head has moved since.
// Given only colour, that warp can do nothing but ROTATE the image: it has no
// idea whether a pixel is a ring at arm's length or a mountain, so it slides
// them all by the same amount. Rotation is right for the mountain and wrong
// for the ring, which is precisely why near objects are the ones that stop
// fusing while the scenery looks fine. Hand the runtime the depth buffer and
// it can move each pixel by what its distance actually warrants.
static VrSwapchain sEyeDepth[2];
static XrCompositionLayerDepthInfoKHR sDepthInfo[2];
static bool   sDepthExtAvailable = false; // runtime advertises the extension
static bool   sDepthLayerReady   = false; // swapchains built, format agreed
// A depth blit is only legal between MATCHING formats - GL rejects the copy
// outright otherwise, which is how the first attempt failed (we rendered into
// DEPTH_COMPONENT24 and the runtime only offers DEPTH_COMPONENT32F, so every
// copy raised INVALID_OPERATION and the layer stood itself down). So the
// runtime picks the format and our own eye depth buffers follow it, rather
// than the other way round.
static unsigned int sDepthGlFormat = GL_DEPTH_COMPONENT24;
static bool   sDepthLayerWanted  = true;  // cv_vr_depthlayer
static bool   sDepthThisFrame[2] = { false, false };

// Frame-cost stamps, in seconds. sEyeWorld is the game drawing the level into
// the eye targets; sEyePost is our own end-of-eye work (MSAA resolve, depth
// copy, effect blit into the swapchain). Accumulated per frame, folded into
// the pacing window in vr_begin_frame. An unstamped cost is an unmeasurable
// one, and every guess about this frame so far has been wrong.
static double sEyeWorldSecs = 0.0, sEyePostSecs = 0.0;
static double sPaceWorldSecs = 0.0, sPacePostSecs = 0.0;
// The other two things a frame can be spent on: waiting for the headset to
// ask for one (time handed back, healthy) and handing it over (compositor
// sync). Whatever is left after all four is the flat game - its tic, its 2D,
// and the desktop mirror.
static double sFrameWaitSecs = 0.0, sFrameSubmitSecs = 0.0;
static double sPaceWaitSecs = 0.0, sPaceSubmitSecs = 0.0, sPaceTotalSecs = 0.0;
static precise_t sEyeStamp = 0;

static double vr_secs_since(precise_t then)
{
	const double per = (double)I_GetPrecisePrecision();
	if (per <= 0.0)
		return 0.0;
	return (double)(I_GetPreciseTime() - then) / per;
}

static GLuint sEyeFbo      = 0; // DRAW FBO: swapchain image as color attachment (blit target)
static GLuint sEyeDepthFbo = 0; // DRAW FBO for the depth blit into the depth swapchain
static GLuint sEyeDepthRB  = 0; // shared depth renderbuffer (per-eye direct-render path)
static GLuint sBlitReadFbo = 0; // READ FBO: the game's finished frame texture is attached here to blit

// Panel swapchain: carries the flat game frame in theater mode (and later the
// HUD/menu overlay - same swapchain serves both roles).
static VrSwapchain sHud;
static GLuint      sOverlayFbo     = 0;
static GLuint      sOverlayDepthRB = 0;
static bool        sHudReady       = false;
static bool        sPanelMode      = false;
static int         sEyesSubmitted  = 0; // eyes blitted into swapchains this frame (gates the proj layer)
static const int   sOverlayW = 1920;
static const int   sOverlayH = 1080;

// Theater panel framing: a big virtual screen floating in front of the head.
// ~3m away and ~4m wide reads like a comfortable cinema screen.
static float sMenuDist = 3.0f; // panel distance (meters)
static float sMenuSize = 4.0f; // panel width (meters)

// The source size of the frame last blitted into the panel, so the quad can be
// sized aspect-correct for the game resolution (the blit stretches the source
// over the whole panel swapchain; the quad un-stretches it).
static int sPanelSrcW = 0;
static int sPanelSrcH = 0;

// Composition layer state recorded per eye for the stereo projection layer.
// Filled in by the per-eye render path; theater mode never populates these.
static XrCompositionLayerProjectionView sProjViews[2];
static XrPosef sRenderPose[2];
static XrFovf  sRenderFov[2];

// ---- stereo eye rendering ----------------------------------------------------
// The world renders once per eye into a VR-owned mono FBO sized to the eye
// swapchain times vr_renderscale (1.0 = native headset resolution, blit is
// 1:1), then blits into that eye's swapchain image. The GL backend's
// GClipRect sizes the raster from vr_eye_raster_width/height during the pass
// instead of the desktop window rect. Two scales compose: the swapchain
// fallback ladder picked at boot (GPU memory) times the live render scale.
//
// With MSAA on, the pass renders into a multisampled renderbuffer FBO and
// vr_end_eye resolve-blits it into the single-sample color texture before the
// eye blit; fixed-function geometry at headset resolution aliases hard bare.
static GLuint sMonoFbo       = 0; // single-sample: color texture (+ depth when MSAA off)
static GLuint sMonoColorTex  = 0;
static GLuint sMonoDepthRB   = 0;
static GLuint sMonoMsFbo     = 0; // multisampled render target (0 = MSAA off)
static GLuint sMonoMsColorRB = 0;
static GLuint sMonoMsDepthRB = 0;
static int    sMonoW         = 0;
static int    sMonoH         = 0;
static int    sMonoBuiltMsaa = -1; // vr_msaa request the target was built for
static GLint  sSavedDrawFb   = 0;  // RHI framebuffer replaced for the eye pass

// Live knobs. All of these are fed by the cv_vr_* cvars (see vr_cvars.c);
// defaults here match the cvar defaults so the module is coherent even
// before the cvars first push.
static float sRenderScale = 1.5f; // mono target = eye swapchain size * this
static int   sMsaaRequest = 4;    // 0 / 2 / 4; falls back to 0 when unsupported

// Eye clip planes in GAME UNITS, matching the flat renderer's frustum
// (ZCLIP_PLANE / FAR_CLIPPING_PLANE in the GL backend).
static const float kEyeZNear = 4.0f;
static const float kEyeZFar  = 32768.0f;

// Per-eye GL matrices rebuilt every frame from the located views. The base
// pair is the pure head view; the output pair (what vr_gl_eye_view serves) is
// the base with the current view mode's extra camera-space transform composed
// in front (first-person kart anchor / diorama placement; see
// vr_compose_mode_views).
static float sEyeProj[2][4][4];         // native asymmetric frustum
static float sEyeView[2][4][4];         // camera-space -> eye-space (rigid, game units)
static float sEyeViewMono[2][4][4];     // rotation-only variant for the sky
static float sEyeViewBase[2][4][4];     // head view before the mode compose
static float sEyeViewMonoBase[2][4][4]; // rotation-only head view before the compose
static bool  sMatricesValid = false;

static bool sInEyePass = false; // between vr_begin_eye and vr_end_eye
static int  sCurrentEye = 0;
static bool sMonoSky    = false; // sky draw in progress: view drops translation

// World scale and comfort knobs.
static float sUnitsPerMeter = 32.0f; // game units per meter of head motion
static float sHeadScale     = 1.0f;  // 6DoF damping: head offset scale (1 = full)
static float sStereoScale   = 1.0f;  // IPD scale around the head center
// Distance in game units to the nearest thing that is permanently on screen
// (third person: your own kart). 0 = nothing is close enough to matter.
static float sFocusUnits    = 0.0f;
// What the clamp below decided this frame, kept so the eye dump can report it.
static float sProximityStereo = 1.0f;
// The angle the eyes can be asked to converge by before the picture reads as
// cross-eyed rather than deep. 60 arcminutes is the classic easy-fusion
// figure; this sits deliberately above it (100') because the chase camera
// already draws in close at low speed - measured at 69' parked, which is fine
// - and a limit tight enough to catch that would make the separation breathe
// with the throttle, which is its own kind of awful.
static const float kMaxDisparityRad = 0.0291f;

// Gameplay HUD overlay: the 2D frame alpha-composited on a quad in front of
// the stereo world - parked in room space by default (the vr_hudlock cvar;
// it doesn't glue to the face, so head motion gives it natural parallax),
// head-locked as the opt-in fallback. Sizing is PHYSICAL: the quad is
// scale x kHudRefWidth meters wide no matter how far it floats, so both
// knobs read directly in the headset - scale grows the panel, distance
// pushes it away (and perspective shrinks it).
static const float kHudRefWidth   = 2.5f; // meters at scale 1.0
static const float kMenuQuadWidth = 2.2f; // menu screen width (fixed, readable)
static bool  sHudOverlay = false; // this frame carries the HUD quad
static bool  sHudWorld   = true;  // room-space anchor (false = head-locked)
static float sHudScale   = 1.5f;  // fraction of kHudRefWidth
static float sHudDist    = 1.7f;  // meters in front of the anchor
static float sMenuQuadDist = 1.4f; // meters the pause menu screen floats - its own knob,
                                   // decoupled from the race HUD's distance

// World-anchored UI: menus and the theater/title panel park in the WORLD
// (LOCAL space) where the player was looking when they appeared, instead of
// gluing to the face. The anchor is the head pose captured at that moment -
// position plus yaw only, so the quad stands upright at eye height and the
// player can look around it. Re-captured whenever the UI quad (re)appears
// and on every recenter (recentering while in theater brings the screen
// back to the gaze).
static XrPosef sUiAnchor = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };
static bool    sUiAnchorValid = false;
static bool    sUiWasUp       = false; // a UI quad was submitted last frame
static bool    sMenuWasUp     = false; // the MENU specifically was up last frame

// ---- view modes ----------------------------------------------------------------
// Third person is the stock behavior (mode compose = identity). First person
// anchors the eye at the kart driver's head with the kart's heading as
// "forward"; diorama shrinks the chase-cam's world onto a tabletop parked in
// front of the player. Theater never renders eyes at all (D_Display skips the
// eye loop, so the flat frame takes the panel path). Persistence is the
// cvars': this module only holds the live values.
static int   sViewMode      = VR_VIEW_THIRD_PERSON;
static bool  sHorizonLock   = true;   // FP: camera pitch/roll never tilts the eye view
static int   sImmersion     = 1;      // FP: 0 off (shipped feel), 1 light, 2 full
static float sFpEyeHeight   = 128.0f; // game units above the kart origin (head anchor)
static float sFpForward     = 0.0f;   // game units the seat slides along the kart's heading
static float sTpEyeHeight   = 0.0f;   // meters the third-person eye lifts above the chase cam
static float sDioramaScale  = 160.0f; // diorama world scale (units/m), its own knob
static float sDioramaDist   = 0.40f;  // meters the tabletop sits in front of the eyes
static float sDioramaHeight = -0.15f; // meters above (+) / below (-) eye level
static bool  sFpSwitchLock  = false;  // pre-race countdown: hold the cockpit back

// Is the first-person compose actually driving the eyes this frame?
//
// Choosing first person ALWAYS takes - the mode is never refused, or it reads
// as missing (it was: the countdown is exactly when you have a spare moment to
// change your view, so the old refusal met every attempt and first person may
// as well not have existed). What the countdown holds back is only the
// COCKPIT: the pre-race camera swings around the grid, and riding that from
// the driver's seat is the most sickening few seconds in the game. So the view
// says first person, the eyes stay on the chase camera, and the moment the
// lights go out you are in the seat. Everything that asks "am I in the
// cockpit" - the compose, the billboard yaw, the eye offset - asks here, or
// they disagree with each other for the length of a countdown.
static bool vr_fp_compose_active(void)
{
	return (sViewMode == VR_VIEW_FIRST_PERSON) && !sFpSwitchLock;
}

// Diorama wall clearance: fraction of the camera->eye offset that is free of
// level geometry, traced by the renderer each frame (see vr.h). The applied
// value snaps inward the moment the trace shrinks - the eye must never spend
// a frame inside a wall - and eases back out so regaining the full distance
// reads as a glide, not a pop.
static float sDioramaClear    = 1.0f; // fraction actually applied this frame
static float sDioramaClearTgt = 1.0f; // this frame's traced clearance

// Kart head pose fed by the game once per rendered frame (world space, SRB2
// coords: x/y ground plane, z height, game units; angles radians CCW).
static float sCockpitPos[3] = { 0 };
static float sCockpitYaw    = 0.0f;
static float sCockpitPitch  = 0.0f; // reserved: horizon lock keeps the HMD authoritative
static float sCockpitRoll   = 0.0f; // reserved, as above
static bool  sCockpitValid  = false;

// The main render pass's interpolated view, fed right after R_SetupFrame with
// the exact values the transform chain will use (so the kart-head offset is
// computed against the same camera the world renders from).
static float sGameViewPos[3] = { 0 };
static float sGameViewYaw    = 0.0f;
static float sGameViewPitch  = 0.0f;
static float sGameViewRoll   = 0.0f;
static bool  sGameViewValid  = false;

// Skybox parallax ratio: how far the skybox viewpoint moves per unit of
// camera movement (1/skybox_scalex from the map header; 0 = the skybox
// doesn't track the camera at all). Eye/head translation shrinks by this on
// skybox passes so the miniature world's stereo reads at the distance it
// stands in for. Fed by the renderer right before each skybox pass.
static float sSkyParallax = 0.0f;

// A menu is up this frame: the HUD quad grows toward the full reference
// width so full-screen menu layouts read comfortably (see vr_submit).
static bool sHudMenuUp = false;

// First-person drama. The GAME says which class is playing and which way the
// kart is spinning; the SHAPING happens here, next to the filter that has to
// survive it.
//
// The original design followed the game's raw angle with a lazy chase and a
// hard per-frame ceiling, and the arithmetic says why that could never work: a
// spinout circulates the target at up to 3150 deg/s while the ceiling caps the
// chaser near 258 deg/s at 90 Hz, so the shortest-path error crosses 180
// degrees about every two tics, the chase reverses, and the eye shivers in
// place instead of being carried anywhere. That is the whole reason nothing
// was felt. Immersion Off keeps it byte for byte, because it is what shipped.
//
// Light and Full replace rate with DISPLACEMENT: a bounded lean, reached over
// a third of a second and held for as long as the wreck lasts. Held, it has no
// angular velocity at all - so the eye travels FURTHER than before while
// moving SLOWER than before. Only a TRICK, the one rotation the player pressed
// a button for, is allowed through whole.
static int   sDramaKind     = VR_DRAMA_NONE;
static float sDramaSpinRaw  = 0.0f;  // raw wrapped spin, as fed
static float sDramaRollRaw  = 0.0f;  // raw wrapped sprite roll, as fed
static float sDramaYawTgt   = 0.0f;  // shaped targets
static float sDramaPitchTgt = 0.0f;
static float sDramaRollTgt  = 0.0f;
static float sDramaYaw      = 0.0f;  // eased, applied to the eye
static float sDramaPitch    = 0.0f;
static float sDramaRoll     = 0.0f;
static float sTrickAccum    = 0.0f;  // a trick's own rotation, UNWRAPPED so a
static float sTrickPrevRaw  = 0.0f;  // double flip is two flips, not a wobble
static bool  sTrickRunning  = false;
static bool  sRaceFinished  = false; // finishing un-hides the kart sprite

// Peak eye rotation per wreck class, radians, indexed [immersion - 1] so [0]
// is Light and [1] is Full. These are angles the eye is CARRIED TO, not rates:
// reached with the attack below, a 67.5 degree sweep never exceeds about
// 202 deg/s even at its steepest instant, which is under the ~258 deg/s the
// shipped per-frame ceiling already permitted at 90 Hz.
static const float kSpinYawPeak[2]     = { 0.785f, 1.178f }; // 45   / 67.5 deg
static const float kTumbleRollPeak[2]  = { 0.524f, 0.785f }; // 30   / 45   deg
static const float kTumblePitchPeak[2] = { 0.314f, 0.489f }; // 18   / 28   deg
static const float kTumbleYawPeak[2]   = { 0.393f, 0.589f }; // 22.5 / 33.75 deg
static const float kFaultYawPeak[2]    = { 0.262f, 0.393f }; // 15   / 22.5 deg
static const float kTrickPeak          = 0.524f;             // 30 deg at Light

// Banked turns and camber, felt without unlocking the world. Horizon lock
// keeps the WORLD level - that is its whole job and it stays the default - but
// a driver still feels camber through the seat, so a fraction of the camera's
// own tilt goes to the eye alone, about the head, clamped hard. A quarter,
// because a 40 degree bank then reads as 10: far enough to feel the road fall
// away under the outside wheels, short of the sustained un-gravitied tilt that
// starts registering as the room going over. With horizon lock OFF this stands
// down - the world already carries all of it there.
static const float kLeanFrac[2]  = { 0.25f, 0.40f };
static const float kLeanRollMax  = 0.175f; // 10 deg
static const float kLeanPitchMax = 0.140f; //  8 deg
static float sBoostKick = 0.0f;  // 0..1, fed on a launch; a head snap, not a tint
static float sLeanRoll  = 0.0f;
static float sLeanPitch = 0.0f;

// Clear everything the drama holds. A stale lean must never greet a new race,
// and a stale trick accumulator must never greet a new trick.
static void vr_drama_reset(void)
{
	sDramaKind    = VR_DRAMA_NONE;
	sDramaSpinRaw = sDramaRollRaw = 0.0f;
	sDramaYawTgt  = sDramaPitchTgt = sDramaRollTgt = 0.0f;
	sDramaYaw     = sDramaPitch    = sDramaRoll    = 0.0f;
	sTrickAccum   = sTrickPrevRaw  = 0.0f;
	sTrickRunning = false;
	sLeanRoll     = sLeanPitch     = 0.0f;
	sBoostKick    = 0.0f;
}

// 6DoF rest capture: tracking gets a short warmup before the rest pose the
// damping references is grabbed; the first recenter happens at that moment.
static const int kHeadWarmupFrames = 15;
static float sHeadRest[3]   = { 0 };
static bool  sHeadRestSet   = false;
static int   sHeadWarmup    = 0;

// Recentered origin: the head yaw + position captured at warmup end and on
// the vr_recenter console command. Eye views are built relative to it, so
// "recentered forward" is the game camera's forward.
static float sRecenterYaw    = 0.0f;
static float sRecenterPos[3] = { 0 };
static bool  sRecenterPending = false;

// Culling wedge (degrees, full angle) for the BSP clipper this frame.
static float sCullFovDeg = 0.0f;

#if defined(_WIN32)
static PFN_xrGetOpenGLGraphicsRequirementsKHR pfnGetGLReq = NULL;
#else
static PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGetGLReq = NULL;
#endif

// Module-private GL function pointers, loaded once at boot via SDL from the
// live context (see the glad include note above).
static GladGLContext sGL;
static bool          sGlReady = false;

extern "C" bool vr_is_active(void)
{
	return sRunning;
}

extern "C" int vr_eye_count(void)
{
	return (int)sViewCount;
}

extern "C" int vr_eye_width(int eye)
{
	return (eye >= 0 && eye < 2) ? (int)sEye[eye].w : 0;
}

extern "C" int vr_eye_height(int eye)
{
	return (eye >= 0 && eye < 2) ? (int)sEye[eye].h : 0;
}

// ---- helpers ----------------------------------------------------------------
static bool xrok(XrResult r, const char* what)
{
	if (XR_SUCCEEDED(r))
		return true;
	char buf[XR_MAX_RESULT_STRING_SIZE] = { 0 };
	if (sInstance != XR_NULL_HANDLE)
		xrResultToString(sInstance, r, buf);
	else
		snprintf(buf, sizeof buf, "%d", (int)r);
	CONS_Printf("VR: %s failed: %s\n", what, buf);
	return false;
}

// Prefer an sRGB format: the game's output is display-referred, and declaring
// it sRGB makes the compositor sample it correctly instead of double-gamma.
static int64_t vr_choose_swapchain_format(void)
{
	uint32_t n = 0;
	if (!XR_SUCCEEDED(xrEnumerateSwapchainFormats(sSession, 0, &n, NULL)) || n == 0)
		return GL_SRGB8_ALPHA8;
	int64_t* fmts = (int64_t*)calloc(n, sizeof(int64_t));
	xrEnumerateSwapchainFormats(sSession, n, &n, fmts);
	const int64_t prefs[] = { GL_SRGB8_ALPHA8, GL_RGBA8 };
	int64_t chosen = fmts[0];
	bool found = false;
	for (uint32_t p = 0; p < 2 && !found; p++)
		for (uint32_t i = 0; i < n; i++)
			if (fmts[i] == prefs[p]) { chosen = prefs[p]; found = true; break; }
	free(fmts);
	CONS_Printf("VR: swapchain format 0x%llx\n", (unsigned long long)chosen);
	return chosen;
}

// ---- eye matrices -------------------------------------------------------------
// Matrix convention: row-vector [row][col], like the reference layer. Those 16
// floats in memory are exactly what OpenGL's column-major glLoadMatrixf expects
// for the equivalent column-vector matrix, so &M[0][0] passes straight through.

// OpenXR fov -> projection (OpenGL clip z in [-1, 1]). The fov is the runtime's
// NATIVE per-eye fov and may be strongly asymmetric on canted-display headsets
// (Quest 3 / Pro angle the panels inward); the off-center terms preserve that.
// Symmetrizing it points both frustum centers inward -> cross-eyed stereo.
static void mat_proj_fov(float m[4][4], XrFovf fov, float zn, float zf)
{
	float l = tanf(fov.angleLeft), r = tanf(fov.angleRight);
	float dn = tanf(fov.angleDown), up = tanf(fov.angleUp);
	float w = r - l, h = up - dn;
	memset(m, 0, sizeof(float) * 16);
	m[0][0] = 2.0f / w;
	m[1][1] = 2.0f / h;
	m[2][0] = (r + l) / w;
	m[2][1] = (up + dn) / h;
	m[2][2] = -(zf + zn) / (zf - zn);
	m[2][3] = -1.0f;
	m[3][2] = -(2.0f * zf * zn) / (zf - zn);
}

// XrPosef -> view matrix: the rigid inverse of the pose's transform.
static void mat_view_from_pose(float m[4][4], XrPosef pose)
{
	float x = pose.orientation.x, y = pose.orientation.y, z = pose.orientation.z, w = pose.orientation.w;
	float Rrv[3][3] = {
		{ 1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + z * w),        2.0f * (x * z - y * w) },
		{ 2.0f * (x * y - z * w),        1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + x * w) },
		{ 2.0f * (x * z + y * w),        2.0f * (y * z - x * w),        1.0f - 2.0f * (x * x + y * y) },
	};
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			m[i][j] = Rrv[j][i];
	m[0][3] = m[1][3] = m[2][3] = 0.0f;
	float px = pose.position.x, py = pose.position.y, pz = pose.position.z;
	m[3][0] = -(px * m[0][0] + py * m[1][0] + pz * m[2][0]);
	m[3][1] = -(px * m[0][1] + py * m[1][1] + pz * m[2][1]);
	m[3][2] = -(px * m[0][2] + py * m[1][2] + pz * m[2][2]);
	m[3][3] = 1.0f;
}

static void mat_identity(float m[4][4])
{
	memset(m, 0, sizeof(float) * 16);
	m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

static void mat_mul(float out[4][4], const float a[4][4], const float b[4][4])
{
	float t[4][4];
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
	memcpy(out, t, sizeof(t));
}

// Row-vector-convention axis rotations, matching the matrices above. Positive
// mat_rot_y turns camera-space "forward" (-Z) toward -X (the viewer's left).
static void mat_rot_y(float m[4][4], float a)
{
	float c = cosf(a), s = sinf(a);
	mat_identity(m);
	m[0][0] = c; m[0][2] = -s;
	m[2][0] = s; m[2][2] = c;
}

static void mat_rot_x(float m[4][4], float a)
{
	float c = cosf(a), s = sinf(a);
	mat_identity(m);
	m[1][1] = c;  m[1][2] = s;
	m[2][1] = -s; m[2][2] = c;
}

// Roll. The file had no Z rotation at all until first person needed to feel a
// barrel roll - which is also why a tumble's roll had been arriving on the
// pitch axis.
static void mat_rot_z(float m[4][4], float a)
{
	float c = cosf(a), s = sinf(a);
	mat_identity(m);
	m[0][0] = c;  m[0][1] = s;
	m[1][0] = -s; m[1][1] = c;
}

static float wrap_pi(float a)
{
	while (a > 3.14159265f)
		a -= 2.0f * 3.14159265f;
	while (a < -3.14159265f)
		a += 2.0f * 3.14159265f;
	return a;
}

static XrQuaternionf quat_mul(XrQuaternionf a, XrQuaternionf b)
{
	XrQuaternionf r;
	r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
	r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
	r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
	r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
	return r;
}

static XrVector3f quat_rotate(XrQuaternionf q, XrVector3f v)
{
	// v' = v + 2*cross(q.xyz, cross(q.xyz, v) + q.w*v)
	float cx = q.y * v.z - q.z * v.y + q.w * v.x;
	float cy = q.z * v.x - q.x * v.z + q.w * v.y;
	float cz = q.x * v.y - q.y * v.x + q.w * v.z;
	XrVector3f r;
	r.x = v.x + 2.0f * (q.y * cz - q.z * cy);
	r.y = v.y + 2.0f * (q.z * cx - q.x * cz);
	r.z = v.z + 2.0f * (q.x * cy - q.y * cx);
	return r;
}

// Capture the world anchor for menu/panel quads: the head's current LOCAL
// position and yaw-only orientation (upright quad at eye height). Fails
// quietly when no views were located this frame; the caller retries next
// frame and the quads fall back to head-locked until it lands.
static void vr_capture_ui_anchor(void)
{
	if (!sViewsValid)
		return;

	sUiAnchor.position.x = 0.5f * (sViews[0].pose.position.x + sViews[1].pose.position.x);
	sUiAnchor.position.y = 0.5f * (sViews[0].pose.position.y + sViews[1].pose.position.y);
	sUiAnchor.position.z = 0.5f * (sViews[0].pose.position.z + sViews[1].pose.position.z);

	const float qy = sViews[0].pose.orientation.y, qw = sViews[0].pose.orientation.w;
	const float qn = sqrtf(qy * qy + qw * qw);
	const float yaw = (qn > 1e-6f) ? 2.0f * atan2f(qy / qn, qw / qn) : 0.0f;
	sUiAnchor.orientation.x = 0.0f;
	sUiAnchor.orientation.y = sinf(0.5f * yaw);
	sUiAnchor.orientation.z = 0.0f;
	sUiAnchor.orientation.w = cosf(0.5f * yaw);

	sUiAnchorValid = true;
}

// Pose for a world-anchored UI quad: the given distance straight out along
// the anchor's forward, facing back at the anchor.
static XrPosef vr_ui_anchor_pose(float dist)
{
	XrVector3f fwd = { 0.0f, 0.0f, -dist };
	fwd = quat_rotate(sUiAnchor.orientation, fwd);
	XrPosef p;
	p.orientation = sUiAnchor.orientation;
	p.position.x = sUiAnchor.position.x + fwd.x;
	p.position.y = sUiAnchor.position.y + fwd.y;
	p.position.z = sUiAnchor.position.z + fwd.z;
	return p;
}

// How much of a flat quad at `dist` metres BOTH eyes can actually see, as a
// scale to apply to a desired size. A headset's frustum is not symmetric: this
// runtime reports about 54 degrees outward and only 40 inward per eye, so the
// two eyes only share the inner 40. Sizing a UI quad purely in metres ignores
// that entirely, and a screen wide enough to run past the inner wall loses its
// outer column in one eye - which does not read as "too big", it reads as text
// with the last letters missing. Anything sized to be READ has to be sized in
// ANGLE, against the numbers the runtime actually reported.
static float vr_ui_fit_scale(float width, float height, float dist)
{
	if (!sViewsValid || dist <= 0.01f || width <= 0.0f || height <= 0.0f)
		return 1.0f;

	// Inner (nasal) edge of each eye, and the vertical extent they share.
	const float nasalL = fabsf(sViews[0].fov.angleRight);
	const float nasalR = fabsf(sViews[1].fov.angleLeft);
	const float halfH  = (nasalL < nasalR) ? nasalL : nasalR;
	const float up     = fabsf(sViews[0].fov.angleUp);
	const float down   = fabsf(sViews[0].fov.angleDown);
	const float halfV  = (up < down) ? up : down;
	if (halfH <= 0.0f || halfV <= 0.0f)
		return 1.0f;

	// The eye is off the head axis by half an IPD, so the far edge subtends a
	// little more from it than from the centre. Take that off the budget, and
	// keep a margin so a small head drift cannot clip the edge back off.
	const float ipdHalf = 0.5f * fabsf(sViews[1].pose.position.x - sViews[0].pose.position.x);
	const float budgetW = dist * tanf(halfH) - ipdHalf;
	const float budgetH = dist * tanf(halfV);
	if (budgetW <= 0.0f || budgetH <= 0.0f)
		return 1.0f;

	const float kMargin = 0.94f; // leave a little room for head drift
	const float sw = (2.0f * budgetW * kMargin) / width;
	const float sh = (2.0f * budgetH * kMargin) / height;
	float s = (sw < sh) ? sw : sh;
	return (s < 1.0f) ? s : 1.0f; // only ever shrink; never inflate a quad
}

// Fold the current view mode's extra camera-space transform onto the base
// head views (the reference layer's "A" matrix, adapted to this port's
// convention where SetTransform pre-multiplies the eye view onto the game's
// own chase-cam chain): eyeView = A * V, both camera-space -> eye-space.
//
// First person: A translates camera space so the eye sits at the kart
// driver's head, then yaws it so the KART's heading (not the chase cam's) is
// "forward" - the world turns with the kart under a stationary driver, which
// is what a driver's seat feels like. The drama spin/flip rotates on top.
// The kart-head offset is rotated from world into camera space with the same
// chain SetTransform applies (see r_opengl.cpp): the GL vertex order is
// (x, height, y) - the chain translates by (-x, -z, -y) - followed by
// Ry(yaw + 270 deg), Rx(pitch), Rz(roll) and a scale whose -Z flip makes the
// camera look down -Z. Horizon lock zeroes pitch/roll both here and in the
// chain (HWR_SetTransformAiming / HWR_RollTransform consult
// vr_horizon_locked), so the world stays level and the HMD is the only tilt.
//
// Diorama: A parks the (already diorama-scaled) camera-space world at a fixed
// spot in front of the eyes; head translation in V gives 6DoF parallax.
//
// skyPass marks the skybox-viewpoint render: its miniature space takes only
// the rotation half of the first-person compose (a kart-head translation is
// meaningless there). The heading offset always measures against the MAIN
// view's yaw: the skybox pass adds the skybox mobj's own angle to viewangle,
// and measuring against that would cancel the skybox's relative rotation.
static void vr_compose_mode_views(bool skyPass)
{
	if (!sMatricesValid)
		return;

	float A[4][4], Arot[4][4];
	mat_identity(A);
	mat_identity(Arot);

	if (sViewMode == VR_VIEW_DIORAMA)
	{
		// Knobs are meters; camera space is diorama-scaled game units. The
		// clearance fraction slides the eye in along the camera->eye segment
		// when a wall would otherwise sit inside the view (see vr.h).
		A[3][1] = sDioramaHeight * sDioramaScale * sDioramaClear;
		A[3][2] = -sDioramaDist * sDioramaScale * sDioramaClear;
		// The mono-sky dome stays a pure rotation (no placement shift).
	}
	else if (vr_fp_compose_active() && sCockpitValid && sGameViewValid)
	{
		// World-space camera -> kart-head offset (SRB2 coords, game units),
		// reordered to the GL vertex layout (x, height, y).
		float g[3];
		g[0] = sCockpitPos[0] - sGameViewPos[0];
		g[1] = sCockpitPos[2] - sGameViewPos[2];
		g[2] = sCockpitPos[1] - sGameViewPos[1];

		// Rotate into camera space: Ry(yaw + 270 deg), Rx(pitch), Rz(roll),
		// then the chain's -Z flip. Pitch/roll are the chain's EFFECTIVE
		// values: zero under horizon lock, the fed camera aim otherwise.
		const float yawGl   = sGameViewYaw + 4.7123890f; // + 270 degrees
		const float pitchGl = sHorizonLock ? 0.0f : sGameViewPitch;
		const float rollGl  = sHorizonLock ? 0.0f : sGameViewRoll;
		float c = cosf(yawGl), s = sinf(yawGl);
		float x1 = g[0] * c + g[2] * s;
		float y1 = g[1];
		float z1 = -g[0] * s + g[2] * c;
		c = cosf(pitchGl); s = sinf(pitchGl);
		float x2 = x1;
		float y2 = y1 * c - z1 * s;
		float z2 = y1 * s + z1 * c;
		c = cosf(rollGl); s = sinf(rollGl);
		float k[3];
		k[0] = x2 * c - y2 * s;
		k[1] = x2 * s + y2 * c;
		k[2] = -z2;

		// Heading offset: how far the kart (plus its eased drama spin) points
		// off the camera's axis. Rotating the world by the inverse puts the
		// kart's nose at -Z.
		const float dYaw = wrap_pi((sCockpitYaw + sDramaYaw) - sGameViewYaw);
		// Yaw, then pitch, then roll, all about the driver's head (the
		// translation below composes first). Order matters: a tumble has to
		// read as spin-and-barrel, not as one smeared diagonal. The lean rides
		// the same rotation, so camber is felt from the seat rather than by
		// tipping the world.
		float R[4][4], Rx[4][4], Rz[4][4];
		mat_rot_y(Arot, -dYaw);
		const float eyePitch = sDramaPitch + sLeanPitch;
		const float eyeRoll  = sDramaRoll  + sLeanRoll;
		if (eyePitch != 0.0f)
		{
			mat_rot_x(Rx, -eyePitch);
			mat_mul(Arot, Arot, Rx);
		}
		if (eyeRoll != 0.0f)
		{
			// POSITIVE, unlike the yaw and pitch terms beside it, and that is
			// not a slip. The unlocked path rolls the world by building its
			// basis directly (k[0] = x*c - y*s, k[1] = x*s + y*c), which sends
			// world-right to +y for a positive roll. mat_rot_z is a row-vector
			// matrix, so reaching the same handedness through it needs the
			// opposite sign to the one the yaw and pitch terms take. Negated,
			// the camber tips AGAINST the turn - which is both wrong and the
			// most reliable way to make someone ill in a headset.
			mat_rot_z(Rz, eyeRoll);
			mat_mul(Arot, Arot, Rz);
		}

		// Full compose: translate the eye to the kart head, then rotate about
		// it (so drama spins in place, not around the chase camera).
		mat_identity(R);
		R[3][0] = -k[0];
		R[3][1] = -k[1];
		R[3][2] = -k[2];
		mat_mul(A, R, Arot);
	}
	else if (sViewMode == VR_VIEW_THIRD_PERSON && sTpEyeHeight != 0.0f)
	{
		// Third person's own seat height: lift the eye straight up in camera
		// space (meters at the live world scale). Deliberately NOT done by
		// raising the chase camera game-side - the chase cam re-aims at the
		// kart, so raising it would pitch the whole world down in the
		// headset. Here only the eye moves; you look down when you choose to.
		A[3][1] = -sTpEyeHeight * sUnitsPerMeter;
	}

	// Skybox pass: every mode's camera-space offset - the diorama's parked
	// world, third person's seat lift, and FIRST PERSON'S WHOLE CHASE LENGTH
	// out to the driver's head - shrinks by the map's own skybox tracking
	// ratio, the same ratio the IPD takes below. Full-size they would
	// misregister the miniature backdrop against the world standing in front
	// of it.
	//
	// First person used to be excluded here and composed rotation-only, on the
	// reasoning that a kart-head offset "means nothing" to a miniature. It
	// means exactly what every other viewer movement means: the backdrop was
	// drawn from the chase camera while the world was drawn from the driver's
	// head, a couple of hundred units apart, and turning swung that gap around
	// - which is the sky and the clouds visibly swimming against the track,
	// in first person and nowhere else. Where a map has no tracking skybox the
	// ratio is zero, so this scales to nothing and the sky sits at infinity
	// exactly as it did.
	if (skyPass)
	{
		A[3][0] *= sSkyParallax;
		A[3][1] *= sSkyParallax;
		A[3][2] *= sSkyParallax;
	}

	for (int eye = 0; eye < 2; eye++)
	{
		// Skybox pass: the skybox viewpoint tracks the camera at the map's
		// skybox ratio, so the head/IPD translation shrinks by the same
		// ratio - full-size eye offsets in the miniature space read as
		// hyperstereo and cross-eye anything living in the skybox. The view
		// matrix's translation row is linear in the head position, so
		// scaling the row is scaling the offsets themselves.
		float base[4][4];
		memcpy(base, sEyeViewBase[eye], sizeof base);
		if (skyPass)
		{
			base[3][0] *= sSkyParallax;
			base[3][1] *= sSkyParallax;
			base[3][2] *= sSkyParallax;
		}

		mat_mul(sEyeView[eye], A, base);
		mat_mul(sEyeViewMono[eye], Arot, sEyeViewMonoBase[eye]);
	}
}

static float vr_clampf(float v, float lo, float hi)
{
	return (v < lo) ? lo : (v > hi) ? hi : v;
}

// Build this frame's per-eye GL matrices from the freshly located views.
// The game's chase-cam chain has already put the world into camera space
// (right-handed, -Z forward, +Y up, game units); XR LOCAL space has the same
// axes in meters, so the eye view is just the rigid inverse of the head pose
// relative to the recentered origin, with translation scaled meters -> units.
static void vr_update_eye_matrices(void)
{
	sMatricesValid = false;

	// Diorama clearance: pulled in on the spot when a wall is closer than the
	// eye wants to be, eased back out once it passes. Both directions land
	// HERE, before either eye has rendered, so the pair can never straddle a
	// change (see vr_set_diorama_clearance).
	if (sDioramaClear > sDioramaClearTgt)
	{
		sDioramaClear = sDioramaClearTgt;
	}
	else if (sDioramaClear < sDioramaClearTgt)
	{
		sDioramaClear += (sDioramaClearTgt - sDioramaClear) * 0.08f;
		if (sDioramaClearTgt - sDioramaClear < 0.001f)
			sDioramaClear = sDioramaClearTgt;
	}

	// Head center (midpoint of the eyes) in LOCAL space, meters.
	float cx = 0.5f * (sViews[0].pose.position.x + sViews[1].pose.position.x);
	float cy = 0.5f * (sViews[0].pose.position.y + sViews[1].pose.position.y);
	float cz = 0.5f * (sViews[0].pose.position.z + sViews[1].pose.position.z);

	if (!sHeadRestSet)
	{
		if (++sHeadWarmup < kHeadWarmupFrames)
			return;
		sHeadRest[0] = cx;
		sHeadRest[1] = cy;
		sHeadRest[2] = cz;
		sHeadRestSet = true;
		sRecenterPending = true;
	}

	// 6DoF damping: scale the head's offset from the rest pose (1 = full).
	float dcx = sHeadRest[0] + (cx - sHeadRest[0]) * sHeadScale;
	float dcy = sHeadRest[1] + (cy - sHeadRest[1]) * sHeadScale;
	float dcz = sHeadRest[2] + (cz - sHeadRest[2]) * sHeadScale;

	if (sRecenterPending)
	{
		// Head yaw about +Y from the quaternion's y/w projection: the new
		// "forward" the game camera maps onto.
		float qy = sViews[0].pose.orientation.y, qw = sViews[0].pose.orientation.w;
		float qn = sqrtf(qy * qy + qw * qw);
		sRecenterYaw = (qn > 1e-6f) ? 2.0f * atan2f(qy / qn, qw / qn) : 0.0f;
		sRecenterPos[0] = dcx;
		sRecenterPos[1] = dcy;
		sRecenterPos[2] = dcz;
		sRecenterPending = false;
		// Menu/panel quads re-anchor to the new gaze on their next submit.
		sUiAnchorValid = false;
		CONS_Printf("VR: recentered (yaw %.1f deg)\n", sRecenterYaw * 57.29578f);
	}

	// Inverse of the recenter yaw, as a quaternion (rotation about +Y).
	XrQuaternionf qinv;
	qinv.x = 0.0f;
	qinv.y = sinf(-0.5f * sRecenterYaw);
	qinv.z = 0.0f;
	qinv.w = cosf(-0.5f * sRecenterYaw);

	// Comfort clamp. What crosses the eyes is the ANGLE between them, which is
	// separation over distance - so a full IPD that is perfectly comfortable
	// at a chase camera's length becomes hyperstereo the moment something
	// parks within arm's reach. In third person that something is your own
	// kart, and the Camera Forward knob can bring it all the way in. Rather
	// than let the view break at one end of a slider, the separation stands
	// down to hold the disparity at the fusion limit. It only ever REDUCES
	// (never exaggerates past a real head), and it keeps a floor so a close-up
	// framing still has depth instead of going flat.
	float proximityTarget = 1.0f;
	if (sFocusUnits > 0.0f)
	{
		const float ex = sViews[1].pose.position.x - sViews[0].pose.position.x;
		const float ey = sViews[1].pose.position.y - sViews[0].pose.position.y;
		const float ez = sViews[1].pose.position.z - sViews[0].pose.position.z;
		const float unitsPerMeterHere =
			(sViewMode == VR_VIEW_DIORAMA) ? sDioramaScale : sUnitsPerMeter;
		const float sep = sqrtf(ex * ex + ey * ey + ez * ez) * unitsPerMeterHere;
		if (sep > 0.0f)
			proximityTarget = vr_clampf((kMaxDisparityRad * sFocusUnits) / sep, 0.15f, 1.0f);
	}

	// Snap in, ease out - the diorama's wall clearance does the same, for the
	// same reason: standing the separation down late is uncomfortable, while
	// bringing it back too eagerly makes it pump. Both directions land here,
	// before either eye renders, so the two eyes can never straddle a change.
	if (proximityTarget < sProximityStereo)
		sProximityStereo = proximityTarget;
	else if (proximityTarget > sProximityStereo)
	{
		sProximityStereo += (proximityTarget - sProximityStereo) * 0.02f;
		if (proximityTarget - sProximityStereo < 0.001f)
			sProximityStereo = proximityTarget;
	}

	// Two independent ceilings on the same number, so take the lower of them
	// rather than multiplying: the comfort clamp is an absolute limit on how
	// far apart the eyes may sit, and someone who has already dialled their
	// own separation down should not then be clamped a second time on top.
	const float stereo = (sProximityStereo < sStereoScale) ? sProximityStereo : sStereoScale;

	for (uint32_t eye = 0; eye < sViewCount && eye < 2; eye++)
	{
		XrPosef pose = sViews[eye].pose;

		// Stereo separation: keep the per-eye offset from xrLocateViews (it
		// carries the IPD), scaled around the damped center.
		pose.position.x = dcx + (pose.position.x - cx) * stereo;
		pose.position.y = dcy + (pose.position.y - cy) * stereo;
		pose.position.z = dcz + (pose.position.z - cz) * stereo;

		// What the compositor gets told must be what we actually drew from.
		// The head damping and the two stereo scales all move the eye off the
		// pose the runtime located, and declaring the located one would hand
		// the compositor a picture that disagrees with its own reprojection by
		// exactly that error, every frame, growing with head motion. (The
		// recenter below is a rigid placement of the WORLD, not of the head,
		// so it is absorbed by where the level is drawn and must not appear
		// here.)
		sRenderPose[eye] = pose;
		sRenderFov[eye]  = sViews[eye].fov;

		// Express relative to the recentered origin, then meters -> game
		// units. Diorama uses its own (much larger) scale: more units per
		// meter of head motion means a smaller world - the tabletop.
		const float unitsPerMeter =
			(sViewMode == VR_VIEW_DIORAMA) ? sDioramaScale : sUnitsPerMeter;
		XrVector3f d;
		d.x = pose.position.x - sRecenterPos[0];
		d.y = pose.position.y - sRecenterPos[1];
		d.z = pose.position.z - sRecenterPos[2];
		XrPosef rel;
		rel.orientation = quat_mul(qinv, pose.orientation);
		rel.position = quat_rotate(qinv, d);
		rel.position.x *= unitsPerMeter;
		rel.position.y *= unitsPerMeter;
		rel.position.z *= unitsPerMeter;

		mat_view_from_pose(sEyeViewBase[eye], rel);

		// Rotation-only variant: no eye translation on the sky, so it sits at
		// infinity (no IPD -> both eyes agree it's infinitely far).
		XrPosef relsky = rel;
		relsky.position.x = relsky.position.y = relsky.position.z = 0.0f;
		mat_view_from_pose(sEyeViewMonoBase[eye], relsky);

		mat_proj_fov(sEyeProj[eye], sViews[eye].fov, kEyeZNear, kEyeZFar);
	}

	sMatricesValid = true;
	vr_compose_mode_views(false);

	// Culling wedge for the BSP clipper, computed from the COMPOSED eye views
	// rather than switched off (360 walked the whole level twice a frame, and
	// that CPU walk measured as the frame's owner). The wedge stays centered
	// on the game camera's angle, so it must cover the eye frustum PLUS
	// however far the head has turned away from that axis; the slop absorbs
	// lean, roll and the eye sitting off the camera origin. An eye pushed
	// FORWARD of the camera (first person) only ever needs a narrower wedge
	// than the camera itself, never wider. Diorama moves the eye around the
	// scaled world too freely for this bound - it stays fully open.
	if (sViewMode == VR_VIEW_DIORAMA)
	{
		sCullFovDeg = 360.0f;
	}
	else
	{
		// The wedge is a YAW bound, so it has to be measured from the corners
		// of what the eye can see, not from where it happens to be pointing.
		// The old version took the centre ray's yaw and added a linear term
		// for pitch, which is a good approximation right up until an edge of
		// the frustum swings past straight-up or straight-down - and with this
		// runtime reporting 55 degrees of downward view, the bottom edge gets
		// there at only 35 degrees of head pitch. Past that the true answer is
		// "everything", while the old arithmetic went on confidently returning
		// about 120 degrees. That is the geometry that was popping in at the
		// edge of vision when looking down, and only beyond a distance, which
		// is why it read as a small remnant rather than half a missing world.
		// Slop stays where it was. The sweep below is exact where the old
		// centre-ray estimate was not, so there is a case for trimming it and
		// buying frame time back - but this change exists to STOP geometry
		// disappearing, and narrowing the wedge in the same edit would be
		// betting the fix against the saving. Trim it later, on its own, with
		// the frame timer open.
		const float kCullSlopDeg = 20.0f;
		float worstHalf = 0.0f;
		for (uint32_t eye = 0; eye < sViewCount && eye < 2; eye++)
		{
			const XrFovf& fov = sViews[eye].fov;
			const float tL = tanf(fov.angleLeft),  tR = tanf(fov.angleRight);
			const float tD = tanf(fov.angleDown),  tU = tanf(fov.angleUp);
			bool degenerate = false;
			float eyeHalf = 0.0f;

			// Walk the frustum's four edges. Corners alone are not enough: a
			// point part-way along an edge can sit wider in yaw than either
			// end once the eye is rolled.
			const int kSteps = 8;
			for (int e = 0; e < 4 && !degenerate; e++)
			{
				for (int s = 0; s <= kSteps && !degenerate; s++)
				{
					const float t = (float)s / (float)kSteps;
					float tx, ty;
					switch (e)
					{
						case 0: tx = tL + (tR - tL) * t; ty = tU;                  break;
						case 1: tx = tL + (tR - tL) * t; ty = tD;                  break;
						case 2: tx = tL;                 ty = tD + (tU - tD) * t;  break;
						default: tx = tR;                ty = tD + (tU - tD) * t;  break;
					}

					// Ray (tx, ty, -1) in eye space, pulled back to camera
					// space. Same convention the old centre-ray code used:
					// forward is minus the third COLUMN, so a general
					// direction is tx*col0 + ty*col1 - col2, and column k of
					// the rotation is m[0..2][k].
					const float* const m0 = sEyeView[eye][0];
					const float* const m1 = sEyeView[eye][1];
					const float* const m2 = sEyeView[eye][2];
					const float dx = tx * m0[0] + ty * m0[1] - m0[2];
					const float dz = tx * m2[0] + ty * m2[1] - m2[2];

					// Horizontal length: if a ray is near vertical its yaw is
					// meaningless and it can be pointing anywhere, so stop
					// pretending and open all the way.
					const float horiz = sqrtf(dx * dx + dz * dz);
					if (horiz < 0.087f) { degenerate = true; break; }

					const float yaw = fabsf(atan2f(dx, -dz));
					if (yaw > eyeHalf)
						eyeHalf = yaw;
				}
			}

			if (degenerate)
			{
				worstHalf = 1e9f;
				break;
			}
			const float half = eyeHalf * 57.29578f + kCullSlopDeg;
			if (half > worstHalf)
				worstHalf = half;
		}
		sCullFovDeg = (worstHalf >= 178.0f) ? 360.0f : worstHalf * 2.0f;
	}
}

#if defined(__ANDROID__)

// ON ANDROID THE LOADER MUST BE HANDED THE APP BEFORE IT WILL ANSWER ANYTHING.
// There is no system-wide runtime to look up: the loader has to be told which
// VM and which activity it belongs to, and every xr* call before that fails.
// SDL already owns both handles, so they are borrowed rather than re-derived.
// Called from both entry points below; the work happens once.
static bool vr_android_init_loader(void)
{
	static bool tried = false;
	static bool ok = false;
	if (tried)
		return ok;
	tried = true;

	PFN_xrInitializeLoaderKHR pfnInit = NULL;
	if (XR_FAILED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
			(PFN_xrVoidFunction *)&pfnInit)) || !pfnInit)
	{
		CONS_Printf("VR: this OpenXR loader has no xrInitializeLoaderKHR\n");
		return false;
	}

	JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
	void *activity = SDL_GetAndroidActivity();
	if (!env || !activity)
	{
		CONS_Printf("VR: no JNI handles from SDL yet\n");
		return false;
	}

	JavaVM *vm = NULL;
	env->GetJavaVM(&vm);

	XrLoaderInitInfoAndroidKHR info = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	info.applicationVM = vm;
	info.applicationContext = activity;
	ok = XR_SUCCEEDED(pfnInit((const XrLoaderInitInfoBaseHeaderKHR *)&info));
	if (!ok)
		CONS_Printf("VR: xrInitializeLoaderKHR failed\n");
	return ok;
}

// The same handles again, this time chained onto instance creation: the runtime
// wants them a second time and will not create an instance without them.
static XrInstanceCreateInfoAndroidKHR vr_android_instance_chain(void)
{
	XrInstanceCreateInfoAndroidKHR ic = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
	JavaVM *vm = NULL;
	if (env)
		env->GetJavaVM(&vm);
	ic.applicationVM = vm;
	ic.applicationActivity = SDL_GetAndroidActivity();
	return ic;
}

#endif // __ANDROID__

// Lightweight startup probe: is a VR headset connected right now? Creates and
// tears down a throwaway OpenXR instance (no GL binding) and asks the runtime
// for an HMD system, so the same exe can auto-enable VR when a headset is
// present and stay flat otherwise.
extern "C" bool vr_headset_present(void)
{
#if defined(_WIN32)
	const char *exts[1] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
	XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
	ici.enabledExtensionCount = 1;
#else
	if (!vr_android_init_loader())
		return false;
	const char *exts[2] = { XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
	                        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME };
	XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
	ici.enabledExtensionCount = 2;
	XrInstanceCreateInfoAndroidKHR androidChain = vr_android_instance_chain();
	ici.next = &androidChain;
#endif
	ici.enabledExtensionNames = exts;
	strncpy(ici.applicationInfo.applicationName, "RingRacers", XR_MAX_APPLICATION_NAME_SIZE - 1);
	ici.applicationInfo.apiVersion = XR_API_VERSION_1_0; // VirtualDesktopXR etc. are OpenXR 1.0

	XrInstance inst = XR_NULL_HANDLE;
	if (XR_FAILED(xrCreateInstance(&ici, &inst)) || inst == XR_NULL_HANDLE)
		return false;

	XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId sys = XR_NULL_SYSTEM_ID;
	XrResult r = xrGetSystem(inst, &sgi, &sys);
	xrDestroyInstance(inst);

	return XR_SUCCEEDED(r) && sys != XR_NULL_SYSTEM_ID;
}

// ---- settings (pushed by the cv_vr_* cvar handlers in vr_cvars.c) --------------
// Each setter clamps defensively and applies live: world/stereo/head scale on
// the next located frame, render scale and MSAA on the next eye pass (the
// mono target rebuilds itself; the swapchain never needs recreating), HUD
// scale/distance on the next vr_submit.

extern "C" void vr_recenter(void)
{
	sRecenterPending = true;
}

extern "C" void vr_set_world_scale(float unitsPerMeter)
{
	sUnitsPerMeter = vr_clampf(unitsPerMeter, 1.0f, 4096.0f);
}

extern "C" void vr_set_render_scale(float scale)
{
	sRenderScale = vr_clampf(scale, 0.4f, 1.5f);
}

extern "C" void vr_set_msaa(int samples)
{
	if (samples == 0 || samples == 2 || samples == 4)
		sMsaaRequest = samples; // mono target rebuilds on the next eye pass
}

extern "C" void vr_set_stereo_strength(float frac)
{
	sStereoScale = vr_clampf(frac, 0.0f, 1.0f);
}

extern "C" void vr_set_focus_distance(float units)
{
	sFocusUnits = (units > 1.0f) ? units : 0.0f;
}

extern "C" void vr_set_head_motion_scale(float frac)
{
	sHeadScale = vr_clampf(frac, 0.0f, 1.0f);
}

extern "C" void vr_set_horizon_lock(bool on)
{
	sHorizonLock = on;
}

extern "C" void vr_set_immersion(int level)
{
	if (level < 0)
		level = 0;
	if (level > 2)
		level = 2;
	sImmersion = level;
}

extern "C" void vr_set_boost_kick(float amount)
{
	sBoostKick = vr_clampf(amount, 0.0f, 1.0f);
}

extern "C" void vr_set_depth_layer(bool on)
{
	sDepthLayerWanted = on;
}

extern "C" void vr_set_fp_eyeheight(float units)
{
	sFpEyeHeight = vr_clampf(units, -64.0f, 256.0f);
}

extern "C" void vr_set_fp_forward(float units)
{
	sFpForward = vr_clampf(units, -128.0f, 256.0f);
}

extern "C" void vr_set_tp_eyeheight(float meters)
{
	sTpEyeHeight = vr_clampf(meters, -1.0f, 2.0f);
}

extern "C" void vr_set_diorama_scale(float unitsPerMeter)
{
	sDioramaScale = vr_clampf(unitsPerMeter, 64.0f, 8192.0f);
}

extern "C" void vr_set_diorama_dist(float meters)
{
	sDioramaDist = vr_clampf(meters, 0.1f, 5.0f);
}

extern "C" void vr_set_diorama_height(float meters)
{
	sDioramaHeight = vr_clampf(meters, -2.0f, 2.0f);
}

// Undo the SetTransform chain's rotation half for a camera-space eye offset
// (game units, GL axes): -Z flip, then roll, pitch, yaw in reverse, then the
// GL (x, height, y) order back to SRB2 (x, y, z). Pitch/roll are the chain's
// live values - every path through here renders with the chase cam's own aim
// (horizon lock is first-person only).
static void vr_camera_offset_to_world(float k0, float k1, float k2, float outOffset[3])
{
	const float yawGl = sGameViewYaw + 4.7123890f; // + 270 degrees
	float c = cosf(sGameViewRoll), s = sinf(sGameViewRoll);
	const float x2 = k0 * c + k1 * s;
	const float y2 = -k0 * s + k1 * c;
	const float z2 = -k2;
	c = cosf(sGameViewPitch); s = sinf(sGameViewPitch);
	const float y1 = y2 * c + z2 * s;
	const float z1 = -y2 * s + z2 * c;
	c = cosf(yawGl); s = sinf(yawGl);
	outOffset[0] = x2 * c - z1 * s;
	outOffset[1] = x2 * s + z1 * c;
	outOffset[2] = y1;
}

// Where would the diorama knobs park the eye, relative to the game camera?
// Inverts the SetTransform chain the compose runs forward (see
// vr_compose_mode_views): the eye sits at camera-space (0, -H, +D), and the
// chain's rotation half is undone to land the offset back in world space.
// The renderer traces this segment against the level and feeds the clear
// fraction into vr_set_diorama_clearance below.
extern "C" bool vr_diorama_eye_offset(float outOffset[3])
{
	if (sViewMode != VR_VIEW_DIORAMA || !sGameViewValid)
		return false;

	// Camera-space eye position: the negation of the compose's A translation.
	vr_camera_offset_to_world(0.0f,
		-sDioramaHeight * sDioramaScale,
		sDioramaDist * sDioramaScale,
		outOffset);
	return true;
}

// Where the eyes ended up, in world space, relative to the camera the game
// drew from (see vr.h). First person is the plain difference between the fed
// cockpit position and the fed camera position - the compose only rotates
// that offset into camera space, it does not change it. The diorama reuses
// the inverse-chain math above, scaled by the live wall clearance because
// that is where the eye REALLY sits, not where the knobs asked for it.
extern "C" bool vr_eye_world_offset(float outOffset[3])
{
	if (!vr_stereo_active() || !sGameViewValid)
		return false;

	if (vr_fp_compose_active())
	{
		if (!sCockpitValid)
			return false;

		outOffset[0] = sCockpitPos[0] - sGameViewPos[0];
		outOffset[1] = sCockpitPos[1] - sGameViewPos[1];
		outOffset[2] = sCockpitPos[2] - sGameViewPos[2];
		return true;
	}

	if (sViewMode == VR_VIEW_DIORAMA)
	{
		if (!vr_diorama_eye_offset(outOffset))
			return false;

		outOffset[0] *= sDioramaClear;
		outOffset[1] *= sDioramaClear;
		outOffset[2] *= sDioramaClear;
		return true;
	}

	// Third person: the eye IS the camera, bar the IPD and a lean - unless
	// the seat-height knob lifted it. That lift is a camera-space translation
	// like the diorama's, so it undoes through the same chain.
	if (sViewMode == VR_VIEW_THIRD_PERSON && sTpEyeHeight != 0.0f)
	{
		vr_camera_offset_to_world(0.0f, sTpEyeHeight * sUnitsPerMeter, 0.0f, outOffset);
		return true;
	}

	return false;
}

extern "C" void vr_set_diorama_clearance(float frac)
{
	// Target only. This used to snap inward and recompose ON THE SPOT, and
	// that recompose happened in the middle of a stereo frame: the renderer
	// traces on the first eye alone, after that eye's skybox pass had already
	// composed with the old value, so on any frame the clearance moved, the
	// left eye's backdrop stood somewhere the right eye's did not. Eyes that
	// disagree about where the world is are the one thing binocular vision
	// cannot forgive. Both the inward snap and the eased recovery now land in
	// vr_update_eye_matrices, once, before either eye renders - the snap is
	// still a snap, just a frame later, and the diorama eye is a stone's
	// throw from a camera the game has already kept out of walls.
	sDioramaClearTgt = vr_clampf(frac, 0.0f, 1.0f);
}

extern "C" void vr_set_hud_scale(float frac)
{
	sHudScale = vr_clampf(frac, 0.1f, 2.5f);
}

extern "C" void vr_set_hud_dist(float meters)
{
	sHudDist = vr_clampf(meters, 1.0f, 5.0f);
}

extern "C" void vr_set_menu_dist(float meters)
{
	sMenuQuadDist = vr_clampf(meters, 1.0f, 5.0f);
}

extern "C" void vr_set_hud_world(bool on)
{
	sHudWorld = on;
}

extern "C" void vr_set_hud_menu(bool menuUp)
{
	sHudMenuUp = menuUp;
}

// True while the in-progress eye draw should dim the world: a menu quad
// hangs over the stereo scene, and undimmed geometry near the quad's depth
// fights it for the eyes. Queried by the GL renderer's post pass.
extern "C" bool vr_menu_dim_active(void)
{
	return sInEyePass && sHudMenuUp;
}

// The one action that stays a console command (it's an event, not a value).
static void Command_VrRecenter_f(void)
{
	vr_recenter();
	CONS_Printf("VR: recenter queued for the next frame.\n");
}

// ---- eye-pair dump ---------------------------------------------------------
// vr_dumpeyes [n]: n stereo frames from now, write both finished eye images
// (post-resolve, pre-effect - the geometry the compositor gets) plus every
// number they rendered with, so stereo geometry can be measured offline
// instead of judged through the lenses. Files land in the home dir beside the
// config: vr_eye0.tga, vr_eye1.tga, vr_eyes.txt.

extern "C" { extern char srb2home[256]; }

static const char* vr_view_mode_name(int mode); // defined just below

static int      sEyeDumpArm       = 0; // stereo frames until the dump fires
static bool     sEyeDumpThisFrame = false;
static uint8_t* sEyeDumpPix[2]    = { NULL, NULL };
static int      sEyeDumpW = 0, sEyeDumpH = 0;
// A burst writes CONSECUTIVE frames to numbered files. A defect that varies
// frame to frame - anything that crawls, shimmers or smears in motion and
// looks perfect the moment the world stops - is invisible in a single dump,
// however carefully that one frame is measured.
static int      sEyeDumpBurst     = 0; // frames still to write in this burst
static int      sEyeDumpIndex     = 0; // number on the files being written

static void Command_VrDumpEyes_f(void)
{
	int wait = 1;
	if (COM_Argc() > 1)
		wait = atoi(COM_Argv(1));
	if (wait < 1)
		wait = 1;
	int burst = 1;
	if (COM_Argc() > 2)
		burst = atoi(COM_Argv(2));
	if (burst < 1)
		burst = 1;
	sEyeDumpArm = wait;
	sEyeDumpBurst = burst;
	sEyeDumpIndex = 0;
	if (burst > 1)
		CONS_Printf("VR: eye dump armed, %d stereo frame(s) away, %d consecutive frames.\n", wait, burst);
	else
		CONS_Printf("VR: eye dump armed, %d stereo frame(s) away.\n", wait);
}

// Uncompressed 24-bit TGA, descriptor 0 = bottom-left origin, which is GL row
// order exactly as glReadPixels hands it back - no flip on either side.
static bool vr_write_tga(const char* path, const uint8_t* rgba, int w, int h)
{
	FILE* f = fopen(path, "wb");
	if (!f)
		return false;
	uint8_t hdr[18];
	memset(hdr, 0, sizeof(hdr));
	hdr[2]  = 2; // uncompressed truecolor
	hdr[12] = (uint8_t)(w & 0xFF);
	hdr[13] = (uint8_t)((w >> 8) & 0xFF);
	hdr[14] = (uint8_t)(h & 0xFF);
	hdr[15] = (uint8_t)((h >> 8) & 0xFF);
	hdr[16] = 24; // BGR
	fwrite(hdr, 1, sizeof(hdr), f);
	uint8_t* row = (uint8_t*)malloc((size_t)w * 3);
	if (!row)
	{
		fclose(f);
		return false;
	}
	for (int y = 0; y < h; y++)
	{
		const uint8_t* src = rgba + (size_t)y * w * 4;
		for (int x = 0; x < w; x++)
		{
			row[x * 3 + 0] = src[x * 4 + 2];
			row[x * 3 + 1] = src[x * 4 + 1];
			row[x * 3 + 2] = src[x * 4 + 0];
		}
		fwrite(row, 1, (size_t)w * 3, f);
	}
	free(row);
	fclose(f);
	return true;
}

static void vr_dump_mat4(FILE* f, const char* name, const float m[4][4])
{
	fprintf(f, "%s", name);
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			fprintf(f, " %.9g", m[r][c]);
	fprintf(f, "\n");
}

static void vr_dump_write(void)
{
	char path[512];
	char tag[16];
	// A single dump keeps the plain names every script already knows; a burst
	// numbers its frames so consecutive ones can be diffed against each other.
	if (sEyeDumpBurst > 1 || sEyeDumpIndex > 0)
		snprintf(tag, sizeof(tag), "_%02d", sEyeDumpIndex);
	else
		tag[0] = '\0';

	for (int eye = 0; eye < 2; eye++)
	{
		snprintf(path, sizeof(path), "%s" PATHSEP "vr_eye%d%s.tga", srb2home, eye, tag);
		if (!vr_write_tga(path, sEyeDumpPix[eye], sEyeDumpW, sEyeDumpH))
			CONS_Printf("VR: eye dump: could not write %s\n", path);
	}

	snprintf(path, sizeof(path), "%s" PATHSEP "vr_eyes%s.txt", srb2home, tag);
	FILE* f = fopen(path, "w");
	if (f)
	{
		fprintf(f, "image_w %d\nimage_h %d\n", sEyeDumpW, sEyeDumpH);
		fprintf(f, "view_mode %d\nfp_compose_active %d\nhorizon_lock %d\n",
			sViewMode, vr_fp_compose_active() ? 1 : 0, sHorizonLock ? 1 : 0);
		fprintf(f, "units_per_meter %.9g\ndiorama_scale %.9g\n", sUnitsPerMeter, sDioramaScale);
		fprintf(f, "head_scale %.9g\nstereo_scale %.9g\nrender_scale %.9g\n",
			sHeadScale, sStereoScale, sRenderScale);
		fprintf(f, "focus_units %.9g\nproximity_stereo %.9g\n",
			sFocusUnits, sProximityStereo);
		fprintf(f, "znear %.9g\nzfar %.9g\n", kEyeZNear, kEyeZFar);
		for (int eye = 0; eye < 2; eye++)
		{
			fprintf(f, "eye%d_fov %.9g %.9g %.9g %.9g\n", eye,
				sRenderFov[eye].angleLeft, sRenderFov[eye].angleRight,
				sRenderFov[eye].angleUp, sRenderFov[eye].angleDown);
			fprintf(f, "eye%d_pose_pos %.9g %.9g %.9g\n", eye,
				sRenderPose[eye].position.x, sRenderPose[eye].position.y,
				sRenderPose[eye].position.z);
			fprintf(f, "eye%d_pose_quat %.9g %.9g %.9g %.9g\n", eye,
				sRenderPose[eye].orientation.x, sRenderPose[eye].orientation.y,
				sRenderPose[eye].orientation.z, sRenderPose[eye].orientation.w);
			char name[32];
			snprintf(name, sizeof(name), "eye%d_view", eye);
			vr_dump_mat4(f, name, sEyeView[eye]);
			snprintf(name, sizeof(name), "eye%d_view_mono", eye);
			vr_dump_mat4(f, name, sEyeViewMono[eye]);
			snprintf(name, sizeof(name), "eye%d_proj", eye);
			vr_dump_mat4(f, name, sEyeProj[eye]);
		}
		fclose(f);
	}

	CONS_Printf("VR: eye pair%s dumped to %s (%dx%d, mode %s)\n",
		tag, srb2home, sEyeDumpW, sEyeDumpH, vr_view_mode_name(sViewMode));

	for (int eye = 0; eye < 2; eye++)
	{
		free(sEyeDumpPix[eye]);
		sEyeDumpPix[eye] = NULL;
	}
	sEyeDumpThisFrame = false;

	// Re-arm for the very next stereo frame until the burst is spent.
	sEyeDumpIndex++;
	if (sEyeDumpBurst > 1)
	{
		sEyeDumpBurst--;
		sEyeDumpArm = 1;
	}
	else
	{
		sEyeDumpBurst = 0;
		sEyeDumpIndex = 0;
	}
}

// Grab the finished eye from the single-sample mono target. Runs inside
// vr_end_eye after the MSAA resolve, so what is read is exactly what the eye
// swapchain is about to receive (before the screen effect, which only
// restyles pixels and would pollute a geometry measurement).
static void vr_dump_capture_eye(int eye)
{
	if (eye < 0 || eye >= 2)
		return;
	if (eye == 0 || sEyeDumpW != sMonoW || sEyeDumpH != sMonoH)
	{
		for (int i = 0; i < 2; i++)
		{
			free(sEyeDumpPix[i]);
			sEyeDumpPix[i] = NULL;
		}
		sEyeDumpW = sMonoW;
		sEyeDumpH = sMonoH;
	}
	uint8_t* buf = (uint8_t*)malloc((size_t)sMonoW * sMonoH * 4);
	if (!buf)
	{
		sEyeDumpThisFrame = false;
		return;
	}

	GLint prevRead = 0;
	sGL.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
	sGL.BindFramebuffer(GL_READ_FRAMEBUFFER, sMonoFbo);
	// ReadBuffer acts on the READ binding, which is sMonoFbo here - and
	// COLOR_ATTACHMENT0 is its default, so nothing downstream changes.
	sGL.ReadBuffer(GL_COLOR_ATTACHMENT0);
	sGL.PixelStorei(GL_PACK_ALIGNMENT, 1);
	sGL.ReadPixels(0, 0, sMonoW, sMonoH, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	sGL.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevRead);

	sEyeDumpPix[eye] = buf;
	if (eye == 1 && sEyeDumpPix[0] != NULL)
		vr_dump_write();
}

// ---- view mode plumbing --------------------------------------------------------

static const char* vr_view_mode_name(int mode)
{
	switch (mode)
	{
		case VR_VIEW_FIRST_PERSON: return "first person";
		case VR_VIEW_THEATER:      return "theater";
		case VR_VIEW_DIORAMA:      return "diorama";
		default:                   return "third person";
	}
}

extern "C" int vr_get_view_mode(void)
{
	return sViewMode;
}

// The vr_viewmode cvar mirrors the live mode so the menu, the console and the
// saved config always show reality - including right-stick cycles and
// switches the FP lock refused. Stealth: setting it back must not re-enter
// the cvar's own onchange.
extern "C" consvar_t cv_vr_viewmode;
static void vr_sync_view_mode_cvar(void)
{
	if (cv_vr_viewmode.string != NULL && cv_vr_viewmode.value != sViewMode)
		CV_StealthSetValue(&cv_vr_viewmode, sViewMode);
}

extern "C" void vr_set_view_mode(int mode)
{
	if (mode < VR_VIEW_THIRD_PERSON)
		mode = VR_VIEW_THIRD_PERSON;
	if (mode > VR_VIEW_DIORAMA)
		mode = VR_VIEW_DIORAMA;
	if (mode != sViewMode)
	{
		sViewMode = mode;
		// Stale drama from the previous mode must not greet the new one.
		vr_drama_reset();
		CONS_Printf("VR: view mode %d (%s)\n", mode, vr_view_mode_name(mode));
	}
	vr_sync_view_mode_cvar();
}

extern "C" void vr_cycle_view_mode(void)
{
	// Theater is a menu-only choice. Cycling in game runs the three modes you
	// actually play in - dropping onto a flat screen mid-race because a thumb
	// caught the stick click was never what anyone wanted. Cycling OUT of
	// theater still works, so choosing it from the menu isn't a trap.
	int next = sViewMode;
	for (int i = 0; i < 4; i++)
	{
		next = (next + 1) & 3;
		if (next == VR_VIEW_THEATER)
			continue;
		break;
	}
	vr_set_view_mode(next);
}

extern "C" void vr_set_fp_switch_locked(bool locked)
{
	sFpSwitchLock = locked;
}

extern "C" void vr_set_cockpit_pose(const float posWorld[3], float yawRad, float pitchRad, float rollRad)
{
	sCockpitPos[0] = posWorld[0];
	sCockpitPos[1] = posWorld[1];
	sCockpitPos[2] = posWorld[2];
	sCockpitYaw    = yawRad;
	sCockpitPitch  = pitchRad;
	sCockpitRoll   = rollRad;
	sCockpitValid  = true;
}

extern "C" void vr_set_game_view(const float posWorld[3], float yawRad, float pitchRad, float rollRad, bool skyPass)
{
	if (!skyPass)
	{
		sGameViewPos[0] = posWorld[0];
		sGameViewPos[1] = posWorld[1];
		sGameViewPos[2] = posWorld[2];
		sGameViewYaw    = yawRad;
		sGameViewPitch  = pitchRad;
		sGameViewRoll   = rollRad;
		sGameViewValid  = true;
	}
	// Recompose with this pass's camera: the main feed lands between the
	// skybox pass and the world pass of each eye, so both render against the
	// view they were actually set up with.
	vr_compose_mode_views(skyPass);
}

extern "C" void vr_set_sky_parallax_scale(float scale)
{
	// Negative map scales amplify skybox tracking; cap the amplification the
	// same way the translation itself is capped by comfort elsewhere.
	sSkyParallax = vr_clampf(scale, 0.0f, 64.0f);
}

// A pure store. All the shaping lives in vr_step_drama, so changing the
// immersion level takes effect on the very next frame with no stale target
// left behind.
extern "C" void vr_set_drama(int kind, short spinBinary, short rollBinary)
{
	sDramaKind    = kind;
	sDramaSpinRaw = (float)spinBinary * (2.0f * 3.14159265f / 65536.0f);
	sDramaRollRaw = (float)rollBinary * (2.0f * 3.14159265f / 65536.0f);
}

extern "C" void vr_set_action_cam(int dramatic, int finished)
{
	(void)dramatic; // targets already carry the state; kept for the feed's shape
	sRaceFinished = (finished != 0);
}

extern "C" float vr_fp_eyeheight_units(void)
{
	return sFpEyeHeight;
}

extern "C" float vr_fp_forward_units(void)
{
	return sFpForward;
}

extern "C" bool vr_fp_hide_player(void)
{
	return sInEyePass && sViewMode == VR_VIEW_FIRST_PERSON && !sRaceFinished;
}

extern "C" bool vr_horizon_locked(void)
{
	return sInEyePass && sViewMode == VR_VIEW_FIRST_PERSON && sHorizonLock;
}

// Ease the first-person drama rotation at the headset rate: the game's
// spin/tumble angles step at tic rate; chasing them on the shortest angular
// path turns that staircase into continuous motion. The chase is deliberately
// lazy (low rate plus a hard per-frame cap of ~0.05 rad) so a full spin reads
// as a smooth sweep rather than a violent snap.
// Shape and ease the first-person drama, once per headset frame.
//
// The step is in SECONDS, not frames. The original ceiling was 0.05 rad per
// RENDER frame, which made the same wreck deliver 206 deg/s on a 72 Hz
// headset, 258 on a 90 and 344 on a 120 - the feel moved with the hardware.
static void vr_step_drama(float dt)
{
	if (sViewMode != VR_VIEW_FIRST_PERSON)
	{
		vr_drama_reset();
		return;
	}

	const bool tricking = (sDramaKind == VR_DRAMA_TRICK_ROLL
		|| sDramaKind == VR_DRAMA_TRICK_FLIP);

	if (sImmersion == 0)
	{
		// Exactly what shipped: raw wrapped angles chased at 0.10 with a hard
		// 0.05 rad per-frame ceiling, yaw and the sprite roll on the pitch
		// axis, and tricks feeding nothing - which is what the old game-side
		// "dramatic" predicate amounted to.
		sDramaYawTgt   = tricking ? 0.0f : sDramaSpinRaw;
		sDramaPitchTgt = tricking ? 0.0f : sDramaRollRaw;
		sDramaRollTgt  = 0.0f;
		sDramaRoll     = 0.0f;
		sTrickRunning  = false;
		sTrickAccum    = 0.0f;
		sLeanRoll      = sLeanPitch = 0.0f;

		float dYaw   = wrap_pi(sDramaYawTgt - sDramaYaw) * 0.10f;
		float dPitch = wrap_pi(sDramaPitchTgt - sDramaPitch) * 0.10f;
		if (dYaw > 0.05f) dYaw = 0.05f; else if (dYaw < -0.05f) dYaw = -0.05f;
		if (dPitch > 0.05f) dPitch = 0.05f; else if (dPitch < -0.05f) dPitch = -0.05f;
		sDramaYaw += dYaw;
		sDramaPitch += dPitch;
		if (sDramaYawTgt == 0.0f && fabsf(sDramaYaw) < 0.003f)
			sDramaYaw = 0.0f;
		if (sDramaPitchTgt == 0.0f && fabsf(sDramaPitch) < 0.003f)
			sDramaPitch = 0.0f;
		return;
	}

	const int L = (sImmersion >= 2) ? 1 : 0; // 0 = light, 1 = full

	// Every wreck branch in P_MovePlayer DECREMENTS drawangle, so the lean has
	// a fixed sign and needs no sampling: the head is thrown the way the kart
	// is going.
	switch (sDramaKind)
	{
		case VR_DRAMA_SPIN:
			sDramaYawTgt   = -kSpinYawPeak[L];
			sDramaPitchTgt = 0.0f;
			sDramaRollTgt  = 0.0f;
			break;

		case VR_DRAMA_TUMBLE:
			// Roll leads: a tumble IS a barrel roll in the simulation, and it
			// has been arriving on the pitch axis all this time.
			sDramaYawTgt   = -kTumbleYawPeak[L];
			sDramaPitchTgt = -kTumblePitchPeak[L];
			sDramaRollTgt  = -kTumbleRollPeak[L];
			break;

		case VR_DRAMA_FAULT:
			sDramaYawTgt   = -kFaultYawPeak[L];
			sDramaPitchTgt = 0.0f;
			sDramaRollTgt  = 0.0f;
			break;

		case VR_DRAMA_TRICK_ROLL:
		case VR_DRAMA_TRICK_FLIP:
		{
			// Accumulate the trick's own rotation UNWRAPPED, so a double flip
			// is two flips rather than a wobble.
			if (!sTrickRunning)
			{
				sTrickRunning = true;
				sTrickAccum   = 0.0f;
				sTrickPrevRaw = sDramaSpinRaw;
			}
			sTrickAccum  += wrap_pi(sDramaSpinRaw - sTrickPrevRaw);
			sTrickPrevRaw = sDramaSpinRaw;

			const float t = (sImmersion >= 2)
				? sTrickAccum // you pressed for this one: all of it
				: ((sTrickAccum < 0.0f) ? -kTrickPeak : kTrickPeak);

			// The simulation spins the sprite's YAW for every trick direction,
			// so which axis it reads as from the seat has to come from the
			// trick itself: sideways is a barrel roll, forward or back a flip.
			if (sDramaKind == VR_DRAMA_TRICK_ROLL)
			{
				sDramaRollTgt  = t;
				sDramaYawTgt   = 0.0f;
				sDramaPitchTgt = 0.0f;
			}
			else
			{
				sDramaPitchTgt = t;
				sDramaYawTgt   = 0.0f;
				sDramaRollTgt  = 0.0f;
			}
			break;
		}

		default:
			sDramaYawTgt = sDramaPitchTgt = sDramaRollTgt = 0.0f;
			break;
	}

	if (!tricking && sTrickRunning)
	{
		// Landed. Fold whole revolutions out of the eased value first - the
		// picture is identical modulo a full turn - so what is left to unwind
		// is never more than half a turn and the return to level reads as the
		// world settling rather than a whip.
		const float tau = 6.28318531f;
		sTrickRunning = false;
		sTrickAccum   = 0.0f;
		sDramaRoll  -= floorf(sDramaRoll  / tau + 0.5f) * tau;
		sDramaPitch -= floorf(sDramaPitch / tau + 0.5f) * tau;
	}

	// Attack slowly, track a trick fast, settle back a little quicker than the
	// attack. No wrap_pi in here: the shaped targets are small and the trick
	// channel is deliberately unwrapped. Wrapping the error is exactly what
	// made the old chase reverse mid-spin.
	const float k = tricking
		? ((sImmersion >= 2) ? 20.0f : 8.0f)
		: ((sDramaKind == VR_DRAMA_NONE) ? 4.0f : 3.0f);
	const float a = 1.0f - expf(-k * dt);

	// Safety net only, radians per SECOND. Nothing but a Full trick comes near
	// it, and the return to level gets a gentler one.
	const float lim = ((sDramaKind == VR_DRAMA_NONE) ? 6.0f : 16.0f) * dt;

	sDramaYaw   += vr_clampf((sDramaYawTgt   - sDramaYaw)   * a, -lim, lim);
	sDramaPitch += vr_clampf((sDramaPitchTgt - sDramaPitch) * a, -lim, lim);
	sDramaRoll  += vr_clampf((sDramaRollTgt  - sDramaRoll)  * a, -lim, lim);

	if (sDramaYawTgt == 0.0f && fabsf(sDramaYaw) < 0.002f)
		sDramaYaw = 0.0f;
	if (sDramaPitchTgt == 0.0f && fabsf(sDramaPitch) < 0.002f)
		sDramaPitch = 0.0f;
	if (sDramaRollTgt == 0.0f && fabsf(sDramaRoll) < 0.002f)
		sDramaRoll = 0.0f;

	// Lean. sGameViewRoll/Pitch are ANG2RAD of an UNSIGNED angle_t, so a
	// rightward roll arrives near 2*pi rather than as a small negative - wrap
	// before scaling or a 10 degree tilt becomes a 350 degree one. Eased
	// slower than the drama so a landing does not snap the head.
	float leanRollTgt = 0.0f, leanPitchTgt = 0.0f;
	if (sHorizonLock)
	{
		const float f = kLeanFrac[L];
		leanRollTgt  = vr_clampf(wrap_pi(sGameViewRoll)  * f, -kLeanRollMax,  kLeanRollMax);
		leanPitchTgt = vr_clampf(wrap_pi(sGameViewPitch) * f, -kLeanPitchMax, kLeanPitchMax);
	}
	const float la = 1.0f - expf(-6.0f * dt);
	sLeanRoll  += (leanRollTgt  - sLeanRoll)  * la;
	sLeanPitch += (leanPitchTgt - sLeanPitch) * la;

	// Boost kick: the kart shoves, so the head goes BACK. Folded into the same
	// lean pitch, because it is the same thing physically - your neck losing
	// an argument with an acceleration - and because that keeps it inside the
	// clamp that already stops pitch running away. Nose-up, hence negative:
	// the same sign a landing dash's own shove would give you.
	sLeanPitch -= sBoostKick * (kLeanPitchMax * ((sImmersion >= 2) ? 1.0f : 0.6f));
}

// ---- motion-controller input (OpenXR actions) ---------------------------------
// One "gameplay" action set covering both thumbsticks, the face buttons, the
// menu button, stick clicks, triggers, grips, and a haptic output per hand.
// Suggested bindings cover Quest Touch (and the Quest 3 / Pro native Touch
// Plus profile), Index, Reverb G2, WMR wands, Vive wands, and the khr simple
// fallback. The input bridge mirrors this state into the game's event queue
// as a synthetic gamepad, so the controllers work everywhere a pad does.
static XrActionSet sActionSet  = XR_NULL_HANDLE;
static XrAction sActMove       = XR_NULL_HANDLE; // left thumbstick (vector2)
static XrAction sActCam        = XR_NULL_HANDLE; // right thumbstick (vector2)
static XrAction sActBtnA       = XR_NULL_HANDLE;
static XrAction sActBtnB       = XR_NULL_HANDLE;
static XrAction sActBtnX       = XR_NULL_HANDLE;
static XrAction sActBtnY       = XR_NULL_HANDLE;
static XrAction sActMenuBtn    = XR_NULL_HANDLE;
static XrAction sActLStick     = XR_NULL_HANDLE; // thumbstick clicks
static XrAction sActRStick     = XR_NULL_HANDLE;
static XrAction sActLTrigger   = XR_NULL_HANDLE; // analog 0..1, digital here via hysteresis
static XrAction sActRTrigger   = XR_NULL_HANDLE;
static XrAction sActLGrip      = XR_NULL_HANDLE;
static XrAction sActRGrip      = XR_NULL_HANDLE;
static XrAction sActHaptic     = XR_NULL_HANDLE; // vibration output, per-hand subactions
static XrPath   sHandPath[2]   = { XR_NULL_PATH, XR_NULL_PATH };
static bool     sInputAttached = false;           // action set attached to the session
static bool     sProfilesLogged = false;          // active-profile log fired this session
static unsigned sCtrlButtons   = 0;               // VR_BTN_* mask, refreshed each xrSyncActions
static float    sCtrlStick[2][2] = {{ 0 }};       // [hand][x,y], +x right +y up
static float    sRumbleAmp     = 0.0f;            // armed rumble amplitude (0 = off)
static XrTime   sRumbleUntil   = 0;               // stop re-arming past this time

static XrAction vr_make_action(XrActionType type, const char* name, const char* localized, bool perHand)
{
	XrActionCreateInfo aci = { XR_TYPE_ACTION_CREATE_INFO };
	aci.actionType = type;
	strncpy(aci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
	strncpy(aci.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
	if (perHand)
	{
		aci.countSubactionPaths = 2;
		aci.subactionPaths = sHandPath;
	}
	XrAction a = XR_NULL_HANDLE;
	if (!xrok(xrCreateAction(sActionSet, &aci, &a), name))
		return XR_NULL_HANDLE;
	return a;
}

struct VrBind
{
	XrAction action;
	const char* path;
};

static void vr_suggest_profile(const char* profilePath, const VrBind* binds, int count)
{
	XrPath profile = XR_NULL_PATH;
	if (XR_FAILED(xrStringToPath(sInstance, profilePath, &profile)))
		return;
	XrActionSuggestedBinding sb[32];
	uint32_t n = 0;
	for (int i = 0; i < count && n < 32; i++)
	{
		if (binds[i].action == XR_NULL_HANDLE)
			continue;
		XrPath p = XR_NULL_PATH;
		if (XR_FAILED(xrStringToPath(sInstance, binds[i].path, &p)))
			continue;
		sb[n].action  = binds[i].action;
		sb[n].binding = p;
		n++;
	}
	if (n == 0)
		return;
	XrInteractionProfileSuggestedBinding spb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	spb.interactionProfile = profile;
	spb.suggestedBindings = sb;
	spb.countSuggestedBindings = n;
	// Not fatal if the runtime rejects a profile it doesn't know; the others
	// still apply.
	if (xrok(xrSuggestInteractionProfileBindings(sInstance, &spb), profilePath))
		CONS_Printf("VR: suggested bindings for %s\n", profilePath);
}

// Create the action set, suggest the per-device bindings and attach to the
// session. Runs once at the end of vr_boot; any failure leaves VR fully
// functional, just without motion controllers.
static void vr_input_create(void)
{
	sInputAttached = false;
	sCtrlButtons = 0;
	memset(sCtrlStick, 0, sizeof(sCtrlStick));
	xrStringToPath(sInstance, "/user/hand/left",  &sHandPath[0]);
	xrStringToPath(sInstance, "/user/hand/right", &sHandPath[1]);

	XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strncpy(asci.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
	strncpy(asci.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
	if (!xrok(xrCreateActionSet(sInstance, &asci, &sActionSet), "xrCreateActionSet"))
	{
		sActionSet = XR_NULL_HANDLE;
		return;
	}

	sActMove     = vr_make_action(XR_ACTION_TYPE_VECTOR2F_INPUT,  "move",          "Steer",             false);
	sActCam      = vr_make_action(XR_ACTION_TYPE_VECTOR2F_INPUT,  "camera",        "Camera",            false);
	sActBtnA     = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "button_a",      "A Button",          false);
	sActBtnB     = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "button_b",      "B Button",          false);
	sActBtnX     = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "button_x",      "X Button",          false);
	sActBtnY     = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "button_y",      "Y Button",          false);
	sActMenuBtn  = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "menu",          "Pause",             false);
	sActLStick   = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "left_stick",    "Left Stick Click",  false);
	sActRStick   = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "right_stick",   "Right Stick Click", false);
	sActLTrigger = vr_make_action(XR_ACTION_TYPE_FLOAT_INPUT,     "left_trigger",  "Left Trigger",      false);
	sActRTrigger = vr_make_action(XR_ACTION_TYPE_FLOAT_INPUT,     "right_trigger", "Right Trigger",     false);
	sActLGrip    = vr_make_action(XR_ACTION_TYPE_FLOAT_INPUT,     "left_grip",     "Left Grip",         false);
	sActRGrip    = vr_make_action(XR_ACTION_TYPE_FLOAT_INPUT,     "right_grip",    "Right Grip",        false);
	sActHaptic   = vr_make_action(XR_ACTION_TYPE_VIBRATION_OUTPUT,"rumble",        "Rumble",            true);

	// Quest Touch (the right controller's Oculus button is reserved by the
	// system, so it isn't bound).
	const VrBind touch[] = {
		{ sActMove,     "/user/hand/left/input/thumbstick" },
		{ sActCam,      "/user/hand/right/input/thumbstick" },
		{ sActBtnA,     "/user/hand/right/input/a/click" },
		{ sActBtnB,     "/user/hand/right/input/b/click" },
		{ sActBtnX,     "/user/hand/left/input/x/click" },
		{ sActBtnY,     "/user/hand/left/input/y/click" },
		{ sActMenuBtn,  "/user/hand/left/input/menu/click" },
		{ sActLStick,   "/user/hand/left/input/thumbstick/click" },
		{ sActRStick,   "/user/hand/right/input/thumbstick/click" },
		{ sActLTrigger, "/user/hand/left/input/trigger/value" },
		{ sActRTrigger, "/user/hand/right/input/trigger/value" },
		{ sActLGrip,    "/user/hand/left/input/squeeze/value" },
		{ sActRGrip,    "/user/hand/right/input/squeeze/value" },
		{ sActHaptic,   "/user/hand/left/output/haptic" },
		{ sActHaptic,   "/user/hand/right/output/haptic" },
	};
	vr_suggest_profile("/interaction_profiles/oculus/touch_controller", touch, (int)(sizeof(touch) / sizeof(touch[0])));

	// Valve Index: same layout, but A/B exist on both hands and there's no
	// menu button.
	const VrBind index[] = {
		{ sActMove,     "/user/hand/left/input/thumbstick" },
		{ sActCam,      "/user/hand/right/input/thumbstick" },
		{ sActBtnA,     "/user/hand/right/input/a/click" },
		{ sActBtnB,     "/user/hand/right/input/b/click" },
		{ sActBtnX,     "/user/hand/left/input/a/click" },
		{ sActBtnY,     "/user/hand/left/input/b/click" },
		{ sActLStick,   "/user/hand/left/input/thumbstick/click" },
		{ sActRStick,   "/user/hand/right/input/thumbstick/click" },
		{ sActLTrigger, "/user/hand/left/input/trigger/value" },
		{ sActRTrigger, "/user/hand/right/input/trigger/value" },
		{ sActLGrip,    "/user/hand/left/input/squeeze/value" },
		{ sActRGrip,    "/user/hand/right/input/squeeze/value" },
		{ sActHaptic,   "/user/hand/left/output/haptic" },
		{ sActHaptic,   "/user/hand/right/output/haptic" },
	};
	vr_suggest_profile("/interaction_profiles/valve/index_controller", index, (int)(sizeof(index) / sizeof(index[0])));

	// Quest 3 / Quest Pro native profile: identical layout to Touch, so the
	// same table applies. Suggesting it explicitly matters: with only the
	// older Touch bindings suggested, the runtime auto-translates them onto
	// Touch Plus and that translation can land buttons on the wrong hand.
	if (sHasTouchPlus)
		vr_suggest_profile("/interaction_profiles/meta/touch_controller_plus", touch, (int)(sizeof(touch) / sizeof(touch[0])));

	// HP Reverb G2: same control set as Touch (a/b right, x/y left, sticks,
	// analog squeeze).
	if (sHasHpMR)
		vr_suggest_profile("/interaction_profiles/hp/mixed_reality_controller", touch, (int)(sizeof(touch) / sizeof(touch[0])));

	// Windows Mixed Reality wands: sticks and triggers as usual; no face
	// buttons, so the trackpad clicks stand in for A (right) and B (left).
	// Squeeze is a click on these, not analog.
	const VrBind wmr[] = {
		{ sActMove,     "/user/hand/left/input/thumbstick" },
		{ sActCam,      "/user/hand/right/input/thumbstick" },
		{ sActBtnA,     "/user/hand/right/input/trackpad/click" },
		{ sActBtnB,     "/user/hand/left/input/trackpad/click" },
		{ sActMenuBtn,  "/user/hand/left/input/menu/click" },
		{ sActLStick,   "/user/hand/left/input/thumbstick/click" },
		{ sActRStick,   "/user/hand/right/input/thumbstick/click" },
		{ sActLTrigger, "/user/hand/left/input/trigger/value" },
		{ sActRTrigger, "/user/hand/right/input/trigger/value" },
		{ sActLGrip,    "/user/hand/left/input/squeeze/click" },
		{ sActRGrip,    "/user/hand/right/input/squeeze/click" },
		{ sActHaptic,   "/user/hand/left/output/haptic" },
		{ sActHaptic,   "/user/hand/right/output/haptic" },
	};
	vr_suggest_profile("/interaction_profiles/microsoft/motion_controller", wmr, (int)(sizeof(wmr) / sizeof(wmr[0])));

	// Vive wands: no sticks at all, so the trackpads steer and drive the
	// camera. Best-effort.
	const VrBind vive[] = {
		{ sActMove,     "/user/hand/left/input/trackpad" },
		{ sActCam,      "/user/hand/right/input/trackpad" },
		{ sActBtnA,     "/user/hand/right/input/trackpad/click" },
		{ sActBtnB,     "/user/hand/left/input/trackpad/click" },
		{ sActMenuBtn,  "/user/hand/left/input/menu/click" },
		{ sActLTrigger, "/user/hand/left/input/trigger/value" },
		{ sActRTrigger, "/user/hand/right/input/trigger/value" },
		{ sActLGrip,    "/user/hand/left/input/squeeze/click" },
		{ sActRGrip,    "/user/hand/right/input/squeeze/click" },
		{ sActHaptic,   "/user/hand/left/output/haptic" },
		{ sActHaptic,   "/user/hand/right/output/haptic" },
	};
	vr_suggest_profile("/interaction_profiles/htc/vive_controller", vive, (int)(sizeof(vive) / sizeof(vive[0])));

	// Bare-minimum fallback profile every runtime understands (select + menu
	// only).
	const VrBind simple[] = {
		{ sActBtnA,    "/user/hand/right/input/select/click" },
		{ sActBtnB,    "/user/hand/left/input/select/click" },
		{ sActMenuBtn, "/user/hand/left/input/menu/click" },
		{ sActHaptic,  "/user/hand/left/output/haptic" },
		{ sActHaptic,  "/user/hand/right/output/haptic" },
	};
	vr_suggest_profile("/interaction_profiles/khr/simple_controller", simple, (int)(sizeof(simple) / sizeof(simple[0])));

	XrSessionActionSetsAttachInfo sai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	sai.countActionSets = 1;
	sai.actionSets = &sActionSet;
	if (!xrok(xrAttachSessionActionSets(sSession, &sai), "xrAttachSessionActionSets"))
		return;
	sInputAttached = true;
	CONS_Printf("VR: motion controllers ready (action set attached).\n");
}

// Print which interaction profile the runtime actually bound for each hand.
// Fires on the first focus and whenever the runtime reports a profile change;
// this line is the first thing to check when a controller behaves oddly
// (wrong hand, dead buttons), since it shows what the runtime matched us to.
static void vr_log_active_profiles(void)
{
	if (!sInputAttached || sSession == XR_NULL_HANDLE)
		return;
	static const char* handName[2] = { "left", "right" };
	for (int h = 0; h < 2; h++)
	{
		char buf[XR_MAX_PATH_LENGTH];
		snprintf(buf, sizeof(buf), "none (not bound)");
		XrInteractionProfileState ips = { XR_TYPE_INTERACTION_PROFILE_STATE };
		if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(sSession, sHandPath[h], &ips))
			&& ips.interactionProfile != XR_NULL_PATH)
		{
			uint32_t len = 0;
			xrPathToString(sInstance, ips.interactionProfile, sizeof(buf), &len, buf);
		}
		CONS_Printf("VR: %s controller profile: %s\n", handName[h], buf);
	}
}

static bool vr_action_bool(XrAction a)
{
	if (a == XR_NULL_HANDLE)
		return false;
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	gi.action = a;
	XrActionStateBoolean st = { XR_TYPE_ACTION_STATE_BOOLEAN };
	return XR_SUCCEEDED(xrGetActionStateBoolean(sSession, &gi, &st)) && st.isActive && st.currentState;
}

static float vr_action_float(XrAction a)
{
	if (a == XR_NULL_HANDLE)
		return 0.0f;
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	gi.action = a;
	XrActionStateFloat st = { XR_TYPE_ACTION_STATE_FLOAT };
	if (XR_FAILED(xrGetActionStateFloat(sSession, &gi, &st)) || !st.isActive)
		return 0.0f;
	return st.currentState;
}

static void vr_action_vec2(XrAction a, float out[2])
{
	out[0] = out[1] = 0.0f;
	if (a == XR_NULL_HANDLE)
		return;
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	gi.action = a;
	XrActionStateVector2f st = { XR_TYPE_ACTION_STATE_VECTOR2F };
	if (XR_FAILED(xrGetActionStateVector2f(sSession, &gi, &st)) || !st.isActive)
		return;
	out[0] = st.currentState.x;
	out[1] = st.currentState.y;
}

// Analog trigger/grip to a digital button with hysteresis: press past 60%,
// release under 40%, so a finger resting lightly on the trigger can't
// flicker the bound action.
static bool vr_analog_latch(float v, bool held)
{
	return held ? (v > 0.4f) : (v > 0.6f);
}

// Pull fresh controller state from the runtime. Once per begun frame, from
// vr_begin_frame.
static void vr_input_sync(void)
{
	if (!sInputAttached)
		return;
	XrActiveActionSet active = { sActionSet, XR_NULL_PATH };
	XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
	si.countActiveActionSets = 1;
	si.activeActionSets = &active;
	// XR_SESSION_NOT_FOCUSED is a success code that means "no input for you"
	// (headset off, runtime menu up). Release everything so a button held at
	// that moment can't stay stuck down, and disarm rumble so the controllers
	// don't keep buzzing in their holders.
	if (xrSyncActions(sSession, &si) != XR_SUCCESS)
	{
		sCtrlButtons = 0;
		memset(sCtrlStick, 0, sizeof(sCtrlStick));
		sRumbleAmp = 0.0f;
		return;
	}
	unsigned b = 0;
	if (vr_action_bool(sActBtnA))
		b |= VR_BTN_A;
	if (vr_action_bool(sActBtnB))
		b |= VR_BTN_B;
	if (vr_action_bool(sActBtnX))
		b |= VR_BTN_X;
	if (vr_action_bool(sActBtnY))
		b |= VR_BTN_Y;
	if (vr_action_bool(sActMenuBtn))
		b |= VR_BTN_MENU;
	if (vr_action_bool(sActLStick))
		b |= VR_BTN_LSTICK;
	if (vr_action_bool(sActRStick))
		b |= VR_BTN_RSTICK;
	if (vr_analog_latch(vr_action_float(sActLTrigger), sCtrlButtons & VR_BTN_LTRIGGER))
		b |= VR_BTN_LTRIGGER;
	if (vr_analog_latch(vr_action_float(sActRTrigger), sCtrlButtons & VR_BTN_RTRIGGER))
		b |= VR_BTN_RTRIGGER;
	if (vr_analog_latch(vr_action_float(sActLGrip), sCtrlButtons & VR_BTN_LGRIP))
		b |= VR_BTN_LGRIP;
	if (vr_analog_latch(vr_action_float(sActRGrip), sCtrlButtons & VR_BTN_RGRIP))
		b |= VR_BTN_RGRIP;
	sCtrlButtons = b;
	vr_action_vec2(sActMove, sCtrlStick[0]);
	vr_action_vec2(sActCam,  sCtrlStick[1]);

	// Haptics: while armed, re-arm a SHORT burst every frame instead of ever
	// submitting one long vibration. Runtimes don't all honor stop requests
	// promptly (or at all, over wireless) - a long one-shot buzz that misses
	// its stop can never be cancelled. Short bursts die on their own right
	// after the last re-arm, so a lost stop can't strand the motor.
	if (sRumbleAmp > 0.0f && sActHaptic != XR_NULL_HANDLE
		&& sFrameState.predictedDisplayTime < sRumbleUntil)
	{
		XrHapticVibration vib = { XR_TYPE_HAPTIC_VIBRATION };
		vib.duration  = 60000000; // 60 ms: outlasts one frame, dies fast once re-arming stops
		vib.frequency = XR_FREQUENCY_UNSPECIFIED;
		vib.amplitude = sRumbleAmp;
		for (int h = 0; h < 2; h++)
		{
			XrHapticActionInfo hai = { XR_TYPE_HAPTIC_ACTION_INFO };
			hai.action = sActHaptic;
			hai.subactionPath = sHandPath[h];
			xrApplyHapticFeedback(sSession, &hai, (const XrHapticBaseHeader*)&vib);
		}
	}
}

extern "C" bool vr_controllers_active(void)
{
	return sInputAttached && sRunning && sState == XR_SESSION_STATE_FOCUSED;
}

extern "C" unsigned vr_controller_buttons(void)
{
	return vr_controllers_active() ? sCtrlButtons : 0;
}

extern "C" void vr_controller_stick(int hand, float out[2])
{
	if (!vr_controllers_active() || hand < 0 || hand > 1)
	{
		out[0] = out[1] = 0.0f;
		return;
	}
	out[0] = sCtrlStick[hand][0];
	out[1] = sCtrlStick[hand][1];
}

// Arm the rumble. No vibration is submitted here: vr_input_sync re-arms a
// short burst each frame while armed, so a runtime that mishandles stop
// requests can't strand the motor on.
extern "C" void vr_controller_rumble(float strength, float seconds)
{
	if (!vr_controllers_active() || sActHaptic == XR_NULL_HANDLE)
		return;
	if (strength < 0.0f)
		strength = 0.0f;
	if (strength > 1.0f)
		strength = 1.0f;
	sRumbleAmp   = strength;
	sRumbleUntil = sFrameState.predictedDisplayTime + (XrTime)(seconds * 1e9);
}

extern "C" void vr_controller_rumble_stop(void)
{
	sRumbleAmp = 0.0f;
	sRumbleUntil = 0;
}

static void vr_boot(void)
{
	CONS_Printf("VR: booting OpenXR...\n");

	// Probe the optional controller-profile extensions before creating the
	// instance: the Quest 3 / Pro native Touch Plus profile and the HP Reverb
	// G2 profile only exist behind extensions, and suggesting the native
	// profile avoids the runtime's auto-translation (which can land buttons
	// on the wrong hand).
	sHasTouchPlus = false;
	sHasHpMR = false;
	{
		uint32_t ec = 0;
		if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, 0, &ec, NULL)) && ec > 0)
		{
			XrExtensionProperties* ep = (XrExtensionProperties*)calloc(ec, sizeof(XrExtensionProperties));
			for (uint32_t i = 0; i < ec; i++)
				ep[i].type = XR_TYPE_EXTENSION_PROPERTIES;
			if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, ec, &ec, ep)))
			{
				for (uint32_t i = 0; i < ec; i++)
				{
					if (strcmp(ep[i].extensionName, "XR_META_touch_controller_plus") == 0)
						sHasTouchPlus = true;
					if (strcmp(ep[i].extensionName, "XR_EXT_hp_mixed_reality_controller") == 0)
						sHasHpMR = true;
					if (strcmp(ep[i].extensionName, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME) == 0)
						sDepthExtAvailable = true;
				}
			}
			free(ep);
		}
	}

	const char* exts[5];
	uint32_t nexts = 0;
#if defined(_WIN32)
	exts[nexts++] = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
#else
	exts[nexts++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
	exts[nexts++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
#endif
	if (sHasTouchPlus)
		exts[nexts++] = "XR_META_touch_controller_plus";
	if (sHasHpMR)
		exts[nexts++] = "XR_EXT_hp_mixed_reality_controller";
	if (sDepthExtAvailable)
		exts[nexts++] = XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME;

	XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
	ici.enabledExtensionCount = nexts;
	ici.enabledExtensionNames = exts;
#if defined(__ANDROID__)
	XrInstanceCreateInfoAndroidKHR androidChain = vr_android_instance_chain();
	ici.next = &androidChain;
#endif
	strncpy(ici.applicationInfo.applicationName, "RingRacers", XR_MAX_APPLICATION_NAME_SIZE - 1);
	ici.applicationInfo.applicationVersion = 1;
	strncpy(ici.applicationInfo.engineName, "srb2", XR_MAX_ENGINE_NAME_SIZE - 1);
	ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	if (!xrok(xrCreateInstance(&ici, &sInstance), "xrCreateInstance")) { vr_shutdown(); return; }

	XrInstanceProperties props = { XR_TYPE_INSTANCE_PROPERTIES };
	if (XR_SUCCEEDED(xrGetInstanceProperties(sInstance, &props)))
		CONS_Printf("VR: runtime: %s\n", props.runtimeName);

	XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	{
		XrResult r = xrGetSystem(sInstance, &sgi, &sSystemId);
		if (XR_FAILED(r))
		{
			// FORM_FACTOR_UNAVAILABLE means the runtime exists but the headset
			// isn't ready yet (asleep, Link still connecting). That can clear
			// up on its own, so mark this boot attempt transient.
			if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
				sBootTransient = true;
			xrok(r, "xrGetSystem");
			vr_shutdown();
			return;
		}
	}

	// OpenXR requires querying the GL requirements before creating a session
	// (skipping it is a validation error on strict runtimes).
#if defined(_WIN32)
	if (!xrok(xrGetInstanceProcAddr(sInstance, "xrGetOpenGLGraphicsRequirementsKHR",
			(PFN_xrVoidFunction*)&pfnGetGLReq), "get xrGetOpenGLGraphicsRequirementsKHR")) { vr_shutdown(); return; }
	XrGraphicsRequirementsOpenGLKHR glReq = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
	if (!xrok(pfnGetGLReq(sInstance, sSystemId, &glReq), "xrGetOpenGLGraphicsRequirementsKHR")) { vr_shutdown(); return; }
#else
	if (!xrok(xrGetInstanceProcAddr(sInstance, "xrGetOpenGLESGraphicsRequirementsKHR",
			(PFN_xrVoidFunction*)&pfnGetGLReq), "get xrGetOpenGLESGraphicsRequirementsKHR")) { vr_shutdown(); return; }
	XrGraphicsRequirementsOpenGLESKHR glReq = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
	if (!xrok(pfnGetGLReq(sInstance, sSystemId, &glReq), "xrGetOpenGLESGraphicsRequirementsKHR")) { vr_shutdown(); return; }
#endif

	// Bind the session to the game's live GL context. vr_begin_frame only runs
	// on the main thread with the SDL GL context current, so these are valid.
#if defined(_WIN32)
	HDC   hdc  = wglGetCurrentDC();
	HGLRC glrc = wglGetCurrentContext();
	if (!hdc || !glrc)
	{
		CONS_Printf("VR: no current WGL context - is the OpenGL renderer active? (VR needs it)\n");
		vr_shutdown();
		return;
	}

	// Load this module's GL entry points from the same context.
	if (!sGlReady)
	{
		if (!gladLoadGLContext(&sGL, (GLADloadfunc)SDL_GL_GetProcAddress))
		{
			CONS_Printf("VR: failed to load GL functions\n");
			vr_shutdown();
			return;
		}
		sGlReady = true;
	}

	XrGraphicsBindingOpenGLWin32KHR gb = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
	gb.hDC = hdc;
	gb.hGLRC = glrc;
#else
	// THE BINDING WANTS THE EGLConfig, AND EGL WILL NOT SIMPLY HAND ONE BACK.
	// eglGetCurrent* gives the display, the context and the surface, but the
	// config the context was made against can only be read out as an id, so it
	// has to be turned back into a config by asking for the one with that id.
	EGLDisplay dpy = eglGetCurrentDisplay();
	EGLContext ctx = eglGetCurrentContext();
	if (dpy == EGL_NO_DISPLAY || ctx == EGL_NO_CONTEXT)
	{
		CONS_Printf("VR: no current EGL context - VR needs the GL renderer running\n");
		vr_shutdown();
		return;
	}

	EGLint configId = 0;
	eglQueryContext(dpy, ctx, EGL_CONFIG_ID, &configId);
	EGLConfig config = NULL;
	{
		const EGLint want[] = { EGL_CONFIG_ID, configId, EGL_NONE };
		EGLint got = 0;
		if (!eglChooseConfig(dpy, want, &config, 1, &got) || got < 1)
		{
			CONS_Printf("VR: could not recover the EGL config (id %d)\n", (int)configId);
			vr_shutdown();
			return;
		}
	}

	// Nothing to load: the entry points are linked in and the struct already
	// points at them. The flag still moves so the teardown path reads the same.
	sGlReady = true;

	XrGraphicsBindingOpenGLESAndroidKHR gb = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
	gb.display = dpy;
	gb.config = config;
	gb.context = ctx;
#endif
	XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
	sci.next = &gb;
	sci.systemId = sSystemId;
	if (!xrok(xrCreateSession(sInstance, &sci, &sSession), "xrCreateSession")) { vr_shutdown(); return; }

	XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rsci.poseInReferenceSpace.orientation.w = 1.0f;
	if (!xrok(xrCreateReferenceSpace(sSession, &rsci, &sLocalSpace), "xrCreateReferenceSpace")) { vr_shutdown(); return; }

	XrViewConfigurationType vct = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	uint32_t n = 0;
	if (!xrok(xrEnumerateViewConfigurationViews(sInstance, sSystemId, vct, 0, &n, NULL), "enum view count")) { vr_shutdown(); return; }
	if (n > 2)
		n = 2;
	for (uint32_t i = 0; i < n; i++)
		sViewConfigs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	if (!xrok(xrEnumerateViewConfigurationViews(sInstance, sSystemId, vct, n, &n, sViewConfigs), "enum views")) { vr_shutdown(); return; }
	sViewCount = n;
	CONS_Printf("VR: %u eyes, %ux%u per eye recommended\n", n,
		sViewConfigs[0].recommendedImageRectWidth, sViewConfigs[0].recommendedImageRectHeight);

	int64_t fmt = vr_choose_swapchain_format();
	for (uint32_t e = 0; e < sViewCount; e++)
	{
		XrSwapchainCreateInfo scci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		scci.format = fmt;
		scci.sampleCount = 1;
		scci.faceCount = 1;
		scci.arraySize = 1;
		scci.mipCount = 1;
		// Try the recommended size, then fall back if GPU memory is tight so
		// VR still boots on smaller cards.
		static const float scEyeScales[] = { 1.0f, 0.85f, 0.7f, 0.55f, 0.4f };
		XrResult scRes = XR_ERROR_RUNTIME_FAILURE;
		for (int s = 0; s < (int)(sizeof(scEyeScales) / sizeof(scEyeScales[0])); s++)
		{
			scci.width  = (uint32_t)(sViewConfigs[e].recommendedImageRectWidth  * scEyeScales[s]);
			scci.height = (uint32_t)(sViewConfigs[e].recommendedImageRectHeight * scEyeScales[s]);
			scRes = xrCreateSwapchain(sSession, &scci, &sEye[e].handle);
			if (XR_SUCCEEDED(scRes))
			{
				if (scEyeScales[s] < 1.0f)
					CONS_Printf("VR: eye %u swapchain fell back to %ux%u\n", e, scci.width, scci.height);
				break;
			}
		}
		if (!XR_SUCCEEDED(scRes))
		{
			CONS_Printf("VR: xrCreateSwapchain failed at every size - VR disabled.\n");
			vr_shutdown();
			return;
		}
		sEye[e].w = scci.width;
		sEye[e].h = scci.height;

		uint32_t imgN = 0;
		xrEnumerateSwapchainImages(sEye[e].handle, 0, &imgN, NULL);
		sEye[e].images = (XrSwapchainImageOpenGLKHR*)calloc(imgN, sizeof(XrSwapchainImageOpenGLKHR));
		for (uint32_t i = 0; i < imgN; i++)
			sEye[e].images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
		xrEnumerateSwapchainImages(sEye[e].handle, imgN, &imgN, (XrSwapchainImageBaseHeader*)sEye[e].images);
		sEye[e].imgCount = imgN;
	}

	// Depth swapchains, same size as the colour ones so the runtime can pair
	// them pixel for pixel. Everything here is best-effort: a runtime without
	// the extension, without a depth format we can write, or short of memory
	// just leaves sDepthLayerReady false and the projection layer submits
	// exactly as it did before.
	if (sDepthExtAvailable)
	{
		const int64_t depthFmts[] = { 0x81A6 /*GL_DEPTH_COMPONENT24*/,
		                              0x8CAC /*GL_DEPTH_COMPONENT32F*/,
		                              0x81A7 /*GL_DEPTH_COMPONENT32*/,
		                              0x81A5 /*GL_DEPTH_COMPONENT16*/ };
		int64_t depthFmt = 0;
		uint32_t fc = 0;
		if (XR_SUCCEEDED(xrEnumerateSwapchainFormats(sSession, 0, &fc, NULL)) && fc > 0)
		{
			int64_t* fl = (int64_t*)calloc(fc, sizeof(int64_t));
			if (XR_SUCCEEDED(xrEnumerateSwapchainFormats(sSession, fc, &fc, fl)))
			{
				for (size_t d = 0; d < sizeof(depthFmts) / sizeof(depthFmts[0]) && !depthFmt; d++)
					for (uint32_t i = 0; i < fc; i++)
						if (fl[i] == depthFmts[d]) { depthFmt = fl[i]; break; }
			}
			free(fl);
		}

		bool depthOk = (depthFmt != 0);
		for (uint32_t e = 0; depthOk && e < sViewCount; e++)
		{
			XrSwapchainCreateInfo dci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
			dci.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
			dci.format = depthFmt;
			dci.sampleCount = 1;
			dci.width  = sEye[e].w;
			dci.height = sEye[e].h;
			dci.faceCount = 1;
			dci.arraySize = 1;
			dci.mipCount = 1;
			if (!XR_SUCCEEDED(xrCreateSwapchain(sSession, &dci, &sEyeDepth[e].handle)))
			{
				depthOk = false;
				break;
			}
			sEyeDepth[e].w = dci.width;
			sEyeDepth[e].h = dci.height;

			uint32_t imgN = 0;
			xrEnumerateSwapchainImages(sEyeDepth[e].handle, 0, &imgN, NULL);
			sEyeDepth[e].images = (XrSwapchainImageOpenGLKHR*)calloc(imgN, sizeof(XrSwapchainImageOpenGLKHR));
			for (uint32_t i = 0; i < imgN; i++)
				sEyeDepth[e].images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
			xrEnumerateSwapchainImages(sEyeDepth[e].handle, imgN, &imgN, (XrSwapchainImageBaseHeader*)sEyeDepth[e].images);
			sEyeDepth[e].imgCount = imgN;
		}

		if (depthOk)
		{
			sGL.GenFramebuffers(1, &sEyeDepthFbo);
			// Render depth in the runtime's own format from here on, so the
			// per-eye copy is a legal blit (see sDepthGlFormat). The mono
			// target rebuilds on the next eye pass and picks this up.
			sDepthGlFormat = (unsigned int)depthFmt;
			sMonoBuiltMsaa = -1;
			sDepthLayerReady = true;
			CONS_Printf("VR: submitting per-eye depth (format 0x%llx) - the compositor can reproject by distance\n",
				(unsigned long long)depthFmt);
		}
		else
		{
			for (uint32_t e = 0; e < 2; e++)
			{
				if (sEyeDepth[e].handle != XR_NULL_HANDLE)
					xrDestroySwapchain(sEyeDepth[e].handle);
				free(sEyeDepth[e].images);
				memset(&sEyeDepth[e], 0, sizeof(sEyeDepth[e]));
			}
			CONS_Printf("VR: no usable depth swapchain format; reprojection stays rotation-only\n");
		}
	}
	else
	{
		CONS_Printf("VR: runtime has no depth layer extension; reprojection stays rotation-only\n");
	}

	sGL.GenFramebuffers(1, &sEyeFbo);
	sGL.GenFramebuffers(1, &sBlitReadFbo);
	sGL.GenRenderbuffers(1, &sEyeDepthRB);
	sGL.BindRenderbuffer(GL_RENDERBUFFER, sEyeDepthRB);
	sGL.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)sEye[0].w, (GLsizei)sEye[0].h);
	sGL.BindRenderbuffer(GL_RENDERBUFFER, 0);

	XrReferenceSpaceCreateInfo vrci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	vrci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	vrci.poseInReferenceSpace.orientation.w = 1.0f;
	xrok(xrCreateReferenceSpace(sSession, &vrci, &sViewSpace), "xrCreateReferenceSpace(VIEW)");

	XrSwapchainCreateInfo oci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	oci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	oci.format = fmt;
	oci.sampleCount = 1;
	oci.width  = sHud.w = (uint32_t)sOverlayW;
	oci.height = sHud.h = (uint32_t)sOverlayH;
	oci.faceCount = 1;
	oci.arraySize = 1;
	oci.mipCount = 1;
	if (xrok(xrCreateSwapchain(sSession, &oci, &sHud.handle), "xrCreateSwapchain(panel)"))
	{
		uint32_t hn = 0;
		xrEnumerateSwapchainImages(sHud.handle, 0, &hn, NULL);
		sHud.images = (XrSwapchainImageOpenGLKHR*)calloc(hn, sizeof(XrSwapchainImageOpenGLKHR));
		for (uint32_t i = 0; i < hn; i++)
			sHud.images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
		xrEnumerateSwapchainImages(sHud.handle, hn, &hn, (XrSwapchainImageBaseHeader*)sHud.images);
		sHud.imgCount = hn;
		sGL.GenFramebuffers(1, &sOverlayFbo);
		sGL.GenRenderbuffers(1, &sOverlayDepthRB);
		sGL.BindRenderbuffer(GL_RENDERBUFFER, sOverlayDepthRB);
		sGL.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, sOverlayW, sOverlayH);
		sGL.BindRenderbuffer(GL_RENDERBUFFER, 0);
	}

	// Motion controllers: action set + suggested bindings + attach. Failure
	// here is non-fatal (VR still renders, just without controller input).
	vr_input_create();

	// Boot can retry after a transient failure, so guard the registration.
	// Every other knob is a cv_vr_* cvar now (registered with the rest of the
	// game's cvars, edited in the VR Options menu, saved to config); only the
	// recenter action stays a command.
	static bool sCommandsAdded = false;
	if (!sCommandsAdded)
	{
		COM_AddCommand("vr_recenter", Command_VrRecenter_f);
		COM_AddCommand("vr_dumpeyes", Command_VrDumpEyes_f);
		sCommandsAdded = true;
	}

	CONS_Printf("VR: OpenXR ready; waiting for session to start.\n");
}

// -vrfake: stand up the eye loop with no OpenXR at all. The eye pass, the
// mono target, the culling, the matrices and the dump all run the shipped
// code; only the runtime calls (wait/locate/swapchain/submit) are replaced by
// synthesized values. A stereo path that can only be exercised with a headset
// awake is a path that ships unverified - this seam is how it gets measured
// on a desk.
static bool sFake = false;

static void vr_boot_fake(void)
{
	if (!sGlReady)
	{
#if defined(_WIN32)
		if (!gladLoadGLContext(&sGL, (GLADloadfunc)SDL_GL_GetProcAddress))
		{
			CONS_Printf("VR: failed to load GL functions\n");
			return;
		}
#endif
		sGlReady = true;
	}
	sGL.GenFramebuffers(1, &sEyeFbo);
	sGL.GenFramebuffers(1, &sBlitReadFbo);

	// The real headset's per-eye target, so the fake pass pays the real cost.
	sViewCount = 2;
	sEye[0].w = sEye[1].w = 3072;
	sEye[0].h = sEye[1].h = 3264;

	static bool sCommandsAdded = false;
	if (!sCommandsAdded)
	{
		COM_AddCommand("vr_recenter", Command_VrRecenter_f);
		COM_AddCommand("vr_dumpeyes", Command_VrDumpEyes_f);
		sCommandsAdded = true;
	}

	sFake = true;
	sRunning = true;
	SDL_GL_SetSwapInterval(0);
	CONS_Printf("VR: fake stereo session up (%ux%u per eye, no headset).\n",
		sEye[0].w, sEye[0].h);
}

static void vr_poll_events(void)
{
	XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
	while (xrPollEvent(sInstance, &ev) == XR_SUCCESS)
	{
		if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED)
		{
			// The runtime rebound the controllers (woke up, changed device,
			// SteamVR remap). Log what each hand is bound to now.
			vr_log_active_profiles();
			sProfilesLogged = true;
		}
		else if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			const XrEventDataSessionStateChanged* e = (const XrEventDataSessionStateChanged*)&ev;
			sState = e->state;
			if (e->state == XR_SESSION_STATE_FOCUSED && !sProfilesLogged)
			{
				// First focus: the earliest point the bindings are resolved.
				vr_log_active_profiles();
				sProfilesLogged = true;
			}
			if (e->state == XR_SESSION_STATE_READY)
			{
				XrSessionBeginInfo sbi = { XR_TYPE_SESSION_BEGIN_INFO };
				sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if (xrok(xrBeginSession(sSession, &sbi), "xrBeginSession"))
				{
					sRunning = true;
					// xrWaitFrame paces the loop at the headset refresh from
					// now on; the desktop swap must not also block on the
					// monitor's vblank or the two fight and judder.
					SDL_GL_SetSwapInterval(0);
					CONS_Printf("VR: session running.\n");
				}
			}
			else if (e->state == XR_SESSION_STATE_STOPPING)
			{
				xrEndSession(sSession);
				sRunning = false;
				CONS_Printf("VR: session stopped.\n");
			}
		}
		else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
		{
			// The runtime just recentered LOCAL space - the player pressed the
			// recenter on their headset/controller, or their dashboard did it.
			// The app is never focused when that happens, so no binding could
			// catch it; the event is the only handle. Re-anchor the game's
			// forward to the new pose so recenter "brings the screen to my
			// gaze," same as the vr_recenter command. Handled in ANY state.
			const XrEventDataReferenceSpaceChangePending* e =
				(const XrEventDataReferenceSpaceChangePending*)&ev;
			if (e->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
			{
				vr_recenter();
				CONS_Printf("VR: runtime recenter received.\n");
			}
		}
		ev.type = XR_TYPE_EVENT_DATA_BUFFER;
	}
}

extern "C" void vr_begin_frame(void)
{
	if (!sRequested || sBootFailed)
		return;

	// Both D_Display (before the stereo eye loop) and I_FinishUpdate (covers
	// the panel path, including wipe loops that never run D_Display) call
	// this; whichever runs first begins the XR frame and the other is a no-op,
	// so there is exactly one xrWaitFrame per submitted frame.
	if (sFrameBegun)
		return;

	// Lazy boot on the first frame (the GL context is current here). Failure
	// is permanent except for the transient headset-unavailable case, which
	// retries on a slow cadence for a while before giving up.
	if (sFakeRequested)
	{
		if (!sFake)
		{
			vr_boot_fake();
			if (!sFake)
			{
				sBootFailed = true;
				CONS_Printf("VR: fake session startup failed; continuing without VR.\n");
				return;
			}
		}
	}
	else if (sSession == XR_NULL_HANDLE)
	{
		if (sBootRetryFrames > 0)
		{
			sBootRetryFrames--;
			return;
		}
		sBootTransient = false;
		vr_boot();
		if (sSession == XR_NULL_HANDLE)
		{
			if (sBootTransient && ++sBootRetries < kBootRetryMax)
			{
				sBootRetryFrames = kBootRetryInterval;
			}
			else
			{
				sBootFailed = true;
				CONS_Printf("VR: OpenXR startup failed; continuing without VR.\n");
			}
			return;
		}
	}

	if (!sFake)
		vr_poll_events();

	sViewsValid = false;
	sHudReady = false;
	sHudOverlay = false;
	sHudMenuUp = false;
	sPanelMode = false;
	sEyesSubmitted = 0;
	sDepthThisFrame[0] = sDepthThisFrame[1] = false;
	sMatricesValid = false;
	if (!sRunning)
		return;

	// Wall clock between one frame's start and the next, so the buckets below
	// can be weighed against the whole and whatever is missing can be named.
	{
		static precise_t sPrevFrameStamp = 0;
		const precise_t nowStamp = I_GetPreciseTime();
		if (sPrevFrameStamp != 0)
			sPaceTotalSecs += vr_secs_since(sPrevFrameStamp);
		sPrevFrameStamp = nowStamp;
	}

	const precise_t waitStamp = I_GetPreciseTime();
	sFrameState.type = XR_TYPE_FRAME_STATE;
	sFrameState.next = NULL;
	if (sFake)
	{
		// No compositor to wait on: the loop runs uncapped (ideal for cost
		// measurement) and the display time is a wall-clock stamp so the
		// pacing window still rolls over every ten seconds.
		sFrameState.shouldRender = XR_TRUE;
		sFrameState.predictedDisplayPeriod = 8333333; // pretend 120 Hz
		const double per = (double)I_GetPrecisePrecision();
		sFrameState.predictedDisplayTime = (per > 0.0)
			? (XrTime)((double)I_GetPreciseTime() * (1e9 / per)) : 1;
	}
	else
	{
		XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
		if (!xrok(xrWaitFrame(sSession, &fwi, &sFrameState), "xrWaitFrame"))
			return;
	}
	sFrameWaitSecs = vr_secs_since(waitStamp);

	// Frame pacing, measured rather than guessed. When the game cannot finish a
	// frame in time the runtime hands the headset the PREVIOUS pair again,
	// warped to the new head pose - and a warp has no idea how far away
	// anything was, so it slides near things by the wrong amount and the eyes
	// stop agreeing about where they are. That reads as swimming or cross-eye,
	// and it is loudest exactly where the view turns fastest (the race-start
	// fly-in) and on the closest objects. The runtime tells us it happened:
	// each predicted display time should be one refresh after the last, so
	// anything longer is a frame the compositor had to invent. One line every
	// ten seconds, so any future report arrives with the number attached.
	{
		static XrTime sLastDisplayTime = 0;
		static XrTime sPaceWindowStart = 0;
		static int sPaceFrames = 0, sPaceMissed = 0;

		// Roll last frame's per-eye stamps into the window and reset them.
		sPaceWorldSecs  += sEyeWorldSecs;
		sPacePostSecs   += sEyePostSecs;
		sPaceWaitSecs   += sFrameWaitSecs;
		sPaceSubmitSecs += sFrameSubmitSecs;
		sEyeWorldSecs = sEyePostSecs = 0.0;
		sFrameWaitSecs = sFrameSubmitSecs = 0.0;

		const XrTime now = sFrameState.predictedDisplayTime;
		const XrTime period = sFrameState.predictedDisplayPeriod > 0
			? sFrameState.predictedDisplayPeriod : 0;

		// Only frames we actually draw count. An idle or sleeping headset asks
		// for none, and the loop then spins at the refresh rate with nothing in
		// it - which would read as a flawless hundred and twenty and mean
		// nothing at all.
		if (!sFrameState.shouldRender)
		{
			sLastDisplayTime = 0;
			sPaceWindowStart = 0;
			sPaceFrames = sPaceMissed = 0;
		}
		else if (sLastDisplayTime != 0 && period > 0)
		{
			sPaceFrames++;
			// Half a period of slack so ordinary jitter isn't counted as a miss.
			if (now - sLastDisplayTime > period + period / 2)
				sPaceMissed++;
		}
		sLastDisplayTime = now;
		if (sPaceWindowStart == 0)
			sPaceWindowStart = now;

		if (period > 0 && now - sPaceWindowStart >= (XrTime)10000000000LL && sPaceFrames > 0)
		{
			// Split the frame into the three things it can be spent on, so a
			// slow frame says WHICH part was slow instead of leaving it to be
			// guessed at. "world" is the game drawing the level twice, "post"
			// is our own resolve/effect/copy per eye, and the rest is
			// everything outside the eye loop plus the wait for the headset.
			const double seconds = (double)(now - sPaceWindowStart) / 1e9;
			const double n = (double)sPaceFrames;
			const double ms = 1000.0 / n;
			// "rest" is the flat game around the eye loop: its tic, its 2D
			// pass, and the desktop mirror. It is derived rather than stamped
			// so nothing can hide in the gap between buckets.
			const double rest = 1000.0 * (sPaceTotalSecs - sPaceWorldSecs - sPacePostSecs
				- sPaceWaitSecs - sPaceSubmitSecs) / n;
			CONS_Printf("VR: %.0f fps app / %.0f Hz headset, %.0f%% reprojected"
				" (world %.1f, rest %.1f, submit %.1f, post %.1f, wait %.1f ms)\n",
				n / seconds,
				1e9 / (double)period,
				100.0 * (double)sPaceMissed / n,
				ms * sPaceWorldSecs,
				rest < 0.0 ? 0.0 : rest,
				ms * sPaceSubmitSecs,
				ms * sPacePostSecs,
				ms * sPaceWaitSecs);
			sPaceWindowStart = now;
			sPaceFrames = sPaceMissed = 0;
			sPaceWorldSecs = sPacePostSecs = 0.0;
			sPaceWaitSecs = sPaceSubmitSecs = sPaceTotalSecs = 0.0;
		}
	}

	if (!sFake)
	{
		XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
		if (!xrok(xrBeginFrame(sSession, &fbi), "xrBeginFrame"))
			return;
	}
	sFrameBegun = true;

	// Fresh controller state for this frame (and the rumble re-arm burst).
	vr_input_sync();

	// Chase the first-person drama angles once per headset frame. The step is
	// in SECONDS so the same wreck feels the same on a 72, 90 or 120 Hz
	// headset - predictedDisplayPeriod is the runtime's own answer for that.
	{
		const XrTime period = sFrameState.predictedDisplayPeriod;
		float dt = (period > 0) ? (float)period * 1e-9f : (1.0f / 90.0f);
		if (dt < 0.001f) dt = 0.001f; // a hitch must not become a lurch
		if (dt > 0.100f) dt = 0.100f;
		vr_step_drama(dt);
	}

	if (sFrameState.shouldRender)
	{
		bool located = false;
		if (sFake)
		{
			// A Quest-class rig at rest: asymmetric frusta, 64 mm IPD, head
			// 1.4 m up looking straight down -Z. Close enough to what VDXR
			// serves that the geometry measured here is the geometry shipped.
			for (int i = 0; i < 2; i++)
			{
				memset(&sViews[i], 0, sizeof(sViews[i]));
				sViews[i].type = XR_TYPE_VIEW;
				sViews[i].fov.angleUp   =  0.785398f; //  45 deg
				sViews[i].fov.angleDown = -0.837758f; // -48 deg
				// -vrfakepitch <deg> tilts the head (negative = looking DOWN).
				// A level stand-in cannot see the ground at your own feet,
				// which is where a player looks when judging ground art.
				sViews[i].pose.orientation.x = sinf(sFakePitchRad * 0.5f);
				sViews[i].pose.orientation.w = cosf(sFakePitchRad * 0.5f);
				sViews[i].pose.position.y = 1.4f;
			}
			sViews[0].fov.angleLeft  = -0.907571f; // -52 deg
			sViews[0].fov.angleRight =  0.785398f; //  45 deg
			sViews[1].fov.angleLeft  = -0.785398f; // mirrored for the right eye
			sViews[1].fov.angleRight =  0.907571f;
			sViews[0].pose.position.x = -0.032f;
			sViews[1].pose.position.x =  0.032f;
			located = true;
		}
		else
		{
			XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
			vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			vli.displayTime = sFrameState.predictedDisplayTime;
			vli.space = sLocalSpace;
			XrViewState vs = { XR_TYPE_VIEW_STATE };
			sViews[0].type = XR_TYPE_VIEW;
			sViews[1].type = XR_TYPE_VIEW;
			uint32_t got = 0;
			located = XR_SUCCEEDED(xrLocateViews(sSession, &vli, &vs, 2, &got, sViews)) && got == 2;
		}
		if (located)
		{
			sViewsValid = true;
			// Fallback declaration, in case the matrices bail out before they
			// build (head warmup): the located pose is the honest answer then,
			// because nothing has been drawn from anything else yet. The real
			// one is written inside vr_update_eye_matrices, from the pose the
			// eye is actually placed at.
			sRenderPose[0] = sViews[0].pose;
			sRenderPose[1] = sViews[1].pose;
			sRenderFov[0]  = sViews[0].fov;
			sRenderFov[1]  = sViews[1].fov;
			vr_update_eye_matrices();

			// The armed eye dump counts renderable stereo frames; when it
			// reaches zero this frame's two eyes get captured in vr_end_eye.
			if (sEyeDumpArm > 0 && --sEyeDumpArm == 0)
				sEyeDumpThisFrame = true;
		}
	}
}

// Acquire + bind the panel swapchain image as the active GL render target for
// direct rendering into the panel. The blit path (vr_submit_panel_texture) is
// what theater mode uses; this is here for callers that want to draw into the
// panel themselves.
extern "C" bool vr_begin_panel(void)
{
	sPanelMode = true;
	if (!sRunning || !sFrameBegun || !sFrameState.shouldRender)
		return false;
	if (sOverlayFbo == 0 || sHud.handle == XR_NULL_HANDLE)
		return false;
	uint32_t idx = 0;
	XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if (!XR_SUCCEEDED(xrAcquireSwapchainImage(sHud.handle, &ai, &idx)))
		return false;
	XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	swi.timeout = XR_INFINITE_DURATION;
	xrWaitSwapchainImage(sHud.handle, &swi);

	sGL.BindFramebuffer(GL_FRAMEBUFFER, sOverlayFbo);
	sGL.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sHud.images[idx].image, 0);
	sGL.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sOverlayDepthRB);
	sGL.Viewport(0, 0, sOverlayW, sOverlayH);
	sGL.Disable(GL_SCISSOR_TEST);
	sGL.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	sGL.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	return true;
}

extern "C" void vr_end_panel(void)
{
	sGL.BindFramebuffer(GL_FRAMEBUFFER, 0);
	XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(sHud.handle, &ri);
	sHudReady = true;
	sPanelSrcW = sOverlayW;
	sPanelSrcH = sOverlayH;
}

extern "C" void vr_set_panel_mode(bool on)
{
	sPanelMode = on;
}

// ---- the Screen Effect on the eye image --------------------------------------
// The Video "Screen Effect" presets are shaders that live in the game data;
// the RHI runs them when it blits the finished frame to the monitor, and the
// flat 2D layer gets them the same way in VR. The stereo world never passes
// through the RHI at all - it renders into a VR-owned FBO and goes straight
// into the swapchain - so the eyes saw none of it. This runs the SAME
// fragment lump on that blit, so the headset shows the picture the player
// picked instead of an untouched frame.
//
// Only the fragment side is shared. The vertex shader is written here against
// the fixed-function built-ins, which lets the quad go up through the plain
// client arrays the legacy renderer already uses - no generic attributes, so
// nothing can collide with the aliased attribute 0 the renderer draws with.
// ---- stereo calibration overlay -------------------------------------------
//
// Lines drawn straight into the eye buffers at EXACT angular positions
// computed from the runtime's own per-eye fov, bypassing the entire game
// renderer. This exists to answer one question no amount of content debugging
// can: does this pipeline, from swapchain to eyeball, display the same
// direction at the same place in both eyes?
//
//   FULL-HEIGHT lines sit at identical view angles in both eyes = infinity.
//     Looking far, they must fuse into single lines. If they double, the
//     defect is in submission or the runtime, and no game content is involved.
//   UPPER-HALF lines carry the disparity of a surface 2 m away.
//   LOWER-HALF lines carry the disparity of a surface 10 m away.
//
// Toggled by the vr_calib cvar; never saved.
static bool   sCalib = false;
static GLuint sCalibProgram = 0;
static bool   sCalibTried = false;

static const char* kCalibFragSource =
	"#version 120\n"
	"void main() { gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0); }\n";

extern "C" void vr_set_calib(bool on)
{
	sCalib = on;
}

static const char* kFxVertexSource =
	"#version 120\n"
	"varying vec4 v_color;\n"
	"varying vec2 v_texcoord0;\n"
	"void main()\n"
	"{\n"
	"	gl_Position = gl_Vertex;\n" // already in clip space
	"	v_color = vec4(1.0);\n"
	"	v_texcoord0 = gl_MultiTexCoord0.xy;\n"
	"}\n";

// One compiled program per cv_scr_effect value. Index 0 (Nearest) is never
// filled - that mode is the plain framebuffer blit below.
typedef struct
{
	GLuint program;
	GLint  uSamplerSize;
	GLint  uOutputSize;
	GLint  uEffectFade;
	GLint  sSampler0;
	GLint  sSampler1;
	bool   attempted; // a failed compile is not retried every frame
} VrFxProgram;

// The CRT strength dial, kept as strength (1 = full effect); the shader takes
// the inverse as a fade so an unset uniform still means the full effect.
static float sFxStrength = 1.0f;

#define VR_FX_COUNT 5
static VrFxProgram sFx[VR_FX_COUNT];
static GLuint      sFxDotTex = 0; // the CRT shaders' 12x4 shadow mask
static int         sScreenEffect = 0; // live cv_scr_effect
static bool        sWorldEffect  = true; // cv_vr_worldeffect

static const char* vr_fx_lump_name(int effect)
{
	switch (effect)
	{
	case 1: return "sharpbilinear";
	case 2: return "crt";
	case 3: return "crtsharp";
	case 4: return "softblit";
	default: return NULL;
	}
}

static GLuint vr_fx_compile(GLenum type, const char* const* sources, int count, const char* what)
{
	GLuint shader = sGL.CreateShader(type);
	if (shader == 0)
		return 0;
	sGL.ShaderSource(shader, count, sources, NULL);
	sGL.CompileShader(shader);

	GLint ok = GL_FALSE;
	sGL.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok != GL_TRUE)
	{
		char log[1024];
		GLsizei len = 0;
		sGL.GetShaderInfoLog(shader, (GLsizei)sizeof(log) - 1, &len, log);
		log[(len > 0 && len < (GLsizei)sizeof(log)) ? len : 0] = '\0';
		CONS_Printf("VR: screen effect %s failed to compile: %s\n", what, log);
		sGL.DeleteShader(shader);
		return 0;
	}
	return shader;
}

// Build the program for an effect, reading the fragment sources from the same
// glsllist the GL2 backend uses so both renderers stay one look.
static bool vr_fx_build(int effect)
{
	VrFxProgram* fx = &sFx[effect];
	if (fx->attempted)
		return fx->program != 0;
	fx->attempted = true;

	const char* name = vr_fx_lump_name(effect);
	if (name == NULL)
		return false;

	srb2::Vector<srb2::String> frag_sources;
	try
	{
		srb2::String list = srb2::format("rhi_glsllist_{}_fragment.txt", name);
		frag_sources = srb2::rhi::read_glsllist_sources(list.c_str());
	}
	catch (const std::exception& ex)
	{
		CONS_Printf("VR: screen effect '%s' unavailable: %s\n", name, ex.what());
		return false;
	}

	// Same preamble the GL2 backend compiles with, so the version-compat
	// macros in the shared lib lump resolve to the legacy GL spelling.
	srb2::Vector<const char*> frag_array;
	frag_array.push_back("#version 120\n");
	for (auto& source : frag_sources)
		frag_array.push_back(source.c_str());

	GLuint vert = vr_fx_compile(GL_VERTEX_SHADER, &kFxVertexSource, 1, "vertex shader");
	if (vert == 0)
		return false;
	GLuint frag = vr_fx_compile(
		GL_FRAGMENT_SHADER,
		frag_array.data(),
		(int)frag_array.size(),
		name
	);
	if (frag == 0)
	{
		sGL.DeleteShader(vert);
		return false;
	}

	GLuint prog = sGL.CreateProgram();
	sGL.AttachShader(prog, vert);
	sGL.AttachShader(prog, frag);
	sGL.LinkProgram(prog);
	sGL.DeleteShader(vert);
	sGL.DeleteShader(frag);

	GLint linked = GL_FALSE;
	sGL.GetProgramiv(prog, GL_LINK_STATUS, &linked);
	if (linked != GL_TRUE)
	{
		char log[1024];
		GLsizei len = 0;
		sGL.GetProgramInfoLog(prog, (GLsizei)sizeof(log) - 1, &len, log);
		log[(len > 0 && len < (GLsizei)sizeof(log)) ? len : 0] = '\0';
		CONS_Printf("VR: screen effect '%s' failed to link: %s\n", name, log);
		sGL.DeleteProgram(prog);
		return false;
	}

	fx->program      = prog;
	fx->uSamplerSize = sGL.GetUniformLocation(prog, "u_sampler0_size");
	fx->uOutputSize  = sGL.GetUniformLocation(prog, "u_output_size");
	fx->uEffectFade  = sGL.GetUniformLocation(prog, "u_effect_fade");
	fx->sSampler0    = sGL.GetUniformLocation(prog, "s_sampler0");
	fx->sSampler1    = sGL.GetUniformLocation(prog, "s_sampler1");
	// Say what actually resolved. A uniform that does not link comes back as
	// -1, the push below is skipped, and the shader reads 0 - which for the
	// fade lane means "flat, everything on", so every bit of gating silently
	// does nothing and looks exactly like a shader that was never edited.
	CONS_Printf("VR: screen effect '%s' ready for the eye pass (fade lane %s, size %s)\n",
		name,
		(fx->uEffectFade  >= 0) ? "live" : "MISSING - gating is inert",
		(fx->uSamplerSize >= 0) ? "live" : "MISSING");
	if (effect == 2)
	{
		// SalCRT's look comes from a nine-direction blur that costs dozens of
		// texture reads per pixel. That is affordable on a monitor and brutal
		// across two eye buffers at headset resolution.
		CONS_Printf("VR: SalCRT is very expensive per eye - use SalCRT Sharp, or turn Screen Effect in 3D off, if frames drop.\n");
	}
	return true;
}

// The CRT presets read their shadow mask from the second sampler, repeated
// across the frame. Same tile the monitor's pass uses.
static GLuint vr_fx_dot_texture(void)
{
	if (sFxDotTex != 0)
		return sFxDotTex;

	GLint prevTex = 0;
	sGL.GetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
	sGL.GenTextures(1, &sFxDotTex);
	sGL.BindTexture(GL_TEXTURE_2D, sFxDotTex);
	sGL.TexImage2D(
		GL_TEXTURE_2D, 0, GL_RGBA8,
		srb2::hwr2::kCrtDotPatternWidth, srb2::hwr2::kCrtDotPatternHeight, 0,
		GL_RGBA, GL_UNSIGNED_BYTE, srb2::hwr2::kCrtDotPattern
	);
	sGL.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	sGL.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	sGL.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	sGL.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	sGL.BindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
	return sFxDotTex;
}

// Draw the source texture over the currently bound draw framebuffer through
// an effect program. Every piece of GL state this touches is put back: the
// legacy renderer caches its bound texture and program and would render with
// whatever we left behind.
static void vr_fx_draw(int effect, unsigned int glTex, int srcW, int srcH, int dstW, int dstH)
{
	VrFxProgram* fx = &sFx[effect];

	static const GLfloat kQuadPos[8] = { -1.f, -1.f,  1.f, -1.f,  -1.f, 1.f,  1.f, 1.f };
	static const GLfloat kQuadUv[8]  = {  0.f,  0.f,  1.f,  0.f,   0.f, 1.f,  1.f, 1.f };

	GLint prevProgram = 0;
	GLint prevTex0 = 0;
	GLint prevTex1 = 0;
	GLint prevViewport[4] = { 0, 0, 0, 0 };
	sGL.GetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
	sGL.GetIntegerv(GL_VIEWPORT, prevViewport);
	sGL.GetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex0);
	sGL.ActiveTexture(GL_TEXTURE1);
	sGL.GetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex1);
	sGL.ActiveTexture(GL_TEXTURE0);
	const GLboolean prevDepth = sGL.IsEnabled(GL_DEPTH_TEST);
	const GLboolean prevBlend = sGL.IsEnabled(GL_BLEND);
	const GLboolean prevCull  = sGL.IsEnabled(GL_CULL_FACE);
	// ALPHA TEST is the one that matters here.
	// The legacy renderer leaves it ENABLED after drawing the world, with a
	// discard rule aimed at cutout sprites. This quad then inherits it, and
	// the fragment it outputs carries the EYE BUFFER'S alpha - which the drop
	// shadow has just driven down, because PF_ReverseSubtract subtracts on the
	// alpha channel as well as on colour. So the shadow's own pixels failed
	// the test and were DISCARDED, and a discarded pixel does not go black,
	// it leaves whatever the swapchain image already held: a piece of an older
	// frame, in the shape of the shadow, moving. That is the smear. It only
	// ever appeared with an effect selected, because the plain blit path is a
	// framebuffer copy and copies do not run the alpha test.
	const GLboolean prevAlpha   = sGL.IsEnabled(GL_ALPHA_TEST);
	const GLboolean prevScissor = sGL.IsEnabled(GL_SCISSOR_TEST);
	const GLboolean prevStencil = sGL.IsEnabled(GL_STENCIL_TEST);

	sGL.Disable(GL_DEPTH_TEST);
	sGL.Disable(GL_BLEND);
	sGL.Disable(GL_CULL_FACE);
	// A full-screen resolve must write every pixel it covers. Anything that
	// can reject a fragment has to be off, not merely assumed off.
	sGL.Disable(GL_ALPHA_TEST);
	sGL.Disable(GL_SCISSOR_TEST);
	sGL.Disable(GL_STENCIL_TEST);
	sGL.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	sGL.Viewport(0, 0, dstW, dstH);

	sGL.UseProgram(fx->program);
	if (fx->uSamplerSize >= 0)
		sGL.Uniform2f(fx->uSamplerSize, (GLfloat)srcW, (GLfloat)srcH);
	if (fx->uOutputSize >= 0)
		sGL.Uniform2f(fx->uOutputSize, (GLfloat)dstW, (GLfloat)dstH);
	if (fx->uEffectFade >= 0)
	{
		// .y is a LEVEL: 0 keeps everything, 1 drops the vertical aperture
		// grille (stripes land on different world content in each eye and
		// shimmer as rivalry), 2 drops the scanlines and the texel snap too.
		//
		// The eyes ask for 1, not 2. Level 2 was built while the scanlines
		// were the prime suspect for the shadow smear; they were not - the
		// smear was the alpha test above discarding the shadow's own pixels.
		// With that fixed the scanlines and the snap are welcome to run, so
		// the headset gets the CRT it is supposed to have. Level 2 stays in
		// the tree because the terms it gates ARE screen-locked, and it is
		// the switch to reach for if they ever need to go.
		sGL.Uniform2f(fx->uEffectFade, 1.0f - sFxStrength, 1.0f);
	}
	if (fx->sSampler0 >= 0)
		sGL.Uniform1i(fx->sSampler0, 0);
	if (fx->sSampler1 >= 0)
	{
		sGL.Uniform1i(fx->sSampler1, 1);
		sGL.ActiveTexture(GL_TEXTURE1);
		sGL.BindTexture(GL_TEXTURE_2D, vr_fx_dot_texture());
	}

	sGL.ActiveTexture(GL_TEXTURE0);
	sGL.BindTexture(GL_TEXTURE_2D, (GLuint)glTex);

	sGL.EnableClientState(GL_VERTEX_ARRAY);
	sGL.EnableClientState(GL_TEXTURE_COORD_ARRAY);
	sGL.VertexPointer(2, GL_FLOAT, 0, kQuadPos);
	sGL.TexCoordPointer(2, GL_FLOAT, 0, kQuadUv);
	sGL.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	sGL.DisableClientState(GL_TEXTURE_COORD_ARRAY);
	sGL.DisableClientState(GL_VERTEX_ARRAY);

	sGL.ActiveTexture(GL_TEXTURE1);
	sGL.BindTexture(GL_TEXTURE_2D, (GLuint)prevTex1);
	sGL.ActiveTexture(GL_TEXTURE0);
	sGL.BindTexture(GL_TEXTURE_2D, (GLuint)prevTex0);
	sGL.UseProgram((GLuint)prevProgram);
	sGL.Viewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
	if (prevDepth)   sGL.Enable(GL_DEPTH_TEST);
	if (prevBlend)   sGL.Enable(GL_BLEND);
	if (prevCull)    sGL.Enable(GL_CULL_FACE);
	if (prevAlpha)   sGL.Enable(GL_ALPHA_TEST);
	if (prevScissor) sGL.Enable(GL_SCISSOR_TEST);
	if (prevStencil) sGL.Enable(GL_STENCIL_TEST);
}

// Draw the calibration lines into the mono target for this eye. Placement is
// pure arithmetic on the runtime's own fov: a view direction with horizontal
// tangent t lands at NDC x = 2*(t - tanL)/(tanR - tanL) - 1. Nothing from the
// game's transform chain is involved, which is the point.
static void vr_calib_draw(int eye)
{
#if defined(__ANDROID__)
	// Same reason the screen effects are off here: this overlay draws through
	// the GLSL 1.20 vertex shader above, which the GLES driver this file talks
	// to cannot compile. The calibration grid is a desk tool anyway.
	(void)eye;
	return;
#else
	if (!sCalibTried)
	{
		sCalibTried = true;
		GLuint vert = vr_fx_compile(GL_VERTEX_SHADER, &kFxVertexSource, 1, "calib vertex");
		GLuint frag = vr_fx_compile(GL_FRAGMENT_SHADER, &kCalibFragSource, 1, "calib fragment");
		if (vert && frag)
		{
			GLuint prog = sGL.CreateProgram();
			sGL.AttachShader(prog, vert);
			sGL.AttachShader(prog, frag);
			sGL.LinkProgram(prog);
			GLint ok = GL_FALSE;
			sGL.GetProgramiv(prog, GL_LINK_STATUS, &ok);
			if (ok == GL_TRUE)
				sCalibProgram = prog;
			else
				sGL.DeleteProgram(prog);
		}
		if (vert) sGL.DeleteShader(vert);
		if (frag) sGL.DeleteShader(frag);
		CONS_Printf(sCalibProgram
			? "VR: calibration overlay ready - full-height lines are infinity and MUST fuse; upper half is 2 m, lower half is 10 m\n"
			: "VR: calibration overlay failed to build\n");
	}
	if (sCalibProgram == 0 || eye < 0 || eye > 1)
		return;

	const XrFovf& fov = sViews[eye].fov;
	const float tanL = tanf(fov.angleLeft), tanR = tanf(fov.angleRight);
	const float span = tanR - tanL;
	if (span <= 0.0f)
		return;

	// Real half-IPD in metres from the located poses; disparity for a surface
	// at d metres is a per-eye horizontal tangent offset of halfIpd/d, nasal:
	// positive (rightward) for the left eye, negative for the right.
	const float halfIpd = 0.5f * fabsf(sViews[1].pose.position.x - sViews[0].pose.position.x);
	const float nasal = (eye == 0) ? 1.0f : -1.0f;

	GLint prevProgram = 0, prevFbo = 0, prevViewport[4] = {0,0,0,0};
	sGL.GetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
	sGL.GetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
	sGL.GetIntegerv(GL_VIEWPORT, prevViewport);
	const GLboolean prevDepth = sGL.IsEnabled(GL_DEPTH_TEST);
	const GLboolean prevBlend = sGL.IsEnabled(GL_BLEND);
	const GLboolean prevCull  = sGL.IsEnabled(GL_CULL_FACE);
	const GLboolean prevAlpha = sGL.IsEnabled(GL_ALPHA_TEST);
	const GLboolean prevScis  = sGL.IsEnabled(GL_SCISSOR_TEST);

	// A full-screen diagnostic must be able to write every pixel it covers.
	sGL.BindFramebuffer(GL_FRAMEBUFFER, sMonoFbo);
	sGL.Viewport(0, 0, sMonoW, sMonoH);
	sGL.Disable(GL_DEPTH_TEST);
	sGL.Disable(GL_BLEND);
	sGL.Disable(GL_CULL_FACE);
	sGL.Disable(GL_ALPHA_TEST);
	sGL.Disable(GL_SCISSOR_TEST);
	sGL.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	sGL.UseProgram(sCalibProgram);

	const float tans[5] = { -0.35f, -0.175f, 0.0f, 0.175f, 0.35f };
	const float halfW = 3.0f / (float)sMonoW; // ~3 px line half-width
	float quads[15 * 8];
	int n = 0;

	for (int set = 0; set < 3; set++)
	{
		// set 0: infinity, full height. set 1: 2 m, upper half.
		// set 2: 10 m, lower half.
		const float depthOff = (set == 0) ? 0.0f
			: nasal * (halfIpd / ((set == 1) ? 2.0f : 10.0f));
		const float y0 = (set == 1) ? 0.15f : -0.85f;
		const float y1 = (set == 2) ? -0.15f : 0.85f;
		for (int i = 0; i < 5; i++)
		{
			const float t = tans[i] + depthOff;
			const float x = 2.0f * (t - tanL) / span - 1.0f;
			if (x < -1.0f || x > 1.0f)
				continue;
			float* q = &quads[n * 8];
			q[0] = x - halfW; q[1] = y0;
			q[2] = x + halfW; q[3] = y0;
			q[4] = x - halfW; q[5] = y1;
			q[6] = x + halfW; q[7] = y1;
			n++;
		}
	}

	sGL.EnableClientState(GL_VERTEX_ARRAY);
	for (int i = 0; i < n; i++)
	{
		sGL.VertexPointer(2, GL_FLOAT, 0, &quads[i * 8]);
		sGL.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
	sGL.DisableClientState(GL_VERTEX_ARRAY);

	sGL.UseProgram((GLuint)prevProgram);
	sGL.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
	sGL.Viewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
	if (prevDepth) sGL.Enable(GL_DEPTH_TEST);
	if (prevBlend) sGL.Enable(GL_BLEND);
	if (prevCull)  sGL.Enable(GL_CULL_FACE);
	if (prevAlpha) sGL.Enable(GL_ALPHA_TEST);
	if (prevScis)  sGL.Enable(GL_SCISSOR_TEST);
#endif // !__ANDROID__
}

// The effect the eye pass should run this frame: the player's Screen Effect,
// unless they've turned it off for the 3D view.
static int vr_eye_effect(void)
{
#if defined(__ANDROID__)
	// NO SCREEN EFFECT IN THE HEADSET BUILD. The effects are the game's own
	// GLSL 1.20, and this file talks to the GLES driver directly rather than
	// through the translation layer the renderer uses, so those sources would
	// fail to compile here. Zero means the plain blit, which is what the eye
	// wants anyway; the flat build on this device is untouched.
	return 0;
#else
	if (!sWorldEffect || sScreenEffect <= 0 || sScreenEffect >= VR_FX_COUNT)
		return 0;
	return vr_fx_build(sScreenEffect) ? sScreenEffect : 0;
#endif
}

extern "C" void vr_set_screen_effect(int effect)
{
	sScreenEffect = effect;
}

extern "C" void vr_set_world_effect(bool on)
{
	sWorldEffect = on;
}

extern "C" void vr_set_effect_strength(float frac)
{
	if (frac < 0.0f)
		frac = 0.0f;
	if (frac > 1.0f)
		frac = 1.0f;
	sFxStrength = frac;
}

extern "C" float vr_billboard_yaw_delta(void)
{
	float delta = 0.0f;

	if (!sMatricesValid)
		return 0.0f;

	// First person spins the world so the KART's heading is forward; the
	// billboards must follow or every card faces the chase camera's plane
	// instead of the driver's eyes. Same term the compose itself uses.
	if (vr_fp_compose_active() && sCockpitValid && sGameViewValid)
		delta += wrap_pi((sCockpitYaw + sDramaYaw) - sGameViewYaw);

	// Free-look: the head's own yaw relative to the recentered rest pose,
	// positive leftward in both the XR and game conventions. One shared
	// value (the left eye's orientation stands in for the pair) keeps both
	// eyes' quads identical, which is the whole point.
	{
		const float qy = sViews[0].pose.orientation.y;
		const float qw = sViews[0].pose.orientation.w;
		const float qn = sqrtf(qy * qy + qw * qw);
		if (qn > 1e-6f)
			delta += wrap_pi(2.0f * atan2f(qy / qn, qw / qn) - sRecenterYaw);
	}

	return wrap_pi(delta);
}

// Copy the eye pass's depth buffer into the depth swapchain image the
// compositor reads. The source is always the single-sample mono FBO (the MSAA
// resolve in vr_end_eye has already brought depth down to it), so this is one
// straight blit; NEAREST because a filtered depth sample is a depth that no
// surface ever had, and because GL refuses anything else for depth.
static bool vr_blit_depth_into(VrSwapchain* sc)
{
	if (sc->handle == XR_NULL_HANDLE || sEyeDepthFbo == 0 || sMonoFbo == 0)
		return false;

	uint32_t idx = 0;
	XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if (!XR_SUCCEEDED(xrAcquireSwapchainImage(sc->handle, &ai, &idx)))
		return false;
	XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	swi.timeout = XR_INFINITE_DURATION;
	xrWaitSwapchainImage(sc->handle, &swi);

	GLint prevRead = 0, prevDraw = 0;
	sGL.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
	sGL.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);

	sGL.BindFramebuffer(GL_DRAW_FRAMEBUFFER, sEyeDepthFbo);
	sGL.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
		sc->images[idx].image, 0);
	// A framebuffer with no colour attachment is only complete once its draw
	// buffer is told so. This acts on the DRAW binding, which is the FBO
	// above. (Its read-side twin would act on whatever framebuffer happens to
	// be bound for READING instead - the multisample target, mid-frame - and
	// quietly leave that FBO unable to serve the next frame's colour resolve.)
	sGL.DrawBuffer(GL_NONE);

	const GLenum status = sGL.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
	bool ok = (status == GL_FRAMEBUFFER_COMPLETE);
	if (ok)
	{
		sGL.BindFramebuffer(GL_READ_FRAMEBUFFER, sMonoFbo);
		// Depth writes are masked off by whatever the renderer left behind;
		// a blit into a depth attachment obeys that mask.
		sGL.DepthMask(GL_TRUE);
		sGL.Disable(GL_SCISSOR_TEST);
		while (sGL.GetError() != GL_NO_ERROR) {} // start from a clean slate
		sGL.BlitFramebuffer(0, 0, sMonoW, sMonoH, 0, 0, (GLint)sc->w, (GLint)sc->h,
			GL_DEPTH_BUFFER_BIT, GL_NEAREST);
		const GLenum err = sGL.GetError();
		if (err != GL_NO_ERROR)
			ok = false;

		// Say once, out loud, whether the depth actually reached the runtime.
		// Everything above this line can be checked on a desk; this cannot -
		// it needs a headset awake enough to ask for frames - so rather than
		// ship it silent, the first eye that tries it reports the verdict.
		static bool sSaid = false;
		if (!sSaid)
		{
			sSaid = true;
			if (ok)
				CONS_Printf("VR: depth layer live (%dx%d -> %ux%u per eye)\n",
					sMonoW, sMonoH, sc->w, sc->h);
			else
				CONS_Printf("VR: depth blit failed (GL 0x%x) - falling back to rotation-only reprojection\n",
					(unsigned)err);
		}
	}
	else
	{
		static bool sSaidFbo = false;
		if (!sSaidFbo)
		{
			sSaidFbo = true;
			CONS_Printf("VR: depth target incomplete (0x%x) - falling back to rotation-only reprojection\n",
				(unsigned)status);
		}
	}

	sGL.BindFramebuffer(GL_DRAW_FRAMEBUFFER, sEyeDepthFbo);
	sGL.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
	sGL.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevRead);
	sGL.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevDraw);

	XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(sc->handle, &ri);

	// A failure here is structural, not a hiccup - stand the layer down rather
	// than pay for an acquire and a blit every frame forever to fail again.
	if (!ok)
		sDepthLayerReady = false;

	return ok;
}

// Blit a rendered texture (the game's finished frame, sized srcW x srcH) into
// the given swapchain image. The source is the RHI backbuffer color texture,
// which is right-side-up in GL convention (bottom row = image bottom - the
// screenshot pass flips rows when saving, which is the tell), and OpenXR GL
// swapchain images share the GL lower-left origin, so this is a straight blit
// with no vertical flip. sEyeFbo is reused as a scratch DRAW fbo.
//
// effect is a cv_scr_effect value, or 0 for the plain framebuffer blit. Only
// the eye path passes one: the panel and the HUD overlay get their effect
// from the RHI before they ever reach here.
static bool vr_blit_into(VrSwapchain* sc, unsigned int glTex, int srcW, int srcH, int effect)
{
	// Fake session: there is no swapchain to receive the image, but the
	// callers' accounting (eyes submitted, HUD readiness) must run as shipped.
	if (sFake)
		return true;
	if (sc->handle == XR_NULL_HANDLE)
		return false;
	uint32_t idx = 0;
	XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if (!XR_SUCCEEDED(xrAcquireSwapchainImage(sc->handle, &ai, &idx)))
		return false;
	XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	swi.timeout = XR_INFINITE_DURATION;
	xrWaitSwapchainImage(sc->handle, &swi);

	sGL.BindFramebuffer(GL_DRAW_FRAMEBUFFER, sEyeFbo);
	sGL.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc->images[idx].image, 0);
	sGL.Disable(GL_SCISSOR_TEST);

	if (effect != 0)
	{
		// Shaded blit: the effect samples the frame itself, so no read fbo.
		vr_fx_draw(effect, glTex, srcW, srcH, (int)sc->w, (int)sc->h);
	}
	else
	{
		sGL.BindFramebuffer(GL_READ_FRAMEBUFFER, sBlitReadFbo);
		sGL.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)glTex, 0);
		GLenum filt = (srcW == (int)sc->w && srcH == (int)sc->h) ? GL_NEAREST : GL_LINEAR;
		sGL.BlitFramebuffer(0, 0, srcW, srcH, 0, 0, (int)sc->w, (int)sc->h, GL_COLOR_BUFFER_BIT, filt);
		sGL.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	}
	sGL.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(sc->handle, &ri);
	return true;
}

// ---- per-eye render pass -----------------------------------------------------

extern "C" bool vr_stereo_active(void)
{
	return sInEyePass;
}

extern "C" bool vr_eye_msaa_active(void)
{
	return sMonoMsFbo != 0;
}

extern "C" int vr_current_eye(void)
{
	return sCurrentEye;
}

extern "C" const float* vr_gl_eye_projection(int eye)
{
	return (eye >= 0 && eye < 2) ? &sEyeProj[eye][0][0] : NULL;
}

extern "C" const float* vr_gl_eye_view(int eye)
{
	if (eye < 0 || eye >= 2)
		return NULL;
	// The rotation-only variant keeps the sky at infinity: no head
	// translation (and no IPD) ever reaches the dome.
	return sMonoSky ? &sEyeViewMono[eye][0][0] : &sEyeView[eye][0][0];
}

extern "C" float vr_units_per_meter(void)
{
	return sUnitsPerMeter;
}

extern "C" float vr_cull_fov_deg(void)
{
	return sCullFovDeg;
}

extern "C" void vr_mono_sky(bool in_sky)
{
	sMonoSky = in_sky;
}

// Drop the multisampled half of the mono target (separate so an MSAA
// fallback can shed just these and keep the single-sample side).
static void vr_destroy_mono_msaa(void)
{
	if (sMonoMsFbo)     { sGL.DeleteFramebuffers(1, &sMonoMsFbo); sMonoMsFbo = 0; }
	if (sMonoMsColorRB) { sGL.DeleteRenderbuffers(1, &sMonoMsColorRB); sMonoMsColorRB = 0; }
	if (sMonoMsDepthRB) { sGL.DeleteRenderbuffers(1, &sMonoMsDepthRB); sMonoMsDepthRB = 0; }
}

// Allocate multisampled renderbuffer storage through whichever entry point
// the driver exposes (GL 3.0 core name or the EXT suffix). False when neither
// is loaded or the allocation errored (unsupported sample count, no memory).
static bool vr_ms_storage(GLuint rb, int samples, GLenum fmt, int w, int h)
{
	sGL.BindRenderbuffer(GL_RENDERBUFFER, rb);
	while (sGL.GetError() != GL_NO_ERROR) {} // drain stale errors first
	if (sGL.RenderbufferStorageMultisample)
		sGL.RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, fmt, w, h);
	else if (sGL.RenderbufferStorageMultisampleEXT)
		sGL.RenderbufferStorageMultisampleEXT(GL_RENDERBUFFER, samples, fmt, w, h);
	else
	{
		sGL.BindRenderbuffer(GL_RENDERBUFFER, 0);
		return false;
	}
	bool ok = (sGL.GetError() == GL_NO_ERROR);
	sGL.BindRenderbuffer(GL_RENDERBUFFER, 0);
	return ok;
}

// (Re)build the mono render target: single-sample color texture (the blit
// source), plus a multisampled render FBO when vr_msaa asks for one. On any
// MSAA failure the pass silently renders single-sample instead.
static bool vr_ensure_mono_target(int w, int h)
{
	if (sMonoFbo != 0 && sMonoW == w && sMonoH == h && sMonoBuiltMsaa == sMsaaRequest)
		return true;
	if (w <= 0 || h <= 0)
		return false;

	if (sMonoFbo == 0)
		sGL.GenFramebuffers(1, &sMonoFbo);
	if (sMonoColorTex == 0)
		sGL.GenTextures(1, &sMonoColorTex);
	if (sMonoDepthRB == 0)
		sGL.GenRenderbuffers(1, &sMonoDepthRB);

	// Multisampled side first, so the single-sample FBO below knows whether
	// it still needs the depth attachment.
	vr_destroy_mono_msaa();
	int samples = sMsaaRequest;
	if (samples > 0)
	{
		GLint maxSamples = 0;
		sGL.GetIntegerv(GL_MAX_SAMPLES, &maxSamples);
		while (sGL.GetError() != GL_NO_ERROR) {} // enum absent pre-FBO: don't leave the error queued
		if (samples > maxSamples)
			samples = (int)maxSamples;
	}
	if (samples > 0)
	{
		sGL.GenRenderbuffers(1, &sMonoMsColorRB);
		sGL.GenRenderbuffers(1, &sMonoMsDepthRB);
		bool msOk = vr_ms_storage(sMonoMsColorRB, samples, GL_RGBA8, w, h)
			&& vr_ms_storage(sMonoMsDepthRB, samples, sDepthGlFormat, w, h);
		if (msOk)
		{
			GLint prevFb = 0;
			sGL.GetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFb);
			sGL.GenFramebuffers(1, &sMonoMsFbo);
			sGL.BindFramebuffer(GL_FRAMEBUFFER, sMonoMsFbo);
			sGL.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, sMonoMsColorRB);
			sGL.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sMonoMsDepthRB);
			msOk = (sGL.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
			sGL.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFb);
		}
		if (!msOk)
		{
			vr_destroy_mono_msaa();
			samples = 0;
		}
	}

	// The legacy renderer caches its bound texture name, so put the binding
	// back exactly as found.
	GLint prevTex = 0;
	sGL.GetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
	sGL.BindTexture(GL_TEXTURE_2D, sMonoColorTex);
	sGL.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	sGL.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	sGL.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	sGL.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	sGL.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	sGL.BindTexture(GL_TEXTURE_2D, (GLuint)prevTex);

	// Single-sample depth. Without MSAA the pass renders straight into this
	// FBO and needs it to depth-test at all; with MSAA it is the resolve
	// target, and it still wants depth so the resolve can carry the depth
	// buffer down to one sample for the compositor's depth layer. (Both
	// attachments here are single-sample, so they agree either way.)
	sGL.BindRenderbuffer(GL_RENDERBUFFER, sMonoDepthRB);
	sGL.RenderbufferStorage(GL_RENDERBUFFER, sDepthGlFormat, w, h);
	sGL.BindRenderbuffer(GL_RENDERBUFFER, 0);

	GLint prevFb = 0;
	sGL.GetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFb);
	sGL.BindFramebuffer(GL_FRAMEBUFFER, sMonoFbo);
	sGL.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sMonoColorTex, 0);
	sGL.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sMonoDepthRB);
	GLenum status = sGL.CheckFramebufferStatus(GL_FRAMEBUFFER);
	sGL.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFb);

	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		CONS_Printf("VR: mono render target incomplete (0x%x)\n", status);
		vr_destroy_mono_msaa();
		return false;
	}

	sMonoW = w;
	sMonoH = h;
	sMonoBuiltMsaa = sMsaaRequest;

	// One line per outcome change, so the console shows what's actually on.
	static int sMsaaLoggedSamples = -1;
	if (samples != sMsaaLoggedSamples)
	{
		if (samples > 0)
			CONS_Printf("VR: MSAA %dx\n", samples);
		else if (sMsaaRequest > 0)
			CONS_Printf("VR: MSAA unavailable\n");
		else
			CONS_Printf("VR: MSAA off\n");
		sMsaaLoggedSamples = samples;
	}
	return true;
}

// Raster size for the GL backend's viewport during an eye pass.
extern "C" int vr_eye_raster_width(void)
{
	return sMonoW;
}

extern "C" int vr_eye_raster_height(void)
{
	return sMonoH;
}

extern "C" bool vr_begin_eye(int eye)
{
	if (!sRunning || !sFrameBegun || !sFrameState.shouldRender || !sViewsValid || !sMatricesValid)
		return false;
	if (eye < 0 || eye >= (int)sViewCount)
		return false;
	if (!sFake && sEye[eye].handle == XR_NULL_HANDLE)
		return false;

	// Render at the eye swapchain size times the live render scale. The
	// swapchain already carries the boot fallback ladder, so 1.0 here means a
	// 1:1 blit at whatever resolution actually got created.
	int rw = (int)((float)sEye[eye].w * sRenderScale + 0.5f);
	int rh = (int)((float)sEye[eye].h * sRenderScale + 0.5f);
	if (rw < 64) rw = 64;
	if (rh < 64) rh = 64;
	if (!vr_ensure_mono_target(rw, rh))
		return false;

	// Bind over the RHI's framebuffer for the duration of this eye's legacy GL
	// pass; vr_end_eye puts it back before the pass is popped. The renderer's
	// own ResetRenderState resyncs the GL state pokes below.
	sGL.GetIntegerv(GL_FRAMEBUFFER_BINDING, &sSavedDrawFb);
	sGL.BindFramebuffer(GL_FRAMEBUFFER, sMonoMsFbo ? sMonoMsFbo : sMonoFbo);
	sGL.Viewport(0, 0, sMonoW, sMonoH);
	sGL.Disable(GL_SCISSOR_TEST);
	sGL.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	sGL.DepthMask(GL_TRUE);
	sGL.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	sGL.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	sCurrentEye = eye;
	sInEyePass = true;
	sMonoSky = false;
	sEyeStamp = I_GetPreciseTime();
	return true;
}

extern "C" void vr_end_eye(int eye)
{
	if (!sInEyePass || eye != sCurrentEye)
		return;
	sInEyePass = false;
	sMonoSky = false;

	// Everything up to here was the game drawing the level; everything after
	// is ours.
	sEyeWorldSecs += vr_secs_since(sEyeStamp);
	sEyeStamp = I_GetPreciseTime();

	const bool wantDepth = sDepthLayerReady && sDepthLayerWanted
		&& eye >= 0 && eye < 2 && sEyeDepth[eye].handle != XR_NULL_HANDLE;

	// Resolve the multisampled render into the single-sample color texture.
	// Sample counts differ across this blit, so src and dst rects must match
	// exactly (any scaling here is a GL error); the eye blit below scales.
	if (sMonoMsFbo)
	{
		sGL.BindFramebuffer(GL_READ_FRAMEBUFFER, sMonoMsFbo);
		sGL.BindFramebuffer(GL_DRAW_FRAMEBUFFER, sMonoFbo);
		sGL.BlitFramebuffer(0, 0, sMonoW, sMonoH, 0, 0, sMonoW, sMonoH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		if (wantDepth)
		{
			// Depth has to come down to one sample too, and a resolve is not
			// meaningful for it - averaging two depths invents a surface that
			// was never there - so this takes sample zero, which is what
			// NEAREST does here by definition.
			sGL.BlitFramebuffer(0, 0, sMonoW, sMonoH, 0, 0, sMonoW, sMonoH, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
		}
	}

	// Calibration lines go in BEFORE the dump capture, so a headless dump can
	// machine-verify their pixel placement through the same code path the
	// headset sees.
	if (sCalib)
		vr_calib_draw(eye);

	if (sEyeDumpThisFrame)
		vr_dump_capture_eye(eye);

	if (wantDepth)
		sDepthThisFrame[eye] = vr_blit_depth_into(&sEyeDepth[eye]);

	// Blit the finished mono frame into the eye's swapchain image
	// (acquire/blit/release; 1:1 at full render scale, linear otherwise) and
	// record the layer view for vr_submit. This is the one place the eye's
	// picture is resampled, so it's where the Screen Effect belongs.
	if (vr_blit_into(&sEye[eye], sMonoColorTex, sMonoW, sMonoH, vr_eye_effect()))
	{
		XrCompositionLayerProjectionView* pv = &sProjViews[eye];
		memset(pv, 0, sizeof(*pv));
		pv->type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		pv->pose = sRenderPose[eye];
		pv->fov  = sRenderFov[eye];
		pv->subImage.swapchain = sEye[eye].handle;
		pv->subImage.imageRect.extent.width  = (int32_t)sEye[eye].w;
		pv->subImage.imageRect.extent.height = (int32_t)sEye[eye].h;

		if (sDepthThisFrame[eye])
		{
			XrCompositionLayerDepthInfoKHR* di = &sDepthInfo[eye];
			memset(di, 0, sizeof(*di));
			di->type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR;
			di->subImage.swapchain = sEyeDepth[eye].handle;
			di->subImage.imageRect.extent.width  = (int32_t)sEyeDepth[eye].w;
			di->subImage.imageRect.extent.height = (int32_t)sEyeDepth[eye].h;
			// glDepthRange is left at its default, so the buffer runs 0..1.
			di->minDepth = 0.0f;
			di->maxDepth = 1.0f;
			// The clip planes are GAME UNITS; the runtime wants METRES, and
			// the divisor is whichever scale this frame's eye matrices used -
			// the diorama runs on its own much larger one. Getting these
			// wrong does not fail loudly, it just tells the compositor the
			// world is the wrong size and makes the warp worse than doing
			// nothing, which is why they are derived here and not typed in.
			const float upm = (sViewMode == VR_VIEW_DIORAMA) ? sDioramaScale : sUnitsPerMeter;
			di->nearZ = kEyeZNear / upm;
			di->farZ  = kEyeZFar  / upm;
			pv->next = di;
		}

		sEyesSubmitted++;

		static bool sStereoLogged = false;
		if (!sStereoLogged && sEyesSubmitted == 2)
		{
			CONS_Printf("VR: stereo eye loop active (%dx%d game -> %ux%u per eye)\n",
				sMonoW, sMonoH, sEye[0].w, sEye[0].h);
			sStereoLogged = true;
		}
	}

	// Leave the still-open RHI pass on the framebuffer it began with.
	sGL.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)sSavedDrawFb);

	sEyePostSecs += vr_secs_since(sEyeStamp);
}

// Mirror the last rendered eye into the desktop window's backbuffer,
// center-cropped to the window's aspect (the eye frustum is much taller than
// a monitor; letterboxing it would waste most of the screen). Called by
// I_FinishUpdate inside the window render pass on stereo frames, before the
// 2D backbuffer blit alpha-blends the HUD on top - without this the window
// only ever shows that (mostly transparent) 2D frame over black. The mono
// target still holds the last eye resolved by vr_end_eye; nothing else
// writes it between there and present.
extern "C" bool vr_blit_mirror(int dstW, int dstH)
{
	if (!sRunning || sEyesSubmitted == 0 || sMonoColorTex == 0 || sBlitReadFbo == 0)
		return false;
	if (dstW <= 0 || dstH <= 0 || sMonoW <= 0 || sMonoH <= 0)
		return false;

	// Largest centered source rect with the window's aspect.
	int srcW = sMonoW;
	int srcH = (int)((float)srcW * (float)dstH / (float)dstW + 0.5f);
	if (srcH > sMonoH)
	{
		srcH = sMonoH;
		srcW = (int)((float)srcH * (float)dstW / (float)dstH + 0.5f);
		if (srcW > sMonoW)
			srcW = sMonoW;
	}
	const int sx = (sMonoW - srcW) / 2;
	const int sy = (sMonoH - srcH) / 2;

	sGL.BindFramebuffer(GL_READ_FRAMEBUFFER, sBlitReadFbo);
	sGL.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sMonoColorTex, 0);
	sGL.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	sGL.Disable(GL_SCISSOR_TEST);
	sGL.BlitFramebuffer(sx, sy, sx + srcW, sy + srcH, 0, 0, dstW, dstH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	sGL.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	return true;
}

extern "C" bool vr_submit_panel_texture(unsigned int glTex, int w, int h)
{
	if (!sRunning || !sFrameBegun || !sFrameState.shouldRender)
		return false;
	// Stereo frames own the projection layer; putting the flat frame's panel
	// on top of the eyes as well would double the world.
	if (sEyesSubmitted > 0)
		return false;
	if (sEyeFbo == 0 || glTex == 0 || w <= 0 || h <= 0)
		return false;
	if (!vr_blit_into(&sHud, glTex, w, h, 0))
		return false;
	sHudReady = true;
	sPanelMode = true;
	sPanelSrcW = w;
	sPanelSrcH = h;
	return true;
}

extern "C" bool vr_frame_has_eyes(void)
{
	return sEyesSubmitted > 0;
}

// Stereo frames' counterpart to the panel: the finished 2D frame - the
// game's backbuffer, which VR frames draw on transparent black so only the
// HUD/menus carry alpha - goes into the HUD swapchain, and vr_submit
// composites it as an alpha-blended quad above the projection layer.
extern "C" bool vr_submit_hud_texture(unsigned int glTex, int w, int h)
{
	if (!sRunning || !sFrameBegun || !sFrameState.shouldRender)
		return false;
	if (sEyesSubmitted == 0)
		return false;
	if (sEyeFbo == 0 || glTex == 0 || w <= 0 || h <= 0)
		return false;
	if (!vr_blit_into(&sHud, glTex, w, h, 0))
		return false;
	sHudReady = true;
	sHudOverlay = true;
	sPanelSrcW = w;
	sPanelSrcH = h;
	return true;
}

extern "C" void vr_submit(void)
{
	if (!sRunning || !sFrameBegun)
		return;
	sFrameBegun = false;

	// Fake session: nothing to hand the compositor.
	if (sFake)
		return;

	const precise_t submitStamp = I_GetPreciseTime();
	struct SubmitStamp
	{
		precise_t at;
		~SubmitStamp() { sFrameSubmitSecs += vr_secs_since(at); }
	} submitStampGuard { submitStamp };

	XrCompositionLayerProjection proj  = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerQuad       panel = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad       hud   = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	const XrCompositionLayerBaseHeader* layers[2];
	uint32_t layerCount = 0;

	// The 3D projection layer only goes up when BOTH eyes were rendered this
	// frame; a half-submitted stereo pair is worse than none. Theater mode
	// never renders eyes, so this stays out of the frame there.
	const bool haveEyes = (sFrameState.shouldRender && sViewsValid && sViewCount == 2 && sEyesSubmitted == 2);

	// World anchor for the menu/panel quads: capture the head pose when a UI
	// quad (re)appears, or when the anchor was invalidated (recenter, session
	// restart) while one is up. Until a capture lands the quads fall back to
	// the head-locked path below.
	const bool uiQuadUp = (haveEyes && sHudReady && sHudOverlay && (sHudMenuUp || sHudWorld))
		|| (!haveEyes && sHudReady && sPanelMode);
	// The menu opening is its own reappearance and needs its own capture. With
	// the HUD parked in room space a quad is ALREADY up every gameplay frame,
	// so opening the menu was never a rising edge here and the menu screen
	// inherited whatever pose the race HUD was anchored at - possibly minutes
	// and a lot of head movement ago. A few degrees of stale yaw is enough to
	// push the far edge of a wide menu outside what an eye can see.
	if (uiQuadUp && (!sUiWasUp || !sUiAnchorValid || (sHudMenuUp && !sMenuWasUp)))
		vr_capture_ui_anchor();
	sUiWasUp = uiQuadUp;
	sMenuWasUp = sHudMenuUp;

	if (haveEyes)
	{
		proj.layerFlags = 0;
		proj.space = sLocalSpace;
		proj.viewCount = 2;
		proj.views = sProjViews;
		layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&proj;
	}

	if (haveEyes && sHudReady && sHudOverlay && sViewSpace != XR_NULL_HANDLE)
	{
		// Gameplay HUD: the 2D frame on an alpha-blended quad layered above
		// the projection layer, head-locked in VIEW space. The texture is
		// premultiplied by construction - the 2D pass blends onto transparent
		// black writing (One, 1-SrcAlpha) coverage into the alpha channel -
		// which is exactly what SOURCE_ALPHA layer blending consumes.
		float srcW = (sPanelSrcW > 0) ? (float)sPanelSrcW : (float)sHud.w;
		float srcH = (sPanelSrcH > 0) ? (float)sPanelSrcH : (float)sHud.h;
		hud.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		hud.space = sViewSpace;
		hud.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		hud.subImage.swapchain = sHud.handle;
		hud.subImage.imageRect.offset.x = 0;
		hud.subImage.imageRect.offset.y = 0;
		hud.subImage.imageRect.extent.width  = (int32_t)sHud.w;
		hud.subImage.imageRect.extent.height = (int32_t)sHud.h;
		if (sHudMenuUp && sUiAnchorValid)
		{
			// Menu up: a fixed-size screen that needs to READ, parked in the
			// world at its OWN distance knob (the race HUD's slider no longer
			// drags the menu around with it, and vice versa).
			hud.space = sLocalSpace;
			hud.pose = vr_ui_anchor_pose(sMenuQuadDist);
			hud.size.width = kMenuQuadWidth;
		}
		else if (sHudWorld && sUiAnchorValid)
		{
			// Gameplay HUD in room space: parked at the anchor (captured
			// when stereo play started, refreshed by vr_recenter), so head
			// motion gives it parallax instead of it riding the face.
			hud.space = sLocalSpace;
			hud.pose = vr_ui_anchor_pose(sHudDist);
			hud.size.width = sHudScale * kHudRefWidth;
		}
		else
		{
			// Head-locked straight ahead (vr_hudlock head, or no anchor
			// capture has landed yet).
			hud.pose.orientation.w = 1.0f;
			hud.pose.position.z = -sHudDist;
			hud.size.width = sHudScale * kHudRefWidth;
		}
		hud.size.height = hud.size.width * srcH / srcW;
		// Fit it to what both eyes can see. A quad that overruns the inner
		// edge of vision does not look oversized, it looks like the right-hand
		// column was cut off - which is exactly how it was reported.
		{
			const float fit = vr_ui_fit_scale(hud.size.width, hud.size.height,
				(sHudMenuUp && sUiAnchorValid) ? sMenuQuadDist : sHudDist);
			hud.size.width  *= fit;
			hud.size.height *= fit;
		}
		layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&hud;
	}
	else if (sHudReady && sPanelMode && sViewSpace != XR_NULL_HANDLE)
	{
		// Theater panel: an opaque virtual cinema screen, anchored in the
		// world where the player was looking when it appeared (title screen,
		// theater mode, intermissions) - vr_recenter brings it back to the
		// gaze. Head-locked only until the first anchor capture lands. The
		// quad is sized aspect-correct for the frame that was blitted in, so
		// the game isn't stretched to the 16:9 panel swapchain.
		float srcW = (sPanelSrcW > 0) ? (float)sPanelSrcW : (float)sHud.w;
		float srcH = (sPanelSrcH > 0) ? (float)sPanelSrcH : (float)sHud.h;
		panel.layerFlags = 0; // opaque
		panel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		panel.subImage.swapchain = sHud.handle;
		panel.subImage.imageRect.offset.x = 0;
		panel.subImage.imageRect.offset.y = 0;
		panel.subImage.imageRect.extent.width  = (int32_t)sHud.w;
		panel.subImage.imageRect.extent.height = (int32_t)sHud.h;
		if (sUiAnchorValid)
		{
			panel.space = sLocalSpace;
			panel.pose = vr_ui_anchor_pose(sMenuDist);
		}
		else
		{
			panel.space = sViewSpace;
			panel.pose.orientation.w = 1.0f;
			panel.pose.position.z = -sMenuDist;
		}
		panel.size.width  = sMenuSize;
		panel.size.height = sMenuSize * srcH / srcW;
		layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&panel;
	}

	XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
	fei.displayTime = sFrameState.predictedDisplayTime;
	fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	fei.layerCount = layerCount;
	fei.layers = layerCount ? layers : NULL;
	xrok(xrEndFrame(sSession, &fei), "xrEndFrame");
}

extern "C" void vr_shutdown(void)
{
	if (sGlReady)
	{
		if (sEyeFbo)         { sGL.DeleteFramebuffers(1, &sEyeFbo); sEyeFbo = 0; }
		if (sEyeDepthFbo)    { sGL.DeleteFramebuffers(1, &sEyeDepthFbo); sEyeDepthFbo = 0; }
		if (sBlitReadFbo)    { sGL.DeleteFramebuffers(1, &sBlitReadFbo); sBlitReadFbo = 0; }
		if (sEyeDepthRB)     { sGL.DeleteRenderbuffers(1, &sEyeDepthRB); sEyeDepthRB = 0; }
		if (sOverlayFbo)     { sGL.DeleteFramebuffers(1, &sOverlayFbo); sOverlayFbo = 0; }
		if (sOverlayDepthRB) { sGL.DeleteRenderbuffers(1, &sOverlayDepthRB); sOverlayDepthRB = 0; }
		if (sMonoFbo)        { sGL.DeleteFramebuffers(1, &sMonoFbo); sMonoFbo = 0; }
		if (sMonoColorTex)   { sGL.DeleteTextures(1, &sMonoColorTex); sMonoColorTex = 0; }
		if (sMonoDepthRB)    { sGL.DeleteRenderbuffers(1, &sMonoDepthRB); sMonoDepthRB = 0; }
		vr_destroy_mono_msaa();
	}
	sMonoW = sMonoH = 0;
	sMonoBuiltMsaa = -1;
	if (sHud.handle != XR_NULL_HANDLE) { xrDestroySwapchain(sHud.handle); sHud.handle = XR_NULL_HANDLE; }
	if (sHud.images) { free(sHud.images); sHud.images = NULL; }
	sHud.imgCount = 0;
	if (sViewSpace != XR_NULL_HANDLE) { xrDestroySpace(sViewSpace); sViewSpace = XR_NULL_HANDLE; }
	for (int e = 0; e < 2; e++)
	{
		if (sEye[e].handle != XR_NULL_HANDLE) { xrDestroySwapchain(sEye[e].handle); sEye[e].handle = XR_NULL_HANDLE; }
		if (sEye[e].images) { free(sEye[e].images); sEye[e].images = NULL; }
		sEye[e].imgCount = 0;
		if (sEyeDepth[e].handle != XR_NULL_HANDLE) { xrDestroySwapchain(sEyeDepth[e].handle); sEyeDepth[e].handle = XR_NULL_HANDLE; }
		if (sEyeDepth[e].images) { free(sEyeDepth[e].images); sEyeDepth[e].images = NULL; }
		sEyeDepth[e].imgCount = 0;
	}
	sDepthLayerReady = false;
	sDepthThisFrame[0] = sDepthThisFrame[1] = false;
	if (sLocalSpace != XR_NULL_HANDLE) { xrDestroySpace(sLocalSpace); sLocalSpace = XR_NULL_HANDLE; }
	// Destroying the action set destroys its actions with it.
	if (sActionSet != XR_NULL_HANDLE) { xrDestroyActionSet(sActionSet); sActionSet = XR_NULL_HANDLE; }
	sActMove = sActCam = XR_NULL_HANDLE;
	sActBtnA = sActBtnB = sActBtnX = sActBtnY = XR_NULL_HANDLE;
	sActMenuBtn = sActLStick = sActRStick = XR_NULL_HANDLE;
	sActLTrigger = sActRTrigger = sActLGrip = sActRGrip = XR_NULL_HANDLE;
	sActHaptic = XR_NULL_HANDLE;
	sHandPath[0] = sHandPath[1] = XR_NULL_PATH;
	sInputAttached = false;
	sProfilesLogged = false;
	sCtrlButtons = 0;
	memset(sCtrlStick, 0, sizeof(sCtrlStick));
	sRumbleAmp = 0.0f;
	sRumbleUntil = 0;
	if (sSession    != XR_NULL_HANDLE) { xrDestroySession(sSession);  sSession    = XR_NULL_HANDLE; }
	if (sInstance   != XR_NULL_HANDLE) { xrDestroyInstance(sInstance); sInstance   = XR_NULL_HANDLE; }
	sSystemId = XR_NULL_SYSTEM_ID;
	sRunning = false;
	sFrameBegun = false;
	sViewsValid = false;
	sHudReady = false;
	sHudOverlay = false;
	sPanelMode = false;
	sViewCount = 0;
	sMatricesValid = false;
	sInEyePass = false;
	sMonoSky = false;
	sHeadRestSet = false;
	sHeadWarmup = 0;
	sRecenterPending = false;
	// View mode itself survives a session restart; the per-frame feeds don't.
	sCockpitValid = false;
	sGameViewValid = false;
	sSkyParallax = 0.0f;
	sHudMenuUp = false;
	sUiAnchorValid = false;
	sUiWasUp = false;
	vr_drama_reset();
	sRaceFinished = false;
	sFpSwitchLock = false;
}

#else // _WIN32

// OpenXR is only wired up for Windows (WGL binding); everywhere else VR never
// activates and no headset is ever reported.

extern "C" bool vr_is_active(void)
{
	return false;
}

extern "C" bool vr_headset_present(void)
{
	return false;
}

extern "C" void vr_begin_frame(void)
{
}

extern "C" int vr_eye_count(void)
{
	return 0;
}

extern "C" int vr_eye_width(int eye)
{
	(void)eye;
	return 0;
}

extern "C" int vr_eye_height(int eye)
{
	(void)eye;
	return 0;
}

extern "C" bool vr_submit_panel_texture(unsigned int glTex, int w, int h)
{
	(void)glTex;
	(void)w;
	(void)h;
	return false;
}

extern "C" bool vr_submit_hud_texture(unsigned int glTex, int w, int h)
{
	(void)glTex;
	(void)w;
	(void)h;
	return false;
}

extern "C" bool vr_frame_has_eyes(void)
{
	return false;
}

extern "C" void vr_set_panel_mode(bool on)
{
	(void)on;
}

extern "C" void vr_recenter(void)
{
}

extern "C" void vr_set_world_scale(float unitsPerMeter)
{
	(void)unitsPerMeter;
}

extern "C" void vr_set_render_scale(float scale)
{
	(void)scale;
}

extern "C" void vr_set_msaa(int samples)
{
	(void)samples;
}

extern "C" void vr_set_stereo_strength(float frac)
{
	(void)frac;
}

extern "C" void vr_set_focus_distance(float units)
{
	(void)units;
}

extern "C" void vr_set_head_motion_scale(float frac)
{
	(void)frac;
}

extern "C" void vr_set_horizon_lock(bool on)
{
	(void)on;
}

extern "C" void vr_set_immersion(int level)
{
	(void)level;
}

extern "C" void vr_set_boost_kick(float amount)
{
	(void)amount;
}

extern "C" void vr_set_fp_eyeheight(float units)
{
	(void)units;
}

extern "C" void vr_set_fp_forward(float units)
{
	(void)units;
}

extern "C" void vr_set_tp_eyeheight(float meters)
{
	(void)meters;
}

extern "C" void vr_set_diorama_scale(float unitsPerMeter)
{
	(void)unitsPerMeter;
}

extern "C" void vr_set_diorama_dist(float meters)
{
	(void)meters;
}

extern "C" void vr_set_diorama_height(float meters)
{
	(void)meters;
}

extern "C" bool vr_diorama_eye_offset(float outOffset[3])
{
	(void)outOffset;
	return false;
}

extern "C" bool vr_eye_world_offset(float outOffset[3])
{
	(void)outOffset;
	return false;
}

extern "C" void vr_set_diorama_clearance(float frac)
{
	(void)frac;
}

extern "C" void vr_set_hud_scale(float frac)
{
	(void)frac;
}

extern "C" void vr_set_hud_dist(float meters)
{
	(void)meters;
}

extern "C" void vr_set_menu_dist(float meters)
{
	(void)meters;
}

extern "C" void vr_set_hud_world(bool on)
{
	(void)on;
}

extern "C" void vr_set_hud_menu(bool menuUp)
{
	(void)menuUp;
}

extern "C" bool vr_menu_dim_active(void)
{
	return false;
}

extern "C" bool vr_blit_mirror(int dstW, int dstH)
{
	(void)dstW;
	(void)dstH;
	return false;
}

extern "C" bool vr_begin_panel(void)
{
	return false;
}

extern "C" void vr_end_panel(void)
{
}

extern "C" void vr_submit(void)
{
}

extern "C" bool vr_stereo_active(void)
{
	return false;
}

extern "C" bool vr_eye_msaa_active(void)
{
	return false;
}

extern "C" int vr_current_eye(void)
{
	return 0;
}

extern "C" const float* vr_gl_eye_projection(int eye)
{
	(void)eye;
	return 0;
}

extern "C" const float* vr_gl_eye_view(int eye)
{
	(void)eye;
	return 0;
}

extern "C" float vr_units_per_meter(void)
{
	return 64.0f;
}

extern "C" float vr_cull_fov_deg(void)
{
	return 0.0f;
}

extern "C" bool vr_begin_eye(int eye)
{
	(void)eye;
	return false;
}

extern "C" void vr_end_eye(int eye)
{
	(void)eye;
}

extern "C" int vr_eye_raster_width(void)
{
	return 0;
}

extern "C" int vr_eye_raster_height(void)
{
	return 0;
}

extern "C" void vr_mono_sky(bool in_sky)
{
	(void)in_sky;
}

extern "C" int vr_get_view_mode(void)
{
	return VR_VIEW_THIRD_PERSON;
}

extern "C" void vr_set_view_mode(int mode)
{
	(void)mode;
}

extern "C" void vr_cycle_view_mode(void)
{
}

extern "C" void vr_set_fp_switch_locked(bool locked)
{
	(void)locked;
}

extern "C" void vr_set_cockpit_pose(const float posWorld[3], float yawRad, float pitchRad, float rollRad)
{
	(void)posWorld;
	(void)yawRad;
	(void)pitchRad;
	(void)rollRad;
}

extern "C" void vr_set_game_view(const float posWorld[3], float yawRad, float pitchRad, float rollRad, bool skyPass)
{
	(void)posWorld;
	(void)yawRad;
	(void)pitchRad;
	(void)rollRad;
	(void)skyPass;
}

extern "C" void vr_set_sky_parallax_scale(float scale)
{
	(void)scale;
}

extern "C" void vr_set_drama(int kind, short spinBinary, short rollBinary)
{
	(void)kind;
	(void)spinBinary;
	(void)rollBinary;
}

extern "C" void vr_set_action_cam(int dramatic, int finished)
{
	(void)dramatic;
	(void)finished;
}

extern "C" float vr_fp_eyeheight_units(void)
{
	return 0.0f;
}

extern "C" float vr_fp_forward_units(void)
{
	return 0.0f;
}

extern "C" bool vr_fp_hide_player(void)
{
	return false;
}

extern "C" bool vr_horizon_locked(void)
{
	return false;
}

extern "C" bool vr_controllers_active(void)
{
	return false;
}

extern "C" unsigned vr_controller_buttons(void)
{
	return 0;
}

extern "C" void vr_controller_stick(int hand, float out[2])
{
	(void)hand;
	out[0] = out[1] = 0.0f;
}

extern "C" void vr_controller_rumble(float strength, float seconds)
{
	(void)strength;
	(void)seconds;
}

extern "C" void vr_controller_rumble_stop(void)
{
}

extern "C" void vr_shutdown(void)
{
}

#endif // _WIN32

#endif // ENABLE_VR
