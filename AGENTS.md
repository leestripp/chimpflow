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

- `src/main.cpp` — all service code: auth, routes, engine proxy, bootstrap admin key.
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
- WebUI keeps the admin key in localStorage; keys are sha-hashed in sqlite.
- Frontend-format workflow gotcha (ComfyUI side): prompt text lives in BOTH
  `widgets_values[0]` and `widgets_values_named.value` — patch both.

## Conventions

- C++23, warning-clean under the project's flags; RAII, no raw new/delete.
- No new dependencies without asking; OmniNet path via `-DOMNINET_DIR=` (default
  `/projects/Websites/OmniNet`, prebuilt `libOmniNet.a` if present, else builds from source).
- Never commit: `build/`, `data/`, `*.wav`, `.daedalus/`, logs.
- Bootstrap admin key prints ONCE on fresh db (in chimpflow.out) — there is no recovery;
  without keys the only fix is deleting data/chimpflow.db (destructive).
