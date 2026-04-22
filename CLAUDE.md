# scumm-game

A SCUMM-style point-and-click adventure engine in C + raylib. Single file (`main.c`), one room (the dock), Guybrush as the actor. Deliberately **not** a SCUMM bytecode VM — no .lfl loader, no interpreter. Just the engine surface.

## Build / run

- `brew install raylib` (already installed, version 5.5 via Homebrew)
- `make` — builds `scumm-game` binary
- `./run.sh` — builds + wraps in `scumm-game.app` + launches via `open -W`
- `./build_app.sh` — rebuilds the `.app` bundle + icon from scratch

The `.app` bundle's MacOS stub `cd`s back to the repo root and execs `./scumm-game`, so all asset paths stay relative to the repo root.

**LSP false positives:** the in-editor diagnostics always complain that `raylib.h` isn't found and all raylib types are unknown. Ignore them — the actual `clang` invocation via `make` succeeds (Homebrew include path is on the command line, not in the LSP config).

## Architecture (single file, `main.c`)

- **Actor movement:** Dijkstra over a visibility graph built from the vertices of *all* walkbox + hole polygons + start/end. Connectivity is tested by `segment_clear_multi` (sampled interior: every ~2px along the segment must be `is_walkable` — inside at least one walkbox AND inside no hole). Same-ring edges are always connected (both walkbox rings and hole rings, so the pather can hug hole boundaries to route around obstacles). See `find_path_multi`.
- **Walkboxes (cyan in the editor):** list of concave polygons (`docks[MAX_WB_POLYS]`, `dock_count`). The walkable area is the **union** — a point is legal if it lies in any polygon. Polygons can overlap or share edges; shared vertices become zero-weight edges in the graph. Point-in-polygon is ray-cast (`point_in_polygon`) — boundaries are ambiguous, so `segment_clear_multi` samples interior only. The user typically calls these "cyan polygons" or "cyan areas".
- **Holes (orange in the editor):** list of concave polygons (`holes[MAX_WB_POLYS]`, `hole_count`) that **subtract** from the walkable union. A point is walkable iff it lies in some walkbox *and* no hole. Use for pillars, furniture, water — any obstacle inside a walkbox. `clamp_to_walkable` snaps out of holes by nearest-edge projection. The user typically calls these "orange polygons", "holes", or "no-walk zones".
- **Foregrounds (magenta/pink in the editor):** list of `Walkbox`es (same polygon type, different role) rendered *after* the actor by ear-clipping triangulation (`triangulate`) and re-sampling the bg texture. So the actor walks behind them. `has_self_intersection` + `sort_poly_by_centroid` let the editor warn on and auto-fix bad polygons. The user typically calls these "magenta polygons" or "pink areas".
- **Actor sprites:** loaded from `sprites/guybrush/<anim>/NN.png`. Facing is picked from the movement vector; 8fps cycle while moving, frozen frame 0 when stopped. Perspective-scaled via `actor_scale_at` + a constant `ACTOR_SPRITE_SCALE` (currently 2.5x). Falls back to geometric rectangles if any anim is missing.
- **Rendering order per frame:** bg → lamp hotspot → actor → foreground polygons → overlays (edit/debug) → message bubble → verb bar → status text.
- **Editors / modes:** mutually exclusive — `edit_mode` (walkbox/fg), `browser_mode` (sprite picker), neither (normal play). Each mode gates the others' input.

## Keys (cheat sheet)

- Normal: click verb, click object, click floor to walk. `D` = walkbox/fg overlay. `E` = edit mode. `B` = sprite browser.
- In edit mode: `W` walkbox mode (press again to cycle), `F` foreground mode (press again to cycle), `H` hole/no-walk mode (press again to cycle), `N` new polygon in current mode, `Bksp` delete current polygon, `O` auto-order verts, `R` reset, `S` save, `E` exit (auto-saves).
- In browser mode: `←/→` prev/next, `↑/↓` ±10, `Home/End`, `0` idle, `1-4` walk down/up/left/right, `5-8` face down/up/left/right, `S` save, `B` exit.

## Data files (all plain text)

- `walkbox.txt` — one `x y` per line, polygons separated by blank lines. Legacy single-polygon files (no blank lines) still parse as one polygon. Loaded and saved via `load_fg_list` / `save_fg_list`.
- `fg.txt` — same format as `walkbox.txt`. Each polygon is ear-clipped at render time.
- `holes.txt` — same format as `walkbox.txt`. Each polygon carves a no-walk zone out of the walkbox union.
- `sprites.txt` — one line per animation: `<name> <frame1> <frame2> ...` where frame numbers index `guybrush_sprites_v3/sprite_NNN.png`. 9 named slots: `idle`, `walk_{down,up,left,right}`, `face_{down,up,left,right}`.
- `player.txt` — truncated on startup, updated every 0.1s while moving. For debugging.
- `export_sprites.sh` — bakes `sprites.txt` → `sprites/guybrush/<anim>/NN.png`. Auto-mirrors `walk_right` → `walk_left` with `sips -f horizontal` (verified: `sips -f horizontal` = left-right mirror on macOS, not top-bottom).

## Working with the user

**Always check `player.txt` when the user asks a question.** The file holds the actor's live `x y` position (updated every 0.1s while the game is running, truncated on startup). The user is usually referring to something at that spot — "why can I walk here?", "what's near me?", "is this inside the walkbox?" — so read `player.txt` before answering instead of asking where they are.

## Gotchas

- `SetWindowIcon` is **ignored on macOS** — that's why there's a `.app` bundle with `.icns`.
- The straight-line walk check clamps *targets*, not the actor's current position mid-walk. Pathfinding handles the walk route; per-frame clamp catches stationary-outside cases.
- The stock hardcoded actor spawn `(SCREEN_W/2, PLAY_H-80)` gets clamped to the walkbox union on startup and on edit-mode-exit. Don't reintroduce a version that trusts the raw coords.
- Walkbox / fg polygons must each be **simple** (non-self-intersecting). The editor warns, and `[O]` auto-orders by centroid angle (only fixes star-shaped cases).
- Raw text files may use CRLF on other platforms — `load_fg_list` strips `\r` implicitly via `sscanf`. Keep it that way.
- Disjoint walkboxes (no shared vertex, no overlap) produce unreachable regions — `find_path_multi` returns just the start point and the click silently no-ops. If you want separate reachable regions, share a vertex or overlap them.
