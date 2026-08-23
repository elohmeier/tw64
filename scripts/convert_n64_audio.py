#!/usr/bin/env python3
"""Offline Teeworlds -> Nintendo 64 sound converter.

Turns the desktop client's authoritative audio (``datasrc/audio/*.wv``,
WavPack) into libdragon ``.wav64`` waveforms for the Teeworlds 64 ROM
filesystem, the same way ``scripts/convert_n64_assets.py`` turns the desktop
art into ``.sprite`` assets.

Nothing here is hand-listed against a guess. The sound sets and their file
lists are read out of ``datasrc/content.py`` -- the same file the desktop build
generates ``SOUND_*`` from -- and the derived enum order is cross-checked
against the generated ``generated/protocol.h`` before anything is
converted. A mismatch is a hard error, because a silently shifted enum would
play the wrong sample for every event on target.

Format, and why:

* **22050 Hz mono.** The N64 AI is initialised at the same rate, so the RSP
  mixer copies each voice at 1:1 instead of resampling it. Halving the desktop
  44.1/48 kHz rate halves the ROM cost for material that is almost entirely
  short, noisy, band-limited foley.
* **VADPCM with Huffman disabled** (``--wav-compress vadpcm,huffman=false``).
  Plain VADPCM is 9 bytes per 16 samples and is decoded *by the RSP* inside
  the mixer ucode, so a playing voice costs the CPU nothing beyond the DMA
  setup. libdragon's default Huffman layer is ~10% smaller but is unpacked by
  ``huffv_decompress()`` on the VR4300, and this port has no CPU headroom to
  spend on audio. Roughly 4x smaller than raw PCM for zero CPU is the right
  trade here.
* **Streamed from ROM** (the renderer opens them with ``wav64_open``), so the
  waveform data never enters the heap: only the mixer's own bounded sample
  buffers do.

One variation per set. The desktop client picks a random file out of each set
and refuses to repeat the previous one; the port is deterministic and always
takes index 0 of the set's file list (``-01`` where a set is numbered). This is
recorded in the stamp file so the choice is auditable rather than implicit.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

# --------------------------------------------------------------------------
# What the port actually plays
# --------------------------------------------------------------------------
#
# Every name here is a sound set in datasrc/content.py, i.e. SOUND_<NAME
# uppercased> in the generated protocol. The list is deliberately a subset:
# the port ships the sounds its presentation layer can actually trigger from
# authoritative server state, and nothing else.
#
# Group 1 -- emitted by CGameContext::CreateSound(), i.e. read straight out of
# the tick's NETEVENTTYPE_SOUNDWORLD events.
SERVER_SETS = [
    "gun_fire",
    "shotgun_fire",
    "grenade_fire",
    "hammer_fire",
    "ninja_fire",
    "grenade_explode",
    "ninja_hit",
    "laser_fire",
    "laser_bounce",
    "weapon_switch",
    "player_pain_short",
    "player_pain_long",
    "player_die",
    "pickup_health",
    "pickup_armor",
    "pickup_grenade",
    "pickup_shotgun",
    "pickup_ninja",
    "weapon_spawn",
    "weapon_noammo",
    "hit",
]

# Group 2 -- the desktop client derives these from snapshot events, netobjects
# or core events rather than from a server sound; the shell derives them from
# exactly the same authoritative state.
CLIENT_SETS = [
    "hammer_hit",  # NETEVENTTYPE_HAMMERHIT
    "player_spawn",  # NETEVENTTYPE_SPAWN
    "player_jump",  # COREEVENTFLAG_GROUND_JUMP
    "player_airjump",  # COREEVENTFLAG_AIR_JUMP
    "hook_attach_ground",  # COREEVENTFLAG_HOOK_ATTACH_GROUND
    "hook_attach_player",  # COREEVENTFLAG_HOOK_ATTACH_PLAYER
    "hook_noattach",  # COREEVENTFLAG_HOOK_HIT_NOHOOK
    "ctf_grab_pl",  # flag stats delta, grabber on a local team
    "ctf_grab_en",  # flag stats delta, grabber on the other team
    "ctf_capture",
    "ctf_return",
    "ctf_drop",
    "chat_client",  # menu: move the selection
    "chat_highlight",  # menu: confirm a page
]

SHIPPED_SETS = SERVER_SETS + CLIENT_SETS

# Deliberately not shipped, with the reason, so the gap is a decision and not
# an oversight. Kept next to the list above because the two are read together.
SKIPPED_SETS = {
    "hook_loop": "looping voice; needs per-character channel ownership",
    "body_land": "the desktop client never plays it either (0.7)",
    "player_skid": "derived from render-side skid detection, which the port has no equivalent of",
    "tee_cry": "emote driven; the port has no emotes",
    "chat_server": "the port has no chat",
    "menu": "25 s of menu music; the port ships no music",
}

SAMPLE_RATE = 22050
COMPRESSION = "vadpcm,huffman=false"


class ConvertError(Exception):
    pass


# --------------------------------------------------------------------------
# content.py / protocol.h
# --------------------------------------------------------------------------

SOUNDSET_RE = re.compile(
    r"container\.sounds\.Add\(SoundSet\(\"([a-z0-9_]+)\"\s*,\s*(.+?)\)\)\s*$"
)
FILELIST_RE = re.compile(r"FileList\(\"([^\"]+)\"\s*,\s*(\d+)\)")
LITERAL_RE = re.compile(r"\"(audio/[^\"]+)\"")


def parse_sound_sets(content_py: str) -> list:
    """The ordered (name, [files]) sound sets, exactly as content.py adds them.

    The order is the enum order: datasrc/compile.py emits
    ``SOUND_<name.upper()>`` for each item of ``container.sounds.items`` in
    append order, so index i here is SOUND_* value i.
    """
    sets = []
    with open(content_py, encoding="utf-8") as handle:
        for line in handle:
            match = SOUNDSET_RE.search(line.strip())
            if not match:
                continue
            name, spec = match.group(1), match.group(2)
            files = []
            for fmt, num in FILELIST_RE.findall(spec):
                files += [fmt % (i + 1) for i in range(int(num))]
            if not files:
                files = LITERAL_RE.findall(spec)
            if not files:
                raise ConvertError(f"could not parse the file list of sound set {name}")
            sets.append((name, files))
    if not sets:
        raise ConvertError(f"no sound sets found in {content_py}")
    return sets


def parse_sound_enum(protocol_h: str) -> list:
    """The SOUND_* enum out of the generated protocol header, in order."""
    with open(protocol_h, encoding="utf-8") as handle:
        text = handle.read()
    match = re.search(r"\bSOUND_GUN_FIRE=0,(.*?)NUM_SOUNDS", text, re.DOTALL)
    if not match:
        return []
    names = ["SOUND_GUN_FIRE"]
    for line in match.group(1).splitlines():
        line = line.strip().rstrip(",")
        if line.startswith("SOUND_"):
            names.append(line)
    return names


def verify_enum(sets: list, protocol_h: str) -> str:
    """Cross-check content.py order against the generated enum.

    Returns a short human-readable status; raises on a real mismatch. The
    check is skipped (and says so) when the generated header has not been
    staged yet, because ``make assets`` can legitimately run before the
    host build has produced it.
    """
    if not os.path.exists(protocol_h):
        return "skipped (no staged protocol.h)"
    names = parse_sound_enum(protocol_h)
    if not names:
        return "skipped (no SOUND_ enum in protocol.h)"
    expected = [f"SOUND_{name.upper()}" for name, _files in sets]
    if names != expected:
        for index, (got, want) in enumerate(zip(names, expected)):
            if got != want:
                raise ConvertError(
                    f"sound enum mismatch at index {index}: "
                    f"protocol.h says {got}, content.py says {want}"
                )
        raise ConvertError(
            f"sound enum length mismatch: protocol.h {len(names)}, "
            f"content.py {len(expected)}"
        )
    return f"ok ({len(names)} sets)"


# --------------------------------------------------------------------------
# Conversion
# --------------------------------------------------------------------------


def decode_to_wav(src: str, dst: str, ffmpeg: str):
    """WavPack -> 22050 Hz mono signed 16-bit PCM, the audioconv64 input."""
    cmd = [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        src,
        "-ac",
        "1",
        "-ar",
        str(SAMPLE_RATE),
        "-acodec",
        "pcm_s16le",
        "-y",
        dst,
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise ConvertError(f"ffmpeg failed for {src}")


def run_audioconv(wavs: list, repo_root: str, out_dir: str, docker: str, image: str):
    """Convert every staged WAV with libdragon's audioconv64, in one container
    start, inside the toolchain image the ROM itself is built with."""
    rel_out = os.path.relpath(out_dir, repo_root)
    rel = [
        os.path.join("/workdir", os.path.relpath(p, repo_root)) for p in sorted(wavs)
    ]
    cmd = [
        docker,
        "run",
        "--rm",
        "-v",
        f"{repo_root}:/workdir:z",
        "-w",
        "/workdir",
        image,
        "audioconv64",
        "--wav-mono",
        "--wav-compress",
        COMPRESSION,
        "--output",
        os.path.join("/workdir", rel_out),
    ] + rel
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise ConvertError("audioconv64 failed")


def prune_stale(out_dir: str, wanted: set):
    for name in sorted(os.listdir(out_dir)):
        if name.endswith(".wav64") and name not in wanted:
            os.remove(os.path.join(out_dir, name))


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------


def source_fingerprint(paths: list) -> str:
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update(path.encode())
        with open(path, "rb") as handle:
            digest.update(hashlib.sha256(handle.read()).digest())
    return digest.hexdigest()


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    project_default = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser.add_argument("--project-root", default=project_default)
    parser.add_argument(
        "--teeworlds-root", default=None, help="pinned Teeworlds source submodule"
    )
    parser.add_argument("--out", default=None, help="ROM filesystem sfx directory")
    parser.add_argument("--scratch", default=None, help="intermediate WAV directory")
    parser.add_argument("--docker", default=os.environ.get("DOCKER", "podman"))
    parser.add_argument("--image", default="tw64-toolchain:local")
    parser.add_argument("--ffmpeg", default=os.environ.get("FFMPEG", "ffmpeg"))
    parser.add_argument(
        "--force", action="store_true", help="ignore the up-to-date stamp"
    )
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    project = os.path.abspath(args.project_root)
    teeworlds = os.path.abspath(
        args.teeworlds_root or os.path.join(project, "teeworlds")
    )
    datasrc = os.path.join(teeworlds, "datasrc")
    audio_dir = os.path.join(datasrc, "audio")
    out_dir = args.out or os.path.join(project, "filesystem", "sfx")
    scratch = args.scratch or os.path.join(project, "build", "audio")
    verbose = not args.quiet
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(scratch, exist_ok=True)

    content_py = os.path.join(datasrc, "content.py")
    protocol_h = os.path.join(project, "generated", "protocol.h")
    sets = parse_sound_sets(content_py)
    by_name = dict(sets)
    index_of = {name: i for i, (name, _f) in enumerate(sets)}

    unknown = [s for s in SHIPPED_SETS if s not in by_name]
    if unknown:
        raise ConvertError("unknown sound sets: " + ", ".join(unknown))

    chosen = [(name, by_name[name][0]) for name in SHIPPED_SETS]
    inputs = [os.path.abspath(__file__), content_py]
    inputs += [os.path.join(datasrc, rel) for _name, rel in chosen]
    missing = [p for p in inputs if not os.path.exists(p)]
    if missing:
        raise ConvertError("missing converter inputs: " + ", ".join(missing))

    enum_status = verify_enum(sets, protocol_h)

    stamp_path = os.path.join(out_dir, "audio.json")
    fingerprint = source_fingerprint(inputs)
    if not args.force and os.path.exists(stamp_path):
        try:
            with open(stamp_path, encoding="utf-8") as handle:
                if json.load(handle).get("fingerprint") == fingerprint:
                    if verbose:
                        print(f"n64 audio up to date (enum check {enum_status})")
                    return 0
        except (OSError, ValueError):
            pass

    if verbose:
        print(f"converting n64 audio from {audio_dir} (enum check {enum_status})")

    wavs = []
    for name, rel in chosen:
        wav = os.path.join(scratch, f"{name}.wav")
        decode_to_wav(os.path.join(datasrc, rel), wav, args.ffmpeg)
        wavs.append(wav)

    run_audioconv(wavs, project, out_dir, args.docker, args.image)

    sizes = {}
    total = 0
    for name, _rel in chosen:
        path = os.path.join(out_dir, f"{name}.wav64")
        if not os.path.exists(path):
            raise ConvertError(f"audioconv64 produced no output for {name}")
        size = os.path.getsize(path)
        sizes[f"{name}.wav64"] = size
        total += size
    sizes["__total__"] = total
    prune_stale(out_dir, {f"{name}.wav64" for name, _rel in chosen})

    with open(stamp_path, "w", encoding="utf-8") as handle:
        json.dump(
            {
                "fingerprint": fingerprint,
                "sample_rate": SAMPLE_RATE,
                "channels": 1,
                "compression": COMPRESSION,
                "variation": "index 0 of each set (deterministic)",
                "sounds": {
                    name: {
                        "enum": f"SOUND_{name.upper()}",
                        "value": index_of[name],
                        "source": rel,
                        "variations_in_set": len(by_name[name]),
                    }
                    for name, rel in chosen
                },
                "skipped": SKIPPED_SETS,
                "enum_check": enum_status,
                "bytes": sizes,
            },
            handle,
            indent=1,
            sort_keys=True,
        )

    if verbose:
        print(
            f"wrote {len(chosen)} waveforms ({SAMPLE_RATE} Hz mono, {COMPRESSION}) "
            f"= {total} bytes into {out_dir}"
        )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ConvertError as err:
        sys.stderr.write(f"convert_n64_audio: {err}\n")
        sys.exit(1)
