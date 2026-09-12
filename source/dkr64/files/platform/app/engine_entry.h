// engine_entry.h -- the C entry points and seams the app shell delegates to.
//
// This is the ONLY header shared between the C++ app shell and the C engine, so
// the two never need hand-synced duplicate declarations. mdkr64_headless_main()
// is the original main() body from platform/main_pc.c (renamed under -DMDKR_APP);
// the shell calls it verbatim for every non-interactive invocation so that path
// stays byte-identical to the pre-shell binary.
#ifndef MDKR64_ENGINE_ENTRY_H
#define MDKR64_ENGINE_ENTRY_H

/* Canonical C handoff/recovery seam. Keep these declarations in one header so
 * the C engine and C++ shell cannot drift. */
#include "../host_window.h"
#include "../modern_character_gpu_timing.h"
#include "../modern_character_semantics.h"
#include "../workshop_preview_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

// The engine's original entry point (platform/main_pc.c under MDKR_APP).
int mdkr64_headless_main(int argc, char **argv);

// --- Launcher -> game boot -------------------------------------------------
// DKR ADAPTATION: mgb64's MgbBootConfig carries level/difficulty/multiplayer/
// players/mp_stage -- GoldenEye's mission-select surface. DKR's engine has no
// equivalent CLI (platform/main_pc.c models no level or difficulty flags; the
// player picks a world in the game's own adventure hub), so those fields are
// deliberately absent rather than present-and-ignored. What the launcher CAN
// meaningfully choose is the ROM and the presentation mode, which is what this
// carries.
#define MDKR_BOOT_MAX_OVERRIDES 16

typedef enum {
    MDKR_CHARACTER_PREVIEW_NONE = 0,
    MDKR_CHARACTER_PREVIEW_SELECT,
    MDKR_CHARACTER_PREVIEW_CAR,
    MDKR_CHARACTER_PREVIEW_HOVERCRAFT,
    MDKR_CHARACTER_PREVIEW_PLANE,
} MdkrCharacterPreviewContext;

/* Vehicle review deliberately spans five fingerprinted course families. The
 * names describe the authoring pressure, while the game owns the exact level
 * mapping per vehicle. Character select has one authored room and therefore
 * admits BASELINE only. */
typedef enum {
    MDKR_CHARACTER_PREVIEW_SCENE_BASELINE = 0,
    MDKR_CHARACTER_PREVIEW_SCENE_DENSE,
    MDKR_CHARACTER_PREVIEW_SCENE_ALTERNATE,
    MDKR_CHARACTER_PREVIEW_SCENE_LOW_VISIBILITY,
    MDKR_CHARACTER_PREVIEW_SCENE_EFFECTS,
    MDKR_CHARACTER_PREVIEW_SCENE_COUNT,
} MdkrCharacterPreviewScene;

/* A pose inspector is a presentation-only exact-renderer request. LIVE keeps
 * ordinary game-driven animation and is the only mode eligible for durable
 * performance evidence. Every other value holds the chosen semantic at the
 * requested normalized phase without changing racer or vehicle logic. */
typedef enum {
    MDKR_CHARACTER_PREVIEW_POSE_LIVE = 0,
#define MDKR_CHARACTER_PREVIEW_POSE_ENUM(suffix, semantic, label) \
    MDKR_CHARACTER_PREVIEW_POSE_##suffix,
    MDKR_MODERN_CHARACTER_INSPECTION_SEMANTICS(
        MDKR_CHARACTER_PREVIEW_POSE_ENUM)
#undef MDKR_CHARACTER_PREVIEW_POSE_ENUM
    MDKR_CHARACTER_PREVIEW_POSE_COUNT,
} MdkrCharacterPreviewPose;

typedef enum {
    MDKR_CHARACTER_PREVIEW_MOTION_NONE = 0,
    MDKR_CHARACTER_PREVIEW_MOTION_AUTHORED,
    MDKR_CHARACTER_PREVIEW_MOTION_REVIEWED_REFERENCE,
    MDKR_CHARACTER_PREVIEW_MOTION_PACKAGE_FALLBACK,
    MDKR_CHARACTER_PREVIEW_MOTION_COUNT,
} MdkrCharacterPreviewMotionSource;

/* One-shot inspection captures are explicit render products. SCENE preserves
 * the ordinary composed gameplay frame. MODEL_ALPHA asks the modern-character
 * backend to replay only validated replacement draws into a transparent
 * target; it never hides world geometry in the visible frame. */
typedef enum {
    MDKR_CHARACTER_PREVIEW_CAPTURE_SCENE = 0,
    MDKR_CHARACTER_PREVIEW_CAPTURE_MODEL_ALPHA,
    MDKR_CHARACTER_PREVIEW_CAPTURE_COUNT,
} MdkrCharacterPreviewCaptureKind;

#define MDKR_CHARACTER_PREVIEW_CONTACTS 4u
#define MDKR_CHARACTER_PREVIEW_LANDMARKS 3u
#define MDKR_CHARACTER_PREVIEW_JOINTS 16u
#define MDKR_CHARACTER_PREVIEW_OCCLUDERS 3u
typedef enum MdkrCharacterPreviewLandmark {
    MDKR_CHARACTER_PREVIEW_LANDMARK_HIPS = 0,
    MDKR_CHARACTER_PREVIEW_LANDMARK_CHEST,
    MDKR_CHARACTER_PREVIEW_LANDMARK_HEAD,
} MdkrCharacterPreviewLandmark;
#define MDKR_CHARACTER_PREVIEW_PROJECTION_POINTS 10u
#define MDKR_CHARACTER_PREVIEW_PROJECTION_BOUNDS_POINTS 8u
#define MDKR_CHARACTER_PREVIEW_PROJECTION_ANCHOR_POINT 8u
#define MDKR_CHARACTER_PREVIEW_PROJECTION_FORWARD_POINT 9u

// Measured evidence returned by an exact Character Workshop session. Interval
// values describe displayed wall cadence after a 120-authored-tick warm-up.
// `gpu_timing` separately identifies exact timestamp scope and availability; a
// short session can legitimately return fewer than 60 wall intervals or
// pending asynchronous GPU readbacks.
typedef struct MdkrCharacterPreviewResult {
    unsigned version;
    int started;
    int warmup_complete;
    int realtime;
    MdkrCharacterPreviewContext context;
    int players;
    MdkrCharacterPreviewPose pose;
    unsigned pose_phase_milli;
    MdkrCharacterPreviewPose transition_from_pose;
    unsigned transition_from_phase_milli;
    unsigned long long warmup_ticks;
    unsigned long long interval_samples;
    unsigned long long displayed_frames;
    unsigned long long interval_p50_us;
    unsigned long long interval_p95_us;
    unsigned long long interval_p99_us;
    unsigned long long interval_mean_us;
    unsigned long long interval_max_us;
    unsigned long long tickwall_samples;
    unsigned long long tickwall_mean_ns;
    MdkrModernCharacterGpuTimingMetrics gpu_timing;
    unsigned long long replacement_draws;
    unsigned long long replacement_primitives;
    unsigned long long hidden_donor_batches;
    /* Comparison-only witness. A nonzero value proves qualified retail donor
     * character batches reached the ordinary composed scene while modern
     * replacement draws remained suppressed. */
    int donor_reference;
    unsigned long long donor_reference_batches;
    /* Exact custom commands prepared only to authenticate fit/camera
     * registration. Their pixels and performance counters remain suppressed. */
    unsigned long long reference_draws;
    unsigned long long reference_primitives;
    unsigned long long contact_solves;
    unsigned long long contact_error_mean_micrometres;
    unsigned long long contact_error_max_micrometres;
    /* Latest successful post-solve vehicle draw. Stable contact order is
     * left hand, right hand, left foot, right foot. Every point is in the
     * donor target frame and every valid bit covers one complete witness. */
    unsigned contact_witness_mask;
    long long contact_chain_root_micrometres[MDKR_CHARACTER_PREVIEW_CONTACTS][3];
    long long contact_bend_micrometres[MDKR_CHARACTER_PREVIEW_CONTACTS][3];
    long long contact_target_micrometres[MDKR_CHARACTER_PREVIEW_CONTACTS][3];
    long long contact_end_micrometres[MDKR_CHARACTER_PREVIEW_CONTACTS][3];
    unsigned long long contact_witness_error_micrometres
        [MDKR_CHARACTER_PREVIEW_CONTACTS];
    /* Latest successful replacement draw, expressed in its donor target
     * frame. Signed micrometres retain sub-millimetre fit evidence without
     * exposing host float representation across the C/C++ app boundary. */
    int fit_diagnostics_valid;
    long long fit_bounds_min_micrometres[3];
    long long fit_bounds_max_micrometres[3];
    long long fit_anchor_micrometres[3];
    int fit_forward_milli[3];
    /* Exact current-pose anatomy points in the donor target frame. Stable
     * order is hips, chest, head. Hips/chest require a reviewed humanoid map;
     * head may fall back to the required package socket. */
    unsigned fit_landmark_mask;
    long long fit_landmark_micrometres
        [MDKR_CHARACTER_PREVIEW_LANDMARKS][3];
    /* Exact current post-solve node-local angular distance from each reviewed
     * humanoid role's compiled bind rotation. Millidegrees avoid carrying
     * host floats across the game/app boundary. This is advisory evidence,
     * never a runtime clamp or an anatomical verdict. */
    unsigned joint_excursion_mask;
    unsigned joint_excursion_millidegrees
        [MDKR_CHARACTER_PREVIEW_JOINTS];
    /* Exact source-v5 runtime witnesses from the same latest replacement
     * draw. Clamp bits use stable humanoid-role order. Secondary values are
     * bounded authoring diagnostics, not a quality score. */
    unsigned constraint_clamped_mask;
    unsigned secondary_chain_count;
    unsigned secondary_joint_count;
    unsigned secondary_active_joint_count;
    unsigned secondary_max_deflection_millidegrees;
    unsigned long long secondary_discontinuity_resets;
    /* Ordinary scene-camera projection of the calibrated volume and anatomy
     * points. Unlike the isolated model capture below, this preserves the
     * gameplay camera and viewport. It proves framing, not depth visibility or
     * vehicle-shell intersection. */
    int camera_projection_valid;
    unsigned camera_projection_width;
    unsigned camera_projection_height;
    int camera_projection_viewport[4];
    int camera_projection_scissor[4];
    unsigned camera_projection_primitive_draws;
    int camera_bounds_pixel_milli[4]; /* left, top, right, bottom */
    unsigned camera_bounds_clip_flags;
    int camera_landmark_pixel_milli
        [MDKR_CHARACTER_PREVIEW_LANDMARKS][2];
    int camera_landmark_depth_millionths
        [MDKR_CHARACTER_PREVIEW_LANDMARKS];
    unsigned camera_landmark_clip_flags
        [MDKR_CHARACTER_PREVIEW_LANDMARKS];
    /* One exact posed-frame surface witness against fingerprint-qualified,
     * retained vehicle-body batches. This detects triangle contact/intersection;
     * it is not a depth-buffer, containment, attachment, or penetration-depth
     * measurement. SELECT contexts deliberately leave it unavailable. */
    int vehicle_surface_valid;
    unsigned vehicle_shell_triangles_submitted;
    unsigned vehicle_shell_triangles_tested;
    unsigned character_surface_triangles_submitted;
    unsigned character_surface_triangles_tested;
    unsigned vehicle_surface_crossing_triangles;
    unsigned vehicle_surface_crossing_pairs;
    long long vehicle_surface_first_crossing_micrometres[3];
    /* Closed-volume containment is separately qualified: open, non-manifold,
     * inconsistently oriented, self-intersecting, or degenerate retained
     * shells still retain truthful surface-crossing evidence but cannot make
     * an inside/depth claim. Qualified results are bounded centroid samples. */
    int vehicle_volume_qualified;
    unsigned vehicle_shell_boundary_edges;
    unsigned vehicle_shell_nonmanifold_edges;
    unsigned vehicle_shell_orientation_mismatch_edges;
    unsigned vehicle_shell_self_intersection_pairs;
    unsigned vehicle_containment_samples_tested;
    unsigned vehicle_containment_inside_samples;
    unsigned vehicle_containment_boundary_samples;
    unsigned vehicle_containment_outside_samples;
    unsigned long long vehicle_containment_maximum_depth_micrometres;
    long long vehicle_containment_deepest_micrometres[3];
    /* Exact final opaque-depth witness from asynchronous WebGPU occlusion
     * queries. A structurally valid but unqualified result names transparent
     * draws and carries no region ratio. Boolean tile masks deliberately do
     * not claim portable per-pixel sample counts. */
    int opaque_visibility_valid;
    int opaque_visibility_qualified;
    unsigned opaque_visibility_width;
    unsigned opaque_visibility_height;
    int opaque_visibility_viewport[4];
    int opaque_visibility_scissor[4];
    unsigned opaque_visibility_primitive_draws;
    unsigned opaque_visibility_opaque_draws;
    unsigned opaque_visibility_masked_draws;
    unsigned opaque_visibility_transparent_draws;
    unsigned opaque_visibility_grid_columns;
    unsigned opaque_visibility_grid_rows;
    unsigned opaque_visibility_isolated_tiles;
    unsigned opaque_visibility_scene_tiles;
    unsigned long long opaque_visibility_isolated_tile_mask;
    unsigned long long opaque_visibility_scene_tile_mask;
    /* Exact named opaque-depth overlap in stable order: retained vehicle body,
     * vehicle-part sprites, held object. Presence is distinct from
     * qualification so "not equipped" never reads as "measured clear." */
    unsigned opaque_visibility_occluder_present_mask;
    unsigned opaque_visibility_occluder_qualified_mask;
    unsigned opaque_visibility_occluder_draws
        [MDKR_CHARACTER_PREVIEW_OCCLUDERS];
    unsigned opaque_visibility_occluder_unqualified_draws
        [MDKR_CHARACTER_PREVIEW_OCCLUDERS];
    unsigned opaque_visibility_occluder_overlap_tiles
        [MDKR_CHARACTER_PREVIEW_OCCLUDERS];
    unsigned long long opaque_visibility_occluder_overlap_tile_mask
        [MDKR_CHARACTER_PREVIEW_OCCLUDERS];
    /* Model-alpha captures additionally bind the donor-target fit to exact PNG
     * pixels. Points 0..7 are the calibrated AABB corners (XYZ bits), point 8
     * is the anchor, and point 9 is a scaled forward endpoint. All values are
     * fixed point so no host float becomes durable app evidence. */
    int fit_projection_valid;
    unsigned fit_projection_width;
    unsigned fit_projection_height;
    int fit_projection_viewport[4];
    int fit_projection_scissor[4];
    unsigned fit_projection_primitive_draws;
    int fit_projection_pixel_milli
        [MDKR_CHARACTER_PREVIEW_PROJECTION_POINTS][2];
    int fit_projection_depth_millionths
        [MDKR_CHARACTER_PREVIEW_PROJECTION_POINTS];
    unsigned fit_projection_clip_flags
        [MDKR_CHARACTER_PREVIEW_PROJECTION_POINTS];
    unsigned long long inspection_pose_ticks;
    unsigned long long inspection_pose_fallback_ticks;
    unsigned long long inspection_transition_switches;
    unsigned long long inspection_transition_blending_ticks;
    unsigned long long inspection_transition_completions;
    unsigned transition_from_blend_milli;
    unsigned transition_to_blend_milli;
    MdkrCharacterPreviewMotionSource transition_from_motion_source;
    MdkrCharacterPreviewMotionSource transition_to_motion_source;
    int view_yaw_degrees;
    int view_pitch_degrees;
    MdkrWorkshopPreviewLighting lighting;
    unsigned long long camera_override_ticks;
    unsigned long long lighting_override_draws;
    int capture_requested;
    MdkrCharacterPreviewCaptureKind capture_kind;
    int capture_armed;
    unsigned long long capture_stable_frames;
    int capture_written;
    unsigned long long capture_png_bytes;
    /* Exact comparison environment captured inside the engine session. Text
     * comes from the bounded GPU diagnostic record; dimensions distinguish
     * output resolution from RenderScale's actual scene resolution. */
    char renderer_backend[32];
    char adapter[192];
    char driver[192];
    unsigned vendor_id;
    unsigned device_id;
    unsigned output_width;
    unsigned output_height;
    unsigned render_width;
    unsigned render_height;
} MdkrCharacterPreviewResult;

#define MDKR_CHARACTER_PREVIEW_RESULT_VERSION 23u
#define MDKR_CHARACTER_PREVIEW_TRANSITION_DWELL_MILLI \
    MDKR_MODERN_CHARACTER_INSPECTION_TRANSITION_DWELL_MILLI
#define MDKR_CHARACTER_PREVIEW_CAPTURE_STABLE_FRAMES 12u

// Owned by the C engine entry module and non-NULL only during a launcher-owned
// preview boot. The game writes through it before engine teardown resets the
// underlying bounded counters.
extern MdkrCharacterPreviewResult *g_mdkrCharacterPreviewResult;

/* A semantic-motion review is deliberately a separate contract from the clean
 * live performance sample above. One exact session holds all three select
 * states or eleven race samples long enough to collect fresh fit,
 * gameplay-camera, visibility, and (for vehicles) retained-body/contact
 * witnesses. Keeping the samples separate prevents a favourable frame from
 * hiding another state's clipping or occlusion without invalidating durable
 * v21 timing evidence. */
typedef enum MdkrCharacterMotionReviewSample {
    MDKR_CHARACTER_MOTION_REVIEW_START = 0,
    MDKR_CHARACTER_MOTION_REVIEW_STEER,
    MDKR_CHARACTER_MOTION_REVIEW_REVERSE,
    MDKR_CHARACTER_MOTION_REVIEW_BOOST,
    MDKR_CHARACTER_MOTION_REVIEW_ITEM,
    MDKR_CHARACTER_MOTION_REVIEW_DAMAGE,
    MDKR_CHARACTER_MOTION_REVIEW_SPIN,
    MDKR_CHARACTER_MOTION_REVIEW_AIRBORNE,
    MDKR_CHARACTER_MOTION_REVIEW_LAND,
    MDKR_CHARACTER_MOTION_REVIEW_FINISH,
    MDKR_CHARACTER_MOTION_REVIEW_FINISH_LOSE,
    MDKR_CHARACTER_MOTION_REVIEW_SAMPLE_COUNT,
} MdkrCharacterMotionReviewSample;

#define MDKR_CHARACTER_MOTION_REVIEW_SELECT_SAMPLE_COUNT 3u
#define MDKR_CHARACTER_MOTION_REVIEW_VEHICLE_SAMPLE_COUNT \
    ((unsigned)MDKR_CHARACTER_MOTION_REVIEW_SAMPLE_COUNT)
#define MDKR_CHARACTER_MOTION_REVIEW_SAMPLE_MASK(count) \
    ((1u << (count)) - 1u)

typedef struct MdkrCharacterMotionReviewResult {
    unsigned version;
    int started;
    int completed;
    MdkrCharacterPreviewContext context;
    MdkrCharacterPreviewScene scene;
    unsigned sample_count;
    unsigned completed_mask;
    unsigned failed_sample;
    MdkrCharacterPreviewResult
        samples[MDKR_CHARACTER_MOTION_REVIEW_SAMPLE_COUNT];
    /* Per held state, the maximum change in endpoint-minus-target between
     * consecutive successful solved draws. This isolates solver/contact
     * instability from legitimate whole-pose or vehicle motion. */
    unsigned contact_stability_mask
        [MDKR_CHARACTER_MOTION_REVIEW_SAMPLE_COUNT];
    unsigned long long contact_stability_observations
        [MDKR_CHARACTER_MOTION_REVIEW_SAMPLE_COUNT]
        [MDKR_CHARACTER_PREVIEW_CONTACTS];
    unsigned long long contact_stability_max_step_micrometres
        [MDKR_CHARACTER_MOTION_REVIEW_SAMPLE_COUNT]
        [MDKR_CHARACTER_PREVIEW_CONTACTS];
} MdkrCharacterMotionReviewResult;

#define MDKR_CHARACTER_MOTION_REVIEW_RESULT_VERSION 6u
#define MDKR_CHARACTER_CONTACT_STABILITY_MINIMUM_OBSERVATIONS 8u
#define MDKR_CHARACTER_MOTION_REVIEW_ALL_SAMPLES \
    MDKR_CHARACTER_MOTION_REVIEW_SAMPLE_MASK( \
        MDKR_CHARACTER_MOTION_REVIEW_VEHICLE_SAMPLE_COUNT)

extern MdkrCharacterMotionReviewResult *g_mdkrCharacterMotionReviewResult;

typedef struct {
    const char *rom_path;      // NULL/empty => engine default (baserom.us.v80.z64)
    int   video_mode;          // MdkrVideoMode, or -1 for "don't pass a preset"
    int   window_width;        // <= 0 => engine default
    int   window_height;
    int   automation_ticks;    // <= 0 => interactive; launcher regression seam
    int   automation_frames;   // mutually exclusive presentation-frame seam
    const char *input_script;  // automation-only deterministic controller fixture
    // One-shot Character Workshop route. The engine enters the exact authored
    // select or race context without menu-navigation scripts; zero disables it.
    const char *character_preview_package;
    MdkrCharacterPreviewContext character_preview_context;
    MdkrCharacterPreviewScene character_preview_scene;
    int character_preview_players;  // 1..4
    MdkrCharacterPreviewPose character_preview_pose;
    unsigned character_preview_pose_phase_milli;  // 0..1000
    MdkrCharacterPreviewPose character_preview_transition_from_pose;
    unsigned character_preview_transition_from_phase_milli;  // 0..1000
    int character_preview_view_yaw_degrees;       // -180..180
    int character_preview_view_pitch_degrees;     // -90..90
    MdkrWorkshopPreviewLighting character_preview_lighting;
    const char *character_preview_capture_png;    // optional, create-only
    MdkrCharacterPreviewCaptureKind character_preview_capture_kind;
    int character_preview_donor_reference; // comparison capture only
    int character_preview_auto_return; // return after queued one-shot capture
    /* Launcher-owned exact Offset Studio. The ordinary game overlay remains
     * available for every other boot; this mode keeps a focused editor open
     * over the real preview scene and never admits gameplay input. */
    int character_preview_studio;
    MdkrCharacterPreviewResult *character_preview_result;
    /* Vehicle-only, one-player, held-pose review. Its result is session
     * evidence and is never admitted to the performance matrix. */
    int character_motion_review;
    MdkrCharacterMotionReviewResult *character_motion_review_result;
    // Staged RESTART-scope settings, as "Video.Key=Value" strings. The settings
    // panel writes these when the player changes a restart-scope key before
    // pressing Play, so the choice takes effect on THIS boot rather than
    // silently waiting for the next one.
    const char *overrides[MDKR_BOOT_MAX_OVERRIDES];
    int   override_count;
} MdkrBootConfig;

// Boot the engine with the given config. Blocks until the game exits; returns
// the engine's exit code. Implemented by synthesizing a CLI invocation and
// delegating to mdkr64_headless_main(), so the launcher shares the exact engine
// boot path rather than a parallel one that could drift.
int mdkr64_engine_boot(const MdkrBootConfig *cfg);

// --- Fatal-crash write mirror (Windows) ------------------------------------
// DiagLog_install() tees stdout/stderr through a pipe drained by a reader
// thread. That thread dies with the process, so a crash-time write to stderr
// can be lost or block forever on a full pipe -- exactly when the diagnostic
// matters most. The tee therefore publishes the PRE-tee console fd and the raw
// mdkr64.log fd here, and the engine's crash handler writes to them directly,
// bypassing the pipe. -1 means the tee is not installed (every CLI invocation),
// in which case stderr is already the real console and needs no bypass.
// Defined in platform/main_pc.c.
extern int g_diagLogRealErrFd;
extern int g_diagLogFileFd;

// --- Crash-screen presentation hook ----------------------------------------
// platform/main_pc.c's SIGSEGV/SIGBUS handler calls this AFTER it has written
// and flushed `[CRASH]` and the backtrace, and before it restores the default
// disposition and re-raises. The app shell registers
// platform/app/crash_screen.cpp here; NULL (every CLI invocation) means the
// handler behaves exactly as it did before the crash screen existed.
//
// Declared here rather than duplicated ad hoc for the reason this header
// exists: the engine defines it, the shell assigns it, and neither carries its
// own copy of the type. Defined in platform/main_pc.c.
extern void (*g_mdkrCrashScreenHook)(int signo);

// --- In-game overlay hooks (platform/app_overlay_hooks.c) ------------------
// The app registers these before boot; the engine's event pump and frame-end
// call them. Keeps the C engine free of any ImGui/C++ dependency.
//
// The pointer types are named, and named HERE, so that they pick up this
// header's C language linkage. The struct itself must stay inside the extern "C"
// block -- platform/app_overlay_hooks.c and tests/test_app_overlay_hooks.c are
// real C consumers -- and a C-linkage member can only legally be assigned a
// function that also has C linkage. Every implementation on the C++ side is
// therefore defined inside an `extern "C" { static ... }` block (internal
// linkage, C function type); see platform/app/ui_overlay.cpp.
typedef int  (*AppOverlayProcessEventFn)(const void *sdl_event);
typedef void (*AppOverlayServiceFn)(void);
typedef int  (*AppOverlayQueryFn)(void);
typedef int  (*AppOverlayRenderFn)(void);

typedef struct {
    /* Nonzero means the host shortcut consumed this event before game input. */
    AppOverlayProcessEventFn process_event;
    /* Called at the event-pump boundary before a new frame begins. */
    AppOverlayServiceFn      service;
    AppOverlayQueryFn        wants_input;
    /* Input capture and simulation pause are deliberately independent. An
     * online Party overlay consumes navigation input without stopping one
     * endpoint's authoritative clock. */
    AppOverlayQueryFn        wants_pause;
    AppOverlayQueryFn        wants_render;
    AppOverlayRenderFn       render;  // zero reports a fatal overlay-render failure
} AppOverlayHooks;
void platformSetOverlayHooks(const AppOverlayHooks *hooks);

// Apply the game's authored pause mix while the app-owned F1 overlay is open.
// The overlay layer composes with the game's latest authored volume mode; menu
// work may update that underlying mode while paused without removing the duck.
// Audio synthesis continues, so sequencing and the host sink remain continuous.
void mdkr64_engine_overlay_audio_pause(void);
void mdkr64_engine_overlay_audio_resume(void);

// --- Flat-window toast -------------------------------------------------------
// A short line the FLAT window shows for a few seconds (the VR view-mode name
// today). State lives engine-side (platform/app_overlay_hooks.c) so the VR
// layer can feed it without reaching the C++ shell. The overlay polls the
// serial each frame: a change (re)starts its own few-second timer, because
// this seam deliberately carries no clock of its own (SDL-free test targets
// build the state file too). Text is NULL until the first feed.
void platformOverlayVrModeToast(const char *name);
const char *platformOverlayVrModeToastText(void);
unsigned platformOverlayVrModeToastSerial(void);

#ifdef __cplusplus
}
#endif

#endif  // MDKR64_ENGINE_ENTRY_H
