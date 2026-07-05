# Build-specific renderer rules

## Graphics presets

- The four graphics presets may directly change only controls that are visible in the F11 menu.
- A visible mode change may apply that mode's normal dependent behavior. In particular, selecting FSR 3 or TAA may set hidden CAS sharpening to `1.0`, while SSAA or disabled anti-aliasing may keep CAS at `0.2`.
- Presets must never directly alter hidden shader quality, sample counts, tuning constants, or other parameters that have no visible F11 control.

## Finished build contents

- Finished builds must not retain generated source images, previews, conversion intermediates, or other files that are not used by compilation, runtime loading, distribution, licensing, or attribution.
- Before removing an asset, verify its references and packaging role; once proven unused, remove it from the finished build instead of shipping it beside the runtime asset.
