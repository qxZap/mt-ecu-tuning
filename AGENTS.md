# AGENTS.md -- ECU Tuning

A native Motor Town plugin: one DLL loaded by
[Plugin Loader](https://github.com/qxZap/mt-plugin-loader). Source is `plugins/ecu_tuning.cpp`.
`include/` is a copy of the loader's headers; refresh it from mt-plugin-loader when the
api version changes.

## Build and test

`build.bat` (MSVC 2022 x64) writes `out\`. Install to `MotorTown\Binaries\Win64\plugins\`
with the game closed. There is no test suite: the log `plugins\mt_plugin_<Name>.log` in that
folder is the check, so log what you need to see in game.

## Rules

- The loader owns the ProcessEvent hook and engine discovery. Use `include/mt_plugin_api.h`,
  and never hook a vtable from a plugin.
- `process_event` only on the game thread (`run_on_game_thread`).
- Find fields by reflection (`api->field_offset`) and code by signature (`find_sig`), never
  by address. A missing field or signature logs a line and leaves the game vanilla.
- Hardcoded offsets are the exception and need a `shortcut:` comment naming the limit.
- Mappings come from the loader: `[Mappings] Export=1` in `mt_loader.ini`. After a game
  update recheck every signature and hardcoded offset.
- A plugin rewriting game data affects the machine running the game: single player or the
  host. On someone else's server that server's rules apply.
- Settings live in `Binaries\Win64\plugins\<plugin>.ini`, through the loader's ini functions (`iniFile` moves an old file over). `[General] Disabled=1` makes `PluginInit` start nothing. After
  renaming one, move the old file on start so players keep their settings.

## This plugin

Settings are per vehicle in `Binaries\Win64\plugins\ecu_tuning.ini`, `[Vehicle<id>]`, with defaults in
`[ECU]`. A file value beats the code default, so changing a default means renaming its key.

- **The game's spin box scales its step by the value's magnitude** (0.01 below 1, 10 from 10 up).
  Rows hold a step count in 1.00 to 9.99 (`toSpin`, `fromSpin`) and write the real number as text.
- **Throttle:** the engine's own value drops at the limiter and on shifts. A lift is read from
  `MTVehicle.Throttle`, the pedal.
- **Turbo values:** `TurbineSpeedRatio` is engine +0x364 (not reflected, can exceed 1) and
  BoostBar is +0x35c, which is `IntakePressureMultiplier` times that speed. The engine reads the
  per-car copies at `Turbocharger`, so `tuneEngine` writes those, remembering the base so a part
  swap is noticed.
- **A loaded asset nothing references is collected about once a minute.** `keepLoaded` forgets
  it and the next use reloads it. A stale pointer crashes; reloading at once stutters.
- **Part row names are the metadata:** the supercharger stage comes from the fitted intake's
  name, so MoreTuning's parts stay plain intakes without this plugin.
- `SlowFrame` is a diagnostic for frame hiccups: remove it once they are understood.
