#pragma once
// The part catalogue: batteries, motors, wheels, bulbs... each a small voxel
// template with its component record. Used to build the scene and in the
// workshop.
#include <memory>
#include <string>
#include <vector>
#include "build.h"

struct PartDef {
  std::string name, blurb;
  int icon = 0;                 // UI icon id
  std::shared_ptr<Template> tpl;
};

void initParts(float vs);
const std::vector<PartDef>& partCatalogue();
const PartDef* findPart(const std::string& name);

// Rotate a template's voxels by quarter turns about Y (yaw) then X (pitch).
std::shared_ptr<Template> rotated(const Template& t, int yaw, int pitch);
// Copy a template into a volume at cell offset; returns the new component id.
uint32_t stamp(Volume& dst, const Template& t, const ivec3& at, const std::string& objectName = "");
// Capture an existing component from a volume as a reusable template (salvage).
std::shared_ptr<Template> captureComponent(const Volume& src, uint32_t compId);
