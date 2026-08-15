# Sonic Robo Blast 2 VR and Ring Racers VR

VR source for both games.

- `base.lock` names the upstream repo and the exact commit
- `files/` are the added files, in their real paths
- `patches/` is one numbered patch per changed upstream file

## Rebuilding

Clone upstream at the commit in `base.lock`, copy `files/` over the tree, apply
everything in `patches/` in numbered order, then build as that project normally
builds.

Sonic Robo Blast 2
  https://github.com/STJr/SRB2
  commit 8701ef41f617c06e23a7b8ccd2199c160c5d1dd1 (SRB2_release_2.2.15)

Dr. Robotnik's Ring Racers
  https://github.com/KartKrewDev/RingRacers
  commit 95cdf5eb6c4b3c02e0a03e88a05c65c1ee9d46f9

## Licence

GPLv2, same as upstream.
