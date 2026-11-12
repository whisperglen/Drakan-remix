# Drakan-remix
Drakan Order of the Flame RTX Remix

# drakan_hook — logging `ddraw.dll` proxy

Our own code (RULES R-02). It validates the renderer findings (F-013 … F-018) in the running game.
It only reads game memory and writes a log; it doesn't change what the game draws.

## What it does
- **Proxy:** exports all 22 DirectDraw functions and forwards each to a chained `ddraw.dll`
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
