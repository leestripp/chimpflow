# ChimpFlow

![ChimpFlow banner — a chimp with a megaphone on a cliff over a vast jungle](resources/chimpflow-banner.png)

**Fully local voice service for Linux — no cloud**
TTS and STT behind your own API-key auth, with a web admin UI. Built on
**OmniNet** (C++23 HTTP/sqlite) with **audio.cpp** + **PocketTTS** (Vulkan) for
speech and **whisper.cpp** + **ggml-tiny.en** for transcription. Every model
runs on your machine; the only network use is the one-time download of model
files.

```
                ┌─▶ audio.cpp server ──▶ PocketTTS (Vulkan)   /v1/audio/speech
client ──HTTP──▶│  ChimpFlow (OmniNet) ──HTTP──▶ whisper.cpp ──▶ ggml-tiny.en   /v1/audio/transcriptions
                └─▶ API keys, scopes, WebUI, sqlite           /v1/models

## Features

- **OpenAI-compatible TTS**: `POST /v1/audio/speech` (model, input, voice), with real **MP3 (recommended)** or WAV output and automatic whole-chapter splitting/joining
- **OpenAI-compatible STT**: `POST /v1/audio/transcriptions` (multipart `file` → `{"text":"..."}`, powered by local whisper.cpp)
- **Fully local**: all inference on your hardware — no external API calls, works offline
- **Scoped API keys**: standard agent keys have `tts` and/or `stt`; the protected
  bootstrap master also has `admin`. Keys are hashed at rest in sqlite, with per-key
  use counts, revoke/restore/permanent-delete lifecycle, and expiration support
- **WebUI** at `/` — master-key-protected key management, copyable agent instructions,
  MP3/WAV TTS test bench (26 voices), Status tab with
  **one card per engine**: an `audio.cpp` card (TTS) and a `whisper.cpp` card (STT),
  each showing endpoint, reachability, and loaded model; the header carries a build
  badge (currently `v0.3.0`) so a stale cached page is self-evident
- **One-time secret copying** — the generate-key area has **Copy new secret
  key** for the newly created `rawKey`; existing rows have no Copy ID action
- **Bootstrap master key** printed once on first run and protected from revoke/delete

## Tests

```bash
ctest --test-dir build --output-on-failure          # unit + integration
ctest --test-dir build -L unit                      # unit only (always runs)
ctest --test-dir build -L integration               # needs the live service (SKIPs if down)
```

Unit tests cover the demo's WAV/multipart helpers, long-form text chunking,
WAV joining, and real WAV→MP3 encoding (`tests/test_wav.cpp`,
`tests/test_text_chunk.cpp`, `tests/test_audio_format.cpp`, zero-dependency harness).
Integration (`tests/integration.sh`) smoke-tests a running service: auth gates
(401/403), WAV and decoded MP3 TTS, STT round trip, /v1/models, /skill, and the
complete key revoke/restore/permanent-delete lifecycle.

## Demo

`demo/` holds **ChimpEcho**, a hold-to-talk parrot app (gtkmm-4 + PipeWire)
that round-trips your voice through the service — STT then TTS. See
[demo/README.md](demo/README.md).

## Build

Requires: GCC 13+ (C++23), CMake 3.20+, OpenSSL, SQLite3, and ffmpeg with
`libmp3lame`. CMake FetchContent obtains OmniNet from `leestripp/omninet.git`
at `main`; never patch the fetched checkout locally.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ln -sfn ../web build/web    # serve the WebUI from next to the binary
./build/chimpflow           # run from anywhere; db lands in the CWD
```

## Install (user-level service, no root)

```bash
bash install.sh
```

Builds and installs as a **systemd user service** running as you: binary +
WebUI to `~/.local/lib/chimpflow/`, data (sqlite + `ADMIN_KEY`) to
`~/.local/share/chimpflow/`. Enabled at boot via `loginctl enable-linger`
— starts without login, restarts on failure, logs to the user journal.
On a fresh database the binary generates a bootstrap admin key
(`chimpflow-bootstrap-admin`, scopes `admin`+`tts`+`stt`) and prints it
**once**; `install.sh` scrapes the journal and saves it to
`~/.local/share/chimpflow/ADMIN_KEY` (mode 600). Lose every admin key and the
only reset is deleting `data/chimpflow.db` (destructive) — full flow in
[docs/deploy.md](docs/deploy.md). Manage with
`systemctl --user {status,restart,stop} chimpflow` and
`journalctl --user -u chimpflow -f`.

## Run (dev, from the repo)

```bash
./build/chimpflow
```

On first start it creates `data/chimpflow.db` (created automatically) and prints
a bootstrap admin key
(**shown once** — save it). Then open http://127.0.0.1:8090/ and sign in.

The audio.cpp engine must be running on `127.0.0.1:8080` (see
`/projects/AI/ChimpFlow/engine` or your engine install).

## API

| Endpoint | Auth | Purpose |
|---|---|---|
| `GET /healthz` | none | liveness |
| `GET /v1/models` | any valid key | engine model list (proxied) |
| `GET /v1/voices` | any valid key | voice catalog (`CHIMPFLOW_VOICES_DIR` `*.wav` basenames; 503 if unconfigured) |
| `POST /v1/audio/transcriptions` | `stt` scope | STT, multipart `file` forwarded to whisper.cpp (`{"text":"..."}`) |
| `GET /skill/` + `/skill/manifest.json` | none | agent guide: how an AI agent should use this API (markdown + JSON twin) |
| `POST /v1/audio/speech` | `tts` scope | TTS with `response_format: "mp3"` (recommended) or `"wav"`; MP3 encoded locally by ffmpeg |
| `GET /admin/keys` | `admin` | list keys (id, name, scopes, useCount, status) |
| `POST /admin/keys` | `admin` | create an agent key with `tts`/`stt` scopes only (default both) → returns `rawKey` once |
| `DELETE /admin/keys/<id>` | `admin` | revoke an agent key; bootstrap master returns 409 |
| `POST /admin/keys/<id>/restore` | `admin` | restore a revoked key |
| `DELETE /admin/keys/<id>/permanent` | `admin` | permanently delete a revoked key (409 if active/master, 404 if missing) |
| `GET /admin/engine/models` | `admin` | engine's model list (proxied) |
| `GET /admin/engine/stt` | `admin` | whisper.cpp reachability + served model (`{"reachable":false}`, never 502) |
| `GET /` | none | WebUI |

### Example

```bash
curl -X POST http://127.0.0.1:8090/v1/audio/speech \
  -H "Authorization: Bearer omn_sk_..." \
  -H "Content-Type: application/json" \
  -d '{"model":"pocket-tts","input":"Hello world","voice":"alba","response_format":"mp3"}' \
  -o out.mp3
```

### Long-form chapters

The same speech endpoint automatically splits long text into paragraph/sentence
chunks (about 1200 bytes), joins all synthesized PCM, and returns one complete
WAV or MP3. There is no separate job endpoint. `X-ChimpFlow-Text-Chunks` reports
the chunk count. Input is limited to 131072 bytes and 512 chunks; joined WAV is
capped at 256 MiB and MP3 at 64 MiB.

For immediate incremental playback, clients should split their own sentences
and make foreground requests. For a whole chapter returned as one file, agents
should **run long-form synthesis as a background process** and use the tool's
automatic completion trigger instead of polling. The HTTP response arrives only
after synthesis and encoding complete, so long-form clients need generous
timeouts. Existing short WAV/MP3 clients require no changes.

## Security notes

- Binds **127.0.0.1** by default — change `serverConfig.address` to expose on LAN.
- Keys are stored as hashes; the raw key is returned exactly once at creation.
  Row IDs are only identifiers and cannot authenticate. Existing raw secrets
  cannot be recovered; create a replacement key if the original was lost.
  **Revoke** disables a key but keeps its record; **Restore** re-enables it;
  **Delete** permanently removes a revoked record after confirmation. Active keys
  cannot be permanently deleted. The bootstrap master cannot be revoked or deleted.
- The WebUI keeps the admin key in `localStorage` (localhost-only trade-off).

## Layout

```
src/main.cpp       service: auth, routes, engine proxy, bootstrap
src/audio_format.* local WAV joining and WAV→MP3 conversion via ffmpeg
src/text_chunk.*   bounded paragraph/sentence/UTF-8 long-form splitting
web/index.html  self-contained admin SPA (no build step)
demo/           ChimpEcho hold-to-talk demo client (standalone CMake)
tests/          unit harness + live-service integration suite
docs/design.md  architecture, API design, invariants
docs/deploy.md  installation, units, updating, troubleshooting
resources/      static assets (banner, etc.)
data/           runtime state (sqlite db) — created on first run, gitignored
CMakeLists.txt     C++23 build; OmniNet via FetchContent
```

Design details live in [docs/design.md](docs/design.md); deployment (what runs where, first install, units, updating, troubleshooting) in [docs/deploy.md](docs/deploy.md).

## Roadmap

- ~~whisper.cpp STT route~~ done: `/v1/audio/transcriptions` via local whisper.cpp on :8081 (`ggml-tiny.en`)
- Model management in the WebUI (download/enable via audio.cpp GGUF registry)
- Rate limiting per key

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 Lee Stripp and daedalus.
