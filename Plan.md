# Plan — OmniNet Voice Service: Local TTS Research & Architecture (2026-10-05)

## What we are building

**OmniNet** — a C++23 network service:

- Many concurrent clients, authenticated per-client via **API_KEY**.
- **whisper.cpp** for STT (native C++ — link directly).
- **TTS engine — DECIDED: audio.cpp** (pure C++23/ggml, same deployment model as
  whisper.cpp). PocketTTS chosen as first model; more models (Kokoro, Supertonic,
  etc.) are GGUF packages in the same repo. See "DECISION" below.
- **WebUI** (served by OmniNet itself) for: settings, model management
  (download/enable/disable), API key management, engine status.

## Goal of today's research

Find the latest, best-voiced TTS tech that runs locally on Linux, and decide the
integration architecture between OmniNet (C++) and the TTS engines.

---

## DECISION (2026-10-05 evening): TTS = audio.cpp — drop all other engine paths

After hands-on testing (see "Local test results"), **audio.cpp is the TTS engine
for OmniNet**. Everything else (sherpa-onnx native path, Kokoro-FastAPI,
pocket-tts serve, Orpheus-FastAPI, vLLM-Omni, Rhapsode, DragonianVoice) is
**rejected** — no longer under consideration. Rationale:

- Pure C++/ggml — same build/deploy story as whisper.cpp, no Python anywhere.
- One runtime covers 100+ audio model families; models are GGUF packages
  (Kokoro, PocketTTS, Supertonic, Magpie, MOSS, Qwen3-TTS, Chatterbox...).
- Vulkan backend drove the RX 7800 XT with zero ROCm setup; RTF 0.12-0.20.
- Ships an OpenAI-compatible `POST /v1/audio/speech` server — that endpoint is
  the engine contract OmniNet talks to, whether the server runs as a subprocess
  or (longer term) audio.cpp is linked in-process.
- WebUI with side-by-side Arena = ready reference for OmniNet's own model UI.

**Architecture going forward:**
- OmniNet (C++23, your networking + sqlite lib) owns: client connections,
  API_KEY auth, rate limits, the WebUI, model/API-key registries (sqlite).
- Engine process: `audiocpp_server --config models.json --ui` on localhost;
  OmniNet proxies `POST /v1/audio/speech` to it per request.
- whisper.cpp stays the STT path, planned identically (in-process first).
- Model scope: start with PocketTTS (MIT, 100M, cloning, streaming); add
  Kokoro / Supertonic GGUF packages later via the same registry.

---

## [SUPERSEDED] Original engine-path research (kept for the record)

### Key architectural finding: two viable integration paths

### Path A — Native C++ via sherpa-onnx (no Python at all)

**sherpa-onnx** (k2-fsa, ONNX Runtime based) has a first-class **C and C++ API**
(`sherpa-onnx/c-api/c-api.h`) and now supports a surprising spread of TTS engines:

| Engine | Notes |
|---|---|
| **Kokoro** | multi-lang v1.1 (zh+en, 103 speakers), en v0.19 (11 speakers) |
| **PocketTTS** | Kyutai's 100M CPU model, voice cloning, streaming |
| **ZipVoice** | zero-shot voice cloning (flow-matching), zh/en |
| **SupertonicTTS** | lightning-fast on-device TTS, native ONNX |
| **KittenTTS** | tiny (~nano/mini), English, runs anywhere |
| **Matcha / VITS (Piper voices)** | the classic CPU workhorses |

Also relevant: **DragonianVoice** — a C++ ONNX inference library wrapping many TTS
families with C/C++/C# bindings (evaluate as an alternative/backup to sherpa-onnx).

**Pros:** single process, no Python deployment story, lowest latency, threads +
a queue per engine instance, exact same deployment model as whisper.cpp.
**Cons:** only the engines sherpa-onnx supports; newest research models (Qwen3-TTS,
Chatterbox, Breeze, MOSS) are not ONNX-exported there.

### Path B — Engine workers behind an OpenAI-compatible HTTP contract

Each TTS engine runs as its **own process** (Python is fine) exposing
`POST /v1/audio/speech` (the OpenAI TTS API shape). OmniNet spawns, supervises,
and load-balances workers on localhost — clients only ever talk to OmniNet.

Ready-made servers:

- **Kokoro-FastAPI** (remsky) — OpenAI-compatible, docker, CPU/AMD/NVIDIA, multi-speaker.
- **pocket-tts serve** — built-in HTTP server + web UI, streaming, ~200 ms first chunk.
- **Orpheus-FastAPI** — Orpheus-3B on llama.cpp, OpenAI-compatible endpoint.
- **vLLM-Omni** — production GPU serving for Qwen3-TTS, Fish S2, Chatterbox etc.
- **Chatterbox** — community OpenAI-compatible wrappers exist; vLLM-Omni covers Turbo.
- **Rhapsode** (rhapsode.dev) — existing multi-engine speech server: one HTTP
  contract, each engine in its own process, capability document per worker.
  **Read its protocol docs — it is essentially the worker layer we want to build,
  and a design reference even if we don't adopt it.**

**Pros:** any engine, any language; crash isolation; scale workers independently;
Python engines are first-class (Chatterbox, Qwen3-TTS, MOSS-TTS, Pocket TTS).
**Cons:** localhost HTTP hop latency (small vs synthesis time), process management
complexity, Python envs to deploy (uv/pin everything).

### Recommended: **Hybrid**

- **Tier 1 (native, in-process):** sherpa-onnx with Kokoro + PocketTTS + ZipVoice —
  the always-available baseline engines, zero extra deployment, same as whisper.cpp.
- **Tier 2 (worker processes):** OpenAI-compatible HTTP workers for heavyweight
  engines (Chatterbox Turbo, Qwen3-TTS, MOSS-TTS, Breeze for personal use).
- OmniNet exposes **one client-facing TTS API**; engine selection is per-request
  (`engine`/`voice` params) with a configured default. API keys can be scoped
  per-engine in the webUI.

This matches how we'll already treat whisper.cpp (in-process) and keeps the
"try several engines" goal trivial: adding an engine = registering a worker URL
or an sherpa-onnx model in the webUI.

---

## Engine shortlist (updated with integration path)

| Engine | Params | License | Footprint | Cloning | Integration |
|---|---|---|---|---|---|
| Kokoro-82M | 82M | Apache 2.0 | CPU / 2–3 GB | No | **sherpa-onnx (native)** or Kokoro-FastAPI |
| Pocket TTS (Kyutai) | 100M | MIT | CPU, 2 cores | Yes (wav ref) | **sherpa-onnx (native)** or `pocket-tts serve` |
| ZipVoice | ~0.5B | Apache 2.0* | GPU/CPU | Yes, zero-shot | **sherpa-onnx (native)** |
| SupertonicTTS | small | check | CPU | No | **sherpa-onnx (native)** |
| Chatterbox Turbo / v3 | 350M/0.5B | MIT | 4–6 GB | Yes (~5 s) | Worker (vLLM-Omni / wrapper) |
| Qwen3-TTS | 0.6/1.7B | Apache 2.0 | 4–8 GB | Yes + VoiceDesign | Worker (vLLM-Omni) |
| MOSS-TTS Nano / 4B | 0.1B/4B | Apache 2.0 | CPU (Nano) / GPU | Yes | Worker (Python) |
| Breeze TTS 2 | 3B | Non-commercial | 8–14 GB | Yes + design + direction | Worker; personal/research only |
| Fish S2 Pro | 4B | Non-commercial | 8+ GB | Best-in-class | Worker; personal/research only |

*Confirm ZipVoice/Supertonic/KittenTTS licenses during evaluation.

## Landscape snapshot (from today's research)

- **Breeze TTS 2** (Aug 25, 2026) — first open-weight model to beat ElevenLabs v3
  on the Artificial Analysis Speech Arena (1,215 vs 1,177 Elo). Non-commercial weights.
- **Qwen3-TTS** (Jan 2026) — Apache 2.0, now the default TTS in HF's
  speech-to-speech pipeline; most-downloaded TTS after Kokoro.
- **Chatterbox Multilingual v3** (Jun 2026) — MIT, 25 languages; Turbo variant
  ~75 ms latency, ~6x real-time. PerTh watermark embedded by default.
- **Pocket TTS** (Kyutai, MIT) — 100M, CPU-only, ~200 ms to first chunk, ~6x
  real-time on 2 cores, streaming, 7 languages, cloning from a wav. Training code
  released Aug 2026. **`pocket-tts serve` ships an HTTP server out of the box.**
- **MOSS-TTS family** (OpenMOSS, Apache 2.0) — v1.5 8B, 4B Local-Transformer
  (48 kHz stereo), CPU-only Nano (0.1B, 20 languages).
- **Kokoro-82M** — still the efficiency king; RTF ~0.03 on GPU; 54 voices.
- **Piper** — original repo archived Oct 2025; OHF-Voice/piper1-gpl fork (GPL-3.0).
  Quality now a tier below 2026 models; keep only as an edge fallback.
- **VibeVoice 7B** — withdrawn by Microsoft. Dead end.
- Serving stacks worth reusing: **Kokoro-FastAPI**, **vLLM-Omni**,
  **llama.cpp-omni** (VoxCPM2 GGUF), **Orpheus-FastAPI**, **LocalAI**.

---

## OmniNet design implications (research conclusions)

1. **Client-facing API:** adopt the OpenAI TTS shape
   (`POST /v1/audio/speech {model, voice, input, response_format, stream}`)
   plus OmniNet extensions (`engine`, `reference_audio` for cloning). Auth via
   `Authorization: Bearer <API_KEY>` — same header pattern clients already know.
2. **Concurrency model:** whisper.cpp and sherpa-onnx engines run as instances
   behind a per-engine request queue (both are not thread-safe per instance).
   Worker engines get a small pool of processes with a health-check loop.
3. **Streaming:** prioritize engines with true chunked streaming
   (Pocket TTS, Chatterbox Turbo, Kokoro sentence-chunking) — the client wants
   audio before the full utterance is synthesized.
4. **WebUI endpoints:** model registry (list/download/remove), engine registry
   (enable/disable, worker URLs, GPU assignment), API key CRUD + per-key
   rate limits and engine scopes, live RTF/latency stats per engine.
5. **Model management:** HF downloads with resume; store under a single
   `models/` tree keyed by engine; sherpa-onnx models are plain ONNX + tokens
   files — trivial to manage; Python workers need pinned uv environments.

## Evaluation plan (next session)

1. Record this machine's GPU/VRAM; decide CUDA vs ROCm vs Vulkan per engine.
2. Build sherpa-onnx and run its C++ TTS examples: Kokoro v1.1, PocketTTS,
   ZipVoice — measure RTF, time-to-first-audio, RAM/VRAM.
3. Stand up workers: `pocket-tts serve`, Kokoro-FastAPI, Chatterbox via
   vLLM-Omni — same metrics over the HTTP contract (incl. hop overhead).
4. Blind listening test: 5 fixed texts across all engines; rank.
5. Prototype the OmniNet engine abstraction:
   `class TtsEngine { virtual Stream<AudioChunk> speak(TtsRequest) }`
   with two impls — `SherpaEngine` (native) and `HttpWorkerEngine` (OpenAI-compat).
6. Full-loop latency test: whisper.cpp STT → LLM → TTS, target < ~1 s to
   first spoken word.

## Local test results — audio.cpp (2026-10-05)

Tested on dev-pc: AMD RX 7800 XT (12 GB), 12-core CPU, 30 GB RAM, Ubuntu.
Prebuilt `audio-v0.9.0-bin-ubuntu-x64-vulkan-portable` from GitHub releases —
zero build, Vulkan backend detected the GPU with no ROCm setup.

- **PocketTTS English Q8_0 GGUF** (128 MB) + 26 voice embeddings (~6 MB each):
  - CLI: RTF **0.12** on Vulkan (~8x real-time), 24 kHz mono, ~3.4 GB peak footprint.
  - Server: `audiocpp_server --config server.json --ui` -> embedded SvelteKit WebUI
    on :8080 (Studio + Arena tabs).
  - `POST /v1/audio/speech` (OpenAI-compatible, `model`/`voice`/`input`/`response_format`)
    -> HTTP 200 wav in 0.93 s including lazy model load. **This is exactly the
    HttpWorkerEngine contract OmniNet needs.**
  - All 26 English voices tested with identical pangram text; distinct output
    per voice (md5-verified). RTF 0.16-0.20. Samples in `voice_samples/`.
    Gotcha: embedding safetensors can corrupt during parallel downloads
    (anna, peter_yearsley, stuart_bell failed, fixed by re-download). Download
    serially. Also `--request-sequence` ignores voice_id/output_name keys -
    use per-voice CLI runs for distinct outputs.
- Assets at `/projects/AI/ChimpFlow/engine/` (binaries, `models/`, `server.json`,
  sample `out_pocket_*.wav`, `api_test.wav`). Server left running on :8080.

Conclusion: audio.cpp is a working Tier-1/Tier-2 candidate. The WebUI's
side-by-side Arena + OpenAI-compatible endpoint confirm both integration paths.
Next: try Kokoro + Supertonic GGUF the same way, then compare against
sherpa-onnx's native C++ API for the in-process engine.

## Open questions

- ZipVoice / Supertonic / KittenTTS license terms and voice quality vs Kokoro.
- sherpa-onnx PocketTTS feature parity with the Python version (streaming?
  all 7 languages? cloning from arbitrary wav?).
- Chatterbox worker: vLLM-Omni vs lightweight FastAPI wrapper — which is saner
  to deploy; how to disable/declare the PerTh watermark.
- Worker IPC: localhost HTTP (simple, language-agnostic) vs Unix-socket +
  length-prefixed PCM (lower overhead) — benchmark both.
- GPU contention policy when whisper.cpp (Vulkan/ROCm) and a GPU TTS engine
  share one card.

## Audio demo pages (listening test shortcuts)

Interactive demos (type text, hear audio):
- Kokoro-82M — https://huggingface.co/spaces/hexgrad/Kokoro-TTS
- Pocket TTS — https://kyutai.org/tts/ (voice catalog: https://huggingface.co/kyutai/tts-voices)
- Chatterbox — https://huggingface.co/spaces/ResembleAI/Chatterbox
  (multilingual v3: https://huggingface.co/spaces/ResembleAI/Chatterbox-Multilingual-TTS)
- Qwen3-TTS — https://huggingface.co/spaces/Qwen/Qwen3-TTS
- Supertonic — https://huggingface.co/spaces/Supertone/supertonic-3
- MOSS-TTS — HF Spaces: MOSS-TTS, MOSS-TTSD-v1.0, MOSS-VoiceGenerator
  (linked from https://github.com/OpenMOSS/MOSS-TTS)
- Fish Audio S2 — https://fish.audio/ (hosted playground; note: this demos their
  cloud model, the open S2 weights are the non-commercial sibling)

Static sample pages:
- Kokoro — https://huggingface.co/hexgrad/Kokoro-82M/blob/main/SAMPLES.md
- ZipVoice — https://zipvoice.github.io/
- Breeze TTS 2 — https://huggingface.co/BreezeBlue/Breeze-TTS-2 (samples on card)
- Pocket TTS tech report samples — https://kyutai.org/pocket-tts-technical-report/
- MOSS-TTS 1.5 user examples — reddit.com/r/LocalLLaMA/comments/1tuh5ko/
- KittenTTS — samples in repo: https://github.com/KittenML/KittenTTS

New finds while hunting demos (added to radar):
- **OmniVoice** (k2-fsa, 0.6B, zero-shot, claims 600+ languages) —
  https://huggingface.co/k2-fsa/OmniVoice
- **audio.cpp** (audio-cpp) — runs **Breeze TTS 2 as GGUF in C++, no Python**,
  ~6 GB VRAM, 4x real-time — https://huggingface.co/audio-cpp/audio.cpp-gguf
  (a second native-C++ path alongside sherpa-onnx; same deployment shape as
  whisper.cpp)

## Sources

- k2-fsa.github.io/sherpa/onnx/tts — sherpa-onnx TTS docs (engine list, C/C++ APIs)
- github.com/k2-fsa/sherpa-onnx — c-api.h
- github.com/kyutai-labs/pocket-tts — Pocket TTS (MIT, serve command, streaming)
- github.com/remsky/Kokoro-FastAPI — OpenAI-compatible Kokoro server
- rhapsode.dev/docs/protocol — multi-engine speech server, worker protocol
- docs.vllm.ai vLLM-Omni — TTS serving (Fish S2 Pro etc.)
- localaimaster.com — Best Local TTS Models 2026; Orpheus setup guide
- pinggy.io — Best Open Source Self-Hosted TTS Models 2026 (Aug 2026)
- DragonianVoice (SourceForge) — C++ ONNX TTS library
- r/LocalLLM, r/LocalTextToSpeech 2026 threads; HN Kokoro thread (Jul 2026)
