#pragma once
// Multi-part machines built from the part catalogue with real wiring. Each
// builder writes into a volume at a local offset `o` (metres) and returns
// false if a wire couldn't be routed. Used by the scene and as workshop
// blueprints.
#include <functional>
#include <string>
#include <vector>
#include "build.h"

struct Assembly {
  std::string name, blurb;
  vec3 size;   // metres
  std::function<bool(Volume&, const vec3&)> build;
};

const std::vector<Assembly>& assemblies();
const Assembly* findAssembly(const std::string& name);

// Helpers shared with the scene.
Component makePart(const std::string& name, const std::string& object, Joint j, CompType t = C_PART);
std::vector<ivec3> cellsOf(const Volume& v, uint32_t comp, uint8_t mat);
ivec3 topmost(const std::vector<ivec3>& c);
ivec3 extremal(const std::vector<ivec3>& c, int axis, bool maxSide);
