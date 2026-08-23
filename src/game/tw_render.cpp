/* Teeworlds 64: textured RDP renderer. See tw_render.h.
 *
 * The frame is built in three RDP phases so the render mode is switched a
 * bounded number of times per frame rather than per primitive:
 *
 *   1. FILL mode, per viewport: the map's own sky colour.
 *   2. STANDARD mode, per viewport: background tile layers, the entities, then
 *      the foreground tile layers, all with combiner TEX*PRIM. Only the TLUT
 *      selection and the primitive colour change inside this phase.
 *   3. FILL mode then STANDARD mode, screen space: HUD panels, the HUD icons
 *      and finally the text, which is an RDP font (mkfont's font64 format) and
 *      therefore part of the same display list. The frame ends with an
 *      asynchronous rdpq_detach_show(), so the CPU returns to the simulation
 *      while the RDP is still drawing and the flip is scheduled on RDP
 *      completion. The predecessor drew text with libdragon's CPU 8x8 font,
 *      which forced an rdpq_detach_wait() drain in the middle of every frame
 *      because CPU and RDP cannot write the same framebuffer concurrently.
 *
 * Tiles are drawn one texture rectangle per *run* of identical tiles rather
 * than per tile: the 16x16 tile is uploaded to TMEM with infinite S repeat, so
 * a horizontal run of N identical tiles is a single rectangle sampling
 * 0..16*N. Because every tile in a run is the same tile, mirroring the whole
 * run is identical to mirroring each tile, so the flip flags survive the
 * merge. TMEM is only re-uploaded when the (tileset, index, rotation) triple
 * changes.
 *
 * The RDP's texture rectangle cannot transpose the S/T axes, so TILEFLAG_ROTATE
 * is served from a second, pre-rotated sheet emitted by the converter; it is
 * loaded only for maps that actually use rotated tiles. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <debug.h>
#include <rdpq.h>
#include <rdpq_attach.h>
#include <rdpq_font.h>
#include <rdpq_mode.h>
#include <rdpq_rect.h>
#include <rdpq_sprite.h>
#include <rdpq_tex.h>
#include <rdpq_text.h>
#include <sprite.h>
#include <surface.h>

#include <base/math.h>
#include <base/system.h>
#include <base/vmath.h>

#include <generated/protocol.h>

#include <engine/map.h>
#include <engine/shared/protocol.h>

#include <game/collision.h>
#include <game/gamecore.h>
#include <game/layers.h>
#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/entities/flag.h>
#include <game/server/entities/laser.h>
#include <game/server/entities/pickup.h>
#include <game/server/entities/projectile.h>
#include <game/server/entity.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/gameworld.h>
#include <game/server/player.h>

#include "tw_render.h"

namespace {

/* World units per screen pixel. 0.375 puts a 32-unit tile on 12 pixels and the
 * desktop client's 64-unit tee visual size on 24, so a full-screen view spans
 * about 27x20 tiles and a splitscreen quadrant about 13x10. The desktop client
 * shows roughly 38x29 tiles; that does not survive a 320x240 composite signal,
 * so Teeworlds 64 is deliberately zoomed in. */
const float SCALE = 0.375f;
const float TILE_SIZE = 32.0f;

/* Source pixels per converted tileset tile; see scripts/convert_n64_assets.py.
 * A tile is drawn at 12 screen pixels, so the RDP downscales by 0.75. */
const int TILE_TEXELS = 16;
const int TILES_PER_ROW = 16;

/* --------------------------------------------------------------------- */
/* Sprite assets                                                          */
/* --------------------------------------------------------------------- */

enum {
  SPR_WEAPON_HAMMER = 0,
  SPR_WEAPON_GUN,
  SPR_WEAPON_SHOTGUN,
  SPR_WEAPON_GRENADE,
  SPR_WEAPON_LASER,
  SPR_WEAPON_NINJA,
  SPR_PROJ_GUN,
  SPR_PROJ_SHOTGUN,
  SPR_PROJ_GRENADE,
  SPR_PROJ_LASER,
  SPR_HOOK_CHAIN,
  SPR_HOOK_HEAD,
  SPR_PICKUP_HEALTH,
  SPR_PICKUP_ARMOR,
  SPR_FLAG_RED,
  SPR_FLAG_BLUE,
  SPR_HUD_HEALTH_FULL,
  SPR_HUD_HEALTH_EMPTY,
  SPR_HUD_ARMOR_FULL,
  SPR_HUD_ARMOR_EMPTY,
  SPR_MUZZLE_GUN1,
  SPR_MUZZLE_GUN2,
  SPR_MUZZLE_GUN3,
  SPR_MUZZLE_SHOTGUN1,
  SPR_MUZZLE_SHOTGUN2,
  SPR_MUZZLE_SHOTGUN3,
  SPR_TEE_PARTS,
  /* Menu art. Unlike everything above, these are larger than TMEM and are
   * therefore always drawn through rdpq_sprite_blit (which slices them), never
   * through BindSprite(). */
  SPR_UI_LOGO,
  SPR_UI_CLOUD1,
  SPR_UI_CLOUD2,
  SPR_UI_MOUNTAINS,
  /* Build-rendered map-menu previews, in the same order as s_aMaps in
   * tw_game.cpp and ROM_MAPS in convert_n64_assets.py. */
  SPR_MAP_PREVIEW_DM1,
  SPR_MAP_PREVIEW_DM2,
  SPR_MAP_PREVIEW_DM3,
  SPR_MAP_PREVIEW_DM6,
  SPR_MAP_PREVIEW_DM7,
  SPR_MAP_PREVIEW_DM8,
  SPR_MAP_PREVIEW_DM9,
  SPR_MAP_PREVIEW_LMS1,
  SPR_MAP_PREVIEW_CTF1,
  SPR_MAP_PREVIEW_CTF2,
  SPR_MAP_PREVIEW_CTF3,
  SPR_MAP_PREVIEW_CTF4,
  SPR_MAP_PREVIEW_CTF5,
  SPR_MAP_PREVIEW_CTF6,
  SPR_MAP_PREVIEW_CTF7,
  SPR_MAP_PREVIEW_CTF8,
  NUM_SPRITES,
  SPR_MAP_PREVIEW_FIRST = SPR_MAP_PREVIEW_DM1,
  NUM_MAP_PREVIEWS = SPR_MAP_PREVIEW_CTF8 - SPR_MAP_PREVIEW_FIRST + 1
};

/* The three tee parts share one IA8 atlas so a whole tee costs one TMEM
 * upload. The rectangles must match TEE_ATLAS_PLACEMENT in
 * scripts/convert_n64_assets.py. */
enum {
  TEE_BODY_SX = 0, TEE_BODY_SY = 0, TEE_BODY_SW = 32, TEE_BODY_SH = 32,
  TEE_FOOT_SX = 32, TEE_FOOT_SY = 0, TEE_FOOT_SW = 16, TEE_FOOT_SH = 16,
  TEE_EYES_SX = 32, TEE_EYES_SY = 16, TEE_EYES_SW = 24, TEE_EYES_SH = 12
};

const char *const s_apSpriteFiles[NUM_SPRITES] = {
    "rom:/gfx/spr_weapon_hammer.sprite",
    "rom:/gfx/spr_weapon_gun.sprite",
    "rom:/gfx/spr_weapon_shotgun.sprite",
    "rom:/gfx/spr_weapon_grenade.sprite",
    "rom:/gfx/spr_weapon_laser.sprite",
    "rom:/gfx/spr_weapon_ninja.sprite",
    "rom:/gfx/spr_proj_gun.sprite",
    "rom:/gfx/spr_proj_shotgun.sprite",
    "rom:/gfx/spr_proj_grenade.sprite",
    "rom:/gfx/spr_proj_laser.sprite",
    "rom:/gfx/spr_hook_chain.sprite",
    "rom:/gfx/spr_hook_head.sprite",
    "rom:/gfx/spr_pickup_health.sprite",
    "rom:/gfx/spr_pickup_armor.sprite",
    "rom:/gfx/spr_flag_red.sprite",
    "rom:/gfx/spr_flag_blue.sprite",
    "rom:/gfx/spr_hud_health_full.sprite",
    "rom:/gfx/spr_hud_health_empty.sprite",
    "rom:/gfx/spr_hud_armor_full.sprite",
    "rom:/gfx/spr_hud_armor_empty.sprite",
    "rom:/gfx/spr_muzzle_gun1.sprite",
    "rom:/gfx/spr_muzzle_gun2.sprite",
    "rom:/gfx/spr_muzzle_gun3.sprite",
    "rom:/gfx/spr_muzzle_shotgun1.sprite",
    "rom:/gfx/spr_muzzle_shotgun2.sprite",
    "rom:/gfx/spr_muzzle_shotgun3.sprite",
    "rom:/gfx/tee_parts.sprite",
    "rom:/gfx/ui_logo.sprite",
    "rom:/gfx/ui_cloud1.sprite",
    "rom:/gfx/ui_cloud2.sprite",
    "rom:/gfx/ui_mountains.sprite",
    "rom:/gfx/map_preview_dm1.sprite",
    "rom:/gfx/map_preview_dm2.sprite",
    "rom:/gfx/map_preview_dm3.sprite",
    "rom:/gfx/map_preview_dm6.sprite",
    "rom:/gfx/map_preview_dm7.sprite",
    "rom:/gfx/map_preview_dm8.sprite",
    "rom:/gfx/map_preview_dm9.sprite",
    "rom:/gfx/map_preview_lms1.sprite",
    "rom:/gfx/map_preview_ctf1.sprite",
    "rom:/gfx/map_preview_ctf2.sprite",
    "rom:/gfx/map_preview_ctf3.sprite",
    "rom:/gfx/map_preview_ctf4.sprite",
    "rom:/gfx/map_preview_ctf5.sprite",
    "rom:/gfx/map_preview_ctf6.sprite",
    "rom:/gfx/map_preview_ctf7.sprite",
    "rom:/gfx/map_preview_ctf8.sprite"};

sprite_t *s_apSprites[NUM_SPRITES];
surface_t s_aSpriteSurfaces[NUM_SPRITES];

/* Screen size of every weapon, derived from the desktop client's own numbers:
 * SelectSprite() scales a DrawSprite() quad by (w,h)/sqrt(w^2+h^2) in grid
 * cells, DrawSprite() multiplies by the weapon's visual_size from
 * datasrc/content.py, and the result is world units times SCALE. */
struct CWeaponVisual {
  int m_Sprite;
  float m_W; /* screen pixels */
  float m_H;
  float m_OffsetX; /* world units along the aim direction */
  float m_OffsetY;
};

const CWeaponVisual s_aWeaponVisuals[NUM_WEAPONS] = {
    {SPR_WEAPON_HAMMER, 28.80f, 21.60f, 4.0f, -20.0f},
    {SPR_WEAPON_GUN, 21.47f, 10.73f, 32.0f, 0.0f},
    {SPR_WEAPON_SHOTGUN, 34.92f, 8.73f, 24.0f, -2.0f},
    {SPR_WEAPON_GRENADE, 34.62f, 9.89f, 24.0f, -2.0f},
    {SPR_WEAPON_LASER, 31.71f, 13.59f, 24.0f, -2.0f},
    {SPR_WEAPON_NINJA, 34.92f, 8.73f, 0.0f, 0.0f}};

/* Projectile sprite per weapon; the desktop client draws every projectile as a
 * 32x32 world-unit quad, i.e. 12 screen pixels. */
const int s_aProjectileSprites[NUM_WEAPONS] = {
    SPR_PROJ_GUN, SPR_PROJ_GUN, SPR_PROJ_SHOTGUN,
    SPR_PROJ_GRENADE, SPR_PROJ_LASER, SPR_PROJ_GUN};

/* --------------------------------------------------------------------- */
/* Tilesets and the map layer table                                       */
/* --------------------------------------------------------------------- */

enum {
  TW64_MAX_TILESETS = 10,
  TW64_MAX_LAYERS = 16,
  TW64_TILESET_NAME = 28
};

struct CTileset {
  char m_aName[TW64_TILESET_NAME];
  sprite_t *m_pSheet;
  sprite_t *m_pRotSheet;
  surface_t m_Surface;
  surface_t m_RotSurface;
  uint16_t *m_pPalette;
  int m_PaletteColors;
};

/* One graphic tile layer, flattened out of the map's group/layer tree so the
 * per-frame loop never walks datafile items again. */
struct CLayerView {
  const CTile *m_pTiles;
  int16_t m_Width;
  int16_t m_Height;
  int8_t m_Tileset; /* index into s_aTilesets, -1 = untextured, skipped */
  bool m_Detail;
  bool m_Foreground; /* drawn after the entities, like the desktop client */
  bool m_Clip;
  uint8_t m_R, m_G, m_B, m_A;
  float m_OffsetX;
  float m_OffsetY;
  float m_ParallaxX; /* 1.0 == the game group */
  float m_ParallaxY;
  int m_ClipX, m_ClipY, m_ClipW, m_ClipH; /* world units, game-group space */
};

CTileset s_aTilesets[TW64_MAX_TILESETS];
int s_NumTilesets;
CLayerView s_aLayers[TW64_MAX_LAYERS];
int s_NumLayers;
uint32_t s_SkyColor;
CLayers s_MapLayers;
bool s_MapReady;

/* --------------------------------------------------------------------- */
/* Text                                                                   */
/* --------------------------------------------------------------------- */

enum {
  /* rdpq font registration ids, one per staged font; id 0 is reserved by the
   * text engine, so the table is offset by one. */
  TW64_FONT_ID = 1,
  /* Centred labels are laid out inside a symmetric box around the requested
   * centre, so no call site has to know the glyph advance. */
  TW64_TEXT_BOX_W = 320,
  TW64_MAX_TEXT_STYLES = 4,
  /* Natural line pitch of the staged font, from mkfont's own report
   * (ascent=11, descent=-2, line_gap=0). Multi-line blocks pass
   * `Pitch - TW64_FONT_LINE_H` as line_spacing so their rows land on the exact
   * y coordinates the single-label call sites used to pass one by one. The
   * font is pinned by the `font` rule in n64/Makefile; GFX_FONT logs the
   * metrics it actually loaded. */
  TW64_FONT_LINE_H = 13,
};

struct CRgb {
  uint8_t m_R;
  uint8_t m_G;
  uint8_t m_B;
};

/* rdpq_text takes a baseline, the old CPU font took the top of the 8-pixel
 * cell, and every call site in this port still passes the latter. The shift is
 * derived from each font at load time -- the negative top offset of a capital
 * glyph -- so no coordinate in the HUD, menu or scoreboard has to move, no
 * ascent constant is hard-coded here, and a label lines up with a label in
 * another size when both are given the same Y. */
struct CFontSlot {
  rdpq_font_t *m_pFont;
  int m_Baseline;
};

CFontSlot s_aFonts[TW64_NUM_FONTS];

inline rdpq_font_t *FontOf(int Font) {
  return s_aFonts[(unsigned)Font < TW64_NUM_FONTS ? Font : 0].m_pFont;
}

/* --------------------------------------------------------------------- */
/* Primitive layer                                                        */
/* --------------------------------------------------------------------- */

int s_ClipX0, s_ClipY0, s_ClipX1, s_ClipY1;
uint32_t s_LastColor;
bool s_HasColor;

/* TMEM cache: what is currently loaded, so a run of identical tiles or a row
 * of identical HUD icons costs one upload instead of one per primitive. */
int s_BoundSprite;
int s_BoundSpriteMask;
int s_BoundTileset;
int s_BoundTile;
bool s_BoundTileRot;
int s_BoundPalette;

/* One text paragraph. `pColors` holds the colours the escape codes ^00..^03 in
 * `pText` select, so a block of differently coloured lines is a single
 * paragraph: the per-paragraph cost on this target is a full RDP mode setup
 * plus the atlas block, measured at about 86 us, which dwarfs the ~1 us a
 * glyph costs. Collapsing the four-viewport HUD from twelve paragraphs to four
 * is worth more than anything that can be done per glyph. */
void DrawTextRuns(int Font, int X, int Y, const char *pText,
                  const CRgb *pColors, int NumColors, rdpq_align_t Align,
                  int BoxWidth, int LineSpacing) {
  rdpq_font_t *pFont = FontOf(Font);
  if (!pFont)
    return;
  /* rdpq_font_style() only updates the font's style table; the paragraph
   * renderer copies each colour into the display list as it emits the glyphs,
   * so the four slots can be rewritten every label. */
  for (int i = 0; i < NumColors && i < TW64_MAX_TEXT_STYLES; ++i) {
    rdpq_fontstyle_t Style;
    memset(&Style, 0, sizeof(Style));
    Style.color = RGBA32(pColors[i].m_R, pColors[i].m_G, pColors[i].m_B, 0xff);
    Style.outline_color = RGBA32(0, 0, 0, 0xff);
    rdpq_font_style(pFont, (uint8_t)i, &Style);
  }

  rdpq_textparms_t Parms;
  memset(&Parms, 0, sizeof(Parms));
  Parms.style_id = 0;
  Parms.width = BoxWidth;
  Parms.align = Align;
  Parms.line_spacing = (int16_t)LineSpacing;
  /* The layout engine's anti-alias fix draws a transparent rectangle behind
   * every paragraph to keep the VI's AA filter from smearing text over a 3D
   * background. This renderer draws flat 2D sprites into a 320x240 frame and
   * every label sits on an opaque HUD panel or the sky fill, so the artifact it
   * guards against cannot occur; skipping it saves a mode switch and a
   * rectangle per paragraph. */
  Parms.disable_aa_fix = true;
  rdpq_text_print(&Parms, TW64_FONT_ID + Font, X,
                  Y + s_aFonts[Font].m_Baseline, pText);
  /* The font renderer leaves the RDP in its own standard mode with its own
   * combiner, and it has loaded the glyph atlas into TMEM, so the fill-colour
   * and TMEM caches are no longer valid. Text is the last phase of every frame
   * today; this keeps that from being a hidden requirement. */
  s_HasColor = false;
  s_BoundSprite = -1;
  s_BoundSpriteMask = -1;
  s_BoundTileset = -1;
  s_BoundTile = -1;
  s_BoundPalette = -1;
}

void DrawText(int X, int Y, const char *pText, uint8_t R, uint8_t G, uint8_t B,
              rdpq_align_t Align, int BoxWidth) {
  CRgb Color = {R, G, B};
  DrawTextRuns(TW64_FONT_HUD, X, Y, pText, &Color, 1, Align, BoxWidth, 0);
}

inline uint32_t PackColor(uint8_t R, uint8_t G, uint8_t B) {
  return ((uint32_t)R << 24) | ((uint32_t)G << 16) | ((uint32_t)B << 8) | 0xff;
}

void SetClip(int X0, int Y0, int X1, int Y1) {
  s_ClipX0 = X0 < 0 ? 0 : X0;
  s_ClipY0 = Y0 < 0 ? 0 : Y0;
  s_ClipX1 = X1 > TW64_SCREEN_W ? TW64_SCREEN_W : X1;
  s_ClipY1 = Y1 > TW64_SCREEN_H ? TW64_SCREEN_H : Y1;
}

/* Fill-mode rectangle. Clipped on the CPU as well as scissored, so a wildly
 * off-screen entity can never cost fill rate or overflow the 12.2 fixed-point
 * command encoding. */
void Fill(int X0, int Y0, int X1, int Y1, uint32_t Color) {
  if (X0 < s_ClipX0)
    X0 = s_ClipX0;
  if (Y0 < s_ClipY0)
    Y0 = s_ClipY0;
  if (X1 > s_ClipX1)
    X1 = s_ClipX1;
  if (Y1 > s_ClipY1)
    Y1 = s_ClipY1;
  if (X1 <= X0 || Y1 <= Y0)
    return;
  if (!s_HasColor || Color != s_LastColor) {
    rdpq_set_fill_color(RGBA32((uint8_t)(Color >> 24), (uint8_t)(Color >> 16),
                               (uint8_t)(Color >> 8), 0xff));
    s_LastColor = Color;
    s_HasColor = true;
  }
  rdpq_fill_rectangle(X0, Y0, X1, Y1);
}

/* Standard-mode solid rectangle, for the few world-space shapes that have no
 * sprite (the laser beam). Uses the flat combiner and the primitive colour. */
void FillTextured(int X0, int Y0, int X1, int Y1) {
  if (X0 < s_ClipX0)
    X0 = s_ClipX0;
  if (Y0 < s_ClipY0)
    Y0 = s_ClipY0;
  if (X1 > s_ClipX1)
    X1 = s_ClipX1;
  if (Y1 > s_ClipY1)
    Y1 = s_ClipY1;
  if (X1 <= X0 || Y1 <= Y0)
    return;
  rdpq_fill_rectangle(X0, Y0, X1, Y1);
}

/* --------------------------------------------------------------------- */
/* Palette                                                                */
/* --------------------------------------------------------------------- */

const uint32_t COLOR_LASER = PackColor(180, 235, 255);
const uint32_t COLOR_LASER_CORE = PackColor(255, 255, 255);
const uint32_t COLOR_HUD_BACK = PackColor(44, 48, 60);
const uint32_t COLOR_PANEL = PackColor(10, 12, 20);
const uint32_t COLOR_VIEWPORT_DIVIDER = PackColor(28, 32, 40);
const uint32_t COLOR_SKY_FALLBACK = PackColor(14, 16, 26);
const uint32_t COLOR_WHITE = PackColor(255, 255, 255);

const CRgb s_aPlayerColors[TW64_MAX_VIEWPORTS] = {
    {90, 170, 255}, {255, 96, 96}, {110, 230, 130}, {250, 210, 80}};

/* Team hues. The second member of a team gets the darker shade so two
 * teammates never share one silhouette colour while both still read as their
 * team at a glance. The bright shades are the desktop client's own team tee
 * colours (CSkins::GetTeamColor with no custom skin colour, run through
 * GetColorV3): HSL(0,255,136) and HSL(155,255,136). */
const CRgb s_aaTeamColors[2][2] = {{{255, 74, 74}, {170, 45, 45}},
                                   {{74, 138, 255}, {45, 95, 190}}};

const char *WeaponShortName(int Weapon) {
  switch (Weapon) {
  case WEAPON_HAMMER:
    return "HAM";
  case WEAPON_GUN:
    return "GUN";
  case WEAPON_SHOTGUN:
    return "SHT";
  case WEAPON_GRENADE:
    return "GRN";
  case WEAPON_LASER:
    return "LAS";
  case WEAPON_NINJA:
    return "NIN";
  default:
    return "---";
  }
}

/* --------------------------------------------------------------------- */
/* Camera                                                                 */
/* --------------------------------------------------------------------- */

/* A 64-entry sine table in turns. Everything the renderer needs a sine for is
 * cosmetic motion at one or two screen pixels of amplitude, and newlib's
 * software sinf() costs thousands of cycles on the VR4300 -- enough that
 * calling it once per pickup per viewport dominated the frame. */
enum { TW64_SIN_TABLE = 64 };
float s_aSinTable[TW64_SIN_TABLE];

inline float FastSin(float Turns) {
  int Index = (int)(Turns * (float)TW64_SIN_TABLE);
  return s_aSinTable[Index & (TW64_SIN_TABLE - 1)];
}

struct CCamera {
  float m_X;
  float m_Y;
  int m_CenterX;
  int m_CenterY;
};

inline int ScreenX(const CCamera &Cam, float WorldX) {
  return Cam.m_CenterX + (int)((WorldX - Cam.m_X) * SCALE);
}

inline int ScreenY(const CCamera &Cam, float WorldY) {
  return Cam.m_CenterY + (int)((WorldY - Cam.m_Y) * SCALE);
}

/* --------------------------------------------------------------------- */
/* Textured primitives                                                    */
/* --------------------------------------------------------------------- */

rdpq_texparms_t s_SpriteParms;

/* Every converted sprite fits TMEM whole and has an 8-byte aligned stride, so
 * an upload is SET_TEXTURE_IMAGE + SET_TILE + LOAD_TILE with no run-time
 * planning. `MaskS` is the power-of-two wrap width: 0 clamps (the normal
 * case), and the HUD uses it to paint a whole row of ten identical icons with
 * one rectangle. Sprites whose stride the RDP cannot address directly fall
 * back to the general loader. */
void BindSprite(int Id, int MaskS) {
  if (s_BoundSprite == Id && s_BoundSpriteMask == MaskS)
    return;
  surface_t *pSurface = &s_aSpriteSurfaces[Id];
  if (pSurface->stride & 7) {
    rdpq_tex_upload(TILE0, pSurface, &s_SpriteParms);
  } else {
    rdpq_tileparms_t Parms;
    memset(&Parms, 0, sizeof(Parms));
    Parms.s.mask = (uint8_t)MaskS;
    rdpq_set_texture_image(pSurface);
    rdpq_set_tile(TILE0, surface_get_format(pSurface), 0,
                  (uint16_t)pSurface->stride, &Parms);
    rdpq_load_tile(TILE0, 0, 0, pSurface->width, pSurface->height);
  }
  s_BoundSprite = Id;
  s_BoundSpriteMask = MaskS;
  s_BoundTileset = -1;
  s_BoundTile = -1;
}

/* Axis-aligned blit of a rectangle of a sprite. `Cx`/`Cy` are the centre in
 * screen pixels and `W`/`H` the on-screen size, matching the desktop client's
 * centred quads. */
void DrawSubCentered(int Id, int Sx, int Sy, int Sw, int Sh, float Cx, float Cy,
                     float W, float H, bool FlipX, bool FlipY) {
  const int X0 = (int)(Cx - W * 0.5f + 0.5f);
  const int Y0 = (int)(Cy - H * 0.5f + 0.5f);
  const int X1 = X0 + (int)(W + 0.5f);
  const int Y1 = Y0 + (int)(H + 0.5f);
  if (X1 <= s_ClipX0 || X0 >= s_ClipX1 || Y1 <= s_ClipY0 || Y0 >= s_ClipY1)
    return;
  BindSprite(Id, 0);
  rdpq_texture_rectangle_scaled(TILE0, X0, Y0, X1, Y1,
                                FlipX ? Sx + Sw : Sx, FlipY ? Sy + Sh : Sy,
                                FlipX ? Sx : Sx + Sw, FlipY ? Sy : Sy + Sh);
}

void DrawSpriteCentered(int Id, float Cx, float Cy, float W, float H,
                        bool FlipX, bool FlipY) {
  DrawSubCentered(Id, 0, 0, s_aSpriteSurfaces[Id].width,
                  s_aSpriteSurfaces[Id].height, Cx, Cy, W, H, FlipX, FlipY);
}

/* Rotated sprite blit. Goes through rdpq_sprite_blit, which uploads and draws
 * two triangles, so it is reserved for the handful of primitives that must
 * follow an aim or velocity angle.
 *
 * `Angle` is a Teeworlds screen-space angle, i.e. what the desktop client
 * passes to QuadsSetRotation(): positive turns from +x towards +y, and +y is
 * down. libdragon's blitparms.theta runs the other way, so it is negated
 * here once instead of at every call site. */
void DrawSpriteRotated(int Id, float Cx, float Cy, float W, float H,
                       float Angle, bool FlipY) {
  const float Theta = -Angle;
  if (Cx < s_ClipX0 - 32 || Cx > s_ClipX1 + 32 || Cy < s_ClipY0 - 32 ||
      Cy > s_ClipY1 + 32)
    return;
  const int Sw = s_aSpriteSurfaces[Id].width;
  const int Sh = s_aSpriteSurfaces[Id].height;
  rdpq_blitparms_t Parms;
  memset(&Parms, 0, sizeof(Parms));
  Parms.cx = Sw / 2;
  Parms.cy = Sh / 2;
  Parms.scale_x = W / (float)Sw;
  Parms.scale_y = H / (float)Sh;
  Parms.flip_y = FlipY;
  Parms.theta = Theta;
  rdpq_sprite_blit(s_apSprites[Id], Cx, Cy, &Parms);
  /* rdpq_sprite_blit re-uploads TMEM behind our back. */
  s_BoundSprite = -1;
  s_BoundSpriteMask = -1;
  s_BoundTileset = -1;
  s_BoundTile = -1;
}

/* --------------------------------------------------------------------- */
/* Map loading                                                            */
/* --------------------------------------------------------------------- */

void FreeTilesets() {
  for (int i = 0; i < TW64_MAX_TILESETS; ++i) {
    if (s_aTilesets[i].m_pSheet)
      sprite_free(s_aTilesets[i].m_pSheet);
    if (s_aTilesets[i].m_pRotSheet)
      sprite_free(s_aTilesets[i].m_pRotSheet);
    memset(&s_aTilesets[i], 0, sizeof(s_aTilesets[i]));
  }
  s_NumTilesets = 0;
}

/* Loads (or finds) the sheet for one map image. `WantRotation` pulls in the
 * pre-rotated companion sheet, which only maps that actually use
 * TILEFLAG_ROTATE pay for. */
int AcquireTileset(const char *pName, bool WantRotation) {
  for (int i = 0; i < s_NumTilesets; ++i) {
    if (str_comp(s_aTilesets[i].m_aName, pName))
      continue;
    if (WantRotation && !s_aTilesets[i].m_pRotSheet) {
      char aPath[64];
      str_format(aPath, sizeof(aPath), "rom:/gfx/mapimg_%s_r.sprite", pName);
      s_aTilesets[i].m_pRotSheet = sprite_load(aPath);
      if (!s_aTilesets[i].m_pRotSheet)
        return -1;
      s_aTilesets[i].m_RotSurface = sprite_get_pixels(s_aTilesets[i].m_pRotSheet);
    }
    return i;
  }
  if (s_NumTilesets >= TW64_MAX_TILESETS) {
    debugf("TW64 GFX_FAIL reason=tileset_slots name=%s\n", pName);
    return -1;
  }
  CTileset *pSet = &s_aTilesets[s_NumTilesets];
  char aPath[64];
  str_format(aPath, sizeof(aPath), "rom:/gfx/mapimg_%s.sprite", pName);
  pSet->m_pSheet = sprite_load(aPath);
  if (!pSet->m_pSheet) {
    debugf("TW64 GFX_FAIL reason=tileset name=%s\n", pName);
    return -1;
  }
  str_copy(pSet->m_aName, pName, sizeof(pSet->m_aName));
  pSet->m_Surface = sprite_get_pixels(pSet->m_pSheet);
  pSet->m_pPalette = sprite_get_palette(pSet->m_pSheet);
  pSet->m_PaletteColors = sprite_get_palette_used_colors(pSet->m_pSheet);
  if (pSet->m_PaletteColors <= 0)
    pSet->m_PaletteColors = 256;
  if (WantRotation) {
    str_format(aPath, sizeof(aPath), "rom:/gfx/mapimg_%s_r.sprite", pName);
    pSet->m_pRotSheet = sprite_load(aPath);
    if (!pSet->m_pRotSheet) {
      debugf("TW64 GFX_FAIL reason=tileset_rot name=%s\n", pName);
      return -1;
    }
    pSet->m_RotSurface = sprite_get_pixels(pSet->m_pRotSheet);
  }
  return s_NumTilesets++;
}

/* The map's own sky: the first untextured quad layer in the file carries the
 * background gradient every shipped map starts with. Its top edge is the sky
 * proper -- the bottom edge is the horizon wash, which averaged in turns a
 * blue sky lavender -- so the two top corners stand in for the parallax quad
 * groups (clouds, mountains, sun) the port does not render. */
uint32_t ReadSkyColor(IMap *pMap) {
  int GroupStart, NumGroups, LayerStart, NumLayers;
  pMap->GetType(MAPITEMTYPE_GROUP, &GroupStart, &NumGroups);
  pMap->GetType(MAPITEMTYPE_LAYER, &LayerStart, &NumLayers);
  for (int g = 0; g < NumGroups; ++g) {
    CMapItemGroup *pGroup =
        static_cast<CMapItemGroup *>(pMap->GetItem(GroupStart + g, 0, 0));
    for (int l = 0; l < pGroup->m_NumLayers; ++l) {
      const int Index = pGroup->m_StartLayer + l;
      if (Index < 0 || Index >= NumLayers)
        continue;
      CMapItemLayer *pLayer =
          static_cast<CMapItemLayer *>(pMap->GetItem(LayerStart + Index, 0, 0));
      if (pLayer->m_Type != LAYERTYPE_QUADS)
        continue;
      CMapItemLayerQuads *pQuads =
          reinterpret_cast<CMapItemLayerQuads *>(pLayer);
      if (pQuads->m_Image != -1 || pQuads->m_NumQuads < 1)
        continue;
      CQuad *pQuad = static_cast<CQuad *>(pMap->GetDataSwapped(pQuads->m_Data));
      if (!pQuad)
        continue;
      int R = 0, G = 0, B = 0;
      for (int c = 0; c < 2; ++c) {
        R += pQuad->m_aColors[c].r;
        G += pQuad->m_aColors[c].g;
        B += pQuad->m_aColors[c].b;
      }
      return PackColor((uint8_t)clamp(R / 2, 0, 255),
                       (uint8_t)clamp(G / 2, 0, 255),
                       (uint8_t)clamp(B / 2, 0, 255));
    }
  }
  return COLOR_SKY_FALLBACK;
}

/* Name of a map image item, as stored in its own data blob. */
const char *ImageName(IMap *pMap, int ImageIndex) {
  int Start, Num;
  pMap->GetType(MAPITEMTYPE_IMAGE, &Start, &Num);
  if (ImageIndex < 0 || ImageIndex >= Num)
    return 0;
  CMapItemImage *pImage =
      static_cast<CMapItemImage *>(pMap->GetItem(Start + ImageIndex, 0, 0));
  if (!pImage)
    return 0;
  return static_cast<const char *>(pMap->GetData(pImage->m_ImageName));
}

} // namespace

bool Tw64RenderSetMap(IKernel *pKernel, IMap *pMap, const char *pMapName) {
  s_MapReady = false;
  s_NumLayers = 0;
  FreeTilesets();
  s_SkyColor = COLOR_SKY_FALLBACK;
  if (!pMap)
    return false;

  /* A private CLayers: CGameContext keeps its own copy private, and the
   * datafile reader caches decompressed blobs, so this costs one extra walk
   * over the tile arrays at load time and no extra memory. */
  s_MapLayers.Init(pKernel, pMap);
  s_SkyColor = ReadSkyColor(pMap);

  int Dropped = 0;
  bool PassedGameLayer = false;
  for (int g = 0; g < s_MapLayers.NumGroups(); ++g) {
    CMapItemGroup *pGroup = s_MapLayers.GetGroup(g);
    for (int l = 0; l < pGroup->m_NumLayers; ++l) {
      CMapItemLayer *pLayer = s_MapLayers.GetLayer(pGroup->m_StartLayer + l);
      if (pLayer->m_Type != LAYERTYPE_TILES)
        continue;
      CMapItemLayerTilemap *pTilemap =
          reinterpret_cast<CMapItemLayerTilemap *>(pLayer);
      if (pTilemap->m_Flags & TILESLAYERFLAG_GAME) {
        PassedGameLayer = true;
        continue; /* collision layer: never drawn, exactly like the client */
      }
      if (pTilemap->m_Image < 0)
        continue; /* untextured tile layer: nothing to sample */
      if (s_NumLayers >= TW64_MAX_LAYERS) {
        ++Dropped;
        continue;
      }
      const char *pName = ImageName(pMap, pTilemap->m_Image);
      if (!pName)
        continue;

      const CTile *pTiles =
          static_cast<const CTile *>(pMap->GetData(pTilemap->m_Data));
      if (!pTiles)
        continue;
      bool Rotated = false;
      const int Count = pTilemap->m_Width * pTilemap->m_Height;
      for (int i = 0; i < Count; ++i) {
        if (pTiles[i].m_Index && (pTiles[i].m_Flags & TILEFLAG_ROTATE)) {
          Rotated = true;
          break;
        }
      }

      const int Set = AcquireTileset(pName, Rotated);
      if (Set < 0)
        return false;

      CLayerView *pView = &s_aLayers[s_NumLayers++];
      pView->m_pTiles = pTiles;
      pView->m_Width = (int16_t)pTilemap->m_Width;
      pView->m_Height = (int16_t)pTilemap->m_Height;
      pView->m_Tileset = (int8_t)Set;
      pView->m_Detail = (pLayer->m_Flags & LAYERFLAG_DETAIL) != 0;
      pView->m_Foreground = PassedGameLayer;
      pView->m_R = (uint8_t)clamp(pTilemap->m_Color.r, 0, 255);
      pView->m_G = (uint8_t)clamp(pTilemap->m_Color.g, 0, 255);
      pView->m_B = (uint8_t)clamp(pTilemap->m_Color.b, 0, 255);
      pView->m_A = (uint8_t)clamp(pTilemap->m_Color.a, 0, 255);
      pView->m_OffsetX = (float)pGroup->m_OffsetX;
      pView->m_OffsetY = (float)pGroup->m_OffsetY;
      pView->m_ParallaxX = pGroup->m_ParallaxX * 0.01f;
      pView->m_ParallaxY = pGroup->m_ParallaxY * 0.01f;
      pView->m_Clip = pGroup->m_Version >= 2 && pGroup->m_UseClipping != 0;
      pView->m_ClipX = pGroup->m_ClipX;
      pView->m_ClipY = pGroup->m_ClipY;
      pView->m_ClipW = pGroup->m_ClipW;
      pView->m_ClipH = pGroup->m_ClipH;
    }
  }

  s_MapReady = true;
  debugf("TW64 GFX_MAP map=%s layers=%d dropped=%d tilesets=%d sky=%06lx\n",
         pMapName ? pMapName : "?", s_NumLayers, Dropped, s_NumTilesets,
         (unsigned long)(s_SkyColor >> 8));
  return true;
}

namespace {

/* --------------------------------------------------------------------- */
/* Tile layers                                                            */
/* --------------------------------------------------------------------- */

/* Tile TMEM traffic is the renderer's single biggest cost, so tiles do not go
 * through rdpq_tex_upload(): the sheet layout is fixed (16x16 CI8 tiles, 256
 * bytes each, 8-byte aligned rows), which makes every upload one SET_TEXTURE_
 * IMAGE + SET_TILE per *sheet* and a bare LOAD_TILE per *tile*. LOAD_TILE also
 * records the loaded rectangle in the tile descriptor, so draws keep using
 * sheet coordinates; the wrap mask (16 texels) is what lets a run of identical
 * tiles be one rectangle. */
void BindSheet(int Set, bool Rot) {
  if (s_BoundTileset == Set && s_BoundTileRot == Rot)
    return;
  CTileset *pSet = &s_aTilesets[Set];
  if (s_BoundPalette != Set) {
    rdpq_tex_upload_tlut(pSet->m_pPalette, 0, pSet->m_PaletteColors);
    s_BoundPalette = Set;
  }
  rdpq_set_texture_image(Rot ? &pSet->m_RotSurface : &pSet->m_Surface);
  rdpq_tileparms_t Parms;
  memset(&Parms, 0, sizeof(Parms));
  Parms.s.mask = 4; /* wrap every 16 texels, so a run repeats the tile */
  Parms.t.mask = 4;
  rdpq_set_tile(TILE0, FMT_CI8, 0, TILE_TEXELS, &Parms);
  s_BoundTileset = Set;
  s_BoundTileRot = Rot;
  s_BoundTile = -1;
  s_BoundSprite = -1;
  s_BoundSpriteMask = -1;
}

/* Collected visible runs for one layer, sorted by tile before drawing so TMEM
 * is loaded once per *unique* tile rather than once per run. Both arrays are
 * fixed size; an unusually busy window is drawn in several batches. */
enum { TW64_MAX_RUNS = 512, TW64_TILE_KEYS = 512 };

struct CTileRun {
  int16_t m_X0, m_X1, m_Y0, m_Y1;
  uint16_t m_Key; /* tile index | rotated << 8 */
  uint8_t m_Flags;
  uint8_t m_Cols;
  uint8_t m_Rows;
};

CTileRun s_aRuns[TW64_MAX_RUNS];
uint16_t s_aRunOrder[TW64_MAX_RUNS];
uint16_t s_aRunBucket[TW64_TILE_KEYS + 1];
int s_NumRuns;

/* Runs of the row above that a run of this row may still grow into. Both
 * lists are built left to right, so matching them is a merge walk rather than
 * a search. */
enum { TW64_MAX_ROW_RUNS = 48 };
uint16_t s_aOpenRuns[TW64_MAX_ROW_RUNS];
uint16_t s_aNextOpenRuns[TW64_MAX_ROW_RUNS];
int s_NumOpenRuns;
int s_NumNextOpenRuns;

void FlushRuns(int Tileset) {
  if (!s_NumRuns)
    return;
  memset(s_aRunBucket, 0, sizeof(s_aRunBucket));
  for (int i = 0; i < s_NumRuns; ++i)
    ++s_aRunBucket[s_aRuns[i].m_Key + 1];
  for (int k = 1; k <= TW64_TILE_KEYS; ++k)
    s_aRunBucket[k] = (uint16_t)(s_aRunBucket[k] + s_aRunBucket[k - 1]);
  for (int i = 0; i < s_NumRuns; ++i)
    s_aRunOrder[s_aRunBucket[s_aRuns[i].m_Key]++] = (uint16_t)i;

  int LastKey = -1;
  for (int i = 0; i < s_NumRuns; ++i) {
    const CTileRun &Run = s_aRuns[s_aRunOrder[i]];
    if ((int)Run.m_Key != LastKey) {
      const bool Rot = (Run.m_Key & 0x100) != 0;
      const int Index = Run.m_Key & 0xff;
      BindSheet(Tileset, Rot);
      const int Sx = (Index % TILES_PER_ROW) * TILE_TEXELS;
      const int Sy = (Index / TILES_PER_ROW) * TILE_TEXELS;
      rdpq_load_tile(TILE0, Sx, Sy, Sx + TILE_TEXELS, Sy + TILE_TEXELS);
      s_BoundTile = (int)Run.m_Key;
      LastKey = (int)Run.m_Key;
    }
    /* LOAD_TILE anchored the tile at its sheet coordinates, so the draw keeps
     * using them; the run repeats through the wrap mask. */
    const int Index = Run.m_Key & 0xff;
    const int Sx = (Index % TILES_PER_ROW) * TILE_TEXELS;
    const int Sy = (Index / TILES_PER_ROW) * TILE_TEXELS;
    const int SpanX = TILE_TEXELS * Run.m_Cols;
    const int SpanY = TILE_TEXELS * Run.m_Rows;
    const bool FlipX = (Run.m_Flags & 1) != 0;
    const bool FlipY = (Run.m_Flags & 2) != 0;
    rdpq_texture_rectangle_scaled(
        TILE0, Run.m_X0, Run.m_Y0, Run.m_X1, Run.m_Y1,
        FlipX ? Sx + SpanX : Sx, FlipY ? Sy + SpanY : Sy,
        FlipX ? Sx : Sx + SpanX, FlipY ? Sy : Sy + SpanY);
  }
  s_NumRuns = 0;
  s_NumOpenRuns = 0;
  s_NumNextOpenRuns = 0;
}

void DrawTileLayer(const CLayerView &Layer, const CTw64Viewport &View,
                   const CCamera &GameCam) {
  if (Layer.m_Tileset < 0 || !Layer.m_A)
    return;

  CCamera Cam;
  Cam.m_CenterX = GameCam.m_CenterX;
  Cam.m_CenterY = GameCam.m_CenterY;
  Cam.m_X = Layer.m_OffsetX + GameCam.m_X * Layer.m_ParallaxX;
  Cam.m_Y = Layer.m_OffsetY + GameCam.m_Y * Layer.m_ParallaxY;

  int Vx0 = View.m_X0, Vy0 = View.m_Y0, Vx1 = View.m_X1, Vy1 = View.m_Y1;
  if (Layer.m_Clip) {
    /* The clip rectangle is expressed in game-group space (see
     * CMapLayers::OnRender), so it is mapped with the game camera and then
     * intersected with the viewport scissor. */
    const int Cx0 = ScreenX(GameCam, (float)Layer.m_ClipX);
    const int Cy0 = ScreenY(GameCam, (float)Layer.m_ClipY);
    const int Cx1 = ScreenX(GameCam, (float)(Layer.m_ClipX + Layer.m_ClipW));
    const int Cy1 = ScreenY(GameCam, (float)(Layer.m_ClipY + Layer.m_ClipH));
    if (Cx0 > Vx0)
      Vx0 = Cx0;
    if (Cy0 > Vy0)
      Vy0 = Cy0;
    if (Cx1 < Vx1)
      Vx1 = Cx1;
    if (Cy1 < Vy1)
      Vy1 = Cy1;
    if (Vx1 <= Vx0 || Vy1 <= Vy0)
      return;
    rdpq_set_scissor(Vx0, Vy0, Vx1, Vy1);
    SetClip(Vx0, Vy0, Vx1, Vy1);
  }

  const float Left = Cam.m_X + (Vx0 - Cam.m_CenterX) / SCALE;
  const float Right = Cam.m_X + (Vx1 - Cam.m_CenterX) / SCALE;
  const float Top = Cam.m_Y + (Vy0 - Cam.m_CenterY) / SCALE;
  const float Bottom = Cam.m_Y + (Vy1 - Cam.m_CenterY) / SCALE;
  const int Tx0 = (int)floorf(Left / TILE_SIZE);
  const int Tx1 = (int)floorf(Right / TILE_SIZE);
  const int Ty0 = (int)floorf(Top / TILE_SIZE);
  const int Ty1 = (int)floorf(Bottom / TILE_SIZE);

  rdpq_set_prim_color(RGBA32(Layer.m_R, Layer.m_G, Layer.m_B, Layer.m_A));
  if (Layer.m_A == 255) {
    rdpq_mode_blender(0);
    rdpq_mode_alphacompare(1);
  } else {
    rdpq_mode_alphacompare(0);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
  }

  const int W = Layer.m_Width;
  const int H = Layer.m_Height;
  s_NumRuns = 0;
  s_NumOpenRuns = 0;
  for (int Ty = Ty0; Ty <= Ty1; ++Ty) {
    s_NumNextOpenRuns = 0;
    int OpenCursor = 0;
    /* The desktop client renders map layers with TILERENDERFLAG_EXTEND, which
     * clamps the sampled tile so the map's border tiles repeat outwards
     * forever instead of showing a void. */
    const int My = Ty < 0 ? 0 : (Ty >= H ? H - 1 : Ty);
    const int Sy0 = ScreenY(Cam, Ty * TILE_SIZE);
    const int Sy1 = ScreenY(Cam, (Ty + 1) * TILE_SIZE);
    if (Sy1 <= Vy0 || Sy0 >= Vy1) {
      s_NumOpenRuns = 0;
      continue;
    }
    const CTile *pRow = Layer.m_pTiles + My * W;
    int Tx = Tx0;
    while (Tx <= Tx1) {
      const int Mx = Tx < 0 ? 0 : (Tx >= W ? W - 1 : Tx);
      const uint8_t Index = pRow[Mx].m_Index;
      if (!Index) {
        /* m_Skip is the client's own run-length accelerator, filled in by
         * CLayers::InitTilemapSkip: it counts the empty tiles that follow. It
         * is only meaningful inside the layer, not in the extended border. */
        Tx += 1 + (Tx == Mx ? pRow[Mx].m_Skip : 0);
        continue;
      }
      const uint8_t Flags =
          pRow[Mx].m_Flags & (TILEFLAG_VFLIP | TILEFLAG_HFLIP | TILEFLAG_ROTATE);
      int Run = 1;
      while (Tx + Run <= Tx1 && Run < 255) {
        const int Nx = Tx + Run < 0 ? 0 : (Tx + Run >= W ? W - 1 : Tx + Run);
        if (pRow[Nx].m_Index != Index)
          break;
        if ((pRow[Nx].m_Flags &
             (TILEFLAG_VFLIP | TILEFLAG_HFLIP | TILEFLAG_ROTATE)) != Flags)
          break;
        ++Run;
      }
      const int Sx0 = ScreenX(Cam, Tx * TILE_SIZE);
      const int Sx1 = ScreenX(Cam, (Tx + Run) * TILE_SIZE);
      if (Sx1 > Vx0 && Sx0 < Vx1 && Sx1 > Sx0) {
        if (s_NumRuns >= TW64_MAX_RUNS) {
          FlushRuns(Layer.m_Tileset);
          OpenCursor = 0;
        }
        const bool Rot = (Flags & TILEFLAG_ROTATE) != 0;
        /* The client's flip flags are named for the axis they mirror in the
         * unrotated tile; once the tile is served from the pre-rotated sheet
         * the two axes have swapped, so VFLIP becomes a vertical mirror. */
        const bool FlipX = Rot ? (Flags & TILEFLAG_HFLIP) != 0
                               : (Flags & TILEFLAG_VFLIP) != 0;
        const bool FlipY = Rot ? (Flags & TILEFLAG_VFLIP) != 0
                               : (Flags & TILEFLAG_HFLIP) != 0;
        const uint16_t Key = (uint16_t)(Index | (Rot ? 0x100 : 0));
        const uint8_t FlipBits = (uint8_t)((FlipX ? 1 : 0) | (FlipY ? 2 : 0));

        /* Grow the identical run directly above instead of adding a new one.
         * Both wrap masks are set, so one rectangle can tile a whole
         * rectangular block of the same tile -- which is what a floor or a
         * wall of a Teeworlds map mostly is. */
        int Extended = -1;
        while (OpenCursor < s_NumOpenRuns &&
               s_aRuns[s_aOpenRuns[OpenCursor]].m_X0 < (int16_t)Sx0)
          ++OpenCursor;
        if (OpenCursor < s_NumOpenRuns) {
          CTileRun &Above = s_aRuns[s_aOpenRuns[OpenCursor]];
          if (Above.m_X0 == (int16_t)Sx0 && Above.m_X1 == (int16_t)Sx1 &&
              Above.m_Y1 == (int16_t)Sy0 && Above.m_Key == Key &&
              Above.m_Flags == FlipBits && Above.m_Rows < 255) {
            Above.m_Y1 = (int16_t)Sy1;
            ++Above.m_Rows;
            Extended = s_aOpenRuns[OpenCursor];
            ++OpenCursor;
          }
        }
        if (Extended < 0) {
          Extended = s_NumRuns++;
          CTileRun *pRun = &s_aRuns[Extended];
          pRun->m_X0 = (int16_t)Sx0;
          pRun->m_X1 = (int16_t)Sx1;
          pRun->m_Y0 = (int16_t)Sy0;
          pRun->m_Y1 = (int16_t)Sy1;
          pRun->m_Key = Key;
          pRun->m_Flags = FlipBits;
          pRun->m_Cols = (uint8_t)Run;
          pRun->m_Rows = 1;
        }
        if (s_NumNextOpenRuns < TW64_MAX_ROW_RUNS)
          s_aNextOpenRuns[s_NumNextOpenRuns++] = (uint16_t)Extended;
      }
      Tx += Run;
    }
    memcpy(s_aOpenRuns, s_aNextOpenRuns,
           s_NumNextOpenRuns * sizeof(s_aOpenRuns[0]));
    s_NumOpenRuns = s_NumNextOpenRuns;
  }
  FlushRuns(Layer.m_Tileset);

  if (Layer.m_Clip) {
    rdpq_set_scissor(View.m_X0, View.m_Y0, View.m_X1, View.m_Y1);
    SetClip(View.m_X0, View.m_Y0, View.m_X1, View.m_Y1);
  }
}

void DrawTileLayers(const CTw64Viewport &View, const CCamera &Cam,
                    bool Foreground, bool HighDetail) {
  for (int i = 0; i < s_NumLayers; ++i) {
    if (s_aLayers[i].m_Foreground != Foreground)
      continue;
    if (s_aLayers[i].m_Detail && !HighDetail)
      continue;
    DrawTileLayer(s_aLayers[i], View, Cam);
  }
}

/* --------------------------------------------------------------------- */
/* Per-frame match state                                                  */
/* --------------------------------------------------------------------- */

/* Rebuilt once per Tw64RenderMatch() call so the per-viewport passes below
 * never re-walk the entity lists. Fixed size, no allocation. */
int8_t s_aTeam[MAX_CLIENTS];
int8_t s_aSlotInTeam[MAX_CLIENTS];

struct CFlagView {
  bool m_Present;
  bool m_AtStand;
  int m_CarrierID; /* -1 when nobody is carrying it */
  vec2 m_Pos;
  vec2 m_StandPos;
};

CFlagView s_aFlags[2];

void BuildFrameState(CGameContext *pGameServer, const CTw64RenderInfo *pInfo) {
  int aCount[2] = {0, 0};
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    s_aTeam[i] = -1;
    s_aSlotInTeam[i] = 0;
    CPlayer *pPlayer = pGameServer->m_apPlayers[i];
    if (!pPlayer)
      continue;
    const int Team = pPlayer->GetTeam();
    if (Team != TEAM_RED && Team != TEAM_BLUE)
      continue;
    s_aTeam[i] = (int8_t)Team;
    s_aSlotInTeam[i] = (int8_t)(aCount[Team] & 1);
    ++aCount[Team];
  }

  for (int i = 0; i < 2; ++i) {
    s_aFlags[i].m_Present = false;
    s_aFlags[i].m_AtStand = false;
    s_aFlags[i].m_CarrierID = -1;
    s_aFlags[i].m_Pos = vec2(0.0f, 0.0f);
    s_aFlags[i].m_StandPos = vec2(0.0f, 0.0f);
  }
  if (!pInfo->m_FlagMode)
    return;

  for (CEntity *pEntity =
           pGameServer->m_World.FindFirst(CGameWorld::ENTTYPE_FLAG);
       pEntity; pEntity = pEntity->TypeNext()) {
    CFlag *pFlag = static_cast<CFlag *>(pEntity);
    const int Team = pFlag->GetTeam();
    if (Team < 0 || Team > 1)
      continue;
    CFlagView &View = s_aFlags[Team];
    View.m_Present = true;
    View.m_AtStand = pFlag->IsAtStand();
    View.m_Pos = pFlag->GetPos();
    View.m_StandPos = pFlag->GetStandPos();
    CCharacter *pCarrier = pFlag->GetCarrier();
    View.m_CarrierID =
        pCarrier && pCarrier->GetPlayer() ? pCarrier->GetPlayer()->GetCID() : -1;
  }
}

/* Slot colour in free-for-all, team colour (shaded by team slot) in team
 * modes. Everything that tints a tee goes through this. */
uint32_t TeeColor(const CTw64RenderInfo *pInfo, int ClientID) {
  uint8_t R, G, B;
  if (pInfo->m_Teamplay && ClientID >= 0 && ClientID < MAX_CLIENTS &&
      s_aTeam[ClientID] >= 0)
    Tw64TeamColor(s_aTeam[ClientID], s_aSlotInTeam[ClientID], &R, &G, &B);
  else
    Tw64PlayerColor(ClientID, &R, &G, &B);
  return PackColor(R, G, B);
}

/* --------------------------------------------------------------------- */
/* Entities                                                              */
/* --------------------------------------------------------------------- */

/* Sprite and on-screen size per pickup type, in the order of the PICKUP_*
 * enum. Health and armour are the client's default 64-unit quad; the weapon
 * powerups reuse the weapon body at its own visual size, and the ninja is
 * drawn at double the default size, as in CItems::RenderPickup. */
struct CPickupVisual {
  int m_Sprite;
  float m_W;
  float m_H;
};

const CPickupVisual s_aPickupVisuals[NUM_PICKUPS] = {
    {SPR_PICKUP_HEALTH, 16.97f, 16.97f},
    {SPR_PICKUP_ARMOR, 16.97f, 16.97f},
    {SPR_WEAPON_GRENADE, 34.62f, 9.89f},
    {SPR_WEAPON_SHOTGUN, 34.92f, 8.73f},
    {SPR_WEAPON_LASER, 31.71f, 13.59f},
    {SPR_WEAPON_NINJA, 46.56f, 11.64f}};

void DrawPickups(CGameContext *pGameServer, const CCamera &Cam) {
  /* The desktop client bobs pickups on local time; the tick is the port's
   * deterministic equivalent. */
  const float Time = pGameServer->Server()->Tick() /
                     (float)pGameServer->Server()->TickSpeed();
  /* Grouped by type so a map full of pickups costs one TMEM upload per type
   * present rather than one per pickup. */
  for (int Type = 0; Type < NUM_PICKUPS; ++Type) {
    const CPickupVisual &Vis = s_aPickupVisuals[Type];
    for (CEntity *pEntity =
             pGameServer->m_World.FindFirst(CGameWorld::ENTTYPE_PICKUP);
         pEntity; pEntity = pEntity->TypeNext()) {
      CPickup *pPickup = static_cast<CPickup *>(pEntity);
      if (pPickup->GetType() != Type || !pPickup->IsAvailable())
        continue;
      const vec2 Pos = pPickup->GetPos();
      /* Cull before the bob: a 2.5-unit wobble is one screen pixel, so the
       * unbobbed position decides visibility, and an off-screen pickup then
       * costs nothing. Doing this the other way round is what made a map full
       * of pickups the most expensive thing on screen. */
      const int Cx = ScreenX(Cam, Pos.x);
      const int Cy = ScreenY(Cam, Pos.y);
      if (Cx < s_ClipX0 - 32 || Cx > s_ClipX1 + 32 || Cy < s_ClipY0 - 32 ||
          Cy > s_ClipY1 + 32)
        continue;
      const float Phase =
          (Time * 2.0f + Pos.y / 32.0f + Pos.x / 32.0f) * (0.5f / pi);
      DrawSpriteCentered(Vis.m_Sprite, Cx + FastSin(Phase + 0.25f) * 2.5f * SCALE,
                         Cy + FastSin(Phase) * 2.5f * SCALE, Vis.m_W, Vis.m_H,
                         false, false);
    }
  }
}

void DrawProjectiles(CGameContext *pGameServer, const CCamera &Cam) {
  const int Tick = pGameServer->Server()->Tick();
  const float TickSpeed = (float)pGameServer->Server()->TickSpeed();
  for (CEntity *pEntity =
           pGameServer->m_World.FindFirst(CGameWorld::ENTTYPE_PROJECTILE);
       pEntity; pEntity = pEntity->TypeNext()) {
    CProjectile *pProjectile = static_cast<CProjectile *>(pEntity);
    const float Time = (Tick - pProjectile->GetStartTick()) / TickSpeed;
    const vec2 Pos = pProjectile->GetPos(Time);
    const vec2 Prev = pProjectile->GetPos(Time - 0.02f);
    const int Sx = ScreenX(Cam, Pos.x);
    const int Sy = ScreenY(Cam, Pos.y);
    if (Sx < s_ClipX0 - 16 || Sx > s_ClipX1 + 16 || Sy < s_ClipY0 - 16 ||
        Sy > s_ClipY1 + 16)
      continue;
    const int Type = clamp(pProjectile->GetType(), 0, NUM_WEAPONS - 1);
    const int Sprite = s_aProjectileSprites[Type];
    float Theta;
    if (Type == WEAPON_GRENADE) {
      /* The client spins the grenade on local time; the tick keeps it
       * deterministic and still reads as a tumble. */
      Theta = (Tick % 25) * (2.0f * pi / 25.0f) * 2.0f;
    } else {
      const vec2 Vel = Pos - Prev;
      Theta = (Vel.x * Vel.x + Vel.y * Vel.y) > 0.000001f
                  ? atan2f(Vel.y, Vel.x)
                  : 0.0f;
    }
    DrawSpriteRotated(Sprite, (float)Sx, (float)Sy, 12.0f, 12.0f, Theta, false);
  }
}

void DrawLasers(CGameContext *pGameServer, const CCamera &Cam) {
  rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
  for (CEntity *pEntity =
           pGameServer->m_World.FindFirst(CGameWorld::ENTTYPE_LASER);
       pEntity; pEntity = pEntity->TypeNext()) {
    CLaser *pLaser = static_cast<CLaser *>(pEntity);
    const int X0 = ScreenX(Cam, pLaser->GetFrom().x);
    const int Y0 = ScreenY(Cam, pLaser->GetFrom().y);
    const int X1 = ScreenX(Cam, pLaser->GetPos().x);
    const int Y1 = ScreenY(Cam, pLaser->GetPos().y);
    const int Dx = X1 - X0;
    const int Dy = Y1 - Y0;
    const int AbsDx = Dx < 0 ? -Dx : Dx;
    const int AbsDy = Dy < 0 ? -Dy : Dy;
    const int Span = AbsDx > AbsDy ? AbsDx : AbsDy;
    if (Span < 1 || Span > 4 * TW64_SCREEN_W)
      continue;
    int Step = 4;
    if (Span / Step > 48)
      Step = Span / 48 + 1;
    /* Two staircase passes: a wide glow, then a white core, matching the
     * client's outer/inner laser colours. */
    for (int Pass = 0; Pass < 2; ++Pass) {
      const int Thick = Pass ? 1 : 3;
      const uint32_t Color = Pass ? COLOR_LASER_CORE : COLOR_LASER;
      rdpq_set_prim_color(RGBA32((uint8_t)(Color >> 24), (uint8_t)(Color >> 16),
                                 (uint8_t)(Color >> 8), 255));
      for (int Begin = 0; Begin < Span; Begin += Step) {
        int End = Begin + Step;
        if (End > Span)
          End = Span;
        const int Ax = X0 + Dx * Begin / Span;
        const int Ay = Y0 + Dy * Begin / Span;
        const int Bx = X0 + Dx * End / Span;
        const int By = Y0 + Dy * End / Span;
        int Lx = Ax < Bx ? Ax : Bx;
        int Hx = Ax < Bx ? Bx : Ax;
        int Ly = Ay < By ? Ay : By;
        int Hy = Ay < By ? By : Ay;
        if (Hx - Lx < Thick)
          Hx = Lx + Thick;
        if (Hy - Ly < Thick)
          Hy = Ly + Thick;
        FillTextured(Lx, Ly, Hx, Hy);
      }
    }
  }
  rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
  rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
}

/* Flag stand, flag and carrier marker. The stand is a hollow square so an
 * empty stand still tells the player where home is; the flag sprite itself is
 * the desktop client's, drawn as its 42x84 world-unit quad. */
void DrawFlags(const CTw64RenderInfo *pInfo, const CCamera &Cam) {
  for (int Team = 0; Team < 2; ++Team) {
    const CFlagView &View = s_aFlags[Team];
    if (!View.m_Present)
      continue;

    /* Stand outline, in the team colour at half brightness. */
    const int Tx = ScreenX(Cam, View.m_StandPos.x);
    const int Ty = ScreenY(Cam, View.m_StandPos.y);
    if (Tx > s_ClipX0 - 16 && Tx < s_ClipX1 + 16 && Ty > s_ClipY0 - 16 &&
        Ty < s_ClipY1 + 16) {
      uint8_t R, G, B;
      Tw64TeamColor(Team, 0, &R, &G, &B);
      rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
      rdpq_set_prim_color(RGBA32((uint8_t)(R / 2), (uint8_t)(G / 2),
                                 (uint8_t)(B / 2), 255));
      FillTextured(Tx - 5, Ty + 3, Tx + 6, Ty + 5);
      FillTextured(Tx - 5, Ty - 5, Tx - 3, Ty + 5);
      FillTextured(Tx + 3, Ty - 5, Tx + 6, Ty + 5);
      rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
      rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
    }

    /* The flag body. m_Pos already tracks the carrier; the client anchors the
     * 42x84 quad at Pos.y - 31.5 world units. */
    const float Fx = (float)ScreenX(Cam, View.m_Pos.x);
    const float Fy = (float)ScreenY(Cam, View.m_Pos.y - 31.5f);
    DrawSpriteCentered(Team == TEAM_RED ? SPR_FLAG_RED : SPR_FLAG_BLUE, Fx, Fy,
                       15.75f, 31.5f, false, false);
  }
}

/* --------------------------------------------------------------------- */
/* Tees                                                                   */
/* --------------------------------------------------------------------- */

/* The desktop walk cycle, straight out of datasrc/content.py. Offsets are in
 * "size 64" units, which at m_Size = 64 are world units; angles are turns. */
struct CAnimKey {
  float m_Time;
  float m_X;
  float m_Y;
};

const CAnimKey s_aWalkBody[6] = {{0.0f, 0.0f, 0.0f},  {0.2f, 0.0f, -1.0f},
                                 {0.4f, 0.0f, 0.0f},  {0.6f, 0.0f, 0.0f},
                                 {0.8f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f}};
const CAnimKey s_aWalkBackFoot[6] = {
    {0.0f, 8.0f, 0.0f},   {0.2f, -8.0f, 0.0f}, {0.4f, -10.0f, -4.0f},
    {0.6f, -8.0f, -8.0f}, {0.8f, 4.0f, -4.0f}, {1.0f, 8.0f, 0.0f}};
const CAnimKey s_aWalkFrontFoot[6] = {
    {0.0f, -10.0f, -4.0f}, {0.2f, -8.0f, -8.0f}, {0.4f, 4.0f, -4.0f},
    {0.6f, 8.0f, 0.0f},    {0.8f, 8.0f, 0.0f},   {1.0f, -10.0f, -4.0f}};

void EvalAnim(const CAnimKey *pKeys, float Time, float *pX, float *pY) {
  int i = 0;
  while (i < 5 && pKeys[i + 1].m_Time < Time)
    ++i;
  const CAnimKey &A = pKeys[i];
  const CAnimKey &B = pKeys[i + 1];
  const float Span = B.m_Time - A.m_Time;
  const float T = Span > 0.0001f ? clamp((Time - A.m_Time) / Span, 0.0f, 1.0f)
                                 : 0.0f;
  *pX = A.m_X + (B.m_X - A.m_X) * T;
  *pY = A.m_Y + (B.m_Y - A.m_Y) * T;
}

void DrawTee(CGameContext *pGameServer, const CTw64RenderInfo *pInfo,
             int ClientID, CCharacter *pCharacter, const CCamera &Cam) {
  const vec2 Pos = pCharacter->GetPos();
  const vec2 Vel = pCharacter->GetVelocity();
  const CNetObj_PlayerInput &Input = pCharacter->GetInput();

  float Ax = (float)Input.m_TargetX;
  float Ay = (float)Input.m_TargetY;
  const float Length = sqrtf(Ax * Ax + Ay * Ay);
  if (Length > 0.001f) {
    Ax /= Length;
    Ay /= Length;
  } else {
    Ax = 1.0f;
    Ay = 0.0f;
  }

  /* Animation selection, exactly as CPlayers::RenderPlayer does it. */
  const bool InAir = !pGameServer->Collision()->CheckPoint(Pos.x, Pos.y + 16.0f);
  const bool Stationary = Vel.x <= 1.0f && Vel.x >= -1.0f;
  const bool WantOtherDir = (Input.m_Direction == -1 && Vel.x > 0.0f) ||
                            (Input.m_Direction == 1 && Vel.x < 0.0f);

  float BodyX = 0.0f, BodyY = -4.0f; /* the base animation */
  float BackX = 0.0f, BackY = 10.0f;
  float FrontX = 0.0f, FrontY = 10.0f;
  if (InAir) {
    BackX -= 3.0f;
    FrontX += 3.0f;
  } else if (Stationary) {
    BackX -= 7.0f;
    FrontX += 7.0f;
  } else if (!WantOtherDir) {
    /* The desktop walk cycle is driven by world X modulo 100 units, not by
     * time, so a tee's feet stay in phase with the ground it covers. */
    const float Wrapped = Pos.x * 0.01f;
    const float WalkTime = Wrapped - floorf(Wrapped);
    float Dx, Dy;
    EvalAnim(s_aWalkBody, WalkTime, &Dx, &Dy);
    BodyX += Dx;
    BodyY += Dy;
    EvalAnim(s_aWalkBackFoot, WalkTime, &Dx, &Dy);
    BackX += Dx;
    BackY += Dy;
    EvalAnim(s_aWalkFrontFoot, WalkTime, &Dx, &Dy);
    FrontX += Dx;
    FrontY += Dy;
  }

  const float BodyCx = (float)ScreenX(Cam, Pos.x + BodyX);
  const float BodyCy = (float)ScreenY(Cam, Pos.y + BodyY);
  const float FootW = 64.0f / 2.1f * SCALE;
  const uint32_t Color = TeeColor(pInfo, ClientID);
  const uint8_t R = (uint8_t)(Color >> 24);
  const uint8_t G = (uint8_t)(Color >> 16);
  const uint8_t B = (uint8_t)(Color >> 8);
  /* The desktop default skin uses a darker lightness for the feet than for the
   * body; two thirds of the body colour is the same idea at one multiply. */
  const uint8_t Fr = (uint8_t)(R * 2 / 3);
  const uint8_t Fg = (uint8_t)(G * 2 / 3);
  const uint8_t Fb = (uint8_t)(B * 2 / 3);

  /* Draw order is the desktop client's: back foot, body, eyes, front foot. */
  rdpq_set_prim_color(RGBA32(Fr, Fg, Fb, 255));
  DrawSubCentered(SPR_TEE_PARTS, TEE_FOOT_SX, TEE_FOOT_SY, TEE_FOOT_SW,
                  TEE_FOOT_SH, (float)ScreenX(Cam, Pos.x + BackX),
                  (float)ScreenY(Cam, Pos.y + BackY), FootW, FootW, false,
                  false);

  rdpq_set_prim_color(RGBA32(R, G, B, 255));
  DrawSubCentered(SPR_TEE_PARTS, TEE_BODY_SX, TEE_BODY_SY, TEE_BODY_SW,
                  TEE_BODY_SH, BodyCx, BodyCy, 64.0f * SCALE, 64.0f * SCALE,
                  false, false);

  /* Eyes: 60% of the body size wide, half that tall, nudged towards the aim. */
  rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
  DrawSubCentered(SPR_TEE_PARTS, TEE_EYES_SX, TEE_EYES_SY, TEE_EYES_SW,
                  TEE_EYES_SH, BodyCx + Ax * 8.0f * SCALE,
                  BodyCy + (-3.2f + Ay * 6.4f) * SCALE, 64.0f * 0.60f * SCALE,
                  64.0f * 0.30f * SCALE, false, false);

  rdpq_set_prim_color(RGBA32(Fr, Fg, Fb, 255));
  DrawSubCentered(SPR_TEE_PARTS, TEE_FOOT_SX, TEE_FOOT_SY, TEE_FOOT_SW,
                  TEE_FOOT_SH, (float)ScreenX(Cam, Pos.x + FrontX),
                  (float)ScreenY(Cam, Pos.y + FrontY), FootW, FootW, false,
                  false);
  rdpq_set_prim_color(RGBA32(255, 255, 255, 255));

  /* Weapon in hand. */
  const int Weapon = clamp(pCharacter->GetActiveWeapon(), 0, NUM_WEAPONS - 1);
  const CWeaponVisual &Vis = s_aWeaponVisuals[Weapon];
  const bool FlipY = Ax < 0.0f;
  float Wx, Wy, Theta;
  if (Weapon == WEAPON_HAMMER || Weapon == WEAPON_NINJA) {
    /* Both are held upright and only mirror with the aim direction. */
    Wx = Pos.x - (FlipY ? Vis.m_OffsetX : 0.0f);
    Wy = Pos.y + Vis.m_OffsetY;
    Theta = -pi / 2.0f;
  } else {
    /* Recoil: the client pulls the weapon back for five ticks after a shot. */
    const float RecoilTick =
        (pGameServer->Server()->Tick() - pCharacter->GetAttackTick()) / 5.0f;
    const float Recoil =
        RecoilTick >= 0.0f && RecoilTick < 1.0f ? sinf(RecoilTick * pi) : 0.0f;
    Wx = Pos.x + Ax * (Vis.m_OffsetX - Recoil * 10.0f);
    Wy = Pos.y + Ay * (Vis.m_OffsetX - Recoil * 10.0f) + Vis.m_OffsetY;
    Theta = atan2f(Ay, Ax);
  }
  DrawSpriteRotated(Vis.m_Sprite, (float)ScreenX(Cam, Wx),
                    (float)ScreenY(Cam, Wy), Vis.m_W, Vis.m_H, Theta, FlipY);

  /* Muzzle flash, for the two weapons the desktop client gives one to. */
  if (Weapon == WEAPON_GUN || Weapon == WEAPON_SHOTGUN) {
    const int Since = pGameServer->Server()->Tick() - pCharacter->GetAttackTick();
    if (Since >= 0 && Since < 3) {
      const int Base = Weapon == WEAPON_GUN ? SPR_MUZZLE_GUN1 : SPR_MUZZLE_SHOTGUN1;
      const float MuzzleX = Weapon == WEAPON_GUN ? 50.0f : 70.0f;
      const float MuzzleY = (Weapon == WEAPON_GUN ? 6.0f : 6.0f) * (FlipY ? 1.0f : -1.0f);
      const float Mx = Wx + Ax * MuzzleX + (-Ay) * MuzzleY;
      const float My = Wy + Ay * MuzzleX + Ax * MuzzleY;
      DrawSpriteRotated(Base + (Since % 3), (float)ScreenX(Cam, Mx),
                        (float)ScreenY(Cam, My), Vis.m_W, Vis.m_H * 1.5f, Theta,
                        FlipY);
    }
  }
}

void DrawHooks(CGameContext *pGameServer, const CCamera &Cam) {
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    CCharacter *pCharacter = pGameServer->GetPlayerChar(i);
    if (!pCharacter)
      continue;
    const int HookState = pCharacter->GetHookState();
    if (HookState != HOOK_FLYING && HookState != HOOK_GRABBED)
      continue;
    const vec2 Pos = pCharacter->GetPos();
    const vec2 HookPos = pCharacter->GetHookPos();
    const int X0 = ScreenX(Cam, Pos.x);
    const int Y0 = ScreenY(Cam, Pos.y);
    const int X1 = ScreenX(Cam, HookPos.x);
    const int Y1 = ScreenY(Cam, HookPos.y);
    const int Dx = X1 - X0;
    const int Dy = Y1 - Y0;
    const float Len = sqrtf((float)(Dx * Dx + Dy * Dy));
    if (Len > 4.0f * TW64_SCREEN_W)
      continue;
    /* The desktop client repeats the chain sprite along the rope; at this
     * scale a link every five pixels reads as a chain without costing dozens
     * of primitives. */
    const int Links = (int)(Len / 5.0f);
    for (int l = 1; l <= Links; ++l) {
      const float T = (float)l / (float)(Links + 1);
      DrawSpriteCentered(SPR_HOOK_CHAIN, X0 + Dx * T, Y0 + Dy * T, 5.0f, 5.0f,
                         false, false);
    }
    DrawSpriteRotated(SPR_HOOK_HEAD, (float)X1, (float)Y1, 8.0f, 4.0f,
                      Len > 0.5f ? atan2f((float)Dy, (float)Dx) : 0.0f, false);
  }
}

void DrawCharacters(CGameContext *pGameServer, const CTw64RenderInfo *pInfo,
                    const CCamera &Cam) {
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    CCharacter *pCharacter = pGameServer->GetPlayerChar(i);
    if (!pCharacter)
      continue;
    const vec2 Pos = pCharacter->GetPos();
    const int Sx = ScreenX(Cam, Pos.x);
    const int Sy = ScreenY(Cam, Pos.y);
    if (Sx < s_ClipX0 - 32 || Sx > s_ClipX1 + 32 || Sy < s_ClipY0 - 32 ||
        Sy > s_ClipY1 + 32)
      continue;
    DrawTee(pGameServer, pInfo, i, pCharacter, Cam);
  }
}

/* True while this client is carrying either flag. */
bool IsFlagCarrier(int ClientID) {
  return s_aFlags[0].m_CarrierID == ClientID ||
         s_aFlags[1].m_CarrierID == ClientID;
}

/* --------------------------------------------------------------------- */
/* HUD                                                                    */
/* --------------------------------------------------------------------- */

void DrawHudPanel(const CTw64RenderInfo *pInfo,
                  const CTw64Viewport &View) {
  /* HUD backdrop so the icons and text stay readable over bright tiles. It is
   * a dark grey rather than black on purpose: the desktop client's "empty"
   * heart and shield icons are near-black outlines and would disappear. */
  Fill(View.m_X0, View.m_Y0, View.m_X0 + 87,
       View.m_Y0 + (pInfo->m_FlagMode ? 47 : 38), COLOR_HUD_BACK);
}

/* A split needs only one quiet seam. Drawing it once avoids the doubled shared
 * edges and outer frame produced by per-viewport borders, while preserving a
 * clear boundary when adjacent cameras show similar map tiles. */
void DrawViewportDividers(int NumViewports) {
  if (NumViewports == 2) {
    Fill(0, TW64_SCREEN_H / 2, TW64_SCREEN_W, TW64_SCREEN_H / 2 + 1,
         COLOR_VIEWPORT_DIVIDER);
  } else if (NumViewports >= 3) {
    Fill(TW64_SCREEN_W / 2, 0, TW64_SCREEN_W / 2 + 1, TW64_SCREEN_H,
         COLOR_VIEWPORT_DIVIDER);
    Fill(0, TW64_SCREEN_H / 2, TW64_SCREEN_W, TW64_SCREEN_H / 2 + 1,
         COLOR_VIEWPORT_DIVIDER);
  }
}

/* Ten hearts and ten shields, the desktop HUD's own full/empty icon rows. */
void DrawHudIcons(CGameContext *pGameServer, const CTw64Viewport &View) {
  CCharacter *pCharacter = pGameServer->GetPlayerChar(View.m_ClientID);
  const int Health = pCharacter ? clamp(pCharacter->GetHealth(), 0, 10) : 0;
  const int Armor = pCharacter ? clamp(pCharacter->GetArmor(), 0, 10) : 0;
  const int X = View.m_X0 + 4;
  for (int Pass = 0; Pass < 2; ++Pass) {
    const int Value = Pass ? Armor : Health;
    const int Full = Pass ? SPR_HUD_ARMOR_FULL : SPR_HUD_HEALTH_FULL;
    const int Empty = Pass ? SPR_HUD_ARMOR_EMPTY : SPR_HUD_HEALTH_EMPTY;
    const int Y = View.m_Y0 + 2 + Pass * 8;
    /* Each row is ten copies of one 8x8 icon, so it is drawn as a single
     * rectangle that wraps the texture ten times instead of ten rectangles. */
    if (Value > 0) {
      BindSprite(Full, 3);
      rdpq_texture_rectangle_scaled(TILE0, X, Y, X + Value * 8, Y + 8, 0, 0,
                                    Value * 8, 8);
    }
    if (Value < 10) {
      BindSprite(Empty, 3);
      rdpq_texture_rectangle_scaled(TILE0, X + Value * 8, Y, X + 80, Y + 8, 0,
                                    0, (10 - Value) * 8, 8);
    }
  }
}

/* Name/score, weapon/ammo and the CTF carrier line are three rows nine pixels
 * apart in one colour each. They go out as ONE paragraph with per-row style
 * escapes rather than three: a paragraph costs a full RDP mode setup, so three
 * viewport HUDs saved here are worth more than every glyph on the screen. */
void DrawHudText(surface_t *pDisp, CGameContext *pGameServer,
                 const CTw64RenderInfo *pInfo, const CTw64Viewport &View) {
  (void)pDisp;
  char aBuf[80];
  char aLine[32];
  CRgb aColors[3];
  if (pInfo->m_Teamplay && View.m_ClientID >= 0 &&
      View.m_ClientID < MAX_CLIENTS && s_aTeam[View.m_ClientID] >= 0)
    Tw64TeamColor(s_aTeam[View.m_ClientID], s_aSlotInTeam[View.m_ClientID],
                  &aColors[0].m_R, &aColors[0].m_G, &aColors[0].m_B);
  else
    Tw64PlayerColor(View.m_ClientID, &aColors[0].m_R, &aColors[0].m_G,
                    &aColors[0].m_B);
  aColors[1].m_R = aColors[1].m_G = aColors[1].m_B = 200;
  aColors[2].m_R = 255;
  aColors[2].m_G = 240;
  aColors[2].m_B = 120;

  const CPlayer *pPlayer = pGameServer->m_apPlayers[View.m_ClientID];
  const char *pName = View.m_ClientID < pInfo->m_NumPlayers
                          ? pInfo->m_apPlayerNames[View.m_ClientID]
                          : "?";
  str_format(aBuf, sizeof(aBuf), "^00%s %d", pName,
             pPlayer ? pPlayer->m_Score : 0);

  CCharacter *pCharacter = pGameServer->GetPlayerChar(View.m_ClientID);
  if (!pCharacter)
    str_copy(aLine, "\n^01DEAD", sizeof(aLine));
  else if (pCharacter->GetActiveWeaponAmmo() < 0)
    /* Hammer and ninja report -1, which means unlimited. */
    str_format(aLine, sizeof(aLine), "\n^01%s --",
               WeaponShortName(pCharacter->GetActiveWeapon()));
  else
    str_format(aLine, sizeof(aLine), "\n^01%s %d",
               WeaponShortName(pCharacter->GetActiveWeapon()),
               pCharacter->GetActiveWeaponAmmo());
  str_append(aBuf, aLine, sizeof(aBuf));

  /* CTF: the one piece of state a carrier must never miss. */
  if (pInfo->m_FlagMode) {
    if (IsFlagCarrier(View.m_ClientID)) {
      str_append(aBuf, "\n^02HAS FLAG", sizeof(aBuf));
    } else if (s_aFlags[0].m_CarrierID >= 0 || s_aFlags[1].m_CarrierID >= 0) {
      aColors[2].m_R = 200;
      aColors[2].m_G = 160;
      aColors[2].m_B = 90;
      str_append(aBuf, "\n^02FLAG OUT", sizeof(aBuf));
    }
  }

  DrawTextRuns(TW64_FONT_HUD, View.m_X0 + 4, View.m_Y0 + 19, aBuf, aColors, 3,
               ALIGN_LEFT, TW64_TEXT_BOX_W, 9 - TW64_FONT_LINE_H);
}

/* Flag-status pip: solid at home, solid with a white core while carried, a
 * hollow frame while lying dropped on the map. */
void DrawFlagPip(int X, int Y, int Team) {
  const CFlagView &View = s_aFlags[Team];
  uint8_t R, G, B;
  Tw64TeamColor(Team, 0, &R, &G, &B);
  const uint32_t Color = PackColor(R, G, B);
  if (!View.m_Present) {
    Fill(X, Y, X + 8, Y + 8, COLOR_PANEL);
    return;
  }
  if (View.m_AtStand || View.m_CarrierID >= 0) {
    Fill(X, Y, X + 8, Y + 8, Color);
    if (View.m_CarrierID >= 0)
      Fill(X + 2, Y + 2, X + 6, Y + 6, COLOR_WHITE);
    return;
  }
  Fill(X, Y, X + 8, Y + 1, Color);
  Fill(X, Y + 7, X + 8, Y + 8, Color);
  Fill(X, Y, X + 1, Y + 8, Color);
  Fill(X + 7, Y, X + 8, Y + 8, Color);
}

void DrawScoreboardText(surface_t *pDisp, CGameContext *pGameServer,
                        const CTw64RenderInfo *pInfo, int BoxX, int BoxY) {
  char aHeader[32];
  if (pInfo->m_Teamplay)
    str_format(aHeader, sizeof(aHeader), "RED %d  BLUE %d",
               pInfo->m_aTeamScore[0] / pInfo->m_TeamScoreDivisor,
               pInfo->m_aTeamScore[1] / pInfo->m_TeamScoreDivisor);
  else
    str_copy(aHeader, "SCORES", sizeof(aHeader));
  Tw64RenderText(pDisp, BoxX + 8, BoxY + 4, aHeader, 255, 255, 255);

  int Row = 0;
  char aRows[176];
  CRgb aColors[TW64_MAX_VIEWPORTS];
  aRows[0] = 0;
  /* Selection sort over at most four slots: no allocation, no recursion. In
   * team modes the primary key is the team, so teammates stay grouped. */
  bool aUsed[TW64_MAX_VIEWPORTS] = {false, false, false, false};
  for (int Rank = 0; Rank < pInfo->m_NumPlayers && Rank < TW64_MAX_VIEWPORTS;
       ++Rank) {
    int Best = -1;
    for (int i = 0; i < pInfo->m_NumPlayers && i < TW64_MAX_VIEWPORTS; ++i) {
      if (aUsed[i] || !pGameServer->m_apPlayers[i])
        continue;
      if (Best < 0) {
        Best = i;
        continue;
      }
      if (pInfo->m_Teamplay && s_aTeam[i] != s_aTeam[Best]) {
        if (s_aTeam[i] < s_aTeam[Best])
          Best = i;
        continue;
      }
      if (pGameServer->m_apPlayers[i]->m_Score >
          pGameServer->m_apPlayers[Best]->m_Score)
        Best = i;
    }
    if (Best < 0)
      break;
    aUsed[Best] = true;
    if (pInfo->m_Teamplay && s_aTeam[Best] >= 0)
      Tw64TeamColor(s_aTeam[Best], s_aSlotInTeam[Best], &aColors[Row].m_R,
                    &aColors[Row].m_G, &aColors[Row].m_B);
    else
      Tw64PlayerColor(Best, &aColors[Row].m_R, &aColors[Row].m_G,
                      &aColors[Row].m_B);
    char aLine[40];
    str_format(aLine, sizeof(aLine), "%s^%02X%-10s %3d", Row ? "\n" : "", Row,
               pInfo->m_apPlayerNames[Best],
               pGameServer->m_apPlayers[Best]->m_Score);
    str_append(aRows, aLine, sizeof(aRows));
    ++Row;
  }
  /* The rows are one paragraph twelve pixels apart, one style per slot. */
  if (Row > 0)
    DrawTextRuns(TW64_FONT_HUD, BoxX + 8, BoxY + 20, aRows, aColors, Row,
                 ALIGN_LEFT, TW64_TEXT_BOX_W, 12 - TW64_FONT_LINE_H);
}

/* --------------------------------------------------------------------- */
/* Menu backdrop                                                          */
/* --------------------------------------------------------------------- */

/* The menu page is the desktop client's own themed menu, reduced to what a
 * 320x240 framebuffer can hold: a night sky, two drifting clouds, a mountain
 * silhouette and the official logo. Every layer is the shipped desktop
 * artwork; the darkening is a primitive-colour modulate at draw time, not a
 * recoloured asset, so the shipped texels stay faithful and the mood can be
 * retuned with a relink instead of an asset rebuild. */
enum {
  MENU_SKY_BANDS = 48,
  MENU_LOGO_W = 256,
  MENU_LOGO_H = 64,
  MENU_LOGO_Y = 2,
  /* The mountain texture is 160x80 and is drawn doubled, so its peaks land
   * just under the page heading and its body runs off the bottom edge. */
  MENU_MOUNTAIN_Y = 112,
  MENU_CLOUD1_W = 112,
  MENU_CLOUD1_H = 48,
  MENU_CLOUD1_Y = 14,
  MENU_CLOUD2_W = 80,
  MENU_CLOUD2_H = 64,
  MENU_CLOUD2_Y = 92,
  /* Starting offsets along each cloud's own period. A cloud crosses the
   * screen in about forty seconds, so without a phase the first minute of the
   * menu would show an empty sky. */
  MENU_CLOUD1_PHASE = 196,
  MENU_CLOUD2_PHASE = 150
};

const CRgb MENU_SKY_TOP = {8, 12, 28};
const CRgb MENU_SKY_BOTTOM = {36, 62, 106};

/* Axis-aligned blit of a menu sprite. These four sprites are all larger than
 * TMEM, so they must go through rdpq_sprite_blit -- which slices them into
 * TMEM-sized strips -- rather than the BindSprite() fast path the in-match
 * sprites use. */
void DrawUiSprite(int Id, int X, int Y, int W, int H) {
  if (!s_apSprites[Id])
    return;
  rdpq_blitparms_t Parms;
  memset(&Parms, 0, sizeof(Parms));
  Parms.scale_x = (float)W / (float)s_aSpriteSurfaces[Id].width;
  Parms.scale_y = (float)H / (float)s_aSpriteSurfaces[Id].height;
  rdpq_sprite_blit(s_apSprites[Id], X, Y, &Parms);
  s_BoundSprite = -1;
  s_BoundSpriteMask = -1;
  s_BoundTileset = -1;
  s_BoundTile = -1;
  s_BoundPalette = -1;
}

/* Enter the textured phase: one combiner and one blend policy for the whole
 * world pass, so only the TLUT and the primitive colour move afterwards. */
void BeginTexturedPhase(bool Tlut) {
  rdpq_set_mode_standard();
  rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
  rdpq_mode_alphacompare(1);
  rdpq_mode_tlut(Tlut ? TLUT_RGBA16 : TLUT_NONE);
  rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
  s_BoundSprite = -1;
  s_BoundSpriteMask = -1;
  s_BoundTileset = -1;
  s_BoundTile = -1;
  s_BoundPalette = -1;
}

} // namespace

/* --------------------------------------------------------------------- */
/* Public interface                                                       */
/* --------------------------------------------------------------------- */

void Tw64RenderInit(void) {
  memset(&s_SpriteParms, 0, sizeof(s_SpriteParms));
  for (int i = 0; i < TW64_SIN_TABLE; ++i)
    s_aSinTable[i] = sinf(i * (2.0f * pi / TW64_SIN_TABLE));

  /* The RDP text fonts, converted by mkfont into font64 by the `font` rule in
   * n64/Makefile:
   *
   *   HUD    monogram (CC0), monochrome, ASCII only, one 32x22 1bpp atlas, so
   *          the per-paragraph TMEM upload is a rounding error. This is the
   *          only font drawn inside a match, and it is the one the frame
   *          budget was measured with.
   *   MENU   DejaVu Sans 12 with a one-pixel outline -- the desktop client's
   *          own UI typeface -- for the menu entries and page headings.
   *   SMALL  the same face at 9, for notes, help lines and the footer.
   *
   * asset_load() asserts on a missing file, so probe first and fall back
   * loudly rather than dying inside libdragon: a missing UI font degrades to
   * the HUD font, a missing HUD font to libdragon's builtin. */
  static const char *const s_apFontFiles[TW64_NUM_FONTS] = {
      "rom:/ui/monogram.font64", "rom:/ui/menu.font64",
      "rom:/ui/menu_small.font64"};
  for (int i = 0; i < TW64_NUM_FONTS; ++i) {
    FILE *pFontFile = fopen(s_apFontFiles[i], "rb");
    if (pFontFile) {
      fclose(pFontFile);
      s_aFonts[i].m_pFont = rdpq_font_load(s_apFontFiles[i]);
    } else {
      debugf("TW64 GFX_FONT missing=%s\n", s_apFontFiles[i]);
      s_aFonts[i].m_pFont = i == TW64_FONT_HUD
                                ? rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO)
                                : s_aFonts[TW64_FONT_HUD].m_pFont;
    }
    s_aFonts[i].m_Baseline = 8;
    rdpq_font_gmetrics_t Metrics;
    const bool HaveMetrics =
        rdpq_font_get_glyph_metrics(s_aFonts[i].m_pFont, 'A', &Metrics);
    if (HaveMetrics)
      s_aFonts[i].m_Baseline = -Metrics.y0;
    /* Registering the same font twice is harmless and keeps the id/table
     * mapping trivial even when a UI font fell back to the HUD one. */
    rdpq_text_register_font(TW64_FONT_ID + i, s_aFonts[i].m_pFont);
    debugf("TW64 GFX_FONT id=%d file=%s advance=%d baseline=%d\n", i,
           s_apFontFiles[i], HaveMetrics ? (int)Metrics.xadvance : -1,
           s_aFonts[i].m_Baseline);
  }

  int Failures = 0;
  for (int i = 0; i < NUM_SPRITES; ++i) {
    s_apSprites[i] = sprite_load(s_apSpriteFiles[i]);
    if (!s_apSprites[i]) {
      debugf("TW64 GFX_FAIL reason=sprite file=%s\n", s_apSpriteFiles[i]);
      ++Failures;
      continue;
    }
    s_aSpriteSurfaces[i] = sprite_get_pixels(s_apSprites[i]);
  }
  memset(s_aTilesets, 0, sizeof(s_aTilesets));
  s_NumTilesets = 0;
  s_NumLayers = 0;
  s_MapReady = false;
  s_SkyColor = COLOR_SKY_FALLBACK;
  SetClip(0, 0, TW64_SCREEN_W, TW64_SCREEN_H);
  debugf("TW64 GFX_OK sprites=%d failures=%d\n", (int)NUM_SPRITES, Failures);
}

void Tw64PlayerColor(int ClientID, uint8_t *pR, uint8_t *pG, uint8_t *pB) {
  const CRgb &Color = s_aPlayerColors[ClientID >= 0
                                          ? ClientID % TW64_MAX_VIEWPORTS
                                          : 0];
  *pR = Color.m_R;
  *pG = Color.m_G;
  *pB = Color.m_B;
}

void Tw64TeamColor(int Team, int SlotInTeam, uint8_t *pR, uint8_t *pG,
                   uint8_t *pB) {
  const CRgb &Color = s_aaTeamColors[Team & 1][SlotInTeam & 1];
  *pR = Color.m_R;
  *pG = Color.m_G;
  *pB = Color.m_B;
}

/* Ends a page and schedules the flip on RDP completion instead of draining the
 * RDP on the CPU. Every page draws its text through the RDP font, so nothing
 * needs the framebuffer back. */
void Tw64RenderEndPage(void) { rdpq_detach_show(); }

void Tw64RenderShade(int X, int Y, int W, int H, uint8_t R, uint8_t G,
                     uint8_t B, uint8_t A) {
  if (W <= 0 || H <= 0)
    return;
  rdpq_set_mode_standard();
  rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
  rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
  rdpq_mode_dithering(DITHER_SQUARE_NONE);
  rdpq_set_prim_color(RGBA32(R, G, B, A));
  rdpq_fill_rectangle(X, Y, X + W, Y + H);
  s_HasColor = false;
  s_BoundSprite = -1;
  s_BoundSpriteMask = -1;
  s_BoundTileset = -1;
  s_BoundTile = -1;
  s_BoundPalette = -1;
}

void Tw64RenderMapPreview(int MapIndex, int X, int Y) {
  if (MapIndex < 0 || MapIndex >= NUM_MAP_PREVIEWS)
    return;
  /* Each map owns its CI8 palette. rdpq_sprite_blit uploads and activates that
   * palette, then slices the 128x96 image through TMEM just like the larger
   * menu backdrop sprites. The preview is stored at its screen size, so this
   * path performs no filtering or rescaling on target. */
  BeginTexturedPhase(false);
  rdpq_mode_dithering(DITHER_SQUARE_NONE);
  DrawUiSprite(SPR_MAP_PREVIEW_FIRST + MapIndex, X, Y, 128, 96);
}

void Tw64RenderBeginMenuPage(surface_t *pDisp, int Frame, bool ShowLogo) {
  rdpq_attach(pDisp, NULL);
  SetClip(0, 0, TW64_SCREEN_W, TW64_SCREEN_H);
  rdpq_set_scissor(0, 0, TW64_SCREEN_W, TW64_SCREEN_H);

  /* Sky. Fill mode cannot dither, and a 16-bit framebuffer turns a smooth
   * ramp into a dozen visible steps, so the bands are drawn in standard mode
   * with the flat combiner and the RDP's "magic square" dithering, which
   * scatters the 5-bit steps into noise the VI filter then smooths. */
  rdpq_set_mode_standard();
  rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
  rdpq_mode_dithering(DITHER_SQUARE_NONE);
  for (int i = 0; i < MENU_SKY_BANDS; ++i) {
    const int Y0 = i * TW64_SCREEN_H / MENU_SKY_BANDS;
    const int Y1 = (i + 1) * TW64_SCREEN_H / MENU_SKY_BANDS;
    const int Num = MENU_SKY_BANDS - 1;
    const uint8_t R = (uint8_t)(MENU_SKY_TOP.m_R +
                                (MENU_SKY_BOTTOM.m_R - MENU_SKY_TOP.m_R) * i /
                                    Num);
    const uint8_t G = (uint8_t)(MENU_SKY_TOP.m_G +
                                (MENU_SKY_BOTTOM.m_G - MENU_SKY_TOP.m_G) * i /
                                    Num);
    const uint8_t B = (uint8_t)(MENU_SKY_TOP.m_B +
                                (MENU_SKY_BOTTOM.m_B - MENU_SKY_TOP.m_B) * i /
                                    Num);
    rdpq_set_prim_color(RGBA32(R, G, B, 0xff));
    rdpq_fill_rectangle(0, Y0, TW64_SCREEN_W, Y1);
  }

  /* Parallax layers. Both clouds cross the screen in well under a minute and
   * wrap through one period, so a single rectangle per cloud is enough and
   * there is never a seam to hide. */
  BeginTexturedPhase(false);
  rdpq_mode_dithering(DITHER_SQUARE_NONE);

  /* Back to front: the far cloud, the near cloud, then the mountains, which
   * are the nearest layer and cut both clouds off at the horizon. */
  const int Period1 = TW64_SCREEN_W + MENU_CLOUD1_W;
  const int Period2 = TW64_SCREEN_W + MENU_CLOUD2_W;
  rdpq_set_prim_color(RGBA32(46, 55, 78, 0xff));
  DrawUiSprite(SPR_UI_CLOUD2,
               (MENU_CLOUD2_PHASE + Frame * 2 / 16) % Period2 - MENU_CLOUD2_W,
               MENU_CLOUD2_Y, MENU_CLOUD2_W, MENU_CLOUD2_H);
  rdpq_set_prim_color(RGBA32(104, 118, 150, 0xff));
  DrawUiSprite(SPR_UI_CLOUD1,
               TW64_SCREEN_W - (MENU_CLOUD1_PHASE + Frame * 3 / 16) % Period1,
               MENU_CLOUD1_Y, MENU_CLOUD1_W, MENU_CLOUD1_H);
  rdpq_set_prim_color(RGBA32(78, 92, 128, 0xff));
  DrawUiSprite(SPR_UI_MOUNTAINS, 0, MENU_MOUNTAIN_Y, TW64_SCREEN_W, 160);

  /* One translucent scrim over the body of the page: the backdrop still shows
   * through, but every label sits on a predictable value instead of on a
   * mountain edge. */
  Tw64RenderShade(0, 70, TW64_SCREEN_W, 150, 6, 10, 20, 104);

  if (ShowLogo) {
    /* The logo is the one asset with a soft glow, so it blends instead of
     * alpha-testing; RGBA16's single alpha bit would cut the glow into a
     * jagged halo. */
    BeginTexturedPhase(false);
    rdpq_mode_alphacompare(0);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_dithering(DITHER_SQUARE_NONE);
    DrawUiSprite(SPR_UI_LOGO, (TW64_SCREEN_W - MENU_LOGO_W) / 2, MENU_LOGO_Y,
                 MENU_LOGO_W, MENU_LOGO_H);
  }
}

void Tw64RenderText(surface_t *pDisp, int X, int Y, const char *pText,
                    uint8_t R, uint8_t G, uint8_t B) {
  (void)pDisp; /* the RDP font writes through the attached surface */
  DrawText(X, Y, pText, R, G, B, ALIGN_LEFT, TW64_TEXT_BOX_W);
}

void Tw64RenderTextF(int Font, int X, int Y, const char *pText, uint8_t R,
                     uint8_t G, uint8_t B, int Align) {
  CRgb Color = {R, G, B};
  switch (Align) {
  case TW64_ALIGN_CENTER:
    DrawTextRuns(Font, X - TW64_TEXT_BOX_W / 2, Y, pText, &Color, 1,
                 ALIGN_CENTER, TW64_TEXT_BOX_W, 0);
    break;
  case TW64_ALIGN_RIGHT:
    DrawTextRuns(Font, X - TW64_TEXT_BOX_W, Y, pText, &Color, 1, ALIGN_RIGHT,
                 TW64_TEXT_BOX_W, 0);
    break;
  default:
    DrawTextRuns(Font, X, Y, pText, &Color, 1, ALIGN_LEFT, TW64_TEXT_BOX_W, 0);
    break;
  }
}

void Tw64RenderTextCentered(surface_t *pDisp, int CenterX, int Y,
                            const char *pText, uint8_t R, uint8_t G,
                            uint8_t B) {
  (void)pDisp;
  DrawText(CenterX - TW64_TEXT_BOX_W / 2, Y, pText, R, G, B, ALIGN_CENTER,
           TW64_TEXT_BOX_W);
}

void Tw64RenderMatch(surface_t *pDisp, CGameContext *pGameServer,
                     const CTw64RenderInfo *pInfo) {
  BuildFrameState(pGameServer, pInfo);

  /* Detail layers are the desktop client's gfx_high_detail switch. A quadrant
   * redraws the same map four times, so the port spends its fill rate on the
   * layers that carry the level's shape and drops the decorative ones once the
   * screen is split more than two ways. */
  const bool HighDetail = pInfo->m_NumViewports <= 2;

  rdpq_attach(pDisp, NULL);

  for (int v = 0; v < pInfo->m_NumViewports; ++v) {
    const CTw64Viewport &View = pInfo->m_pViewports[v];
    rdpq_set_scissor(View.m_X0, View.m_Y0, View.m_X1, View.m_Y1);
    SetClip(View.m_X0, View.m_Y0, View.m_X1, View.m_Y1);

    CCamera Cam;
    Cam.m_CenterX = (View.m_X0 + View.m_X1) / 2;
    Cam.m_CenterY = (View.m_Y0 + View.m_Y1) / 2;
    const CPlayer *pPlayer = pGameServer->m_apPlayers[View.m_ClientID];
    const CCharacter *pCharacter = pGameServer->GetPlayerChar(View.m_ClientID);
    if (pCharacter) {
      Cam.m_X = pCharacter->GetPos().x;
      Cam.m_Y = pCharacter->GetPos().y;
    } else if (pPlayer) {
      Cam.m_X = pPlayer->m_ViewPos.x;
      Cam.m_Y = pPlayer->m_ViewPos.y;
    } else {
      Cam.m_X = 0.0f;
      Cam.m_Y = 0.0f;
    }

    /* The port draws no parallax quad groups, so the map's own background
     * gradient colour stands in for sky, clouds, mountains and sun. */
    rdpq_set_mode_fill(RGBA32((uint8_t)(s_SkyColor >> 24),
                              (uint8_t)(s_SkyColor >> 16),
                              (uint8_t)(s_SkyColor >> 8), 0xff));
    s_LastColor = s_SkyColor;
    s_HasColor = true;
    rdpq_fill_rectangle(View.m_X0, View.m_Y0, View.m_X1, View.m_Y1);

    BeginTexturedPhase(true);
    if (s_MapReady)
      DrawTileLayers(View, Cam, false, HighDetail);

    /* Entities cover very few pixels but carry the frame's only soft edges
     * (the IA8 tee parts have four alpha bits), so they blend instead of
     * alpha-testing; the tile layers keep the cheaper alpha test. */
    rdpq_mode_tlut(TLUT_NONE);
    rdpq_mode_alphacompare(0);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    DrawPickups(pGameServer, Cam);
    if (pInfo->m_FlagMode)
      DrawFlags(pInfo, Cam);
    DrawHooks(pGameServer, Cam);
    DrawCharacters(pGameServer, pInfo, Cam);
    DrawProjectiles(pGameServer, Cam);
    DrawLasers(pGameServer, Cam);

    if (s_MapReady) {
      rdpq_mode_blender(0);
      rdpq_mode_alphacompare(1);
      rdpq_mode_tlut(TLUT_RGBA16);
      s_BoundTileset = -1;
      s_BoundTile = -1;
      s_BoundPalette = -1;
      DrawTileLayers(View, Cam, true, HighDetail);
      rdpq_mode_tlut(TLUT_NONE);
    }
  }

  rdpq_set_scissor(0, 0, TW64_SCREEN_W, TW64_SCREEN_H);
  SetClip(0, 0, TW64_SCREEN_W, TW64_SCREEN_H);

  rdpq_set_mode_fill(RGBA32(0, 0, 0, 0xff));
  s_HasColor = false;
  for (int v = 0; v < pInfo->m_NumViewports; ++v)
    DrawHudPanel(pInfo, pInfo->m_pViewports[v]);
  DrawViewportDividers(pInfo->m_NumViewports);
  if (pInfo->m_ScoreQuadrant)
    Fill(TW64_SCREEN_W / 2, TW64_SCREEN_H / 2, TW64_SCREEN_W, TW64_SCREEN_H,
         COLOR_PANEL);
  /* One 11-pixel strip along the top edge carries the clock, both team scores
   * and the two flag-status pips. Staying on a single line is a hard
   * constraint, not a style choice: in the four-way split the top-right
   * viewport's own HUD starts just below it. */
  const int StripHalfWidth = pInfo->m_Teamplay ? 64 : 20;
  Fill(TW64_SCREEN_W / 2 - StripHalfWidth, 0,
       TW64_SCREEN_W / 2 + StripHalfWidth, 11, COLOR_HUD_BACK);
  if (pInfo->m_FlagMode) {
    DrawFlagPip(TW64_SCREEN_W / 2 - 62, 2, TEAM_RED);
    DrawFlagPip(TW64_SCREEN_W / 2 + 54, 2, TEAM_BLUE);
  }
  if (pInfo->m_ShowScoreboard)
    Fill(90, 60, 230, 132, COLOR_HUD_BACK);

  BeginTexturedPhase(false);
  rdpq_mode_alphacompare(0);
  rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
  for (int v = 0; v < pInfo->m_NumViewports; ++v)
    DrawHudIcons(pGameServer, pInfo->m_pViewports[v]);

  /* Text is the last RDP phase of the frame, not a CPU pass after a drain. */
  for (int v = 0; v < pInfo->m_NumViewports; ++v)
    DrawHudText(pDisp, pGameServer, pInfo, pInfo->m_pViewports[v]);

  char aBuf[32];
  str_format(aBuf, sizeof(aBuf), "%d:%02d", pInfo->m_SecondsLeft / 60,
             pInfo->m_SecondsLeft % 60);
  Tw64RenderTextCentered(pDisp, TW64_SCREEN_W / 2, 2, aBuf, 235, 235, 235);

  if (pInfo->m_Teamplay) {
    /* Two separate labels rather than one string, so each score carries its
     * own team colour. */
    uint8_t R, G, B;
    str_format(aBuf, sizeof(aBuf), "%d",
               pInfo->m_aTeamScore[TEAM_RED] / pInfo->m_TeamScoreDivisor);
    Tw64TeamColor(TEAM_RED, 0, &R, &G, &B);
    Tw64RenderTextCentered(pDisp, TW64_SCREEN_W / 2 - 42, 2, aBuf, R, G, B);
    str_format(aBuf, sizeof(aBuf), "%d",
               pInfo->m_aTeamScore[TEAM_BLUE] / pInfo->m_TeamScoreDivisor);
    Tw64TeamColor(TEAM_BLUE, 0, &R, &G, &B);
    Tw64RenderTextCentered(pDisp, TW64_SCREEN_W / 2 + 42, 2, aBuf, R, G, B);
  }

  if (pInfo->m_ScoreQuadrant)
    DrawScoreboardText(pDisp, pGameServer, pInfo, TW64_SCREEN_W / 2,
                       TW64_SCREEN_H / 2);
  if (pInfo->m_ShowScoreboard)
    DrawScoreboardText(pDisp, pGameServer, pInfo, 90, 60);

  /* Asynchronous: the RDP keeps drawing this frame while the CPU goes back to
   * the simulation, and the buffer flip is queued behind the RDP's own
   * completion. */
  rdpq_detach_show();
}
