#!/usr/bin/env python3
"""Offline Teeworlds -> Nintendo 64 graphics converter.

Turns the desktop client's authoritative art (``datasrc/game.png``, the 0.7
tee part PNGs and the ``datasrc/mapres`` tilesets) into libdragon ``.sprite``
assets for the Teeworlds 64 ROM filesystem.

Nothing here is hand-cropped. Sprite rectangles are read out of
``datasrc/content.py`` -- the same file the desktop build generates its sprite
table from -- and tileset tiles are cut on the 16x16 grid the client's
``TEXLOAD_ARRAY_256`` upload path uses. The set of tilesets to convert is
derived by parsing the shipped ``.map`` datafiles, so an unreferenced or
missing image is a hard error instead of a silent gap on target.

Three asset classes, three deliberate formats:

* ``gfx/spr_*.sprite`` -- RGBA16 cut-outs of ``game.png`` (weapons, projectiles,
  hook, pickups, flags, HUD icons). Small, individually colourful, never
  tinted, and RGBA16's single alpha bit is enough at 8-35 screen pixels.
* ``gfx/tee_*.sprite`` -- IA8 tee parts. The 0.7 parts are greyscale plus alpha
  by construction, which is exactly what IA8 stores, and it lets the renderer
  reproduce the desktop colourisation (``texel_rgb * color_rgb``) with a single
  RDP primitive-colour modulate instead of shipping one texture per player
  colour. The body composes outline + body + shadow + upper outline into one
  sprite, and the foot composes outline + foot; the intensity channel keeps the
  outline black, so tinting a composed part is still the desktop result.
* ``gfx/ui_*.sprite`` -- the menu's own art: the official ``gui_logo.png`` and
  the sky backdrop layers (clouds and mountains) the desktop menu themes are
  built from. The logo is RGBA32 because it is the one asset whose soft glow
  and anti-aliased edges do not survive RGBA16's single alpha bit; the backdrop
  layers are flat-shaded vector art with hard edges and stay RGBA16. Nothing is
  recoloured here -- the menu dims the backdrop through the RDP primitive
  colour, so the shipped texels stay the desktop artwork.
* ``gfx/mapimg_*.sprite`` -- CI8 tileset sheets, 16x16 tiles of 16x16 pixels
  (256x256 per sheet, 64 KiB + a 256-entry RGBA16 palette). A palette costs
  half of RGBA16 for the same tile count and TMEM only ever holds one 16x16
  tile at a time. Each tileset also gets a ``_r`` sheet with every tile rotated
  90 degrees clockwise, because the RDP's texture rectangle cannot transpose
  the S/T axes; the renderer loads it lazily, only for maps that actually use
  ``TILEFLAG_ROTATE``.

The PNG slicing runs on the host with Pillow; the PNG -> ``.sprite``
conversion runs ``mksprite`` inside the libdragon toolchain container, the same
way the project ``Makefile`` runs the ROM build.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import zlib
from dataclasses import dataclass, field

from PIL import Image

# --------------------------------------------------------------------------
# Sizing model
# --------------------------------------------------------------------------
#
# The renderer maps world units to pixels with TW64_WORLD_SCALE = 0.375, so a
# 32-unit tile is 12 screen pixels and a tee (64 units of *visual* size, 28 of
# physical size) is 24. Every target size below is that on-screen size rounded
# up to a convenient texel count, which keeps a little supersampling headroom
# without ever exceeding the RDP's 4 KiB TMEM for a single sprite.

TILE_PIXELS = 16  # per tileset tile, drawn at 12 screen pixels
TILES_PER_ROW = 16  # the client's fixed 16x16 tileset grid

# name in content.py -> (output basename, width, height)
# Widths are multiples of 4 so an RGBA16 TMEM line stays 8-byte aligned.
GAME_SPRITES = {
    # weapons held in hand; PC visual_size 64..96 -> 21..35 screen pixels
    "weapon_hammer_body": ("weapon_hammer", 32, 24),
    "weapon_gun_body": ("weapon_gun", 32, 16),
    "weapon_shotgun_body": ("weapon_shotgun", 48, 12),
    "weapon_grenade_body": ("weapon_grenade", 44, 12),
    "weapon_laser_body": ("weapon_laser", 44, 20),
    "weapon_ninja_body": ("weapon_ninja", 48, 12),
    # projectiles; PC draws them at 32 units -> ~9 screen pixels
    "weapon_gun_proj": ("proj_gun", 12, 12),
    "weapon_shotgun_proj": ("proj_shotgun", 12, 12),
    "weapon_grenade_proj": ("proj_grenade", 16, 16),
    "weapon_laser_proj": ("proj_laser", 12, 12),
    # hook
    "hook_chain": ("hook_chain", 8, 8),
    "hook_head": ("hook_head", 16, 8),
    # pickups; PC draws health/armor at 64 units -> 17 screen pixels
    "pickup_health": ("pickup_health", 20, 20),
    "pickup_armor": ("pickup_armor", 20, 20),
    # flags; PC draws them at 42 units -> 7x14 screen pixels
    "flag_red": ("flag_red", 16, 32),
    "flag_blue": ("flag_blue", 16, 32),
    # HUD icons, ten of each per row
    "health_full": ("hud_health_full", 8, 8),
    "health_empty": ("hud_health_empty", 8, 8),
    "armor_full": ("hud_armor_full", 8, 8),
    "armor_empty": ("hud_armor_empty", 8, 8),
    # muzzle flashes
    "weapon_gun_muzzle1": ("muzzle_gun1", 24, 16),
    "weapon_gun_muzzle2": ("muzzle_gun2", 24, 16),
    "weapon_gun_muzzle3": ("muzzle_gun3", 24, 16),
    "weapon_shotgun_muzzle1": ("muzzle_shotgun1", 24, 16),
    "weapon_shotgun_muzzle2": ("muzzle_shotgun2", 24, 16),
    "weapon_shotgun_muzzle3": ("muzzle_shotgun3", 24, 16),
}

# Tee parts are composed from several cells of one PNG; see the module docstring.
# (output basename, source png, grid, [cells composited bottom to top], w, h)
TEE_BODY_CELLS = [
    "tee_body_outline",
    "tee_body",
    "tee_body_shadow",
    "tee_body_upper_outline",
]
TEE_FOOT_CELLS = ["tee_foot_outline", "tee_foot"]

TEE_PARTS = [
    ("tee_body", "skins/body/standard.png", TEE_BODY_CELLS, 32, 32),
    ("tee_foot", "skins/feet/standard.png", TEE_FOOT_CELLS, 16, 16),
    ("tee_eyes", "skins/eyes/standard.png", ["tee_eyes_normal"], 24, 12),
]

# The three parts are packed into one 56x32 IA8 atlas (1792 bytes, comfortably
# inside the RDP's 4 KiB TMEM) so drawing a whole tee costs one TMEM upload
# instead of three. The renderer hard-codes these rectangles; keep the two in
# sync. Layout: body at (0,0), foot at (32,0), eyes at (32,16).
TEE_ATLAS_W = 56
TEE_ATLAS_H = 32
TEE_ATLAS_PLACEMENT = {"tee_body": (0, 0), "tee_foot": (32, 0), "tee_eyes": (32, 16)}

# Menu art. (output basename, source png under datasrc/, width, height, format,
# crop to the opaque bounding box first). Sizes are the on-screen sizes the menu
# draws them at on a 320x240 framebuffer, so the RDP never rescales them:
#
# * the logo at 256x64 spans four fifths of the screen width and still leaves
#   the four menu pages their eight rows;
# * the two clouds keep the aspect of their own opaque box (2000x852 and
#   590x484) to within a pixel;
# * the mountain strip is drawn at 320x160, i.e. the 160x80 texture doubled --
#   a silhouette dimmed to a third of its brightness has no detail that a 2x
#   upscale can lose, and halving it keeps the TMEM traffic of a full-width
#   backdrop layer down.
UI_SPRITES = [
    ("ui_logo", "ui/gui_logo.png", 256, 64, "RGBA32", False),
    ("ui_cloud1", "mapres/bg_cloud1.png", 112, 48, "RGBA16", True),
    ("ui_cloud2", "mapres/bg_cloud3.png", 80, 64, "RGBA16", True),
    ("ui_mountains", "mapres/mountains.png", 160, 80, "RGBA16", False),
]

MAPITEMTYPE_IMAGE = 2
MAPITEMTYPE_GROUP = 4
MAPITEMTYPE_LAYER = 5

LAYERTYPE_TILES = 2
TILESLAYERFLAG_GAME = 1
TILEFLAG_ROTATE = 8

# Every map staged into the ROM filesystem; must match ROM_MAPS in Makefile
# and s_aMaps[] in src/game/tw_game.cpp.
ROM_MAPS = [
    "dm1",
    "dm2",
    "dm3",
    "dm6",
    "dm7",
    "dm8",
    "dm9",
    "lms1",
    "ctf1",
    "ctf2",
    "ctf3",
    "ctf4",
    "ctf5",
    "ctf6",
    "ctf7",
    "ctf8",
]


class ConvertError(RuntimeError):
    """Any condition that must fail the build loudly rather than truncate."""


# --------------------------------------------------------------------------
# content.py sprite table
# --------------------------------------------------------------------------


@dataclass
class SpriteSet:
    name: str
    image: str
    gridx: int
    gridy: int


@dataclass
class SpriteDef:
    name: str
    setname: str
    x: int
    y: int
    w: int
    h: int


@dataclass
class Content:
    images: dict = field(default_factory=dict)  # variable name -> filename
    sets: dict = field(default_factory=dict)  # variable name -> SpriteSet
    sprites: dict = field(default_factory=dict)  # sprite name -> SpriteDef


_RE_IMAGE = re.compile(
    r'^\s*(\w+)\s*=\s*Image\(\s*"([^"]*)"\s*,\s*"([^"]*)"', re.MULTILINE
)
_RE_SET = re.compile(
    r'^\s*(\w+)\s*=\s*SpriteSet\(\s*"([^"]*)"\s*,\s*(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)',
    re.MULTILINE,
)
_RE_SPRITE = re.compile(
    r'Sprite\(\s*"([^"]+)"\s*,\s*(\w+)\s*,\s*'
    r"(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\)"
)


def parse_content(path: str) -> Content:
    """Read the sprite grid definitions straight out of ``datasrc/content.py``.

    Regex rather than ``import`` on purpose: content.py pulls in datatypes.py
    and builds the whole desktop content container, and this converter only
    needs the four literal tables that define where a sprite lives.
    """
    with open(path, encoding="utf-8") as handle:
        src = handle.read()
    out = Content()
    for var, _name, filename in _RE_IMAGE.findall(src):
        out.images[var] = filename
    for var, name, image, gx, gy in _RE_SET.findall(src):
        out.sets[var] = SpriteSet(name, image, int(gx), int(gy))
    for name, setvar, x, y, w, h in _RE_SPRITE.findall(src):
        out.sprites[name] = SpriteDef(name, setvar, int(x), int(y), int(w), int(h))
    if not out.sprites:
        raise ConvertError(f"no Sprite() definitions found in {path}")
    return out


def sprite_box(content: Content, name: str, img_w: int, img_h: int):
    """Pixel rectangle of a named sprite inside its sprite set's image."""
    spr = content.sprites.get(name)
    if spr is None:
        raise ConvertError(f"sprite '{name}' is not defined in content.py")
    sset = content.sets.get(spr.setname)
    if sset is None:
        raise ConvertError(f"sprite '{name}' references unknown set '{spr.setname}'")
    if img_w % sset.gridx or img_h % sset.gridy:
        raise ConvertError(
            f"image for '{name}' is {img_w}x{img_h}, not a multiple of the "
            f"{sset.gridx}x{sset.gridy} sprite grid"
        )
    cw = img_w // sset.gridx
    ch = img_h // sset.gridy
    box = (spr.x * cw, spr.y * ch, (spr.x + spr.w) * cw, (spr.y + spr.h) * ch)
    if box[2] > img_w or box[3] > img_h or spr.w <= 0 or spr.h <= 0:
        raise ConvertError(
            f"sprite '{name}' rectangle {box} is outside {img_w}x{img_h}"
        )
    return box


# --------------------------------------------------------------------------
# Image helpers
# --------------------------------------------------------------------------


def resize_rgba(img: Image.Image, w: int, h: int) -> Image.Image:
    """Alpha-weighted resize.

    Straight RGBA resampling averages the colour of fully transparent texels
    into the visible edge and leaves a dark halo, which is very visible once a
    64-pixel tile is squeezed into 16. Premultiplying first ("RGBa") makes the
    filter weight colour by coverage, and the result is converted back to the
    straight alpha the RDP blender expects.
    """
    if img.size == (w, h):
        return img
    return img.convert("RGBa").resize((w, h), Image.LANCZOS).convert("RGBA")


def binarize_alpha(img: Image.Image, threshold: int = 80) -> Image.Image:
    """Make an RGBA image survive RGBA16's single alpha bit.

    RGBA16 is RGB5551, so every texel is either fully opaque or gone. Two
    things go wrong if the decision is left to the default 50% cut:

    * uniformly translucent art disappears completely -- the desktop HUD's
      "empty" heart and shield are drawn at ~40% alpha and would never show up
      on target at all;
    * after a 4x downscale, a small sprite is mostly anti-aliased edge, and a
      50% cut eats the shape.

    So the alpha channel is first normalised to its own maximum (which only
    changes uniformly translucent sources) and then cut at a deliberately
    generous threshold, which keeps silhouettes legible at 8-35 screen pixels.
    """
    alpha = img.getchannel("A")
    peak = alpha.getextrema()[1]
    if peak == 0:
        return img
    scale = 255.0 / peak
    img.putalpha(alpha.point(lambda v: 255 if v * scale >= threshold else 0))
    return img


def load_rgba(path: str) -> Image.Image:
    if not os.path.exists(path):
        raise ConvertError(f"missing source image: {path}")
    return Image.open(path).convert("RGBA")


# --------------------------------------------------------------------------
# Teeworlds datafile (.map) reader
# --------------------------------------------------------------------------


class DataFile:
    """Minimal reader for the Teeworlds datafile container.

    Mirrors ``CDataFileReader::Open`` in src/engine/shared/datafile.cpp: a
    36-byte header, an item-type index, item and data offset tables, the v4
    uncompressed-size table, the item area and finally the zlib-compressed
    data area.
    """

    def __init__(self, path: str):
        with open(path, "rb") as handle:
            buf = handle.read()
        if len(buf) < 36:
            raise ConvertError(f"{path}: truncated datafile")
        (
            magic,
            version,
            _size,
            _swaplen,
            num_types,
            num_items,
            num_data,
            item_size,
            data_size,
        ) = struct.unpack_from("<4siiiiiiii", buf, 0)
        if magic not in (b"DATA", b"ATAD"):
            raise ConvertError(f"{path}: bad datafile signature {magic!r}")
        if version not in (3, 4):
            raise ConvertError(f"{path}: unsupported datafile version {version}")
        off = 36
        self.types = []
        for _ in range(num_types):
            self.types.append(struct.unpack_from("<iii", buf, off))
            off += 12
        self.item_offsets = list(struct.unpack_from(f"<{num_items}i", buf, off))
        off += 4 * num_items
        self.data_offsets = list(struct.unpack_from(f"<{num_data}i", buf, off))
        off += 4 * num_data
        if version == 4:
            off += 4 * num_data  # uncompressed sizes; zlib already reports them
        self.items_start = off
        self.data_start = off + item_size
        self.data_size = data_size
        self.num_data = num_data
        self.version = version
        self.buf = buf
        self.path = path

    def item(self, index: int):
        off = self.items_start + self.item_offsets[index]
        type_and_id, size = struct.unpack_from("<ii", self.buf, off)
        return (type_and_id >> 16) & 0xFFFF, self.buf[off + 8 : off + 8 + size]

    def type_range(self, wanted: int):
        for typ, start, num in self.types:
            if typ == wanted:
                return start, num
        return 0, 0

    def data(self, index: int) -> bytes:
        if index < 0 or index >= self.num_data:
            raise ConvertError(f"{self.path}: data index {index} out of range")
        begin = self.data_start + self.data_offsets[index]
        if index == self.num_data - 1:
            end = self.data_start + self.data_size
        else:
            end = self.data_start + self.data_offsets[index + 1]
        raw = self.buf[begin:end]
        if self.version == 3:
            return raw
        return zlib.decompress(raw)

    def string(self, index: int) -> str:
        return self.data(index).split(b"\0")[0].decode("utf-8", "replace")


def ints(payload: bytes, count: int, offset: int = 0):
    return struct.unpack_from(f"<{count}i", payload, offset * 4)


def expand_tiles(blob: bytes, count: int, tilemap_version: int):
    """Expand the v4 run-length tile encoding (see src/engine/shared/map.cpp).

    Each stored 4-byte CTile means "emit this (index, flags) m_Skip + 1 times".
    """
    if tilemap_version <= 3:
        return [(blob[i * 4], blob[i * 4 + 1]) for i in range(count)]
    out = []
    pos = 0
    while len(out) < count and pos + 4 <= len(blob):
        index, flags, skip = blob[pos], blob[pos + 1], blob[pos + 2]
        out.extend([(index, flags)] * min(skip + 1, count - len(out)))
        pos += 4
    if len(out) != count:
        raise ConvertError("tile layer data ran out before the layer was filled")
    return out


def scan_map_tilesets(map_path: str):
    """Return {image name: uses_rotate} for every graphic tile layer of a map."""
    df = DataFile(map_path)
    start, num = df.type_range(MAPITEMTYPE_IMAGE)
    images = []
    for i in range(num):
        _typ, payload = df.item(start + i)
        _ver, _w, _h, external, name_idx, data_idx = ints(payload, 6)
        images.append((df.string(name_idx), external, data_idx))

    gstart, gnum = df.type_range(MAPITEMTYPE_GROUP)
    lstart, _lnum = df.type_range(MAPITEMTYPE_LAYER)
    used = {}
    for g in range(gnum):
        _typ, payload = df.item(gstart + g)
        _ox, _oy, _px, _py, first_layer, layer_count = ints(payload, 6, 1)
        for i in range(layer_count):
            _typ, lp = df.item(lstart + first_layer + i)
            _lver, ltype, _lflags = ints(lp, 3)
            if ltype != LAYERTYPE_TILES:
                continue
            tver, width, height, tflags = ints(lp, 4, 3)
            _cenv, _cenvoff, image, data = ints(lp, 4, 11)
            if tflags & TILESLAYERFLAG_GAME:
                continue  # collision layer, never drawn
            if image < 0:
                continue  # untextured tile layer: the renderer skips it
            if image >= len(images):
                raise ConvertError(
                    f"{map_path}: layer references image {image} of {len(images)}"
                )
            name, external, data_idx = images[image]
            if not external and data_idx >= 0:
                raise ConvertError(
                    f"{map_path}: image '{name}' is embedded; the converter only "
                    "handles external mapres, extend it before shipping this map"
                )
            tiles = expand_tiles(df.data(data), width * height, tver)
            rotate = any(flags & TILEFLAG_ROTATE for index, flags in tiles if index)
            used[name] = used.get(name, False) or rotate
    return used


# --------------------------------------------------------------------------
# Conversion steps
# --------------------------------------------------------------------------


@dataclass
class Job:
    """One PNG that mksprite has to turn into a .sprite."""

    png: str
    fmt: str


def convert_game_sprites(datasrc: str, content: Content, scratch: str) -> list:
    src = load_rgba(os.path.join(datasrc, "game.png"))
    jobs = []
    for name, (out, w, h) in sorted(GAME_SPRITES.items()):
        box = sprite_box(content, name, src.width, src.height)
        cut = binarize_alpha(resize_rgba(src.crop(box), w, h))
        path = os.path.join(scratch, f"spr_{out}.png")
        cut.save(path)
        jobs.append(Job(path, "RGBA16"))
    return jobs


def convert_ui_sprites(datasrc: str, scratch: str) -> list:
    """The menu's logo and backdrop layers, at their on-screen sizes."""
    jobs = []
    for out, png, w, h, fmt, crop in UI_SPRITES:
        src = load_rgba(os.path.join(datasrc, png))
        if crop:
            box = src.getbbox()
            if box is None:
                raise ConvertError(f"ui source '{png}' is fully transparent")
            src = src.crop(box)
        img = resize_rgba(src, w, h)
        if fmt == "RGBA16":
            img = binarize_alpha(img)
        path = os.path.join(scratch, f"{out}.png")
        img.save(path)
        jobs.append(Job(path, fmt))
    return jobs


def compose_tee_part(
    datasrc: str, content: Content, png: str, cells: list, w: int, h: int
):
    src = load_rgba(os.path.join(datasrc, png))
    box0 = sprite_box(content, cells[0], src.width, src.height)
    composed = Image.new("RGBA", (box0[2] - box0[0], box0[3] - box0[1]), (0, 0, 0, 0))
    for cell in cells:
        box = sprite_box(content, cell, src.width, src.height)
        layer = src.crop(box)
        if layer.size != composed.size:
            raise ConvertError(
                f"tee part cell '{cell}' is {layer.size}, expected {composed.size}"
            )
        composed.alpha_composite(layer)
    return resize_rgba(composed, w, h)


def convert_tee_parts(datasrc: str, content: Content, scratch: str) -> list:
    atlas = Image.new("RGBA", (TEE_ATLAS_W, TEE_ATLAS_H), (0, 0, 0, 0))
    for out, png, cells, w, h in TEE_PARTS:
        img = compose_tee_part(datasrc, content, png, cells, w, h)
        # The 0.7 parts are greyscale by construction; the renderer relies on
        # that to reproduce the desktop "grey texel * skin colour" modulate.
        for pixel in img.get_flattened_data():
            r, g, b, a = pixel
            if a > 8 and not (r == g == b):
                raise ConvertError(
                    f"tee part '{out}' is not greyscale ({r},{g},{b}); IA8 tinting "
                    "would silently drop its hue"
                )
        x, y = TEE_ATLAS_PLACEMENT[out]
        if x + w > TEE_ATLAS_W or y + h > TEE_ATLAS_H:
            raise ConvertError(
                f"tee part '{out}' does not fit the {TEE_ATLAS_W}x{TEE_ATLAS_H} atlas"
            )
        atlas.paste(img, (x, y))
    path = os.path.join(scratch, "tee_parts.png")
    atlas.save(path)
    return [Job(path, "IA8")]


def build_tileset_sheet(source: Image.Image, rotate: bool) -> Image.Image:
    """Cut a 16x16 tileset into 16-pixel tiles, optionally rotated 90 CW.

    Each tile is resized on its own. Resizing the full sheet in one go would
    let neighbouring tiles bleed across their borders, which shows up on target
    as a bright seam around every solid block.
    """
    if source.width % TILES_PER_ROW or source.height % TILES_PER_ROW:
        raise ConvertError(
            f"tileset is {source.width}x{source.height}, not a multiple of "
            f"{TILES_PER_ROW} in both axes"
        )
    tw = source.width // TILES_PER_ROW
    th = source.height // TILES_PER_ROW
    sheet = Image.new(
        "RGBA", (TILES_PER_ROW * TILE_PIXELS, TILES_PER_ROW * TILE_PIXELS), (0, 0, 0, 0)
    )
    for ty in range(TILES_PER_ROW):
        for tx in range(TILES_PER_ROW):
            tile = source.crop((tx * tw, ty * th, (tx + 1) * tw, (ty + 1) * th))
            if rotate:
                # TILEFLAG_ROTATE draws the tile rotated 90 degrees clockwise;
                # ROTATE_270 is Pillow's counter-clockwise spelling of that.
                tile = tile.transpose(Image.Transpose.ROTATE_270)
            sheet.paste(
                resize_rgba(tile, TILE_PIXELS, TILE_PIXELS),
                (tx * TILE_PIXELS, ty * TILE_PIXELS),
            )
    return sheet


def convert_tilesets(datasrc: str, maps_dir: str, scratch: str, verbose: bool):
    used = {}
    for name in ROM_MAPS:
        path = os.path.join(maps_dir, f"{name}.map")
        if not os.path.exists(path):
            raise ConvertError(f"map staged in ROM_MAPS is missing: {path}")
        for image, rotate in scan_map_tilesets(path).items():
            used[image] = used.get(image, False) or rotate
    if not used:
        raise ConvertError("no tile-layer images referenced by any ROM map")

    jobs = []
    for image in sorted(used):
        src = load_rgba(os.path.join(datasrc, "mapres", f"{image}.png"))
        sheet = build_tileset_sheet(src, rotate=False)
        path = os.path.join(scratch, f"mapimg_{image}.png")
        sheet.save(path)
        jobs.append(Job(path, "CI8"))
        if used[image]:
            rot = build_tileset_sheet(src, rotate=True)
            path = os.path.join(scratch, f"mapimg_{image}_r.png")
            rot.save(path)
            jobs.append(Job(path, "CI8"))
        if verbose:
            print(
                f"  tileset {image:<22} {src.width}x{src.height} "
                f"rotate_sheet={bool(used[image])}"
            )
    return jobs, used


# --------------------------------------------------------------------------
# mksprite
# --------------------------------------------------------------------------


def run_mksprite(
    jobs: list, repo_root: str, out_dir: str, docker: str, image: str, verbose: bool
):
    """Convert every staged PNG with libdragon's mksprite, in the toolchain image.

    One invocation per output format, so a full asset rebuild costs a handful
    of container starts rather than one per sprite.
    """
    by_format = {}
    for job in jobs:
        by_format.setdefault(job.fmt, []).append(job.png)
    rel_out = os.path.relpath(out_dir, repo_root)
    for fmt in sorted(by_format):
        rel = [
            os.path.join("/workdir", os.path.relpath(p, repo_root))
            for p in sorted(by_format[fmt])
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
            "mksprite",
            "--format",
            fmt,
            "--compress",
            "1",
            "--output",
            os.path.join("/workdir", rel_out),
        ] + rel
        if verbose:
            print(f"  mksprite --format {fmt} ({len(rel)} files)")
        proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if proc.returncode != 0:
            sys.stderr.write(proc.stdout)
            sys.stderr.write(proc.stderr)
            raise ConvertError(f"mksprite failed for format {fmt}")


def check_outputs(jobs: list, out_dir: str) -> dict:
    """Every staged PNG must have produced a .sprite; report the total size."""
    total = 0
    sizes = {}
    for job in jobs:
        name = os.path.splitext(os.path.basename(job.png))[0] + ".sprite"
        path = os.path.join(out_dir, name)
        if not os.path.exists(path):
            raise ConvertError(f"mksprite produced no output for {job.png}")
        size = os.path.getsize(path)
        sizes[name] = size
        total += size
    sizes["__total__"] = total
    return sizes


def prune_stale(out_dir: str, jobs: list):
    """Drop .sprite files no longer produced, so the DFS image cannot grow stale."""
    wanted = {os.path.splitext(os.path.basename(j.png))[0] + ".sprite" for j in jobs}
    for name in sorted(os.listdir(out_dir)):
        if name.endswith(".sprite") and name not in wanted:
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
    parser.add_argument("--out", default=None, help="ROM filesystem gfx directory")
    parser.add_argument("--scratch", default=None, help="intermediate PNG directory")
    parser.add_argument("--docker", default=os.environ.get("DOCKER", "podman"))
    parser.add_argument("--image", default="tw64-toolchain:local")
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
    maps_dir = os.path.join(datasrc, "maps")
    out_dir = args.out or os.path.join(project, "filesystem", "gfx")
    scratch = args.scratch or os.path.join(project, "build", "assets")
    verbose = not args.quiet
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(scratch, exist_ok=True)

    inputs = [
        os.path.abspath(__file__),
        os.path.join(datasrc, "content.py"),
        os.path.join(datasrc, "game.png"),
    ]
    inputs += [os.path.join(datasrc, png) for _o, png, _c, _w, _h in TEE_PARTS]
    inputs += [os.path.join(datasrc, png) for _o, png, _w, _h, _f, _c in UI_SPRITES]
    inputs += [os.path.join(maps_dir, f"{m}.map") for m in ROM_MAPS]
    inputs += [
        os.path.join(datasrc, "mapres", f)
        for f in sorted(os.listdir(os.path.join(datasrc, "mapres")))
        if f.endswith(".png")
    ]
    missing = [p for p in inputs if not os.path.exists(p)]
    if missing:
        raise ConvertError("missing converter inputs: " + ", ".join(missing))

    stamp_path = os.path.join(out_dir, "assets.json")
    fingerprint = source_fingerprint(inputs)
    if not args.force and os.path.exists(stamp_path):
        try:
            with open(stamp_path, encoding="utf-8") as handle:
                if json.load(handle).get("fingerprint") == fingerprint:
                    if verbose:
                        print("n64 assets up to date")
                    return 0
        except (OSError, ValueError):
            pass

    content = parse_content(os.path.join(datasrc, "content.py"))
    if verbose:
        print(f"converting n64 assets from {datasrc}")
    jobs = convert_game_sprites(datasrc, content, scratch)
    jobs += convert_tee_parts(datasrc, content, scratch)
    jobs += convert_ui_sprites(datasrc, scratch)
    tile_jobs, tilesets = convert_tilesets(datasrc, maps_dir, scratch, verbose)
    jobs += tile_jobs

    run_mksprite(jobs, project, out_dir, args.docker, args.image, verbose)
    sizes = check_outputs(jobs, out_dir)
    prune_stale(out_dir, jobs)

    with open(stamp_path, "w", encoding="utf-8") as handle:
        json.dump(
            {
                "fingerprint": fingerprint,
                "sprites": len(jobs),
                "tilesets": {k: bool(v) for k, v in sorted(tilesets.items())},
                "bytes": sizes,
            },
            handle,
            indent=1,
            sort_keys=True,
        )
    if verbose:
        rotated = sum(1 for v in tilesets.values() if v)
        print(
            f"wrote {len(jobs)} sprites ({len(tilesets)} tilesets, "
            f"{rotated} rotated sheets) = {sizes['__total__']} bytes into {out_dir}"
        )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ConvertError as err:
        sys.stderr.write(f"convert_n64_assets: {err}\n")
        sys.exit(1)
