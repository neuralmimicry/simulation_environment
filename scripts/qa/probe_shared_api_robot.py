#!/usr/bin/env python3
"""Probe one Webots API robot against a local, bounded neural activity stub.

Run one --kind per process. The output is synthetic and proves controller,
sensor, and motor plumbing only; it does not prove biological output.
"""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from threading import Thread

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from sim_content import compile_catalog  # noqa: E402

PROFILES = {
    "celegans": ("CelegansRobot", "C_ELEGANS_01", 24, 96, "celegans"),
    "drosophila-banc": ("DrosophilaBancRobot", "DROS_BANC_01", 418, 48, "dros"),
    "drosophila-fafb": ("DrosophilaFafbRobot", "DROS_FAFB_01", 418, 48, "dros"),
    "hexapod": ("HexapodRobot", "HEXAPOD_01", 34, 18, "hex"),
    "zebrafish": ("ZebrafishRobot", "ZEBRAFISH_01", 32, 32, "zebrafish"),
    "nao": (None, "NAO_01", 250, 40, "nao"),
}

FINISH_CONTROLLER = '''from controller import Supervisor
import json, math, os
from pathlib import Path
s = Supervisor()
name = os.environ['NM_API_PROBE_ROBOT']
scale = int(os.environ['NM_API_PROBE_STEP_SCALE'])
children = s.getRoot().getField('children')
def robot_node():
    for i in range(children.getCount()):
        node = children.getMFNode(i)
        field = node.getField('name')
        if field and field.getSFString() == name:
            return node
    return None
def spine_positions():
    node = robot_node()
    if not node:
        return []
    field = node.getBaseNodeField('children')
    joint = next((field.getMFNode(i) for i in range(field.getCount())
                  if field.getMFNode(i).getBaseTypeName() == 'HingeJoint'), None)
    positions = []
    while joint:
        end = joint.getField('endPoint').getSFNode()
        if not end:
            break
        positions.append(end.getPosition())
        nested = end.getField('children')
        joint = next((nested.getMFNode(i) for i in range(nested.getCount())
                      if nested.getMFNode(i).getBaseTypeName() == 'HingeJoint'), None)
    return positions
samples = {}
for step in range(90 * scale):
    if s.step(int(s.getBasicTimeStep())) == -1:
        break
    if step in (7 * scale, 55 * scale, 88 * scale):
        node = robot_node()
        samples[str(step // scale)] = {
            'height_m': node.getPosition()[2] if node else None,
            'spine': spine_positions() if os.environ['NM_API_PROBE_KIND'] in
                      ('celegans', 'zebrafish') else []}
Path(os.environ['NM_API_PROBE_REPORT']).write_text(json.dumps(samples))
s.simulationQuit(0)
'''


def output_indices(kind, count):
    if kind != "celegans":
        return list(range(0, count // 2)) if kind.startswith("drosophila") else list(range(0, count, 2))
    profile = next(p for p in compile_catalog()["profiles"] if p["id"] == "celegans")
    return [index for index, name in enumerate(profile["output_names"])
            if re.search(r"_(?:MVL|MVR)\d{2}$", name)]


def run(kind):
    proto, robot_name, sensory_count, output_count, prefix = PROFILES[kind]
    driven_indices = output_indices(kind, output_count)
    assert driven_indices
    result_root = ROOT / "target/qa/shared-api-robot"
    result_root.mkdir(parents=True, exist_ok=True)
    result_dir = Path(tempfile.mkdtemp(prefix=f"{kind}-", dir=result_root))

    class Handler(BaseHTTPRequestHandler):
        latest_step = 0
        admitted = 0
        input_spikes = 0
        reads = 0
        capture_times = []
        sequences = []

        def log_message(self, *_args):
            pass

        def reply(self, payload):
            body = json.dumps(payload).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            if self.path != "/api/simulation/aer/inject":
                self.send_error(404)
                return
            payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if payload.get("network_id") != "synthetic-local-probe":
                self.send_error(422)
                return
            Handler.latest_step = max(Handler.latest_step, int(payload["step_index"]))
            Handler.admitted += 1
            Handler.input_spikes += len(payload["spike_indices"])
            Handler.capture_times.append(float(payload["time_ms"]))
            Handler.sequences.append(int(payload["step_index"]))
            self.reply({"ok": True})

        def do_GET(self):
            if not self.path.startswith("/api/activity?network_id=synthetic-local-probe"):
                self.send_error(404)
                return
            Handler.reads += 1
            step = Handler.latest_step
            self.reply({"sim_step": step,
                        "output": {"indices": driven_indices if 12 <= step < 65 else []}})

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server_thread = Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="nm-webots-api-") as project_str:
            project = Path(project_str)
            worlds = project / "worlds"
            worlds.mkdir()
            controllers = project / "controllers"
            controllers.mkdir()
            (controllers / "nm_api_robot_controller").symlink_to(
                ROOT / "webots_world/controllers/nm_api_robot_controller", target_is_directory=True)
            finish = controllers / "probe_finish"
            finish.mkdir()
            (finish / "probe_finish.py").write_text(FINISH_CONTROLLER)

            world = worlds / "probe.wbt"
            brain = kind + "_api_probe"
            command = [sys.executable, str(ROOT / "scripts/build_webots_multi_world.py"),
                       "--world", str(world), f"--{kind}-brains", brain]
            if proto:
                command += [f"--{kind}-proto", str(ROOT / f"webots_world/protos/{proto}.proto")]
            subprocess.run(command, cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
            world.write_text(world.read_text().replace(
                'controller "nao_nn_controller_uds"', 'controller "nm_api_robot_controller"').replace(
                'controller "nm_world_recorder"', 'controller "probe_finish"'))

            token = project / "token"
            token.write_text("synthetic-local-probe-token")
            config = project / "fleet.json"
            config.write_text(json.dumps({
                "api_base": f"http://127.0.0.1:{server.server_port}/api",
                "access_token_file": str(token),
                "inference_interval_ms": 100,
                "output_hold_ms": 250,
                "robots": {robot_name: {
                    "network_id": "synthetic-local-probe",
                    "sensor_regex": ".*" if kind == "nao" else f"^{prefix}_s_.*$",
                    "actuator_regex": ".*" if kind == "nao" else f"^{prefix}_o_.*$",
                    "expected_sensory": sensory_count,
                    "expected_outputs": output_count,
                }},
            }))
            report_path = project / "posture.json"
            env = dict(os.environ,
                       NM_WEBOTS_FLEET_CONFIG=str(config),
                       NM_API_PROBE_REPORT=str(report_path),
                       NM_API_PROBE_KIND=kind,
                       NM_API_PROBE_ROBOT=robot_name,
                       NM_API_PROBE_STEP_SCALE="4" if kind.startswith("drosophila") else "1",
                       NM_WEBOTS_MOTOR_DEBUG_INTERVAL="10",
                       NM_CAMERA_RETINA_WIDTH="8", NM_CAMERA_RETINA_HEIGHT="6",
                       NM_DROS_CAMERA_RETINA_WIDTH="12", NM_DROS_CAMERA_RETINA_HEIGHT="8",
                       NM_CAMERA_RETINA_WIDTH_HEX_S_26_HEAD_CAMERA="1",
                       NM_CAMERA_RETINA_HEIGHT_HEX_S_26_HEAD_CAMERA="1",
                       NM_CAMERA_RETINA_WIDTH_ZEBRAFISH_S_16_EYE_LEFT="1",
                       NM_CAMERA_RETINA_HEIGHT_ZEBRAFISH_S_16_EYE_LEFT="1",
                       NM_CAMERA_RETINA_WIDTH_ZEBRAFISH_S_18_EYE_RIGHT="1",
                       NM_CAMERA_RETINA_HEIGHT_ZEBRAFISH_S_18_EYE_RIGHT="1")
            log_path = result_dir / "webots.log"
            with log_path.open("w") as output:
                completed = subprocess.run(
                    ["webots", "--batch", "--mode=realtime", "--no-rendering",
                     "--stdout", "--stderr", str(world)],
                    cwd=ROOT, env=env, stdout=output, stderr=subprocess.STDOUT,
                    timeout=60, check=False)
            feedback = json.loads(report_path.read_text()) if report_path.exists() else {}
    finally:
        server.shutdown()
        server.server_close()
        server_thread.join(timeout=1)

    log = log_path.read_text()
    assert completed.returncode == 0, log_path
    assert Handler.admitted >= 10 and Handler.reads >= 10 and Handler.input_spikes > 0, log_path
    assert "motor outputs applied" in log, log_path
    motor_rows = re.findall(
        r"\[DeviceMapper\] motor diag robot=" + re.escape(robot_name) +
        r" active=(\d+)/(\d+) max_target_fraction=([\d.e+-]+) target_mismatches=(\d+)",
        log)
    assert motor_rows and any(int(active) > 0 and float(fraction) > 0.01 and
                              int(mismatches) == 0
                              for active, _, fraction, mismatches in motor_rows), log_path
    assert Handler.sequences == sorted(set(Handler.sequences)), log_path
    assert Handler.capture_times == sorted(Handler.capture_times), log_path
    moved = None
    if kind in ("celegans", "zebrafish"):
        before, after = feedback["7"]["spine"], feedback["55"]["spine"]
        assert len(before) >= 4 and len(before) == len(after), (kind, feedback)
        moved = sum(any(abs((a[i] - before[0][i]) - (b[i] - after[0][i])) > 1e-4
                        for i in range(3)) for a, b in zip(before, after))
        assert moved >= 4, (kind, moved, log_path)
    if kind.startswith("drosophila"):
        assert feedback["55"]["height_m"] > feedback["7"]["height_m"] + 0.02, feedback
        assert feedback["88"]["height_m"] < feedback["55"]["height_m"] - 0.02, feedback
    (result_dir / "posture.json").write_text(json.dumps(feedback, indent=2) + "\n")
    result = {"kind": kind, "robot": robot_name, "admitted_frames": Handler.admitted,
              "input_spikes": Handler.input_spikes, "activity_reads": Handler.reads,
              "first_sequence": Handler.sequences[0], "last_sequence": Handler.sequences[-1],
              "max_physical_motors_active": max(int(row[0]) for row in motor_rows),
              "segments_moved": moved,
              "height_m": {step: sample["height_m"] for step, sample in feedback.items()},
              "log": str(log_path)}
    (result_dir / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=PROFILES, required=True,
                        help="one robot profile per Webots process")
    args = parser.parse_args()
    run(args.kind)


if __name__ == "__main__":
    main()
