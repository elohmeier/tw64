#!/usr/bin/env python3
"""Verify the pinned host reference and its deterministic observation seam."""

from __future__ import annotations

import json
import os
import subprocess
import sys

EXPECTED_HASH = "5b97d4ea22d5a7d8"


def run_once(executable: str) -> dict:
    executable = os.path.abspath(executable)
    command = [
        executable,
        "--bot-a",
        "hunter140",
        "--bot-b",
        "hunter119",
        "--map",
        "dm6",
        "--seed",
        "1164458525508301476",
        "--scramble-bot",
        "wander",
        "--warmup-ticks",
        "1827",
        "--ticks",
        "4725",
    ]
    process = subprocess.run(
        command,
        cwd=os.path.dirname(os.path.abspath(executable)),
        capture_output=True,
        text=True,
        check=False,
    )
    if process.returncode != 0:
        sys.stderr.write(process.stdout)
        sys.stderr.write(process.stderr)
        raise RuntimeError(f"botbench exited with {process.returncode}")
    return json.loads(process.stdout)


def deterministic_projection(result: dict) -> dict:
    projected = dict(result)
    projected.pop("elapsed_ms", None)
    projected.pop("ticks_per_second", None)
    return projected


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} BOTBENCH", file=sys.stderr)
        return 2
    first = run_once(sys.argv[1])
    second = run_once(sys.argv[1])
    if first.get("schema") != 15:
        raise RuntimeError(f"unexpected schema: {first.get('schema')}")
    if first.get("state_hash") != EXPECTED_HASH:
        raise RuntimeError(
            f"state hash {first.get('state_hash')} != expected {EXPECTED_HASH}"
        )
    if not first.get("observation_invariants_passed"):
        raise RuntimeError("observation invariants failed")
    if deterministic_projection(first) != deterministic_projection(second):
        raise RuntimeError("determinism replay differs")
    print(
        f"host verification passed: schema 15, hash {EXPECTED_HASH}, "
        "observation invariants and replay deterministic"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        print(f"verify_host: {error}", file=sys.stderr)
        sys.exit(1)
