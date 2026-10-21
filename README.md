# RHWind: vegetation wind and clean foliage for GTA SA RenderHook

An ASI plugin plus a shader patch for [gtaRenderHook](https://github.com/petrgeorgievsky/gtaRenderHook) (DX11) in GTA San Andreas. It does two things:

1. **Wind.** Trees and bushes made for SkyGfx Extended's wind shader sway under RenderHook too. These are models with RenderWare atomic plugin `0xEDED`, for example *BSOR 5 Stars – Wind Shader Adapted*. The motion is the same as SkyGfx Extended's and follows the game's wind strength and direction.
2. **Clean foliage.** RenderHook normally gives you two bad choices for leaves:
   - `UseDitheringForAlphaObjects="false"`: leaves are alpha-blended in the forward pass, so they get no sun shadows and look washed out or see-through.
   - `"true"`: leaves are dithered into the G-buffer and look noisy.

   RHWind draws foliage in two passes instead. The solid parts are alpha-tested into the G-buffer, so they're lit and shadowed like opaque geometry, and only the soft edges are blended. Far away, where that would shimmer, foliage is dithered with a pattern that changes every frame, and RenderHook's TAA smooths it out. This removes the grain on distant trees.

Part 2 works with any foliage, including models without wind data.

## Requirements

- GTA San Andreas 1.0 US exe.
- `_gtaRenderHook.asi`, build of 2021-09-11. RHWind patches fixed offsets in that binary, so other builds are not supported.
- RenderHook's `shaders` folder. Tested with the **Serendipity 1.0** preset. The install script refuses to touch shaders that don't match and changes nothing.
- An ASI loader.

## Install

1. Copy `scripts\RHWind.asi`, `scripts\RHWind.ini` and `shaders\Wind.hlsli` into the game folder, keeping the folders.
2. Copy `install-shaders.ps1` into the game folder and run:
   ```
   powershell -ExecutionPolicy Bypass -File install-shaders.ps1
   ```
   It patches `shaders\SACustomBuilding.hlsl` and `shaders\SACustomBuildingDN.hlsl` and keeps the originals as `*.before_rhwind`. Running it again does nothing. You can also pass the game folder as the first argument and run it from anywhere.
3. Keep `UseDitheringForAlphaObjects="false"` in RenderHook's `settings.xml`.

`RHWind.log` next to the ASI shows what was hooked.

**To uninstall:** delete the three files and rename the two `.before_rhwind` shaders back.

## Settings (`RHWind.ini`)

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | 1 | Dual-pass foliage. 0 = RenderHook's own behaviour; wind still works. |
| `Cutoff` | 0.5 | Texture alpha at or above this goes to the G-buffer; below it is blended. |
| `FadeStart` / `FadeEnd` | 150 / 350 | View depth (m) over which the cutoff rises, so more of the foliage is blended. 0 = off. |
| `MipAlphaBoost` | 0.25 | Raises alpha on lower mip levels so distant leaves don't thin out. |
| `DitherStart` / `DitherEnd` | 150 / 200 | View depth (m) where distant foliage switches to per-frame dither. Pixels switch at random between the two values, so there's no visible line. `DitherEnd` ≤ `DitherStart` turns it off. |

## How it works

- **Wind flag.** The `0xEDED` atomic plugin is registered from the game's `PluginAttach`, so the flag survives model loading.
- **Wind values.** RenderHook's building render callbacks are wrapped to fill a constant buffer (`b12`, VS and PS) per draw. It holds the object position plus a wind-direction scroll, intensity = `CWeather::Wind * 1.5 + 2`, the dual-pass parameters and the frame number.
- **Wind weight.** It is `1 - day prelight alpha`, as in SkyGfx Extended. On those meshes the alpha is not opacity, so the shaders force it to 1.
- **Dual pass.** A stub in RenderHook's alpha-mesh push stores per-mesh flags in the entry's padding. Dual-pass meshes are also sent to the G-buffer draw, and the forward alpha-list callback sets each mesh's own `b12`.

Source: `src/RHWind.cpp` and `src/SAProcObjLimit.cpp`. Build with `build.bat` (Visual Studio 2019/2022 or Build Tools, x86).

## Also included: SAProcObjLimit (procedural object limit)

`scripts\SAProcObjLimit.asi` is a separate, optional plugin that works with or without RenderHook. It fixes a crash you can get with vegetation mods.

The game has room for **128** procedural object types: the bushes, weeds, rocks and cacti it scatters on grass, sand and pavement, listed in `data\procobj.dat`. Modloader merges the `procobj.dat` files of all your mods. *BSOR 5 Stars Vegetation* and *Sidewalk Weeds* together, for example, come to 137. The game loads the extra lines without any check, writes them over its list of spawned objects, and later crashes at `0x5A3FA6` when it removes ground objects near the affected surfaces.

SAProcObjLimit moves that part of the game's memory to a buffer with room for **512** types and updates every place in the game code that uses it. It writes `SAProcObjLimit.log`: check that the limit was raised to 512 and that the number of types loaded is what you expect. If any place it patches doesn't hold the expected value (another mod patching there, or not the 1.0 US exe), it changes nothing and says so in the log.

Install: copy `scripts\SAProcObjLimit.asi` into the game's `scripts` folder.

## Credits

- RenderHook by PetkaGTA.
- Serendipity preset by Brian Tu (RTU).
- Wind data format and motion from SkyGfx Extended.
