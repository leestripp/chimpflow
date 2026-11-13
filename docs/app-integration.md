# ChimpFlow HTTP API client guide

Language-neutral reference for any application that needs to call a running
ChimpFlow service. Use it from any programming language with an HTTP client;
there is no required SDK and no dependency on the application's UI framework.

The API uses JSON for discovery and control, binary audio for speech responses,
and multipart form data for transcription uploads.

## Connection

Use the ChimpFlow gateway:

```text
http://127.0.0.1:8090
```

This is the base URL. Append paths such as `/v1/models` yourself. Do not
configure a base URL ending in `/v1` if the client also appends `/v1/...`, or
it will request `/v1/v1/...` and receive 404.

Remote applications should use their provisioned secure tunnel. Do not connect
directly to the unauthenticated audio.cpp or whisper.cpp engine ports.

## Authentication

Send an application API key in this HTTP header:

```http
Authorization: Bearer omn_sk_...
```

Speech requires `tts` scope. Transcription requires `stt` scope. Discovery
accepts any valid key. Health and the WebUI require no key.

Use a per-application key, not the bootstrap/admin key. Keep keys out of source
code, URLs, and logs.

## Endpoint summary

| Method and path | Input | Successful response | Auth |
|---|---|---|---|
| `GET /healthz` | None | JSON health object | None |
| `GET /v1/models` | None | JSON model list | Any valid key |
| `GET /v1/voices?model=<model-id>` | Query parameter `model` | JSON voice list for that model | Any valid key |
| `GET /v1/voices` | None | Legacy JSON WAV-directory voice list | Any valid key |
| `POST /v1/audio/speech` | JSON speech request | WAV or MP3 bytes plus metadata headers | `tts` |
| `POST /v1/audio/transcriptions` | Multipart audio upload | JSON transcription object | `stt` |

## Health

### `GET /healthz`

Example response:

```json
{
  "status": "ok",
  "service": "chimpflow"
}
```

Use this only as a liveness check. Model availability is discovered separately.

## Model discovery

### `GET /v1/models`

There is no request body. The response is a JSON object with a `data` array:

```json
{
  "data": [
    {
      "id": "kokoro-82m",
      "object": "model",
      "owned_by": "engine",
      "family": "kokoro_tts",
      "task": "tts",
      "mode": "offline",
      "loaded": true,
      "path": "/path/on/server/model.gguf"
    }
  ]
}
```

Model fields:

| Field | Type | Client use |
|---|---|---|
| `id` | string | Submit as `model` in speech requests |
| `object` | string | Object type; currently `model` |
| `owned_by` | string | Informational owner/provider label |
| `family` | string | Engine model family, such as `kokoro_tts` |
| `task` | string | Model task; speech models use `tts` |
| `mode` | string | Engine execution mode, such as `offline` |
| `loaded` | boolean | Whether the engine reports the model loaded |
| `path` | string | Server-side model path; informational, not a client URL |

Model availability is host-specific. Discover IDs at runtime rather than
assuming every host has the same inventory.

Typical model choices:

- **`kokoro-82m`** — default for everyday application speech and small devices.
- **`chatterbox`** — specialist high-end narration/video-voiceover model when
  listed by the host; much larger and more RAM/VRAM/compute intensive.
- **`pocket-tts`** — legacy compatibility only; not recommended for new work
  because of intermittent garbled output.

## Voice discovery

### `GET /v1/voices?model=<model-id>`

The `model` query parameter is the exact `id` returned by `/v1/models`. There
is no request body.

Example request target:

```text
/v1/voices?model=kokoro-82m
```

Example response:

```json
{
  "data": [
    {
      "id": "af_alloy",
      "object": "voice",
      "owned_by": "engine"
    },
    {
      "id": "af_heart",
      "object": "voice",
      "owned_by": "engine"
    }
  ]
}
```

Voice fields:

| Field | Type | Client use |
|---|---|---|
| `id` | string | Submit as `voice` in speech requests |
| `object` | string | Object type; currently `voice` |
| `owned_by` | string | Informational owner/provider label |

Voice IDs are model-specific. Query voices again when the selected model
changes; do not reuse one global voice list across models.

The legacy unscoped `GET /v1/voices` returns the same outer `data` structure,
but its IDs come from the configured WAV directory rather than a selected
model's catalog. New clients should use the model-scoped endpoint. The legacy
catalog returns 503 if that directory is not configured.

## Speech synthesis

### `POST /v1/audio/speech`

Request content type:

```http
Content-Type: application/json
```

Request body:

```json
{
  "model": "kokoro-82m",
  "input": "Hello from my application.",
  "voice": "af_heart",
  "response_format": "mp3"
}
```

Request fields:

| Field | JSON type | Required | Behavior |
|---|---|---|---|
| `model` | string | No | Exact model ID. Omitted or empty selects `kokoro-82m`; an explicit ID is honored |
| `input` | string | Yes | UTF-8 text to synthesize |
| `voice` | string | No | Voice ID for the selected model. Omitted uses the model default; an unknown ID falls back to that default |
| `response_format` | string | No | `mp3` or `wav`; MP3 is recommended for new clients |

Extra engine-specific fields are passed through, but portable clients should
prefer the fields above.

### Speech success response

The body is the complete audio file, not JSON:

| Requested format | HTTP `Content-Type` | Body |
|---|---|---|
| `mp3` | `audio/mpeg` | MP3 bytes |
| `wav` | `audio/wav` | WAV bytes |

Relevant response headers:

| Header | Presence | Meaning |
|---|---|---|
| `X-ChimpFlow-Model` | Successful speech responses | Effective model used |
| `X-ChimpFlow-Voice-Fallback` | Only when an unknown voice was replaced | URL-encoded original voice ID |
| `X-ChimpFlow-Text-Chunks` | Long-form responses | Number of synthesized text chunks |

A fallback is not an error. If the header is present, the request still
returned audio using the model's default voice. URL-decode its value before
displaying arbitrary user-supplied voice IDs.

Voice validation is best-effort. If the model's voice catalog cannot be read,
ChimpFlow forwards the original request rather than blocking synthesis.

### Long-form speech

The same endpoint accepts long text, splits it into paragraphs/sentences,
synthesizes the chunks, joins the audio, and returns one complete file. There
is no job ID and no separate polling endpoint.

Current limits:

- 131072 input bytes
- 512 chunks
- 256 MiB joined WAV
- 64 MiB MP3

The HTTP response arrives only after synthesis and encoding finish. Clients
should use appropriate timeouts and avoid blocking their UI thread. For
immediate incremental playback, split sentences in the client and make
separate requests.

## Speech-to-text transcription

### `POST /v1/audio/transcriptions`

Request content type:

```http
Content-Type: multipart/form-data
```

Multipart fields:

| Field | Type | Required | Behavior |
|---|---|---|---|
| `file` | file upload | Yes | Audio file in a format whisper.cpp accepts; 16 kHz mono WAV is safest |
| `model` | form string | No | Use `whisper` |

Keep uploads below the gateway's 24 MB request limit.

Example response:

```json
{
  "text": "The transcribed text."
}
```

The `text` field is a JSON string containing the transcription.

## Errors

Error responses are JSON rather than audio:

```json
{
  "error": "Human-readable error message"
}
```

Clients should check the HTTP status and content type before parsing a speech
response as audio.

| Status | Meaning |
|---|---|
| `400` | Invalid JSON, missing input, unsupported response format, or another invalid request |
| `401` | Missing, revoked, expired, or invalid API key |
| `403` | Valid key without the required scope |
| `404` | Endpoint does not exist; also check for an accidental `/v1/v1/...` URL |
| `413` | Long-form input exceeds the byte/chunk limit |
| `502` | Companion engine, WAV assembly, transcription engine, or MP3 encoder failed |
| `503` | Legacy unscoped voice catalog is not configured |

Do not automatically retry every error. Correct 4xx requests and report
persistent 5xx failures to the service operator.

## Generic client flow

1. Optionally call `GET /healthz`.
2. Call `GET /v1/models` and select a model `id`.
3. Call `GET /v1/voices?model=<selected-id>` and select a voice `id`.
4. Send JSON to `POST /v1/audio/speech` with `input`, `model`, `voice`, and
   `response_format`.
5. Check the HTTP status and `Content-Type`.
6. On success, consume the response body as WAV or MP3 bytes. Read
   `X-ChimpFlow-Model` and optional fallback metadata if needed.
7. For transcription, upload multipart `file` and parse the returned `text`
   string.

These are ordinary HTTP operations and can be implemented with any language's
HTTP, JSON, multipart, and binary-file facilities.

## Compatibility for existing clients

- Existing Bearer authentication, WAV/MP3 handling, and STT interfaces remain
  unchanged.
- Requests that omit `model` now select Kokoro. Set `model` explicitly only
  when a particular model is intentional.
- Explicit `pocket-tts` requests are not redirected to Kokoro.
- Existing clients with an unknown voice receive the model's default voice;
  inspect `X-ChimpFlow-Voice-Fallback` when diagnostics are needed.
- Existing WAV clients can keep using WAV. MP3 is a recommendation for new
  clients, not a required migration.

## Reference documents

- [`README.md`](../README.md) — installation and endpoint overview
- [`design.md`](design.md) — internal request flows and architecture
- [`deploy.md`](deploy.md) — service and model configuration
