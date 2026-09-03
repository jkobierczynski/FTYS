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
integration run of the pipeline). Built and verified on Linux, and now
also confirmed built and running end to end on a real Windows machine
(MSVC + vcpkg) after several rounds of real-user build fixes -- see the
"Windows port" entry below. macOS hasn't been attempted yet (see Known
limitations).

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

## Quality Inspector: sorted sharpness graph with a scrub preview

Added a "View Quality Graph..." button to Frame Selection, opening a new
`QualityInspectorDialog` (`src/gui/QualityInspectorDialog.h/.cpp`), backed
by a new reusable `QualityGraphWidget` (`src/gui/QualityGraphWidget.h/.cpp`)
following the same custom-painted-widget convention as `CurvesWidget`
(dark background, dotted grid, `kMargin = 12`).

`PipelineController::qualityScoresDebug()` was renamed to `qualityScores()`
and its doc comment updated -- it's no longer just a diagnostic accessor
for `manual_pipeline_run`/`wavelet_diagnostic`, the GUI now depends on it
too. `QualityInspectorDialog::refresh()` copies that (original-index-order)
vector and `std::stable_sort`s it descending by score -- deliberately the
same comparator `selectTopPercent` (FrameSelector.cpp) uses internally, so
`rank < keptCount` in the dialog always agrees with which original frames
`selectPercent()` actually kept, without the dialog needing to duplicate
`selectTopPercent`'s own logic or ask the controller to expose it. That one
sorted vector (`sorted_`) is the single ordering shared by the graph, the
slider/spin box, and the frame preview -- "rank" means the same position
in all three.

`QualityGraphWidget` just draws whatever vector it's handed in the order
it's handed (it doesn't sort or know about "kept" beyond a `keptCount`
integer boundary) -- a green/gray bar chart with a dashed cutoff line at
`keptCount` and a bright yellow scrub cursor, plus a `rankClicked(int)`
signal so clicking or dragging directly on the graph moves the dialog's
own slider (not just the other way around).

Two refresh paths, not one, because they need different side effects:
`refresh()` (called after a genuinely new `computeQuality()` pass) resets
the scrub position back to rank 0 and re-fits the preview, since the whole
distribution -- and what each rank even refers to -- may have changed, same
convention as `AlignmentInspectorDialog::refresh()`. `refreshSelection()`
(called after `selectPercent()` alone, i.e. the "Keep best %" slider moved
but the scores didn't) only recolors the graph's kept/not-kept boundary and
updates the info line, leaving the current scrub position and preview
zoom alone -- an early version called `refresh()` from both paths and it
was genuinely disruptive: dragging the percent slider while the dialog was
open kept yanking the view back to rank 0 and re-fitting the zoom on every
tick. `MainWindow::onSelectionChanged` (which fires on every percent-slider
tick) calls `refreshSelection()`; `onQualityDone` calls the full `refresh()`.

The preview reuses the same fitInView-before-layout-is-ready fix as
`AlignmentInspectorDialog` (`showEvent()` + a deferred `QTimer::singleShot(0,
...)`, `fitDone_` guard) -- copied deliberately rather than re-derived, since
that exact bug (zoomed-out-on-first-open) was already hunted down once for
the alignment inspector.

Verified headlessly against the synthetic Jupiter capture (not part of
ctest): confirmed the slider/spin box range matches the frame count,
confirmed a graph `rankClicked` signal moves the spin box (and therefore
the slider, via their existing sync connection), confirmed the info label
reports the same original frame index and score as an independently
computed sort for a specific rank, and confirmed `refreshSelection()` after
a stricter `selectPercent()` call updates a rank's kept/not-kept text
without moving the scrub position away from where the user left it. A
rendered screenshot of the dialog against the synthetic capture (sharpest
frame, index 36, at rank 0) showed the expected sorted green-to-gray bar
shape with the cutoff and cursor lines in the right places.

## FITS/TIFF export and 16-bit output

Export previously only wrote 8-bit PNG via Qt's own `QImage::save()`. Added
a new `io/ImageWriter.h/.cpp` module (CFITSIO and libtiff directly, not
through Qt) so TIFF and FITS can be written at the internal pipeline's full
float precision instead of always being rounded down to 8-bit, and added
FITS as an export format alongside PNG/TIFF. The Export panel now has a
**Format** combo (PNG/TIFF/FITS) and a **Bit depth** combo (8-bit/16-bit);
the bit-depth combo is disabled and forced to 8-bit whenever PNG is
selected (`MainWindow::onExportFormatChanged`), since Qt's PNG writer,
which PNG export still goes through unchanged, doesn't offer 16-bit output.
`ImageWriter::writeImage()` deliberately doesn't handle
`ExportFormat::PNG` at all (returns false) -- that format stays on the
pre-existing `imageBufferToQImage(...).save(path)` path in
`PipelineController::exportImage()`, which now takes `ExportFormat` and
`ExportBitDepth` parameters (both defaulted, so the existing
`manual_pipeline_run`/`test_integration` call sites that just pass a path
still compile unchanged).

Quantization for TIFF/FITS follows the same clamp-to-[0,1]-and-rescale
convention `imageBufferToQImage` already uses for PNG (not a per-image
min/max renormalization) -- "what you see in the preview is what gets
written," just at 8 or 16 bits instead of always 8.

FITS output mirrors `FitsReader`'s own read convention exactly, since that
class already existed and its layout wasn't going to change to suit a new
writer: NAXIS=2 for a mono frame, or NAXIS=3 with NAXIS3=3 for RGB stored
as three separate planes (not interleaved) -- confirmed by reading
`FitsReader.h`/`.cpp` before writing a single line of the writer. 16-bit
FITS is written via CFITSIO's `USHORT_IMG`/`TUSHORT`, which sets
BZERO=32768/BSCALE=1 automatically (the standard "unsigned 16-bit via a
signed-16 container" FITS convention) rather than native signed BITPIX=16
-- confirmed against `/usr/include/fitsio.h` and cross-checked with
`astropy.io.fits` during validation (below), which read back exactly that
header shape unprompted. TIFF tags are set explicitly rather than left to
libtiff defaults: `SAMPLEFORMAT_UINT`, `PHOTOMETRIC_MINISBLACK` (mono) or
`PHOTOMETRIC_RGB` (color), `PLANARCONFIG_CONTIG`, `ORIENTATION_TOPLEFT`,
and `BITSPERSAMPLE` of 8 or 16 depending on the requested depth.

New dependency: `libtiff-4` (pkg-config), matching the project's existing
pattern of pulling in CFITSIO/FFTW3 the same way; added to
`CMakeLists.txt`'s `ls_io` target and to the Linux build dependency list
in the README (`libtiff-dev`).

Validated with a new headless harness, `tests/export_validation.cpp` (not
part of ctest, same convention as `manual_pipeline_run`/`inspector_verify`)
run against the synthetic Jupiter capture through the real pipeline
(open -> quality -> select -> align -> stack -> sharpen -> color), then
exporting all five meaningful combinations (PNG-8, TIFF-8, TIFF-16,
FITS-8, FITS-16) and checking each file actually exists and is non-empty.
The two FITS files were round-tripped back through the project's own
`FitsReader` and spot-checked against the source preview (max pixel
deviation 2.9e-8 for 8-bit, 0.0019 for 16-bit -- both consistent with
quantization, not a bug). That alone only proves the writer and reader
agree with *each other*, though, so the same files were independently
checked with tools that have nothing to do with this codebase:
`tifffile`/ImageMagick `identify` on the TIFF files confirmed genuine
16-bit-per-channel storage (`identify` reported `Depth: 16-bit`; `tifffile`
read back `dtype=uint16` with values scaled ~257x versus the 8-bit export,
as expected for 255->65535 range quantization of the same source data),
and `astropy.io.fits` on the FITS files independently confirmed
NAXIS=3/NAXIS1=531/NAXIS2=506/NAXIS3=3, BITPIX=8 (no BZERO/BSCALE) for the
8-bit file and BITPIX=16/BZERO=32768/BSCALE=1 for the 16-bit file exactly
as designed. Finally, the TIFF-8 and FITS-8 outputs (both quantizing the
*same* source buffer to 8-bit, just through different libraries) were
compared pixel-for-pixel after reordering FITS's plane-major layout to
interleaved -- an exact match (max abs diff 0), which is the strongest
evidence the plane-order/channel-order convention is consistent across
both writers, not just internally self-consistent.

## Quality Graph: logarithmic scale

Quality scores (Laplacian variance) commonly span a couple of orders of
magnitude between the sharpest frames in a capture and the softest -- on
the graph's original linear scale that squashes almost the whole
distribution, including the "keep best %" cutoff region (which usually
sits well below the very best frames, not near them), down near the
bottom axis where it's hard to read. Added a **Logarithmic scale**
checkbox next to the existing legend line in `QualityInspectorDialog`,
wired straight to a new `QualityGraphWidget::setLogScale(bool)` --
deliberately a separate call from `setData()` rather than a parameter to
it, so toggling the checkbox doesn't require re-supplying the (unchanged)
score vector and the setting persists naturally across `refresh()`/
`refreshSelection()` calls.

The mapping itself needs a positive floor, since zero (and any measured
score at or below it) has no `log10`. Rather than a fixed floor picked
out of thin air, `paintEvent()` now also tracks the smallest *positive*
score in the data while it's already looping over every entry for
`maxScore`, and uses that as the floor -- so the chart always uses its
full vertical range for whatever dynamic range a given capture actually
has, instead of leaving most of a low-dynamic-range capture's bars
bunched near the top or a high-dynamic-range one's bars bunched near the
bottom. Falls back to a fixed three-decade floor below the max
(`maxScore * 1e-3`) only in the degenerate cases where that adaptive
floor doesn't make sense: every score non-positive, or every score
identical (so no distinct "smallest positive" exists below the max). A
small "log scale" label is drawn in the corner of the plot when it's
active, since the bar shapes alone don't otherwise announce which mode
is showing.

Validated with a throwaway headless harness (grabbing the real
`QualityInspectorDialog` widget, not a reimplementation) against the
synthetic Jupiter capture: computed quality, selected the best 20%,
rendered the dialog, screenshotted it, ticked the checkbox in code the
same way a user's click would, and screenshotted again. The two
renders showed visibly different bar shapes for the same underlying
data -- the log render kept the softer end of the distribution well
above zero height where the linear render had it pressed flat against
the axis -- confirming the toggle actually changes what's drawn rather
than just compiling.

## Removed the bundled synthetic Jupiter capture

The `sample_capture/synth_jupiter.avi` test asset (used throughout this
log for headless validation runs) has been removed from the source
package to keep it smaller -- it was never referenced by the build
(CMakeLists.txt), the app itself, or any doc, only ever passed as a
command-line argument to the manual/non-ctest diagnostic executables
(`manual_pipeline_run`, `export_validation`, etc.), so removing it
doesn't affect the build or the automated test suite. Anyone wanting to
re-run those diagnostics needs to point them at their own SER/AVI/FITS
capture instead.

## Color adjustments: dedicated window, live preview, color balance/hue/brightness

The "Histogram / Color" stage used to be a handful of spin boxes (black
point, white point, gamma, saturation) plus a small `CurvesWidget`
squeezed into the side panel behind a manual "Apply Color Adjustments"
button -- not much room to make a deliberate decision from a histogram,
and only those four numeric parameters plus the curve to work with. Two
changes: a real window to work in, and more to work with.

**New window.** All of it now lives in a new `ColorAdjustmentDialog`
(`src/gui/ColorAdjustmentDialog.h/.cpp`), opened via an "Adjust Color..."
button in the side panel -- same lazily-created-dialog convention as
`AlignmentInspectorDialog`/`QualityInspectorDialog` (`MainWindow` just owns
the button and a `refresh()`-calling lazy pointer). `CurvesWidget` itself
didn't need any changes -- it already supported exactly what was asked for
(a histogram backdrop, add a point by double-clicking empty space, remove
one by double-clicking it, drag to move) -- it just needed room to breathe
instead of `sizeHint()`'s cramped 320x200 inside a 400-ish-px-wide side
panel.

**Live preview instead of an Apply button.** Every control (all nine spin
boxes plus the curve) is wired to a single debounced recompute: any change
restarts a 150ms single-shot `QTimer`, and only when it actually fires does
the dialog call `PipelineController::applyColor()` -- coalescing a slider
drag or a curve-point drag into one recompute after things settle, rather
than one per mouse-move event. `applyColor()`'s own color-stretch math
(levels/curve/saturation, now plus brightness/color balance/hue) is all
simple O(pixels) loops, not FFT-based like alignment, so even an
uncoalesced recompute would likely be fine, but debouncing costs nothing
and avoids finding out the hard way on a slower machine. The dialog
listens to `PipelineController::colorDone` directly (with itself as the
connection's context object, same thread-safety reasoning as
`QualityInspectorDialog`'s screenshot-test bug writeup earlier in this
log: `colorDone` is emitted from a `QtConcurrent` worker thread, and a
context object is what makes Qt actually queue the slot onto the main
thread instead of running it inline on the worker) to update its own
preview -- independent of `MainWindow`, which listens to the exact same
signal for its own preview/status bar/CA-cascade logic. Both react to one
`applyColor()` call; neither needs to know about the other.

**New parameters**, all in `proc/ColorStretch.h/.cpp`:
- `ColorBalanceParams` (`applyColorBalance`): independent red/green/blue
  multiplicative gain -- classic color balance, clamped to [0,1], no-op on
  mono (same convention `applySaturation` already used for that case).
- `HueParams` (`applyHueRotation`): converts each pixel to HSV, rotates
  the hue angle, converts back -- saturation and value are left
  numerically alone by construction. Also a no-op on mono, since hue is
  meaningless for a single channel.
- `BrightnessParams` (`applyBrightness`): a plain additive offset on every
  sample, clamped back to [0,1] -- deliberately not exposure/multiplicative,
  so its effect is easy to reason about next to the levels stretch, which
  already handles overall exposure.

`PipelineController::applyColor()` grew three new parameters (all
default-constructing to a no-op, so the existing `manual_pipeline_run`/
`export_validation`/`test_integration` call sites -- which only ever cared
about levels/curve/saturation -- compile unchanged) and now runs, in
order: levels -> curve -> brightness -> color balance -> hue -> saturation.
Saturation stays last, matching where it already was before this change.

Unit-tested in `tests/test_proc.cpp` (`testColorBalanceHueBrightness`):
per-channel gain scales and clamps correctly and no-ops on mono; +120
degrees of hue rotation takes pure red to pure green (exact on the
standard HSV wheel) and 0 degrees is a no-op; hue is a no-op on mono;
brightness adds directly in range and clamps at both ends. Also validated
against real data with a throwaway headless harness (not part of ctest,
matching this log's usual practice): ran the real pipeline against a
synthetic Jupiter capture through to a sharpened result, opened the real
`ColorAdjustmentDialog`, and drove its actual spin boxes (not a
reimplementation) the same way a user's input would -- screenshotted the
default (identity) state, then red gain 2.0 + hue 60 degrees (visibly
turned the disk yellow-green, confirming the new math actually reaches
the rendered preview, not just that it compiles), then clicked the real
**Reset to Defaults** button and confirmed the screenshot came back
byte-identical to the original default state.

## Windows port (source-level; now confirmed build-tested on Windows)

Scoped deliberately: this sandbox is Linux-only, with no Windows machine,
no MSVC, and no prebuilt Windows Qt6/OpenCV/FFmpeg/CFITSIO/FFTW3/libtiff
packages available to it (cross-compiling all of those for MinGW from
source was considered and rejected as a multi-hour, failure-prone
undertaking with no way to test the result on real Windows anyway). So
this is a source-and-build-system port, verified as far as this
environment allows -- every change was checked against a real rebuild +
`ctest` run on Linux to confirm nothing regressed there -- plus a Windows
code path written using only well-established, standard CMake/MSVC
mechanisms, but genuinely **not** verified against a live Windows/vcpkg
build. Treat every claim below at that confidence level, not as "tested."

**The application source needed almost nothing.** A grep for the usual
POSIX portability offenders (`unistd.h`, `sys/*.h`, raw `mmap`/`fork`,
hardcoded `/proc`, `/tmp`, `/usr` paths) turned up exactly one hit outside
the build system, and it's in a non-shipped diagnostic, not the app itself
(see below). Everything else already goes through Qt's own cross-platform
file/path APIs.

**What actually changed, in `CMakeLists.txt`:**
- OpenCV and libtiff switched from `pkg_check_modules` to plain
  `find_package(OpenCV REQUIRED)` / `find_package(TIFF REQUIRED)` --
  OpenCV ships its own CMake config package on every platform it's
  installed from (apt, vcpkg, or the official Windows SDK build), and
  CMake itself ships a builtin `FindTIFF.cmake` module, so both resolve
  identically cross-platform with no OS branching needed at all. Verified
  this doesn't regress Linux: reconfigured and rebuilt from clean, `ctest`
  still 3/3. (Note the variable-name change this forced: pkg_check_modules
  gives `OPENCV_INCLUDE_DIRS`/`OPENCV_LIBRARIES`, all-caps;
  `find_package(OpenCV)` gives `OpenCV_INCLUDE_DIRS`/`OpenCV_LIBS`,
  mixed-case and LIBS not LIBRARIES -- easy to typo, confirmed the actual
  names empirically against this repo's installed OpenCV rather than
  assuming them.)
- FFmpeg (avformat/avcodec/avutil/swscale) and cfitsio/fftw3 have no CMake
  config package on any platform this project has access to, so Linux
  keeps using pkg-config exactly as before (unchanged, still what `ctest`
  exercises), gated behind `if(NOT WIN32)`. The `if(WIN32)` branch instead
  uses plain `find_path()`/`find_library()` against `CMAKE_PREFIX_PATH` --
  deliberately not a hand-written `Findcfitsio.cmake`-style module
  guessing at vcpkg's internal target names (`unofficial::cfitsio::...` or
  similar), since there was no way to confirm those names without a real
  vcpkg install to inspect. `find_path`/`find_library` is exactly the
  mechanism vcpkg's own toolchain file is designed to satisfy for any port
  without a CMake config package, so it should work regardless of the
  port's internal target naming -- but "should" is doing real work in that
  sentence until someone actually runs it.
- `-Wall -Wextra` (unconditional before) now goes through a
  `LS_WARN_FLAGS` variable that's empty under MSVC -- `cl.exe` doesn't
  understand GCC-style flags and errors out on them rather than ignoring
  them.
- `NOMINMAX` and `WIN32_LEAN_AND_MEAN` are defined project-wide under
  `if(WIN32)`. This one's a known, well-documented class of failure, not a
  guess: `<windows.h>` -- pulled in transitively by Qt or by FFmpeg/cfitsio
  headers -- `#define`s `min`/`max` as macros, which breaks essentially
  every `std::min`/`std::max`/`std::clamp` call in `ImageBuffer.h` and most
  of `proc/` unless this is set first.
- `ftys.exe` gets `WIN32_EXECUTABLE TRUE` (no console window behind the
  GUI) and a real icon via a new `assets/ftys.rc` + `assets/ftys.ico`
  (generated from the existing `assets/ftys_logo.png` with ImageMagick's
  `convert -define icon:auto-resize=256,128,64,48,32,16`, then verified
  with `identify` that all six resolutions actually landed in the .ico)
  -- both added to the `ftys` target's sources only `if(WIN32)`, so
  Linux/macOS are untouched and still get the icon the way they always
  did, via the Qt resource file.

**New `vcpkg.json` manifest** in the repo root lists `opencv4`, `ffmpeg`
(with the `avcodec`/`avformat`/`swscale` features explicitly requested),
`cfitsio`, `fftw3`, and `tiff` -- deliberately *not* Qt6 itself, since
vcpkg's own Qt port builds Qt from source (slow, and known to be fragile
across vcpkg versions); the README instead points at the official Qt
Online Installer for Qt6 and vcpkg only for the plain C/C++ libraries.
Port names weren't independently confirmed against a live `vcpkg search`
(no network access to vcpkg's registry from this sandbox) -- `opencv4` in
particular is worth double-checking, since vcpkg has renamed its OpenCV
port before.

**`tests/manual_pipeline_run.cpp`** (a non-shipped diagnostic, not part of
the app or of `ctest`) was the one actual POSIX-ism found: its per-stage
`VmRSS` memory reporting read `/proc/self/status` directly. Given a
`GetProcessMemoryInfo()`-based Windows branch (`psapi.h`, linked via a new
`if(WIN32) target_link_libraries(... psapi)` in `tests/CMakeLists.txt`)
since it's shipped in the source tree and cheap to make portable too, even
though it isn't part of the port's real scope. Confirmed the `#else`
(Linux) branch still compiles and links unchanged.

**What to check first if a real Windows build attempt fails:** in rough
order of how likely each is to be the actual problem --
1. `vcpkg install --triplet x64-windows` fails with "requires a list of
   packages... classic mode" -- **confirmed by an actual user attempt**,
   the first real Windows-side signal on this port. Cause: vcpkg only
   activates manifest mode (reading `vcpkg.json` automatically) when
   `vcpkg.json` is in the *current working directory* -- running the
   command from inside vcpkg's own cloned folder, rather than `cd`-ing
   into the FTYS project directory first, leaves vcpkg unable to find it
   and falls back to classic mode, which then correctly complains it has
   no package names on the command line. Fixed the README's step-by-step
   to `cd` into the FTYS project directory before invoking
   `<path-to-vcpkg>\vcpkg install`, rather than showing the clone/
   bootstrap/install commands as one undifferentiated block that could be
   (and was) run entirely from within the vcpkg folder.
2. A vcpkg port name or feature flag above has changed upstream (`vcpkg
   search <name>` shows the current name).
3. `find_path`/`find_library` for FFmpeg/cfitsio/fftw3 came back empty --
   almost always `CMAKE_TOOLCHAIN_FILE` wasn't passed, or was passed after
   `project()` took effect (it must be a `-D` on the `cmake` command line,
   not set inside `CMakeLists.txt`).
4. A vcpkg port's actual header/library file names differ from the
   plain, upstream-standard names guessed here (`avformat`, `cfitsio`,
   `fftw3` as the base library names) -- unlikely for these particular
   libraries since vcpkg deliberately keeps them close to upstream, but
   not something this environment could confirm directly.
5. Qt6's CMake config package not found -- `CMAKE_PREFIX_PATH` needs to
   point at the specific Qt6 kit directory (e.g.
   `C:\Qt\6.7.0\msvc2019_64` on older Qt 6, or `C:\Qt\6.11.0\msvc2022_64`
   on Qt 6.8+), not just `C:\Qt`, and the compiler suffix itself changed
   with Qt 6.8 -- see the fifth real user signal below.

**`vcpkg install` (specifically FFmpeg) is very slow -- expected, not a
sign of a stuck build.** Second real user signal on this port: a build
sitting at "Building ffmpeg for Release" then "Building ffmpeg for Debug"
for a long time is working as intended, just slowly. Two compounding
reasons, neither fixable from this project's side: FFmpeg is the one
dependency here vcpkg can't grab as a prebuilt binary, so it compiles from
source through its own `./configure`+`make` build under vcpkg's bundled
MSYS2/bash rather than a native MSVC/CMake build (slower on Windows on its
own), and it includes a large default codec/format list regardless of
which vcpkg *features* are requested (this project's `avcodec`/`avformat`/
`swscale` features only gate optional external libraries, not FFmpeg's own
already-large defaults). What *is* fixable: the plain `x64-windows`
triplet builds both Debug and Release back-to-back, roughly doubling
every compiled-from-source port's time, for a Debug configuration this
project never uses. Switched the README's recommended triplet to the
community `x64-windows-release` triplet (Release only) and added the
matching `-DVCPKG_TARGET_TRIPLET=x64-windows-release` to the CMake
configure step -- omitting that second flag while having installed under
the `-release` triplet is its own failure mode: CMake would look for
packages under `vcpkg_installed\x64-windows\` while they actually landed
under `vcpkg_installed\x64-windows-release\`, and every `find_path`/
`find_library` call from item 3 above comes back empty.

**`'cmake' is not recognized...`** Third real user signal: the README
never actually told anyone to install a C++ compiler or CMake itself
before jumping straight into the Qt/vcpkg steps -- a real gap, not
something a user did wrong. Fixed by adding an explicit first step:
install Visual Studio with the "Desktop development with C++" workload
(gives both the MSVC compiler and a bundled CMake) and, critically, run
every subsequent step from the **Developer Command Prompt for VS** rather
than a plain `cmd`/PowerShell window -- that prompt is what puts
`cmake.exe` and `cl.exe` on `PATH`; neither is there in an ordinary
terminal even with Visual Studio fully installed. Also noted the
standalone cmake.org installer as an alternative, since not everyone
building this will want the full Visual Studio IDE.

**`Could not find toolchain file: "C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake"`**
Fourth real user signal: this one wasn't a bug in the build at all -- the
user had copy-pasted the CMake configure command's example straight out
of the README, literally including the placeholder text `C:\path\to\vcpkg`,
instead of substituting their own vcpkg path. Reviewing the surrounding
text made clear why: the placeholders looked like plausible real paths
(`C:\path\to\vcpkg\...`, `C:\Qt\6.x.x\msvc2019_64`), so nothing marked them
as "replace this" rather than "this is what to type." Fixed by switching
every Windows build placeholder to unmistakable bracketed tokens
(`<PATH-TO-VCPKG>`, `<PATH-TO-QT-KIT>`, `<PATH-TO-YOUR-FTYS-PROJECT-FOLDER>`,
`<SOME-FOLDER-OF-YOUR-CHOOSING>`) plus an explicit sentence ahead of the
configure command stating outright that those two tokens are not real
paths. Reviewing the same section surfaced two further latent bugs, both
fixed in the same pass even though this particular user hadn't hit them
yet:

- Step 5 (copying DLLs next to `ftys.exe`) referenced
  `vcpkg\installed\x64-windows-release\bin` -- vcpkg's *classic-mode*
  install location, inside vcpkg's own cloned folder. But step 3 has the
  user run `vcpkg install` from the *project* directory, which is
  manifest mode (triggered by `vcpkg.json` living there), and manifest
  mode installs into `vcpkg_installed\<triplet>\` next to `vcpkg.json` --
  i.e. inside the FTYS project folder, not inside vcpkg's. Confirmed
  against this same user's own pasted build log, which showed paths like
  `C:/Users/PC/Documents/FTYS/vcpkg_installed/x64-windows/debug/lib` --
  clearly under the project root. Fixed step 5 to point at the project's
  own `vcpkg_installed\x64-windows-release\bin`.
- Step 3's example paths (`C:\Users\PC\Documents\GitHub` for vcpkg,
  `...\GitHub\FTYS` for the project) assumed a sibling-folder layout and
  used a relative `..\vcpkg\vcpkg install` invocation that depends on it.
  This user's actual FTYS folder is `C:\Users\PC\Documents\FTYS` -- not
  under `GitHub` at all, and not a sibling of vcpkg (`...\GitHub\vcpkg`)
  -- so that relative path would never have resolved for them even if
  they'd substituted correctly. Rewrote step 3 with generic placeholder
  tokens and switched to always invoking vcpkg by its full absolute path,
  since there's no reason vcpkg and this project need to be siblings.

**`Could not find a package configuration file provided by "Qt6"`** Fifth
real user signal: after fixing the path substitution above, this user hit
Qt6 not resolving even with a real, correctly-typed
`-DCMAKE_PREFIX_PATH=C:\Qt\6.11.2\msvc2019_64`. The path itself was the
bug this time, not a copy-paste mistake: that folder doesn't exist on
their machine. Confirmed via a web search of Qt's own documentation that
starting with Qt 6.8, Qt's prebuilt Windows binaries switched from an
MSVC 2019 build to an MSVC 2022 build (Qt's blog: "the upcoming Qt 6.8
will have packages for Windows built with MSVC 2022 only, and MSVC 2019
ones will be discontinued in binary packages"), and the installer's own
package id for 6.11.2 is `qt.qt6.6112.win64_msvc2022_64` -- so the kit
folder on disk is `msvc2022_64`, not `msvc2019_64`, for any Qt version
from 6.8 onward. This user is on 6.11.2, well past that cutoff. The
README's own example path (`C:\Qt\6.7.2\msvc2019_64`) was written before
this was checked and is now actively wrong advice for anyone installing a
current Qt version -- it wasn't just an unlikely edge case flagged as
"can't confirm," it was stale information from an older Qt release
presented as a plausible example. Fixed by rewriting the example to show
both cases (`msvc2019_64` on Qt releases before 6.8, `msvc2022_64` on
6.8+) with the reason for the split stated directly, and hardening the
instruction from "check what's there rather than guessing the version
number" to also cover not guessing the compiler suffix -- open
`C:\Qt\<version>\` and use whatever single folder is actually there.

**Real compiler errors, not doc/path issues -- the first actual source bugs
this port has hit.** Sixth real user signal: with the path issues above
resolved, MSVC got far enough to actually compile the codebase, and found
two genuine portability bugs that GCC/libstdc++ on Linux had been masking:

- `test_proc.cpp` (and, once grepped for the same pattern, seven other
  files: `tests/test_io.cpp`, `tests/export_validation.cpp`, and
  `src/io/SerReader.cpp`/`FitsReader.cpp`/`AviReader.cpp`/
  `FrameSourceFactory.cpp`/`ImageWriter.cpp`) use `std::string` and/or
  `std::to_string` without ever including `<string>` directly -- they
  compiled on Linux only because some other standard header they *do*
  include (`<random>`, `<fstream>`, `<algorithm>`, `<stdexcept>`, `<cstring>`,
  etc.) happens to transitively pull in `<string>` under libstdc++.
  MSVC's STL doesn't guarantee that same transitive chain -- concretely,
  its `<random>` does not drag in `<string>` -- so `std::to_string` came
  back as `error C2039: 'to_string': is not a member of 'std'`. (The
  header files for these same readers were already fine: they all go
  through `core/FrameSource.h`, which does `#include <string>` itself, so
  including *our own* headers was always going to be reliable regardless
  of platform -- it's specifically relying on some *other* library's
  transitive includes that isn't.) Fixed by adding an explicit
  `#include <string>` to all eight files rather than only the one the
  user's compiler happened to reach first -- this was a real bug in the
  actual application source (the `io` readers are compiled into `ftys.exe`
  itself, not just the test binaries), so it would have broken the real
  Windows build even after every test passed to compile.
- Separately, `wavelet_diagnostic.cpp` (a Qt-linked manual test target,
  same as the other Qt-linked targets) hit
  `error C1189: "Qt requires a C++17 compiler, and a suitable value for
  __cplusplus. On MSVC, you must pass the /Zc:__cplusplus option to the
  compiler."` even though the project already sets
  `CMAKE_CXX_STANDARD 17`. Root cause: MSVC's `/std:c++17` flag (which is
  what `CMAKE_CXX_STANDARD 17` translates to under MSVC) does not, on its
  own, make the `__cplusplus` preprocessor macro report `201703L` -- MSVC
  leaves that macro at the ancient `199711L` by default for backward
  compatibility with old code that branches on it, and only the separate
  `/Zc:__cplusplus` flag opts a translation unit into reporting the real
  value. Qt6's `qcompilerdetection.h` checks `__cplusplus` directly and
  hard-errors if it looks pre-C++17, regardless of what standard the
  compiler is actually using. GCC/Clang have no equivalent gap (their
  `__cplusplus` has always been accurate), which is why this never showed
  up building on Linux. Fixed by adding `add_compile_options(/Zc:__cplusplus)`
  under a top-level `if(MSVC)` guard in `CMakeLists.txt`, ahead of every
  target -- a no-op on Linux/macOS, and it fixes every Qt-linked target at
  once (`ftys`, `test_integration`, `wavelet_diagnostic`, `inspector_verify`,
  `export_validation`, `manual_pipeline_run`), not just the one the log
  happened to show failing.

Both fixes were verified with a full clean Linux rebuild + `ctest` (3/3
passing) before repackaging, same as every other change in this log --
that only confirms neither fix regressed the Linux build, since the
`/Zc:__cplusplus` line is a no-op outside MSVC and `<string>` was always
implicitly available on Linux; the actual MSVC compile still can't be
verified without a Windows machine.

**`On MSVC you must pass the /permissive- option to the compiler` +
cascading `QString`/template errors, on every Qt-linked file.** Seventh
real user signal: after the `/Zc:__cplusplus` fix, MSVC got past that
check and immediately hit a second, similarly worded hard requirement
from the same Qt header (`qcompilerdetection.h`): Qt6's headers detect
`cl.exe` and static-assert that `/permissive-` must be passed, and
without it every file that includes practically anything from QtCore
fails with cascading, hard-to-read template errors (`QString`: an
undefined class is not allowed as an argument to compiler intrinsic type
trait, recursive alias declaration in the `<=>`-comparison helpers,
etc.) -- all downstream noise from the same missing flag, not separate
bugs. `/permissive-` switches `cl.exe` into standards-conformant parsing
mode, which Qt6's more template-heavy headers assume; it's a distinct
flag from `/Zc:__cplusplus` (one fixes what `__cplusplus` reports, the
other fixes how the parser actually behaves) and MSVC requires both
independently for Qt6. Fixed the same way as the previous flag: added
`/permissive-` to the existing `if(MSVC) add_compile_options(...)` block
in `CMakeLists.txt` -- a no-op on GCC/Clang, which parse conformantly by
default.

Separately, and more importantly: every one of this user's error paths
pointed at `C:\Qt\6.11.2\mingw_64\include\...` -- **the MinGW-built Qt
kit**, not an MSVC-built one. This matters beyond just the compiler
flags above: this project builds vcpkg's OpenCV/FFmpeg/etc. and FTYS
itself with MSVC (`cl.exe`), and a MinGW-built Qt cannot be linked into
an MSVC-built program at all -- MinGW (GCC) and MSVC use incompatible
C++ ABIs (name mangling, exception handling, the STL's own object
layout), so even a fully successful compile of every Qt-including file
would still fail at the link step against `mingw_64`'s import libraries.
The `/permissive-` static-assert firing at all regardless of which kit's
headers were in use is what surfaced this -- the fix above makes the
compiler-detection check pass, but doesn't and can't fix the underlying
kit mismatch. This user appears to have only ever installed Qt's MinGW
component (probably because it's the one some Qt installer flows check
by default), never the MSVC one. Fixed by rewriting README step 2 to
explicitly say to check "MSVC 2022 64-bit" during the Qt Online
Installer's component selection -- not "MinGW 64-bit" -- with the ABI
mismatch explained inline so a future user understands *why* the
distinction matters rather than just being told which box to tick, plus
a note that they can add the MSVC component alongside an existing MinGW
one via the Qt Maintenance Tool without needing to remove anything.
Step 4's Qt-kit-path guidance was also tightened to say explicitly to
pick whichever folder starts with `msvc`, not `mingw_64`, if both exist
side by side.

Verified with the same clean Linux rebuild + `ctest` discipline as every
other round (3/3 passing) -- `/permissive-` is a no-op outside MSVC, so
this doesn't change anything checkable on Linux; the kit-mismatch issue
is a Windows/Qt-installer concern this sandbox has no way to reproduce
at all, so that part rests on general MSVC/MinGW ABI-incompatibility
being well-established rather than on anything testable here.

**"Where is windeployqt.exe?"** Eighth real user signal, and the first
one that isn't a build failure -- this user got all the way through
compiling and linking (the `/permissive-` + MSVC-Qt-kit fixes above
evidently worked) and reached step 5's deployment instructions, which
said to "run Qt's `windeployqt.exe`" without ever saying where it lives.
It ships inside the Qt kit itself, at `<kit>\bin\windeployqt.exe`, and
isn't on `PATH` by default. Fixed step 5 to spell out the full path using
the same `<PATH-TO-QT-KIT>` token already introduced in step 4, plus a
concrete example and the exact invocation.

**The Windows port is confirmed working end to end** -- this same user
reported `ftys.exe` actually running successfully, then asked what
libraries it needs to run. That's a real milestone worth recording
plainly: every fix logged above (the eight "real user signal" items,
covering vcpkg manifest mode, FFmpeg build time, missing prerequisites,
placeholder paths, the Qt kit folder rename, the missing `<string>`
includes, `/Zc:__cplusplus`/`/permissive-`, and the MinGW-vs-MSVC Qt kit
mismatch) added up to an actual working build on a real machine, not just
a plausible-looking one. Updated the top-of-file Status section and the
README's Windows section/FAQ to say so instead of "not build-tested,"
now that it's been verified for real rather than only reasoned through.

Ninth real user signal, and not a bug this time: "what libraries are
needed to run ftys.exe?" surfaced one real gap in step 5 that hadn't
come up yet because nobody had gotten this far before -- the
`windeployqt.exe` + `vcpkg_installed\...\bin` instructions were already
right, but the doc never mentioned the Microsoft Visual C++
Redistributable. This project links the MSVC runtime dynamically (the
CMake+MSVC default), so `ftys.exe` needs `vcruntime140.dll`/
`msvcp140.dll`/etc. from that redistributable on any machine that
doesn't already have Visual Studio or its Build Tools installed --
irrelevant on the developer's own machine (which has it via Visual
Studio already), but a real problem for anyone they hand the exe to.
Fixed by adding that to step 5, along with `windeployqt.exe`'s
`--compiler-runtime` flag, which bundles those same DLLs automatically.

## CI (GitHub Actions)

Added `.github/workflows/build.yml` after the project was published on
GitHub, so every push/PR gets built and tested on both platforms
automatically instead of relying on a real person to hit build breakage
first -- exactly the gap that made every fix in the Windows-port log
above take a live back-and-forth to find. Deliberately mirrors the
README's own documented build steps rather than inventing a different
path: Linux job installs the same apt packages listed in "Build (Linux)"
and runs a plain `cmake`/`ctest`; Windows job installs the MSVC 2022 64-bit
Qt kit (never MinGW -- see the sixth/seventh real-user-signal entries
above for why that specifically breaks), bootstraps vcpkg via
`lukka/run-vcpkg` (which also wires up vcpkg's binary caching backed by
the GitHub Actions cache -- without it, every single run would rebuild
FFmpeg from source, the "very slow, expected" issue documented above,
instead of restoring a cached copy after the first run), and configures
with the same `x64-windows-release` triplet and MSVC flags CMakeLists.txt
already sets. Both jobs package their built binary as a workflow artifact
(Windows via `windeployqt.exe --compiler-runtime` + copying the
vcpkg-built DLLs, same as README step 5; Linux as a plain binary with a
caveat file about needing matching system library versions, since there's
no AppImage/static-linking setup here). A third job attaches both
packages to a GitHub Release whenever a `v*` tag is pushed. None of this
can be exercised in this sandbox (no GitHub Actions runner available
here) -- the YAML was checked for valid syntax, and each action's current
usage was verified against its own docs rather than assumed from memory,
but the workflow itself is unverified until it actually runs on GitHub.

**First real CI run: Linux passed, Windows failed at "Install Qt."** The
Linux job worked on the first try -- good early evidence the plain
apt+cmake+ctest path this job mirrors from the README is solid in CI, not
just on this sandbox. The Windows job failed inside
`jurplel/install-qt-action`'s own internal "Setup and run aqtinstall"
step, before it ever got to installing Qt itself: `Unexpected error
attempting to determine if executable file exists
'C:\Users\runneradmin\AppData\Local\Microsoft\WindowsApps\python.EXE':
Error: EACCES: permission denied`. Root cause, confirmed against
upstream: GitHub's windows-latest runner images ship a Windows "App
Execution Alias" stub at that exact path (and the same for `python3.exe`)
that isn't a real Python -- it's a placeholder Windows Store redirect.
`@actions/toolkit` (used internally by many actions, including this
one's Python-invoking step) resolves executables on `PATH` with `stat()`,
which follows the alias's reparse point and throws `EACCES`, instead of
`lstat()`, which wouldn't; this is a known, already-acknowledged upstream
bug (`actions/toolkit#1925`, with an `lstat()` fix already up as
`actions/toolkit#1953`) that install-qt-action's own bundled toolkit
version hits when its internal aqtinstall setup tries to shell out to
`python`. Nothing about our CMake/vcpkg/Qt setup was wrong -- the failure
never got that far. Fixed by adding a step immediately before "Install
Qt" that deletes both alias stub files
(`%LOCALAPPDATA%\Microsoft\WindowsApps\python.exe`/`python3.exe`) with
`Remove-Item -ErrorAction SilentlyContinue`, so nothing ever tries to
resolve them again -- chosen over trying to filter `WindowsApps` out of
`PATH` via `$GITHUB_ENV`/`$GITHUB_PATH`, since that mechanism is
documented to behave inconsistently between bash and pwsh specifically
on Windows runners, where deleting the two known files at their known
paths is a more direct, reliable fix. Like the workflow itself, this is
grounded in the actual upstream bug reports rather than guessed, but
still unverified until the next real CI run confirms it.

**Second CI attempt: past the WindowsApps bug, new failure fetching Qt
6.11.2 itself.** The python-alias fix worked -- aqtinstall now runs (v3.3.0
on Python 3.12.10) and gets as far as actually requesting Qt. It then
failed with `WARNING: Failed to download checksum for the file
'online/qtsdkrepository/windows_x86/desktop/qt6_6112/qt6_6112/Updates.xml'`
followed by `ERROR: Failed to locate XML data for Qt version '6.11.2'`.
Root cause, confirmed against the aqtinstall project's own issue tracker:
Qt restructured its download-repository folder layout starting at 6.11 --
older layouts nested a second `qt6_XXXX` folder per compiler variant
(`qt6_6110/qt6_6110/Updates.xml`), the new one puts each compiler variant
directly (`qt6_6112/qt6_6112_msvc2022_64/Updates.xml`) -- and aqtinstall
didn't gain support for the new layout until after its current stable
PyPI release (3.3.0, June 2025); the fix ("Support Qt 6.11+ for Windows
X64", miurahr/aqtinstall#959 and #1000) exists only in an unreleased dev
build as of this writing. `pip install aqtinstall` (what
install-qt-action actually runs) has no way to get that fix yet. This has
nothing to do with this project's own code or CMake setup -- it's purely
"the Qt-fetching tool doesn't understand the new Qt version's folder
layout yet." Fixed by pinning CI to `6.10.*` instead of `6.11.2` --
deliberately *not* matching the version confirmed working on the
maintainer's own Windows machine, since there's nothing 6.11-specific
in this codebase and any recent Qt6 release builds an equivalent app;
`6.10.*` predates the repo-layout change, so it's unaffected. Worth
bumping back to a 6.11.x/6.12.x wildcard once aqtinstall ships a stable
release with the new-layout support -- there's a comment in the workflow
itself as a reminder of exactly that.

**Third CI attempt: past Qt entirely, `lukka/run-vcpkg` now fails
immediately** with `Error: A Git commit id for vcpkg's baseline was not
found nor in vcpkg.json nor in vcpkg-configuration.json`. Straightforward
this time: `vcpkg.json` never had a `builtin-baseline` field (it only
ever needed one for reproducible version resolution, which nothing in
this project's manual local workflow required), and `lukka/run-vcpkg@v11`
requires one -- it's how the action knows which exact vcpkg commit to
check out for its own bootstrap and caching, not optional the way it was
for a plain manual `vcpkg install`. Fixed by adding
`"builtin-baseline": "30ef65cad98f08e7197c9a1656fbd871bcb72f2d"` to
`vcpkg.json` (a real commit from `microsoft/vcpkg`, dated 2026-08-31,
confirmed to actually contain all five of this project's dependencies in
its `versions/baseline.json` before pinning it). This is a genuine
project-wide improvement, not a CI-only patch: it pins exactly which
port versions get resolved for anyone running `vcpkg install` from this
manifest, local or CI, rather than silently floating to whatever's newest
in each person's own local vcpkg clone -- the kind of reproducibility gap
that eventually causes "it built fine for me yesterday" reports. Worth
bumping this hash forward periodically (`git rev-parse HEAD` in a fresh
vcpkg clone) rather than letting it go stale for years.

**Fourth CI attempt: past vcpkg entirely (full dependency build succeeded,
about an hour on a cold cache -- expected, see the "very slow" entry
above), new failure in CMake's own configure step:**
`CMake Error at CMakeLists.txt:2 (project): Generator Visual Studio 17
2022 could not find any instance of Visual Studio.` Confirmed against
GitHub's own runner-images repo: `windows-latest` moved from Windows
Server 2022 (Visual Studio 2022) to Windows Server 2025 (Visual Studio
2026) in mid-2026 -- the workflow's hardcoded `-G "Visual Studio 17 2022"`
was written against the old image and stopped matching reality the
moment GitHub finished that rollout. Confirmation this is exactly a
version-name mismatch, not a missing MSVC install: vcpkg had *just*
successfully compiled OpenCV/FFmpeg/etc. with `cl.exe` moments earlier in
the very same job, so a working MSVC toolchain was unambiguously present
-- CMake's own generator-name lookup was simply asking for a VS version
that no longer exists on the image. This is also exactly the generator
this project's own real user's local machine reports
(`-- Building for: Visual Studio 18 2026`, visible all the way back in
this log's very first Windows CMake error) when *not* passing `-G` at
all -- i.e. the README's local instructions were never at risk of this,
only this workflow's hardcoded generator name was. Fixed by dropping
`-G "Visual Studio 17 2022"` entirely and letting CMake auto-detect
whichever VS is actually installed, same as the README already does --
keeping `-A x64` explicit, though, since unlike the generator name that
flag isn't tied to a specific VS version and stays correct across future
runner-image upgrades, whereas an omitted `-A` risks a silent 32-bit
configure that wouldn't link against the `x64-windows-release` vcpkg
libraries. This should make the workflow durable against the *next* VS
version bump too, rather than just patching today's mismatch.

**Fifth CI attempt: past CMake's configure step too, build succeeded on
Windows, but `ctest` failed there --** `test_io` reported `Exit code
0xc0000409***Exception` and `test_integration` reported `***Failed`,
while `test_proc` passed (33% tests passed, 2/3 failed). The exit code
looks alarming -- `0xc0000409` is `STATUS_STACK_BUFFER_OVERRUN`, which
sounds like a genuine memory-safety bug -- but reading `test_io.cpp`
and `SerReader.h`/`.cpp` end to end found every fixed-size buffer read
and write matching its declared size exactly (the 14-byte SER file-ID
field, the three 40-byte header strings, all of it). The real clue was
which tests failed: exactly the two (`test_io`, `test_integration`)
that write scratch files to a hardcoded `"/tmp/..."` path, while
`test_proc` -- which touches no files at all -- passed clean. On Linux
`/tmp` always exists; on Windows it resolves to `<current drive
root>\tmp\...` (e.g. `D:\tmp\...` in CI), a directory nothing ever
creates. `std::ofstream`/`QFile` don't throw when the parent directory
is missing -- the open just silently fails -- so the write itself
doesn't crash; it's the read-back immediately afterward that throws
(file not found). These test drivers are a single `main()` with no
`try`/`catch`, so the uncaught exception reaches `std::terminate()` ->
`abort()`, and on Windows/MSVC's UCRT that surfaces to the OS as the
generic fail-fast code `STATUS_STACK_BUFFER_OVERRUN` -- a real crash,
just not the kind its name implies. Fixed by adding a shared
`tests/TestTempDir.h` header (`ls::test::tempPath(filename)`, built on
`std::filesystem::temp_directory_path()`, which resolves correctly and
is guaranteed to already exist on every platform) and switching every
hardcoded `/tmp/...` reference over to it: `test_io.cpp` and
`test_integration.cpp` (the two tests ctest actually runs and the two
that were failing), plus three more manual/diagnostic drivers that
aren't part of ctest but had the exact same latent bug --
`export_validation.cpp`, `inspector_verify.cpp` (using `QDir::tempPath()`
instead, since those files are already Qt-heavy), and
`manual_pipeline_run.cpp`. Rebuilt clean and reran the full suite in
this sandbox afterward -- all three tests still pass on Linux
(`test_io`, `test_proc`, `test_integration`, 100%), confirming the
portable-path helper didn't regress anything here; the real test of the
Windows fix is the next CI run.

**Sixth CI attempt: the `/tmp`-path crash is gone -- `test_io` no longer
aborts, it now runs to completion and reports a real, specific
failure:** `FAIL: AVI frame 0 center pixel is bright` (every other
check, including SER and FITS round-trips, passed). `test_integration`
also failed, but by timing out at 20.15s -- suspiciously close to its
hardcoded 20000ms watchdog, not a crash of any kind.

For the AVI failure: `AviReader::readFrame()` already reads decoded
frames through `sws_scale()`, which respects `AVFrame::linesize`
properly, so the bug had to be on the write side. `writeSyntheticAvi()`
allocates its encode frame with `av_frame_get_buffer(frame, 0)` --
align `0` meaning "automatic," i.e. libavutil picks the row stride
itself based on internal heuristics (CPU SIMD capability, codec
requirements) that are a build/platform detail, not something this
fixture controls. It then filled that frame with a single flat
`memcpy(frame->data[0], buf.data(), buf.size())`, silently assuming
`frame->linesize[0] == W`. FFmpeg's own `AVFrame` docs are explicit
that this isn't guaranteed -- linesize can exceed width for alignment
padding, and callers must use the real stride, never assume packed
rows. A quick local probe confirmed why this never showed up here:
this sandbox's FFmpeg build happens to pick `linesize[0] == 32` for a
32-pixel-wide `GRAY8` frame (no padding), so the flat memcpy was
accidentally correct in this environment; evidently CI's FFmpeg build
(vcpkg, MSVC, a different CPU baseline) picks a wider stride, and every
row after the first ends up shifted -- corrupting the whole image, not
just frame 0. Frame 0 is just the only frame this test actually
checks pixel content for (frame 2's check only verifies width/height),
so it was the only one positioned to catch it. Fixed by copying
row-by-row using `frame->linesize[0]` as the destination stride
instead of one flat memcpy.

For the timeout: 20000ms was tuned against this sandbox's own Linux
run, where the full pipeline finishes in well under a second for this
tiny synthetic sequence -- it had no real margin for a slower or more
loaded CI runner, and landing at 20.15s (not instant failure, not a
hang reported by any stage) points at the watchdog itself being too
tight rather than a genuine deadlock. Widened to 60000ms. Left in
place, and worth relying on if this recurs: `test_integration.cpp`
already logs a `qInfo()` line at every pipeline stage transition
(`sequenceOpened`, `qualityDone`, `selectionChanged`, `alignDone`,
`stackDone`, `sharpenDone`, `colorDone`) -- if it times out again even
at 60s, whichever of those lines is last in the CI log pinpoints
exactly which stage never returned, straight from the log, no
guessing required.

Rebuilt clean and reran the full suite in this sandbox afterward --
`test_io`, `test_proc`, `test_integration` all still pass on Linux
(100%), confirming neither fix regressed anything here; as with every
Windows-specific fix so far, the real test is the next CI run.

**Seventh CI attempt: the AVI fix held (`test_io` passed clean), but
`test_integration` failed again -- at 20.43s, despite the timeout from
the previous entry having just been widened from 20s to 60s.** That's
the finding, not a footnote: it disproves the "just needed more
headroom" theory outright. If the 60s watchdog were what's firing,
this run would have failed at ~60s, not ~20s again. Something else is
ending the process, and `ctest --output-on-failure` showed *nothing*
for it -- not even the `qInfo() << "sequenceOpened"` line the driver
logs the moment the very first pipeline stage completes. A test that
fails partway through ordinarily still leaves whatever it managed to
log before failing; logging literally nothing points at the process
dying hard enough to lose its own output, which is exactly what
happens when a C++ exception reaches `std::terminate()` -- the same
failure class as the earlier `/tmp`-path crash, just triggered from
inside the pipeline's own asynchronously-delivered slots this time
rather than test-fixture file I/O. Qt does not catch exceptions thrown
from inside a slot; one thrown on a QtConcurrent worker thread and
redelivered via a queued connection, or thrown directly in a lambda
connected to a signal, propagates straight up through Qt's event
dispatch and out of `QCoreApplication::exec()` -- taking down the
process before any of the surrounding `qInfo()` calls' output is
guaranteed to have reached the log.

Rather than guess further at which specific stage throws (there's
still no log evidence pointing at one), added a shared
`tests/TestLogging.h` with two pieces of hardening applied to every
Qt-event-loop-driven driver in this directory
(`test_integration.cpp`, and proactively to `export_validation.cpp`,
`inspector_verify.cpp`, `manual_pipeline_run.cpp`, which all share the
identical pattern): `installFlushingMessageHandler()` installs a Qt
message handler that writes and immediately flushes every
`qDebug`/`qInfo`/`qWarning`/`qCritical` line, so a later hard crash can
no longer erase the log leading up to it; `runEventLoop()` wraps
`app.exec()` in `try`/`catch`, so an exception that reaches the event
loop produces one clear diagnostic line naming itself instead of
silently vanishing into an opaque non-zero exit. This doesn't fix
whatever the underlying exception is -- there isn't enough information
yet to know what that is -- but it guarantees the *next* CI run either
passes or hands back the exact exception message and the last stage
reached, instead of another silent ~20s failure.

Rebuilt clean and reran the full suite in this sandbox afterward --
`test_io`, `test_proc`, `test_integration` all still pass on Linux
(100%); this sandbox was never able to reproduce the crash itself
(same real-user-verification gap as always -- Windows behavior can
only be confirmed on Windows), so this is a logging fix aimed
squarely at making the *next* real failure legible, not a claimed fix
for the crash itself.

**Eighth CI attempt: `test_integration` passed. The mundane explanation
turned out to be the right one --** the next CI run (still without
`tests/test_integration.cpp` or `tests/inspector_verify.cpp` actually
pushed, a mismatch only caught by diffing the real GitHub commit
against what had been delivered) reproduced the exact same ~20s,
zero-output failure as every round before it, which made it possible
to be sure of something that had only been suspected until then: those
two files had silently never been pushed since the very first
`ls::test::tempPath()` fix several rounds back, for the `/tmp`-path
crash. Every "new" failure investigated in between -- the AVI stride
bug (real, and fixed, but that was `test_io`, not
`test_integration`), the 20s-timeout theory, the uncaught-exception
theory that motivated `TestLogging.h` -- was chasing `test_integration`
behavior that may simply have been the *original*, already-diagnosed
`/tmp` bug the whole time, still present because its fix had never
actually reached this file. Once both files were actually added and
pushed (bundling the original `tempPath()` fix together with the
`TestLogging.h` hardening, since both had accumulated in the same
local copy), `test_integration` passed clean at 0.15s. Which of the two
changes actually mattered can't be fully separated out after the fact
since they landed in the same push -- but given the timeline, the
`tempPath()` fix (never before actually deployed for this file) is the
far more likely explanation than the exception-safety wrapper fixing a
bug that may never have needed it. `TestLogging.h` stays in either
way: it's a reasonable hardening on its own regardless of whether it
was the thing that mattered here.

The practical lesson, worth remembering for its own sake: when a fix
doesn't take effect, check that the file it's in was actually pushed
before diagnosing a new theory for the same failure.

## First real user signal: PAL8 AVI captures loading fully dark

With CI green and a real Mac mini in play for the macOS port, the
project's first real usage report came in: some AVI movies (not the
ones used to build/test so far) loaded as a completely dark, blank
movie. `ffprobe` on the actual affected files showed `pix_fmt=pal8`
across the board -- 8-bit palette/indexed video, where each pixel byte
is an index into a 256-color lookup table rather than a direct
gray/RGB sample, a format some older or simpler capture tools still
use. Neither of the two synthetic AVI fixtures used everywhere else in
this project's tests exercise PAL8 at all (both use plain Mono8), so
this whole code path had never actually been tested against real data.

Rather than patch on a guess, this was reproduced for real in this
sandbox: generated an actual PAL8 AVI with `ffmpeg` (a genuine 0-255
gray gradient, `-pix_fmt pal8 -vcodec rawvideo -f avi`), then read it
back through the project's real `AviReader` class via a small
standalone driver. Result: every pixel read back as 0 -- the exact
"fully dark" bug, reproduced outside the app entirely, confirming this
wasn't specific to whatever recorded the user's files.

Diagnosis, verified step by step rather than assumed: FFmpeg exposes a
PAL8 AVI's color table two different ways -- as `AV_PKT_DATA_PALETTE`
side data attached to a packet, and as static, position-independent
stream metadata (`AVCodecParameters`/`AVCodecContext::extradata`,
populated once by `avformat_find_stream_info()`). Probing the actual
decode confirmed the packet-side-data copy is fragile: it's only
attached to a packet on a genuine cold read of the stream from the
start. `AviReader::scanFrames()` already performs exactly such a cold
read once, up front, to record every frame's pts -- consuming that
one-time palette delivery before it's ever put to use. `readFrame(0)`
then does its first (necessary) backward seek and
`avcodec_flush_buffers()`, and a direct test confirmed the re-read
packet after that seek has *no* palette side data at all, and the
decoded frame's palette plane comes back all-zero -- meaning every
index resolves to black, regardless of its actual value. Confirmed
`sws_scale` itself was never the problem (a separate isolated test
showed it correctly resolves an explicitly-supplied palette for either
a GRAY8 or RGB24 destination) -- the palette data reaching it was
simply empty by the time any real frame gets decoded.

Fixed by not depending on that fragile, seek-sensitive delivery
mechanism at all: `AviReader` now copies the palette out of
`codecCtx_->extradata` once at open time (confirmed via the same
real PAL8 file to hold the identical color table, independent of
packet/seek position) and force-attaches its own stored copy to
`frame->data[1]` on every decoded PAL8 frame, right before the
`sws_scale` conversion -- regardless of whatever the decoder itself
did or didn't cache. Verified against the real ffmpeg-generated PAL8
gradient file afterward: the reader now reproduces the actual gradient
instead of all zeros.

Added a permanent regression test (`testAviPal8()` in `test_io.cpp`)
rather than relying on this having been checked by hand once: it
writes a real PAL8 AVI with a deliberately *inverted* palette (the
fixture's usual "background" index byte maps to a bright color, its
"disk" index byte maps to a dark color -- the opposite of what the raw
index bytes alone would suggest), so the test can only pass if the
palette is genuinely being resolved. Confirmed this actually catches
the bug, not just coincidentally passing either way: temporarily
disabling the fix and rebuilding made the new test fail exactly as
expected, then re-enabled it and reran clean. Full suite passes in
this sandbox (`test_io`, `test_proc`, `test_integration`, 100%).

## Next steps

1. Do a real macOS port (this environment has no macOS machine, so it'll
   need the same kind of source-level-first, then real-user-verified
   loop that got the Windows port working).
2. Watch the new CI workflow's first real run on GitHub and fix whatever
   it gets wrong -- same real-user-verification discipline as the Windows
   port itself, just aimed at a workflow file instead of a build.
