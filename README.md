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

- **OpenAI-compatible TTS**: `POST /v1/audio/speech` (model, input, voice, response_format)
- **OpenAI-compatible STT**: `POST /v1/audio/transcriptions` (multipart `file` → `{"text":"..."}`, powered by local whisper.cpp)
- **Fully local**: all inference on your hardware — no external API calls, works offline
- **Scoped API keys**: `tts` / `stt` / `admin` scopes, hashed at rest in sqlite,
  per-key use counts, revocation, expiration support
- **WebUI** at `/` — key management, TTS test bench (26 voices), Status tab with
  **one card per engine**: an `audio.cpp` card (TTS) and a `whisper.cpp` card (STT),
  each showing endpoint, reachability, and loaded model; the header carries a build
  badge (currently `v0.1.5`) so a stale cached page is self-evident
- **Bootstrap admin key** printed once on first run

## Tests

```bash
ctest --test-dir build --output-on-failure          # unit + integration
ctest --test-dir build -L unit                      # unit only (always runs)
ctest --test-dir build -L integration               # needs the live service (SKIPs if down)
```

Unit tests cover the demo's WAV/multipart helpers (`tests/test_wav.cpp`,
zero-dependency harness). Integration (`tests/integration.sh`) smoke-tests a
running service: auth gates (401/403), TTS, STT round trip, /v1/models, /skill.

## Demo

`demo/` holds **ChimpEcho**, a hold-to-talk parrot app (gtkmm-4 + PipeWire)
that round-trips your voice through the service — STT then TTS. See
[demo/README.md](demo/README.md).

## Build

Requires: GCC 13+ (C++23), CMake 3.20+, OpenSSL, SQLite3. OmniNet is expected at
`/projects/Websites/OmniNet` (override with `-DOMNINET_DIR=...`); if its
prebuilt `libOmniNet.a` is missing, OmniNet is built from source automatically.

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
`/projects/audio-cpp-test` or your engine install).

## API

| Endpoint | Auth | Purpose |
|---|---|---|
| `GET /healthz` | none | liveness |
| `GET /v1/models` | any valid key | engine model list (proxied) |
| `GET /v1/voices` | any valid key | voice catalog (`CHIMPFLOW_VOICES_DIR` `*.wav` basenames; 503 if unconfigured) |
| `POST /v1/audio/transcriptions` | `stt` scope | STT, multipart `file` forwarded to whisper.cpp (`{"text":"..."}`) |
| `GET /skill/` + `/skill/manifest.json` | none | agent guide: how an AI agent should use this API (markdown + JSON twin) |
| `POST /v1/audio/speech` | `tts` scope | TTS, body forwarded to the engine (OpenAI shape) |
| `GET /admin/keys` | `admin` | list keys (id, name, scopes, useCount, status) |
| `POST /admin/keys` | `admin` | create key `{"name":"...","scopes":["tts"]}` → returns `rawKey` once |
| `DELETE /admin/keys/<id>` | `admin` | revoke a key |
| `GET /admin/engine/models` | `admin` | engine's model list (proxied) |
| `GET /admin/engine/stt` | `admin` | whisper.cpp reachability + served model (`{"reachable":false}`, never 502) |
| `GET /` | none | WebUI |

### Example

```bash
curl -X POST http://127.0.0.1:8090/v1/audio/speech \
  -H "Authorization: Bearer omn_sk_..." \
  -H "Content-Type: application/json" \
  -d '{"model":"pocket-tts","input":"Hello world","voice":"alba","response_format":"wav"}' \
  -o out.wav
```

## Security notes

- Binds **127.0.0.1** by default — change `serverConfig.address` to expose on LAN.
- Keys are stored as hashes; the raw key is returned exactly once at creation.
- The WebUI keeps the admin key in `localStorage` (localhost-only trade-off).

## Layout

```
src/main.cpp    service: auth, routes, engine proxy, bootstrap
web/index.html  self-contained admin SPA (no build step)
demo/           ChimpEcho hold-to-talk demo client (standalone CMake)
tests/          unit harness + live-service integration suite
docs/design.md  architecture, API design, invariants
docs/deploy.md  installation, units, updating, troubleshooting
resources/      static assets (banner, etc.)
data/           runtime state (sqlite db) — created on first run, gitignored
CMakeLists.txt  links OmniNet (prebuilt or from source)
```

Design details live in [docs/design.md](docs/design.md); deployment (what runs where, first install, units, updating, troubleshooting) in [docs/deploy.md](docs/deploy.md).

## Roadmap

- ~~whisper.cpp STT route~~ done: `/v1/audio/transcriptions` via local whisper.cpp on :8081 (`ggml-tiny.en`)
- Model management in the WebUI (download/enable via audio.cpp GGUF registry)
- Rate limiting per key

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 Lee Stripp and daedalus.
