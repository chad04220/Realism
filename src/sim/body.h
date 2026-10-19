#pragma once
// A rigid body is a free-moving voxel volume: a table, a dropped brick, a
// rover. Every voxel keeps its material, component, temperature and damage, so
// a body is as simulated as the world around it.
#include <vector>
#include "volume.h"
#include "comps.h"

struct Sample {        // collision sphere standing in for a few surface voxels
  vec3 local;          // metres from the centre of mass, in body space
  float r;
  ivec3 cell;          // a representative voxel (material, component)
  uint32_t comp;
  uint8_t mat;
};

struct RotPart {       // a wheel or propeller drawn spinning about its axle
  uint32_t comp;
  vec3 pivot;          // voxel coords in the body volume
  vec3 axis;           // body space
  float radius;        // metres
};

struct Body {
  int id = 0;
  Volume vol;
  vec3 com;            // centre of mass in voxel coords (cell centres at i+0.5)
  vec3 pos;            // world position of the centre of mass, metres
  quat rot;
  vec3 vel, angVel;
  float mass = 0, invMass = 0;
  mat3 invIlocal = mat3::zero(), invIworld = mat3::zero();
  std::vector<Sample> samples;
  std::vector<RotPart> rot_parts;
  std::vector<uint32_t> compIds;   // components present
  size_t voxels = 0;
  float radius = 0;                // bounding sphere about the COM, metres
  bool asleep = false;
  float sleepTimer = 0, age = 0, restTime = 0;
  bool shapeDirty = true;          // mass/samples/devices need rebuilding
  bool splitCheck = false;         // connectivity must be rechecked
  HotSet heat;                     // bricks with thermal activity
  bool frozen = false;             // pinned in place (workshop jig)
  bool ghost = false;              // placement preview: drawn only, never simulated
  bool viewmodel = false;          // the tool in the player's hands (drawn opaque, in front)
  bool hidden = false;             // not drawn this frame
  bool held = false;
  int lastHit = 0;
  float submerged = 0;             // fraction in water this step (for buoyancy and visuals)
  // Accumulated external force/torque for this step (motors, rockets, tools).
  vec3 force, torque;

  mat3 R() const { return rot.toMat3(); }
  vec3 toWorld(const vec3& voxelCoord, float vs) const { return pos + rot.rotate((voxelCoord - com) * vs); }
  vec3 toLocalVoxel(const vec3& worldP, float vs) const { return rot.conj().rotate(worldP - pos) / vs + com; }
  vec3 pointVel(const vec3& worldP) const { return vel + cross(angVel, worldP - pos); }
  void applyImpulse(const vec3& j, const vec3& at) {
    if (invMass == 0) return;
    vel += j * invMass;
    angVel += invIworld * cross(at - pos, j);
  }
  void wake() { asleep = false; sleepTimer = 0; }
  void updateInertiaWorld() { mat3 r = R(); invIworld = r * invIlocal * r.transposed(); }
};
