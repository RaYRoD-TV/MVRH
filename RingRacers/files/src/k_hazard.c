// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  k_hazard.c
/// \brief Hazardous surfaces - lava that actually burns, and how it feels.

#include "k_hazard.h"

#include "doomstat.h"
#include "g_game.h"
#include "g_demo.h"
#include "m_fixed.h"
#include "p_local.h"
#include "p_mobj.h"
#include "k_terrain.h" // terrain damageType is what marks a hazardous surface
#include "r_defs.h"
#include "r_main.h"
#include "s_sound.h"
#include "sounds.h"

// Exposure per local view: climbs while a player is burning and falls away
// once they're clear. Rising is quicker than falling on purpose - the moment
// you touch lava should land immediately, while the heat leaving you is what
// tells you how close that was.
static fixed_t hazard_exposure[MAXSPLITSCREENPLAYERS];

// How long the burning sound waits before it retriggers. sfx_fire is a short
// sample, so it's laid down repeatedly to make a bed rather than played once
// - no new sound data ships for this.
#define HAZARD_SOUND_PERIOD (TICRATE/3)
static UINT8 hazard_sound_timer[MAXSPLITSCREENPLAYERS];

// Rising is quicker than falling on purpose: touching lava should land the
// same tic, while the heat leaving you afterwards is what tells you how close
// that was. Not instant either way - a snapped-on tint reads as a filter
// someone switched on, and a fade reads as heat.
#define HAZARD_RISE (FRACUNIT/12)
#define HAZARD_FALL (FRACUNIT/28)

// The first-person boost cue eases the same way: sharp in so the payout lands
// with the sound, gentle out so it reads as the boost spending itself.
static fixed_t boost_cue[MAXSPLITSCREENPLAYERS];
// A landing dash is an EVENT, not a state: you hit the ground and the kart
// shoves. The steady cue above cannot say that - it is the same glow whether
// you have been boosting for two seconds or two tics - so the kick is its own
// channel, spiking on the tic a launch-class boost STARTS and decaying fast.
// It drives a head snap in first person and a punch on the cue, which is what
// makes a land boost land instead of just brightening the screen.
static fixed_t boost_kick[MAXSPLITSCREENPLAYERS];
static boolean boost_launching[MAXSPLITSCREENPLAYERS];
#define BOOSTCUE_RISE (FRACUNIT/6)
#define BOOSTCUE_FALL (FRACUNIT/22)
// About a third of a second to fall away. Long enough to feel as a shove,
// short enough that it is over before you need to steer.
#define BOOSTKICK_FALL (FRACUNIT/12)

// What counts as a boost you can SEE: the speed-lines list plus the drop and
// landing dashes (which the lines skip), minus drafting - a draft lasts whole
// straights, and feedback that long stops reading as an event. Presentation
// only (the first-person cue reads it), so it is free to differ from the
// sim's own lists without touching them.
static boolean K_PlayerVisiblyBoosting(player_t *player)
{
	return (player->sneakertimer || player->panelsneakertimer
		|| player->weaksneakertimer || player->ringboost
		|| player->driftboost || player->startboost
		|| player->eggmanexplode || player->trickboost
		|| player->gateBoost || player->wavedashboost
		|| player->dropdashboost || player->aciddropdashboost);
}

boolean K_HazardRulesActive(void)
{
	// A rules change is a simulation change. Online races and replays run
	// against clients that don't have it, so it stands down for both rather
	// than desyncing them.
	if (netgame || demo.playback)
	{
		return false;
	}

	return (cv_hazardsurfaces.value != 0);
}

boolean K_LandingDashActive(void)
{
	if (netgame || demo.playback)
	{
		return false;
	}

	return (cv_landingdash.value != 0);
}

boolean K_TrickAssistActive(void)
{
	if (netgame || demo.playback)
	{
		return false;
	}

	return (cv_trickassist.value != 0);
}

// Which players' live trick the assist conjured out of plain air. Set when the
// grant fires, cleared the moment no trick is live (both in K_KartPlayerThink),
// so a panel's or a spring's trick never carries the mark.
static boolean sAirGrantedTrick[MAXPLAYERS];

void K_SetAirGrantedTrick(const player_t *player, boolean granted)
{
	const INT32 i = (player == NULL) ? -1 : (INT32)(player - players);

	if (i >= 0 && i < MAXPLAYERS)
	{
		sAirGrantedTrick[i] = granted;
	}
}

boolean K_AirGrantedTrick(const player_t *player)
{
	const INT32 i = (player == NULL) ? -1 : (INT32)(player - players);

	return (i >= 0 && i < MAXPLAYERS) ? sAirGrantedTrick[i] : false;
}

boolean K_PlayerOnHazard(player_t *player)
{
	if (player == NULL || player->mo == NULL || P_MobjWasRemoved(player->mo))
	{
		return false;
	}

	// The real one: the floor's texture picks a terrain, and a terrain with a
	// damage type IS a hazardous surface. This is what every track in the
	// game uses - the Damage / WipeoutDamage / TumbleDamage terrains, mapped
	// from 146 floor textures - so it is what the feel has to follow.
	//
	// Stumble is left out: it is a trip, not a burn, and lighting the screen
	// up for it would cry wolf. Death pits and instakill are instant, with no
	// exposure to build.
	if (player->mo->terrain != NULL && player->mo->terrain->damageType > 0)
	{
		const UINT8 dmg = (UINT8)(player->mo->terrain->damageType & 0xFF);

		if (dmg != DMG_STUMBLE && dmg != DMG_DEATHPIT && dmg != DMG_INSTAKILL)
		{
			return true;
		}
	}

	// A swimmable lava FOF raises this as the mobj moves through it.
	if (player->mo->eflags & MFE_TOUCHLAVA)
	{
		return true;
	}

	// A plain lava floor raises nothing - the flag above is only set inside
	// the FOF loop - so the sector has to be asked directly.
	if (player->mo->subsector != NULL
		&& player->mo->subsector->sector != NULL
		&& player->mo->subsector->sector->damagetype == SD_LAVA)
	{
		fixed_t height;

		// Deliberately not "are you standing on it". Burning throws you off
		// the ground, so a strict footing test would cut the heat out at the
		// exact moment it should be at its worst - and skimming a lava
		// channel should still cook you. Anything within a kart's height of
		// the surface counts; a real jump climbs out of it.
		if (player->mo->eflags & MFE_VERTICALFLIP)
		{
			height = player->mo->ceilingz - (player->mo->z + player->mo->height);
		}
		else
		{
			height = player->mo->z - player->mo->floorz;
		}

		if (height <= FixedMul(160 * FRACUNIT, mapobjectscale))
		{
			return true;
		}
	}

	return false;
}

fixed_t K_HazardExposure(UINT8 viewnum)
{
	if (viewnum >= MAXSPLITSCREENPLAYERS)
	{
		return 0;
	}

	return hazard_exposure[viewnum];
}

fixed_t K_BoostCue(UINT8 viewnum)
{
	if (viewnum >= MAXSPLITSCREENPLAYERS)
	{
		return 0;
	}

	return boost_cue[viewnum];
}

fixed_t K_BoostKick(UINT8 viewnum)
{
	if (viewnum >= MAXSPLITSCREENPLAYERS)
	{
		return 0;
	}

	return boost_kick[viewnum];
}

void K_ResetHazardFeedback(void)
{
	UINT8 i;

	for (i = 0; i < MAXSPLITSCREENPLAYERS; i++)
	{
		hazard_exposure[i] = 0;
		hazard_sound_timer[i] = 0;
		boost_cue[i] = 0;
		boost_kick[i] = 0;
		boost_launching[i] = false;
	}
}

void K_HazardFeedbackTick(void)
{
	UINT8 i;

	for (i = 0; i < MAXSPLITSCREENPLAYERS; i++)
	{
		player_t *player = NULL;
		boolean burning = false;

		if (i <= r_splitscreen)
		{
			player = &players[displayplayers[i]];
		}

		// Feedback follows the rules: with hazards standing down (a netgame,
		// a replay, or the setting off) lava does nothing, and saying
		// otherwise with a rumble and a red screen would be a lie.
		burning = (K_HazardRulesActive() && K_PlayerOnHazard(player));

		if (burning)
		{
			hazard_exposure[i] += HAZARD_RISE;
			if (hazard_exposure[i] > FRACUNIT)
			{
				hazard_exposure[i] = FRACUNIT;
			}

			if (hazard_sound_timer[i] == 0)
			{
				S_StartSoundAtVolume(player->mo, sfx_fire, 96);
				hazard_sound_timer[i] = HAZARD_SOUND_PERIOD;
			}
			else
			{
				hazard_sound_timer[i]--;
			}
		}
		else
		{
			hazard_exposure[i] -= HAZARD_FALL;
			if (hazard_exposure[i] < 0)
			{
				hazard_exposure[i] = 0;
			}

			hazard_sound_timer[i] = 0;
		}

		// The boost cue rides the same per-view tick. It tracks the boost
		// state itself, not the view mode - the draw side decides whether
		// first person is showing, so switching views mid-boost stays right.
		if (player != NULL && player->mo != NULL && player->spectator == false
			&& player->speed > 0 && K_PlayerVisiblyBoosting(player))
		{
			boost_cue[i] += BOOSTCUE_RISE;
			if (boost_cue[i] > FRACUNIT)
			{
				boost_cue[i] = FRACUNIT;
			}
		}
		else
		{
			boost_cue[i] -= BOOSTCUE_FALL;
			if (boost_cue[i] < 0)
			{
				boost_cue[i] = 0;
			}
		}

		// The kick. Only the launch-class boosts - the ones that arrive as a
		// shove rather than as a state you sit in - and only on the tic they
		// START, so holding a sneaker does not hold your head back.
		{
			const boolean launching = (player != NULL && player->mo != NULL
				&& player->spectator == false
				&& (player->dropdashboost || player->aciddropdashboost
					|| player->wavedashboost || player->trickboost
					|| player->startboost));

			if (launching && !boost_launching[i])
			{
				boost_kick[i] = FRACUNIT;
			}
			boost_launching[i] = launching;

			boost_kick[i] -= BOOSTKICK_FALL;
			if (boost_kick[i] < 0)
			{
				boost_kick[i] = 0;
			}
		}
	}
}
