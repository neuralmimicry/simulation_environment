#include <webots/Field.hpp>
#include <webots/Node.hpp>
#include <webots/Supervisor.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

using webots::Field;
using webots::Node;
using webots::Supervisor;

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

  const std::string state_path = env_or("NM_WEBOTS_RUNTIME_WORLD_FILE", "");
  const auto state_file = state_path.empty() ? std::filesystem::path{} : std::filesystem::path(state_path);
  const auto clock_file = state_path.empty() ? std::filesystem::path{} : std::filesystem::path(state_path + ".clock");
  const double elapsed_before_start = load_world_clock(clock_file);
  auto next_save = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  std::cout << "[nm_ecology] shared ecology supervisor ready; timestep=" << step_ms
            << "ms; state=" << (state_path.empty() ? "disabled" : state_path)
            << "; prior_elapsed=" << elapsed_before_start << "s\n";

  while (supervisor.step(step_ms) != -1) {
    const double t = elapsed_before_start + supervisor.getTime();
    // A complete day-night cycle takes twelve real-time minutes; seasonal light
    // and flora cycles take four hours. This keeps ecology changes visible while
    // leaving the shared simulator running at real-time pace.
    const double day = kTau * t / 720.0;
    const double season = kTau * t / 14400.0;
    if (light_intensity)
      light_intensity->setSFFloat(0.20 + 0.80 * (0.5 + 0.5 * std::sin(day - 1.1)));
    if (light_color) {
      const double warmth = 0.06 * (0.5 + 0.5 * std::sin(season));
      const double color[3]{1.0, 0.91 + warmth, 0.78 + warmth};
      light_color->setSFColor(color);
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

    if (!state_file.empty() && std::chrono::steady_clock::now() >= next_save) {
      save_snapshot(supervisor, state_file);
      save_world_clock(clock_file, t);
      next_save = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    }
  }

  if (!state_file.empty()) {
    save_snapshot(supervisor, state_file);
    save_world_clock(clock_file, elapsed_before_start + supervisor.getTime());
  }
  return 0;
}
