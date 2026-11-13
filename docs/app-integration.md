# Integrating ChimpFlow into an application

This guide is for applications that call ChimpFlow directly. It covers the
v0.3.4 model-aware TTS API and the migration needed by older PocketTTS-era
clients. AI agents can also use the served [`/skill/`](../README.md#api) guide;
this document is the application developer reference.

## What changed in v0.3.4

- `GET /v1/models` discovers the speech models configured on the host.
- `GET /v1/voices?model=<model-id>` discovers voices for one selected model.
- A speech request with no `model`, or an empty `model`, now uses **`kokoro-82m`**.
- Explicit model IDs are honored; ChimpFlow does not redirect a request that
  explicitly names `pocket-tts`.
- Voice IDs are model-specific. An unknown voice falls back to the selected
  model's configured default instead of failing the request.
- Successful speech responses identify the effective model and any voice
  fallback through response headers.
- WAV and MP3 responses, authentication, long-form synthesis, and STT retain
  their existing interfaces.

## Base URL and authentication

Use the gateway, not the audio.cpp or whisper.cpp engine ports:

```text
http://127.0.0.1:8090
```

If your application appends `/v1/...` itself, its configured base URL must **not**
already end in `/v1`; otherwise it will request `/v1/v1/...` and receive 404.

Send a per-application API key in every authenticated request:

```http
Authorization: Bearer omn_sk_...
```

Use a key with `tts` scope for speech, `stt` scope for transcription, or both
for a conversational app. Do not embed the bootstrap/admin key in an
application, source repository, browser bundle, or log. Remote/containerized
apps should use their existing restricted SSH tunnel rather than exposing the
unauthenticated engine ports.

## 1. Discover the host's models

Model availability is host-specific. Query it instead of hard-coding an
inventory:

```bash
curl -fsS \
  -H "Authorization: Bearer $CHIMPFLOW_KEY" \
  "$CHIMPFLOW_URL/v1/models"
```

The response has a `data` array. Use each entry's `id` as the speech request's
`model` value.

Typical choices:

| Model | Intended use | Deployment consideration |
|---|---|---|
| `kokoro-82m` | Default for everyday application speech, agents, and phone-sized deployments | Approximately 212 MB in BF16; much lighter than Chatterbox |
| `chatterbox` | High-end narration, video voiceovers, and other quality-critical work | Only when listed by the host. Q8 is approximately 2.09 GB and requires substantially more RAM/VRAM and compute; uses reference-WAV voice presets |
| `pocket-tts` | Legacy compatibility only | Not recommended for new work because of intermittent garbled output |

Model file size is not total runtime memory usage. Do not make Chatterbox the
default on memory-constrained devices.

## 2. Discover voices for the selected model

Always scope voice discovery to the model you will actually use:

```bash
curl -fsS \
  -H "Authorization: Bearer $CHIMPFLOW_KEY" \
  "$CHIMPFLOW_URL/v1/voices?model=kokoro-82m"
```

The response has a `data` array of voice objects. Submit the object's `id` as
`voice`. For example, Kokoro includes `af_heart`; that ID is not necessarily
valid for another model.

The legacy unscoped `GET /v1/voices` endpoint remains available, but it lists
WAV-directory basenames rather than the selected model's catalog. Existing
legacy clients can continue using it; migrated applications should use the
model-scoped form.

## 3. Synthesize speech

`POST /v1/audio/speech` accepts JSON and returns the complete audio response.
MP3 is recommended for new integrations; WAV remains supported for existing
clients.

```bash
curl -fsS \
  -H "Authorization: Bearer $CHIMPFLOW_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "kokoro-82m",
    "input": "Hello from my application.",
    "voice": "af_heart",
    "response_format": "mp3"
  }' \
  "$CHIMPFLOW_URL/v1/audio/speech" \
  -o speech.mp3
```

Core request fields:

| Field | Required | Behavior |
|---|---|---|
| `input` | yes | Text to synthesize; UTF-8, byte and chunk limits apply to long-form input |
| `model` | no | Omitted or empty selects `kokoro-82m`; an explicit ID is honored |
| `voice` | no | Omitted uses the engine's default. An unknown ID falls back to that model's default |
| `response_format` | no | `mp3` (recommended) or `wav`; unsupported values return 400 |

Extra engine-specific fields are passed through, but portable applications
should prefer the fields above.

### Effective-model and voice-fallback headers

Successful speech responses include:

| Header | Meaning |
|---|---|
| `X-ChimpFlow-Model` | The effective model used for the request |
| `X-ChimpFlow-Voice-Fallback` | Present only when an unknown requested voice was replaced; value is the URL-encoded original voice ID |
| `X-ChimpFlow-Text-Chunks` | Number of chunks used for long-form synthesis |

Applications may log or display fallback for diagnostics, but should not treat
it as a synthesis failure. URL-decode the fallback value before displaying
arbitrary user-supplied voice IDs. If no fallback occurred,
`X-ChimpFlow-Voice-Fallback` is absent.

Voice validation is best-effort: if ChimpFlow cannot fetch the model's voice
catalog, it preserves the original request rather than blocking synthesis.

## Migrating an existing application

1. **Check the base URL.** It must end at the gateway root, not `/v1`.
2. **Keep authentication and audio handling.** Existing Bearer keys, WAV/MP3
   parsing, downloads, and STT calls do not need a format migration.
3. **Choose a model policy.** Use omitted/empty `model` for Kokoro, or select an
   ID returned by `/v1/models`. Do not assume Chatterbox exists on every host.
4. **Replace hard-coded cross-model voices.** Query
   `/v1/voices?model=<selected-model>` and store voice IDs per model, not as one
   global list. A PocketTTS voice such as `alba` is not a Kokoro voice ID.
5. **Decide whether legacy PocketTTS is intentional.** Explicit `pocket-tts`
   requests still select PocketTTS. Remove that explicit selection to adopt the
   Kokoro default.
6. **Accept fallback headers.** Do not reject a 200 response because
   `X-ChimpFlow-Voice-Fallback` is present; it means compatibility handling
   worked.
7. **Preserve long-form timeouts.** The endpoint returns only after all chunks
   and encoding finish; do not assume a separate job ID or polling API exists.
8. **Run the smoke tests below.** Verify both the new default and any explicit
   legacy path your app still uses.

Existing HiveMind voice and ChimpEcho clients explicitly require WAV; keep WAV
compatibility for those clients even though MP3 is recommended for new apps.

## Long-form input and responses

The same speech endpoint splits long text into paragraph/sentence chunks,
joins the synthesized PCM, and returns one WAV or MP3 file. There is no
separate job endpoint.

Current limits are 131072 input bytes, 512 chunks, 256 MiB joined WAV, and
64 MiB MP3. `X-ChimpFlow-Text-Chunks` reports the chunk count. Clients needing
immediate incremental playback should split their own sentences and make
foreground requests. Whole-chapter clients need generous HTTP timeouts and
should run synthesis outside their UI thread.

## Errors and operational behavior

- `400` — invalid JSON, missing input, unsupported response format, or input
  limits exceeded.
- `401` — missing, revoked, expired, or invalid API key.
- `403` — key lacks the required scope.
- `413` — long-form input exceeds the documented byte/chunk limit.
- `502` — the companion engine, WAV assembly, or local MP3 encoder failed.
- `503` — the legacy unscoped voice catalog is not configured.

Do not retry every failure automatically. In particular, do not add speech
prefixes, warm-up requests, or automatic retakes to hide PocketTTS corruption;
migrate to Kokoro, or explicitly choose Chatterbox for suitable high-end work.

## Smoke tests

Set `CHIMPFLOW_URL` to the gateway root and `CHIMPFLOW_KEY` to the app's key.
These checks generate short speech but do not modify service configuration.

### Default model plus legacy-voice fallback

```bash
curl -fsS -D response.headers \
  -H "Authorization: Bearer $CHIMPFLOW_KEY" \
  -H "Content-Type: application/json" \
  -d '{"input":"Application migration test","voice":"pixel_voice","response_format":"wav"}' \
  "$CHIMPFLOW_URL/v1/audio/speech" \
  -o response.wav

grep -i '^X-ChimpFlow-Model: kokoro-82m' response.headers
grep -i '^X-ChimpFlow-Voice-Fallback: pixel_voice' response.headers
file response.wav
```

Expected: HTTP 200, valid WAV audio, Kokoro as the effective model, and the
fallback header naming the intentionally invalid voice.

### Explicit legacy model remains explicit

```bash
curl -fsS -D pocket.headers \
  -H "Authorization: Bearer $CHIMPFLOW_KEY" \
  -H "Content-Type: application/json" \
  -d '{"model":"pocket-tts","input":"Legacy compatibility test","response_format":"wav"}' \
  "$CHIMPFLOW_URL/v1/audio/speech" \
  -o pocket.wav

grep -i '^X-ChimpFlow-Model: pocket-tts' pocket.headers
```

Expected: PocketTTS remains selected. This is a compatibility check, not a
recommendation to use PocketTTS for new speech.

## Reference documents

- [`README.md`](../README.md) — endpoint summary, installation, and examples
- [`design.md`](design.md) — internal request flows and architectural decisions
- [`deploy.md`](deploy.md) — service/model configuration and dual-host deployment
