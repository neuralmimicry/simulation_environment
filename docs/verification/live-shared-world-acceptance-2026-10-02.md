# Live shared-world acceptance — 2026-10-02

The serial native-worker rollout completed on `qc03`, `sm00`, `sm01`, `qc02`,
and `qc04`, with the current shared-SNN sensory ingress owner updated last.
Every updated `aarnn-node.service` returned active and rejoined the
orchestrator. The shared SNN remained playing during the rollout. The x86
hosts `sm00` and `sm01` also passed the runtime checks for `libgpgme.so.11` and
OpenCV 4.6 video I/O.

`verify_shared_world_io.yml` confirmed that `nm-webots-shared-world.service`
was active only on primary host `sm00`, that the `AARNN_NETWORK_SHARED_SNN`
robot connected with 32 sensory inputs and 16 actuators, and that the world
clock file existed. The separate 35-second clock probe advanced from
`142316.704` to `142346.336` seconds.

The live neural I/O gate failed. The Webots controller logged committed frames
with `input_spikes=12`, but the available activity projection repeatedly
reported `output_spikes=0` and `mapped_actuators=0`. The output source was
layer 2 on `native-sm00`, and that layer was assigned, but its sampled output
and output history were empty. The verification playbook consequently found
no `motor outputs applied` event. Sampled activity from the sensory owner
`native-qc04` showed no spikes at the query instants; the controller's HTTP
acknowledgement confirms frame admission to the sensory ingress mailbox, not
that a biological step consumed it.

The shared-SNN network status reported `playing=true`, with sensory layer 0 on
`native-qc04`, hidden layer 1 on `native-qc03`, and output layer 2 on
`native-sm00`. `tenant-aarnn` was left untouched. No claim of end-to-end
sensory-to-motor operation is supported by this run. Keep services running
until neural output and motor application are observed and the full acceptance
playbook passes; do not treat the paired `qc00`/`qc01` power-cycle as ready.
