// ChimpEcho — parrot demo for ChimpFlow: hold to talk, it STTs your speech
// through the service and speaks it straight back (TTS). gtkmm-4 + PipeWire
// for audio I/O, OmniNet for HTTP. The network round trip runs on a worker
// thread with hard timeouts — the UI thread never blocks on the service.
//
// Debug: everything is logged to stderr (launcher redirects to
// /tmp/chimp-echo.log); set CHIMPECHO_LOG=/path to also append to a file.
#include <gtkmm.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/builder.h>

#include "http/HttpClient.h"
#include "wav.h"
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using OmniNet::HTTP::Client;
using OmniNet::HTTP::ClientConfig;
using OmniNet::HTTP::ClientRequest;
using OmniNet::HTTP::StatusCode;
using namespace chimpecho;

namespace {

// ---- logging --------------------------------------------------------------
void logLine(const std::string& msg) {
    char ts[16] = {};
    std::time_t t = std::time(nullptr);
    std::strftime(ts, sizeof ts, "%H:%M:%S", std::localtime(&t));
    std::string line = std::string{"["} + ts + "] " + msg + "\n";
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);  // survive a crash: never leave the last line buffered
    if (auto* p = std::getenv("CHIMPECHO_LOG")) {
        if (std::FILE* f = std::fopen(p, "a")) {
            std::fwrite(line.data(), 1, line.size(), f);
            std::fclose(f);
        }
    }
}

// ---- config ---------------------------------------------------------------
std::string g_base  = "http://127.0.0.1:8090";
std::string g_key;
std::string g_voice = "alba";

void loadConfig() {
    std::string keySource = "none";
    if (auto* e = std::getenv("CHIMPFLOW_URL"))  g_base = e;
    if (auto* e = std::getenv("CHIMPFLOW_VOICE")) g_voice = e;
    if (auto* e = std::getenv("CHIMPFLOW_KEY")) { g_key = e; keySource = "CHIMPFLOW_KEY"; }
    else {
        auto p = std::filesystem::path(std::getenv("HOME")) / ".local/share/chimpflow/ADMIN_KEY";
        if (std::ifstream f{p}; f && std::getline(f, g_key)) keySource = "ADMIN_KEY file";
    }
    logLine("config: base=" + g_base + " voice=" + g_voice + " key=" + keySource);
}

// ---- PipeWire -------------------------------------------------------------
struct PwInitGuard {
    PwInitGuard() { pw_init(nullptr, nullptr); }
    ~PwInitGuard() { pw_deinit(); }
};

struct Capture {
    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    std::atomic<bool> active{false};
    std::mutex mtx;
    std::vector<float> buf;

    static void onProcess(void* userdata) {
        auto* self = static_cast<Capture*>(userdata);
        pw_buffer* b;
        while ((b = pw_stream_dequeue_buffer(self->stream)) != nullptr) {
            spa_data& d = b->buffer->datas[0];
            if (self->active && d.data) {
                auto n = static_cast<std::size_t>(d.chunk->size) / sizeof(float);
                const float* s = static_cast<const float*>(d.data);
                std::lock_guard lk(self->mtx);
                self->buf.insert(self->buf.end(), s, s + n);
            }
            pw_stream_queue_buffer(self->stream, b);
        }
    }

    void start() {
        static pw_stream_events events = {PW_VERSION_STREAM_EVENTS, nullptr, nullptr, nullptr};
        events.process = &Capture::onProcess;
        loop = pw_thread_loop_new("chimpecho-capture", nullptr);
        pw_properties* props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Communication", nullptr);
        stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop),
                                      "chimpecho-in", props, &events, this);
        std::uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
        spa_audio_info_raw info = {};
        info.format = SPA_AUDIO_FORMAT_F32_LE;
        info.rate = 16000;
        info.channels = 1;
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
        const spa_pod* param = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);
        const spa_pod* params[] = {param};
        if (pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1) < 0)
            throw std::runtime_error("pw capture connect failed");
        if (pw_thread_loop_start(loop) < 0) throw std::runtime_error("pw loop start failed");
        active = true;
        logLine("capture started (16 kHz mono f32)");
    }

    std::vector<float> stop() {
        if (!loop) return {};  // idempotent — stop may fire twice per cycle
        active = false;  // RT callback stops touching buf before we tear down
        pw_thread_loop_lock(loop);
        pw_stream_destroy(stream); stream = nullptr;
        // MUST unlock before pw_thread_loop_stop: stop() joins the loop
        // thread, which cannot process its own do_stop while we hold the
        // lock — holding it across stop() deadlocks (UI froze on every
        // capture/playback teardown). Verified with /tmp/pw_repro (5 cycles).
        pw_thread_loop_unlock(loop);
        pw_thread_loop_stop(loop);
        pw_thread_loop_destroy(loop); loop = nullptr;
        std::lock_guard lk(mtx);
        auto out = std::move(buf);
        buf.clear();
        logLine("capture stopped: " + std::to_string(out.size()) + " samples (" +
                std::to_string(out.size() / 16000) + "." +
                std::to_string((out.size() / 160) % 100) + " s)");
        return out;
    }
};

struct Playback {
    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    std::mutex mtx;
    std::deque<float> queue;
    Glib::Dispatcher& done;
    bool notified = false;

    explicit Playback(Glib::Dispatcher& d) : done(d) {}

    static void onProcess(void* userdata) {
        auto* self = static_cast<Playback*>(userdata);
        pw_buffer* b;
        while ((b = pw_stream_dequeue_buffer(self->stream)) != nullptr) {
            spa_data& d = b->buffer->datas[0];
            if (d.data) {
                auto frames = static_cast<std::size_t>(d.maxsize) / sizeof(float);
                float* dst = static_cast<float*>(d.data);
                {
                    std::lock_guard lk(self->mtx);
                    auto n = std::min(frames, self->queue.size());
                    for (std::size_t i = 0; i < n; ++i) { dst[i] = self->queue.front(); self->queue.pop_front(); }
                    for (std::size_t i = n; i < frames; ++i) dst[i] = 0.0f;
                    d.chunk->size = frames * sizeof(float);
                    if (self->queue.empty() && !self->notified) {
                        self->notified = true;
                        // All samples are handed to the stream but NOT yet
                        // heard: signaling done here made stop() destroy the
                        // stream with the tail still queued -> cut-off audio.
                        // Ask PipeWire to play everything out and signal
                        // done from onDrained instead.
                        pw_stream_flush(self->stream, true);
                    }
                }
            }
            pw_stream_queue_buffer(self->stream, b);
        }
    }

    static void onDrained(void* userdata) {
        auto* self = static_cast<Playback*>(userdata);
        logLine("playback drained");   // all queued samples actually played
        self->done.emit();
    }

    void start(std::uint32_t rate, std::vector<float> samples) {
        {
            std::lock_guard lk(mtx);
            queue.assign(samples.begin(), samples.end());
            notified = false;  // re-arm the done latch — without this the 2nd
        }                      // playback never signals and the UI dead-locks
        static pw_stream_events events = {PW_VERSION_STREAM_EVENTS, nullptr, nullptr, nullptr};
        events.process = &Playback::onProcess;
        events.drained = &Playback::onDrained;
        loop = pw_thread_loop_new("chimpecho-play", nullptr);
        pw_properties* props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Communication", nullptr);
        stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop),
                                      "chimpecho-out", props, &events, this);
        std::uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
        spa_audio_info_raw info = {};
        info.format = SPA_AUDIO_FORMAT_F32_LE;
        info.rate = rate;
        info.channels = 1;
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
        const spa_pod* param = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);
        const spa_pod* params[] = {param};
        if (pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1) < 0)
            throw std::runtime_error("pw playback connect failed");
        if (pw_thread_loop_start(loop) < 0) throw std::runtime_error("pw loop start failed");
        logLine("playback started: rate=" + std::to_string(rate) +
                " samples=" + std::to_string(samples.size()));
    }

    void stop() {
        if (!loop) return;
        pw_thread_loop_lock(loop);
        pw_stream_destroy(stream); stream = nullptr;
        // MUST unlock before pw_thread_loop_stop: stop() joins the loop
        // thread, which cannot process its own do_stop while we hold the
        // lock — holding it across stop() deadlocks (UI froze on every
        // capture/playback teardown). Verified with /tmp/pw_repro (5 cycles).
        pw_thread_loop_unlock(loop);
        pw_thread_loop_stop(loop);
        pw_thread_loop_destroy(loop); loop = nullptr;
    }
};

// ---- HTTP (OmniNet) -------------------------------------------------------
// Timed (3 s connect / 45 s request) so a wedged service can never hang the
// app. Returns body on HTTP 200; on failure sets `err`.
std::optional<std::string> post(const std::string& path, const std::string& contentType,
                                const std::string& payload, std::string& err) {
    ClientRequest req;
    req.method("POST").url(g_base + path)
       .header("Authorization", "Bearer " + g_key)
       .header("Content-Type", contentType)
       .body(payload);
    ClientConfig cfg;
    cfg.connect_timeout = std::chrono::seconds(3);
    cfg.request_timeout = std::chrono::seconds(45);
    const auto t0 = std::chrono::steady_clock::now();
    auto r = Client::get_instance().send_blocking(req, cfg);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    if (!r) {
        err = "request failed or timed out after " + std::to_string(ms) + " ms (is ChimpFlow running?)";
        logLine("POST " + path + " -> FAILED after " + std::to_string(ms) + " ms");
        return std::nullopt;
    }
    const std::string body(reinterpret_cast<const char*>(r->body().data()), r->body().size());
    logLine("POST " + path + " -> " + std::to_string(static_cast<int>(r->status())) +
            " in " + std::to_string(ms) + " ms, " + std::to_string(body.size()) + " bytes");
    if (r->status() != StatusCode::OK) {
        err = "HTTP " + std::to_string(static_cast<int>(r->status())) + ": " + body.substr(0, 200);
        return std::nullopt;
    }
    return body;
}

// GET — same timeout policy as post(), for the /v1/voices discovery call.
std::optional<std::string> get(const std::string& path, std::string& err) {
    ClientRequest req;
    req.method("GET").url(g_base + path)
       .header("Authorization", "Bearer " + g_key);
    ClientConfig cfg;
    cfg.connect_timeout = std::chrono::seconds(3);
    cfg.request_timeout = std::chrono::seconds(10);
    auto r = Client::get_instance().send_blocking(req, cfg);
    if (!r) { err = "GET " + path + " failed (is ChimpFlow running?)"; return std::nullopt; }
    const std::string body(reinterpret_cast<const char*>(r->body().data()), r->body().size());
    if (r->status() != StatusCode::OK) {
        err = "GET " + path + " -> HTTP " + std::to_string(static_cast<int>(r->status()));
        return std::nullopt;
    }
    return body;
}

// ---- window ---------------------------------------------------------------
struct EchoResult {
    bool ok = false;
    std::string err, text;
    std::uint32_t rate = 0;
    std::vector<float> pcm;
};

class EchoWindow : public Gtk::ApplicationWindow {
public:
    EchoWindow() {
        set_title("ChimpEcho — parrot demo");
        set_default_size(360, 340);
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
        box->set_margin(14);
        set_child(*box);

        auto* title = Gtk::make_managed<Gtk::Label>();
        title->set_markup("<b>ChimpEcho</b>\n<small>hold the button, say something — "
                          "ChimpFlow will say it back</small>");
        box->append(*title);

        // Voice picker: populated from GET /v1/voices (worker thread below);
        // starts as a single-entry list (g_voice) so the dropdown is usable
        // even if the gateway is unreachable.
        auto* voiceRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        voiceRow->append(*Gtk::make_managed<Gtk::Label>("Voice"));
        voiceModel_ = Gtk::StringList::create({Glib::ustring{g_voice}});
        voiceDrop_.set_model(voiceModel_);
        voiceDrop_.set_hexpand(true);
        voiceRow->append(voiceDrop_);
        box->append(*voiceRow);

        talk_.set_label("🎤\nHold to talk");
        talk_.set_size_request(170, 130);
        talk_.add_css_class("suggested-action");
        box->append(talk_);

        status_.set_text(g_key.empty() ? "⚠ no API key — set CHIMPFLOW_KEY" : "ready");
        box->append(status_);

        heard_.set_wrap(true);
        heard_.set_selectable(true);
        box->append(heard_);

        auto gesture = Gtk::GestureClick::create();
        gesture->set_button(GDK_BUTTON_PRIMARY);
        gesture->signal_pressed().connect([this](int, double, double) { beginTalk(); });
        gesture->signal_released().connect([this](int, double, double) { endTalk(); });
        talk_.add_controller(gesture);
        // Belt and braces: the button's own activation also stops recording —
        // endTalk() is idempotent, so a double stop is harmless.
        talk_.signal_clicked().connect([this] { endTalk(); });

        playbackDone_.connect([this] { onPlaybackDone(); });
        workDone_.connect([this] { onWorkDone(); });
        voicesLoaded_.connect([this] { onVoicesLoaded(); });
        if (!g_key.empty()) {
            voiceWorker_ = std::jthread([this] {
                std::string err;
                std::vector<std::string> ids;
                if (auto r = get("/v1/voices", err)) {
                    try {
                        for (const auto& v : nlohmann::json::parse(*r).at("data"))
                            ids.push_back(v.at("id").get<std::string>());
                    } catch (const std::exception&) { err = "bad /v1/voices response"; }
                }
                voicesResult_ = std::move(ids);  // empty on failure -> keep fallback
                voicesErr_ = std::move(err);
                voicesLoaded_.emit();
            });
        }
        signal_close_request().connect([this] {
            logLine("window closing");
            capturing_ ? (void)capture_.stop() : void();
            playing_.stop();
            if (worker_.joinable()) worker_.join();
            if (voiceWorker_.joinable()) voiceWorker_.join();
            return false;
        }, false);
    }

private:
    std::string selectedVoice() const {
        const auto pos = voiceDrop_.get_selected();
        if (voiceModel_ && pos != GTK_INVALID_LIST_POSITION)
            return std::string{voiceModel_->get_string(pos)};
        return g_voice;
    }

    // Worker fetched the catalog; replace the fallback list. Dispatcher
    // gives the UI thread happens-before visibility into voicesResult_.
    void onVoicesLoaded() {
        if (!voicesResult_) return;
        auto ids = std::move(*voicesResult_);
        voicesResult_.reset();
        if (ids.empty()) {
            logLine("voices: /v1/voices unavailable (" + voicesErr_ + ") — keeping " + g_voice);
            return;
        }
        const std::string current = selectedVoice();
        voiceModel_ = Gtk::StringList::create(
            std::vector<Glib::ustring>{ids.begin(), ids.end()});
        voiceDrop_.set_model(voiceModel_);
        for (guint i = 0; i < voiceModel_->get_n_items(); ++i)
            if (voiceModel_->get_string(i) == current) { voiceDrop_.set_selected(i); break; }
        logLine("voices: loaded " + std::to_string(ids.size()) + " from /v1/voices");
    }

    void setBusy(bool busy) {
        talking_ = busy;
        talk_.set_sensitive(!busy);
        if (!busy) talk_.grab_focus();
    }

    void beginTalk() {
        if (talking_ || g_key.empty()) return;
        if (capturing_) { endTalk(); return; }  // press-again toggles off even
        try {                                   // if 'released' was eaten
            capture_.start();
            capturing_ = true;
            talk_.set_label("🔴\nListening…");
            status_.set_text("recording — release to send");
        } catch (const std::exception& e) {
            logLine(std::string{"ERROR capture start: "} + e.what());
            status_.set_text(std::string("⚠ mic failed: ") + e.what());
        }
    }

    void endTalk() {
        if (!capturing_) return;
        capturing_ = false;
        talk_.set_label("⏳\nThinking…");
        status_.set_text("transcribing…");
        std::vector<float> samples;
        try { samples = capture_.stop(); }
        catch (const std::exception& e) {
            logLine(std::string{"ERROR capture stop: "} + e.what());
            status_.set_text(std::string("⚠ ") + e.what());
            setBusy(false);
            return;
        }
        if (samples.size() < 16000 / 4) {  // < 0.25 s — probably silence
            heard_.set_text("(heard nothing)");
            status_.set_text("ready");
            setBusy(false);
            return;
        }
        setBusy(true);
        // The network round trip runs on a worker thread — the UI stays alive.
        worker_ = std::jthread([this, wav = makeWav16(samples)]() mutable {
            logLine("STT request: " + std::to_string(wav.size()) + " byte wav");
            EchoResult r;
            std::string err;
            auto stt = post("/v1/audio/transcriptions",
                            "multipart/form-data; boundary=----ChimpEchoBoundary",
                            multipartBody("----ChimpEchoBoundary", wav), err);
            if (!stt) { r.err = err; finish(std::move(r)); return; }
            try { r.text = nlohmann::json::parse(*stt).at("text").get<std::string>(); }
            catch (const std::exception&) { r.err = "bad STT response: " + stt->substr(0, 200); finish(std::move(r)); return; }
            while (!r.text.empty() && (r.text.back() == '\n' || r.text.back() == ' ')) r.text.pop_back();
            logLine("STT heard: \"" + r.text + "\"");
            if (r.text.empty()) { r.ok = true; finish(std::move(r)); return; }
            const std::string voice = selectedVoice();
            logLine("TTS voice: " + voice);
            nlohmann::json ttsReq = {{"model", "pocket-tts"}, {"input", r.text},
                                     {"voice", voice}, {"response_format", "wav"}};
            auto tts = post("/v1/audio/speech", "application/json", ttsReq.dump(), err);
            if (!tts) { r.err = err; finish(std::move(r)); return; }
            if (!parseWav(*tts, r.rate, r.pcm)) { r.err = "couldn't parse TTS audio"; finish(std::move(r)); return; }
            r.ok = true;
            finish(std::move(r));
        });
    }

    // Worker writes result_, then emits — Dispatcher gives the UI thread
    // happens-before visibility.
    void finish(EchoResult r) {
        result_ = std::move(r);
        workDone_.emit();
    }

    void onWorkDone() {
        auto r = std::move(result_);
        if (!r) return;
        if (!r->ok) {
            logLine("ERROR: " + r->err);
            status_.set_text("⚠ " + r->err);
            talk_.set_label("🎤\nHold to talk");
            setBusy(false);
            return;
        }
        heard_.set_text(r->text.empty() ? "(silence?)" : "heard: “" + r->text + "”");
        if (r->text.empty()) {
            status_.set_text("ready");
            setBusy(false);
            return;
        }
        status_.set_text("speaking…");
        try { playing_.start(r->rate, std::move(r->pcm)); }
        catch (const std::exception& e) {
            logLine(std::string{"ERROR playback start: "} + e.what());
            status_.set_text(std::string("⚠ speaker failed: ") + e.what());
            setBusy(false);
        }
    }

    void onPlaybackDone() {
        logLine("playback done");
        playing_.stop();
        status_.set_text("ready");
        talk_.set_label("🎤\nHold to talk");
        setBusy(false);
    }

    Gtk::Button talk_;
    Gtk::Label status_, heard_;
    Gtk::DropDown voiceDrop_;
    Glib::RefPtr<Gtk::StringList> voiceModel_;
    Glib::Dispatcher playbackDone_, workDone_, voicesLoaded_;
    Capture capture_;
    Playback playing_{playbackDone_};
    std::optional<EchoResult> result_;
    std::optional<std::vector<std::string>> voicesResult_;
    std::string voicesErr_;
    std::jthread worker_, voiceWorker_;
    bool talking_ = false, capturing_ = false;
};

}  // namespace

int main(int argc, char** argv) {
    loadConfig();
    PwInitGuard pw;
    logLine("chimp-echo starting");
    auto app = Gtk::Application::create("com.leestripp.chimpecho");
    app->signal_activate().connect([&] {
        auto* win = new EchoWindow();
        win->set_application(app);
        win->present();
    });
    const int rc = app->run(argc, argv);
    logLine("chimp-echo exiting");
    return rc;
}
