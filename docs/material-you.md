# Material You (Material 3) UI

The Android host is a Material 3 app built from Google's own components rather
than a hand-drawn imitation.  Everything below comes from
`com.google.android.material:material:1.12.0`; the token values were read out of
that AAR (`res/values/values.xml`) so they cannot drift from the library.

## References

| What | Source |
|---|---|
| Component library | [Material Components for Android 1.12.0](https://github.com/material-components/material-components-android) |
| Dynamic colour | [`DynamicColors.applyToActivitiesIfAvailable`](https://developer.android.com/develop/ui/views/theming/dynamic-colors) (Android 12+) |
| Colour roles | [M3 colour roles](https://m3.material.io/styles/color/roles) — `colorSurface`, `colorSurfaceContainerHighest`, `colorOnSurface`, `colorOnSurfaceVariant`, `colorPrimary` |
| Shape scale | [M3 shape tokens](https://m3.material.io/styles/shape/shape-scale-tokens) — 12dp medium, 28dp extra-large |
| Type scale | [M3 type tokens](https://m3.material.io/styles/typography/type-scale-tokens) — `HeadlineMedium` 28sp, `BodyLarge` 16sp, `BodyMedium` 14sp, `LabelLarge` 14sp |
| Filled card | `Widget.Material3.CardView.Filled` → `?attr/colorSurfaceContainerHighest`, `?attr/shapeAppearanceCornerMedium` (12dp) |
| Buttons | `Widget.Material3.Button`, `.TonalButton`, `.TextButton`, `.IconButton` (the menu button adds the `colorSecondaryContainer` / `colorOnSecondaryContainer` roles of the filled-tonal icon button) |
| Dialogs | `MaterialAlertDialogBuilder` → 28dp corners (`m3_alert_dialog_corner_size`) |
| Bottom sheet | `BottomSheetDialog` + `BottomSheetDragHandleView` (`Widget.Material3.BottomSheet.Modal`) |
| State layer | 12% of `colorOnSurface` (`RippleDrawable`) |
| Scrim | 32% black (`m3_comp_scrim_container_opacity`) |
| Top app bar | 64dp small app bar (`m3_comp_top_app_bar_small_container_height`) |
| List rows | 56dp menu row, 72dp two-line settings row |

## Mapping in this app

* **Theme** — `Theme.Material3.DayNight.NoActionBar`, with
  `PrimeShimApp` applying the wallpaper palette on Android 12+.  Below Android
  12 the same theme falls back to the official M3 baseline palette.
* **Calculator screen** — the HP framebuffer still owns the screen.  Its only
  chrome is a 48dp filled-tonal icon button (`ic_menu`) over the LCD.
* **Main menu** — `BottomSheetDialog` with a drag handle and 56dp rows.
* **Settings** — `MaterialToolbar` navigation row, `HeadlineMedium` page title,
  filled cards, `MaterialDivider`, and rows with `BodyLarge`/`BodyMedium` text
  plus a Material chevron.  The speed control is the MD3 `Slider` (20–200,
  step 5); switches are `MaterialSwitch`.
* **Log** — full-height surface with an app bar, a filled card holding the
  monospace log, and filled-tonal / text buttons.
* **Feedback** — `Snackbar` for transient messages; `MaterialAlertDialogBuilder`
  only for blocking or must-read results.
* **Splash / icon** — `colorSurface` + `colorPrimary`; adaptive launcher icon
  with a `monochrome` layer for Material You themed icons, and the Android 12
  splash-screen API in `values-v31`.
* **Icons** — `ic_arrow_back`, `ic_close`, `ic_chevron_right` are copied
  verbatim from the Material AAR (Apache-2.0 headers kept).  `ic_menu` is the
  standard Material Icons menu glyph.

No colour, corner radius, elevation or type size is invented in this tree; the
only app-specific values are the HP logo path and the fixed 320×240 guest
framebuffer.
