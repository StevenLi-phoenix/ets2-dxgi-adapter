# ets2-dxgi-adapter

Run **Euro Truck Simulator 2** full screen on a **secondary monitor** without
changing the Windows primary display and without windowed mode.

It is a drop-in `dxgi.dll` proxy for the game folder. To the game, the monitor you
pick looks like display #0, the primary one. Windows itself is not changed.

> 中文简介：欧卡 2 在 DX11 下把 `r_output` 指向非主显示器会直接崩溃
> （`Unsupported target color space`）。把这个 `dxgi.dll` 放进游戏的 `bin\win_x64`，
> 游戏会把你指定的副屏当成主屏，在副屏上全屏运行。Windows 的主显示器设置不会被改动。

## The problem

ETS2 has an `r_output` setting for choosing a display. With the DX11 renderer, setting
it to any non-primary output crashes the game while the window is being set up:

```
Unsupported target color space
```

This was seen on 1.62.3.3s, Windows 11, on a hybrid Intel + NVIDIA laptop with an
external monitor on the dGPU. The Vulkan and OpenGL renderers ignore `r_output`
completely.

The usual workarounds are to make the external monitor the Windows primary display, or
to run windowed or borderless and drag the window across. Both have side effects.

## How it works

`eurotrucks2.exe` loads `dxgi.dll` from its own folder before `System32`, so a
proxy DLL placed there sits between the game and the real DXGI.

1. **DXGI reordering.** `IDXGIFactory::EnumAdapters`, `EnumAdapters1`,
   `EnumAdapterByGpuPreference`, `EnumAdapterByLuid` and `IDXGIAdapter::EnumOutputs`
   are hooked through vtable patching. The adapter that drives the target monitor
   becomes adapter #0, and the target monitor becomes its output #0. The game's
   default path (`r_output -1`) then renders on the target. Because the game believes
   it is on its primary output, the colour-space check passes.
2. **Win32 monitor remapping.** The game places and sizes its window with Win32
   APIs, not DXGI. These `USER32` imports of `eurotrucks2.exe` are hooked through
   the IAT, in the game executable only, so the target monitor looks like the primary:
   - `MonitorFromWindow` and `MonitorFromPoint` resolve to the target monitor
     instead of the primary.
   - `GetMonitorInfoW` and `EnumDisplayDevicesW` report the target as the primary.
   - `GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN)` returns the target's size.
   - `EnumDisplaySettingsW` and `ChangeDisplaySettingsExW` with a `NULL` device use
     the target device.
   - `CreateWindowExW`, `SetWindowPos` and `MoveWindow` move top-level windows that
     would land on the primary onto the target.
3. **Pass-through.** The other 17 `dxgi.dll` exports are assembly stubs that jump
   straight to the real `System32\dxgi.dll`, which is loaded lazily.

### App-compat shim gotcha (for anyone writing a dxgi proxy)

The Windows app-compat engine (`apphelp` / `AcGenral`) calls dxgi's compat exports,
such as `SetAppCompatStringPointer` and `CompatValue`, from inside the loader
notification for the proxy. That happens **before the proxy's `DllMain` and before the
CRT is initialised**, and `LoadLibrary` of the real dxgi fails there with
`ERROR_NOT_FOUND` (1168). The proxy handles this as follows:

- everything reachable from the export stubs uses zero-initialised statics only
  (`SRWLOCK`, `INIT_ONCE`, no C++ static constructors);
- such early calls are recorded and replayed once the real dxgi is loaded;
- re-entry from the same thread while the real dxgi is loading is ignored, because
  loading it triggers the shim again.

## Install

1. Build `dxgi.dll` (see below).
2. Copy `build\dxgi.dll` and `ets2_monitor_adapter.ini` into the game's
   `bin\win_x64` folder, for example
   `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`.
3. In `Documents\Euro Truck Simulator 2\config.cfg`, make sure the game uses the
   default output and runs full screen:
   ```
   uset r_output "-1"
   uset r_adapter "-1"
   uset r_fullscreen "1"
   uset r_mode_width "2560"     # your target monitor's resolution
   uset r_mode_height "1440"
   ```
   Use the DX11 renderer, which is the default. Remove any `-rdevice` launch option
   that selects another renderer.
4. Start the game from Steam as usual.

To **uninstall**, delete `dxgi.dll`, `ets2_monitor_adapter.ini` and any
`ets2_monitor_adapter.log` from `bin\win_x64`. Steam's "Verify integrity of game
files" does not remove extra files.

## Configuration

`ets2_monitor_adapter.ini` lives next to the DLL:

| key | default | meaning |
|---|---|---|
| `monitor` | `auto` | `auto`: first desktop monitor that is not the Windows primary.<br>A substring of the monitor's device id, e.g. `AOCB305`.<br>A GDI name, e.g. `\\.\DISPLAY5`. |
| `reorder_adapters` | `1` | Make the GPU that drives the target monitor adapter #0. |
| `reorder_outputs` | `1` | Make the target monitor output #0 of its adapter. |
| `hook_win32` | `1` | Remap Win32 monitor queries and window placement in `eurotrucks2.exe`. |
| `log` | `0` | Write `ets2_monitor_adapter.log` next to the DLL. |

### Finding your monitor id

With more than two monitors, or when `auto` picks the wrong one, pin the monitor
by its hardware id:

```powershell
Get-PnpDevice -Class Monitor -PresentOnly | Select-Object FriendlyName, InstanceId
```

An `InstanceId` such as `DISPLAY\AOCB305\5&...` gives the id `AOCB305`. Put that in
`monitor=`. The id survives re-plugging and port changes, which `\\.\DISPLAYn` names
do not.

## Build

You need Visual Studio 2019 or later with the **Desktop development with C++**
workload, which provides the x64 MSVC toolchain and `ml64`.

```bat
build.bat            :: -> build\dxgi.dll
build.bat harness    :: also builds build\harness.exe
```

`test\harness.cpp` loads the proxy the way the game does. It prints the adapter and
output order the proxy presents, then creates a D3D11 device. Put an
`ets2_monitor_adapter.ini` with `log=1` next to `build\dxgi.dll`, then run
`build\harness.exe dxgi.dll` from the `build` folder to see what the proxy does.

## Troubleshooting

- Set `log=1` and check `bin\win_x64\ets2_monitor_adapter.log`. Look for lines such
  as `monitor remap: target=... primary=...` and `SetWindowPos ... -> <target x>,<target y>`.
- In `Documents\Euro Truck Simulator 2\game.log.txt`, `Display 0` should be the
  target monitor.
- If the game still opens on the primary display, check that `hook_win32=1` and
  that `monitor` resolves. The log prints `monitor=auto resolved to ...`.
- **To tell whether a crash comes from the proxy**, look at
  `Documents\Euro Truck Simulator 2\game.crash.txt`. The proxy only runs when the
  game enumerates displays and places its window. If no frame in the call stack is
  `dxgi.dll` and the crash happens after the game is running, the proxy is not
  involved. A common cause is an outdated mod. One seen in testing: a mod built for
  1.37–1.39 overrode truck interior definitions with an `@include "glass.dat"`.
  - The game log then shows `Failed to open file '.../interior/glass.dat'` and
    `Missing accessory (/def/vehicle/truck/<truck>/interior/...)`.
  - The game crashed with an access violation in `eurotrucks2.exe` as soon as a
    Quick Job spawned one of those trucks.
  - Disabling the mod fixed it.

## Caveats

- Tested on ETS2 1.62.3.3s (64-bit, DX11) on Windows 11 with an Intel iGPU and an
  NVIDIA dGPU. American Truck Simulator uses the same engine and will probably
  work, but it is untested.
- The DLL patches code inside the game process. It is meant for single player.
  It has not been tested with TruckersMP or other multiplayer mods; use it there at
  your own risk.
- A game update can change which APIs the game calls. If the window starts landing
  on the primary display again, enable the log and open an issue with it.

## License

MIT. See [LICENSE](LICENSE).
