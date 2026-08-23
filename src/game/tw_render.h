/* Teeworlds 64: textured RDP renderer.
 *
 * One camera, one HUD and one scissor rectangle per local player. The world is
 * drawn straight from the authoritative server entities -- there is no
 * snapshot and no interpolation -- but the pixels are the desktop client's:
 * the map's own graphic tile layers, the 0.7 tee parts tinted through the RDP
 * primitive colour, and `game.png` sprites for weapons, projectiles, hook,
 * pickups, flags and the HUD. All of it comes from `.sprite` assets produced
 * offline by scripts/convert_n64_assets.py. */
#ifndef TW64_GAME_TW_RENDER_H
#define TW64_GAME_TW_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include <surface.h>

class CGameContext;
class IKernel;
class IMap;

enum {
  TW64_MAX_VIEWPORTS = 4,
  TW64_SCREEN_W = 320,
  TW64_SCREEN_H = 240
};

/* The three staged typefaces. The HUD is the only text surface with a frame
 * budget, so it keeps the 1bpp monochrome pixel font it was measured with; the
 * full-screen pages use the desktop client's own UI typeface (DejaVu Sans) in
 * two sizes, both anti-aliased with a one-pixel outline so a label stays
 * readable over the animated menu backdrop. See the `font` rule in
 * n64/Makefile. */
enum {
  TW64_FONT_HUD = 0,
  TW64_FONT_MENU,  /* DejaVu Sans 12, outlined: menu entries, page headings */
  TW64_FONT_SMALL, /* DejaVu Sans 9, outlined: notes, help and the footer */
  TW64_NUM_FONTS
};

/* Horizontal anchor of a text call: X is the left edge, the centre or the
 * right edge of the label. */
enum { TW64_ALIGN_LEFT = 0, TW64_ALIGN_CENTER, TW64_ALIGN_RIGHT };

struct CTw64Viewport {
  int m_X0;
  int m_Y0;
  int m_X1; /* exclusive */
  int m_Y1; /* exclusive */
  int m_ClientID;
};

/* Everything the renderer needs to know about the match that is not readable
 * from CGameContext itself. */
struct CTw64RenderInfo {
  const CTw64Viewport *m_pViewports;
  int m_NumViewports;
  int m_NumPlayers;
  const char *const *m_apPlayerNames;
  bool m_ShowScoreboard;
  /* Three local players leave one quadrant free; it becomes a permanent
   * scoreboard panel instead of dead space. */
  bool m_ScoreQuadrant;
  int m_SecondsLeft;
  int m_ScoreLimit;
  /* Team modes colour tees by team, show the two team scores in the centre
   * HUD and rank the scoreboard by team. */
  bool m_Teamplay;
  /* CTF adds flag entities, the flag-status pips and the carrier marker. */
  bool m_FlagMode;
  int m_aTeamScore[2];
  /* CTF team score counts 100 per capture; the HUD shows captures. */
  int m_TeamScoreDivisor;
};

/* Loads the fixed sprite set from the ROM filesystem. Fatal on a missing
 * asset: a half-textured renderer is worse than a loud failure. */
void Tw64RenderInit(void);

/* Binds the renderer to a freshly loaded map: builds the graphic tile-layer
 * table and loads the tileset sheets that map references. Call after
 * IEngineMap::Load() and before the first Tw64RenderMatch(). Returns false if
 * a referenced tileset is missing from the ROM filesystem. */
bool Tw64RenderSetMap(IKernel *pKernel, IMap *pMap, const char *pMapName);

/* Draws one frame of a running match into `pDisp` and schedules the flip on
 * RDP completion. The caller must not call display_show(): the whole frame,
 * text included, is one RDP display list, and draining it on the CPU is
 * exactly the stall this renderer avoids. */
void Tw64RenderMatch(surface_t *pDisp, CGameContext *pGameServer,
                     const CTw64RenderInfo *pInfo);

/* Full-screen pages: menu, loading and the final scoreboard. Text is part of
 * the page, so it must be drawn between Tw64RenderBeginMenuPage() and
 * EndPage(); EndPage() schedules the flip and the caller must not call
 * display_show() either. */
void Tw64RenderEndPage(void);
void Tw64RenderText(surface_t *pDisp, int X, int Y, const char *pText,
                    uint8_t R, uint8_t G, uint8_t B);
void Tw64RenderTextCentered(surface_t *pDisp, int CenterX, int Y,
                            const char *pText, uint8_t R, uint8_t G,
                            uint8_t B);

/* The menu pages' own text call: picks one of the staged fonts and an anchor.
 * `Y` is the cap top of the line in every font, so a small note and a menu
 * entry line up by eye when they are given the same Y. */
void Tw64RenderTextF(int Font, int X, int Y, const char *pText, uint8_t R,
                     uint8_t G, uint8_t B, int Align);

/* Translucent rectangle (standard mode, flat combiner). Used for the scrim the
 * menu lays over the backdrop and for the selection highlight, so the sky
 * still shows through instead of being punched out by an opaque panel. */
void Tw64RenderShade(int X, int Y, int W, int H, uint8_t R, uint8_t G,
                     uint8_t B, uint8_t A);

/* The full-screen menu/end-screen background: a night-sky gradient, two
 * drifting clouds and a mountain silhouette, all from the desktop client's own
 * mapres, plus the official logo. `Frame` animates the clouds; `ShowLogo` is
 * false for pages that carry their own headline. Attaches `pDisp`, so the
 * caller must finish with Tw64RenderEndPage(). */
void Tw64RenderBeginMenuPage(surface_t *pDisp, int Frame, bool ShowLogo);

/* Stable per-slot colour, shared by viewport frames, HUD, scoreboard and the
 * tee body tint. */
void Tw64PlayerColor(int ClientID, uint8_t *pR, uint8_t *pG, uint8_t *pB);

/* Team colour for team modes. `Team` is TEAM_RED (0) or TEAM_BLUE (1); the
 * two slots of a team are separated by a brightness step so teammates stay
 * distinguishable without leaving the team hue. */
void Tw64TeamColor(int Team, int SlotInTeam, uint8_t *pR, uint8_t *pG,
                   uint8_t *pB);

#endif
