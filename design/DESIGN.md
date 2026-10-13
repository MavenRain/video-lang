# video-lang domain design

> Paste the domain design below the line, verbatim. Do not edit it.

This file is the only source of the core types and the core operations of
video-lang. `SPEC.md` sections 5 and 6 cite its sections. Do not add a core
type or a core operation that this file does not give.

When the design changes, paste the new version. Then record the change as an
open item in `SPEC.md` section 9.

Note (ruling M3-R4): the gate refuses dashes in every file, thus ` - ` replaces the 4 em dashes of the paste (lines 30 to 33). The paste is verbatim otherwise.

---

# video-lang denotational design (USER, verbatim, 2026-10-10; Concatenation 2026-10-12)

Source of truth for the domain types and operations. The language gets the lang-template formers plus ONLY these types and operations (USER: "based on lang-template and additionally only the types and operations from the following denotational design").

USER clarification (verbatim): "Further clarifying, the Video type will be the actual in-memory representation of VODs/mp4/etc, and the Position total order will be calculated from the time order in which the Frames appear in the source media. The host language shall be TinyCC and the output should be the same as the equivalent FFmpeg output given the Video objects and the resulting operations on them."

---

Yes. The continuous-time model can be replaced by a discrete, order-theoretic one without losing the denotational character.

### Revised denotational model

**Basic types**

- `Frame`  -  an image (and, if desired, an accompanying audio sample or packet). Its internal structure remains a spatial function or raster; the change is only in how frames are sequenced.
- `Position`  -  an element of a totally ordered set (typically a finite initial segment of the naturals, or an abstract discrete order). Positions are the only "locations" in a video.
- `Interval`  -  a convex subset of positions, equivalently a pair of positions `(lo, hi)` with `lo ≤ hi`, representing the closed span between them. The interval is identified with the set of frames that lie between its endpoints.
- `Video`  -  a totally ordered sequence of frames, denotationally
  ```
  Video ≜ Position ⇀ Frame
  ```
  (a partial function whose domain is a finite interval of positions). Equivalently, one may think of a video as a finite list together with the order induced by list indices; the two presentations are isomorphic for finite media.

The meaning of a concrete media container is exactly such a function (or list). There is no ambient continuous time; the only ordering is the order of frames themselves.

### Operations and their denotations

All operations remain pure functions. Their meanings are expressed by equational clauses on the sequence model.

- **Temporal restriction (trim)**
  ```
  trim : Interval → Video → Video
  μ(trim (lo, hi) v)(p) =
    if lo ≤ p ≤ hi then μ(v)(p) else undefined
  ```
  The result is the sub-sequence lying strictly between the two endpoint positions (or the frames that bound them). Nested trims compose by interval intersection:
  ```
  trim i ∘ trim j = trim (i ∩ j)
  ```
  (when the intersection is non-empty; otherwise the empty video).

- **Spatial scale** (and crop, pad, etc.)
  ```
  scale : Resolution → Video → Video
  μ(scale r v)(p) = scaleFrame r (μ(v)(p))
  ```
  The operation acts pointwise on each frame of the sequence. Functoriality is immediate:
  ```
  scale r₂ ∘ scale r₁ = scale (r₂ ∘ r₁)
  ```
  and `scale` commutes with `trim` because both are defined by restriction and pointwise application.

- **Text / subtitle burn-in**
  A subtitle track is now a function from positions (or from intervals of positions) to optional text:
  ```
  SubtitleTrack ≜ Position → Maybe Text
  ```
  (or, more economically, a finite set of interval-annotated texts). Then
  ```
  burnText : Style → SubtitleTrack → Video → Video
  μ(burnText style track v)(p) =
    overlayText style (track p) (μ(v)(p))
  ```
  Donation alerts and watermarks are the same operation restricted to a sub-interval or to a constant overlay. Because the overlay is pointwise, `burnText` also commutes with `trim` and with spatial filters that do not depend on the text.

- **Concatenation**
  ```
  concat : Video → Video → Video
  μ(concat a b)(p) =
    if p < |a| then a[p] else b[p - |a|]
  ```
  where |v| is the length of v (the size of its domain) and v[k] is the
  frame of rank k in v, that is μ(v)(min(dom v) + k). The result is the
  sequence a followed by b; its positions are the ranks 0, 1, ..., |a| + |b| - 1.
  The audio follows the frames: the audio of a, then the audio of b.

  In the list presentation, concatenation is associative, every frame map
  that does not read the position distributes over it, and restriction
  splits at the seam, each side in the positions of its own operand:
  ```
  concat (concat a b) c = concat a (concat b c)
  scale r (concat a b) = concat (scale r a) (scale r b)
  trim (lo, hi) (concat a b) =
    concat (trim (lo + a₀, hi + a₀) a) (trim (lo - |a| + b₀, hi - |a| + b₀) b)
  ```
  (the same for crop and pad; a₀ = min(dom a), b₀ = min(dom b); a side
  whose interval misses its operand is the empty video). burnText does
  not distribute: its track is indexed by the positions of the result.

- **Sequential composition**
  Filters still form a monoid under ordinary function composition:
  ```
  (•) : Filter → Filter → Filter
  μ(f • g) = μ(f) ∘ μ(g)
  ```
  Any pipeline built from the operations above therefore possesses a unique denotation obtained by composing the individual sequence transformers.

- **Encode**
  Remains the sole impure step; its denotation on the abstract video is the identity.

### Equational laws (by denotation)

- Sub-sequence extraction is associative and idempotent: `trim i ∘ trim i = trim i`, and `trim i ∘ trim j = trim (i ∩ j)`.
- Spatial operations are natural with respect to the order: they commute with restriction to any interval.
- The monoid of filters acts faithfully on the set of videos; different syntactic pipelines that denote the same function on sequences are interchangeable.

### Relationship to the earlier model

The discrete presentation is the specialization of the continuous model obtained by restricting the domain of `Time → Frame` to a discrete totally ordered subset and identifying intervals with the spans between those discrete points. All of the algebraic structure (pointwise spatial maps, monoid of filters, commutation of trim with pure frame transformers) survives unchanged. The only conceptual shift is that "time" is no longer an independent continuous parameter; it is recovered, if desired, solely as the order type of the frame sequence.

This formulation stays faithful to Elliott's denotational style: the meaning of each type and operation is a mathematical object (here an ordered family of frames and functions on such families), laws hold by equational reasoning on those objects, and any concrete realization (FFmpeg filter graph, streaming pipeline, etc.) is merely a witness that the denotation is computable.
