#include <webots/Field.hpp>
#include <webots/Node.hpp>
#include <webots/Supervisor.hpp>
#include <nlohmann/json.hpp>
#include "habitat_policy.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using webots::Field;
using webots::Node;
using webots::Supervisor;
using nlohmann::json;
using nm_webots::Bounds3d;
using nm_webots::Mobility;
using nm_webots::Position;

using RobotMobility = std::unordered_map<std::string, Mobility>;

namespace {
constexpr double kTau = 6.28318530717958647692;

struct Npc {
  const char *def;
  double center_x;
  double center_y;
  double radius_x;
  double radius_y;
  double base_z;
  double lift;
  double speed;
  double phase;
};

constexpr std::array<Npc, 6> kNpcs{{
    {"NPC_POLLINATOR_00", -3.8, 1.4, 1.1, 0.75, 0.9, 0.38, 0.21, 0.0},
    {"NPC_POLLINATOR_01", -2.8, -0.8, 0.85, 0.52, 1.0, 0.34, 0.17, 1.4},
    {"NPC_DRAGONFLY_00", 4.1, -0.9, 1.35, 0.6, 1.25, 0.48, 0.29, 2.6},
    {"NPC_BIRD_00", 0.5, 4.3, 2.3, 0.95, 1.8, 0.68, 0.12, 4.0},
    {"NPC_FROG_00", 5.8, 0.4, 0.75, 0.3, 0.18, 0.11, 0.09, 0.7},
    {"NPC_FIREFLY_00", 2.6, 2.0, 1.2, 0.72, 0.75, 0.5, 0.24, 3.2},
}};

std::string env_or(const char *key, const char *fallback) {
  const char *value = std::getenv(key);
  return value && *value ? value : fallback;
}

void remove_unbound_robots(Supervisor &supervisor, const std::string &fleet_path,
                           const std::string &capabilities_path,
                           RobotMobility &mobility_by_robot, Bounds3d &water_bounds) {
  json robots = json::object();
  json profiles = json::object();
  json slots = json::object();
  std::ifstream capabilities_input(capabilities_path);
  if (capabilities_input) {
    try {
      const auto capabilities = json::parse(capabilities_input);
      profiles = capabilities.value("profiles", json::object());
      slots = capabilities.value("slots", json::object());
      const auto regions = capabilities.value("world_regions", json::object());
      const auto water = regions.value("water", json::object()).value("bounds_m", json::object());
      if (water.is_object()) {
        water_bounds = {
            water.value("min_x", water_bounds.min_x), water.value("max_x", water_bounds.max_x),
            water.value("min_y", water_bounds.min_y), water.value("max_y", water_bounds.max_y),
            water.value("min_z", water_bounds.min_z), water.value("max_z", water_bounds.max_z)};
      }
    } catch (const json::exception &error) {
      std::cerr << "[nm_ecology] cannot parse robot capability contract at " << capabilities_path
                << ": " << error.what() << "; water and mobility defaults will be used\n";
    }
  } else {
    std::cerr << "[nm_ecology] robot capability contract is unavailable at " << capabilities_path
              << "; water and mobility defaults will be used\n";
  }

  std::ifstream input(fleet_path);
  if (input) {
    try {
      const auto config = json::parse(input);
      if (config.is_object()) {
        const auto configured_robots = config.value("robots", json::object());
        if (configured_robots.is_object())
          robots = configured_robots;
      }
    } catch (const json::exception &error) {
      std::cerr << "[nm_ecology] cannot parse fleet config at " << fleet_path
                << ": " << error.what() << "; unbound slots will be removed\n";
    }
  } else {
    std::cerr << "[nm_ecology] fleet config is unavailable at " << fleet_path
              << "; unbound slots will be removed\n";
  }

  Node *root = supervisor.getRoot();
  Field *children = root ? root->getField("children") : nullptr;
  if (!children) {
    std::cerr << "[nm_ecology] cannot inspect world children for fleet pruning\n";
    return;
  }

  std::size_t retained = 0;
  std::size_t removed = 0;
  for (int index = children->getCount() - 1; index >= 0; --index) {
    Node *node = children->getMFNode(index);
    Field *name_field = node ? node->getField("name") : nullptr;
    if (!name_field)
      continue;
    const std::string robot_name = name_field->getSFString();
    if (robot_name.rfind("AARNN_", 0) != 0)
      continue;

    const auto binding = robots.find(robot_name);
    const std::string network_id =
        binding != robots.end() && binding->is_object()
            ? binding->value("network_id", std::string{})
            : std::string{};
    if (network_id.find_first_not_of(" \t\r\n") == std::string::npos) {
      std::cout << "[nm_ecology] removing unbound robot slot=" << robot_name << std::endl;
      node->remove();
      ++removed;
    } else {
      const std::string profile = binding->value("robot_profile", std::string{});
      const auto profile_config = profiles.find(profile);
      const auto slot_profile = slots.find(robot_name);
      const bool profile_matches_slot = slot_profile != slots.end() &&
          slot_profile->is_string() && slot_profile->get<std::string>() == profile;
      if (profile_matches_slot && profile_config != profiles.end() && profile_config->is_object()) {
        mobility_by_robot[robot_name] = nm_webots::mobility_from_string(
            profile_config->value("mobility", std::string("land")));
      } else {
        mobility_by_robot[robot_name] = Mobility::Land;
        std::cerr << "[nm_ecology] robot profile " << (profile.empty() ? "<missing>" : profile)
                  << " does not match the shared-world slot " << robot_name
                  << "; restricting it to shared land\n";
      }
      std::cout << "[nm_ecology] bound robot slot=" << robot_name << " profile="
                << (profile.empty() ? "land" : profile) << " mobility="
                << nm_webots::mobility_name(mobility_by_robot[robot_name]) << std::endl;
      ++retained;
    }
  }
  std::cout << "[nm_ecology] fleet physics robots retained=" << retained
            << " removed_unbound=" << removed << std::endl;
}

void set_vec3(Field *field, double x, double y, double z) {
  if (!field)
    return;
  const double value[3]{x, y, z};
  field->setSFVec3f(value);
}

bool save_snapshot(Supervisor &supervisor, const std::filesystem::path &target) {
  if (target.empty())
    return false;

  std::error_code error;
  if (!target.parent_path().empty())
    std::filesystem::create_directories(target.parent_path(), error);
  if (error) {
    std::cerr << "[nm_ecology] cannot create state directory: " << error.message() << '\n';
    return false;
  }

  const auto temporary = target.parent_path() /
      (target.stem().string() + ".pending" + target.extension().string());
  if (!supervisor.worldSave(temporary.string())) {
    std::cerr << "[nm_ecology] Webots could not save the shared world snapshot\n";
    return false;
  }

  std::filesystem::rename(temporary, target, error);
  if (error) {
    std::cerr << "[nm_ecology] cannot publish world snapshot: " << error.message() << '\n';
    return false;
  }
  const char *revision = std::getenv("NM_WEBOTS_SOURCE_REVISION");
  if (revision && *revision) {
    const auto revision_path = std::filesystem::path(target.string() + ".source-revision");
    const auto revision_temporary = std::filesystem::path(revision_path.string() + ".pending");
    {
      std::ofstream output(revision_temporary, std::ios::trunc);
      if (!output) {
        std::cerr << "[nm_ecology] cannot stage source revision marker\n";
      } else {
        output << revision << '\n';
        output.flush();
        if (output) {
          std::filesystem::rename(revision_temporary, revision_path, error);
        } else {
          error = std::make_error_code(std::errc::io_error);
        }
      }
    }
    if (error) {
      std::filesystem::remove(revision_temporary);
      std::cerr << "[nm_ecology] cannot publish source revision marker: " << error.message() << '\n';
      error.clear();
    }
  }
  std::cout << "[nm_ecology] saved persistent world state at " << target << '\n';
  return true;
}

double load_world_clock(const std::filesystem::path &path) {
  std::ifstream input(path);
  double elapsed = 0.0;
  if (input >> elapsed && std::isfinite(elapsed) && elapsed > 0.0)
    return elapsed;
  return 0.0;
}

bool save_world_clock(const std::filesystem::path &path, double elapsed) {
  if (path.empty())
    return false;
  std::error_code error;
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path(), error);
  if (error)
    return false;

  const auto temporary = path.string() + ".pending";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output)
      return false;
    output.precision(17);
    output << elapsed << '\n';
    output.flush();
    if (!output)
      return false;
  }
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary);
    std::cerr << "[nm_ecology] cannot publish world clock: " << error.message() << '\n';
    return false;
  }
  return true;
}
}  // namespace

int main() {
  Supervisor supervisor;
  const int step_ms = static_cast<int>(supervisor.getBasicTimeStep());
  RobotMobility mobility_by_robot;
  Bounds3d water_bounds{2.82, 7.38, -2.28, 2.28, 0.06, 1.35};
  remove_unbound_robots(
      supervisor, env_or("NM_WEBOTS_FLEET_CONFIG", "/etc/neuralmimicry/webots/fleet.json"),
      env_or("NM_WEBOTS_CAPABILITIES_FILE",
             "/opt/neuralmimicry/simulation_environment/webots_world/configs/robot_capabilities.json"),
      mobility_by_robot, water_bounds);
  Node *daylight = supervisor.getFromDef("NM_DAYLIGHT");
  Field *light_intensity = daylight ? daylight->getField("intensity") : nullptr;
  Field *light_color = daylight ? daylight->getField("color") : nullptr;

  struct TrackedNpc {
    const Npc *motion;
    Field *translation;
    Field *rotation;
  };
  std::array<TrackedNpc, kNpcs.size()> tracked{};
  for (std::size_t i = 0; i < kNpcs.size(); ++i) {
    Node *node = supervisor.getFromDef(kNpcs[i].def);
    if (!node) {
      std::cerr << "[nm_ecology] missing NPC DEF " << kNpcs[i].def << '\n';
      continue;
    }
    tracked[i] = {&kNpcs[i], node->getField("translation"), node->getField("rotation")};
  }

  struct TrackedRobot {
    Field *translation;
    Mobility mobility;
  };
  std::vector<TrackedRobot> tracked_robots;
  Node *root = supervisor.getRoot();
  Field *children = root ? root->getField("children") : nullptr;
  if (children) {
    for (int index = 0; index < children->getCount(); ++index) {
      Node *node = children->getMFNode(index);
      Field *name_field = node ? node->getField("name") : nullptr;
      if (!name_field)
        continue;
      const auto mobility = mobility_by_robot.find(name_field->getSFString());
      if (mobility == mobility_by_robot.end())
        continue;
      Field *translation = node->getField("translation");
      if (translation)
        tracked_robots.push_back({translation, mobility->second});
    }
  }

  const std::string state_path = env_or("NM_WEBOTS_RUNTIME_WORLD_FILE", "");
  const auto state_file = state_path.empty() ? std::filesystem::path{} : std::filesystem::path(state_path);
  const auto clock_file = state_path.empty() ? std::filesystem::path{} : std::filesystem::path(state_path + ".clock");
  const double elapsed_before_start = load_world_clock(clock_file);
  auto last_save_wall = std::chrono::steady_clock::now();
  double last_save_world = elapsed_before_start;
  auto next_save = last_save_wall + std::chrono::seconds(30);
  std::cout << "[nm_ecology] shared ecology supervisor ready; timestep=" << step_ms
            << "ms; state=" << (state_path.empty() ? "disabled" : state_path)
            << "; prior_elapsed=" << elapsed_before_start << "s\n";

  while (supervisor.step(step_ms) != -1) {
    const double t = elapsed_before_start + supervisor.getTime();
    // All robots and ecology use this one world clock. Real-time mode targets
    // wall-clock pacing; network inference never owns or advances a private clock.
    // A complete day-night cycle takes twelve world-clock minutes and a seasonal
    // cycle takes four hours.
    const double day = kTau * t / 720.0;
    const double season = kTau * t / 14400.0;
    if (light_intensity)
      light_intensity->setSFFloat(0.20 + 0.80 * (0.5 + 0.5 * std::sin(day - 1.1)));
    if (light_color) {
      const double warmth = 0.06 * (0.5 + 0.5 * std::sin(season));
      const double color[3]{1.0, 0.91 + warmth, 0.78 + warmth};
      light_color->setSFColor(color);
    }

    for (const auto &robot : tracked_robots) {
      const double *current = robot.translation->getSFVec3f();
      Position position{current[0], current[1], current[2]};
      if (nm_webots::constrain_position(position, robot.mobility, water_bounds))
        set_vec3(robot.translation, position.x, position.y, position.z);
    }

    for (const auto &entry : tracked) {
      if (!entry.motion || !entry.translation)
        continue;
      const auto &npc = *entry.motion;
      const double phase = npc.speed * t + npc.phase;
      const double x = npc.center_x + npc.radius_x * std::sin(phase) + 0.18 * std::sin(phase * 0.37);
      const double y = npc.center_y + npc.radius_y * std::sin(phase * 1.37 + 0.5);
      const double z = npc.base_z + npc.lift * (0.5 + 0.5 * std::sin(phase * 1.91));
      set_vec3(entry.translation, x, y, z);
      if (entry.rotation) {
        const double rotation[4]{0.0, 0.0, 1.0, phase + 1.5707963267948966};
        entry.rotation->setSFRotation(rotation);
      }
    }

    const auto save_wall = std::chrono::steady_clock::now();
    if (!state_file.empty() && save_wall >= next_save) {
      save_snapshot(supervisor, state_file);
      save_world_clock(clock_file, t);
      const double wall_seconds =
          std::chrono::duration<double>(save_wall - last_save_wall).count();
      const double world_seconds = t - last_save_world;
      const double pace = wall_seconds > 0.0 ? world_seconds / wall_seconds : 0.0;
      std::cout << "[nm_ecology] world clock pace sim_seconds=" << world_seconds
                << " wall_seconds=" << wall_seconds << " ratio=" << pace << "x"
                << std::endl;
      last_save_wall = save_wall;
      last_save_world = t;
      next_save = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    }
  }

  if (!state_file.empty()) {
    save_snapshot(supervisor, state_file);
    save_world_clock(clock_file, elapsed_before_start + supervisor.getTime());
  }
  return 0;
}
