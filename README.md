# Drakan-remix
Drakan Order of the Flame RTX Remix

# drakan_hook — logging `ddraw.dll` proxy

Our own code (RULES R-02). It validates the renderer findings (F-013 … F-018) in the running game.
It mostly reads game memory and writes a log. The only exception is the optional free camera, which moves the view.

## What it does
- **Proxy:** exports the 15 DirectDraw functions common to dgVoodoo2 and Windows (see F-020) and forwards each to a chained `ddraw.dll`
  (dgVoodoo2 renamed, or the system one), chosen in `drakan_hook.ini`.
- **Safety check:** hooks are installed only if the host is the expected `Drakan.exe`
  (base `0x400000`, PE checksum `0x89E91`) and the first bytes of every hook site match.
  Otherwise it stays a pure proxy and says so in the log.
- **Hooks** (5–9 byte `jmp` patches with trampolines; the original always runs):
  | Address | Function | Logged |
  |---|---|---|
  | `0x43BEB0` | frame render | capture trigger (F10 / auto), per-frame summary |
  | `0x447690` | object vertex transform + projection | world matrix, model-view, sample vertices (model → world → view → screen) |
  | `0x43C3C0` / `0x43C630` / `0x43C7D0` | 3D triangle-fan draws (1 UV / 4 UV / queued) | per-polygon view-space + reconstructed world + screen |
- **Terrain dump at level load:** hooks the level loader `0x438060`. After every successful load it writes
  `drakan_dump\level_NNN_<name>.dklv` (world, materials, every sector's cells and vertices, PVS).
  Turn it off with `[dump] terrain=0`. Convert it with `scripts\dklv_tool.py` (summary / `--obj` / `--png`).
- **Free camera (no-clip view):** hooks the camera builder `0x40A160` (F-025). This is the one hook that
  changes what you see: it moves only the view, never game state. NumLock on:
  | Key | Action |
  |---|---|
  | F9 | toggle (starts where the game camera is) |
  | Num8 / Num5 | forward / back along the view direction (pitch included) |
  | Num4 / Num6 | strafe left / right |
  | Num9 / Num3 | up / down (world vertical) |
  | Num1 / Num7 | speed ×2 / ×0.5 (start: `[freecam] speed=4096` units/s; 2048 = one terrain cell) |
  | Num. | jump back to the game camera |
  | F11 | write the camera position + terrain cell to the log (also works with the free camera off) |
  The mouse still looks around, because the rotation is the game's. The player stays put but still obeys WASD.
  Keys can be changed in `[freecam]` (Windows virtual-key codes). `enable=0` turns it off.
  `move_sky=1` (default) also hooks the object draw `0x446E70`: the object that sits at the game camera's x,z
  (the sky dome, F-026) is drawn around the free camera for that call only. Its stored position is restored right after.
- **Self-checks per captured frame:** recomputes the game's view-space and screen results from
  our matrix interpretation and reports the max errors. Near 0 means the findings are right.

## Build
`hook\build.bat` (VS 2022 Community, x86 toolset). Output: `hook\build\ddraw.dll`.

## Install (in the game folder, e.g. `D:\GAMES\Drakan`)
1. Back up, then rename dgVoodoo's `DDraw.dll` to `ddraw_dgvoodoo.dll` (leave `D3DImm.dll` as is).
2. Copy `hook\build\ddraw.dll` and `hook\drakan_hook.ini` next to `Drakan.exe`.
3. In `drakan_hook.ini` set `[chain] ddraw=ddraw_dgvoodoo.dll` (or leave it empty to use the
   system DirectDraw instead of dgVoodoo).
4. Start the game, get into a level, press **F10** a few times (each press captures one frame).
5. Quit. Send back `drakan_hook.log` from the game folder.

**Uninstall:** delete our `ddraw.dll` and `drakan_hook.ini`, rename `ddraw_dgvoodoo.dll` back to `DDraw.dll`.
**If the game misbehaves:** set `[hooks] enable=0` (pure proxy) to tell a proxy problem from a hook problem.
