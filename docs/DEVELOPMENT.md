# FTYS — Development Log

This is the detailed engineering log: design decisions, what was tried and
rejected, and the real-capture validation numbers behind every default
parameter in the pipeline. It used to be the whole README; that file is now
a short, user-facing doc (build/usage/FAQ) and this is where the "why"
lives. If you just want to build and use the app, see [README.md](../README.md)
instead.

## Status

Core engine and Qt GUI are implemented and passing an automated test suite
(io format readers, every processing algorithm, and a full headless
integration run of the pipeline). Built and verified on Linux only so far;
Windows/macOS have not yet been attempted (see Known limitations).

## Project layout

```
src/core/   Image buffer, pixel formats, raw-frame <-> ImageBuffer conversion
            (including bilinear Bayer demosaic), FrameSource interface.
src/io/     SER, AVI (via FFmpeg), and FITS (via CFITSIO) readers, plus the
            extension-based factory (openFrameSource).
src/proc/   Quality metric + percentage frame selection, FFT-based (FFTW)
            translation alignment (whole-frame and multi-point/MAP -- see
            below), mean/sigma-clip stacking, drizzle, a trous wavelet
            sharpening, Richardson-Lucy deconvolution,
            levels/curves/saturation/histogram, chromatic aberration
            correction.
src/gui/    PipelineController (orchestrates the above on QtConcurrent
            worker threads), PreviewWidget (zoom/pan image view),
            CurvesWidget (interactive tone curve + histogram), MainWindow.
src/app/    main.cpp
tests/      test_io, test_proc (unit-level, synthetic data), test_integration
            (drives PipelineController end-to-end headlessly).
```

## Known limitations / simplifications (by design, for this first pass)

- **Alignment (both whole-frame and multi-point) is translation-only.**
  Frame-to-frame rotation from seeing within a single capture run is
  negligible; the field rotation that accumulates over a longer alt-az
  session would need a separate, once-per-run correction, which isn't
  implemented yet.
- **Richardson-Lucy assumes a symmetric Gaussian PSF**, not a measured or
  asymmetric one.
- **AVI is always treated as 8-bit** (Mono8 or RGB24) -- this matches how
  planetary capture AVIs are actually produced; 16-bit workflows go through
  SER or FITS.
- **FITS RGB-cube detection is a heuristic** (NAXIS3==3 is assumed to mean
  three color planes rather than a 3-frame sequence). This matches common
  convention but isn't foolproof.
- **FITS dynamic range for >8-bit data** is normalized using the first
  frame's min/max, held fixed for the rest of the sequence (there's no
  universal FITS convention for this).
- **Export is 8-bit PNG only** for now; 16-bit TIFF/FITS export would be a
  natural next step.
- **Windows/macOS builds are untested.** The code avoids anything
  Linux-specific, but real porting work (dependency packaging via
  vcpkg/Conan, MSVC quirks, macOS app bundling/codesigning) hasn't been
  attempted yet.
- **Drizzle is a resampling operation, not a resolution-restoring one.**
  Drizzling onto a finer output grid changes the sampling density but
  doesn't add information the frames didn't already carry sub-pixel dither
  for. Confirmed on real capture data (before the multi-point alignment
  upgrade below): a 2x drizzle stack looked like a smooth upsample of the
  mean-stack result, not a sharper one. Drizzle earns its keep once genuine
  sub-pixel jitter across frames is present to exploit; worth re-checking
  now that alignment is local rather than a single global shift.
- **Multi-point alignment points are auto-placed only** -- no manual
  add/move/delete UI yet (AutoStakkert-style manual MAP editing). Automatic
  placement (grid of candidates within the object ROI, kept by local
  contrast) covers the common case; a very low-contrast or partly-obscured
  disk might benefit from user-placed points later.
- **Point-to-pixel blending is inverse-distance-weighted over the 3
  nearest points**, not a full local warp/mesh. This is cheap and, per
  real-data testing, already a clear improvement over one global shift, but
  a proper piecewise/triangulated local warp (matching AutoStakkert's own
  approach more closely) is a possible follow-on if more local detail
  recovery is wanted.

## Memory behavior (fixed this pass)

Two real out-of-memory kills (SIGKILL, no dump -- classic OOM-killer
signature) were root-caused against a real ~850-frame 944x632 RGB capture
and fixed:

- Quality assessment used to decode and cache every frame of the whole
  sequence before any selection happened. It now decodes, scores, and
  discards each frame in turn -- only a per-frame (index, score) pair is
  retained -- so its memory use is O(1 frame) regardless of sequence
  length. The frame cache used for alignment/stacking is now built
  afterwards, sized only to the *selected* subset.
- Stacking used to build a full intermediate array of shifted frames
  before accumulating, doubling peak memory right when it was already
  highest. It now samples each source frame directly per output pixel.

The GUI's "Keeping N of M frames" label now also shows the estimated
memory the selected subset will use, so a percentage that would risk
exhausting RAM is visible *before* committing to it, rather than the
process silently dying.

Sequential AVI reads were also sped up significantly (~6-7x on the real
test file) by skipping the seek+flush FFmpeg was doing before every single
frame when the pipeline was already reading forward through the file.

The wavelet sharpening gain sliders were also capped at 3.0, which turned
out to be too restrictive for planetary work in practice; the range is now
0-20.

## Multi-point alignment (added this pass)

Alignment previously computed one FFT phase-correlation shift for the
*whole frame* and applied it uniformly. Real-capture testing showed this
was unreliable: its confidence value sat only ~2x above the theoretical
noise floor for a frame this size (mostly near-black background plus a
soft, low-contrast disk dilutes the correlation signal), and a direct probe
comparing the whole-frame estimate against several small patches placed on
actual disk features (belt edges, limb) found the whole-frame shift could
be tens of pixels away from what every local patch agreed on -- i.e. it was
sometimes locking onto a spurious peak rather than the true motion.

Alignment now works the way dedicated planetary stackers (e.g.
AutoStakkert's Multiple Alignment Points) do:

1. Several tracking points (default up to 16) are placed automatically
   across the detected object, biased toward whatever has real local
   contrast -- a candidate grid is scored by local image gradient and the
   highest-scoring, well-spread points are kept (`selectAlignmentPoints`,
   `src/proc/MultiPointAlignment.cpp`).
2. Each point gets its own FFT phase-correlation shift per frame, on a
   small patch (64x64px, `estimateLocalShift`) rather than the whole frame
   -- both faster in aggregate (less total signal to transform) and, once
   sized correctly (see below), considerably more reliable.
3. A per-frame robustness pass replaces any point whose shift deviates
   wildly from that frame's median (a bad local lock, not real seeing
   distortion -- genuine atmospheric motion moves neighboring points
   together) with the median (`robustifyPointShifts`).
4. Stacking and drizzle blend each pixel's local shift from its 3 nearest
   points (inverse-distance weighted, geometry-only weights precomputed
   once and shared across every frame -- `computeBlendWeights`/
   `blendShiftAt`), instead of reading one shift for the whole frame.

**The first version of this shipped with a real bug, caught by the user
visually comparing output before I did.** The initial patch size (56x56)
was too small for this footage: instrumented directly (a dedicated probe
comparing patch sizes on an identical frame subset), it produced a per-
point shift disagreement of ~16-17px between points on the same frame, and
**49.9% of all point/frame shift estimates were unreliable outliers** that
the robustify fallback had to overwrite with the frame's median -- coin-
flip odds on any single point's own correlation, not real local seeing
variation. The resulting stack looked *softer* than the plain whole-frame
method it was meant to improve on, matching what the user reported after
inspecting the delivered image rather than trusting the confidence number
alone.

Raising the patch size to 96px (and thinning to 12 well-separated points
rather than 16 crowded ones) cut that outlier rate to under 20% in the
same controlled test, and in a same-subset comparison visibly
out-resolved both the 56px and whole-frame alternatives.

**A second real bug, again only visible by actually watching the points
move: most boxes stayed visually frozen while only a few tracked the
planet, and it was specifically the edge points that never moved.**
Instrumented directly against real per-point shifts across the whole
sequence: two of twelve points were reporting a shift under 0.1px on
*every single frame tested*, with confidence 3-8x higher than every other
point (0.3-0.4 vs. 0.04-0.1). Both sat at the outer corners of the
tracking grid, beyond ~75% of the disk's radius. The reason: the
candidate-scoring step ranks positions by raw local contrast, and the
single sharpest edge in the whole frame is the disk-to-black-sky limb
itself -- so the highest-scoring candidates cluster right on top of it. A
96px patch centered there is mostly static black sky, which "aligns
perfectly" frame to frame (hence the inflated confidence) while saying
nothing about the planet's actual motion. That's exactly "boxes at the
edge stay stationary."

Fixed by rejecting any candidate whose full tracking patch would touch
background at all (more than 2% of its pixels below the same
object/background brightness split `detectObjectRoi` already uses),
regardless of how sharp its raw contrast score looks
(`backgroundFraction`, `src/proc/MultiPointAlignment.cpp`). Applied
against the 96px patch size, this was over-corrected: only 4 of 12
points had room to place a full patch without touching background at
all, losing most of the spatial coverage the feature depends on. Patch
size was brought down to 64px (a spacing/point-count sweep across several
sizes on real data confirmed 64px is the largest size that still lets a
full 12-point set fit inside the disk with real spread once background-
touching candidates are excluded) -- verified directly: with the fix,
no point shows the "always near-zero, inflated confidence" signature
across the same real test frames, and the alignment inspector's boxes
visibly move together with the disk in raw mode and stay put on the same
features in aligned mode, both checked against real capture data before
shipping.

A separate, still-present phenomenon: several points away from the edge
occasionally report a large (15-50px), clearly wrong shift on a given
frame -- most likely Jupiter's repeating belt-band structure creating a
competing correlation peak a "belt period" away (a classic periodic-
pattern aliasing risk for phase correlation, not a coding bug). The
per-frame median-based robustness pass catches these -- when enough other
points in that frame are still reliable to form a sane median -- but this
is a real limitation worth knowing about, not something this pass fully
solves.

Worth flagging honestly: Laplacian variance (and "looks textured") can
rise from either genuine resolved detail *or* noise/residual misalignment
grain, and this codebase can't fully distinguish the two from one frame in
isolation. Both bugs above were caught only by actually watching the
alignment points move (or fail to) on real capture data, not by any
single-number metric -- that's the check worth repeating any time this
area changes again.

**A third real bug, found on an actual user capture (out2_2.avi, 6118
frames, mostly-flat MJPEG-compressed footage with none of the visible
belt/festoon texture the earlier fixes were validated against) rather than
synthetic or hand-picked test data.** Screen-recording the alignment
inspector while scrubbing showed most boxes barely moving while the disk
visibly drifted tens of pixels underneath them -- worse than "some points
unreliable," closer to "most points look frozen." Direct per-point
instrumentation against the real file (`estimateLocalShift` +
`robustifyPointShifts`, same constants as production, run against real
frames) found two distinct things layered on top of each other:

1. **10 of 12 points' raw shifts are close to pure noise on this footage**:
   10-40px in magnitude, inconsistent direction and magnitude frame to
   frame, confidence 0.05-0.09 -- barely above the ~0.016 noise floor
   established for whole-frame correlation earlier. Because
   `robustifyPointShifts`'s consensus used to be a plain unweighted median,
   and a *majority* of points were this unreliable (not a minority, as the
   feature was designed to tolerate), the per-frame median landed close to
   a small, near-zero value purely by the arithmetic of averaging
   symmetric noise -- not because that was the true shift. Points got
   pulled toward that fake consensus instead of the other way around. That
   is the direct mechanism behind "boxes barely move here."
2. **2 of 12 points reproduce the earlier edge-lock bug in a form the
   existing background-fraction check doesn't catch.** These two never
   got robustified and carried 2-4x every other point's confidence
   (0.17-0.22 vs 0.05-0.09) while moving under 4px across the whole test.
   They sit at 61-67% of the way to the disk's limb -- inside the object by
   `detectObjectRoi`'s definition (so the 2%-background check doesn't
   reject them), but still dominated by the smooth, static limb-darkening
   gradient rather than real surface texture, which correlates
   "confidently" against itself without reflecting real motion.

**Fix attempted for (1): `robustifyPointShifts`'s consensus is now a
confidence-weighted median (each point's own confidence is its vote
weight) instead of an unweighted one, and any point below a confidence
floor (0.03) is excluded from the vote and unconditionally replaced by the
consensus rather than just measured against it.** Implemented, and it does
not introduce a regression (existing tests still pass, whole-frame/56px/
96px behavior on the earlier test footage is unaffected since those points
were never this uniformly unreliable). But re-running the same real
per-point diagnostic against out2_2.avi before and after shows **no
meaningful improvement** -- the "post" consensus values are essentially
identical, off by hundredths of a pixel. The reason, found by checking the
actual numbers rather than assuming the fix worked: the honestly-noisy
points cluster at confidence 0.05-0.09 and the falsely-confident edge
points at 0.17-0.22 -- not different enough in magnitude, given how wildly
scattered the noisy points' raw values are (10-40px in mixed directions),
for weighting to shift which value ends up as the weighted-median answer.
Confidence, as currently computed, doesn't separate "quietly correct" from
"loudly wrong" cleanly enough on this footage for a vote-weighting scheme
to fix on its own.

A patch-size sweep (64/96/128/160px) against the same real frames, using
the box-size control described below, made things worse, not better: this
capture's object ROI is only ~355x337px, so a bigger patch leaves room for
far fewer spaced-out candidates (7 at 96px, 2 at 128px, 1 at 160px, down
from 12 at 64px), and the points that do survive show no more consistent
or plausible a shift than at 64px -- confidence stays in the same low
range, and the single point left at 160px is frozen near-zero on 11 of 11
test frames. More signal per patch didn't materialize just from making
the patch bigger on this particular footage.

**Net honest state at the time**: the box-size control and the weighted-
consensus change were both shipped and didn't regress anything, but
neither demonstrably fixed this specific capture's alignment quality on
its own -- raw phase-correlation confidence didn't distinguish a patch
that found nothing from one that found the wrong thing confidently.

**Fourth pass: recenter on the frame's own detected object position before
correlating, then only trust a residual correction where an axis actually
found a dominant peak.** This was the user's proposed fix, and real-data
testing confirms it's the single biggest improvement in this whole
investigation:

1. `detectObjectCenter` (`src/proc/QualityMetric.cpp`) computes a sub-pixel,
   brightness-weighted centroid of the thresholded object -- the same
   object detectObjectRoi finds, but a single (x, y) point instead of a
   box. Deliberately not correlation-based: a centroid can't lock onto the
   wrong periodic peak the way phase correlation can, and it still works
   when local contrast is too weak to correlate on at all.
2. Once per frame, `alignSelected` computes that frame's centroid relative
   to the reference's (`recenterOffset`), then passes it into
   `estimateLocalShift` for every point on that frame. Each point's target
   patch is now cropped at `point + recenterOffset` instead of at the same
   fixed position used for the reference -- so the FFT phase correlation
   only has to find whatever's genuinely left over (local seeing
   distortion, real surface evolution), not the whole disk's bulk
   translation on top of it.
3. That residual is applied per axis independently, gated by
   `estimateShiftDetailed`'s new `sharpnessX`/`sharpnessY` (`src/proc/
   Alignment.h`) -- the ratio of the correlation's primary peak to the
   tallest *other* candidate peak along that axis's row/column. Below
   `axisSharpnessThreshold`, that axis's correlation isn't a dominant,
   unambiguous difference, and the coarse recenter is trusted alone for
   it.

**The first attempt at step 3 didn't work, caught by testing rather than
assuming it would.** It measured "sharpness" as the peak's local curvature
normalized by its height. Swept threshold values 0.05 through 0.5 against
real out2_2.avi frames and got *identical* output at every value --
checking why, actual per-point curvature values came back 1-2 to 2.5,
regardless of whether that point's shift was later confirmed right or
wrong. The reason: phase-*only* correlation (the whitened cross-power
spectrum this project uses) produces a sharp, near-impulse-like peak
almost regardless of whether the match is genuine, since every frequency
bin contributes equal magnitude once normalized -- curvature alone can't
tell a real match from a confident-looking accident. Replaced with a
peak-to-second-peak ratio (exclude a small neighborhood around the primary
peak, find the tallest remaining candidate along the same row/column, take
the ratio) -- the standard peak-to-sidelobe idea used to judge correlation-
tracker reliability elsewhere. That one actually separates real matches
from ambiguous ones on this data (values from ~1.1 up to ~4), and a
threshold sweep against real out2_2.avi frames showed results converging
cleanly as it's raised: at 2.0, nearly every point's residual collapses
into tight agreement with the independently-measured recenter, with only
an occasional genuine outlier left for `robustifyPointShifts` to catch
(going higher, e.g. 3.0+, starts rejecting *every* residual and
degenerates into pure centroid-only tracking, losing the point of doing
per-point correlation at all). 2.0 is the new default.

The improvement on real data is large, not marginal. Before recentering,
the 12 points' raw per-frame shifts on out2_2.avi were essentially
independent noise -- 10-40px, inconsistent direction and magnitude,
different points landing nowhere near each other or any consensus. After
recentering, re-running the same per-point instrumentation shows most
points agreeing with each other *and* with the independently-measured
disk centroid to within a pixel or two on most frames, with only the
occasional point needing `robustifyPointShifts` to correct it back --
which now actually works, because the majority it's voting against is
honestly reliable instead of being noise itself. Average reported
confidence on the same real capture went from 0.072-0.078 to 0.114 end to
end. The alignment inspector's boxes visibly move together, coherently,
with the disk in raw mode across widely separated frames, not "a few
drift while most stay frozen" as before.

**Patch size was re-tuned on top of this, and the result reverses the
earlier 64px conclusion.** All of the previous patch-size tuning (56px too
small, 96px too aggressive with background-rejection, 64px as the
settled default) predates recentering and was validated on a different,
higher-contrast test capture. With recentering in place, a fresh sweep
(24/32/40/48/64px) against the real out2_2.avi frames shows *smaller*
patches tracking tighter and more consistently across all 12 points --
32px clearly beat 48px and 64px, which both showed more scatter and a
couple of points meaningfully disagreeing with the rest. This matches
what the user reported independently, from actually using it: "a little
better when I lower the size of the boxes." 32px is the new default.

Why this makes sense in hindsight: once the bulk translation is removed
up front, a patch only needs to be big enough to find the much smaller
*residual* motion, and a smaller patch is less likely to straddle mixed
content (part real texture, part smooth limb gradient, part a different
belt) that a bigger patch would average together. It's also a good
illustration of why re-tuning a parameter without re-validating the
things tuned around it is risky -- 64px was correct for the pipeline that
existed when it was chosen, and stopped being correct the moment the
pipeline's most upstream step changed.

**Tracking patch ("box") size is user-configurable** (a "Box size
(px)" field in the Alignment panel, 24-256px, default 32 -- see above for
how that default was chosen). `PipelineController::
setAlignPatchSize()` clamps and stores it; `alignSelected()` uses it in
place of the previous hardcoded constant, and the minimum point spacing
still scales with it at the same validated ~0.75x ratio. 32px is what
real-data testing on out2_2.avi settled on after recentering was added
(see above), but a different capture's contrast characteristics may want
a different value without a code change.

## Alignment point inspector (added this pass)

"Inspect Alignment Points..." (enabled once alignment completes) opens a
dialog to scroll through the aligned frame subset one at a time, with each
multi-point alignment point drawn as a yellow box -- sized to the actual
tracking patch (see below for how that size is chosen), not a single-pixel
marker, since these track distinct disk features (belt edges, limb), not
individual pixels.

Two view modes, toggled by a checkbox:

- **Raw**: the frame exactly as decoded, with each box positioned at
  wherever that point's tracked feature was actually found in *this
  frame's own pixels* (point position + that frame's local shift).
  Scrubbing through frames, you can watch each box "chase" real belt/limb
  detail -- the direct visual check that tracking is landing on something
  real rather than noise, and exactly the kind of check that caught the
  56px-patch bug described above.
- **Aligned**: the frame after its own per-pixel blended warp (literally
  what stacking samples), with boxes fixed at each point's canonical
  position. If tracking and blending are working, the same real feature
  should sit under the same box in every frame.

The label above the checkbox also reports the original sequence frame
index and this frame's average point confidence, for cross-referencing
against `manual_pipeline_run`'s console output if you're digging deeper.

## Presentation (added this pass)

The stacked result is now cropped to the imaged object (planet/moon/sun
disk plus a margin) before being previewed or exported, instead of
returning the full sensor frame with the object as a small disk lost in a
mostly-black background.

## GUI/workflow fixes (added this pass)

Five changes requested after real-data testing confirmed the recentering
architecture above was a genuine improvement:

- **Control panel widened +20%** (340-420px -> 408-504px): the alignment
  panel now holds three spin boxes (box size, point count, max deviation)
  plus the existing controls, and was getting cramped.
- **Align/Stack now drive the same progress bar as Assess Quality.**
  `PipelineController` gained `alignProgress(int)`/`stackProgress(int)`
  signals with the same 0-100 convention as the existing `qualityProgress`,
  connected to the same `onQualityProgress` slot in `MainWindow` (so it's
  literally the same bar, not a lookalike). `alignSelected()` emits it once
  per processed frame; `stackFrames()`/`drizzleStack()` each gained an
  optional `std::function<void(int)> progressCallback` parameter (default
  `nullptr`, so tests/diagnostics that don't care don't have to pass
  anything) invoked per output row (stackFrames) or per input frame
  (drizzleStack), and `PipelineController` passes a lambda that emits
  `stackProgress`.
- **Number of alignment points is now user-configurable** (a "Number of
  boxes" field, 4-40, default 12 -- 12 was the fixed value used throughout
  this project's real-data testing). `setAlignMaxPoints()` clamps and
  stores it; `alignSelected()` uses it in place of the previous hardcoded
  `kAlignMaxPoints` constant. Verified end-to-end against the real
  out2_2.avi sample with a standalone diagnostic: requesting 6 vs 24 points
  actually produced alignment results with 6 vs 24 points (not silently
  clamped back to the old constant).
- **Per-frame outlier rejection ("sigma clip") threshold is now
  user-configurable** (a "Max deviation (px)" field, 2-50, default 12.0 --
  this is `robustifyPointShifts`' `maxDeviationPx`, previously hardcoded
  and not reachable from the GUI at all). Verified against the real sample:
  a very tight 0.5px threshold collapsed the per-frame spread of point
  shifts to near-zero stddev (~0.055px, i.e. almost every point gets
  snapped to the frame's consensus), while a loose 50px threshold left the
  natural spread mostly intact (~3.18px stddev) -- confirming the parameter
  actually reaches `robustifyPointShifts` rather than being ignored.
- **Reapplying Wavelet or Richardson-Lucy sharpening no longer drops a
  previously-applied histogram/color stretch.** Previously, once color
  adjustments had been applied, re-running sharpening (e.g. after tweaking
  wavelet gains) updated the preview straight from the new unstretched
  `sharpenedResult_` and left `finalResult_` (and the exported image) stuck
  showing the *old* sharpen's color-adjusted output until the user noticed
  and clicked "Apply Color Adjustments" again. `MainWindow` now tracks
  whether color has been applied at least once (`colorApplied_`, reset when
  a new sequence is opened); `onSharpenDone()` automatically re-invokes
  `onApplyColor()` with whatever the color widgets currently hold whenever
  that flag is set, instead of showing the raw sharpen output.

## Alignment inspector zoom fix + chromatic aberration correction (added this pass)

- **Alignment Point Inspector now opens already fit to the window**,
  instead of zoomed far out. Root cause: `MainWindow::onInspectAlignment()`
  calls `refresh()` (which draws the first frame and, in the old code,
  immediately called `fitToWindow()`) *before* `show()` -- at that point the
  dialog's layout hadn't been activated yet, so the preview's
  `QGraphicsView` viewport still reported a stale/default size, and
  `fitInView` computed against that lands far too zoomed out. Fixed by
  making the fit conditional on `isVisible()`: on a re-align while the
  dialog is already open (already correctly sized), it still fits
  immediately; on first open, it's deferred to a new `showEvent()` override
  via `QTimer::singleShot(0, ...)`, which runs after Qt has actually applied
  the pending layout, once the viewport's real size is in place. Verified
  headlessly (offscreen QPA) against the real out2_2.avi sample by
  instantiating the dialog with the exact same `refresh()`-then-`show()`
  call order MainWindow uses and comparing the resulting view transform's
  scale against the independently-computed expected fit-to-window scale:
  ratio 0.996 (previously this kind of bug typically leaves the view at a
  small fraction of the correct scale).
- **Chromatic aberration (RGB channel alignment) correction**, a new panel
  below Histogram/Color. Lateral CA shows up as red/blue fringing around
  planetary limb and belt edges; the fix is a small, independent 2D shift
  applied to the red and blue channels (green held as the fixed reference)
  -- `proc/ChromaticAberration.h`'s `correctChromaticAberration()`, a plain
  per-channel `sampleBilinear` resample, no-op on mono captures or an
  all-zero offset. An "Auto-detect" button reuses inter-frame alignment's
  existing FFT phase correlation (`estimateShift` from `Alignment.h`)
  treating green as the reference channel and red/blue each as a "target"
  channel -- the same subpixel-shift-finding problem, just applied across
  channels of one frame instead of across frames. Runs as the last stage,
  after color adjustments (`PipelineController::applyChromaticAberration()`
  requires `finalResult_`, same requires-the-previous-stage pattern as
  `applySharpen()`/`applyColor()`), and `MainWindow` cascades a color
  re-apply into a CA re-apply (mirroring the sharpen-into-color cascade
  above) so tweaking color after CA correction has already run doesn't
  silently leave the preview/export on a stale CA-corrected image.
  `exportImage()` prefers the CA-corrected result when one exists, falling
  back to the plain color-adjusted result otherwise.

  Verified against the real out2_2.avi sample with a synthetic-shift test:
  a known (+2.3, -1.1)px red-channel and (-1.7, +0.8)px blue-channel
  misregistration was introduced (green channel content re-shifted to build
  synthetic R/B channels, so the only difference from the reference is the
  shift itself, not real per-channel content differences), then
  `detectChromaticAberration()` was run against it. Detected offsets came
  back within ~0.2px of the true values (e.g. red (2.11, -1.03) vs true
  (2.30, -1.10)), and applying the detected correction dropped the mean
  cross-channel residual from 0.00422/0.00369 to 0.00091/0.00108 (red/blue
  respectively) -- roughly a 75-80% reduction, consistent with FFT phase
  correlation's known sub-pixel precision limits rather than a broken
  correction. A first test using real (not synthetically-shifted) R/G/B
  channels showed a smaller residual reduction (~17-31%), which is expected
  and not a bug: real R/G/B channels of a genuine color image differ in
  actual content (Jupiter's belts have real per-band albedo differences),
  not just registration, so their residual can't go to zero from shifting
  alone -- the synthetic-shift test isolates correction accuracy from that
  confound.

## Renamed to FTYS

The project was originally called LuckyStack, but that name turned out to
already be in use by another developer's planetary stacking tool
(LuckyStackWorker). Renamed to FTYS ("Focus Through Your Seeing" --
"seeing" is the real astronomy term for the atmospheric turbulence lucky
imaging fights). `project()`/`add_executable()` in CMakeLists.txt, the
window title, and `QApplication::setApplicationName`/`setOrganizationName`
were all updated accordingly; the binary is now `ftys` instead of
`luckystack`.

## Manual alignment-box editing, and sigma-clipped boxes shown in red

Two additions to multi-point alignment, both scoped to
MultiPointAlignment/PipelineController/AlignmentInspectorDialog:

- **Sigma-clipped boxes shown in red.** `robustifyPointShifts()`
  (MultiPointAlignment.cpp) already replaced an unreliable point's shift
  with the frame's consensus, but silently -- nothing recorded *which*
  points that happened to. Added `Transform2D::clipped` (Alignment.h,
  default `false`), set `true` by `robustifyPointShifts()` whenever it
  overwrites a point's own measurement. `AlignmentInspectorDialog::
  updateView()` now colors a box red for a given frame when
  `pointShiftsForFrame(pos)[i].clipped` is true, yellow otherwise, and the
  info line reports how many of that frame's boxes were clipped. Verified
  on the synthetic Jupiter test capture (see below): 240 of 8000 total
  (frame, point) shifts across the run were clipped (3.0%), and a specific
  box (#44, sitting right at the limb where confidence is naturally
  weaker) showed up red in the saved inspector frame while every other box
  stayed yellow -- confirmed by actually rendering and looking at the
  frame, not just checking the count.

- **Manual box editing (add/move/delete), automatic placement raised to a
  cap of 50.** `PipelineController::kAlignMaxPointsMax` (previously a
  file-local constant, 40) is now a public class constant, raised to 50,
  and shared by three places that used to duplicate or hardcode it: the
  "Number of boxes" spin box's range, `setAlignMaxPoints()`'s clamp, and
  the manual editor's add-limit.

  The per-frame tracking pass that used to live inline in `alignSelected()`
  (recenter + per-point correlation + `robustifyPointShifts`) was factored
  out into a new private `PipelineController::trackPoints(points, patchSize,
  maxDeviationPx)`, which assumes the selected-frame cache and reference
  frame (now cached in new members `referencePos_` / `referenceCenter_`,
  alongside the existing `referenceIndex_`) are already valid.
  `alignSelected()` calls it after automatic placement, exactly as before;
  a new public `realignWithPoints(points)` calls it directly with a
  caller-supplied point list instead, skipping `selectAlignmentPoints()`
  entirely. Both emit the same `alignDone` signal, so nothing downstream
  (MainWindow's `onAlignDone`, which refreshes an open inspector) needed to
  change. `realignWithPoints()` reuses the already-decoded
  `selectedFrameCache_` from the last `alignSelected()` call rather than
  redecoding the sequence, so an edit-then-retrack cycle only pays for the
  correlation pass, not another full decode.

  `PreviewWidget` gained an edit mode: `setEditMode(true)` swaps
  `ScrollHandDrag` for `NoDrag` (so a left-press starts a box interaction
  instead of panning) and starts emitting `imagePressed`/`imageMoved`/
  `imageReleased` signals in *scene* coordinates -- which are the same
  coordinates `AlignmentPoint::x/y` already use, since `setImage()` sizes
  the scene rect to exactly the pixmap's bounding rect at the origin.
  `AlignmentInspectorDialog` added a "Manual box editing" checkbox that
  forces (and disables toggling of) the "aligned" view and disables frame
  scrubbing while active -- boxes are edited in the reference frame's
  canonical coordinate space, and "aligned" is the only view where that
  space lines up consistently across every frame, regardless of which
  frame happens to be displayed. While editing: left-click empty space
  adds a box (rejected past 50, with a message rather than silently
  failing); left-press-and-drag an existing box moves it; right-click
  deletes it. Edits are staged in `editedPoints_` (drawn in cyan, the
  actively-dragged one in white) and only take effect -- get real
  per-frame shifts instead of sitting untracked at their canonical
  position -- once "Re-track with these boxes" calls
  `realignWithPoints(editedPoints_)`; "Reset to automatic placement" just
  calls `alignSelected()` again, reusing whatever patch size/max points/max
  deviation the controller currently holds.

  Verified headlessly against the synthetic Jupiter capture (three
  standalone drivers, not part of ctest): (1) `setAlignMaxPoints(50)` then
  `alignSelected()` placed exactly 50 points, confirming the raised cap
  actually takes effect, not just accepted and silently clamped lower
  elsewhere; (2) a synthetic edit (drop the last point, shift point 0 by
  (+15,+10)px, append a new point at the frame center) passed to
  `realignWithPoints()` completed, reported the edited point count back,
  and produced non-trivial (non-zero) per-frame shifts for a mid-sequence
  frame -- i.e. real tracking ran against the edited list rather than
  copying stale data -- and the too-many-points (51) and empty-list error
  paths both reported `errorOccurred` as expected rather than crashing or
  silently doing nothing; (3) a dialog-level test constructed a real
  `AlignmentInspectorDialog`, toggled its actual "Manual box editing"
  checkbox, drove `PreviewWidget`'s real `imagePressed`/`imageMoved`/
  `imageReleased` signals to add, drag, and right-click-delete boxes
  exactly as real mouse events would, then clicked the real "Re-track"
  button -- confirming the whole add/move/delete/retrack path works
  through the actual widgets, not just the controller underneath them.

## Synthetic test capture

Since no confidently-licensed real planetary AVI sample was readily
available, a synthetic Jupiter-like capture generator was written instead
(`make_synthetic_capture.py`) to exercise every pipeline stage end to end:
a belted, oblate, limb-darkened disk with festoon texture, per-frame
variable "seeing" blur, mount drift + jitter, sensor noise, and a baked-in
per-channel chromatic-aberration shift, encoded to a 400-frame MJPEG AVI
(matching real capture conventions) at 944x632.

Two real bugs were caught by checking actual output rather than assuming
the script did what it was meant to: an oversampled (2x) render step that
was never downsampled back to the target resolution before being used
per-frame (caught via ffmpeg's own reported input size, exactly 2x the
intended W/H), and a baked chromatic-aberration shift applied *before*
that same downsample, silently halving the intended magnitude (fixed by
reordering: downsample first, then shift, so the constants really are
final-resolution pixel offsets as documented).

A third finding was about MJPEG itself, not the script: comparing
`detectChromaticAberration()` on the same frame before and after an
MJPEG round-trip showed a ~40-170x attenuation of the fine, sub-pixel
per-channel shift signal (0.68px measured pre-encode, 0.004-0.02px after,
across several tested magnitudes and quality settings, including 4:4:4
chroma at near-max quality) -- JPEG's chroma quantization is specifically
tuned to discard the kind of fine high-frequency color detail a sub-pixel
fringe amounts to, confirmed by a lossless FFV1 round-trip preserving the
same signal almost exactly. Stacking many frames did not recover it either
(the attenuation is a systematic per-frame effect, not random noise that
averages out), so the baked CA magnitude was increased (from under 1px to
~2-3px per channel) until the fringe was clearly visible in the final
stacked/color-adjusted output by eye -- confirmed in the actual delivered
image -- even though whole-frame FFT auto-detect on the compressed result
still underestimates its true magnitude, a real and honestly-reported
limitation of auto-detecting CA on compressed footage, not a bug in the
CA correction feature itself (which was validated separately against
uncompressed/lossless data, see the chromatic-aberration section above).

The full pipeline (open -> quality -> select -> align -> stack -> sharpen
-> color -> export) was run against the resulting AVI via
`manual_pipeline_run` and produced a clean, sharp, correctly 400-frame,
944x632 result with a visibly correct (if auto-detect-underestimated)
color fringe.

## Next steps

1. Port to Windows and macOS (vcpkg/Conan for dependencies, CI builds).
2. 16-bit export (TIFF/FITS).
3. Live curve-drag preview instead of requiring "Apply".
