# BlipCode16
A fast barcode reader optimized to locate and decode a very specific barcode format in a video frame.
Optimized for speed and horizontal motion blur, using an esp32 camera video source. 
Identifies up to 16 different targets in real time, suitable for motion tracking.

This library was developed to track targets for a passive laser tag game with the "guns" and targets mounted on Powerwheels type ride-on toy cars.

It can identify any of 16 different bar code targets. 
The target format is based on a small subset of "Pharmacode One Track" bar codes. 
Each valid target starts with a narrow bar then a wide bar, then 4 additional bars encoding 0-16. (Corresponding to 79-94 in proper Pharmacode format)
The codes must be read from the top down, and must include a white margin. 
The detector tolerates up to 30 degrees tilt, but it handles motion blur better when the codes are close to horizontal.

The valid code images are included in the /target_images folder. 
A python script to regenerate the codes is also included, just because I already had it ready. 
You can just print the ready made images on your choice of paper.

Note: most of the code was written by Claude (let's be honest- all of the code was writen by Claude.) 
If it makes you feel better, I only used the free tier... 
In my defense, it required a lot of physical testing to get it working well, and the target code format was my idea. 
I can now put "prompt engineer" on my resume.

-Steve

# Description

Blipcode16 is a fast decoder for a custom 6-bar linear barcode, built for
camera-based target tracking (originally: a laser-tag-style game with
targets mounted on moving vehicles). Designed to be used the way you'd use
[quirc](https://github.com/dlbeer/quirc): hand it a grayscale frame buffer
and its dimensions, get back a list of decoded targets and where they are
in the image.

This is not a real Pharmacode/Laetus-standard decoder. It was inspired by that format's simplicity,
(no finder patterns, no error correction, just bar widths) but using our own fixed 6-bar layout. 
If you need a real Pharmacode reader, this isn't it.

## Why not just use a QR/barcode library?

This started as a QR-code-over-WebSocket setup and moved to a custom
format for a few reasons that might matter to your use case too:

- **Speed.** No finder-pattern search, no error-correction decode, no
  version/mask metadata -- a 6-bar linear code is about as cheap to
  decode as a symbol can be.
- **Motion blur resistance.** Bars are read along a single axis; if you
  orient that axis perpendicular to your dominant motion direction (e.g.
  bars running parallel to a horizontal camera sweep), blur elongates the
  bars rather than smearing across the width measurements that actually
  carry the data.
- **Tilt tolerance.** In practice this decoder holds a valid read through
  roughly ±30° of target tilt, since bar/space classification is entirely
  ratio-based (calibrated per-column off the sync pair's own measured
  widths) rather than assuming any fixed pixel geometry.

The tradeoff: only 16 possible codes (4 free data bits), no error
detection beyond the sync-pattern check.

## The protocol

```
[sync: narrow][space][sync: wide][space][data0][space][data1][space][data2][space][data3]
```

- **6 bars total**: 2 fixed sync bars (always narrow, then wide, in that
  order) + 4 data bars.
- **Data bars** are a plain 4-bit field, narrow=0/wide=1, MSB first (the
  data bar immediately after sync is the most significant bit) -- giving
  16 possible codes (0-15).
- **Spaces** carry no information; they're required to be strictly
  between the narrow and wide bar widths measured on that same read, and
  purely serve as separators.
- **Quiet zones** (the white margin before the first bar and after the
  last) must be at least as wide as the wide bar itself.
- **Reading direction is top-to-bottom**, fixed. If your physical mounting
  needs the other direction, rotate the image/camera, not the decoder.

### Why ratios, not pixel sizes

Every width/space check in this decoder is a ratio against that specific
read's own sync pair -- there is no assumption anywhere about how many
pixels a narrow or wide bar "should" be. This is what makes the same
decoder work at close range and at the edge of your camera's resolving
range without retuning: what changes with distance is the absolute pixel
count, not the narrow:wide:space ratio of a correctly-printed target.

The default tolerance constants (`wideRatioMin`/`Max`, `matchFracMin`/
`Max`) were validated against targets printed with a 6:13:18
(narrow:space:wide) pixel ratio at one particular size/distance, and have
worked well across a wide range of real-world distances and lighting
since the checks are relative, not absolute. If you print at a
meaningfully different ratio, these are public fields on the `BlipCode16`
instance -- adjust and re-test rather than assuming the defaults transfer.

### Multi-threshold matching (why lighting doesn't easily break this)

A single global threshold per column (e.g. the midpoint between that
column's brightest and darkest pixel) fails badly in mixed lighting: a
shadow anywhere in the same vertical scan line as a sunlit target will
drag that midpoint away from the target's own local bar/background
contrast, even though the target itself has perfectly good contrast
against its own background.

Instead, each column is tried at several threshold candidates (10th
through 90th percentile of that column's own luminance distribution), and
the **decoded structure itself is the selection criterion** -- whichever
threshold (if any) happens to produce a valid 6-bar pattern wins. There's
no need to identify "the" correct threshold in advance.

## Usage

```cpp
#include <BlipCode16.h>

BlipCode16 reader;

void setup() {
  // Restrict scanning to the vertical band your targets can actually
  // appear in -- a real speed win with none of the downsides a downscale
  // would have (crops pixels the decoder doesn't need to look at, rather
  // than reducing the resolution of the pixels it does look at).
  reader.setBandHeightFraction(0.5f);
}

void loop() {
  camera_fb_t *fb = esp_camera_fb_get();

  BlipCode16Result results[4];
  uint8_t count = reader.decode(fb->buf, fb->width, fb->height, results, 4);

  for (uint8_t i = 0; i < count; i++) {
    // results[i].value: 0-15
    // results[i].corners[0..3]: image-space quad, TL/TR/BR/BL
  }

  esp_camera_fb_return(fb);
}
```

See `examples/ESP32CAM_BlipCode16` for a complete sketch including camera
initialization.

### Image format

`image` must be single-channel 8-bit grayscale, row-major, no padding
(`image[y * width + x]`). On an ESP32-CAM, set `config.pixel_format =
PIXFORMAT_GRAYSCALE` and pass `fb->buf` straight in -- no conversion step.

### Merged results

If a single physical target produces more than one confirmed detection
cluster (e.g. it's wide enough to span past one confirmation pass), those
get merged into a single `BlipCode16Result` before being returned -- so
the result count is the number of *distinct codes* found, not the number
of raw detections. The merged quad is built from the **leftmost** and
**rightmost** individual detection's own top/bottom edges, not an
averaged or axis-aligned box -- so if the target is tilted, the merged
quad's top/bottom lines genuinely skew to match its real orientation
instead of being forced into a rectangle.

## Performance notes

- No dynamic allocation anywhere in `decode()` -- all scratch buffers are
  fixed-size members, sized via `BLIPCODE16_MAX_COLUMN_HEIGHT` /
  `BLIPCODE16_MAX_RUNS` / `BLIPCODE16_MAX_RAW_MATCHES` (all `#define`s,
  override before `#include <BlipCode16.h>` if you need more headroom).
- Threshold candidates are derived from a 256-bin histogram + cumulative
  sum, not a sort -- O(height + 256) per column rather than O(height log
  height).
- Trying multiple thresholds per column costs real time -- if profiling
  shows this mattering on your hardware, `BLIPCODE16_THRESHOLD_CANDIDATES`
  (default 9) is the first knob to turn down.

## Extending

`BLIPCODE16_BARS_TOTAL` / `BLIPCODE16_SYNC_BARS` are fixed at 6/2 in this
version -- changing the bar count isn't just a constant change, since the
data-bit-packing loop and quiet-zone/space checks are written assuming
this specific layout. Treat a different bar count as a fork, not a
config option, unless you're prepared to touch `tryMatchFrom()`.

## License

MIT -- see LICENSE.
