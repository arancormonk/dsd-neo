#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerate the Android launcher, themed, notification and splash bitmaps from
# the project logo.
#
# The outputs are committed rather than generated at build time: the Android CI
# job has no ImageMagick, and androiddeployqt packages android/package/res as-is.
# Run this only when images/dsd-neo.png changes, then commit the result.
#
# The source logo is a 1024x1024 RGBA canvas whose ink occupies a 704x544 box
# floating in transparency, so every step below starts by trimming to the ink and
# rescaling it onto the canvas the platform expects.
#
# Usage: tools/gen_android_icons.sh [source.png]

set -euo pipefail

# Density buckets, in the order the size tables below use them.
densities=(mdpi hdpi xhdpi xxhdpi xxxhdpi)

# Adaptive-icon layers live on a 108dp canvas.
adaptive_px=(108 162 216 324 432)

# Legacy square launcher icons are 48dp. minSdk 29 means mipmap-anydpi-v26 always
# wins and these never render, but aapt badging and store tooling expect a bitmap.
legacy_px=(48 72 96 144 192)

# Status-bar notification icons are 24dp.
notification_px=(24 36 48 72 96)

# Splash art is width-driven, not square: the layer-list centres it.
splash_px=(160 240 320 480 640)

# Fraction of the canvas width the ink is allowed to occupy. The adaptive-icon
# safe zone is a 66dp circle inside the 108dp canvas; at 56% the outermost ink of
# this logo (the waveform tails and the "1010" corner) sits just inside that
# circle, so no launcher mask clips it. Legacy icons are full-bleed instead.
adaptive_fill=56
legacy_fill=80
notification_fill=92

# Backdrop for the opaque layers. The logo is drawn for a dark background: near
# black is what makes the cyan/magenta neon read at launcher size.
background='#0E1116'

# Threshold applied to the logo's alpha before it becomes a single-colour layer.
# Themed icons and notification icons are tinted by the system, so only alpha
# survives; without a threshold the soft neon glow smears into a solid blob.
alpha_threshold='45%'

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
src=${1:-$repo_root/images/dsd-neo.png}
res_dir=$repo_root/android/package/res

if [[ ! -f "$src" ]]; then
  echo "Source logo not found: $src" >&2
  exit 2
fi

magick_bin=""
for candidate in magick convert; do
  if command -v "$candidate" > /dev/null 2>&1; then
    magick_bin=$(command -v "$candidate")
    break
  fi
done
if [[ -z "$magick_bin" ]]; then
  echo "ImageMagick (magick or convert) is required." >&2
  exit 2
fi

tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT

art=$tmp_dir/art.png
"$magick_bin" "$src" -trim +repage "$art"

# White-on-transparent silhouette feeding the themed-icon and notification layers.
mono=$tmp_dir/mono.png
"$magick_bin" "$art" -alpha extract -threshold "$alpha_threshold" -transparent black "$mono"

# emit_square <input> <canvas-px> <fill-percent> <background|none> <output>
emit_square() {
  local input=$1 canvas=$2 fill=$3 bg=$4 out=$5
  local width=$((canvas * fill / 100))
  mkdir -p "${out%/*}"
  if [[ "$bg" == "none" ]]; then
    "$magick_bin" "$input" -resize "${width}x" -background none -gravity center \
      -extent "${canvas}x${canvas}" -strip PNG32:"$out"
  else
    "$magick_bin" "$input" -resize "${width}x" -background "$bg" -gravity center \
      -extent "${canvas}x${canvas}" -alpha remove -alpha off -strip "$out"
  fi
}

# emit_wide <input> <width-px> <output>
emit_wide() {
  local input=$1 width=$2 out=$3
  mkdir -p "${out%/*}"
  "$magick_bin" "$input" -resize "${width}x" -strip PNG32:"$out"
}

# Drop the previous run so a change in the density list cannot leave orphans
# behind. The hand-written resources (values/, xml/, drawable/, anydpi) stay.
for density in "${densities[@]}"; do
  rm -rf "$res_dir/mipmap-$density" "$res_dir/drawable-$density"
done

for i in "${!densities[@]}"; do
  density=${densities[$i]}
  mipmap_dir=$res_dir/mipmap-$density
  drawable_dir=$res_dir/drawable-$density

  emit_square "$art" "${adaptive_px[$i]}" "$adaptive_fill" none \
    "$mipmap_dir/ic_launcher_foreground.png"
  emit_square "$mono" "${adaptive_px[$i]}" "$adaptive_fill" none \
    "$mipmap_dir/ic_launcher_monochrome.png"
  emit_square "$art" "${legacy_px[$i]}" "$legacy_fill" "$background" \
    "$mipmap_dir/ic_launcher.png"
  emit_square "$mono" "${notification_px[$i]}" "$notification_fill" none \
    "$drawable_dir/ic_stat_dsdneo.png"
  emit_wide "$art" "${splash_px[$i]}" "$drawable_dir/splash_logo.png"

  echo "Generated $density icons"
done

echo "Icons written to $res_dir"
