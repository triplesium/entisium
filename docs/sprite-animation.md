# Sprite animation assets

The native Editor agent and standalone Agent CLI expose `sprite_animation`.
The default V1 workflow is one-call image-to-video generation with local soft-edge
matting and sprite export. The TypeScript implementation lives in DevKit.
The legacy image-row workflow follows the staged approach
of [sprite-gen](https://github.com/aldegad/sprite-gen): preserve the reference,
generate one action row at a time, process locally, and export explicit frame
rectangles. It does not require the upstream Python CLI or copy its code.

## V1: one-call video workflow

Create/import a full-body character PNG on a flat `#00ff00` background, with clear
margins and no green materials on the character. V1 targets smooth cartoon loops,
not pixel-art. Configure the `fal` API credential (or `FAL_KEY`) and install
`ffmpeg` and `ffprobe` on the host PATH. Missing tools fail before paid submission.

Call `sprite_animation` once:

```json
{
  "operation": "create",
  "run": "assets/generated/hero-walk-v1",
  "characterImage": "assets/hero.png",
  "action": "Walk in place facing right, alternating foot contacts and opposite arm swings",
  "name": "walk"
}
```

The service submits one `minimax/h3-max-turbo/image-to-video` request (5 seconds,
480P, balanced prompt expansion, safety enabled), then downloads and processes it.
V1 supports this endpoint only. Optional `seed` defaults to 20260911, `cellSize`
to 256, `frameCount` to 16 and `align` to true. Pricing is provider-controlled;
there is no hard-coded price or automatic paid retry.

`waitMs` controls queue polling (default 20000, max 30000); network and local
processing have separate timeouts. If still queued, the result contains
`status: "pending"` and `jobId` equal to `run`. Continue with:

```json
{"operation":"status","run":"assets/generated/hero-walk-v1"}
```

Status resumes the same provider request and completes local export. No background
worker keeps running after a pending response; the Agent calls status to advance
it. Job records survive host restarts. Repeating create with identical inputs
resumes the job; conflicting inputs are rejected. An exclusive reservation is
written before submission, preventing duplicate paid requests across hosts.
`uncertain` means acknowledgement was not saved: inspect provider history rather
than automatically creating a new run. `failed` reports provider rejection or a
local processing error; local failures preserve the video for reprocessing.

Local processing decodes source timestamps at native resolution, recovers soft
alpha and edge color against green, aligns upper bodies horizontally, searches
0.5–2.5-second repeating poses, samples the cycle, and performs one Lanczos resize.
It exports immutable `exports/<id>/` revisions with `atlas.png`, `frames/*.png`,
`animation.json`, `quality.json` and a pausable checkerboard HTML preview. Use the
per-frame `durationMs`, not the average `fps`, for playback. Motion and anatomy
still require visual review; no detected cycle is a failure, not a fabricated loop.

The run stores `video-job.json`, `video-reference.png`, `video-attempt.json`,
`video-queue.json`, `video.mp4`, `video-result.json` (expanded prompt) and
`video-completed.json`. No credentials or base64 inputs are stored. Completed
status returns the same export. To change output settings without generation:

```json
{"operation":"reprocess","run":"assets/generated/hero-walk-v1","name":"walk","cellSize":256,"frameCount":16}
```

To import an existing video without credentials or paid calls, additionally set
`video` to a project-local `assets/...mp4`; `run` may be new. Reprocess always makes
a fresh export. Imported videos must have 8–360 frames, duration up to 15 seconds,
at most 2048 pixels per side, 64 megapixels across all frames, and valid increasing
timestamps. Input MP4 size is limited to 100 MiB. The same green-screen conditions
apply. Temporary decoded files are removed after processing.

Both CLI and Editor Agent use this contract. The Editor HTTP endpoint remains
authenticated `POST /api/v1/sprite-animation`; it returns the same status and paths.

## Legacy image-row workflow

First create a character PNG using `generate_image`, or import an existing PNG
inside the project's `assets/`. Then prepare a new run:

```json
{
  "operation": "prepare",
  "run": "assets/generated/hero-v1",
  "request": {
    "reference": "assets/hero.png",
    "description": "Small red knight, fixed side view facing right",
    "cellSize": 128,
    "chroma": "#00ff00",
    "states": [{
      "name": "walk",
      "action": "Walk in place facing right",
      "poses": ["Left foot contact", "Passing pose", "Right foot contact", "Passing pose"],
      "fps": 8,
      "loop": true
    }]
  }
}
```

Prepare is local. It copies the character into `base.png`, writes layout guides,
per-action prompts, and finally `request.json`. Names are unique ignoring case.
Choose a new run directory; existing assets are never replaced.

Generate each state with an explicit invocation:

```json
{"operation":"generate","run":"assets/generated/hero-v1","state":"walk"}
```

Generation uses the configured image provider and sends the preserved
character and layout guide. Optional `states[].motionReference` names a project
PNG containing multiple motion phases (for walking, include both left-forward
and right-forward contacts). Prepare validates and snapshots it into
`motion/<state>.png`; generation sends that preserved image as reference 3.
The prompt assigns reference 1 identity/facing, reference 2 layout, and reference
3 motion only. A motion reference is guidance, not skeleton control or a gait guarantee. The default output has one horizontal row; dimensions
are multiples of 16 and the number of frames. Optional `options` overrides the
image dimensions/quality. The selected model must support the requested size and
reference images. Background is forced opaque for subsequent chroma extraction.

An attempt record is created before contacting the paid API. Failed or cancelled
attempts are never automatically repeated, including after restarting the host.
Use a new run for another take after deciding to retry. Already generated rows
remain available if a later state fails. Preparing a new run makes no API calls.

Compose once all states have an image:

```json
{
  "operation": "compose",
  "run": "assets/generated/hero-v1",
  "tolerance": 40,
  "despill": true,
  "align": true,
  "selections": [{"state":"walk","order":[0,1,2,3],"fps":8,"loop":true}]
}
```

`order` uses zero-based source indices and can omit, reorder or repeat frames.
`selections[].source` optionally imports an existing PNG row instead of the
generated raw image. This supports paid-API-free testing and manual repair.
Compose can be repeated with different selections and colour tolerance. Each
compose creates a new immutable `exports/<id>/` revision containing:

- `atlas.png`: transparent runtime texture.
- `animation.json`: image dimensions, top-left pixel rectangles, durations,
  loop flags, normalized pivot, original frame indices and processing details.
- `frames/<state>/<index>.png`: selected frames in playback order.
- `before-atlas.png`: background removal only, at the same scale as the result.
- `quality.json`: before/after metrics and heuristic warnings, in selected playback order.
- `preview.html`: synchronized before/after playback; keep both atlases beside it.

The manifest is written last, after its assets. An interrupted export without a
manifest is incomplete and must not be consumed. Original rows and prior exports
are preserved. Frame selection is applied to both the atlas and individual PNGs.

## Processing and limits

The extractor divides each source into equal-width cells and removes the
key-coloured region connected to the edges. Empty cells and content touching
cell edges fail composition. `despill` and `align` default to true; each can be
disabled independently when composing the same raw image.

- Despill estimates excess key chroma near transparency (within four source
  pixels), separates foreground RGB from key colour, and retains partial alpha.
  Fully interior tinted clusters covering at most 0.5% of the subject, with a
  strongly tinted pixel, are corrected without reducing alpha. Larger interior
  coloured regions survive. This is a heuristic for saturated keys, not a general
  matting solver: neutral keys receive no despill, and intentional key-like edge
  colours can be changed. Pick a background absent from the character; red outfits
  usually suit green better than magenta. There is no automatic key selection yet.
- Registration matches upper-body alpha masks against the median-position frame,
  with a bounded horizontal search and small vertical matching slack. Only a
  horizontal offset is applied; weak or search-boundary matches remain unshifted
  and are flagged. Disable alignment for intentional sideways motion, rotations,
  or characters without a stable upper body. Raw-pixel `dx`, `dy: 0`, overlap
  scores, reference frame and estimated output-pixel offsets are recorded per
  source frame in `animation.json`.
- Both comparison variants share a union crop and scale. Nearest-neighbour resize
  retains hard pixel edges and preserves vertical movement. The comparison's
  "before" is background removal only, not the opaque raw model output.

Quality reports include upper-body horizontal drift, visible area, bounds,
possible key-colour residue, 64-bit dHash, premultiplied image differences,
near-duplicate neighbours and loop-seam difference. Thresholds are emitted in the
report: low mean motion <0.06; near-duplicate difference <0.035, or dHash distance
<=6 plus difference <0.12; horizontal drift >max(2px, 3% of cell width); loop seam
>2.5 times the interior mean. These conservative heuristics can flag legitimate
holds and miss incorrect limb alternation. `requiresVisualReview` is always true;
`no-heuristic-flags` is not an animation quality pass. Missing or repeated leg
phases require better motion references or new generation, not post-processing.

This version does not detect arbitrary irregular sprite layouts, repair anatomy,
remove shadows, snap AI artwork to a pixel grid, or rebuild shared palettes.
Different actions are fitted independently and may need source scale adjustment
for consistent playback across actions. There is no dedicated Editor timeline;
selections are tool parameters and preview is a standalone HTML artifact.

References are project PNGs up to 10 MiB each, maximum four in `generate_image`.
Source decoding and final atlases are limited to 16 megapixels. States support
2–16 poses and 1–60 FPS. Curation accepts up to 32 selected frames per state.

## Image provider references

- OpenAI: multipart `/images/edits`, preserving the configured model.
  [API reference](https://developers.openai.com/api/reference/resources/images/methods/edit)
- OpenRouter: `/images` with `input_references` data URLs.
  [Image generation](https://openrouter.ai/docs/guides/overview/multimodal/image-generation)
- fal.ai: the existing Sunburst text-to-image adapter uses the matching `/edit`
  endpoint with `image_urls`. Other fal image adapters are not implied.
  [Edit schema](https://fal.ai/models/openai/gpt-image-2.5/sunburst/edit/api)

Known metadata that excludes image input is rejected. Unknown metadata does not
prove support; an unsupported provider request still fails without retrying.

## Runtime consumption

This change exports animation data; it does not introduce a native animation
component or loader. A consumer loads the atlas, advances through each named
animation using `durationMs`, and converts the explicit pixel rectangle into the
texture-coordinate convention of its renderer. Use the exported `origin` and
`pivot`, and inspect orientation in the target renderer. Do not infer frame
counts or spacing from the atlas dimensions.

## Validation

`npm run typecheck` and `npm test` cover reference request payloads, path
validation, cancellation, paid-attempt protection, background extraction,
preserved motion, immutable exports, selections and the authenticated Editor
route. Refinement tests cover known horizontal offsets with vertical motion,
edge colour recovery, preservation of red/large interior materials, trapped spill,
static-motion flags, comparison switches and immutable motion references. Provider responses are mocked; these checks make no paid image calls.


## Local knight regression

The existing six-frame knight raw was recomposed locally, without model calls.
Measured on the exported 128px cells with the same crop and scale:

| Metric | Background removal only | Despill + registration |
| --- | ---: | ---: |
| Upper-body horizontal drift | 7.46px | 0.41px |
| Possible key-tinted pixels, six frames total | 763 | 18 |

The result still does not have correct alternating foot contacts. The heuristic
report flags near-duplicate neighbouring frames at the loop seam; this is a
review cue, not semantic detection of the bad gait. The new motion-reference
payload is covered by mocked provider tests, not a new paid-generation experiment.
Local artifacts and the API-disabled replay script live under
`build/sprite-realtest-20260911/` and are not source assets to commit.
