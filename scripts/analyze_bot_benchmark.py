#!/usr/bin/env python3
"""Summarize guest-timed TW64 BOT_BENCH markers from a Gopher64 log."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

MARKER = "TW64 BOT_BENCH "
FIELD = re.compile(r"([a-z_]+)=([0-9]+)")
EXPECTED_BOTS = [1, 3, 5, 7, 9, 11, 13, 15]


def parse_log(path: Path) -> list[dict[str, int | float | bool]]:
    rows: list[dict[str, int | float | bool]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if MARKER not in line:
            continue
        values = {key: int(value) for key, value in FIELD.findall(line)}
        required = {
            "actors",
            "bots",
            "fps_milli",
            "sim_avg_us",
            "sim_max_us",
            "render_avg_us",
            "render_max_us",
            "frames",
            "window_ticks",
            "dropped",
        }
        missing = required - values.keys()
        if missing:
            raise ValueError(f"incomplete marker, missing {sorted(missing)}: {line}")
        rows.append(
            {
                **values,
                "fps": values["fps_milli"] / 1000.0,
                "high_fps": values["fps_milli"] >= 50_000
                and values["sim_max_us"] <= 20_000
                and values["dropped"] == 0,
            }
        )
    if not rows:
        raise ValueError(f"no {MARKER.strip()} markers in {path}")
    observed = [int(row["bots"]) for row in rows]
    if observed != EXPECTED_BOTS:
        raise ValueError(
            f"incomplete or reordered benchmark sweep: expected {EXPECTED_BOTS}, "
            f"observed {observed}"
        )
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--json", action="store_true", help="emit JSON")
    parser.add_argument(
        "--require-bots",
        type=int,
        help="fail unless at least this many bots meet the high-FPS definition",
    )
    args = parser.parse_args()
    rows = parse_log(args.log)
    passing = [row for row in rows if row["high_fps"]]
    result = {
        "schema": 1,
        "source": str(args.log),
        "definition": {
            "minimum_fps": 50.0,
            "maximum_sim_tick_us": 20_000,
            "maximum_dropped_frames": 0,
        },
        "maximum_high_fps_bots": max((int(row["bots"]) for row in passing), default=0),
        "rows": rows,
    }
    if args.json:
        print(json.dumps(result, indent=2))
        return int(
            args.require_bots is not None
            and result["maximum_high_fps_bots"] < args.require_bots
        )

    print("bots actors fps sim_avg_us sim_max_us dropped high_fps")
    for row in rows:
        print(
            f"{row['bots']:>4} {row['actors']:>6} {row['fps']:>5.1f} "
            f"{row['sim_avg_us']:>10} {row['sim_max_us']:>10} "
            f"{row['dropped']:>7} {'yes' if row['high_fps'] else 'no'}"
        )
    print(f"maximum high-FPS bot count: {result['maximum_high_fps_bots']}")
    if (
        args.require_bots is not None
        and result["maximum_high_fps_bots"] < args.require_bots
    ):
        print(
            f"benchmark failed: requires {args.require_bots} high-FPS bots",
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
