# ChimpEcho — parrot demo

Hold-to-talk echo app for ChimpFlow: press the button, say something, and the
service transcribes it (whisper.cpp) then speaks it back (PocketTTS).
gtkmm-4 UI, PipeWire mic/speaker, OmniNet HTTP client.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/chimp-echo
```

## Debugging

Everything the app does is logged to stderr (the launcher redirects it to
`/tmp/chimp-echo.log`). Set `CHIMPECHO_LOG=/path/to/file` to also append to a
file. Logged: config (key *source*, never the key), capture start/stop with
sample counts, every HTTP request with status/latency/bytes, STT text,
playback start/done, and all errors. If the app freezes, the last line in the
log pinpoints the phase.

The STT/TTS round trip runs on a worker thread with hard timeouts (3 s
connect / 45 s request), so a wedged service degrades to an error message
instead of freezing the UI.

Config via env: `CHIMPFLOW_URL` (default `http://127.0.0.1:8090`),
`CHIMPFLOW_KEY` (default: reads `~/.local/share/chimpflow/ADMIN_KEY`),
`CHIMPFLOW_VOICE` (default `alba` — the initially selected entry in the
voice dropdown). The key needs the `stt` + `tts` scopes.

The voice dropdown is populated at startup from `GET /v1/voices` (fetched
on a worker thread, so the window appears instantly). If the gateway is
unreachable or voice listing is unconfigured (503), the dropdown falls
back to the single `CHIMPFLOW_VOICE` entry.
