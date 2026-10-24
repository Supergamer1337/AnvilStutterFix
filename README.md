# AnvilStutterFix

This small DLL mod hopes to resolve most of the traversal stutter I experienced in _Assassin's Creed Valhalla_ and _Assassin's Creed Mirage_ (since they use the same engine). It might work for other Anvil games, but I have not yet tested it, and I don't own them to check. It won't work for the earlier DX11 games like Origins and Odyssey, since this works by directly hooking into the DX12 calls made by the game. The actual fix was tuned for Valhalla, since that is where I had the most issues personally.

No game files are changed with this fix, and it should hopefully work no matter what version of the games you are using, since it only intercepts the problematic DX12 calls.

## Results

The fix was developed and tuned with the following machine and versions:

- AMD Ryzen 7 5700X, 32 GB RAM
- NVIDIA GeForce GTX 1080 Ti, driver 582.78
- Windows 11 Pro, build 26200
- Assassin's Creed Valhalla, version 1.7.0

With a 60 FPS cap, enforced via Special K, I got the following reduction in frame spikes while moving around in the world:

| | Frames over 33 ms | Frames over 50 ms |
|---|---|---|
| Without the mod | 4.7 to 6.1% | about 0.5% |
| With the mod | 0.07% | none |

## How it works

Unlike most game engines, the game doesn't reuse its DX12 resources. While you move, it keeps destroying and recreating the same resources in the same place in its GPU memory. The memory itself stays allocated, but every create and destroy forces Windows and the driver to map and unmap the resource's GPU virtual addresses again. This means updating the GPU's page tables, waiting for the GPU and on a shared lock over and over again, which stalls the frame.

This mod fixes that by hooking into those calls, freeing released resources and creating new identical ones ahead of time on its own threads. This essentially amortizes the cost over time, instead of hogging the game's own threads that the frame waits on. A proper fix would mean changing the game's own code, which is mostly prevented by the game's Denuvo anti-tamper DRM. However, if anyone at Ubisoft sees this fix and wants to implement it, just do that.

## Install

1. Download `AnvilStutterFix-release.zip` from the [latest release](https://github.com/SuperGamer1337/AnvilStutterFix/releases/latest).
2. Copy `version.dll` from the zip into the game folder, where you find `ACValhalla.exe` (or `ACMirage.exe`).
3. Start the game. `AnvilStutterFix.log` appears next to the DLL, which should show that it was successfully injected into the game.

To uninstall simply delete `version.dll` (and `AnvilStutterFix.log` if you want) from the game folder.

If another mod already put its own `version.dll` in that folder, see if it can be used as another `.dll` filename instead, as this currently only supports hooking in via `version.dll`.

## Debug build

`AnvilStutterFix-debug.zip` runs the same fix as release, but with some extra logging and configuration. On first start it writes `AnvilStutterFix.ini` with every setting and what it does so that you can tune it for your system. Every 5 seconds it also logs detailed stats to the log. It also has hotkeys that switch parts of the fix on and off in game, so you can compare within one session.

| Hotkey | Toggles |
|---|---|
| Ctrl+Shift+F12 | Deferred release |
| Ctrl+Shift+F11 | Prepared copies (the spare pool) |
| Ctrl+Shift+F10 | Pausing while the game creates resources |
| Ctrl+Shift+F9 | Event trace to `AnvilStutterFix_events.csv` |

A beep confirms each toggle. The release build ignores the ini, so you cannot use your custom tuned version with a release build. They should perform mostly the same though.

## Building

Clone the repository with its submodules. This brings in the appropriate MinHook version automatically. Then build it with CMake and Ninja, using the `release` or `debug` preset.

```
git clone --recursive https://github.com/SuperGamer1337/AnvilStutterFix.git
cd AnvilStutterFix
cmake --preset release
cmake --build --preset release
```

The DLL ends up in `build/release/`. You need a C++20 compiler for 64-bit Windows that understands GCC-style inline assembly, so use MinGW-w64 GCC or Clang rather than MSVC. If CMake doesn't pick it up, set `CC` and `CXX` to it before configuring. The [build workflow](.github/workflows/build.yml) shows how the downloadable builds are made.

## Contributing

If you run into any issues, open an issue with the `AnvilStutterFix.log` from the debug build and the hardware you used. Pull requests are welcome.

## License

[0BSD](LICENSE). MinHook is under the [BSD 2-Clause license](https://github.com/TsudaKageyu/minhook/blob/v1.3.4/LICENSE.txt).
