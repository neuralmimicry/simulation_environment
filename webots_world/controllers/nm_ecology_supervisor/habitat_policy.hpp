#pragma once

#include <algorithm>
#include <string>

namespace nm_webots {

enum class Mobility { Land, Flight, SwimOnly, Amphibious };

struct Position {
  double x;
  double y;
  double z;
};

struct Bounds3d {
  double min_x;
  double max_x;
  double min_y;
  double max_y;
  double min_z;
  double max_z;
};

inline Mobility mobility_from_string(const std::string &value) {
  if (value == "flight")
    return Mobility::Flight;
  if (value == "swim_only")
    return Mobility::SwimOnly;
  if (value == "amphibious")
    return Mobility::Amphibious;
  return Mobility::Land;
}

inline const char *mobility_name(Mobility mobility) {
  switch (mobility) {
    case Mobility::Flight: return "flight";
    case Mobility::SwimOnly: return "swim_only";
    case Mobility::Amphibious: return "amphibious";
    default: return "land";
  }
}

// Keep the same scene clock for all agents and clamp only movement domains
// that a profile cannot enter. Flight and amphibious profiles can cross every
// authored surface; swimmer bounds include the water volume, not its shore.
inline bool constrain_position(Position &position, Mobility mobility,
                               const Bounds3d &water, double land_margin = 0.08) {
  const Position before = position;
  if (mobility == Mobility::SwimOnly) {
    position.x = std::clamp(position.x, water.min_x, water.max_x);
    position.y = std::clamp(position.y, water.min_y, water.max_y);
    position.z = std::clamp(position.z, water.min_z, water.max_z);
  } else if (mobility == Mobility::Land && position.x >= water.min_x &&
             position.x <= water.max_x && position.y >= water.min_y &&
             position.y <= water.max_y) {
    // The south bank is the shared amphibious entry, so land-only bodies
    // return there instead of being teleported through a walled pool edge.
    position.x = std::clamp(position.x, water.min_x, water.max_x);
    position.y = water.min_y - land_margin;
  }
  return position.x != before.x || position.y != before.y || position.z != before.z;
}

}  // namespace nm_webots
