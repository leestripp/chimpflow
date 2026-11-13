// ChimpFlow - OmniNet voice service: API-key auth + local TTS/STT gateway
#include "audio_format.h"
#include "text_chunk.h"

#include "OmniNet.h"
#include "http/HttpClient.h"
#include "token/ApiKeyStore.h"
#include "db/Database.h"
#include "http/StaticHandler.h"
#include "utils/Utils.h"
#include "token/ApiKey.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <system_error>
#include <vector>

#include <iostream>
#include <string>
#include <string_view>

using namespace OmniNet;
using namespace OmniNet::HTTP;

namespace {

// Defaults for the CHIMPFLOW_* environment variables (see chimpflow.env,
// loaded via systemd EnvironmentFile= — install.sh creates the file).
// Engine endpoint (audio.cpp TTS server)
constexpr std::string_view kEngineBase = "http://127.0.0.1:8080";
// Transcription endpoint (whisper.cpp server; see chimpflow-whisper.service)
constexpr std::string_view kSttBase = "http://127.0.0.1:8081";
// Model the companion whisper service serves. whisper-server has no
// model-listing endpoint, so the status route reports this value — the
// whisper unit reads the same CHIMPFLOW_STT_MODEL var (single source).
constexpr std::string_view kSttModel = "ggml-tiny.en";
constexpr std::string_view kDefaultTtsModel = "kokoro-82m";
constexpr std::string_view kFfmpeg = "/usr/bin/ffmpeg";
constexpr std::string_view kMasterKeyName = "chimpflow-bootstrap-admin";
constexpr std::size_t kMaxLongFormBytes = 128 * 1024;
constexpr std::size_t kMaxTextChunks = 512;

// Environment override: empty/unset falls back to the compiled default.
std::string envOr(const char* name, std::string_view fallback) {
    if (const char* v = std::getenv(name); v && *v != '\0') return v;
    return std::string(fallback);
}


// Locate the web/ dir next to the executable (independent of CWD)
std::string webRoot() {
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) return (exe.parent_path() / "web").string();
    return "web";
}

// Extract Bearer token from Authorization header
std::string bearerToken(const Request& req) {
    auto auth = req.authHeader();
    if (auth.substr(0, 7) == "Bearer ") return std::string(auth.substr(7));
    return {};
}

void sendJson(Response& resp, StatusCode code, std::string_view body) {
    resp.setStatus(code);
    resp.setContentType("application/json");
    resp.setBody(body);
}

bool isMasterKey(const Token::ApiKey& key) {
    return key.name == kMasterKeyName && key.hasScope("admin");
}

// OmniNet has revoke but no restore operation. Keep the inverse lifecycle in
// ChimpFlow's own code; RETURNING makes the conditional update atomic.
bool restoreKey(DB::Database& db, std::string_view keyId) {
    auto stmt = db.prepare(
        "UPDATE api_keys SET revoked = 0, revoked_at = NULL "
        "WHERE id = ? AND revoked = 1 RETURNING id");
    stmt.bind(1, keyId);
    return stmt.step();
}

// A missing catalog must not make synthesis unavailable: in that case the
// caller forwards the original voice and lets the engine report any error.
std::optional<std::vector<std::string>> engineVoiceIds(
    std::string_view engineBase, std::string_view modelId) {
    ClientRequest fwd;
    fwd.method("GET").url(std::string(engineBase) + "/v1/audio/voices?model=" + Utils::urlEncode(modelId));
    auto r = Client::get_instance().send_blocking(fwd);
    if (!r || r->status() != StatusCode::OK) return std::nullopt;
    const auto body = nlohmann::json::parse(r->bodyAsStringView(), nullptr, false);
    if (body.is_discarded() || !body.contains("voices") || !body["voices"].is_array())
        return std::nullopt;
    std::vector<std::string> ids;
    for (const auto& voice : body["voices"]) {
        if (!voice.is_string()) return std::nullopt;
        ids.push_back(voice.get<std::string>());
    }
    return ids;
}

// Agent-facing guide to this API, served at /skill/ (see README "Skill endpoint").
// Written for an AI agent reader: task-oriented, exact request shapes, honest errors.
constexpr std::string_view kSkillMd = R"CHIMP(---
name: chimpflow-tts
description: Local text-to-speech AND speech-to-text for AI agents on this machine. Use whenever a task needs spoken audio (prefer MP3) or audio transcription. Runs on localhost, no cloud, OpenAI-compatible endpoints.
version: 0.3.4
service: chimpflow
base_url: http://127.0.0.1:8090
---

# ChimpFlow — local TTS + STT gateway for agents

ChimpFlow checks scoped API keys and proxies text-to-speech to audio.cpp and
transcription to whisper.cpp. Audio and text stay on this machine.

## Authentication

Every route except `/healthz`, `/skill/`, and the WebUI assets requires:

    Authorization: Bearer omn_sk_...

Agent keys carry `tts` (speech) and/or `stt` (transcription). The human's
bootstrap master key also has `admin` for key management. Ask the operator for
an agent key; never request or use the master key for ordinary audio work.
Any valid key can call `/v1/models` and `/v1/voices?model=<model-id>`.

## Text to speech — POST /v1/audio/speech (scope: tts)

**Prefer MP3.** It is much smaller than WAV and is the right default for agent
voice output. Use WAV only when a consumer specifically needs uncompressed PCM.

    curl -fsS -X POST http://127.0.0.1:8090/v1/audio/speech \
      -H "Authorization: Bearer $CHIMPFLOW_KEY" \
      -H "Content-Type: application/json" \
      -d '{"model":"kokoro-82m","input":"Hello world","voice":"af_heart","response_format":"mp3"}' \
      -o speech.mp3

## Choose a TTS model

Discover the models actually configured on this host with `GET /v1/models`.
- `kokoro-82m`: default for everyday agent speech and small/phone-oriented deployments. The BF16 model file is about 212 MB.
- `chatterbox`: specialist model for high-end narration, video voiceovers, and other quality-critical work when present on the host. Its Q8 model file is about 2.09 GB—roughly ten times Kokoro's file size—and it needs substantially more RAM/VRAM and compute. It supports reference-WAV voice cloning; use a voice id returned for this model.
- `pocket-tts`: legacy compatibility only; not recommended for new work because of intermittent garbled output.

The model file size is not the total runtime memory requirement. Do not make Chatterbox the default on memory-constrained devices.

Request fields:

- `model`: speech model id discovered from `GET /v1/models`; omitted/empty defaults to `kokoro-82m`. Explicit model ids are always honored.
- `input`: text to speak.
- `voice`: id discovered from `GET /v1/voices?model=<model-id>`. If the id is unavailable for the selected model, ChimpFlow uses that model's default voice instead of failing.
- `response_format`: `mp3` (recommended) or `wav`.

MP3 returns `Content-Type: audio/mpeg`; WAV returns `audio/wav`. Successful
responses include `X-ChimpFlow-Model` with the effective model. When an unknown
voice was replaced by the model default, `X-ChimpFlow-Voice-Fallback` contains
the URL-encoded originally requested voice id. Unsupported formats return 400. ChimpFlow
performs MP3 encoding locally with ffmpeg. Extra engine-specific fields pass through.

### Short speech vs. a whole chapter

Use the SAME `/v1/audio/speech` endpoint for both. ChimpFlow automatically
splits valid text input at paragraph/sentence/word boundaries (about 1200 bytes
per engine request), synthesizes sequentially, joins the PCM, and returns ONE
complete WAV or MP3. It never returns a playlist or partial chapter. The
`X-ChimpFlow-Text-Chunks` response header reports the number of joined chunks.

- **Need speech back immediately / incremental playback?** Split sentences in
  your own client and make ordinary foreground requests as each is needed.
- **Want one long-form audio file?** Send the whole chapter in one request and
  **run long-form synthesis as a background process**. Its automatic completion
  trigger tells you when the file is ready; do not poll a job endpoint (there
  isn't one). Use a generous HTTP client timeout because the response arrives
  only after all chunks and encoding finish.

Long-form input is limited to 131072 UTF-8 bytes and 512 chunks (413 when
exceeded). Joined WAV is capped at 256 MiB and MP3 at 64 MiB. Existing short
clients use exactly the same request and response shapes; no client migration
or separate long-form endpoint is needed.

## Speech to text — POST /v1/audio/transcriptions (scope: stt)

    curl -fsS -X POST http://127.0.0.1:8090/v1/audio/transcriptions \
      -H "Authorization: Bearer $CHIMPFLOW_KEY" \
      -F file=@speech.mp3 \
      -F model=whisper

Returns JSON such as `{"text":"..."}`. Multipart accepts formats whisper.cpp
supports (WAV, MP3, OGG, and others); 16 kHz mono WAV is safest. Keep uploads
below the gateway's 24 MB request limit.

## Discovery

    curl -fsS -H "Authorization: Bearer $CHIMPFLOW_KEY" \
      http://127.0.0.1:8090/v1/models
    curl -fsS -H "Authorization: Bearer $CHIMPFLOW_KEY" \
      http://127.0.0.1:8090/v1/voices?model=kokoro-82m

Discover model and voice ids at runtime instead of hardcoding them. Request voices
with the selected model id. The legacy unscoped `/v1/voices` WAV catalog returns
503 if `CHIMPFLOW_VOICES_DIR` is not configured.

## Health and errors

    curl -fsS http://127.0.0.1:8090/healthz

Errors are JSON: `{"error":"human-readable message"}`.

- 400: invalid request or unsupported response format; correct the request.
- 401: missing, revoked, expired, or invalid key; ask the operator for a key.
- 403: valid key without the required scope; use a key with `tts` or `stt`.
- 413: long-form input exceeds the documented byte/chunk limit.
- 502: companion engine, WAV assembly, or local MP3 encoder failed; tell the
  operator and do not retry-loop.
- 503: voice catalog is not configured.

## Agent integration checklist

1. Store the key in `CHIMPFLOW_KEY` or a secrets manager, never source control.
2. Use base URL `http://127.0.0.1:8090` on the machine where ChimpFlow runs.
   Do not expose the service or send its keys/text to a cloud service.
3. Check `/healthz`, discover `/v1/models`, then query `/v1/voices?model=<model-id>`.
4. For speech, request `response_format: "mp3"` unless WAV is required.
5. For immediate speech, self-split and use foreground requests. For one chapter
   file, post the whole text as a background process and wait for its automatic
   completion trigger rather than polling.
6. Keep the key out of logs. A generated raw key is shown only once.

## Master-only key administration (scope: admin)

The WebUI uses the bootstrap master key. Agent keys cannot include `admin`.

- `GET /admin/keys` — list ids, names, scopes, usage, master/revoked/expired state.
- `POST /admin/keys` with `{"name":"agent-name","scopes":["tts","stt"]}` —
  create an agent key; `rawKey` is returned once. Scopes may be `tts` and/or
  `stt` only; omitting scopes grants both.
- `DELETE /admin/keys/<id>` — revoke an active key.
- `POST /admin/keys/<id>/restore` — restore a revoked key.
- `DELETE /admin/keys/<id>/permanent` — permanently delete a revoked key;
  active keys return 409. The bootstrap master key cannot be revoked or deleted.
- `GET /admin/engine/models` — audio.cpp model list.
- `GET /admin/engine/stt` — whisper.cpp reachability and configured model.
)CHIMP";

// Same guide as structured data, for agent frameworks that prefer JSON.
constexpr std::string_view kSkillManifest = R"CHIMP({
  "name": "chimpflow-tts",
  "description": "Local authenticated text-to-speech (prefer MP3) and speech-to-text for agents; no cloud.",
  "version": "0.3.4",
  "base_url": "http://127.0.0.1:8090",
  "auth": {"type": "bearer", "header": "Authorization", "prefix": "Bearer ", "agent_scopes": ["tts", "stt"], "master_scope": "admin"},
  "recommended_audio_format": "mp3",
  "long_form_tts": {"endpoint": "/v1/audio/speech", "automatic": true, "max_input_bytes": 131072, "target_chunk_bytes": 1200, "max_chunks": 512, "response": "one complete WAV or MP3", "chunk_count_header": "X-ChimpFlow-Text-Chunks", "execution": "background process with automatic completion trigger"},
  "endpoints": [
    {"method": "GET", "path": "/healthz", "auth": "none"},
    {"method": "GET", "path": "/skill/", "auth": "none", "returns": "agent guide markdown"},
    {"method": "GET", "path": "/skill/manifest.json", "auth": "none"},
    {"method": "GET", "path": "/v1/models", "auth": "bearer", "notes": "any valid key"},
    {"method": "GET", "path": "/v1/voices?model=<model-id>", "auth": "bearer", "notes": "any valid key; voices for the selected model"},
    {"method": "POST", "path": "/v1/audio/speech", "auth": "bearer", "scope": "tts", "content_type": "application/json", "body": {"model": "kokoro-82m", "input": "text", "voice": "af_heart", "response_format": "mp3"}, "response_formats": {"mp3": "audio/mpeg (recommended)", "wav": "audio/wav"}, "notes": "automatic paragraph/sentence splitting and joined long-form output; use a background process for chapters"},
    {"method": "POST", "path": "/v1/audio/transcriptions", "auth": "bearer", "scope": "stt", "content_type": "multipart/form-data", "body": {"file": "@speech.mp3", "model": "whisper"}, "returns": "JSON transcription"},
    {"method": "GET", "path": "/admin/keys", "auth": "bearer", "scope": "admin"},
    {"method": "POST", "path": "/admin/keys", "auth": "bearer", "scope": "admin", "body": {"name": "agent-name", "scopes": ["tts", "stt"]}, "notes": "agent scopes only; rawKey returned once; scopes default to tts+stt"},
    {"method": "DELETE", "path": "/admin/keys/{id}", "auth": "bearer", "scope": "admin", "notes": "revoke; bootstrap master is protected"},
    {"method": "POST", "path": "/admin/keys/{id}/restore", "auth": "bearer", "scope": "admin", "notes": "restore a revoked key"},
    {"method": "DELETE", "path": "/admin/keys/{id}/permanent", "auth": "bearer", "scope": "admin", "notes": "revoked keys only; active 409; bootstrap master protected"},
    {"method": "GET", "path": "/admin/engine/models", "auth": "bearer", "scope": "admin"},
    {"method": "GET", "path": "/admin/engine/stt", "auth": "bearer", "scope": "admin"}
  ],
  "errors": {"shape": {"error": "message"}, "400": "invalid request/format", "413": "long-form input too large", "401": "missing/invalid/revoked key", "403": "missing scope", "502": "engine or MP3 encoder failed", "503": "voice catalog unconfigured"}
})CHIMP";

}  // namespace

int main() {
    OmniNet::init();
    Logger::instance().setLevel(LogLevel::INFO);

    // --- Database + API key store ---
    // Data dir lives next to the working directory (created if missing)
    std::filesystem::create_directories("data");
    DB::Database db;
    db.open("data/chimpflow.db");
    Token::ApiKeyStore keys(db);   // namespace TBD by real header
    keys.initialize();

    // Bootstrap: create admin key if none exist for "admin"
    auto existing = keys.listForUser("admin");
    if (existing.empty()) {
        auto res = keys.create("admin", "chimpflow-bootstrap-admin",
                               {"admin", "tts", "stt"});
        std::cout << "\n=== CHIMPFLOW BOOTSTRAP ADMIN KEY (shown once) ===\n"
                  << res.rawKey << "\n===================================================" << std::endl;
    }

    // --- HTTP server ---
    // --- Config: CHIMPFLOW_* env overrides with localhost defaults ---
    const std::string cfgListenHost = envOr("CHIMPFLOW_LISTEN_HOST", "127.0.0.1");
    const unsigned short cfgListenPort = static_cast<unsigned short>(
        std::stoul(envOr("CHIMPFLOW_LISTEN_PORT", "8090")));
    const std::string cfgEngineBase = envOr("CHIMPFLOW_ENGINE_URL", kEngineBase);
    const std::string cfgSttBase    = envOr("CHIMPFLOW_STT_URL", kSttBase);
    const std::string cfgSttModel   = envOr("CHIMPFLOW_STT_MODEL", kSttModel);
    const std::string cfgFfmpeg     = envOr("CHIMPFLOW_FFMPEG", kFfmpeg);
    // Legacy unscoped voice catalog: dir of engine voice samples (*.wav).
    // Model-scoped /v1/voices?model=<id> requests use the engine catalog.
    // Empty/unset -> unscoped /v1/voices answers 503 with a configuration hint.
    const std::string cfgVoicesDir  = envOr("CHIMPFLOW_VOICES_DIR", "");
    std::cout << "[chimpflow] listen=" << cfgListenHost << ":" << cfgListenPort
              << " engine=" << cfgEngineBase
              << " stt=" << cfgSttBase << " sttModel=" << cfgSttModel
              << " ffmpeg=" << cfgFfmpeg
              << " voicesDir=" << (cfgVoicesDir.empty() ? "<unset>" : cfgVoicesDir) << std::endl;

    Server::Config serverConfig;
    serverConfig.address = cfgListenHost;  // localhost service; set 0.0.0.0 only if exposing to LAN
    serverConfig.port = cfgListenPort;
    // Clients upload up to ~120 s of audio for STT; worst case (44.1 kHz
    // stereo 16-bit WAV) is ~21 MB. OmniNet's defaults (1 MB request cap,
    // 16 MB per-socket buffer cap) drop those mid-upload, and the socket cap
    // must exceed the largest request/response we move. Both are externally
    // configurable since OmniNet v1.0.5 (maxBufferSize landed via our
    // bug report; see ./omninet_bugs.md).
    serverConfig.maxRequestSize = 24 * 1024 * 1024;  // 24 MB
    serverConfig.maxBufferSize  = 32 * 1024 * 1024;  // 32 MB per-socket cap
    Server server(serverConfig);

    // Health (open)
    server.get("/healthz", [](const Request&, Response& resp) {
        sendJson(resp, StatusCode::OK, R"({"status":"ok","service":"chimpflow"})");
    });

    // Agent skill doc: how an AI agent should use this API (open, like /healthz).
    // Human-readable markdown at /skill/ and /skill; machine-readable twin at
    // /skill/manifest.json for agent frameworks that prefer structured data.
    auto serveSkill = [](const Request&, Response& resp) {
        resp.setStatus(StatusCode::OK);
        resp.setContentType("text/markdown; charset=utf-8");
        resp.setBody(kSkillMd);
    };
    server.get("/skill", serveSkill);
    server.get("/skill/", serveSkill);
    server.get("/skill/manifest.json", [](const Request&, Response& resp) {
        sendJson(resp, StatusCode::OK, kSkillManifest);
    });

    // TTS: split long-form prose into bounded engine requests, join the PCM,
    // then return one WAV or locally encoded MP3. Non-JSON/legacy requests keep
    // the original single-request proxy behavior.
    server.post("/v1/audio/speech", [&keys, &cfgEngineBase, &cfgFfmpeg](const Request& req, Response& resp) {
        auto tok = bearerToken(req);
        if (tok.empty()) return sendJson(resp, StatusCode::UNAUTHORIZED,
            R"({"error":"missing Authorization: Bearer <api_key>"})");
        auto v = keys.validateAndUse(tok, req.headerOr("X-Forwarded-For", "127.0.0.1"));
        if (!v.valid) {
            nlohmann::json err = {{"error", "invalid api key: " + v.error}};
            return sendJson(resp, StatusCode::UNAUTHORIZED, err.dump());
        }
        if (!v.key->hasScope("tts"))
            return sendJson(resp, StatusCode::FORBIDDEN, R"({"error":"key lacks 'tts' scope"})");

        std::string engineBody(req.bodyAsString());
        bool wantsMp3 = false;
        const auto requestJson = nlohmann::json::parse(engineBody, nullptr, false);
        if (!requestJson.is_discarded() && requestJson.is_object()) {
            if (requestJson.contains("response_format")) {
                if (!requestJson["response_format"].is_string()) {
                    return sendJson(resp, StatusCode::BAD_REQUEST,
                        R"({"error":"response_format must be 'mp3' or 'wav'"})");
                }
                const auto format = requestJson["response_format"].get<std::string>();
                if (format != "mp3" && format != "wav") {
                    return sendJson(resp, StatusCode::BAD_REQUEST,
                        R"({"error":"unsupported response_format; use 'mp3' or 'wav'"})");
                }
                wantsMp3 = format == "mp3";
            }
        }

        auto engineJson = requestJson;
        std::optional<std::string> voiceFallback;
        if (engineJson.is_object()) {
            if (!engineJson.contains("model") ||
                (engineJson["model"].is_string() && engineJson["model"].get<std::string>().empty())) {
                engineJson["model"] = kDefaultTtsModel;
            }
            if (engineJson["model"].is_string() && engineJson.contains("voice") &&
                engineJson["voice"].is_string()) {
                const auto model = engineJson["model"].get<std::string>();
                const auto requestedVoice = engineJson["voice"].get<std::string>();
                if (!requestedVoice.empty()) {
                    const auto voices = engineVoiceIds(cfgEngineBase, model);
                    if (voices && std::ranges::find(*voices, requestedVoice) == voices->end()) {
                        std::cerr << "[chimpflow] Voice '" << Utils::urlEncode(requestedVoice)
                                  << "' is unavailable for model '" << model
                                  << "'; using the model default voice\n";
                        voiceFallback = requestedVoice;
                        engineJson.erase("voice");
                    }
                }
            }
        }
        const std::string effectiveModel = engineJson.is_object() && engineJson["model"].is_string()
            ? engineJson["model"].get<std::string>() : std::string();

        ClientConfig engineClientConfig;
        engineClientConfig.request_timeout = std::chrono::seconds{180};

        auto sendAudio = [&](std::span<const std::uint8_t> wav) {
            if (!effectiveModel.empty()) resp.setHeader("X-ChimpFlow-Model", effectiveModel);
            if (voiceFallback) resp.setHeader("X-ChimpFlow-Voice-Fallback", Utils::urlEncode(*voiceFallback));
            if (wantsMp3) {
                try {
                    auto mp3 = chimpflow::encodeMp3(wav, cfgFfmpeg);
                    resp.setStatus(StatusCode::OK);
                    resp.setContentType("audio/mpeg");
                    resp.setBody(std::move(mp3));
                    return;
                } catch (const std::exception& e) {
                    std::cerr << "[chimpflow] MP3 encoding failed: " << e.what() << '\n';
                    return sendJson(resp, StatusCode::BAD_GATEWAY,
                        R"({"error":"local MP3 encoding failed; check CHIMPFLOW_FFMPEG"})");
                }
            }
            resp.setStatus(StatusCode::OK);
            resp.setContentType("audio/wav");
            resp.setBody(wav);
        };

        const bool hasText = !requestJson.is_discarded() && requestJson.is_object() &&
                             requestJson.contains("input") && requestJson["input"].is_string();
        if (!hasText) {
            if (engineJson.is_object()) engineBody = engineJson.dump();
            ClientRequest fwd;
            fwd.method("POST").url(cfgEngineBase + "/v1/audio/speech")
               .header("Content-Type", "application/json").body(engineBody);
            auto r = Client::get_instance().send_blocking(fwd, engineClientConfig);
            if (!r) return sendJson(resp, StatusCode::BAD_GATEWAY, R"({"error":"engine unreachable"})");
            auto& er = r.value();
            if (er.status() == StatusCode::OK) return sendAudio(er.body());
            resp.setStatus(er.status());
            resp.setContentType(er.header("Content-Type").empty() ? "application/json" : er.header("Content-Type"));
            resp.setBody(er.body());
            return;
        }

        const auto input = requestJson["input"].get<std::string>();
        if (input.size() > kMaxLongFormBytes) {
            return sendJson(resp, StatusCode::PAYLOAD_TOO_LARGE,
                R"({"error":"long-form input exceeds the 131072-byte chapter limit"})");
        }
        const auto chunks = chimpflow::splitLongForm(input);
        if (chunks.empty()) {
            return sendJson(resp, StatusCode::BAD_REQUEST, R"({"error":"input must contain text"})");
        }
        if (chunks.size() > kMaxTextChunks) {
            return sendJson(resp, StatusCode::PAYLOAD_TOO_LARGE,
                R"({"error":"long-form input requires too many speech chunks"})");
        }

        engineJson["response_format"] = "wav";
        chimpflow::WavJoiner joiner;
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            engineJson["input"] = chunks[i];
            ClientRequest fwd;
            fwd.method("POST").url(cfgEngineBase + "/v1/audio/speech")
               .header("Content-Type", "application/json").body(engineJson.dump());
            auto r = Client::get_instance().send_blocking(fwd, engineClientConfig);
            if (!r) {
                std::cerr << "[chimpflow] TTS engine unreachable on chunk " << (i + 1)
                          << '/' << chunks.size() << '\n';
                return sendJson(resp, StatusCode::BAD_GATEWAY, R"({"error":"engine unreachable during long-form synthesis"})");
            }
            auto& er = r.value();
            if (er.status() != StatusCode::OK) {
                std::cerr << "[chimpflow] TTS engine failed on chunk " << (i + 1)
                          << '/' << chunks.size() << " with HTTP " << static_cast<int>(er.status()) << '\n';
                resp.setStatus(er.status());
                resp.setContentType(er.header("Content-Type").empty() ? "application/json" : er.header("Content-Type"));
                resp.setBody(er.body());
                return;
            }
            try {
                joiner.append(er.body());
            } catch (const std::exception& e) {
                std::cerr << "[chimpflow] WAV assembly failed on chunk " << (i + 1)
                          << '/' << chunks.size() << ": " << e.what() << '\n';
                return sendJson(resp, StatusCode::BAD_GATEWAY,
                    R"({"error":"engine returned invalid WAV audio during long-form synthesis"})");
            }
        }

        try {
            const auto wav = joiner.finish();
            resp.setHeader("X-ChimpFlow-Text-Chunks", std::to_string(chunks.size()));
            return sendAudio(wav);
        } catch (const std::exception& e) {
            std::cerr << "[chimpflow] WAV assembly failed: " << e.what() << '\n';
            return sendJson(resp, StatusCode::BAD_GATEWAY,
                R"({"error":"could not assemble long-form WAV audio"})");
        }
    });

    // STT: POST /v1/audio/transcriptions (OpenAI-compatible, proxied to whisper.cpp)
    // The multipart body is forwarded byte-for-byte — ChimpFlow never parses it,
    // so it stays a pure auth gateway (10MB OmniNet body limit applies).
    server.post("/v1/audio/transcriptions", [&keys, &cfgSttBase](const Request& req, Response& resp) {
        auto tok = bearerToken(req);
        if (tok.empty()) return sendJson(resp, StatusCode::UNAUTHORIZED,
            R"({"error":"missing Authorization: Bearer <api_key>"})");
        auto v = keys.validateAndUse(tok, req.headerOr("X-Forwarded-For", "127.0.0.1"));
        if (!v.valid) {
            nlohmann::json err = {{"error", "invalid api key: " + v.error}};
            return sendJson(resp, StatusCode::UNAUTHORIZED, err.dump());
        }
        if (!v.key->hasScope("stt"))
            return sendJson(resp, StatusCode::FORBIDDEN, R"({"error":"key lacks 'stt' scope"})");

        ClientRequest fwd;
        fwd.method("POST").url(cfgSttBase + "/inference")
           .header("Content-Type", req.headerOr("Content-Type", "multipart/form-data"))
           .body(req.bodyAsString());
        auto r = Client::get_instance().send_blocking(fwd);
        if (!r) {
            return sendJson(resp, StatusCode::BAD_GATEWAY,
                R"({"error":"transcription engine unreachable"})");
        }
        resp.setStatus(r->status());
        resp.setContentType(r->header("Content-Type").empty()
                            ? "application/json" : r->header("Content-Type"));
        resp.setBody(r->body());
    });

    // Model discovery: any valid key lists engine models (model/voice values
    // for the speech route). Deliberately not admin-gated.
    server.get("/v1/models", [&keys, &cfgEngineBase](const Request& req, Response& resp) {
        auto tok = bearerToken(req);
        if (tok.empty()) return sendJson(resp, StatusCode::UNAUTHORIZED,
            R"({"error":"missing Authorization: Bearer <api_key>"})");
        auto v = keys.validateAndUse(tok, req.headerOr("X-Forwarded-For", "127.0.0.1"));
        if (!v.valid) {
            nlohmann::json err = {{"error", "invalid api key: " + v.error}};
            return sendJson(resp, StatusCode::UNAUTHORIZED, err.dump());
        }
        ClientRequest fwd;
        fwd.method("GET").url(cfgEngineBase + "/v1/models");
        auto r = Client::get_instance().send_blocking(fwd);
        if (!r) return sendJson(resp, StatusCode::BAD_GATEWAY, R"({"error":"engine unreachable"})");
        resp.setStatus(r->status());
        resp.setContentType("application/json");
        resp.setBody(r->bodyAsStringView());
    });

    // Voice discovery: model-scoped requests use audio.cpp's catalog so
    // built-in voices and reference-voice presets stay specific to the model.
    // Keep the original WAV-directory response for unscoped legacy clients.
    server.get("/v1/voices", [&keys, &cfgEngineBase, &cfgVoicesDir](const Request& req, Response& resp) {
        auto tok = bearerToken(req);
        if (tok.empty()) return sendJson(resp, StatusCode::UNAUTHORIZED,
            R"({"error":"missing Authorization: Bearer <api_key>"})");
        auto v = keys.validateAndUse(tok, req.headerOr("X-Forwarded-For", "127.0.0.1"));
        if (!v.valid) {
            nlohmann::json err = {{"error", "invalid api key: " + v.error}};
            return sendJson(resp, StatusCode::UNAUTHORIZED, err.dump());
        }

        const auto modelId = req.queryParameter("model");
        if (modelId && !modelId->empty()) {
            ClientRequest fwd;
            fwd.method("GET").url(cfgEngineBase + "/v1/audio/voices?model=" + Utils::urlEncode(*modelId));
            auto r = Client::get_instance().send_blocking(fwd);
            if (!r) return sendJson(resp, StatusCode::BAD_GATEWAY, R"({"error":"engine unreachable"})");
            if (r->status() != StatusCode::OK) {
                resp.setStatus(r->status());
                resp.setContentType(r->header("Content-Type").empty()
                                    ? "application/json" : r->header("Content-Type"));
                resp.setBody(r->body());
                return;
            }

            const auto engineJson = nlohmann::json::parse(r->bodyAsStringView(), nullptr, false);
            if (engineJson.is_discarded() || !engineJson.contains("voices") ||
                !engineJson["voices"].is_array()) {
                return sendJson(resp, StatusCode::BAD_GATEWAY,
                    R"({"error":"engine returned an invalid voice catalog"})");
            }
            nlohmann::json data = nlohmann::json::array();
            for (const auto& voice : engineJson["voices"]) {
                if (!voice.is_string() || voice.get<std::string>().empty()) {
                    return sendJson(resp, StatusCode::BAD_GATEWAY,
                        R"({"error":"engine returned an invalid voice catalog"})");
                }
                data.push_back({{"id", voice}, {"object", "voice"}, {"owned_by", "engine"}});
            }
            nlohmann::json out = {{"object", "list"}, {"data", std::move(data)}};
            sendJson(resp, StatusCode::OK, out.dump());
            return;
        }

        if (cfgVoicesDir.empty())
            return sendJson(resp, StatusCode::SERVICE_UNAVAILABLE,
                R"({"error":"voices not configured: set CHIMPFLOW_VOICES_DIR in chimpflow.env"})");

        std::error_code ec;
        std::vector<std::string> ids;
        for (const auto& entry : std::filesystem::directory_iterator(cfgVoicesDir, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".wav") continue;
            ids.push_back(entry.path().stem().string());
        }
        if (ec)
            return sendJson(resp, StatusCode::SERVICE_UNAVAILABLE,
                nlohmann::json{{"error", "voices dir unreadable: " + ec.message()}}.dump());
        std::ranges::sort(ids);

        nlohmann::json data = nlohmann::json::array();
        for (const auto& id : ids)
            data.push_back({{"id", id}, {"object", "voice"}, {"owned_by", "engine"}});
        nlohmann::json out = {{"object", "list"}, {"data", std::move(data)}};
        sendJson(resp, StatusCode::OK, out.dump());
    });

    // --- Admin API (admin scope required) ---
    auto requireAdmin = [&keys](const Request& req,
                                Token::ApiKeyStore::ValidationResult& out) {
        auto tok = bearerToken(req);
        out = tok.empty() ? Token::ApiKeyStore::ValidationResult::failure("none")
                          : keys.validateAndUse(tok, req.headerOr("X-Forwarded-For", "127.0.0.1"));
        return out.valid && out.key->hasScope("admin");
    };

    // List keys (full detail)
    server.get("/admin/keys", [&keys, &requireAdmin](const Request& req, Response& resp) {
        Token::ApiKeyStore::ValidationResult v;
        if (!requireAdmin(req, v))
            return sendJson(resp, StatusCode::FORBIDDEN, R"({"error":"admin scope required"})");
        nlohmann::json arr = nlohmann::json::array();
        for (auto& k : keys.listForUser("admin")) {
            arr.push_back({
                {"id", k.id}, {"name", k.name}, {"scopes", k.scopes},
                {"useCount", k.useCount}, {"revoked", k.revoked},
                {"master", isMasterKey(k)},
                {"expired", k.isExpired()},
                {"createdAt", std::chrono::system_clock::to_time_t(k.createdAt)},
            });
        }
        sendJson(resp, StatusCode::OK, arr.dump());
    });

    // Create a standard agent key. Master/admin keys are bootstrap-only.
    server.post("/admin/keys", [&keys, &requireAdmin](const Request& req, Response& resp) {
        Token::ApiKeyStore::ValidationResult v;
        if (!requireAdmin(req, v))
            return sendJson(resp, StatusCode::FORBIDDEN, R"({"error":"admin scope required"})");
        try {
            const auto j = nlohmann::json::parse(req.bodyAsString());
            const std::string name = j.value("name", "");
            if (name.empty())
                return sendJson(resp, StatusCode::BAD_REQUEST, R"({"error":"key name required"})");
            auto scopes = j.value("scopes", std::vector<std::string>{"tts", "stt"});
            if (scopes.empty())
                return sendJson(resp, StatusCode::BAD_REQUEST,
                                R"({"error":"at least one agent scope is required"})");
            for (const auto& scope : scopes) {
                if (scope == "admin")
                    return sendJson(resp, StatusCode::FORBIDDEN,
                        R"({"error":"agent keys cannot include admin scope"})");
                if (scope != "tts" && scope != "stt")
                    return sendJson(resp, StatusCode::BAD_REQUEST,
                        R"({"error":"agent key scopes must be 'tts' and/or 'stt'"})");
            }
            std::ranges::sort(scopes);
            scopes.erase(std::unique(scopes.begin(), scopes.end()), scopes.end());
            auto res = keys.create("admin", name, scopes);
            nlohmann::json out = {{"id", res.id}, {"rawKey", res.rawKey},
                                  {"name", name}, {"scopes", scopes}};
            sendJson(resp, StatusCode::CREATED, out.dump());
        } catch (const std::exception& e) {
            nlohmann::json err = {{"error", std::string("bad key request: ") + e.what()}};
            sendJson(resp, StatusCode::BAD_REQUEST, err.dump());
        }
    });

    // Revoke key by id: DELETE /admin/keys/<id>
    server.delete_(R"(/admin/keys/[a-zA-Z0-9_]+)", [&keys, &requireAdmin](const Request& req, Response& resp) {
        Token::ApiKeyStore::ValidationResult v;
        if (!requireAdmin(req, v))
            return sendJson(resp, StatusCode::FORBIDDEN, R"({"error":"admin scope required"})");
        const std::string path = std::string(req.path());
        const auto id = path.substr(path.find_last_of('/') + 1);
        const auto existing = keys.getById(id);
        if (!existing)
            return sendJson(resp, StatusCode::NOT_FOUND, R"({"error":"key not found"})");
        if (isMasterKey(*existing))
            return sendJson(resp, StatusCode::CONFLICT,
                            R"({"error":"bootstrap master key cannot be revoked"})");
        if (keys.revokeById(id))
            sendJson(resp, StatusCode::OK, R"({"ok":true})");
        else
            sendJson(resp, StatusCode::NOT_FOUND, R"({"error":"key not found"})");
    });

    // Restore a revoked key without ever revealing or changing its secret.
    server.post(R"(/admin/keys/[a-zA-Z0-9_]+/restore)", [&db, &keys, &requireAdmin](const Request& req, Response& resp) {
        Token::ApiKeyStore::ValidationResult v;
        if (!requireAdmin(req, v))
            return sendJson(resp, StatusCode::FORBIDDEN, R"({"error":"admin scope required"})");
        const std::string path = std::string(req.path());
        constexpr std::string_view prefix = "/admin/keys/";
        constexpr std::string_view suffix = "/restore";
        const auto id = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
        const auto existing = keys.getById(id);
        if (!existing)
            return sendJson(resp, StatusCode::NOT_FOUND, R"({"error":"key not found"})");
        if (!existing->revoked)
            return sendJson(resp, StatusCode::CONFLICT, R"({"error":"key is not revoked"})");
        if (restoreKey(db, id))
            sendJson(resp, StatusCode::OK, R"({"ok":true})");
        else
            sendJson(resp, StatusCode::CONFLICT, R"({"error":"key state changed; reload and retry"})");
    });

    // Permanent delete: DELETE /admin/keys/<id>/permanent (revoked only).
    server.delete_(R"(/admin/keys/[a-zA-Z0-9_]+/permanent)", [&keys, &requireAdmin](const Request& req, Response& resp) {
        Token::ApiKeyStore::ValidationResult v;
        if (!requireAdmin(req, v))
            return sendJson(resp, StatusCode::FORBIDDEN, R"({"error":"admin scope required"})");
        const std::string path = std::string(req.path());
        constexpr std::string_view prefix = "/admin/keys/";
        constexpr std::string_view suffix = "/permanent";
        const auto id = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
        const auto existing = keys.getById(id);
        if (!existing)
            return sendJson(resp, StatusCode::NOT_FOUND, R"({"error":"key not found"})");
        if (isMasterKey(*existing))
            return sendJson(resp, StatusCode::CONFLICT,
                            R"({"error":"bootstrap master key cannot be deleted"})");
        if (!existing->revoked)
            return sendJson(resp, StatusCode::CONFLICT,
                            R"({"error":"revoke the key before permanent deletion"})");
        if (keys.remove(id))
            sendJson(resp, StatusCode::OK, R"({"ok":true})");
        else
            sendJson(resp, StatusCode::INTERNAL_SERVER_ERROR,
                     R"({"error":"permanent delete failed"})");
    });

    // Engine models (proxied from audio.cpp, admin only)
    server.get("/admin/engine/models", [&requireAdmin, &cfgEngineBase](const Request& req, Response& resp) {
        Token::ApiKeyStore::ValidationResult v;
        if (!requireAdmin(req, v))
            return sendJson(resp, StatusCode::FORBIDDEN, R"({"error":"admin scope required"})");
        ClientRequest fwd;
        fwd.method("GET").url(cfgEngineBase + "/v1/models");
        auto r = Client::get_instance().send_blocking(fwd);
        if (!r) return sendJson(resp, StatusCode::BAD_GATEWAY, R"({"error":"engine unreachable"})");
        resp.setStatus(r->status());
        resp.setContentType("application/json");
        resp.setBody(r->bodyAsStringView());
    });

    // STT engine status (whisper.cpp reachability + served model, admin only).
    // Never 502s: reports {"reachable":false} so the WebUI can show it.
    server.get("/admin/engine/stt", [&](const Request& req, Response& resp) {
        Token::ApiKeyStore::ValidationResult v;
        if (!requireAdmin(req, v))
            return sendJson(resp, StatusCode::FORBIDDEN, R"({"error":"admin scope required"})");
        ClientRequest fwd;
        fwd.method("GET").url(cfgSttBase + "/health");
        OmniNet::HTTP::ClientConfig cfg;
        cfg.connect_timeout = std::chrono::seconds(2);
        cfg.request_timeout = std::chrono::seconds(4);
        const bool up = Client::get_instance().send_blocking(fwd, cfg).has_value();
        nlohmann::json out = {
            {"engine", "whisper.cpp"},
            {"model", cfgSttModel},
            {"url", cfgSttBase},
            {"reachable", up},
        };
        sendJson(resp, StatusCode::OK, out.dump());
    });

    // --- WebUI (static, open) ---
    StaticHandler staticHandler(webRoot());
    staticHandler.setDefaultFile("index.html");
    auto serveIndex = [&staticHandler](const Request& req, Response& resp) {
        staticHandler.handle(req, resp);
        // Single-file SPA: never let the browser cache index.html, or UI
        // updates look like they "didn't deploy" until a hard refresh.
        resp.setHeader("Cache-Control", "no-cache");
    };
    server.router().get("/", serveIndex);
    server.router().get("/index.html", serveIndex);

    std::cout << "ChimpFlow listening on http://127.0.0.1:8090\n"
              << "  /healthz            (open)\n"
              << "  /skill/             (open — agent guide, markdown + manifest.json)\n"
              << "  /v1/models         (Bearer key, any valid)\n"
              << "  /v1/voices?model=<id> (Bearer key, any valid)\n"
              << "  /v1/audio/speech    (Bearer key, 'tts' scope)\n"
              << "  /v1/audio/transcriptions (Bearer key, 'stt' scope)\n"
              << "  /admin/engine/stt   (Bearer key, 'admin' scope)\n"
              << "  /admin/keys         (Bearer key, 'admin' scope)\n";
    server.start();
    OmniNet::cleanup();
    return 0;
}
