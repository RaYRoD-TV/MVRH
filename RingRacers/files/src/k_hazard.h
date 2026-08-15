// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  k_hazard.h
/// \brief Hazardous surfaces - lava that actually burns, and how it feels.
///
/// The engine has always carried the parts: SD_LAVA in the sector damage
/// enum, MFE_TOUCHLAVA raised on contact, a dedicated lava splash. Nothing
/// ever read them for damage, so lava was a surface you drove across.
///
/// This module owns both halves. The rules half answers "is this player on
/// something that should hurt them", which the sector damage code asks once a
/// tic. The feel half turns that contact into one exposure value per local
/// player - a single number that rumble, sound and the screen's heat cast all
/// read, so the three can never disagree with each other or with the damage.

#ifndef __K_HAZARD_H__
#define __K_HAZARD_H__

#include "doomdef.h"
#include "doomtype.h"
#include "d_player.h"

#ifdef __cplusplus
extern "C" {
#endif

extern consvar_t cv_hazardsurfaces;
extern consvar_t cv_hazardtumble;
extern consvar_t cv_trickassist;
extern consvar_t cv_handling;
extern consvar_t cv_drifthandling;
extern consvar_t cv_landingdash;

/// Tics you have to hit gas after a touchdown, and what it pays. Both are
/// feel numbers - tune them in a race, not on paper. Well over half a second:
/// generous enough that anyone reaching for it lands it, short enough that a
/// plain late throttle doesn't read as a payout.
#define LANDDASHWINDOW 24

/// How long the stock Drop Dash has to charge before it will pay. The game
/// asks for a quarter second of held gas, which is a long time to be
/// falling and easy to miss entirely on a short drop - with the assists on
/// it becomes barely a tap. Netgames and replays keep the stock timing.
#define DROPDASHCHARGE 2
#define LANDDASHBOOST 100

/// Is the Landing Dash live? Off in netgames and replays like every other
/// rule here: it changes the simulation, and a stock client would desync.
boolean K_LandingDashActive(void);

/// Does the Trick Assist own tricks right now? While it does, tricks arm from
/// plain air with no panel under them, fire off the drift button, and stay a
/// trick and nothing more - no thrust that redirects the kart, no tumble for
/// running out of air. Rides the Trick Assist setting, and stands down online
/// and in replays like every rule change here.
boolean K_TrickAssistActive(void);

/// Did this player's live trick come from the assist's own anywhere-grant,
/// rather than from a trick panel or a spring? The game's own trick spots are
/// DESIGNED to throw you somewhere - that is the point of taking one - so they
/// keep every bit of their kick and their get-ready ring. Only the trick the
/// assist conjured out of plain air goes quiet: no shove, no prompt.
///
/// Local rules only, so it lives beside the other assist state rather than in
/// player_t: nothing to archive, nothing for a stock client to disagree with.
boolean K_AirGrantedTrick(const player_t *player);
void K_SetAirGrantedTrick(const player_t *player, boolean granted);

static inline UINT8 K_DropDashChargeTics(void)
{
	return K_LandingDashActive() ? DROPDASHCHARGE : (TICRATE/4);
}

/// Are hazardous surfaces live right now? Off in netgames and during demo
/// playback no matter what the cvar says: this changes the race simulation,
/// and online play and replays have to stay identical to a stock client.
boolean K_HazardRulesActive(void);

/// Is this player in contact with a burning surface? True for a lava floor
/// sector and for a swimmable lava FOF - the two raise different flags, and
/// checking only the FOF one (the tempting single test, since it has a name)
/// silently misses every flat lava floor in the game.
boolean K_PlayerOnHazard(player_t *player);

/// Per-tic feedback update for the local players. Advances each one's
/// exposure and keeps the burning sound going underneath.
void K_HazardFeedbackTick(void);

/// This view's exposure, 0 (clear) to FRACUNIT (fully in it). Drives the
/// rumble strength and the heat cast over the 3D view.
fixed_t K_HazardExposure(UINT8 viewnum);

/// This view's boost intensity, 0 to FRACUNIT, eased like the exposure.
/// Drives the first-person boost cue - the afterimages trail behind the eye
/// there, so the boost reads on the picture instead.
fixed_t K_BoostCue(UINT8 viewnum);
// 0..FRACUNIT, spikes on the tic a launch-class boost starts and falls away in
// about a third of a second. Drives the first-person head snap and the punch
// on the boost cue - a landing dash is an event, and the steady cue cannot
// say so on its own.
fixed_t K_BoostKick(UINT8 viewnum);

/// Drop every local player's exposure immediately - level change, respawn,
/// anything that means the burning stopped without the player driving out.
void K_ResetHazardFeedback(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // __K_HAZARD_H__
