/**
 * BlipCode16.h
 *
 * Decoder for a custom fixed-length linear barcode inspired by (but not
 * compatible with) the real Pharmacode/Laetus one-track standard. Reads a
 * 6-bar symbol from a grayscale image:
 *
 *   [sync: narrow][space][sync: wide][space][data x4, each narrow or wide]
 *
 * The 2 sync bars are a fixed, known pattern (narrow then wide) used both
 * to confirm "this is really one of our codes, not random image noise"
 * and to calibrate the expected narrow/wide pixel widths for THIS
 * specific symbol -- there is no fixed pixel-size assumption anywhere in
 * this decoder, so it works at any distance/zoom the camera can still
 * resolve individual bars at. The remaining 4 data bars are a plain
 * 4-bit field (narrow=0, wide=1), giving 16 possible codes. No error
 * correction -- capacity was intentionally spent entirely on code space,
 * not redundancy. See the accompanying README for the full protocol
 * write-up, including how the tolerance constants below were derived.
 *
 * ---------------------------------------------------------------------
 * USAGE
 * ---------------------------------------------------------------------
 * Designed to be used the way you'd use quirc: hand it a pointer to a
 * grayscale (single byte per pixel) image buffer plus its dimensions,
 * and it hands back a list of decoded targets and their locations. No
 * persistent image copy is kept between calls -- every decode() call is
 * a fresh, independent pass over whatever buffer you give it.
 *
 *   BlipCode16 reader;
 *   reader.setBandHeightFraction(0.5f); // see below
 *
 *   BlipCode16Result results[4];
 *   uint8_t count = reader.decode(fb->buf, fb->width, fb->height,
 *                                  results, 4);
 *   for (uint8_t i = 0; i < count; i++) {
 *     Serial.println(results[i].value); // 0-15
 *   }
 *
 * See examples/ESP32CAM_BlipCode16 for a full sketch using the ESP32
 * camera driver directly.
 *
 * ---------------------------------------------------------------------
 * IMAGE FORMAT
 * ---------------------------------------------------------------------
 * `image` must be single-channel 8-bit grayscale, row-major, with no
 * padding between rows (i.e. image[y * width + x] is the pixel at
 * (x, y)). On an ESP32-CAM, this means configuring the camera driver
 * with PIXFORMAT_GRAYSCALE -- then camera_fb_t::buf is already exactly
 * this format and can be passed straight in with no conversion step.
 *
 * ---------------------------------------------------------------------
 * ORIENTATION
 * ---------------------------------------------------------------------
 * Bars are read top-to-bottom (the sync pair is always the topmost 2
 * bars found in a given vertical scan). If your printed/mounted codes
 * are rotated the other way, rotate the image buffer (or your camera
 * mount) rather than the decoder -- this keeps the decoder itself
 * simple and its behavior easy to reason about.
 *
 * ---------------------------------------------------------------------
 * THE VERTICAL BAND
 * ---------------------------------------------------------------------
 * Real-world use of this decoder (ground-vehicle-mounted targets, a
 * camera that only ever needs to look for them in a horizontal strip of
 * the frame) doesn't need to search the full image height. setBandHeightFraction()
 * restricts scanning to a vertical band of that fraction of the image
 * height, centered on the frame -- this is a genuine speed win (less area
 * to search) with none of the downsides a downscale would have (no loss
 * of the pixel density that determines how far away a code can still be
 * read). Defaults to 1.0 (scan the full height) if never called.
 */

#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------
// Compile-time tuning: scratch buffer sizes, plus
// BLIPCODE16_THRESHOLD_CANDIDATES (a real compute-cost knob, not a
// buffer size -- lower it if decode() is too slow for your hardware,
// at some cost to robustness in harsh/mixed lighting; see README).
// Override any of these by #define-ing before #include <BlipCode16.h>.
// ---------------------------------------------------------------------

#ifndef BLIPCODE16_MAX_COLUMN_HEIGHT
#define BLIPCODE16_MAX_COLUMN_HEIGHT 800  ///< Max supported band height, px.
#endif

#ifndef BLIPCODE16_MAX_RUNS
#define BLIPCODE16_MAX_RUNS 64  ///< Max black/white runs tracked per column per threshold attempt.
#endif

#ifndef BLIPCODE16_MAX_RAW_MATCHES
#define BLIPCODE16_MAX_RAW_MATCHES 32  ///< Max confirmed-but-not-yet-merged clusters per frame.
#endif

#ifndef BLIPCODE16_THRESHOLD_CANDIDATES
#define BLIPCODE16_THRESHOLD_CANDIDATES 9  ///< Compile-time CEILING for the thresholdCandidates field below (array size) -- change the field itself at runtime, not this.
#endif
#define BLIPCODE16_BARS_TOTAL 6            ///< 2 sync + 4 data bars. Fixed; see README to change.
#define BLIPCODE16_SYNC_BARS 2

#ifndef BLIPCODE16_MAX_COARSE_SAMPLES
#define BLIPCODE16_MAX_COARSE_SAMPLES 256  ///< Array size backing the center-out coarse scan order; must exceed coarseSamples.
#endif

/** A single (x, y) pixel coordinate in the source image. */
struct BlipCode16Point {
  int16_t x;
  int16_t y;
};

/**
 * One decoded target: its 4-bit value and its location in the source
 * image, as a quadrilateral rather than an axis-aligned box.
 *
 * The quad is deliberately allowed to be non-rectangular: corners[0]/[3]
 * (the left edge) come from the leftmost confirmed detection of this
 * code, and corners[1]/[2] (the right edge) come from the rightmost --
 * so if the physical target is tilted, the top/bottom edges of this quad
 * will skew to actually match its real orientation instead of being
 * forced into an axis-aligned rectangle. Left/right edges are each side's
 * own straight top-to-bottom run.
 */
struct BlipCode16Result {
  uint8_t value;              ///< Decoded data value, 0-15.
  BlipCode16Point corners[4]; ///< Order: top-left, top-right, bottom-right, bottom-left.
};

class BlipCode16 {
public:
  BlipCode16();

  /**
   * Restricts scanning to a vertical band this fraction of the image
   * height, centered on the frame. Range (0, 1.0]. Defaults to 1.0 (no
   * cropping) until called.
   */
  void setBandHeightFraction(float frac);
  float getBandHeightFraction() const;

  // -------------------------------------------------------------------
  // Structural-match tuning. Defaults below match what was validated
  // against real printed targets generated at a 6:13:18 (narrow:space:
  // wide) pixel ratio -- see the README for how to re-derive these if
  // you're printing at a meaningfully different ratio. All of these are
  // checked as RATIOS relative to widths measured on each column's own
  // sync pair, not fixed pixel values, so they generalize across
  // distance/zoom without retuning -- only the relationship between
  // your narrow/space/wide print sizes should require a revisit here.
  // -------------------------------------------------------------------

  float wideRatioMin = 1.4f;    ///< Min acceptable (wide bar / narrow bar) ratio.
  float wideRatioMax = 4.0f;    ///< Max acceptable (wide bar / narrow bar) ratio.
  float matchFracMin = 0.6f;    ///< A data bar must be >= this fraction of its expected (narrow or wide) width.
  float matchFracMax = 1.6f;    ///< A data bar must be <= this fraction of its expected width.
  uint16_t minCodeHeightPx = 24;///< Reject any candidate shorter than this (bars + internal gaps), px.
  uint8_t minColumnContrast = 20; ///< Skip columns whose brightest/darkest pixel differ by less than this.

  /**
   * Adjacent-column confirmation: a coarse hit must reproduce the same
   * 6-bar pattern on at least confirmMinCount of the next confirmMaxMisses
   * columns to either side before being accepted -- rejects single-column
   * noise flukes that don't have real 2D bar width behind them.
   */
  uint8_t confirmMaxMisses = 10;
  uint8_t confirmMinCount = 3;

  /// Number of evenly-spaced columns sampled in the initial coarse pass.
  uint8_t coarseSamples = 80;

  /**
   * Number of percentile threshold candidates actually tried per column
   * (see the multi-threshold matching explanation in the README) --
   * runtime-settable, e.g. `reader.thresholdCandidates = 3;`. This is the
   * single biggest speed/robustness tradeoff available: fewer candidates
   * is faster but less tolerant of harsh/mixed lighting.
   *
   * NOTE: earlier versions of this library used a compile-time
   * #define for this (BLIPCODE16_THRESHOLD_CANDIDATES), which only
   * worked if you edited BlipCode16.h directly -- a #define in your own
   * sketch before #include <BlipCode16.h> has no effect, since
   * BlipCode16.cpp is a separate translation unit that never sees your
   * sketch's preprocessor definitions. This field replaces that. The
   * macro still exists, but now only as the compile-time ceiling (array
   * size) this field is clamped to -- raise BLIPCODE16_THRESHOLD_CANDIDATES
   * itself only if you want to allow MORE than 9 candidates at runtime.
   */
  uint8_t thresholdCandidates = BLIPCODE16_THRESHOLD_CANDIDATES;

  /**
   * Scans `image` (grayscale, `width` x `height`, row-major, no padding)
   * for BlipCode16 targets, writing up to `maxResults` decoded targets
   * into `results`. Multiple confirmed detections of the same value are
   * merged into a single result (see BlipCode16Result) -- so the return
   * count is the number of DISTINCT codes found, not the number of raw
   * detections.
   *
   * Stops scanning as soon as `maxResults` distinct codes have been
   * confirmed -- e.g. maxResults=1 returns after the first confirmed
   * code rather than scanning the rest of the frame for others. Scan
   * order is center-out (see coarseSamples), so with maxResults=1 this
   * also means "closest to center wins" in the common case where the
   * target is near the aim point.
   *
   * Returns the number of results written (0 if nothing was found).
   * Safe to call every frame; keeps no state between calls EXCEPT the
   * threshold lock (see thresholdCandidates' sibling optimization,
   * documented in the .cpp) which persists across calls by design.
   */
  uint8_t decode(const uint8_t *image, uint16_t width, uint16_t height,
                 BlipCode16Result *results, uint8_t maxResults);

private:
  float _bandHeightFrac = 1.0f;

  // Threshold lock: once a column succeeds at a specific threshold,
  // subsequent columns try that exact value FIRST before falling back to
  // the full multi-candidate search -- adjacent columns of the same
  // physical target under the same lighting almost always share a
  // working threshold, so this turns most of a scan into single-threshold
  // attempts instead of thresholdCandidates-many. Persists across
  // decode() calls (frames), not just within one -- lighting is usually
  // similar frame-to-frame, and a stale lock only costs one quick failed
  // attempt before falling back, so there's no real downside to carrying
  // it over. Cleared on any column that fails to find a result at all
  // (contrast too low OR no valid structural match), per the same "lock
  // until failure" rule that governs when a NEW lock gets set.
  bool _hasLockedThreshold = false;
  uint8_t _lockedThreshold = 0;

  struct Run {
    bool black;
    uint16_t len;
    uint16_t yOffset;
  };

  // One column's full decode attempt result, before adjacent-column
  // confirmation or cross-value merging.
  struct ColumnMatch {
    uint8_t value;
    uint8_t digits[BLIPCODE16_BARS_TOTAL]; // 1=narrow, 2=wide, for confirmation comparisons
    uint16_t totalHeight;
    uint16_t yStart, yEnd; // band-relative
    int16_t x;
  };

  // A confirmed cluster (a coarse hit plus its adjacent-column support),
  // with the horizontal extent it was confirmed across.
  struct ConfirmedMatch {
    uint8_t value;
    uint16_t yStart, yEnd; // band-relative, from the coarse hit column
    int16_t minX, maxX;
  };

  uint8_t candidateThresholds(const uint16_t hist[256], uint16_t n,
                               uint8_t *out, uint8_t count);
  uint8_t runsAtThreshold(const uint8_t *column, uint16_t n, uint8_t threshold,
                           Run *runs, uint8_t maxRuns);
  bool tryMatchFrom(const Run *runs, uint8_t runCount, uint8_t start,
                     int16_t x, ColumnMatch *out);
  bool tryDecodeColumn(const uint8_t *image, uint16_t width, uint16_t bandY,
                        uint16_t bandHeight, int16_t x, ColumnMatch *out);

  // Fixed-size scratch buffers, reused across every column in a decode()
  // call -- no heap allocation anywhere in the decode path.
  uint8_t _columnBuf[BLIPCODE16_MAX_COLUMN_HEIGHT];
  uint16_t _histogram[256];
  Run _runs[BLIPCODE16_MAX_RUNS];
};