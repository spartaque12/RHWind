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
- One of these `_gtaRenderHook.asi` builds:
  - a build with RenderHook's plugin API (`RHSetBuildingDrawCallback`, [gtaRenderHook PR #35](https://github.com/petrgeorgievsky/gtaRenderHook/pull/35)). RHWind registers a callback and patches nothing, and the dual pass is RenderHook's own (`UseDualPassForAlphaObjects`, [PR #34](https://github.com/petrgeorgievsky/gtaRenderHook/pull/34));
  - the 2021-09-11 release, which has no API. RHWind patches fixed offsets in that binary and draws the dual pass itself.

  Any other build is detected and left untouched.
- RenderHook's `shaders` folder. Tested with the **Serendipity 1.0** preset. The install script refuses to touch shaders that don't match and changes nothing.
- An ASI loader.

## Install

1. Copy `scripts\RHWind.asi`, `scripts\RHWind.ini` and `shaders\Wind.hlsli` into the game folder, keeping the folders.
2. Copy `install-shaders.ps1` into the game folder and run:
   ```
   powershell -ExecutionPolicy Bypass -File install-shaders.ps1
   ```
   It patches `shaders\SACustomBuilding.hlsl`, `shaders\SACustomBuildingDN.hlsl` and `shaders\Globals.hlsl`, and keeps the originals as `*.before_rhwind`. Running it again changes nothing, and shaders patched by RHWind 1.2–1.4 are brought up to date. You can also pass the game folder as the first argument and run it from anywhere.
3. In RenderHook's `settings.xml`, in the `<Deferred ... />` line:
   - keep `UseDitheringForAlphaObjects="false"`;
   - with a RenderHook that has the plugin API, add `UseDualPassForAlphaObjects="true"` (and optionally `AlphaObjectsCutoff="0.5"`). That turns on RenderHook's own dual pass. On the 2021 release this setting is ignored and RHWind's `[DualPass]` settings apply instead.

`RHWind.log` next to the ASI shows what was hooked, or that it registered with RenderHook's building draw callback.

**To uninstall:** delete the three files and rename the two `.before_rhwind` shaders back.

## Settings (`RHWind.ini`)

The `[DualPass]` settings are for the 2021-09-11 release. With a RenderHook that has the plugin API, its own `UseDualPassForAlphaObjects` and `AlphaObjectsCutoff` take their place, and these are ignored.

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | 1 | Dual-pass foliage. 0 = RenderHook's own behaviour; wind still works. |
| `Cutoff` | 0.5 | Texture alpha at or above this goes to the G-buffer; below it is blended. |
| `FadeStart` / `FadeEnd` | 150 / 350 | View depth (m) over which the cutoff rises, so more of the foliage is blended. 0 = off. |
| `MipAlphaBoost` | 0.25 | Raises alpha on lower mip levels so distant leaves don't thin out. |
| `DitherStart` / `DitherEnd` | 150 / 200 | View depth (m) where distant foliage switches to per-frame dither. Pixels switch at random between the two values, so there's no visible line. `DitherEnd` ≤ `DitherStart` turns it off. |

## How it works

- **Wind flag.** The `0xEDED` atomic plugin is registered from the game's `PluginAttach`, so the flag survives model loading.
- **Wind values.** A constant buffer (`b12`, VS and PS) is filled per draw. With the plugin API, RenderHook calls RHWind's callback before every building draw in every pass, including the blended pass of queued meshes; on the 2021 release, RenderHook's building render callbacks are wrapped. It holds the object position plus a wind-direction scroll, intensity = `CWeather::Wind * 1.5 + 2`, the dual-pass parameters and the frame number.
- **Wind weight.** It is `1 - day prelight alpha`, as in SkyGfx Extended. On those meshes the alpha is not opacity, so the shaders force it to 1. With the plugin API, the callback also tells RenderHook so (`RH_DRAW_VERTEX_ALPHA_IS_NOT_OPACITY`), and RenderHook's dual pass then handles wind meshes like any other foliage.
- **Dual pass (2021 release).** A stub in RenderHook's alpha-mesh push stores per-mesh flags in the entry's padding. Dual-pass meshes are also sent to the G-buffer draw, and the forward alpha-list callback sets each mesh's own `b12`.

Source: `src/RHWind.cpp`, `src/SAProcObjLimit.cpp` and `src/RHGrass.cpp`. Build with `build.bat` (Visual Studio 2019/2022 or Build Tools, x86).

## Also included: SAProcObjLimit (procedural object limit)

`scripts\SAProcObjLimit.asi` is a separate, optional plugin that works with or without RenderHook. It fixes a crash you can get with vegetation mods.

The game has room for **128** procedural object types: the bushes, weeds, rocks and cacti it scatters on grass, sand and pavement, listed in `data\procobj.dat`. Modloader merges the `procobj.dat` files of all your mods. *BSOR 5 Stars Vegetation* and *Sidewalk Weeds* together, for example, come to 137. The game loads the extra lines without any check, writes them over its list of spawned objects, and later crashes at `0x5A3FA6` when it removes ground objects near the affected surfaces.

SAProcObjLimit moves that part of the game's memory to a buffer with room for **512** types and updates every place in the game code that uses it. It writes `SAProcObjLimit.log`: check that the limit was raised to 512 and that the number of types loaded is what you expect. If any place it patches doesn't hold the expected value (another mod patching there, or not the 1.0 US exe), it changes nothing and says so in the log.

Install: copy `scripts\SAProcObjLimit.asi` into the game's `scripts` folder.

## Also included: RHGrass (grass under RenderHook)

`scripts\RHGrass.asi` is a separate, optional plugin. RenderHook doesn't draw the game's grass at all: the call is commented out in its source. RHGrass adds it back, after RenderHook's forward pass. It needs RenderHook (same 2021-09-11 build) but not RHWind. RenderHook builds that include [PR #32](https://github.com/petrgeorgievsky/gtaRenderHook/pull/32) draw the grass themselves; there RHGrass sees a different build and does nothing, so you don't need it.

Turning the call back on isn't enough, for two reasons:

- **Every tuft in one spot.** The game draws all grass of one type through a single model and moves it before each tuft. RenderHook only uploads a new world matrix when the matrix *pointer* changes, and here it never does, so every tuft was drawn where the first one was. RHGrass makes RenderHook upload the matrix for every tuft.
- **Invisible grass.** The grass colour from `data\plants.dat` has a low alpha (about 40/255), which the game blends softly. RenderHook's shader drops every pixel below 30% opacity, so all of it was discarded. RHGrass multiplies the alpha before it's applied.

Grass only appears with the game's effects quality set above Low, as in the unmodded game.

Install: copy `scripts\RHGrass.asi` and `scripts\RHGrass.ini` into the game's `scripts` folder. `RHGrass.log` shows what was patched. If RenderHook or the game code isn't what it expects, it patches nothing and says so in the log.

| Key (`RHGrass.ini`) | Default | Meaning |
| --- | --- | --- |
| `Enabled` | 1 | Draw grass at start. |
| `ToggleKey` | 0x77 (F8) | Switches grass on and off while playing. Windows virtual-key code, 0 = none. |
| `AlphaScale` | 6 | Multiplies the grass alpha from `plants.dat`. Below about 2 the grass disappears again. |
| `ColourScale` | 1 | Multiplies the grass colour. The grass is drawn unlit, so raise it if it looks too dark. |

## Credits

- RenderHook by PetkaGTA.
- Serendipity preset by Brian Tu (RTU).
- Wind data format and motion from SkyGfx Extended.
