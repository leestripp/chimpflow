<!-- monkeybyte:type: cpp -->

# ChimpFlow — agent notes

Fully local voice service for Linux (no cloud): API-key auth + WebUI
(OmniNet/C++23) proxying audio.cpp (PocketTTS TTS) and whisper.cpp
(ggml-tiny.en STT) — all inference on localhost. Listens on
127.0.0.1:8090; TTS engine at 127.0.0.1:8080; whisper.cpp STT at
127.0.0.1:8081 (~/Apps/whisper.cpp, ggml-tiny.en, systemd user unit
chimpflow-whisper.service).

## Commands

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # configure (first time)
cmake --build build -j$(nproc)                    # build → build/chimpflow
bash install.sh                                   # user-level install (systemd --user, no root)
systemctl --user restart chimpflow                # restart the service
journalctl --user -u chimpflow -f                 # service logs
pgrep -x chimpflow | xargs -r kill                # stop a dev instance
setsid ./build/chimpflow > chimpflow.out 2>&1 < /dev/null &   # dev start detached
curl -s http://127.0.0.1:8090/healthz             # smoke test
ctest --test-dir build --output-on-failure      # unit + integration tests
ctest --test-dir build -L integration           # live-service suite (skips if down)
```

Installed layout (do not confuse with the repo): binary + `web/` in
`~/.local/lib/chimpflow/`, `data/` + `ADMIN_KEY` in `~/.local/share/chimpflow/`
(the service's WorkingDirectory). Boot-time start needs
`loginctl enable-linger $USER` (install.sh does it).

Run from the project root — `data/chimpflow.db` is created in the CWD.
`build/web` must point at `../web` (symlink) so the binary finds the WebUI
next to itself (web root resolves via /proc/self/exe).

## Layout

- `src/main.cpp` — service code: auth, routes, engine proxy, bootstrap master key.
- `src/audio_format.{h,cpp}` — bounded PCM16 WAV joiner + local WAV→MP3 ffmpeg adapter.
- `src/text_chunk.{h,cpp}` — bounded long-form paragraph/sentence/word splitting without cutting UTF-8.
  Agent-facing API guide is served at `/skill/` (markdown) and `/skill/manifest.json`
  (structured twin) — update `kSkillMd`/`kSkillManifest` in the anonymous namespace
  whenever routes change, so the guide never drifts from the code.
- `web/index.html` — self-contained admin SPA (vanilla JS, dark theme; see the
  global `webui-design` skill — select/option/button CSS is mandatory)
- `demo/` — ChimpEcho parrot app (standalone CMake: gtkmm-4 + PipeWire + OmniNet)
- `tests/` — zero-dep unit harness (`test_util.h`) + `integration.sh` live suite
- `docs/design.md` — architecture, API design, invariants (read before structural changes)
- `resources/` — static assets (chimp banner)
- `data/` — runtime sqlite db (gitignored)
- `Plan.md` — research history + DECISION (audio.cpp is the engine; all others rejected)

## Hard-won facts (don't relearn)

- PipeWire: `pw_thread_loop_stop()` MUST be called WITHOUT `pw_thread_loop_lock`
  held — it posts do_stop to the loop thread then `pthread_join()`s it; the loop
  thread can't process do_stop while the lock is held → deadlock (this froze
  ChimpEcho's UI on every record stop). Order: lock → `pw_stream_destroy` →
  unlock → stop. `pw_stream_destroy` itself is non-blocking and lock-safe. Also:
  no `SPA_AUDIO_POSITION_MONO` on PW 1.6 (it's `SPA_AUDIO_CHANNEL_MONO` in raw.h),
  `pw_stream_connect` needs `const spa_pod**` + a cast on the flags.
- OmniNet API: `StatusCode::SCREAMING_SNAKE_CASE`, `validateAndUse(key, ip)` →
  `.valid` / `.key` / `.error`, `Server` is `.start()` not `.run()`. The bundled
  helper is renamed `sendJson` (nlohmann `json` collides with OmniNet's).
- OmniNet bug FIXED here: `TokenUtils.cpp` validated keys by splitting at the
  FIRST `_`, but keys are `omn_sk_<hex>_<secret>` — every key failed its own
  validator. Fix = split at `rfind('_')`. **Push upstream to GitLab (leestrip/OmniNet).**
- Reused audio/state objects: re-arm one-shot latches in `start()` — ChimpEcho's
  `Playback::notified` fired once ever, so the 2nd+ echo played but never
  signaled done and the UI dead-locked (log showed `playback started` with no
  `playback done`). Repro pattern for such bugs: extract the struct into a
  tiny CLI harness (`/tmp/pw_repro*.cpp`), run under `timeout`, A/B the fix.
- WebUI keeps the master key in localStorage; keys are sha-hashed in sqlite.
- audio.cpp currently ignores `response_format: "mp3"` and returns WAV. ChimpFlow
  therefore asks for WAV and converts successful MP3 requests with ffmpeg. Use
  an explicit `-f mp3` output muxer: secure temporary files have no extension.
  The bootstrap/startup `std::cout` messages must be flushed (`std::endl`);
  otherwise systemd's non-TTY stdout buffering hides the one-time master key
  from the journal and `install.sh` cannot save it.
- Frontend-format workflow gotcha (ComfyUI side): prompt text lives in BOTH
  `widgets_values[0]` and `widgets_values_named.value` — patch both.
- Deployed-WebUI verification has three layers — check in order before believing
  a "still broken" report: (1) `curl` the served bytes, (2) render in a fresh
  browser and read the DOM, (3) `journalctl --user -u chimpflow | grep "succeeded
  - path"` to see whether the user's browser ever re-fetched. A browser held a
  pre-fix page for ~2h while every server-side check passed — the request log
  proved zero fetches from it.
- Single-file SPAs cache heuristically and hard: serve `index.html` with
  `Cache-Control: no-cache` and **bump the visible header badge on every WebUI
  change** so stale renders are self-diagnosing (badge is in the header next to
  the status dot).
- Status-page clarity: one card per engine, titled with the binary name
  (`audio.cpp`, `whisper.cpp`) + role + endpoint + loaded model file.
  Ambiguous headings ("Engine models") caused a multi-round confusion spiral —
  name things so a stranger knows exactly what's running on the system.
- PipeWire playback "done" must mean **PLAYED, not QUEUED**: signaling
  completion when the last sample is copied into a stream buffer makes
  stop() destroy the stream with the tail still queued -> randomly cut-off
  audio (ChimpEcho clipped TTS tails). Correct pattern: on queue-empty call
  pw_stream_flush(stream, true) and emit done from the stream events'
  `drained` callback.

## Conventions

- C++23, warning-clean under the project's flags; RAII, no raw new/delete.
- No new dependencies without asking. OmniNet is a fetched library via
  `FetchContent` at `main`; never modify its checkout. For disconnected builds,
  configure with `-DFETCHCONTENT_SOURCE_DIR_OMNINET=/path/to/clean/source`.
- Engine checkout paths default to `/projects/AI/ChimpFlow/engine` and
  `~/Apps/whisper.cpp`; override with `CHIMPFLOW_ENGINE_HOME` and
  `CHIMPFLOW_WHISPER_HOME` before running `install.sh` on another machine.
  When copying `engine/server.json`, rewrite its absolute model path too.
- Never commit: `build/`, `data/`, `*.wav`, `*.mp3`, `.daedalus/`, logs.
- Bootstrap master key prints ONCE on a fresh db to the journal; `install.sh`
  saves it as `ADMIN_KEY` mode 600. It cannot be revoked/deleted from the API.
  If it is lost, the only reset is replacing data/chimpflow.db (destructive).
