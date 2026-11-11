# idtech8 test maps

`idtech8_test_glass.map` is the static glass view used for the transparency froxel blend A/B. It
is one glass brush, one `info_player_start` facing it and one fixed `light`, so the blend 0/0.5/1
frames differ only by the froxel contribution instead of animated-scene noise.

Build it with ericw-tools (qbsp), from this directory:

```sh
qbsp idtech8_test_glass.map
```

The map uses the AD texture `glass2`; the runtime material entry is:

```yaml
  - name: glass2
    material_glass: true
```

Copy the BSP into the game's `maps/` folder and run `map idtech8_test_glass` (or rename the source
to `test_glass`). Measured on the master build with the froxel unit fix: blend 0/0.5/1 red means
7.54/8.36/8.38, with green/blue stable to 0.02 and no scene animation.
