# simulation_environment
Simulation environments and robot bridges for AARNN.

## Persistent shared Webots world

`webots_world/worlds/shared_fleet_neuroworld.wbt` provides eight robot slots:
two Celegans (24/96), one Hexapod (34/18), two generic-profile (32/16) slots
using the existing Hexapod model with its camera channels excluded, a
Drosophila, a Zebrafish, and one amphibious Celegans slot. The site profile
binds only the logical networks listed below; a robot profile in the world
does not imply that an AARNN network is active.
Bindings follow network IDs rather than compute-host names, because each
network may be distributed across several `qc` and `sm` nodes.
The world reuses the AARNN `DeviceMapper` and habitat assets in
`webots_world/protos/`. A supervisor animates wildlife, changes the light cycle,
removes unbound robot slots from the physics scene, enforces the selected
profile's movement region, and writes a recoverable world snapshot every 30
seconds. Agar, orchard, terrain, and room features open onto one continuous
land surface. The freshwater stream has an open southern shore: amphibious
robots can cross it, swimming-only robots are constrained to its water volume,
land-only robots are returned to shore, and flight profiles are not clipped by
land or water boundaries. The region and profile contract is
`webots_world/configs/robot_capabilities.json`. Changing the configured fleet population
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
to the cluster ingress networks. The server runs Webots with `--no-rendering`
while keeping its W3D stream enabled; local profiling confirmed the stream
continues sending world updates, without rendering a duplicate 3D view on the
compute host. Sign in at
`https://neuralmimicry.ai/webots`; the page connects to the authenticated
`wss://webots.neuralmimicry.ai/stream` viewer stream.

Robot slots default to unbound so the world can be inspected before selecting
networks. The current NeuralMimicry site profile defines one generic 32/16
binding: `neuralmimicry-shared-snn`. Earlier Celegans and
Hexapod bindings are not active merely because their robot profiles remain in
the world. Add a binding only after verifying that its existing AARNN network
is available and publishes the required sensory I/O bridge. The service token
can be supplied via `AARNN_WEBOTS_ACCESS_TOKEN` or
`AARNN_WEBOTS_ACCESS_TOKEN_FILE`, which points at a
control-node secret file containing the Webots service token. It needs general
`aarnn:use` access and an active network-scoped peripheral-input grant in
`simulation_environment_webots_peripheral_input_grants`, as a list of
`{principal, brain_id}` objects; the site profile grants the `webots` service
principal only the shared SNN network ID listed above.
Keep the credential independent of a browser login. When its protected file is
rotated, each robot controller reloads it after an HTTP 401 and retries the
current request without restarting the shared world.
To change the fleet, override `simulation_environment_webots_network_bindings`
with the logical network ID, `robot_profile`, compatible device regexes, and
exact input/output counts for each robot key, then keep its AARNN grant mapping
in sync. Ansible checks each bound profile against
`robot_capabilities.json`; the world supervisor uses that same profile to
enforce its land, water, amphibious, or flight movement class. The
Hexapod camera is sampled at 1x1 resolution to keep its two event channels
aligned with the existing 34-channel AARNN profile. The C++
bridge converts normalized sensor readings to raw AER spike indices locally,
using the configurable `simulation_environment_webots_input_spike_threshold`
(default `0.5`). This avoids downloading full AARNN snapshots just to encode
each frame, which can exceed the API's 64 MiB shard snapshot limit.
The bridge sends virtual-world frames through `/api/simulation/aer/inject` and
reads motor activity from `/api/activity`. This server-managed route requires
the allow-listed `webots` service identity, `aarnn:use`, and the existing exact
network-scoped `PeripheralInput` grant. It is separate from workstation
`/api/aer/inject`, which still requires a locally consented short-lived
PeripheralSession. No browser session or local device is needed to keep the
Webots world running. Sensory frames omit a worker address so the orchestrator
can select the current sensory bridge and preserve cluster fan-out. Activity
reads also use API placement discovery instead of pinning a worker that may be
a backup shard or become stale after a placement change. Busy activity reads
are retryable and are not evidence that the network produced no output. Once
sensory admission succeeds, an unavailable activity projection does not turn
that accepted frame into an input failure or back off later frames; the
controller retains only its existing motor hold and checks activity on a later
sample. Each controller process uses a distinct `session_id`; its sensory
frame sequence and timestamp derive from the common Webots world clock,
restored from the persisted ecology clock after a simulator restart. This is
one wall-clock-paced world time domain shared by every robot and NPC, not a
per-network counter. A transient AARNN outage can expire an individual
unadmitted frame, but the controller keeps its session alive and resumes with
the newest frame from the shared world clock when the network route recovers.
`--mode=realtime` targets wall-clock pacing, but CPU saturation can make the
world lag; the supervisor logs the measured simulation-to-wall-clock rate with
each state save. Sensor timestamps remain tied to the shared world state at the
time of sampling. AARNN inference runs asynchronously per network, so different
calculation and communication latencies change when each action arrives
without changing or pausing the shared world clock. Do not retimestamp an older
sensor sample to its later host arrival time.
Treat sensory admission as separate from motor activity. The acceptance check
requires a bound robot's non-zero `input_spikes` frame to be admitted to its
configured network, then separately requires non-zero `output_spikes`, mapped
actuators, and a controller report that those outputs were applied to Webots
motors before claiming that robot has acted.
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

To inspect the live owner and rollout order without restarting a worker, run
the tagged preflight in check mode:

```sh
cd /home/pbisaacs/Developer/neuralmimicry/simulation_environment/ansible
ANSIBLE_CONFIG=./ansible.cfg ansible-playbook -i inventory/hosts.ini \
  playbooks/deploy_aarnn_sensory_workers.yml --tags rollout_order_preflight --check
```

The worker playbook checks that both local files match their declared
architectures, verifies the shared-SNN workspace snapshot exists, selects the
matching binary for each host, and requires each listed native service to be
active before replacement. It stores a root-owned rollback copy, waits for the
gRPC port, and confirms that each worker rejoined the orchestrator before
moving to the next host. Before rollout, it reads the live `/api/status` and
builds a temporary host order with inactive workers first, active non-ingress
workers next, and the current sensory ingress owner last. It checks placement
again before every worker restart and stops before touching a host if that
owner moves ahead in the sequence. The current inventory covers `qc02`–`qc04` and
`sm00`–`sm01`. In the 2026-09-30 host check, `qc00` did not accept SSH and
`qc05` was reachable but had no active `aarnn-node.service`; neither is
currently in the native worker rollout.
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
Apply `playbooks/shared_world.yml` after the AARNN ingress rollout succeeds:

```sh
cd /home/pbisaacs/Developer/neuralmimicry/simulation_environment/ansible
ANSIBLE_CONFIG=./ansible.cfg ansible-playbook -i inventory/hosts.ini \
  playbooks/shared_world.yml
```

That play targets `sm00` and `sm01`, installs the pinned OpenCV 4.6 video-I/O
runtime plus `libgpgme.so.11`, and keeps the shared Webots world on the
configured primary host after viewers disconnect. Both libraries were verified
installed on the two x86 hosts during the 2026-10-01 check.

After all three rollouts, verify every configured robot/network I/O path and
the unattended world clock:

```sh
cd /home/pbisaacs/Developer/neuralmimicry/simulation_environment/ansible
ANSIBLE_CONFIG=./ansible.cfg ansible-playbook -i inventory/hosts.ini \
  playbooks/verify_shared_world_io.yml
```

The check reports sensory admission and motor actuation independently for each
bound pair, checks the configured sensor/actuator dimensions, requires one
active world on `sm00` with `sm01` stopped, and confirms the persisted common
clock continues advancing.

The authenticated browser broker, world catalogue, robot bindings, controllers,
and Ansible runtime installation all live in this repository. The website's
`/webots` route and the Continuum Ansible playbook provide the sign-in and
deployment integration.

Browser launch sends the cached central bearer token when it is still present.
If it has expired or was cleared while the user remains signed in, the broker
also accepts the shared Customers session cookie for the handoff. Cookie-based
exchange is restricted to HTTPS requests originating from the commercial site
or the Webots host and is validated by the internal Customers `/api/session`
endpoint before the broker creates its own Webots session.
