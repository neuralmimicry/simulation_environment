# Align Webots robot and environment behavior with AARNN

This is a living implementation record for the simulation_environment checkout.

## Purpose and observable outcome

The shared and isolated Webots worlds use the current AARNN robot geometry and sensor names. Committed neural output produces the corresponding physical motor or flight response, and fish encounter physical water under normal gravity. The shared world's fleet, open shore, API ingress, and independent ecological clock remain available.

## Authority and boundary

The AARNN `docs/specifications/distributed-whole-brain-emulator-v1.1.md` governs peripheral input provenance and committed output (`INV-015`, `INV-016`). The relevant implementation record is AARNN `docs/execplans/phase-08-workstation-io.md`. This repository owns the persistent Webots scene and API controller; AARNN owns the generated single-robot PROTOs and reusable C++ response headers. This change does not alter neural time, network state, grants, or checkpoint formats.

## Repository orientation

- `simulation_environment` root: `/home/pbisaacs/Developer/neuralmimicry/simulation_environment`; branch `codex/webots-shared-world-20260929` at `b6d080e`; clean before this task.
- AARNN root: `/home/pbisaacs/Developer/neuralmimicry/aarnn_rust`; `main` at `19fb394`; clean before this task.
- AARNN generators: `scripts/build_webots_celegans_assets.py`, `scripts/build_webots_drosophila_assets.py`, `scripts/build_webots_zebrafish_assets.py`, `scripts/build_webots_multi_world.py`, and `scripts/regenerate_simulator_assets.py`.
- This repository's shared habitat source is `scripts/sim_content.py` plus `sim/content/catalog.json`; its derived PROTOs are under `webots_world/protos/`. The shared fleet scene is authored in `webots_world/worlds/shared_fleet_neuroworld.wbt`.
- Persistent controller: `webots_world/controllers/nm_api_robot_controller/nm_api_robot_controller.cpp`; local UDS controller: `webots_world/controllers/nao_nn_controller_uds/nao_nn_controller_uds.cpp`. Both use AARNN's `include/device_mapper.hpp`; response headers also remain owned by AARNN.
- Deployment source synchronization and controller compilation are in `ansible/roles/webots_shared_world/tasks/main.yml`. No live deployment is part of this task.

## Milestones and progress

- [x] `2026-10-02 20:03Z` Compared tracked robot PROTOs, worlds, config, controllers, generators and Ansible deployment. The AARNN robot assets had drifted, while this repository's shared-world camera, stream opening, ecology supervisor and HTTP ingress are intentional extensions.
- [x] `2026-10-02 20:08Z` Copied the five changed robot PROTOs, updated the local UDS controller and standalone fly/fish worlds, and added `scripts/check_aarnn_webots_parity.py`; its 27-file check passes. Re-generated the two tracked mixed-world fixtures from the local generator, retaining its level camera and open-land habitat policy.
- [x] `2026-10-02 20:11Z` Added AARNN-header-based C. elegans spine response and committed-output-based fly wing/leg/lift mapping to the shared API controller. The shared scene has 9.81 m/s² gravity, an 8 ms fly collision step and a named physical freshwater volume aligned with capability bounds. The controller preserves the previously deployed 32 ms world-clock source-sequence mapping across this physics-step change. Ansible installs the response headers before building the API controller.
- [x] `2026-10-02 20:14Z` Source, build and isolated checks passed; evidence below. No live fleet service was restarted or deployed.
- [x] `2026-10-02 20:18Z` Added C. elegans MVULVA blending and AARNN-style neural projection to fly leg joints, recompiled the API controller and reran the C. elegans, BANC and FAFB one-robot API probes. Each passed physical motor diagnostics with zero target mismatches; the other three profiles use unchanged direct-output mapping and their earlier one-robot API probes remain valid. Final parity, habitat-generation, Ansible syntax and diff checks passed.

## Validation and rollback

Evidence from this checkout on x86_64 Linux with Webots R2025a:

- `python3 scripts/check_aarnn_webots_parity.py`: 27 copied source/asset files aligned (the local controller comparison ignores trailing whitespace only).
- `python3 scripts/sim_content.py --check --webots-only`: all five tracked habitat PROTOs match this repository's generator. The broader `--check` reports five untracked web/Unity/Unreal/Minecraft compiled-content outputs absent from this repository; `--webots-only` is the relevant supported check here.
- AARNN `python3 scripts/regenerate_simulator_assets.py --check`: passed at its source checkout.
- From this repository's root, `g++ -std=c++17 -O2 -I/home/pbisaacs/Developer/neuralmimicry/aarnn_rust/include webots_world/controllers/nm_api_robot_controller/tests.cpp -o /tmp/nm_api_robot_controller_tests && /tmp/nm_api_robot_controller_tests`: passed, including neutral/ventral muscle, wing and leg-response cases. Both Webots C++ controllers built with `make -B -C webots_world/controllers/<controller> -j1 AARNN_INCLUDE_ROOT=/home/pbisaacs/Developer/neuralmimicry/aarnn_rust/include`. `ANSIBLE_CONFIG=./ansible.cfg ansible-playbook --syntax-check -i inventory/hosts.ini playbooks/shared_world.yml` passed from `ansible/`.
- Adapted AARNN isolated neural-actuator probe, invoked one `--kind` at a time against this checkout: C. elegans, BANC and FAFB fly, hexapod, zebrafish and NAO all passed sensory-width/activity and motor-command checks. C. elegans moved 23/24 sampled spine points; each fly rose to about 0.316 m and returned near the floor; zebrafish moved 4/5 sampled spine points and stayed at 0.188–0.217 m depth.
- `scripts/qa/probe_shared_api_robot.py --kind <one-kind>` passed in six separate processes for those same profiles. Each local HTTP stub admitted 24–62 sensory frames with nonzero input spikes, returned mapped output and logged motor application. C. elegans moved 23/24 spine points; both flies rose to about 0.316 m and descended after output stopped; zebrafish moved four spine points in water. The NAO received motor commands but toppled under the synthetic half-channel pattern, so gait stability is not established.
- `git diff --check`: passed. No eight-robot live world was launched.

Do not use the eight-robot persistent scene as a probe. Rollback is the source diff in this repository; no live service or persisted world was changed by editing it. Deployment and a live committed-neural-output acceptance check remain separate operations.

## Discoveries and decisions

- The shared world's stream has an intentionally open southern shore and a site-specific habitat naming scheme. Wholesale replacement with AARNN's generated habitat PROTOs would remove these features.
- Its existing `nm_stream_water_volume` is visual only. A separate named Webots `Fluid` is required for the fish's `ImmersionProperties` to act under normal gravity.
- The API controller's current `0.75` output command addresses muscle channels but does not synthesize C. elegans spine targets or apply fly lift. Both adapters must be driven only by committed, currently held output spikes.
- The simulated-muscle time constants and gain come from AARNN `include/celegans_muscle_response.hpp` (55 ms muscle, 80 ms spine, 3× neural gain). Fly lift follows the AARNN controller's approximate 14 g articulated-rig model, with a 0.20 N cap and 0.32 m height target; the shared sparse-API projection reaches full hover when roughly one quarter of its output channels are active. These are bounded heuristics validated here by Webots movement and recovery, not measured animal biomechanics or proof of stable gait.
- Changing the shared physics step from 32 to 8 ms would otherwise multiply `step_index` by four after a persistent-world restart. A fixed 32 ms source sequence quantum preserves the pre-existing capture mapping while `time_ms` continues to record the actual Webots capture instant.

## Outcomes

The two repositories now agree on copied robot assets, sensory/output labels, the local UDS controller, physical gravity/flight/water behavior, and one-robot isolated response checks. The persistent shared-world additions remain source-only until an explicitly scheduled deployment; the live network-to-motor acceptance gate reported in `docs/verification/live-shared-world-acceptance-2026-10-02.md` is still open.
