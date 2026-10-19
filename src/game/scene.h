#pragma once
#include "../sim/sim.h"

struct SceneInfo {
  vec3 spawn;
  float spawnYaw = 0;
  vec3 jigOrigin;          // workshop build volume corner (metres)
  ivec3 jigDims;           // voxels
  float groundY = 1.0f;
};

// Build the starting environment: terrain, pond, sand, a brick shed with a
// workbench, lamp and fan inside, a tree, a campfire, a powder keg, crates,
// a reinforced concrete column, a rover and the workshop pad.
SceneInfo buildScene(Sim& s);
// After bulk edits: rebuild the world's electrical, thermal and fluid bookkeeping.
void rescanWorld(Sim& s);
