#!/usr/bin/env bash
# Integration smoke suite — needs a RUNNING ChimpFlow service.
# Behavioral failures exit 1; unreachable service or missing key SKIP (exit 0).
BASE="${CHIMPFLOW_URL:-http://127.0.0.1:8090}"
KEY="${CHIMPFLOW_KEY:-}"
[ -z "$KEY" ] && [ -f "$HOME/.local/share/chimpflow/ADMIN_KEY" ] && KEY=$(cat "$HOME/.local/share/chimpflow/ADMIN_KEY")

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'PASS  %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n' "$1"; }
expect() { # name expected actual
  if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (expected $2, got $3)"; fi
}

if ! curl -fsS -m 3 "$BASE/healthz" >/dev/null 2>&1; then
  echo "SKIP: $BASE unreachable — start the service to run integration tests"
  exit 0
fi
if [ -z "$KEY" ]; then echo "SKIP: no API key (set CHIMPFLOW_KEY)"; exit 0; fi
AUTH=(-H "Authorization: Bearer $KEY")
wav=$(mktemp /tmp/chimpflow-it-XXXX.wav)
mp3=$(mktemp /tmp/chimpflow-it-XXXX.mp3)
long_mp3=$(mktemp /tmp/chimpflow-it-long-XXXX.mp3)
long_headers=$(mktemp /tmp/chimpflow-it-headers-XXXX)
long_json=$(mktemp /tmp/chimpflow-it-long-XXXX.json)
default_wav=$(mktemp /tmp/chimpflow-it-default-XXXX.wav)
default_headers=$(mktemp /tmp/chimpflow-it-default-headers-XXXX)
id=""
cleanup() {
  rm -f "$wav" "$mp3" "$long_mp3" "$long_headers" "$long_json" "$default_wav" "$default_headers"
  if [ -n "$id" ]; then
    curl -s -X DELETE "${AUTH[@]}" "$BASE/admin/keys/$id" >/dev/null 2>&1 || true
    curl -s -X DELETE "${AUTH[@]}" "$BASE/admin/keys/$id/permanent" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

expect "GET /healthz" 200 "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/healthz")"
expect "no key -> 401" 401 "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/v1/audio/speech" -H 'Content-Type: application/json' -d '{}')"
expect "bad key -> 401" 401 "$(curl -s -o /dev/null -w '%{http_code}' -H 'Authorization: Bearer omn_sk_invalid_000000_deadbeef' "$BASE/v1/models")"
expect "GET /v1/models" 200 "$(curl -s -o /dev/null -w '%{http_code}' "${AUTH[@]}" "$BASE/v1/models")"
expect "model-scoped voices -> 200" 200 "$(curl -s -o /dev/null -w '%{http_code}' "${AUTH[@]}" "$BASE/v1/voices?model=pocket-tts")"
if curl -fsS "${AUTH[@]}" "$BASE/v1/voices?model=pocket-tts" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d.get("object") == "list" and d.get("data") and all(v.get("id") for v in d["data"])' 2>/dev/null
then ok "model-scoped voices use the ChimpFlow list shape"; else bad "model-scoped voice response malformed or empty"; fi
expect "legacy unscoped voices -> 200" 200 "$(curl -s -o /dev/null -w '%{http_code}' "${AUTH[@]}" "$BASE/v1/voices")"
expect "omitted model + unknown voice -> 200" 200 "$(curl -s -D "$default_headers" -o "$default_wav" -w '%{http_code}' -X POST "$BASE/v1/audio/speech" "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"input":"default model and voice fallback test","voice":"pixel_voice","response_format":"wav"}')"
default_model=$(awk '/^X-ChimpFlow-Model:/ {gsub("\r", "", $2); print $2}' "$default_headers")
fallback_voice=$(awk '/^X-ChimpFlow-Voice-Fallback:/ {gsub("\r", "", $2); print $2}' "$default_headers")
expect "omitted model defaults to Kokoro" "kokoro-82m" "$default_model"
expect "unknown voice uses fallback header" "pixel_voice" "$fallback_voice"
if head -c 4 "$default_wav" | grep -q RIFF; then ok "default/fallback body is WAV"; else bad "default/fallback body is not RIFF"; fi
expect "unsupported audio format -> 400" 400 "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/v1/audio/speech" "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"model":"pocket-tts","input":"bad format","voice":"alba","response_format":"flac"}')"

code=$(curl -s -o "$wav" -w '%{http_code}' -X POST "$BASE/v1/audio/speech" "${AUTH[@]}" \
  -H 'Content-Type: application/json' \
  -d '{"model":"pocket-tts","input":"integration test one two three","voice":"alba","response_format":"wav"}')
expect "TTS WAV -> 200" 200 "$code"
if head -c 4 "$wav" | grep -q RIFF; then ok "TTS body is WAV"; else bad "TTS body is not RIFF"; fi

mp3_type=$(curl -s -o "$mp3" -w '%{content_type}' -X POST "$BASE/v1/audio/speech" "${AUTH[@]}" \
  -H 'Content-Type: application/json' \
  -d '{"model":"pocket-tts","input":"MP3 integration test","voice":"alba","response_format":"mp3"}')
expect "TTS MP3 content type" "audio/mpeg" "$mp3_type"
if head -c 3 "$mp3" | grep -q ID3 || [ "$(od -An -tu1 -N1 "$mp3" | tr -d ' ')" = "255" ]; then
  ok "TTS body is MP3"
else
  bad "TTS body is not MP3"
fi
if command -v ffprobe >/dev/null 2>&1 && ffprobe -v error -show_entries format=format_name \
     -of default=noprint_wrappers=1:nokey=1 "$mp3" 2>/dev/null | grep -q mp3; then
  ok "ffprobe decodes MP3 response"
else
  bad "ffprobe cannot decode MP3 response"
fi

python3 - "$long_json" <<'PYLONG'
import json
import sys
paragraph = "This is a long-form chapter test. ChimpFlow splits the prose into sentences and joins the audio. "
text = "\n\n".join([paragraph * 12, "A second paragraph confirms that chapter boundaries are preserved. " * 12])
with open(sys.argv[1], "w") as output:
    json.dump({"model":"pocket-tts", "input":text, "voice":"alba", "response_format":"mp3"}, output)
PYLONG
long_code=$(curl -s -D "$long_headers" -o "$long_mp3" -w '%{http_code}' -X POST "$BASE/v1/audio/speech" "${AUTH[@]}" \
  -H 'Content-Type: application/json' --data-binary @"$long_json")
expect "long-form TTS -> 200" 200 "$long_code"
chunks=$(awk '/^X-ChimpFlow-Text-Chunks:/ {gsub("\r", "", $2); print $2}' "$long_headers")
if [ "${chunks:-0}" -gt 1 ] 2>/dev/null; then ok "long-form TTS auto-split into $chunks chunks"; else bad "long-form TTS did not report multiple chunks"; fi
if command -v ffprobe >/dev/null 2>&1 && ffprobe -v error -show_entries format=format_name \
     -of default=noprint_wrappers=1:nokey=1 "$long_mp3" 2>/dev/null | grep -q mp3; then
  ok "joined long-form MP3 decodes"
else
  bad "joined long-form MP3 cannot be decoded"
fi
expect "empty TTS input -> 400" 400 "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/v1/audio/speech" "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"model":"pocket-tts","input":"   ","voice":"alba","response_format":"mp3"}')"
python3 - "$long_json" <<'PYLONG'
import json
import sys
with open(sys.argv[1], "w") as output:
    json.dump({"model":"pocket-tts", "input":"x"*131073, "voice":"alba", "response_format":"mp3"}, output)
PYLONG
expect "over-limit TTS input -> 413" 413 "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/v1/audio/speech" "${AUTH[@]}" -H 'Content-Type: application/json' --data-binary @"$long_json")"

resp=$(curl -s -X POST "$BASE/v1/audio/transcriptions" "${AUTH[@]}" -F file=@"$wav" -F model=whisper)
if echo "$resp" | grep -qi "integration test"; then ok "STT round trip: $resp"
else bad "STT round trip got: $resp"; fi

stt=$(curl -s "$BASE/admin/engine/stt" "${AUTH[@]}")
if echo "$stt" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d.get("reachable") is True and "whisper" in d.get("engine","")' 2>/dev/null
then ok "STT admin status: $stt"; else bad "STT admin status: $stt"; fi

expect "GET /skill/manifest.json" 200 "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/skill/manifest.json")"
if curl -fsS "$BASE/skill/manifest.json" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d.get("recommended_audio_format") == "mp3"; assert isinstance(d.get("endpoints"), list) and len(d["endpoints"]) >= 8' 2>/dev/null
then ok "skill manifest lists endpoints and recommends MP3"; else bad "skill manifest malformed"; fi
if curl -fsS "$BASE/skill/" | grep -q "Prefer MP3"; then ok "skill markdown recommends MP3"; else bad "skill markdown missing MP3 guidance"; fi

expect "agent keys cannot request admin scope" 403 "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/admin/keys" "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"name":"it-admin-test","scopes":["admin"]}')"
raw=$(curl -s -X POST "$BASE/admin/keys" "${AUTH[@]}" -H 'Content-Type: application/json' \
  -d "{\"name\":\"it-lifecycle-test-$$\",\"scopes\":[\"tts\"]}")
id=$(echo "$raw"  | python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])')
tkey=$(echo "$raw" | python3 -c 'import json,sys; print(json.load(sys.stdin)["rawKey"])')
expect "tts-only key on STT -> 403" 403 "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/v1/audio/transcriptions" -H "Authorization: Bearer $tkey" -F file=@"$wav" -F model=whisper)"
expect "active key permanent delete -> 409" 409 "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "${AUTH[@]}" "$BASE/admin/keys/$id/permanent")"
expect "restore active key -> 409" 409 "$(curl -s -o /dev/null -w '%{http_code}' -X POST "${AUTH[@]}" "$BASE/admin/keys/$id/restore")"
expect "revoke agent key -> 200" 200 "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "${AUTH[@]}" "$BASE/admin/keys/$id")"
expect "revoked key -> 401" 401 "$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $tkey" "$BASE/v1/models")"
expect "restore revoked key -> 200" 200 "$(curl -s -o /dev/null -w '%{http_code}' -X POST "${AUTH[@]}" "$BASE/admin/keys/$id/restore")"
expect "restored key -> 200" 200 "$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $tkey" "$BASE/v1/models")"
expect "re-revoke before delete -> 200" 200 "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "${AUTH[@]}" "$BASE/admin/keys/$id")"
expect "revoked key permanent delete -> 200" 200 "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "${AUTH[@]}" "$BASE/admin/keys/$id/permanent")"
if curl -fsS "${AUTH[@]}" "$BASE/admin/keys" | python3 -c 'import json,sys; kid=sys.argv[1]; assert all(k["id"] != kid for k in json.load(sys.stdin))' "$id"
then ok "permanently deleted key is absent from list"; else bad "permanently deleted key still listed"; fi
id=""

master_id=$(curl -fsS "${AUTH[@]}" "$BASE/admin/keys" | python3 -c 'import json,sys; print(next(k["id"] for k in json.load(sys.stdin) if k.get("master")))' 2>/dev/null || true)
if [ -n "$master_id" ]; then
  ok "master key is marked protected in list"
  expect "master revoke is blocked" 409 "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "${AUTH[@]}" "$BASE/admin/keys/$master_id")"
  expect "master permanent delete is blocked" 409 "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "${AUTH[@]}" "$BASE/admin/keys/$master_id/permanent")"
else
  bad "master key missing from list"
fi

page=$(curl -fsS "$BASE/")
for needle in 'data-delete' 'data-restore' 'navigator.clipboard.writeText' 'document.execCommand("copy")' \
              'Copy new secret key' 'Copy instructions' 'PREFER MP3' 'MP3 (recommended)' '/permanent' \
              'Accepts long-form text' 'run long-form synthesis as a background process' \
              'id="ttsModel"' 'loadModels()' '/v1/voices?model=' 'model, input:' \
              'chatterbox' 'video voiceovers' 'v0.3.4'; do
  if printf '%s' "$page" | grep -qF "$needle"; then ok "WebUI control: $needle"
  else bad "WebUI control missing: $needle"; fi
done
if printf '%s' "$page" | grep -qE 'data-copy-id|Copy ID'; then bad "Copy ID is absent"; else ok "Copy ID is absent"; fi

echo "---"
echo "integration: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
