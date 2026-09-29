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
control-node secret file containing a Webots service token with `aarnn:use`
permission, then override `simulation_environment_webots_network_bindings` with the logical
network ID, compatible device regexes, and exact input/output counts for each
robot key. The Hexapod camera is sampled at 1x1 resolution to keep its two
event channels aligned with the existing 34-channel AARNN profile. The C++
bridge converts normalized sensor readings to raw AER spike indices locally,
using the configurable `simulation_environment_webots_input_spike_threshold`
(default `0.5`). This avoids downloading full AARNN snapshots just to encode
each frame, which can exceed the API's 64 MiB shard snapshot limit.
The role checks that bound deployments have a token and installs it with
owner-only permissions on both fleet hosts. Keep token values out of version
control.

The authenticated browser broker, world catalogue, robot bindings, controllers,
and Ansible runtime installation all live in this repository. The website's
`/webots` route and the Continuum Ansible playbook provide the sign-in and
deployment integration.
