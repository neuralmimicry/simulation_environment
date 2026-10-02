#pragma once

#include <celegans_muscle_response.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <regex>
#include <string>
#include <vector>

namespace nm_webots {

// The AARNN output channels represent body-wall muscles. The Webots spine is
// a separate physical mechanism, so decode those channels before applying
// motor targets. Time constants and gain come from the shared AARNN header.
class CelegansSpineEffectors {
 public:
  explicit CelegansSpineEffectors(float step_ms) : response_(step_ms) {
    spine_.fill(-1);
    for (auto &group : muscle_)
      group.fill(-1);
    for (auto &group : trace_)
      group.fill(0.0f);
    previous_.fill(0.5f);
  }

  void discover(const std::vector<std::string> &names) {
    const std::regex spine_re("^celegans_spine_([0-9]{2})$");
    const std::regex muscle_re("^celegans_o_[0-9]{3}_(MDL|MDR|MVL|MVR)([0-9]{2})$");
    const std::regex mvulva_re("^celegans_o_[0-9]{3}_MVULVA$");
    std::smatch match;
    int spine_count = 0;
    int muscle_count = 0;
    for (std::size_t index = 0; index < names.size(); ++index) {
      if (std::regex_match(names[index], match, spine_re)) {
        const int segment = std::stoi(match[1]);
        if (segment >= 1 && segment <= 24) {
          spine_[segment - 1] = static_cast<int>(index);
          ++spine_count;
        }
      } else if (std::regex_match(names[index], match, muscle_re)) {
        const int segment = std::stoi(match[2]);
        if (segment < 1 || segment > 24)
          continue;
        const std::string group = match[1];
        const int side = group == "MDL" ? 0 : group == "MDR" ? 1 : group == "MVL" ? 2 : 3;
        muscle_[side][segment - 1] = static_cast<int>(index);
        ++muscle_count;
      } else if (std::regex_match(names[index], mvulva_re)) {
        mvulva_index_ = static_cast<int>(index);
      }
    }
    active_ = spine_count >= 8 && muscle_count >= 24;
  }

  bool active() const { return active_; }

  void apply(std::vector<float> &commands) {
    if (!active_)
      return;
    const float mvulva = response_.contraction(
        mvulva_index_ >= 0 && static_cast<std::size_t>(mvulva_index_) < commands.size()
            ? commands[static_cast<std::size_t>(mvulva_index_)] : 0.5f,
        mvulva_trace_);
    for (int segment = 0; segment < 24; ++segment) {
      const int spine_index = spine_[segment];
      if (spine_index < 0 || static_cast<std::size_t>(spine_index) >= commands.size())
        continue;
      std::array<float, 4> contraction{};
      for (int side = 0; side < 4; ++side) {
        const int muscle_index = muscle_[side][segment];
        const float raw = muscle_index >= 0 &&
                                  static_cast<std::size_t>(muscle_index) < commands.size()
                              ? commands[static_cast<std::size_t>(muscle_index)]
                              : 0.5f;
        contraction[side] = response_.contraction(raw, trace_[side][segment]);
      }
      const float dorsal = 0.5f * (contraction[0] + contraction[1]);
      float ventral = 0.5f * (contraction[2] + contraction[3]);
      if (mvulva_index_ >= 0 && segment >= 9 && segment <= 14) {
        const float center_weight = std::max(0.0f,
            1.0f - std::fabs(segment - 11.5f) / 3.0f);
        ventral = (1.0f - 0.22f * center_weight) * ventral +
                  0.22f * center_weight * mvulva;
      }
      float drive = ventral - dorsal;
      if (segment <= 3)
        drive += 0.08f * (contraction[1] + contraction[3] -
                          contraction[0] - contraction[2]);
      if (segment < 5 || segment > 20)
        drive *= 0.72f;
      previous_[segment] = response_.smooth_spine(response_.target(drive),
                                                   previous_[segment]);
      commands[static_cast<std::size_t>(spine_index)] = previous_[segment];
    }
  }

 private:
  aarnn::webots::CelegansMuscleResponse response_;
  std::array<int, 24> spine_{};
  std::array<std::array<int, 24>, 4> muscle_{};
  std::array<std::array<float, 24>, 4> trace_{};
  std::array<float, 24> previous_{};
  int mvulva_index_ = -1;
  float mvulva_trace_ = 0.0f;
  bool active_ = false;
};

// The API supplies sparse, 0.5-neutral output channels. Project their active
// fraction to visible wing joints and a bounded fraction of hover lift. No
// force or flutter is synthesized when all committed output has expired.
class FlyEffectors {
 public:
  FlyEffectors() {
    for (auto &leg : legs_)
      leg.fill(-1);
    for (auto &leg : leg_values_)
      leg.fill(0.5f);
  }

  void discover(const std::vector<std::string> &names) {
    const std::regex leg_re("^leg_(left|right)_(front|mid|rear)_(coxa|femur|tibia|tarsus)$");
    std::smatch match;
    for (std::size_t index = 0; index < names.size(); ++index) {
      if (names[index].rfind("dros_o_", 0) == 0)
        outputs_.push_back(static_cast<int>(index));
      else if (names[index] == "wing_left_flap")
        left_ = static_cast<int>(index);
      else if (names[index] == "wing_right_flap")
        right_ = static_cast<int>(index);
      else if (std::regex_match(names[index], match, leg_re)) {
        const std::string side = match[1];
        const std::string position = match[2];
        const std::string joint = match[3];
        const int leg = (side == "right" ? 3 : 0) +
                        (position == "front" ? 0 : position == "mid" ? 1 : 2);
        const int part = joint == "coxa" ? 0 : joint == "femur" ? 1 :
                         joint == "tibia" ? 2 : 3;
        legs_[leg][part] = static_cast<int>(index);
      }
    }
  }

  bool active() const { return !outputs_.empty() && left_ >= 0 && right_ >= 0; }

  float apply(std::vector<float> &commands, float step_ms) {
    if (!active())
      return 0.0f;
    float drive_sum = 0.0f;
    for (int index : outputs_) {
      const float raw = commands[static_cast<std::size_t>(index)];
      if (std::isfinite(raw))
        drive_sum += std::fabs(std::clamp(raw, 0.0f, 1.0f) - 0.5f);
    }
    // A quarter of the available output channels can supply full hover. A
    // single channel still gives a bounded visible response.
    const float activity = std::clamp(
        drive_sum / (0.25f * std::max(1.0f, outputs_.size() * 0.25f)),
        0.0f, 1.0f);
    phase_ = std::fmod(phase_ + step_ms * 0.045f, 6.28318530718f);
    const float amplitude = 0.32f * activity;
    const float target_left = 0.5f + amplitude * std::sin(phase_);
    const float target_right = 0.5f - amplitude * std::sin(phase_);
    const float alpha = aarnn::webots::SimulatedMotorResponse::alpha(step_ms, 55.0f);
    left_value_ += alpha * (target_left - left_value_);
    right_value_ += alpha * (target_right - right_value_);
    commands[static_cast<std::size_t>(left_)] = left_value_;
    commands[static_cast<std::size_t>(right_)] = right_value_;
    constexpr float gains[4] = {0.52f, 0.45f, 0.48f, 0.56f};
    constexpr float limits[4][2] = {
        {0.20f, 0.80f}, {0.18f, 0.82f}, {0.12f, 0.88f}, {0.10f, 0.90f}};
    const float leg_alpha = aarnn::webots::SimulatedMotorResponse::alpha(step_ms, 130.0f);
    for (int leg = 0; leg < 6; ++leg) {
      for (int part = 0; part < 4; ++part) {
        const int index = legs_[leg][part];
        if (index < 0)
          continue;
        const float projected = project_pair(commands,
            leg * 7 + part * 11 + 5, leg * 13 + part * 3 + 19);
        const float target = std::clamp(0.5f + (projected - 0.5f) * gains[part],
                                        limits[part][0], limits[part][1]);
        leg_values_[leg][part] += leg_alpha * (target - leg_values_[leg][part]);
        commands[static_cast<std::size_t>(index)] = leg_values_[leg][part];
      }
    }
    return activity;
  }

 private:
  float project_pair(const std::vector<float> &commands, int seed_a, int seed_b) const {
    const auto count = static_cast<int>(outputs_.size());
    const auto safe = [&](int seed) {
      const float raw = commands[static_cast<std::size_t>(outputs_[seed % count])];
      return std::isfinite(raw) ? std::clamp(raw, 0.0f, 1.0f) : 0.5f;
    };
    const float a = safe(seed_a);
    const float b = safe(seed_b);
    return std::clamp(0.5f + (0.62f * a + 0.38f * b - 0.5f) * 0.78f, 0.0f, 1.0f);
  }

  std::vector<int> outputs_;
  std::array<std::array<int, 4>, 6> legs_{};
  std::array<std::array<float, 4>, 6> leg_values_{};
  int left_ = -1;
  int right_ = -1;
  float phase_ = 0.0f;
  float left_value_ = 0.5f;
  float right_value_ = 0.5f;
};

}  // namespace nm_webots
