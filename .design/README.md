# Design canvas working files

Source for the **Yip Aurora Glass** canvas — the glass redesign of the main
window and the floating pill. Each `.dc.html` is one artboard; `canvas.json`
lays them out.

| File | Artboard |
| --- | --- |
| `Main.dc.html` | Main window, idle |
| `Recording.dc.html` | Main window, mid-take |
| `Pill.dc.html` | Floating pill — recording / expanded / saving |
| `Material.dc.html` | Glass recipe, WinUI mapping, per-frame rendering budget |

Palette, meter ramp, type ramp and window geometry are lifted from
[../yip-app/App.xaml](../yip-app/App.xaml) (Dark theme dictionary) and
`kDefaultWindowW/H` in [../yip-app/MainWindow.xaml.cpp](../yip-app/MainWindow.xaml.cpp).
Keep them in step: if a token changes in the app, change it here too, or the
canvas stops being a description of the product.

## Re-seeding

The published canvas is assembled from these files by the `/design` skill's
helper; the assembled `.html` is build output and is gitignored. To change the
design, edit the artboards here and re-run the seed + publish — never edit the
assembled file.
