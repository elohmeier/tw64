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

Five asset classes, four deliberate formats:

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
* ``gfx/map_preview_*.sprite`` -- CI8 menu thumbnails rendered deterministically
  from each map's target-visible tile layers. Each map keeps its own 256-colour
  palette; a generated contact sheet makes the automatic camera crops easy to
  review without booting an emulator.

The PNG slicing runs on the host with Pillow; the PNG -> ``.sprite``
conversion runs ``mksprite`` inside the libdragon toolchain container, the same
way the project ``Makefile`` runs the ROM build.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import statistics
import struct
import subprocess
import sys
import zlib
from dataclasses import dataclass, field

from PIL import Image, ImageChops, ImageDraw

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

# Map-menu previews are rendered at twice their stored size, then reduced once
# with Pillow's high-quality filter. The working canvas uses the target's own
# 0.375 world scale, so the thumbnail shows a slightly tighter, more legible
# 21x16-tile camera without inventing a separate map-art style.
MAP_PREVIEW_W = 128
MAP_PREVIEW_H = 96
MAP_PREVIEW_SUPERSAMPLE = 2
MAP_PREVIEW_RENDER_W = MAP_PREVIEW_W * MAP_PREVIEW_SUPERSAMPLE
MAP_PREVIEW_RENDER_H = MAP_PREVIEW_H * MAP_PREVIEW_SUPERSAMPLE
MAP_PREVIEW_WORLD_SCALE = 0.375
MAP_TILE_WORLD_SIZE = 32

# Automatic anchors are deliberately the default. A map may opt into an
# art-directed camera in tile coordinates if the generated contact sheet shows
# that its spawn/flag midpoint is not its most recognisable room.
MAP_PREVIEW_CAMERA_OVERRIDES = {
    # These very wide CTF maps have visually sparse midfields. Frame the red
    # base instead: it carries the same mirrored geometry as blue and shows
    # more of each map's material language at thumbnail scale.
    "ctf2": (35.5, 26.5),
    "ctf5": (32.5, 61.5),
    # ctf4's flag stands are at the bottom edge; the central spawn deck is the
    # representative jungle room and avoids a crop dominated by the border.
    "ctf4": (90.5, 25.5),
}

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
LAYERTYPE_QUADS = 3
TILESLAYERFLAG_GAME = 1
TILEFLAG_VFLIP = 1
TILEFLAG_HFLIP = 2
TILEFLAG_ROTATE = 8

ENTITY_OFFSET = 255 - 16 * 4
ENTITY_SPAWN = ENTITY_OFFSET + 1
ENTITY_SPAWN_RED = ENTITY_OFFSET + 2
ENTITY_SPAWN_BLUE = ENTITY_OFFSET + 3
ENTITY_FLAGSTAND_RED = ENTITY_OFFSET + 4
ENTITY_FLAGSTAND_BLUE = ENTITY_OFFSET + 5

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
# Map-menu preview renderer
# --------------------------------------------------------------------------


@dataclass
class Job:
    """One PNG that mksprite has to turn into a .sprite."""

    png: str
    fmt: str


@dataclass
class PreviewLayer:
    width: int
    height: int
    tiles: list
    image: str
    color: tuple
    offset_x: int
    offset_y: int
    parallax_x: float
    parallax_y: float
    clip: tuple | None


@dataclass
class PreviewMap:
    sky: tuple
    game_width: int
    game_height: int
    markers: list
    layers: list


def parse_preview_map(map_path: str) -> PreviewMap:
    """Read the static subset the target renderer can actually display.

    This intentionally follows ``Tw64RenderSetMap`` rather than the desktop
    client: textured tile layers are rendered with group transforms, while the
    first untextured quad contributes the target's flat sky colour. A preview
    therefore never promises decorative quad art that the match will omit.
    """
    df = DataFile(map_path)
    image_start, image_count = df.type_range(MAPITEMTYPE_IMAGE)
    images = []
    for i in range(image_count):
        _typ, payload = df.item(image_start + i)
        _ver, _w, _h, external, name_idx, data_idx = ints(payload, 6)
        images.append((df.string(name_idx), external, data_idx))

    group_start, group_count = df.type_range(MAPITEMTYPE_GROUP)
    layer_start, _layer_count = df.type_range(MAPITEMTYPE_LAYER)
    sky = None
    game_width = 0
    game_height = 0
    markers = []
    layers = []

    for g in range(group_count):
        _typ, gp = df.item(group_start + g)
        group_version = ints(gp, 1)[0]
        offset_x, offset_y, parallax_x, parallax_y, first_layer, num_layers = ints(
            gp, 6, 1
        )
        clip = None
        if group_version >= 2 and len(gp) >= 12 * 4:
            use_clip, clip_x, clip_y, clip_w, clip_h = ints(gp, 5, 7)
            if use_clip:
                clip = (clip_x, clip_y, clip_w, clip_h)

        for i in range(num_layers):
            _typ, lp = df.item(layer_start + first_layer + i)
            _layer_version, layer_type, _layer_flags = ints(lp, 3)

            if layer_type == LAYERTYPE_QUADS and sky is None and len(lp) >= 7 * 4:
                _quad_version, num_quads, data_idx, image_idx = ints(lp, 4, 3)
                if image_idx == -1 and num_quads > 0:
                    quad = df.data(data_idx)
                    if len(quad) >= 18 * 4:
                        colors = ints(quad, 8, 10)
                        sky = tuple(
                            (colors[c] + colors[4 + c]) // 2 for c in range(3)
                        )
                continue

            if layer_type != LAYERTYPE_TILES or len(lp) < 15 * 4:
                continue
            tile_version, width, height, tile_flags = ints(lp, 4, 3)
            color = ints(lp, 4, 7)
            _color_env, _color_env_offset, image_idx, data_idx = ints(lp, 4, 11)
            tile_data = expand_tiles(
                df.data(data_idx), width * height, tile_version
            )

            if tile_flags & TILESLAYERFLAG_GAME:
                game_width = width
                game_height = height
                for pos, (index, _flags) in enumerate(tile_data):
                    if index in (
                        ENTITY_SPAWN,
                        ENTITY_SPAWN_RED,
                        ENTITY_SPAWN_BLUE,
                        ENTITY_FLAGSTAND_RED,
                        ENTITY_FLAGSTAND_BLUE,
                    ):
                        markers.append(
                            (index, pos % width + 0.5, pos // width + 0.5)
                        )
                continue
            if image_idx < 0:
                continue
            if image_idx >= len(images):
                raise ConvertError(
                    f"{map_path}: preview layer references image {image_idx} "
                    f"of {len(images)}"
                )
            image, external, embedded_idx = images[image_idx]
            if not external and embedded_idx >= 0:
                raise ConvertError(
                    f"{map_path}: preview image '{image}' is embedded; only "
                    "external mapres are supported"
                )
            # Keep the same fixed layer budget as the target renderer.
            if len(layers) < 16:
                layers.append(
                    PreviewLayer(
                        width,
                        height,
                        tile_data,
                        image,
                        color,
                        offset_x,
                        offset_y,
                        parallax_x * 0.01,
                        parallax_y * 0.01,
                        clip,
                    )
                )

    if game_width <= 0 or game_height <= 0:
        raise ConvertError(f"{map_path}: no game layer for preview camera")
    return PreviewMap(
        sky or (14, 16, 26), game_width, game_height, markers, layers
    )


def preview_camera(name: str, preview_map: PreviewMap) -> tuple:
    """Choose and clamp a stable camera in world coordinates."""
    override = MAP_PREVIEW_CAMERA_OVERRIDES.get(name)
    if override is not None:
        tile_x, tile_y = override
    else:
        flags = [
            (x, y)
            for marker, x, y in preview_map.markers
            if marker in (ENTITY_FLAGSTAND_RED, ENTITY_FLAGSTAND_BLUE)
        ]
        spawns = [
            (x, y)
            for marker, x, y in preview_map.markers
            if marker in (ENTITY_SPAWN, ENTITY_SPAWN_RED, ENTITY_SPAWN_BLUE)
        ]
        anchors = flags if len(flags) >= 2 else spawns
        if anchors:
            tile_x = statistics.median(x for x, _y in anchors)
            tile_y = statistics.median(y for _x, y in anchors)
        else:
            tile_x = preview_map.game_width * 0.5
            tile_y = preview_map.game_height * 0.5

    camera_x = tile_x * MAP_TILE_WORLD_SIZE
    camera_y = tile_y * MAP_TILE_WORLD_SIZE
    half_w = MAP_PREVIEW_RENDER_W / (2.0 * MAP_PREVIEW_WORLD_SCALE)
    half_h = MAP_PREVIEW_RENDER_H / (2.0 * MAP_PREVIEW_WORLD_SCALE)
    world_w = preview_map.game_width * MAP_TILE_WORLD_SIZE
    world_h = preview_map.game_height * MAP_TILE_WORLD_SIZE
    camera_x = world_w * 0.5 if world_w <= 2 * half_w else min(
        max(camera_x, half_w), world_w - half_w
    )
    camera_y = world_h * 0.5 if world_h <= 2 * half_h else min(
        max(camera_y, half_h), world_h - half_h
    )
    return camera_x, camera_y


def composite_clipped(dest: Image.Image, src: Image.Image, x: int, y: int, clip):
    x0 = max(x, clip[0], 0)
    y0 = max(y, clip[1], 0)
    x1 = min(x + src.width, clip[2], dest.width)
    y1 = min(y + src.height, clip[3], dest.height)
    if x1 <= x0 or y1 <= y0:
        return
    cut = src.crop((x0 - x, y0 - y, x1 - x, y1 - y))
    dest.alpha_composite(cut, (x0, y0))


def render_map_preview(
    name: str, preview_map: PreviewMap, datasrc: str, sheet_cache: dict
) -> Image.Image:
    camera_x, camera_y = preview_camera(name, preview_map)
    canvas = Image.new(
        "RGBA",
        (MAP_PREVIEW_RENDER_W, MAP_PREVIEW_RENDER_H),
        preview_map.sky + (255,),
    )
    center_x = MAP_PREVIEW_RENDER_W // 2
    center_y = MAP_PREVIEW_RENDER_H // 2
    tile_screen = int(MAP_TILE_WORLD_SIZE * MAP_PREVIEW_WORLD_SCALE)
    tile_cache = {}

    def screen_x(world_x, layer_camera_x):
        return center_x + int(
            (world_x - layer_camera_x) * MAP_PREVIEW_WORLD_SCALE
        )

    def screen_y(world_y, layer_camera_y):
        return center_y + int(
            (world_y - layer_camera_y) * MAP_PREVIEW_WORLD_SCALE
        )

    for layer in preview_map.layers:
        layer_camera_x = layer.offset_x + camera_x * layer.parallax_x
        layer_camera_y = layer.offset_y + camera_y * layer.parallax_y
        left = layer_camera_x - center_x / MAP_PREVIEW_WORLD_SCALE
        right = layer_camera_x + center_x / MAP_PREVIEW_WORLD_SCALE
        top = layer_camera_y - center_y / MAP_PREVIEW_WORLD_SCALE
        bottom = layer_camera_y + center_y / MAP_PREVIEW_WORLD_SCALE
        tx0 = math.floor(left / MAP_TILE_WORLD_SIZE)
        tx1 = math.floor(right / MAP_TILE_WORLD_SIZE)
        ty0 = math.floor(top / MAP_TILE_WORLD_SIZE)
        ty1 = math.floor(bottom / MAP_TILE_WORLD_SIZE)

        draw_clip = (0, 0, MAP_PREVIEW_RENDER_W, MAP_PREVIEW_RENDER_H)
        if layer.clip:
            clip_x, clip_y, clip_w, clip_h = layer.clip
            draw_clip = (
                max(0, screen_x(clip_x, camera_x)),
                max(0, screen_y(clip_y, camera_y)),
                min(MAP_PREVIEW_RENDER_W, screen_x(clip_x + clip_w, camera_x)),
                min(MAP_PREVIEW_RENDER_H, screen_y(clip_y + clip_h, camera_y)),
            )
            if draw_clip[2] <= draw_clip[0] or draw_clip[3] <= draw_clip[1]:
                continue

        sheet = sheet_cache.get(layer.image)
        if sheet is None:
            source = load_rgba(
                os.path.join(datasrc, "mapres", f"{layer.image}.png")
            )
            sheet = build_tileset_sheet(source, rotate=False)
            sheet_cache[layer.image] = sheet

        for ty in range(ty0, ty1 + 1):
            map_y = min(max(ty, 0), layer.height - 1)
            for tx in range(tx0, tx1 + 1):
                map_x = min(max(tx, 0), layer.width - 1)
                index, flags = layer.tiles[map_y * layer.width + map_x]
                if not index:
                    continue
                relevant_flags = flags & (
                    TILEFLAG_VFLIP | TILEFLAG_HFLIP | TILEFLAG_ROTATE
                )
                key = (layer.image, index, relevant_flags, layer.color)
                tile = tile_cache.get(key)
                if tile is None:
                    sx = (index % TILES_PER_ROW) * TILE_PIXELS
                    sy = (index // TILES_PER_ROW) * TILE_PIXELS
                    tile = sheet.crop((sx, sy, sx + TILE_PIXELS, sy + TILE_PIXELS))
                    rotated = bool(relevant_flags & TILEFLAG_ROTATE)
                    if rotated:
                        tile = tile.transpose(Image.Transpose.ROTATE_270)
                    flip_x = bool(
                        relevant_flags
                        & (TILEFLAG_HFLIP if rotated else TILEFLAG_VFLIP)
                    )
                    flip_y = bool(
                        relevant_flags
                        & (TILEFLAG_VFLIP if rotated else TILEFLAG_HFLIP)
                    )
                    if flip_x:
                        tile = tile.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
                    if flip_y:
                        tile = tile.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
                    tile = resize_rgba(tile, tile_screen, tile_screen)
                    tile = ImageChops.multiply(
                        tile, Image.new("RGBA", tile.size, layer.color)
                    )
                    tile_cache[key] = tile
                x = screen_x(tx * MAP_TILE_WORLD_SIZE, layer_camera_x)
                y = screen_y(ty * MAP_TILE_WORLD_SIZE, layer_camera_y)
                composite_clipped(canvas, tile, x, y, draw_clip)

    preview = resize_rgba(canvas, MAP_PREVIEW_W, MAP_PREVIEW_H)
    rgb_preview = preview.convert("RGB")
    colors = rgb_preview.getcolors(maxcolors=MAP_PREVIEW_W * MAP_PREVIEW_H)
    flat = Image.new("RGB", preview.size, rgb_preview.getpixel((0, 0)))
    if (colors is not None and len(colors) < 8) or ImageChops.difference(
        rgb_preview, flat
    ).getbbox() is None:
        raise ConvertError(f"{name}: generated map preview is blank or near-uniform")
    return preview


def convert_map_previews(datasrc: str, maps_dir: str, scratch: str, verbose: bool):
    jobs = []
    previews = []
    sheet_cache = {}
    for name in ROM_MAPS:
        preview_map = parse_preview_map(os.path.join(maps_dir, f"{name}.map"))
        preview = render_map_preview(name, preview_map, datasrc, sheet_cache)
        path = os.path.join(scratch, f"map_preview_{name}.png")
        preview.save(path)
        jobs.append(Job(path, "CI8"))
        previews.append((name, preview))
        if verbose:
            camera_x, camera_y = preview_camera(name, preview_map)
            print(
                f"  preview {name:<4} camera=({camera_x / MAP_TILE_WORLD_SIZE:.1f},"
                f"{camera_y / MAP_TILE_WORLD_SIZE:.1f}) "
                f"layers={len(preview_map.layers)}"
            )

    cell_h = MAP_PREVIEW_H + 16
    contact = Image.new(
        "RGB", (MAP_PREVIEW_W * 4, cell_h * 4), (10, 12, 20)
    )
    draw = ImageDraw.Draw(contact)
    for i, (name, preview) in enumerate(previews):
        x = (i % 4) * MAP_PREVIEW_W
        y = (i // 4) * cell_h
        contact.paste(preview.convert("RGB"), (x, y))
        draw.text(
            (x + 4, y + MAP_PREVIEW_H + 2),
            name.upper(),
            fill=(220, 225, 235),
        )
    contact.save(os.path.join(scratch, "map_previews_contact.png"))
    return jobs


# --------------------------------------------------------------------------
# Conversion steps
# --------------------------------------------------------------------------


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
    jobs += convert_map_previews(datasrc, maps_dir, scratch, verbose)
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
                "map_previews": ROM_MAPS,
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
