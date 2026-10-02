#!/usr/bin/env python3
"""Check Webots assets copied from the adjacent authoritative AARNN source."""

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COPIED_FILES = (
    "scripts/robot_profiles.py",
    "sim/content/catalog.json",
    "webots_world/configs/config_celegans_webots.json",
    "webots_world/configs/config_celegans_webots.io_alignment.json",
    "webots_world/configs/config_drosophila_webots.json",
    "webots_world/configs/config_drosophila_webots.io_alignment.json",
    "webots_world/configs/config_drosophila_banc_webots.json",
    "webots_world/configs/config_drosophila_banc_webots.io_alignment.json",
    "webots_world/configs/config_drosophila_fafb_webots.json",
    "webots_world/configs/config_drosophila_fafb_webots.io_alignment.json",
    "webots_world/configs/config_hexapod_webots.json",
    "webots_world/configs/config_hexapod_webots.io_alignment.json",
    "webots_world/configs/config_nao_webots.json",
    "webots_world/configs/config_zebrafish_webots.json",
    "webots_world/configs/config_zebrafish_webots.io_alignment.json",
    "webots_world/controllers/nao_nn_controller_uds/nao_nn_controller_uds.cpp",
    "webots_world/protos/CelegansRobot.proto",
    "webots_world/protos/DrosophilaRobot.proto",
    "webots_world/protos/DrosophilaBancRobot.proto",
    "webots_world/protos/DrosophilaFafbRobot.proto",
    "webots_world/protos/HexapodRobot.proto",
    "webots_world/protos/ZebrafishRobot.proto",
    "webots_world/worlds/celegans_neuroworld.wbt",
    "webots_world/worlds/drosophila_neuroworld.wbt",
    "webots_world/worlds/hexapod_neuroworld.wbt",
    "webots_world/worlds/neuroworld.wbt",
    "webots_world/worlds/zebrafish_neuroworld.wbt",
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--aarnn-root", type=Path, default=ROOT.parent / "aarnn_rust",
        help="Path to the authoritative AARNN checkout",
    )
    args = parser.parse_args()
    source_root = args.aarnn_root.resolve()
    if not (source_root / "scripts/regenerate_simulator_assets.py").is_file():
        parser.error(f"AARNN source checkout not found at {source_root}")

    stale = []
    for relative in COPIED_FILES:
        source = source_root / relative
        target = ROOT / relative
        if not source.is_file() or not target.is_file():
            stale.append(relative)
        elif relative.endswith(".cpp"):
            # The upstream controller contains a few trailing spaces that
            # should not be copied into this repository's review diff.
            normalized = lambda path: [line.rstrip() for line in path.read_text().splitlines()]
            if normalized(source) != normalized(target):
                stale.append(relative)
        elif source.read_bytes() != target.read_bytes():
            stale.append(relative)
    if stale:
        raise SystemExit("AARNN Webots asset parity mismatch:\n  " + "\n  ".join(stale))
    print(f"AARNN Webots source parity verified ({len(COPIED_FILES)} files)")


if __name__ == "__main__":
    main()
