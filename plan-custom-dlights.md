# Plan: custom dlights in the light editor

Status: design proposal (no code written yet). Owner: `feature/qr-editor`.

## Goal

A mode in the light editor for authoring freely placed dynamic lights ("custom
dlights") that a map does not have: the author flies to a spot, drops a light,
tunes it in the panel, and the result is written to a per-mod YAML file the
engine reads back whenever that level loads. The lights live only in the editor
while it runs and are otherwise invisible.

A custom dlight is meant to be the authored twin of the engine's own dynamic
lights: it is uploaded through the same per-frame spherical-light path, so it
lights the world and the models exactly as a classic DLIGHT does, and it carries
a **light style** (flicker, candle, strobe, ...) evaluated per frame against the
map's own lightstyle strings — i.e. it takes over the behaviour of the original
dlights, only authored per level and persisted in a file instead of spawned by
an entity.

## Storage

`<gamedir>/qray/lights.yaml` — one file per mod, with one section per level
(named after the map file without path or extension, `maps/start.bsp` -> `start`):

```yaml
# Custom lights authored with the light editor, per level.
start:
  - name: torch                # optional label, kept for reading
    origin: 512 -256 64        # x y z, Quake units
    radius: 0.4                # in rt_dlight_radius (metric) units, 0..10
    intensity: 1.0             # multiplier of the colour
    color: ff9900              # rrggbb
    offset: 0 0 16             # optional shift from origin, x y z
    style: candle              # light style, see below
e1m2:
  - origin: -128 96 48
    color: 88ccff
    style: flicker
```

Rules:

- Load order: `id1/qray/lights.yaml` first, then the running gamedir's; a level
  section of a later file **replaces** the same level's section entirely (the
  same "later file wins by name" philosophy as materials.yaml/lights.yaml).
- Only the current level's section is loaded into memory at map load; the other
  sections are parsed lazily (or the whole file is parsed and only one section
  materialised — parser size is tiny, parse it all).
- Unknown keys are preserved on save (merge-into-text like the materials
  session does), so comments and future fields survive.
- `origin` is the anchor the light is placed at; `offset` moves the actual
  light from the anchor (same semantics as the per-emitter `light_offset`).

## Runtime

- New struct (`rt_lights.h`): `rt_custom_light_t` — `name[32]`, `origin[3]`,
  `offset[3]` + has flags, `radius`, `intensity`, `color[3]` + has flag.
- A per-map list `RT_CustomLights` loaded in `RT_LIGHT_ChangeMap`-adjacent code
  (gl_model.c next to `RT_MAT_ChangeMap` / `RT_LIGHT_Reload`; note the
  still-unbraced `if` there — fix on the way).
- Upload: each frame, append the list to the spherical-light upload (the same
  path the legacy `cl_dlights` use), so they light the world and models like
  any dlight, and respect `rt_dlight_*` scaling where it applies. Under
  `rt_truelight 1` the `RT_UploadAllElights` early-return must not skip them.
- Budget: `QRAY_MAX_CUSTOM_LIGHTS` = 64 (confirmed). Above the cap the editor
  refuses to add more and says so.
- Light style: `style` selects one of the engine's light styles (0..11, the
  classic set: steady, flicker, slow strong pulse, candle, fast strobe, gentle
  pulse, flicker B, candle B, candle C, slow strobe, fluorescent flicker, slow
  pulse). Each frame the light's intensity is multiplied by the style's current
  value (`d_lightstylevalue[style] / 256`), so a custom light flickers exactly
  like a map light of that style, and it follows a `lightstyle` command or a
  map's animated style without any extra data. `style: steady` (0) is the
  default.
- They are **tracked**: appended to the `RT_TRACK_*` list with a new kind
  `RT_LIGHT_KIND_CUSTOM`, a stable `uniqueID` (map hash + index), so the
  editor's wireframes, hover and picking work with no new pick code. Wireframe
  colour: a fourth colour (e.g. magenta) so authored lights read apart from
  material/legacy/map lights.
- No flicker beyond the light style; colour and intensity stay static.

## Editor UX

- The light editor gains a third tab: `Entity` / `Global` / `Custom`.
- The Custom tab lists the level's custom lights. Each entry: collapsible row
  with `name`, and the same parameter set as the entity panel — `light_radius`
  (0..10 slider, metric units), `light_intensity`, `light_offset` (one X/Y/Z
  row, one reset), `light_color` (hex picker), `light_style` (combo with the
  style names above) — plus two buttons:
  - `Place` — switches to the flying view ("aim and press fire"): the press
    drops the light **on the surface the crosshair hits** (the hit point, pushed
    a little out along the surface normal so the light is not inside the wall),
    then returns to the cursor mode with that light selected. Uses the same
    swallow-the-press mechanism as `Set sun position`.
  - `Remove` — deletes the light (with the list shifting; tracked ids re-derive
    from index, which is fine because selection is re-resolved by position).
- `Add` at the bottom: creates a light at the current aim point (or, when the
  aim hits nothing, at the camera position ahead of the view), defaults
  (radius 0.4, intensity 1, white) and opens its row.
- The Entity tab stays for existing lights; a custom light picked in the world
  also selects its Custom-tab row (and vice versa) via the shared tracked id.

## Session flow (consistent with the rest of the editor)

- Target file `<gamedir>/qray/lights.yaml`; session file
  `<gamedir>/qray/lights.editor.yaml`; backup `<gamedir>/qray/backup_lights.yaml`.
- `Apply` merges the touched level section into the target's own text (other
  levels, comments and unknown keys copied verbatim).
- `Exit` asks Save/Discard as today; `Cancel` reverts the in-memory list to the
  snapshot.
- A map change ends the editor (`QR_Editor_OnNewMap`), so unsaved custom lights
  are dropped with the session file removed — the existing crash-leftover
  cleanup already covers it.

## Integration points (code map)

- `Quake/rt_lights.h/.c`: struct, list, load/save/merge, `RT_CustomLights_ForMap`,
  `RT_CustomLights_Ensure/Remove`, the yaml writer that preserves comments,
  `QRAY_MAX_CUSTOM_LIGHTS`.
- `Quake/gl_model.c`: call the loader on map load (fix the unbraced `if`).
- Light upload: the per-frame dlight upload path (gl_rmain.c / r_world.c),
  appended after the material/legacy lights; skip when `rt_truelight != 1`.
- `Quake/gl_rmain.c`: `RT_TRACK_*` — add the custom lights (kind, stable id,
  wireframe colour).
- `Quake/qr_editor.c`: the third tab, Place/Add/Remove, session files for the
  new target, snapshot/Cancel for the custom list, Save/Discard messages.

## Decisions (confirmed with the user)

1. Cap: **64** custom lights per level.
2. `Place` **snaps to the surface** the crosshair hits (small push-out along the
   normal so the light is not inside the wall).
3. Radius slider keeps the metric 0..10 (`rt_dlight_radius` units) of the
   Entity tab.
4. Light **styles are in v1** (flicker/candle/strobe/... as described above);
   this is what makes a custom light take over the behaviour of the original
   dlights. Static colour and intensity otherwise.
5. Custom lights light the **world and the models**, like the classic dlights
   (same upload path).
