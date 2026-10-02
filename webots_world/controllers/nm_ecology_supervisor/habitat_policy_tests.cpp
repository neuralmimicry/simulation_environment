#include "habitat_policy.hpp"

#include <cassert>

using nm_webots::Bounds3d;
using nm_webots::Mobility;
using nm_webots::Position;

int main() {
  const Bounds3d water{2.82, 7.38, -2.28, 2.28, 0.06, 0.44};

  Position land_on_shared_ground{0.0, 4.0, 0.19};
  assert(!nm_webots::constrain_position(land_on_shared_ground, Mobility::Land, water));
  assert(land_on_shared_ground.y == 4.0);

  Position land_entering_pool{5.1, 0.0, 0.19};
  assert(nm_webots::constrain_position(land_entering_pool, Mobility::Land, water));
  assert(land_entering_pool.y == water.min_y - 0.08);

  Position fish_outside_water{-5.0, 3.0, 4.0};
  assert(nm_webots::constrain_position(fish_outside_water, Mobility::SwimOnly, water));
  assert(fish_outside_water.x == water.min_x);
  assert(fish_outside_water.y == water.max_y);
  assert(fish_outside_water.z == water.max_z);

  Position amphibious_at_water_edge{5.1, -2.5, 0.04};
  assert(!nm_webots::constrain_position(amphibious_at_water_edge, Mobility::Amphibious, water));
  Position flyer_over_water{5.1, 0.0, 3.0};
  assert(!nm_webots::constrain_position(flyer_over_water, Mobility::Flight, water));
  assert(nm_webots::mobility_from_string("unknown") == Mobility::Land);
}
