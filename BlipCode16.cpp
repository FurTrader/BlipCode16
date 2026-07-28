/**
 * BlipCode16.cpp
 *
 * See BlipCode16.h for the protocol description and public API docs.
 * Implementation notes specific to this embedded port (vs. the original
 * JS prototype it was validated against) are called out inline below.
 */

#include "BlipCode16.h"
#include <string.h>

BlipCode16::BlipCode16() {}

void BlipCode16::setBandHeightFraction(float frac) {
  _bandHeightFrac = frac;
}

float BlipCode16::getBandHeightFraction() const {
  return _bandHeightFrac;
}

// ---------------------------------------------------------------------
// Reads one column of the image into a flat scratch buffer and builds a
// 256-bin luminance histogram over it in the same pass. Also returns the
// column's min/max pixel value via outMin/outMax, used for the cheap
// "any contrast at all in this column" early-out in tryDecodeColumn.
// ---------------------------------------------------------------------
static void buildColumnAndHistogram(const uint8_t *image, uint16_t width,
                                     uint16_t bandY, uint16_t bandHeight,
                                     int16_t x, uint8_t *outColumn,
                                     uint16_t hist[256], uint8_t *outMin,
                                     uint8_t *outMax) {
  memset(hist, 0, sizeof(uint16_t) * 256);
  uint8_t mn = 255, mx = 0;
  for (uint16_t y = 0; y < bandHeight; y++) {
    uint8_t px = image[(uint32_t)(bandY + y) * width + x];
    outColumn[y] = px;
    hist[px]++;
    if (px < mn) mn = px;
    if (px > mx) mx = px;
  }
  *outMin = mn;
  *outMax = mx;
}

// ---------------------------------------------------------------------
// Percentile thresholds from the column's own histogram (10%..90% in
// even steps). This is the embedded-appropriate version of what the JS
// prototype did by sorting the column's pixel array -- a histogram over
// the 256 possible 8-bit luminance values plus a cumulative-sum walk is
// O(height + 256) instead of an O(height log height) sort, and needs no
// scratch copy of the column beyond the histogram itself.
//
// The reason to try several thresholds at all, rather than one global
// (min+max)/2 midpoint: that midpoint gets dragged around by anything
// extreme elsewhere in the column -- e.g. a shadow far from a sunlit
// target skews the "true" split point away from where the target's own
// local bar/background contrast actually sits. Trying several candidates
// and letting the structural match in tryMatchFrom() decide which one
// (if any) produces a valid 6-bar pattern sidesteps needing to guess a
// single correct threshold at all.
// ---------------------------------------------------------------------
uint8_t BlipCode16::candidateThresholds(const uint16_t hist[256], uint16_t n,
                                         uint8_t *out, uint8_t count) {
  uint16_t cum[256];
  uint16_t running = 0;
  for (uint16_t v = 0; v < 256; v++) {
    running += hist[v];
    cum[v] = running;
  }
  for (uint8_t i = 1; i <= count; i++) {
    float p = (float)i / (count + 1);
    uint16_t target = (uint16_t)(p * (n - 1));
    uint16_t v = 0;
    while (v < 255 && cum[v] <= target) v++;
    out[i - 1] = (uint8_t)v;
  }
  return count;
}

// ---------------------------------------------------------------------
// Segments one column into alternating black/white runs at a single
// threshold. Bails out (returning whatever it has so far) if a column
// is noisy enough to exceed maxRuns -- that many transitions in one
// column is not a real 6-bar code regardless, so there's nothing lost
// by not tracking further.
// ---------------------------------------------------------------------
uint8_t BlipCode16::runsAtThreshold(const uint8_t *column, uint16_t n,
                                     uint8_t threshold, Run *runs,
                                     uint8_t maxRuns) {
  uint8_t count = 0;
  bool curBlack = column[0] < threshold;
  uint16_t runLen = 1;
  uint16_t yOff = 0;

  for (uint16_t y = 1; y < n; y++) {
    bool black = column[y] < threshold;
    if (black == curBlack) {
      runLen++;
    } else {
      if (count >= maxRuns) return count; // too noisy, bail
      runs[count].black = curBlack;
      runs[count].len = runLen;
      runs[count].yOffset = yOff;
      count++;
      yOff += runLen;
      curBlack = black;
      runLen = 1;
    }
  }
  if (count < maxRuns) {
    runs[count].black = curBlack;
    runs[count].len = runLen;
    runs[count].yOffset = yOff;
    count++;
  }
  return count;
}

// ---------------------------------------------------------------------
// Attempts a full sync+data match starting at runs[start] (which must be
// a black/bar run). Every width/space check here is a ratio against the
// sync pair's OWN measured widths -- there is no fixed pixel-size
// assumption anywhere in this function, which is what makes the decoder
// work across distance/zoom without retuning.
//
// Returns false on any failed check; the caller (tryDecodeColumn) moves
// on to the next start position rather than giving up on the column --
// i.e. a bad guess at where the sync pattern begins just costs one
// failed attempt, not the whole read.
// ---------------------------------------------------------------------
bool BlipCode16::tryMatchFrom(const Run *runs, uint8_t runCount,
                               uint8_t start, int16_t x, ColumnMatch *out) {
  if (!runs[start].black) return false;
  if (start == 0) return false; // no room for a quiet-before run

  const uint8_t needed = BLIPCODE16_BARS_TOTAL * 2 - 1; // 6 bars + 5 internal spaces
  if ((uint16_t)start + needed > runCount) return false;

  for (uint8_t i = 0; i < needed; i++) {
    bool shouldBeBlack = (i % 2 == 0);
    if (runs[start + i].black != shouldBeBlack) return false;
  }

  const uint16_t unitNarrow = runs[start].len;     // sync bar 1
  const uint16_t syncSpace = runs[start + 1].len;
  const uint16_t unitWide = runs[start + 2].len;   // sync bar 2

  const float wideRatio = (float)unitWide / (float)unitNarrow;
  if (wideRatio < wideRatioMin || wideRatio > wideRatioMax) return false;

  // Space is strictly wider than a narrow bar and narrower than a wide
  // bar -- calibrated directly off this sync pair's own measured widths,
  // not a separately-guessed ratio constant.
  if (syncSpace <= unitNarrow || syncSpace >= unitWide) return false;

  uint8_t digits[BLIPCODE16_BARS_TOTAL];
  digits[0] = 1;
  digits[1] = 2;

  for (uint8_t i = 4; i < needed; i += 2) {
    const uint16_t spaceLen = runs[start + i - 1].len;
    const uint16_t barLen = runs[start + i].len;

    if (spaceLen <= unitNarrow || spaceLen >= unitWide) return false;

    const uint16_t distNarrow = (barLen > unitNarrow) ? (barLen - unitNarrow) : (unitNarrow - barLen);
    const uint16_t distWide = (barLen > unitWide) ? (barLen - unitWide) : (unitWide - barLen);
    const uint8_t barIndex = BLIPCODE16_SYNC_BARS + (i - 4) / 2;

    if (distNarrow <= distWide) {
      if (barLen < unitNarrow * matchFracMin || barLen > unitNarrow * matchFracMax) return false;
      digits[barIndex] = 1;
    } else {
      if (barLen < unitWide * matchFracMin || barLen > unitWide * matchFracMax) return false;
      digits[barIndex] = 2;
    }
  }

  // Quiet zone before/after must be clearly larger than any internal
  // space -- at least as wide as the wide bar itself.
  const Run &quietBefore = runs[start - 1];
  if (quietBefore.black || quietBefore.len < unitWide) return false;

  const uint16_t afterIdx = start + needed;
  if (afterIdx >= runCount) return false;
  const Run &quietAfter = runs[afterIdx];
  if (quietAfter.black || quietAfter.len < unitWide) return false;

  uint16_t totalHeight = 0;
  for (uint8_t i = 0; i < needed; i++) totalHeight += runs[start + i].len;
  if (totalHeight < minCodeHeightPx) return false;

  uint8_t value = 0;
  for (uint8_t i = BLIPCODE16_SYNC_BARS; i < BLIPCODE16_BARS_TOTAL; i++) {
    value = (uint8_t)((value << 1) | (digits[i] - 1));
  }

  out->value = value;
  memcpy(out->digits, digits, BLIPCODE16_BARS_TOTAL);
  out->totalHeight = totalHeight;
  out->yStart = runs[start].yOffset;
  out->yEnd = out->yStart + totalHeight;
  out->x = x;
  return true;
}

// ---------------------------------------------------------------------
// Tries the locked threshold (if any) first, then every candidate
// threshold in turn, and within each, every possible start position --
// i.e. the "exactly 6 dark bands in valid proportion" structure IS the
// selection criterion for which threshold is correct for this column,
// rather than committing to one threshold upfront. Returns on the first
// full match found.
// ---------------------------------------------------------------------
bool BlipCode16::tryDecodeColumn(const uint8_t *image, uint16_t width,
                                  uint16_t bandY, uint16_t bandHeight,
                                  int16_t x, ColumnMatch *out) {
  uint8_t colMin, colMax;
  buildColumnAndHistogram(image, width, bandY, bandHeight, x, _columnBuf,
                           _histogram, &colMin, &colMax);
  if ((uint16_t)(colMax - colMin) < minColumnContrast) {
    _hasLockedThreshold = false; // this column found nothing at all; drop any lock
    return false;
  }

  // Fast path: adjacent columns of the same physical target under the
  // same lighting almost always share a working threshold, so try
  // whatever worked last before paying for the full multi-candidate
  // search again. No point re-searching once we already know a value
  // that works here.
  if (_hasLockedThreshold) {
    uint8_t runCount = runsAtThreshold(_columnBuf, bandHeight, _lockedThreshold,
                                        _runs, BLIPCODE16_MAX_RUNS);
    for (uint8_t start = 0; start < runCount; start++) {
      if (tryMatchFrom(_runs, runCount, start, x, out)) return true; // lock still good
    }
    // Locked threshold didn't work for this column -- fall through to a
    // full search rather than giving up (lock only gets cleared below if
    // the full search ALSO fails).
  }

  uint8_t candidateCount = thresholdCandidates;
  if (candidateCount > BLIPCODE16_THRESHOLD_CANDIDATES) {
    candidateCount = BLIPCODE16_THRESHOLD_CANDIDATES; // can't exceed the array's compile-time size
  }

  uint8_t thresholds[BLIPCODE16_THRESHOLD_CANDIDATES];
  candidateThresholds(_histogram, bandHeight, thresholds, candidateCount);

  for (uint8_t t = 0; t < candidateCount; t++) {
    uint8_t runCount = runsAtThreshold(_columnBuf, bandHeight, thresholds[t],
                                        _runs, BLIPCODE16_MAX_RUNS);
    for (uint8_t start = 0; start < runCount; start++) {
      if (tryMatchFrom(_runs, runCount, start, x, out)) {
        _lockedThreshold = thresholds[t]; // lock onto whatever just worked
        _hasLockedThreshold = true;
        return true;
      }
    }
  }

  _hasLockedThreshold = false; // total failure at this column; drop the lock
  return false;
}

uint8_t BlipCode16::decode(const uint8_t *image, uint16_t width,
                            uint16_t height, BlipCode16Result *results,
                            uint8_t maxResults) {
  uint16_t bandHeight = (uint16_t)(height * _bandHeightFrac + 0.5f);
  if (bandHeight > BLIPCODE16_MAX_COLUMN_HEIGHT) {
    bandHeight = BLIPCODE16_MAX_COLUMN_HEIGHT; // see BLIPCODE16_MAX_COLUMN_HEIGHT
  }
  uint16_t bandY = (height - bandHeight) / 2;

  uint16_t coarseStep = width / coarseSamples;
  if (coarseStep < 1) coarseStep = 1;

  // Center-out coarse sample order: the target is usually near the
  // aim/reticle center, so checking there first (and working outward)
  // finds it faster on average than a fixed left-to-right sweep, while
  // still covering the same set of positions overall. This also means
  // maxResults=1 effectively returns "whichever confirmed code is
  // closest to center" rather than "whichever is leftmost".
  int16_t coarseX[BLIPCODE16_MAX_COARSE_SAMPLES];
  uint16_t coarseCount = 0;
  {
    int32_t center = ((int32_t)width / 2 / coarseStep) * coarseStep;
    if (coarseCount < BLIPCODE16_MAX_COARSE_SAMPLES) coarseX[coarseCount++] = (int16_t)center;

    int32_t left = center - coarseStep;
    int32_t right = center + coarseStep;
    bool takeLeft = true;
    while ((left >= 0 || right < width) && coarseCount < BLIPCODE16_MAX_COARSE_SAMPLES) {
      if (takeLeft) {
        if (left >= 0) { coarseX[coarseCount++] = (int16_t)left; left -= coarseStep; }
        else if (right < width) { coarseX[coarseCount++] = (int16_t)right; right += coarseStep; }
      } else {
        if (right < width) { coarseX[coarseCount++] = (int16_t)right; right += coarseStep; }
        else if (left >= 0) { coarseX[coarseCount++] = (int16_t)left; left -= coarseStep; }
      }
      takeLeft = !takeLeft;
    }
  }

  // Coarse scan + adjacent-column confirmation. A confirmed cluster here
  // is one physical detection event -- the same real code can still
  // produce more than one cluster if it spans wider than one confirm
  // pass, which is why merging by value happens as a separate step below.
  ConfirmedMatch rawMatches[BLIPCODE16_MAX_RAW_MATCHES];
  uint8_t rawCount = 0;

  bool valueSeen[16] = {false};
  uint8_t distinctSeen = 0;

  for (uint16_t ci = 0; ci < coarseCount && rawCount < BLIPCODE16_MAX_RAW_MATCHES; ci++) {
    int16_t x = coarseX[ci];

    // Skip columns already covered by a previously confirmed match's
    // neighborhood. This replaces the old single-direction "x +=
    // confirmMaxMisses" skip-ahead, which doesn't translate cleanly to a
    // bidirectional center-out scan order -- a simple range check against
    // already-confirmed clusters works regardless of scan order.
    bool alreadyCovered = false;
    for (uint8_t m = 0; m < rawCount; m++) {
      if (x >= rawMatches[m].minX - (int16_t)confirmMaxMisses &&
          x <= rawMatches[m].maxX + (int16_t)confirmMaxMisses) {
        alreadyCovered = true;
        break;
      }
    }
    if (alreadyCovered) continue;

    ColumnMatch hit;
    if (!tryDecodeColumn(image, width, bandY, bandHeight, x, &hit)) continue;

    uint8_t confirmCount = 0;
    int16_t minX = x, maxX = x;
    for (uint8_t d = 1; d <= confirmMaxMisses; d++) {
      int32_t candidates[2] = { x + d, x - d };
      for (uint8_t c = 0; c < 2; c++) {
        int32_t nx = candidates[c];
        if (nx < 0 || nx >= width) continue;
        ColumnMatch nhit;
        if (tryDecodeColumn(image, width, bandY, bandHeight, (int16_t)nx, &nhit) &&
            memcmp(nhit.digits, hit.digits, BLIPCODE16_BARS_TOTAL) == 0) {
          confirmCount++;
          if ((int16_t)nx < minX) minX = (int16_t)nx;
          if ((int16_t)nx > maxX) maxX = (int16_t)nx;
        }
      }
    }

    if (confirmCount >= confirmMinCount) {
      rawMatches[rawCount].value = hit.value;
      rawMatches[rawCount].yStart = hit.yStart;
      rawMatches[rawCount].yEnd = hit.yEnd;
      rawMatches[rawCount].minX = minX;
      rawMatches[rawCount].maxX = maxX;
      rawCount++;

      if (!valueSeen[hit.value]) {
        valueSeen[hit.value] = true;
        distinctSeen++;
      }
      // Caller only wants maxResults distinct codes -- once we have that
      // many, further scanning is wasted work. Note this means decode()
      // no longer always scans the full frame when maxResults is small;
      // previously it always scanned everything and only truncated
      // output at the very end.
      if (distinctSeen >= maxResults) break;
    }
  }

  // Merge by value: at most 16 possible values, so a flat 16-slot table
  // indexed directly by value stands in for a hash map. Left/right are
  // each fully replaced (not field-by-field) by whichever raw cluster is
  // furthest in that direction, so the merged quad's top/bottom edges
  // come from two REAL detections rather than an averaged/synthetic one
  // -- see BlipCode16Result's doc comment for why that matters for tilt.
  struct Group {
    bool seen;
    int16_t leftMinX; uint16_t leftYStart, leftYEnd;
    int16_t rightMaxX; uint16_t rightYStart, rightYEnd;
  };
  Group groups[16] = {};

  for (uint8_t i = 0; i < rawCount; i++) {
    const ConfirmedMatch &m = rawMatches[i];
    Group &g = groups[m.value];
    if (!g.seen) {
      g.seen = true;
      g.leftMinX = m.minX; g.leftYStart = m.yStart; g.leftYEnd = m.yEnd;
      g.rightMaxX = m.maxX; g.rightYStart = m.yStart; g.rightYEnd = m.yEnd;
    } else {
      if (m.minX < g.leftMinX) {
        g.leftMinX = m.minX; g.leftYStart = m.yStart; g.leftYEnd = m.yEnd;
      }
      if (m.maxX > g.rightMaxX) {
        g.rightMaxX = m.maxX; g.rightYStart = m.yStart; g.rightYEnd = m.yEnd;
      }
    }
  }

  uint8_t outCount = 0;
  for (uint16_t v = 0; v < 16 && outCount < maxResults; v++) {
    if (!groups[v].seen) continue;
    const Group &g = groups[v];
    BlipCode16Result &r = results[outCount];
    r.value = (uint8_t)v;
    // Translate band-relative y back into full-image coordinates.
    r.corners[0] = { g.leftMinX,  (int16_t)(bandY + g.leftYStart) };  // top-left
    r.corners[1] = { g.rightMaxX, (int16_t)(bandY + g.rightYStart) }; // top-right
    r.corners[2] = { g.rightMaxX, (int16_t)(bandY + g.rightYEnd) };   // bottom-right
    r.corners[3] = { g.leftMinX,  (int16_t)(bandY + g.leftYEnd) };    // bottom-left
    outCount++;
  }

  return outCount;
}