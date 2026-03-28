# Desktop Icon Hide Labels — Developer Documentation

## Overview

This Windhawk mod hides the text labels beneath Windows desktop icons and replaces the default selection/focus highlight with a uniform square centered on the icon. It works by injecting into `explorer.exe`, subclassing the desktop's `SysListView32` and `SHELLDLL_DefView` windows, and hooking three GDI/User32 functions that the ListView calls during its paint cycle.

## Architecture

The mod operates at two levels: **window subclassing** for intercepting paint messages and custom draw notifications, and **function hooking** for modifying the actual GDI draw calls.

### Window Hierarchy

The desktop icon view lives inside a chain of windows:

```
Progman (or WorkerW)
  └── SHELLDLL_DefView    ← receives WM_NOTIFY / NM_CUSTOMDRAW
        └── SysListView32 ← the actual icon ListView
```

On startup, the mod walks this hierarchy (checking both `Progman` and `WorkerW` since Windows can use either depending on wallpaper slideshow state) and subclasses both the DefView and the ListView.

### Paint Cycle

When the ListView repaints an icon (hover, click, focus change), the following sequence occurs. The mod intervenes at each numbered step:

1. **WM_PAINT** on `SysListView32` — The LV subclass proc sets `g_inDesktopPaint = true`, computes `iconOffsetY` (label height) and queries the icon size from the image list. All hooks check this flag to avoid interfering with non-desktop drawing.

2. **FillRect** — The ListView erases the full icon cell background. The hook detects this by comparing the rect dimensions against the CUSTOMDRAW cell rect (with tolerance), and replaces it with a centered square rect sized to `max(iconW, iconH) + 2 * HIGHLIGHT_PAD`.

3. **NM_CUSTOMDRAW / CDDS_ITEMPREPAINT** — The DefView subclass captures the original cell rect (stored in `g_currentCellRect` for the hooks to reference), computes the square highlight, and overwrites `cd->nmcd.rc` so the ListView's internal highlight/focus painting uses the square bounds.

4. **DrawTextW** (called 2–3 times per icon) — The hook returns early with a plausible height value. The multiple calls correspond to measuring (DT_CALCRECT), shadow/outline rendering, and final text draw. All are suppressed.

5. **BitBlt** (small, label-area) — A blit covering just the text strip below the icon (dimensions ~64×17). The hook suppresses this entirely.

6. **BitBlt** (large, full-cell) — The final composite blit of the whole icon cell to screen. The hook replaces the coordinates and dimensions with the square highlight rect.

7. **NM_CUSTOMDRAW / CDDS_ITEMPOSTPAINT** — Clears `g_haveItemRect` so the hooks stop matching until the next item.

8. **WM_PAINT returns** — `g_inDesktopPaint` is cleared.

### Rect Identification

Since the GDI hooks fire for every `FillRect`/`BitBlt` call in the process (not just desktop icons), the mod uses several strategies to identify relevant calls:

- **`g_inDesktopPaint` flag** — Only active between PAINT START and PAINT END.
- **`g_haveItemRect` flag** — Only active between ITEMPREPAINT and ITEMPOSTPAINT.
- **`IsFullCellRect` / `IsFullCellBlit`** — Compares against `g_currentCellRect` with ±4 to +20 pixel tolerance to account for padding the ListView adds around the CUSTOMDRAW rc.
- **`IsTextStripRect`** — Matches small rects where `h <= iconOffsetY + 4` and `w < 200`.

### Icon Size Detection

Rather than hardcoding icon dimensions, the mod queries the ListView's `LVSIL_NORMAL` image list via `LVM_GETIMAGELIST` and calls `ImageList_GetIconSize` (resolved dynamically via `GetProcAddress` to avoid linking `comctl32.lib`, which Windhawk's build environment doesn't include). This means the square highlight automatically adapts if the user changes their desktop icon size.

### Label Height Computation

`ComputeLabelHeight` sends `LVM_GETITEMRECT` twice — once with `LVIR_LABEL` to get the label-only rect, and once with `LVIR_BOUNDS` to get the full cell. The label height is `rcBounds.bottom - rcLabel.top`, which captures both the text and the gap between icon and text. This value (`g_iconOffsetY`) is recomputed every paint cycle.

## Key Globals

| Variable | Purpose |
|---|---|
| `g_inDesktopPaint` | Guard flag — hooks only modify behavior when true |
| `g_iconOffsetY` | Computed label area height in pixels |
| `g_iconW`, `g_iconH` | Actual icon dimensions from the image list |
| `g_highlightSide` | Square highlight side length: `max(iconW, iconH) + 2*PAD` |
| `g_currentCellRect` | The raw CUSTOMDRAW rc for the current item |
| `g_squareHighlight` | The computed square rect, centered on the icon |
| `g_haveItemRect` | True between ITEMPREPAINT and ITEMPOSTPAINT |

## Cleanup

On `Wh_ModUninit`, the mod restores both original window procedures and invalidates the ListView to force a clean repaint. The function hooks are automatically removed by Windhawk's framework.

## Limitations and Known Issues

- The shortcut overlay arrow (drawn via `IImageList::Draw` COM vtable) is not currently removed. Hooking the flat `ImageList_Draw`/`ImageList_DrawIndirect` exports does not work because the ListView calls through the COM interface directly.
- The square highlight padding (`HIGHLIGHT_PAD = 5`) is a compile-time constant. A future version could expose this as a Windhawk setting.
- The mod assumes a single desktop ListView. Multiple-monitor configurations with separate ListViews are not handled.
- `g_inDesktopPaint` and related state are not thread-safe (no mutex), but this is acceptable because all desktop painting occurs on explorer's main UI thread.