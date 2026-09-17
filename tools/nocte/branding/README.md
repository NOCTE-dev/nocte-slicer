# NØCTE app-identity assets

`make_assets.py` regenerates every app-identity image under `resources/images/`. The generated
files are committed; this directory exists so that the next person can reproduce them instead of
reverse-engineering a binary.

```bash
python tools/nocte/branding/make_assets.py            # fetch originals if missing, then generate
python tools/nocte/branding/make_assets.py --fetch    # force a re-download of the originals
python tools/nocte/branding/make_assets.py --measure  # print the measurements the geometry uses
python tools/nocte/branding/make_assets.py --out DIR  # write somewhere else, for review
```

Requires Pillow and an authenticated `gh` CLI (the originals live in private repos).

## Where the artwork comes from

The brand originals are fetched into `_src/`, which is gitignored — they are the user's own brand
files and only *derived* assets belong in `resources/`. The reference is
`NOCTE-dev/alma:branding/NOCTE_TRANSPARENTE.png`: the `Ⓝ NØCTE` lockup in white on transparency.
`LOGO_NOCTE.png` is the same lockup flattened onto black, and `nocte-sentinel:nocte-wordmark.png`
is byte-identical to it. The `nocte-ide` files (`logo-filled`, `logo-outline`,
`ICONO_NOCTE-IDE`) carry the *NØCTE IDE PRØ* sub-brand and a teal tint, so the slicer does not
use them.

## Why the assets are drawn rather than resampled

The ring in the mark is 6.7 % of its diameter. Downsampling a 1582 px original to a 16 px icon
turns that ring into a sub-pixel grey smear. So the script measures the original once — every
constant in `MARK` and `WORD` is a pixel measurement, reproducible with `--measure` — and
re-draws the same geometry at each target size, thickening the stroke where a target is too small
to carry the nominal weight (`MIN_STROKE_PX`). One geometry model feeds both the Pillow raster
targets and the hand-emitted SVGs, so the vector and raster forms cannot drift apart.

Two rendering conventions differ and are easy to get wrong: Pillow grows an outline *inwards*
from the bounding box, while an SVG stroke is *centred* on the path. The raster helpers therefore
pass the outer box, and the SVG emitters inset by half a stroke.

## Constraints the generated files must keep

- **Monochrome.** `#000000` and `#FFFFFF` only; grey appears solely as anti-aliasing. Those two
  values are also the only ones `BitmapCache::load_svg` never rewrites for dark mode — using
  `#949494` or `#009688` would get the icon recoloured out from under us.
- **No `<text>`.** The SVGs are rendered by nanosvg, which does not implement text. Everything is
  `rect`, `circle`, `ellipse`, `line`, `polygon` and `path`, all of which existing
  `resources/images` icons already use.
- **Same filenames, sizes and formats as upstream.** The C++ and the packaging scripts reference
  these names (`AboutDialog.cpp`, `MainFrame.cpp`, `MsgDialog.cpp`, `ConfigWizard.cpp`,
  `TroubleshootDialog.cpp`, `SendSystemInfoDialog.cpp`, `DesktopIntegrationDialog.cpp`,
  `CMakeLists.txt`, `build_appimage.sh.in`, `scripts/msix/generate_assets.ps1`), so each target
  is written at the exact pixel size of the file it replaces. The script reads that size from the
  file on disk rather than hard-coding it. The one deliberate addition is a 24x24 entry in
  `OrcaSlicer.ico`, which Windows uses for some shell views.

## What is generated

| Target | Size | Content |
|---|---|---|
| `OrcaSlicer.ico` | 16, 24, 32, 48, 64, 128, 256 | white mark on a black rounded square |
| `OrcaSlicerTitle.ico`, `OrcaSlicer-mac_256px.ico` | 154, 256 | same |
| `OrcaSlicer{,Title}.png`, `OrcaSlicer_{32px,64,128px,154,154_title,192px}.png`, `OrcaSlicer-mac_128px.png`, `OrcaSlicer_192px_grayscale.png` | as on disk | same |
| `OrcaSlicer_192px_transparent.png` | 192 | black mark, no plate (it is a watermark on the light ConfigWizard page) |
| `OrcaSlicer.icns` | 1024 | same square icon |
| `OrcaSlicer.svg` | 64 | square icon, for the flatpak/hicolor scalable entry |
| `OrcaSlicer_gradient_circle.svg` | 1024 | circular badge, for `resources/web/homepage` and the MSIX assets |
| `OrcaSlicer_about{,_dark}.svg` | 560x125 | left-aligned lockup plus a rule, for the About dialog |
| `splash_logo{,_dark}.svg` | 480x480 | centred lockup, for the splash screen |
| `OrcaSlicer_horizontal_{light,dark}.svg` | 214x80 | left-aligned lockup, for `TroubleshootDialog` |

The filenames still say `OrcaSlicer` on purpose: renaming them means touching upstream C++ and
packaging files that are not on the fork's touch-point allowlist. The rename is deferred to the
same block as the `SLIC3R_APP_KEY` change.
