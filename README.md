# simulation_environment
Simulation environments and robot bridges for AARNN.

## Persistent shared Webots world

`webots_world/worlds/shared_fleet_neuroworld.wbt` provides eight robot slots:
two Celegans (24/96), one Hexapod (34/18), two generic-profile (32/16) slots
using the existing Hexapod model with its camera channels excluded, and three
future slots. The site profile binds only the logical networks listed below;
a robot profile in the world does not imply that an AARNN network is active.
Bindings follow network IDs rather than compute-host names, because each
network may be distributed across several `qc` and `sm` nodes.
The world reuses the AARNN `DeviceMapper` and habitat assets in
`webots_world/protos/`. A supervisor animates wildlife, changes the light cycle,
removes unbound robot slots from the physics scene, and writes a recoverable
world snapshot every 30 seconds. Changing the configured fleet population
reloads the source scene while preserving the ecological clock; robot poses
return to their authored starting positions. The systemd service runs
independently of browser sessions.

Install Webots and run the shared world on `sm00` and `sm01` with Ansible:

```sh
cd ansible
ANSIBLE_CONFIG=./ansible.cfg ansible-playbook -i inventory/hosts.ini playbooks/shared_world.yml
```

The playbook installs Webots from Cyberbotics' signed APT repository, builds
the C++ robot and ecology controllers, enables the persistent world on `sm00`,
and keeps `sm01` prepared as the second stream host. The W3D socket is limited
to the cluster ingress networks. Sign in at
`https://neuralmimicry.ai/webots`; the page connects to the authenticated
`wss://webots.neuralmimicry.ai/stream` viewer stream.

Robot slots default to unbound so the world can be inspected before selecting
networks. The current NeuralMimicry site profile defines two generic 32/16
bindings: `neuralmimicry-shared-snn` and `tenant-aarnn`. Earlier Celegans and
Hexapod bindings are not active merely because their robot profiles remain in
the world. Add a binding only after verifying that its existing AARNN network
is available and publishes the required sensory I/O bridge. The service token
can be supplied via `AARNN_WEBOTS_ACCESS_TOKEN` or
`AARNN_WEBOTS_ACCESS_TOKEN_FILE`, which points at a
control-node secret file containing the Webots service token. It needs general
`aarnn:use` access and an active network-scoped peripheral-input grant in
`simulation_environment_webots_peripheral_input_grants`; the site profile
grants the `webots` service principal only the two network IDs listed above.
Keep the credential independent of a browser login. When its protected file is
rotated, each robot controller reloads it after an HTTP 401 and retries the
current request without restarting the shared world.
To change the fleet, override `simulation_environment_webots_network_bindings`
with the logical network ID, compatible device regexes, and exact input/output
counts for each robot key, then keep its AARNN grant mapping in sync. The
Hexapod camera is sampled at 1x1 resolution to keep its two event channels
aligned with the existing 34-channel AARNN profile. The C++
bridge converts normalized sensor readings to raw AER spike indices locally,
using the configurable `simulation_environment_webots_input_spike_threshold`
(default `0.5`). This avoids downloading full AARNN snapshots just to encode
each frame, which can exceed the API's 64 MiB shard snapshot limit.
The bridge sends frames through `/api/aer/inject` and reads motor activity from
`/api/activity`. Sensory frames omit a worker address so the orchestrator can
select the current sensory bridge and preserve cluster fan-out. Activity reads
also use API placement discovery instead of pinning a worker that may be a
backup shard or become stale after a placement change. Busy activity reads
are retryable and are not evidence that the network produced no output. Each
controller process uses a distinct `session_id`; its sensory frame sequence
and timestamp derive from the common Webots world clock, restored from the
persisted ecology clock after a simulator restart. This is one wall-clock-paced
world time domain shared by every robot and NPC, not a per-network counter.
`--mode=realtime` targets wall-clock pacing, but CPU saturation can make the
world lag; the supervisor logs the measured simulation-to-wall-clock rate with
each state save. Sensor timestamps remain tied to the shared world state at the
time of sampling. AARNN inference runs asynchronously per network, so different
calculation and communication latencies change when each action arrives
without changing or pausing the shared world clock. Do not retimestamp an older
sensor sample to its later host arrival time.
Treat sensory admission as separate from motor activity: verify a successful
controller report with non-zero `output_spikes` and `mapped_actuators` before
claiming a robot has acted.
The role checks that bound deployments have a token and installs it with
owner-only permissions on both fleet hosts. Keep token values out of version
control.

Before enabling those robot bindings, build and import the AARNN branch images
specified in `ansible/vars/neuralmimicry-site.yml` on the QC workers. The
bounded sensory Prepare/Commit RPCs also need to be installed on the native
workers that may own a network's sensory layer. Build one release executable
for each active worker architecture from an `aarnn_rust` worktree checked out to
`codex/webots-api-ingress-20260929`. Build the x86-64 artifact on an x86-64
host:

```sh
AARNN_RUST_SRC=/path/to/aarnn_rust-webots-api-ingress-worktree
cd "$AARNN_RUST_SRC"
CARGO_TARGET_DIR=/path/to/x86_64-target \
  cargo build --locked --release --bin aarnn_rust --features node_workload,cuda
```

Build the AArch64 artifact natively on an AArch64 host (the active `qc02`–
`qc04` workers use this architecture):

```sh
AARNN_RUST_SRC=/path/to/aarnn_rust-webots-api-ingress-worktree
cd "$AARNN_RUST_SRC"
CARGO_TARGET_DIR=/path/to/aarch64-target \
  cargo build --locked --release --target aarch64-unknown-linux-gnu \
    --bin aarnn_rust --features node_workload
```

The x86-64 artifact is shared by `sm00` and `sm01`, both of which have RTX
3060 GPUs. Include the `cuda` feature or the worker will report that CUDA was
not compiled and run neural computation on the CPU. The ARM workers currently
use the CPU/OpenCL profile. After installation, check the worker journal for a
`[compute.cuda]` or `[compute.opencl] backend_initialized=1` line and verify
GPU use with `nvidia-smi` on the native hosts.
A created CUDA context does not prove every compute stage ran on the GPU; also
check for repeated `CPU_reference_fallback` transitions and confirm AARNN
process utilisation.

Pass both executable paths to the serial worker rollout:

```sh
cd /home/pbisaacs/Developer/neuralmimicry/simulation_environment/ansible
AARNN_NODE_BINARY_X86_64=/path/to/x86_64-target/release/aarnn_rust \
AARNN_NODE_BINARY_AARCH64=/path/to/aarch64-target/aarch64-unknown-linux-gnu/release/aarnn_rust \
  ANSIBLE_CONFIG=./ansible.cfg ansible-playbook -i inventory/hosts.ini playbooks/deploy_aarnn_sensory_workers.yml
```

The worker playbook checks that both local files match their declared
architectures, selects the matching binary for each host, and requires each
listed native service to be active before replacement. It stores a root-owned
rollback copy and waits for the gRPC port after each serial restart. The
current inventory covers `qc02`–`qc04` and `sm00`–`sm01`. In the 2026-09-30
host check, `qc00` did not accept SSH and `qc05` was reachable but had no
active `aarnn-node.service`; neither is currently in the native worker rollout.
The separate Kubernetes `aarnn-engine` workload on `qc01` is managed through
its DaemonSet rather than this native-worker playbook.

Then run the scoped API ingress rollout:

```sh
cd ansible
ANSIBLE_CONFIG=./ansible.cfg ansible-playbook -i inventory/hosts.ini playbooks/deploy_aarnn_webots_ingress.yml
```

That playbook provisions a separate root-only orchestrator bearer on Spirit,
stores it as a Kubernetes Secret, patches `aarnn-orchestrator` and
`aarnn-web-ui` with the source-built images and network-scoped grants, and
updates the `aarnn-engine` DaemonSet so Kubernetes-hosted bridges implement the
same Prepare/Commit RPCs. The control API and FPV workloads remain unchanged.
The Web UI reports the authenticated caller's scopes at
`/api/peripheral/input-grants`; revoke a scope by removing its principal/network
pair and rerunning the playbook.
Apply `playbooks/shared_world.yml` after the AARNN ingress rollout succeeds.

The authenticated browser broker, world catalogue, robot bindings, controllers,
and Ansible runtime installation all live in this repository. The website's
`/webots` route and the Continuum Ansible playbook provide the sign-in and
deployment integration.
