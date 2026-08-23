#!/usr/bin/env python3
"""Create the patched Teeworlds source tree used by the ROM build."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile


class PrepareError(RuntimeError):
    pass


def run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    process = subprocess.run(command, check=False, **kwargs)
    if process.returncode != 0:
        raise PrepareError(
            f"command failed ({process.returncode}): {' '.join(command)}"
        )
    return process


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--teeworlds", required=True)
    parser.add_argument("--patch", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    teeworlds = os.path.abspath(args.teeworlds)
    patch = os.path.abspath(args.patch)
    output = os.path.abspath(args.output)
    if not os.path.isfile(os.path.join(teeworlds, "CMakeLists.txt")):
        raise PrepareError(f"Teeworlds submodule is not initialized: {teeworlds}")
    if not os.path.isfile(patch):
        raise PrepareError(f"missing target patch: {patch}")

    revision = run(
        ["git", "-C", teeworlds, "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
    ).stdout.strip()
    with open(patch, "rb") as handle:
        patch_digest = hashlib.sha256(handle.read()).hexdigest()
    identity = {"teeworlds": revision, "patch_sha256": patch_digest}
    stamp = os.path.join(output, ".tw64-source.json")
    try:
        with open(stamp, encoding="utf-8") as handle:
            if json.load(handle) == identity:
                return 0
    except (FileNotFoundError, OSError, ValueError):
        pass

    parent = os.path.dirname(output)
    os.makedirs(parent, exist_ok=True)
    temporary = tempfile.mkdtemp(prefix="teeworlds-source-", dir=parent)
    try:
        archive = run(
            ["git", "-C", teeworlds, "archive", "--format=tar", "HEAD", "src"],
            capture_output=True,
        ).stdout
        with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as tar:
            tar.extractall(temporary, filter="data")
        run(["patch", "--batch", "--forward", "-p1", "-i", patch], cwd=temporary)
        with open(
            os.path.join(temporary, ".tw64-source.json"), "w", encoding="utf-8"
        ) as handle:
            json.dump(identity, handle, indent=2, sort_keys=True)
            handle.write("\n")
        if os.path.exists(output):
            shutil.rmtree(output)
        os.replace(temporary, output)
    finally:
        if os.path.exists(temporary):
            shutil.rmtree(temporary)

    print(f"prepared Teeworlds {revision[:12]} with target patch")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except PrepareError as error:
        print(f"prepare_teeworlds: {error}", file=sys.stderr)
        sys.exit(1)
