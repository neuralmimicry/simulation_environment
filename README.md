# simulation_environment
Simulation environments and robot bridges for AARNN.

## Persistent shared Webots world

`webots_world/worlds/shared_fleet_neuroworld.wbt` hosts one embodied robot for
each active logical AARNN network: two Celegans (24/96), one Hexapod (34/18),
and two generic-profile networks (32/16) using the existing Hexapod model with
its camera channels excluded. The other three world slots remain available for
future networks. Bindings follow network IDs rather than compute-host names,
because each network may be distributed across several `qc` and `sm` nodes.
The world reuses the AARNN `DeviceMapper` and habitat assets in
`webots_world/protos/`. A supervisor animates wildlife, changes the light cycle,
and writes a recoverable world snapshot every 30 seconds. The systemd service
runs independently of browser sessions and reloads the source world when its
world or PROTO revision changes while preserving the ecological clock.

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
networks. The NeuralMimicry site profile binds the five currently active
logical networks (`celegans_01`, `celegans_02`, `hexapod_01`,
`neuralmimicry-shared-snn`, and `tenant-aarnn`) with their verified I/O
dimensions. To change them, set
`AARNN_WEBOTS_ACCESS_TOKEN` or point `AARNN_WEBOTS_ACCESS_TOKEN_FILE` at a
control-node secret file containing the Webots service token. It needs general
`aarnn:use` access and an active network-scoped peripheral-input grant in
`simulation_environment_webots_peripheral_input_grants`; the site profile
grants the `webots` service principal only the five network IDs listed above.
To change the fleet, override `simulation_environment_webots_network_bindings`
with the logical network ID, compatible device regexes, and exact input/output
counts for each robot key, then keep its AARNN grant mapping in sync. The Hexapod camera is sampled at 1x1 resolution to keep its two
event channels aligned with the existing 34-channel AARNN profile. The C++
bridge converts normalized sensor readings to raw AER spike indices locally,
using the configurable `simulation_environment_webots_input_spike_threshold`
(default `0.5`). This avoids downloading full AARNN snapshots just to encode
each frame, which can exceed the API's 64 MiB shard snapshot limit.
The bridge sends frames through `/api/aer/inject` and reads motor activity from
`/api/activity`. Sensory frames omit a worker address so the orchestrator can
select the current sensory bridge and preserve cluster fan-out. Activity reads
also use API placement discovery instead of pinning a worker that may be a
backup shard or become stale after a placement change. The `/api/aer/infer`
route waits for fresh output on each request, so the persistent controller
keeps input admission and activity polling separate. Each controller process
uses a distinct `session_id` with its monotonically increasing Webots step so
retries remain idempotent and a restarted simulation cannot reuse an earlier
frame identity.
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
  cargo build --locked --release --bin aarnn_rust --features node_workload
```

Build the AArch64 artifact natively on an AArch64 host (the active `qc02`–
`qc04` workers use this architecture):

```sh
cd "$AARNN_RUST_SRC"
CARGO_TARGET_DIR=/path/to/aarch64-target \
  cargo build --locked --release --target aarch64-unknown-linux-gnu \
    --bin aarnn_rust --features node_workload
```

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
current inventory covers `qc02`–`qc04` and `sm00`–`sm01`, which participate in
the five-network placement. It excludes offline `qc00` and `qc05`, and the
separate Kubernetes `aarnn-engine` workload on `qc01`.

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
