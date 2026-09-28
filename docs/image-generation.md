# Agent image generation

The native Editor Agent and CLI expose `generate_image`. DevKit implements the
OpenAI Images API service and project asset storage; Agent supplies the tool adapter.
The browser sends authenticated requests to the Editor host. It never receives API keys.

## Configuration

Set `providers.openai.type: openai` and `providers.openai.apiKey` in `config.yaml`.
Every configured provider must declare its `type`. Configure the selected
provider and model under `imageGeneration`; see [configuration.md](configuration.md)
and [config.example.yaml](../config.example.yaml). `OPENAI_API_KEY` takes precedence
for the `openai` image provider. ChatGPT OAuth credentials are not used by this service.

The image model defaults to `gpt-image-2`, independently of the conversation model.
`ETS_IMAGE_MODEL` overrides YAML; `ENTISIUM_IMAGE_MODEL` remains a compatibility alias.
Restart the host after changing configuration. There is no image settings dialog yet.

## Usage

Ask the Agent: “Generate a game inventory potion icon and save it to
assets/generated/potion.png.” The tool accepts:

- `prompt`: image description.
- `path`: a new PNG asset path. Path segments use letters, numbers, underscores or hyphens.
- `resolution`: `512`, `1K`, `2K`, or `4K`.
- `aspect_ratio`: `auto` or a supported ratio such as `1:1`, `16:9`, `9:16`, `3:2`.
- `size`: optional tier (`2K`) or explicit pixels (`2048x2048`); prefer resolution and aspect_ratio.
- `quality`: `auto`, `low`, `medium`, `high`, `xhigh`, or `max`; model support varies.
- `background`: `auto`, `transparent`, or `opaque`.
- `seed`: optional integer from 0 through 2147483647 (OpenRouter only).

Results include the path, actual PNG dimensions, byte count and model. Open the
saved asset in the Editor asset browser to preview it. CLI also emits an `artifact`
JSON event with the absolute file path. Image bytes are not stored in conversation history.

Generation is pinned to the project open at request start and never overwrites an
existing asset. Requests time out after `imageGeneration.timeoutMs` (ten minutes by default). OpenAI and OpenRouter requests are not automatically retried; fal queue requests disable automatic retries as well.
Cancelling aborts the HTTP request and prevents saving if observed before writing;
it cannot guarantee cancellation or refund of work already accepted by OpenAI.
Reference-image editing and the browser demo are not supported in this first version.

API contract: [OpenAI image generation documentation](https://developers.openai.com/api/docs/guides/image-generation).

## OpenRouter

OpenRouter uses its dedicated Image API rather than the OpenAI Images endpoint.
Set the provider type to `openrouter`; it resolves both chat and image defaults.
The image selection does not need a separate protocol or model catalogue.

```yaml
providers:
  openrouter:
    type: openrouter
    apiKey: your-openrouter-api-key

imageGeneration:
  model: { provider: openrouter, id: openai/gpt-image-2 }
  defaults:
    resolution: 1K
    aspect_ratio: "1:1"
    quality: low
```

This posts to `<baseUrl>/images`, passing unified generation options with `model`, `prompt`, `n: 1` and `output_format: png`. The result is decoded from
`data[0].b64_json`; `media_type`, when present, must be `image/png`. The saved asset
reports the actual returned dimensions. Models must support PNG image generation.
Reference images, streaming, multiple images, other file formats and provider routing options are not implemented.
No automatic fallback to a different protocol or retry is performed.

See the [OpenRouter image generation documentation](https://openrouter.ai/docs/guides/overview/multimodal/image-generation).

## fal.ai GPT Image 2.5

Use the dedicated fal.ai queue protocol for GPT Image 2.5 Sunburst:

```yaml
providers:
  fal:
    type: fal
    apiKey: your-fal-key

imageGeneration:
  model: { provider: fal, id: openai/gpt-image-2.5/sunburst/text-to-image }
  defaults:
    resolution: 1K
    aspect_ratio: "1:1"
    quality: high
```

`FAL_KEY` takes precedence over the configured key. The host submits through the
official fal.ai queue, requests a single synchronous PNG data URI, validates the
returned bytes and saves them through the same protected project-asset path. It
does not expose the key or generated image URL to the browser. A custom `providers.<id>.images.baseUrl`
is rejected for this protocol.

The GPT Image 2.5 model adapter maps common 1K ratios to fal.ai presets and maps 2K, 4K or explicit
pixel sizes to `image_size`. Explicit sizes follow GPT Image 2.5's documented
multiples-of-16, pixel-count, edge and 3:1 ratio limits. `quality`, including
`xhigh` and `max`, and `background` pass through. This text-to-image endpoint does
not accept `seed`, so the tool rejects it before submitting billable work. Other fal
model IDs require their own adapters; these constraints are not platform-wide.

See the [fal.ai GPT Image 2.5 Sunburst API documentation](https://fal.ai/models/openai/gpt-image-2.5/sunburst/text-to-image/api).

## Unified options and backend conversion

Tool arguments and `imageGeneration.defaults` share the same option names. With no
dimensions specified the effective defaults are `resolution: 1K`, `aspect_ratio: "1:1"`
and `quality: auto`. Old YAML pixel `size` values remain supported.

If a call supplies any dimension option, its dimension group replaces the YAML
`size`, `resolution` and `aspect_ratio` group. Other defaults remain in effect.
Tier `size` normalizes to `resolution`; contradictory dimensions are rejected.
Explicit pixel size cannot be combined with a resolution tier, and a supplied
aspect ratio must match those pixels. The backend determines actual output dimensions.

OpenRouter receives the normalized fields directly. Model-specific capabilities
still apply. OpenAI maps `1K` with `1:1`, `3:2`, or `2:3` to its standard pixel
sizes, and `aspect_ratio: auto` to `size: auto`. Other tier/ratio combinations and
`seed` are rejected before any request. Explicit pixel `size` is passed to OpenAI;
use dimensions supported by the configured model. Background and quality pass through.
