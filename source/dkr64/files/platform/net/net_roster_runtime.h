/* Engine-lifetime publication of a launcher-owned, validated network roster. */
#ifndef MDKR_NET_ROSTER_RUNTIME_H
#define MDKR_NET_ROSTER_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "net_roster.h"
#include "match_launch_descriptor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install is intentionally copy-based: the engine never borrows launcher
 * memory and cannot mutate matchmaking/session state. Install before engine
 * threads start; clear only after they have joined. */
bool mdkr_net_roster_runtime_install(
    const MdkrMatchManifestV1 *manifest, const MdkrNetRoster *roster);
/* Player-visible online path. The validated descriptor is copied with the
 * roster; launcher memory is never borrowed by game code. */
bool mdkr_net_roster_runtime_install_launch(
    const MdkrMatchLaunchDescriptorV1 *launch,
    const MdkrNetRoster *roster);
void mdkr_net_roster_runtime_clear(void);
bool mdkr_net_roster_runtime_active(void);

/* Ownership-guard decision for the local-Play beach-ball (issue: a local boot
 * inheriting an online session's process-global roster, flipping the engine into
 * online-race mode and stalling forever on network input a local race never
 * sends).
 *
 * This is the pure decision only; the mutable owner token and the actual
 * install/clear live in the beta online-wiring layer (platform/app/
 * online_live_wiring.cpp), NOT here, because this translation unit is compiled
 * into every build WITHOUT the MDKR_ENABLE_ONLINE_BETA macro -- so keeping state
 * or callable state-mutating functions here would leak code into the beta-OFF
 * binary. As a `static inline` that a non-beta build never calls, this emits no
 * code there and the beta-OFF object stays byte-identical, while remaining
 * directly unit-testable.
 *
 * Returns true when a boot that presents `boot_owner` must force-clear the
 * currently installed roster before booting: the roster is active and this boot
 * does not own it. boot_owner == 0 means "this boot installs no roster"
 * (ordinary local Play) and clears ANY active roster; a boot that installed its
 * own roster passes its own nonzero token and is left untouched. */
static inline bool mdkr_net_roster_guard_decides_clear(
    bool roster_active, uint64_t current_owner, uint64_t boot_owner) {
    return roster_active && (boot_owner == 0u || current_owner != boot_owner);
}

const MdkrNetRoster *mdkr_net_roster_runtime_get(void);
const MdkrMatchManifestV1 *mdkr_net_roster_runtime_manifest(void);
const MdkrMatchLaunchDescriptorV1 *
mdkr_net_roster_runtime_launch_descriptor(void);
const MdkrMatchSeatSelectionV1 *mdkr_net_roster_runtime_selection(
    unsigned canonical_slot);

/* Authoritative gameplay calls this with its ordinary local-play value. The
 * fallback preserves retail/local behavior when no online roster is active. */
uint8_t mdkr_net_roster_runtime_canonical_player_count(uint8_t fallback);
uint8_t mdkr_net_roster_runtime_viewport_count(uint8_t fallback);
bool mdkr_net_roster_runtime_local_to_canonical(
    unsigned local_seat, uint8_t *canonical_slot);
bool mdkr_net_roster_runtime_viewport_to_canonical(
    unsigned viewport, uint8_t *canonical_slot);
/* Local play keeps its player index; an active roster owns the viewport map.
 * A missing online viewport must never select another player's racer. */
uint8_t mdkr_net_roster_runtime_viewport_player(
    unsigned viewport, uint8_t fallback);
bool mdkr_net_roster_runtime_canonical_to_local(
    unsigned canonical_slot, uint8_t *local_seat);

#ifdef __cplusplus
}
#endif
#endif
