# simulation_environment
Simulation environments and robot bridges for AARNN.

## Persistent shared Webots world

`webots_world/worlds/shared_fleet_neuroworld.wbt` is the shared C. elegans
habitat for the six `qc00`–`qc05` slots and two `sm00`–`sm01` slots. It reuses
the AARNN `DeviceMapper`, the existing 24-sensor/96-actuator robot profile, and
the habitat assets in `webots_world/protos/`. A supervisor animates wildlife,
changes the light cycle, and writes a recoverable world snapshot every 30
seconds. The systemd service runs independently of browser sessions.

Install Webots and run the shared world on `sm00` and `sm01` with Ansible:

```sh
cd ansible
ansible-playbook -i inventory/hosts.ini playbooks/shared_world.yml
```

The playbook installs Webots from Cyberbotics' signed APT repository, builds
the C++ robot and ecology controllers, enables the persistent world on `sm00`,
and keeps `sm01` prepared as the second stream host. The W3D socket is limited
to the cluster ingress networks. Sign in at
`https://neuralmimicry.ai/webots`; the page connects to the authenticated
`wss://webots.neuralmimicry.ai/stream` viewer stream.

Robot slots default to unbound so the world can be inspected before selecting
networks. To drive them from AARNN, set
`AARNN_WEBOTS_ACCESS_TOKEN` to a service token with `aarnn:use` and override
`simulation_environment_webots_network_bindings` with a `network_id` for each
robot key (`AARNN_QC00` through `AARNN_QC05`, `AARNN_SM00`, and `AARNN_SM01`).
The role checks that bound deployments have a token. Keep token values out of
version control.

The authenticated browser broker, world catalogue, robot bindings, controllers,
and Ansible runtime installation all live in this repository. The website's
`/webots` route and the Continuum Ansible playbook provide the sign-in and
deployment integration.
