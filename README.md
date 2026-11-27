# Patrician III Widescreen Renderer

Play in widescreen up to **2560×1440** on Windows. For the GOG and Steam editions of *Patrician III*.

**[Download the beta](https://github.com/onethree7/patrizier3_d3d11/releases)** · [Install](#install) · [Settings](#settings) · [Update or uninstall](#update-or-uninstall) · [Help](#help)

![Gdańsk in Patrician III at 2560×1440](docs/images/gdansk-2560x1440.jpg)

*Gdańsk at 2560×1440.* Play in a borderless or resizable window and go beyond the original 50 FPS limit. Some scenes in this beta may still show drawing errors or crashes.

## Install

1. **Download the ZIP.** On the [Releases page](https://github.com/onethree7/patrizier3_d3d11/releases), choose `Patrician3-D3D11.zip` under **Assets**.

2. **Back up the original.** Open the folder containing `Patrician3.exe` and copy `ddraw_Dll.dll` somewhere safe.

3. **Extract the ZIP** into that game folder and allow Windows to replace the DLL. You should see `high_res.ini` and `renderer-assets` beside `Patrician3.exe`.

<details>
<summary>Using the P3Modding modloader?</summary>

Before launching, move the modloader’s `high_res.dll` out of the `mods` folder. The modloader still loads the file if you rename it there.

</details>

<details>
<summary>Playing Patrician II?</summary>

The same `ddraw_Dll.dll` also runs *Patrician II* (`Patrician 2.exe`). Install it into that game’s folder the same way, then set `width=0` and `height=0` in `high_res.ini`.

</details>

Start the game and choose your resolution in the game options. The main menu stays centred. Enter a city to see the wider view.

## Settings

**For resolution changes, use the in-game options.** Choose **1280×1024**, **1600×900**, **1920×1080**, or **2560×1440**. The game saves your choice for the next launch.

If your game executable is patched to lay out wide screens itself, set `width=0` and `height=0` in `high_res.ini`. The renderer then leaves the game’s screen layout alone and renders at the size the game requests.

For window and frame-rate settings, open `high_res.ini` in the game folder with Notepad. Find the setting you want to change under `[display]`, edit its value, and save the file.

| What you want | What to put in `high_res.ini` |
|---|---|
| Fill the screen with a borderless window (default) | `mode=borderless` |
| Use a window you can move and resize | `mode=windowed` |
| Remove the frame-rate limit | `max_fps=0` |
| Wait for the screen refresh on every frame for even pacing (default) | `vsync=1` |
| Stop waiting for the screen refresh, so only `max_fps` limits the frame rate | `vsync=0` |
| Show the frame counter in the top-left corner | `show_fps=1` |
| Hide the frame counter | `show_fps=0` |
| Blend translucent images exactly like the original (default) | `exact_blend=1` |
| Use faster hardware blending, at most one colour step off, for example on Apple Silicon | `exact_blend=0` |

You can set a frame-rate limit between `10` and `1000`. If the `max_fps` line is missing, the game uses its original 50 FPS limit.

## Update or uninstall

**To update**, close the game and extract the new release ZIP into the same game folder. Keep the backup of your original `ddraw_Dll.dll`. If you’ve changed `high_res.ini`, save a copy before extracting so you can reapply your settings.

<details>
<summary>Updating from 0.2.x?</summary>

You can delete the old `wqhd-side-fill` folder inside `renderer-assets`. Keep `renderer-assets` itself. The renderer still uses its `fast-forward-map` folder.

</details>

**To uninstall**, close the game and copy your backed-up original `ddraw_Dll.dll` back into the game folder. Allow Windows to replace the renderer’s DLL.

## Help

**The main menu still has black bars. Is widescreen working?**

The original menus keep their proportions. Choose a widescreen resolution in the game options, then enter a city. The city view, top bar, and side panels use the full width.

**I use the P3Modding modloader. Which mods conflict?**

Move the modloader’s `high_res.dll` out of `mods` before starting the game. The `shipyard-details`, `town-hall-details`, and `debug-nav` mods are also incompatible with this renderer.

**The game crashed or something looks wrong. How do I report it?**

[Open an issue](https://github.com/onethree7/patrizier3_d3d11/issues) and start with what you were doing when it happened. If an error window appeared, include its title and exact message. For a drawing problem, a screenshot is especially helpful.

Include your game edition, renderer version, graphics card, screen resolution, and `high_res.ini` if available. Mention whether you use the modloader. If the problem happens every time you load a particular save, attaching it makes the problem easier to reproduce.

## For developers

The renderer replaces `ddraw_Dll.dll`, the game’s graphics library. It exposes the original 180 SGL entry points and draws through Direct3D 11. Widescreen support adjusts the game’s resolution handling in memory. The executable on disk stays unchanged.

<details>
<summary><strong>Build and run the tests</strong></summary>

You’ll need Windows, Visual Studio 2022 with the C++ desktop workload, x86 tools and a Windows SDK, CMake 3.25 or newer, and Node.js.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
node tests/test-exports.mjs
```

The build produces `build\Release\ddraw_Dll.dll`. For a local installation, copy the DLL into the game folder and the contents of `assets\` into `renderer-assets\`.

The unit tests run without game files. To compare rendering against the original library, point the comparison runner at a game folder containing the original `ddraw_Dll.dll`, `AIM.dll`, `archiver.dll`, and `p2arch0_eng.cpr`.

```powershell
node tests/run-sweep.mjs build\Release\sgl_sweep_oracle.exe "<game folder>" build\Release\ddraw_Dll.dll
```

</details>

## License

The source code is licensed under the [Apache License 2.0](LICENSE).

The fast-forward maps in `assets\` are derived from the *Patrician III* map and are excluded from that license. *Patrician III* is a trademark of its respective owners. This project is independent of them.
