# ChimpFlow — Design

Design document for the ChimpFlow voice service and the ChimpEcho demo.
Audience: future maintainers and coding agents. For the living API guide
served to AI agents at runtime, see `/skill/`; for build/deploy commands and
the hard-won-facts list, see [AGENTS.md](../AGENTS.md).

## Goals

- **Fully local voice for Linux** — TTS and STT with zero cloud dependency;
  the only network use is the one-time download of model files.
- **OpenAI-compatible surface** so existing clients (and AI agents) "just work".
- **One auth gate** — scoped API keys in front of engines that have none.
- **Boring operations** — user-level systemd, sqlite, no root, no containers.

Non-goals: general model serving, multi-user tenancy, non-localhost exposure
by default, GPU/engine orchestration (engines are companions we expect to be
already running).

## System architecture

```
 clients (curl, demo apps, agents)
        │  HTTP + Bearer key
        ▼
 ┌─────────────────────────────┐        ┌─▶ audio.cpp server :8080 ─▶ PocketTTS (Vulkan)   TTS
 │  ChimpFlow :8090 (OmniNet)  │─proxy──┤
 │  auth · scopes · keys       │        └─▶ whisper.cpp :8081 ──────▶ ggml-tiny.en        STT
 │  sqlite · WebUI · /skill    │
 └─────────────────────────────┘
```

ChimpFlow is deliberately **a gateway, not a processor**: audio bodies are
forwarded byte-for-byte to the engine that owns them. It never parses speech
payloads, which keeps it small, fast, and out of the codec business.

Companions (expected to be running, started independently):

| Companion | Address | Role | Run by |
|---|---|---|---|
| audio.cpp server | `127.0.0.1:8080` | TTS (PocketTTS, Vulkan) | see `/projects/AI/ChimpFlow/engine` |
| whisper.cpp server | `127.0.0.1:8081` | STT (`ggml-tiny.en`) | systemd user unit `chimpflow-whisper.service` |

The whisper model has a single source: `CHIMPFLOW_STT_MODEL` in
`chimpflow.env` (compiled default `kSttModel`) feeds both the whisper unit's
`--model` and the status route — whisper-server exposes no model-listing
endpoint.

## Components

### ChimpFlow service (`src/main.cpp`, ~450 lines, single file)

Everything lives in one translation unit by design (the service is a thin
auth/proxy layer; splitting it would add ceremony, not clarity):

- **Routes** — OmniNet `Server` with per-route lambdas; two helper closures
  (`bearerToken`, `requireAdmin`) keep auth checks uniform.
- **Key store** — OmniNet `Token::ApiKeyStore` over sqlite: keys are
  `omn_sk_<hex>_<secret>`, stored sha-hashed, raw value returned exactly once
  at creation. Scopes: `admin`, `tts`, `stt`. Per-key use counts, revocation,
  expiry support.
- **Engine proxy** — each `/v1/*` route validates the key + scope, then
  forwards method/body/content-type to the engine and streams the engine's
  status/headers/body straight back. Errors: 502 when an engine is down.
- **`/skill/`** — markdown guide + `manifest.json` twin generated from the
  same constants (`kSkillMd`/`kSkillManifest`). **Invariant:** whenever a
  route changes, update these too — the runtime guide must never drift.
- **WebUI** — `web/index.html`, a self-contained vanilla-JS SPA (no build
  step), served from the directory next to the binary (resolved via
  `/proc/self/exe`).

### WebUI

Three tabs: **Keys** (create/revoke, scopes checkboxes), **TTS** (test bench,
voice dropdown from `/v1/models`), **Status** (service health plus **one full
card per engine** — an `audio.cpp` card for TTS and a `whisper.cpp` card for
STT, each showing endpoint, reachability dot, and loaded model. Revoked key
rows also offer a confirmed **Delete** action; active keys must be revoked
before permanent deletion. The audio.cpp
card also shows the model file path). The header carries a visible build
badge (`v0.1.9`) so a stale cached render is self-diagnosing. `index.html`
is served with `Cache-Control: no-cache` because a single-file SPA otherwise
heuristically caches. The admin key lives in `localStorage` (localhost-only
trade-off, documented). The generate-key area offers **Copy new secret key**
for the one-time `rawKey`; clipboard helpers are scoped to that action only.
Existing raw secrets are never reconstructed from IDs, and there is no Copy ID.

### ChimpEcho demo (`demo/`)

Hold-to-talk parrot: PipeWire mic → WAV → STT → TTS → PipeWire speaker.
Separate from the service on purpose — it is a *client*, proving the API from
the outside. Thread model:

- **GTK main thread** owns all widgets; never blocks on the network.
- **PipeWire thread loops** (one per stream) run the audio callbacks.
- **One `std::jthread` worker** per echo cycle runs STT→TTS HTTP with hard
  timeouts (3 s connect / 45 s request); the result marshals back through a
  `Glib::Dispatcher` (gives happens-before visibility).
- **Logging** to stderr, flushed per line (`CHIMPECHO_LOG` mirrors to a file);
  the log is the first diagnostic stop.

Audio helpers (`demo/src/wav.h`, header-only, shared with the unit tests):
canonical RIFF/WAVE writer (`makeWav16`), a strict PCM parser (16/24/32-bit,
sign extension, odd-chunk padding), and multipart body framing.

## API design

| Route | Auth | Behaviour |
|---|---|---|
| `GET /healthz` | none | liveness |
| `GET /` | none | WebUI |
| `GET /skill/`, `/skill/manifest.json` | none | agent guide (markdown + JSON twin) |
| `GET /v1/models` | any valid key | audio.cpp model/voice list, proxied |
| `POST /v1/audio/speech` | `tts` scope | body proxied to audio.cpp `/v1/audio/speech` |
| `POST /v1/audio/transcriptions` | `stt` scope | multipart proxied to whisper.cpp `/inference` |
| `GET /admin/keys` | `admin` | list keys (id, name, scopes, useCount, status) |
| `POST /admin/keys` | `admin` | create; returns `rawKey` once |
| `DELETE /admin/keys/<id>` | `admin` | revoke |
| `DELETE /admin/keys/<id>/permanent` | `admin` | permanently delete a revoked row (409 active, 404 missing) |
| `GET /admin/engine/models` | `admin` | audio.cpp models, proxied |
| `GET /admin/engine/stt` | `admin` | whisper.cpp health probe + served model (env `CHIMPFLOW_STT_MODEL`); reports `{"reachable":false}` instead of 502 so the UI can render it |

Design choices:

- **Scope checks live in the gateway**, not the engines — engines stay
  unmodified and unauthenticated on localhost.
- **`/v1/models` is deliberately not admin-gated** — clients need model/voice
  names to form valid requests; it lists nothing sensitive.
- **`/admin/engine/stt` never 502s**: an admin status probe that errors when
  the subject is down is useless for showing status.
- Multipart STT bodies pass through untouched (OmniNet's ~10 MB body limit
  applies), so any format whisper.cpp accepts (wav/mp3/ogg…) works.

## Request flows

**TTS** — client → `POST /v1/audio/speech` → key+`tts` scope → forward body
to audio.cpp → stream WAV back. 502 if audio.cpp is down.

**STT** — client → `POST /v1/audio/transcriptions` (multipart `file`) →
key+`stt` scope → forward to whisper.cpp `/inference` → JSON `{"text"}` back.
502 if whisper is down.

**Key lifecycle** — bootstrap admin key is printed once on a fresh db and
saved to `ADMIN_KEY` (mode 600); there is no recovery — losing every admin
key means deleting the db (destructive). Created keys return their raw value
exactly once; only the sha hash is stored.

## Data & configuration

- sqlite db in the service CWD (`data/chimpflow.db` dev,
  `~/.local/share/chimpflow/` installed) — hashed keys only.
- Engine addresses and the STT model are env-driven (`CHIMPFLOW_ENGINE_URL`,
  `CHIMPFLOW_STT_URL`, `CHIMPFLOW_STT_MODEL` in `chimpflow.env`; compile-time
  constants are the defaults); localhost is the deployment target.
- Demo configuration is env-based: `CHIMPFLOW_URL`, `CHIMPFLOW_KEY`
  (default: read `~/.local/share/chimpflow/ADMIN_KEY`), `CHIMPFLOW_VOICE`.
- The demo key needs **both** `stt` and `tts` scopes.

## Deployment

`install.sh` — user-level, no root: binary + WebUI to `~/.local/lib/chimpflow/`,
data + `ADMIN_KEY` to `~/.local/share/chimpflow/`, systemd user unit with
restart-on-failure, `loginctl enable-linger` for boot-time start. Dev runs
straight from `build/` with a `build/web → ../web` symlink. The WebUI is
served from next to the binary, so the installed copy must be refreshed by
re-running `install.sh` after web changes.

## Testing

- **Unit** (`ctest -L unit`, always runs): the zero-dependency harness in
  `tests/test_util.h` covers `demo/src/wav.h` — WAV round-trip, 16/24/32-bit
  parsing, sign extension, odd-chunk padding, rejection cases, multipart
  framing.
- **Integration** (`ctest -L integration`, skips cleanly if the service is
  down): `tests/integration.sh` — 13 curl-based checks: auth gates (401/403),
  TTS returns WAV, **STT round trip** (TTS output transcribed back), model
  lists, `/skill` docs, `/admin/engine/stt`, key create/revoke.
- **Concurrency repros**: audio teardown/drain bugs were proven with tiny
  extracted-struct harnesses run under `timeout` (`/tmp/pw_repro*.cpp`
  pattern) — no GUI clicking required. See invariants below.

## Security model

- Binds `127.0.0.1`; change `serverConfig.address` deliberately to expose.
- Keys hashed at rest; raw shown once; per-key use counting; revocation.
- WebUI admin key in `localStorage` — acceptable only because the origin is
  localhost.
- Engines are unauthenticated localhost services — anything on the machine
  can talk to them directly; ChimpFlow adds the gate for remote clients
  should you ever bind a LAN address.

## Invariants & known pitfalls

- `/skill` guide + manifest must be updated with any route change.
- `CHIMPFLOW_STT_MODEL` in `chimpflow.env` must stay the model the whisper unit
  actually serves — it drives both the unit's `--model` and the status page
  (compiled default `kSttModel`).
- WebUI changes require re-running `install.sh` to reach the served copy.
- **Bump the header badge on every WebUI change** (`web/index.html`, currently
  `v0.1.9`). The badge + `no-cache` exist because heuristic browser caching
  showed a user a pre-fix page for hours while `curl` proved the server was
  correct. Verify UI complaints at three layers: served bytes (`curl`), a
  fresh-browser render, and the request log
  (`journalctl --user -u chimpflow | grep "succeeded - path"`) showing whether
  the browser even re-fetched.
- **Status tab = one card per engine, named for the binary** (`audio.cpp` /
  `whisper.cpp`) with role, endpoint, and model. Vague headings ("Engine
  models") read as ambiguous — name things so a first-time user knows exactly
  what is running on the machine.
- **PipeWire:** never call `pw_thread_loop_stop()` while holding
  `pw_thread_loop_lock()` — join-vs-lock deadlock (froze the demo UI).
  Order: lock → `pw_stream_destroy` → unlock → stop. Full list in
  [AGENTS.md](../AGENTS.md) "Hard-won facts".
- **State latches on reused objects must be re-armed** (`Playback::notified`
  was a one-shot that broke the second echo cycle) — reset per-cycle state in
  `start()`, not just in the constructor.
- One-shot HTTP diagnostics: log to stderr with per-line flush; a wedged UI's
  last log line should name the phase.

## Roadmap

- Model management in the WebUI (download/enable via audio.cpp GGUF registry)
- Rate limiting per key
- See [Plan.md](../Plan.md) for the engine-selection research history
  (audio.cpp chosen; all alternatives rejected with reasons).
