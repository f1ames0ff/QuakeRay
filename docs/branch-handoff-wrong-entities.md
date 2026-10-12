# Hand-off: wrong entities in Arcane Dimensions (ghost models)

## Symptom

In Arcane Dimensions only, models appear that the level does not place:

- `progs/misc_broken.mdl` (the blue "diamond"; frame 4 in the report) — seen at
  spots where an explosion happened earlier, and after the player died and the
  level was restarted (`restart`); a fresh game start has none.
- `progs/misc_corner1.mdl` (a beige "arrow") — seen at the centre of a barrel
  explosion, oriented along the player's view at the time.

They stay and pile up over a session: the same reports mention the `ents` CPU
slot of `rt_stats` growing after several restarts and the frame rate falling.

## What is known

- The objects are real client entities: the material editor picks them out of
  `cl.entities` (log: `winner ent 366 model 'progs/misc_broken.mdl'` with the
  texture `progs/misc_broken.mdl:frame1`).
- Both models are referenced by the mod's own `progs.dat` (once each), so the
  mod does spawn something with them; the open question is whether the entity
  seen is that object or a different entity drawn with the wrong model.
- The server's edicts do not grow across `restart`: `edictcount` reported
  423 -> 418 -> 411 (map load, after one and after two restarts), so the server
  side is not accumulating entities.
- The GL texture table is stable across level switches (`imagelist`: 659 ->
  1014 -> 801 textures), so the other map's textures are freed again and this
  is not a texture-table leak.

## Hypothesis

A model-index mismatch, not an "error model": this engine has no Source-style
ERROR placeholder, so an index that does not line up with the client's precache
list draws a *different valid* model. The client resolves an entity's model
with

    model = cl.model_precache[ent->netstate.modelindex];   (Quake/cl_parse.c)

without a bounds check, and the FTE/999+ protocol carries 16-bit model indices
(`B_LARGEMODEL`). A precache entry the client missed, or an index mapped
differently on both sides, would draw the mod's intended effect model as
`misc_broken`/`misc_corner1`.

The "oriented with the player's view, at the centre of the explosion" detail
fits an effect entity (impact, spark, `tfog`) whose model went wrong, and
"AD only" fits its very large model list.

## Probe in this branch

`Quake/cl_parse.c` carries a temporary probe (`CL_DebugGhostModel`, called from
`CL_EntitiesDeltaed`). For every entity whose resolved model name contains
`misc_broken` or `misc_corner1` it writes one line, once per entity number:

    qr ghost probe: ent N client model '...' netindex I at (x y z) |
                    server classname '...' model '...' index J

The server half is read from the local listen server's edict (`sv.qcvm`,
`EDICT_NUM`, `PR_GetString`), so no console commands are needed.

How to read it:

- the server's classname is an effect (`tfog`, spark, `explosion`, a missile)
  while the client draws `misc_broken`/`misc_corner1`: the engine's model index
  handling is wrong — start at `CL_ParseServerInfo` (the precache list),
  `CL_ParseBaseline`/`CLFTE_ReadDelta` (the 16-bit model index paths) and the
  server's `SV_CreateBaseline`/`SVFTE_CalcEntityDeltas`;
- the server's classname is one of the mod's own misc objects: the mod spawns
  these models and the engine is not at fault.

## Reproduction

1. Build `.\build_win.ps1 Debug` and start the game on AD
   (`quakeray.exe -basedir . -game ad +map ad_e1m1`).
2. Let a barrel explode (the "arrow") and die plus `restart` a few times (the
   "diamond"); no console work is needed — the probe writes into
   `build/Debug/qconsole.log`.
3. Read the lines:
   `Select-String -Path build\Debug\qconsole.log -Pattern "qr ghost probe"`.

## Related but separate: about 75 MB per level switch

Alternating two maps (`ad_e1m1`/`ad_tfuma`, no input) grows the private bytes
of the process by roughly 75 MB per switch (451 MB over six), while reloading
the same map does not grow (2-3 MB). The server edicts and the GL textures are
stable, so something per-map is retained on the renderer side. Not addressed
here; the counters worth instrumenting next are the sound cache, the renderer's
material and geometry caches and the per-map RT scene structures.

## State

- Branch `bugfix/wrong-entities`, based on `origin/master`.
- One temporary change: the probe above (remove it once the question is
  answered).
