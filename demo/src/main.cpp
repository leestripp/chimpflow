// ChimpEcho — parrot demo for ChimpFlow: hold to talk, it STTs your speech
// through the service and speaks it straight back (TTS). gtkmm-4 + PipeWire
// for audio I/O, OmniNet for HTTP.
#include <gtkmm.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/builder.h>

#include "http/HttpClient.h"
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

using OmniNet::HTTP::Client;
using OmniNet::HTTP::ClientRequest;
using OmniNet::HTTP::StatusCode;

namespace {

// ---- config ---------------------------------------------------------------
std::string g_base  = "http://127.0.0.1:8090";
std::string g_key;
std::string g_voice = "alba";

void loadConfig() {
    if (auto* e = std::getenv("CHIMPFLOW_URL"))  g_base = e;
    if (auto* e = std::getenv("CHIMPFLOW_VOICE")) g_voice = e;
    if (auto* e = std::getenv("CHIMPFLOW_KEY")) { g_key = e; return; }
    auto p = std::filesystem::path(std::getenv("HOME")) / ".local/share/chimpflow/ADMIN_KEY";
    if (std::ifstream f{p}; f) std::getline(f, g_key);
}

// ---- WAV helpers ----------------------------------------------------------
// floats (-1..1), 16 kHz mono -> 16-bit PCM WAV in memory (whisper's favourite)
std::vector<std::uint8_t> makeWav16(const std::vector<float>& samples) {
    const auto n = static_cast<std::uint32_t>(samples.size());
    const std::uint32_t dataBytes = n * 2, rate = 16000;
    std::vector<std::uint8_t> w(44 + dataBytes);
    auto put = [&w](std::size_t o, auto v) { std::memcpy(w.data() + o, &v, sizeof v); };
    std::memcpy(w.data(), "RIFF", 4);          put(4,  36u + dataBytes);
    std::memcpy(w.data() + 8, "WAVEfmt ", 8);  put(16, 16u);
    put(18, std::uint16_t{1});                 // PCM
    put(20, std::uint16_t{1});                 // mono
    put(22, rate);  put(24, rate * 2u);  put(28, 2u);  put(30, std::uint16_t{16});
    std::memcpy(w.data() + 32, "data", 4);     put(36, dataBytes);
    for (std::size_t i = 0; i < n; ++i) {
        float s = std::clamp(samples[i], -1.0f, 1.0f);
        auto v = static_cast<std::int16_t>(std::lrint(s * 32767.0f));
        put(44 + i * 2, v);
    }
    return w;
}

// Parse whatever PCM WAV the engine returns (24-bit/24 kHz today) -> floats at
// the stream rate. Returns false on anything we don't understand.
bool parseWav(const std::string& bytes, std::uint32_t& rate, std::vector<float>& out) {
    if (bytes.size() < 44 || bytes.compare(0, 4, "RIFF") || bytes.compare(8, 4, "WAVE"))
        return false;
    std::uint16_t fmt = 0, ch = 0, bits = 0;
    rate = 0;
    std::size_t pos = 12, dataPos = 0, dataLen = 0;
    while (pos + 8 <= bytes.size()) {
        std::uint32_t sz;
        std::memcpy(&sz, bytes.data() + pos + 4, 4);
        const char* id = bytes.data() + pos;
        if (!std::memcmp(id, "fmt ", 4)) {
            std::memcpy(&fmt,  bytes.data() + pos + 8, 2);
            std::memcpy(&ch,   bytes.data() + pos + 10, 2);
            std::memcpy(&rate, bytes.data() + pos + 12, 4);
            std::memcpy(&bits, bytes.data() + pos + 22, 2);
        } else if (!std::memcmp(id, "data", 4)) {
            dataPos = pos + 8; dataLen = sz;
        }
        pos += 8 + sz + (sz & 1);
    }
    if (fmt != 1 || ch != 1 || rate == 0 || dataPos == 0) return false;
    const std::uint8_t* d = reinterpret_cast<const std::uint8_t*>(bytes.data() + dataPos);
    out.clear();
    if (bits == 16) {
        out.resize(dataLen / 2);
        for (std::size_t i = 0; i < out.size(); ++i) {
            std::int16_t v; std::memcpy(&v, d + i * 2, 2); out[i] = v / 32768.0f;
        }
    } else if (bits == 24) {
        out.resize(dataLen / 3);
        for (std::size_t i = 0; i < out.size(); ++i) {
            std::int32_t v = d[i*3] | (d[i*3+1] << 8) | (d[i*3+2] << 16);
            if (v & 0x800000) v |= ~0xFFFFFF;
            out[i] = v / 8388608.0f;
        }
    } else if (bits == 32) {
        out.resize(dataLen / 4);
        for (std::size_t i = 0; i < out.size(); ++i) {
            std::int32_t v; std::memcpy(&v, d + i * 4, 4); out[i] = v / 2147483648.0f;
        }
    } else return false;
    return true;
}

// ---- multipart ------------------------------------------------------------
std::string multipartBody(const std::string& boundary, const std::vector<std::uint8_t>& wav) {
    std::string b;
    b += "--" + boundary + "\r\n";
    b += "Content-Disposition: form-data; name=\"file\"; filename=\"echo.wav\"\r\n";
    b += "Content-Type: audio/wav\r\n\r\n";
    b.append(reinterpret_cast<const char*>(wav.data()), wav.size());
    b += "\r\n--" + boundary + "--\r\n";
    return b;
}

// ---- PipeWire -------------------------------------------------------------
struct PwInitGuard {
    PwInitGuard() { pw_init(nullptr, nullptr); }
    ~PwInitGuard() { pw_deinit(); }
};

struct Capture {
    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    std::mutex mtx;
    std::vector<float> buf;

    static void onProcess(void* userdata) {
        auto* self = static_cast<Capture*>(userdata);
        pw_buffer* b;
        while ((b = pw_stream_dequeue_buffer(self->stream)) != nullptr) {
            spa_data& d = b->buffer->datas[0];
            if (d.data) {
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
    }

    std::vector<float> stop() {
        pw_thread_loop_lock(loop);
        pw_stream_destroy(stream); stream = nullptr;
        pw_thread_loop_stop(loop);
        pw_thread_loop_destroy(loop); loop = nullptr;
        std::lock_guard lk(mtx);
        return std::move(buf);
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
                        self->done.emit();
                    }
                }
            }
            pw_stream_queue_buffer(self->stream, b);
        }
    }

    void start(std::uint32_t rate, std::vector<float> samples) {
        {
            std::lock_guard lk(mtx);
            queue.assign(samples.begin(), samples.end());
        }
        static pw_stream_events events = {PW_VERSION_STREAM_EVENTS, nullptr, nullptr, nullptr};
        events.process = &Playback::onProcess;
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
    }

    void stop() {
        if (!loop) return;
        pw_thread_loop_lock(loop);
        pw_stream_destroy(stream); stream = nullptr;
        pw_thread_loop_stop(loop);
        pw_thread_loop_destroy(loop); loop = nullptr;
    }
};

// ---- HTTP (OmniNet) -------------------------------------------------------
// Returns body on HTTP 200; on failure sets `err` and returns std::nullopt.
std::optional<std::string> post(const std::string& path, const std::string& contentType,
                                const std::string& payload, std::string& err) {
    ClientRequest req;
    req.method("POST").url(g_base + path)
       .header("Authorization", "Bearer " + g_key)
       .header("Content-Type", contentType)
       .body(payload);
    auto r = Client::get_instance().send_blocking(req);
    if (!r) { err = "service unreachable (is ChimpFlow running?)"; return std::nullopt; }
    if (r->status() != StatusCode::OK) {
        err = "HTTP " + std::to_string(static_cast<int>(r->status())) + ": " +
              std::string(reinterpret_cast<const char*>(r->body().data()),
                          r->body().size()).substr(0, 200);
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char*>(r->body().data()), r->body().size());
}

// ---- window ---------------------------------------------------------------
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

        playbackDone_.connect([this] { onPlaybackDone(); });
        signal_close_request().connect([this] { capturing_ ? (void)capture_.stop() : void();
                                                  playing_.stop(); return false; }, false);
    }

private:
    void setBusy(bool busy) {
        talking_ = busy;
        talk_.set_sensitive(!busy);
        if (!busy) talk_.grab_focus();
    }

    void beginTalk() {
        if (talking_ || g_key.empty()) return;
        try {
            capture_.buf.clear();
            capture_.start();
            capturing_ = true;
            talk_.set_label("🔴\nListening…");
            status_.set_text("recording — release to send");
        } catch (const std::exception& e) {
            status_.set_text(std::string("⚠ mic failed: ") + e.what());
        }
    }

    void endTalk() {
        if (!capturing_) return;
        capturing_ = false;
        talk_.set_label("⏳\nThinking…");
        status_.set_text("transcribing…");
        // let the UI paint, then do the HTTP round trip on the main thread
        Glib::signal_idle().connect_once([this] { echo(); });
    }

    void echo() {
        std::vector<float> samples;
        try { samples = capture_.stop(); }
        catch (const std::exception& e) { status_.set_text(std::string("⚠ ") + e.what()); setBusy(false); return; }

        setBusy(true);
        if (samples.size() < 16000 / 4) {  // < 0.25 s — probably silence
            heard_.set_text("(heard nothing)");
            status_.set_text("ready");
            setBusy(false);
            return;
        }

        std::string err;
        const std::string boundary = "----ChimpEchoBoundary";
        auto wav = makeWav16(samples);
        auto stt = post("/v1/audio/transcriptions",
                        "multipart/form-data; boundary=" + boundary,
                        multipartBody(boundary, wav), err);
        if (!stt) { status_.set_text("⚠ " + err); setBusy(false); return; }

        std::string text;
        try { text = nlohmann::json::parse(*stt).at("text").get<std::string>(); }
        catch (const std::exception&) { status_.set_text("⚠ bad STT response: " + *stt); setBusy(false); return; }
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
        heard_.set_text("heard: “" + text + "”");
        if (text.empty()) { status_.set_text("ready"); setBusy(false); return; }

        status_.set_text("speaking…");
        nlohmann::json ttsReq = {{"model", "pocket-tts"}, {"input", text},
                                 {"voice", g_voice}, {"response_format", "wav"}};
        auto tts = post("/v1/audio/speech", "application/json", ttsReq.dump(), err);
        if (!tts) { status_.set_text("⚠ " + err); setBusy(false); return; }

        std::uint32_t rate = 0;
        std::vector<float> pcm;
        if (!parseWav(*tts, rate, pcm)) { status_.set_text("⚠ couldn't parse TTS audio"); setBusy(false); return; }
        try { playing_.start(rate, std::move(pcm)); }
        catch (const std::exception& e) { status_.set_text(std::string("⚠ speaker failed: ") + e.what()); setBusy(false); }
    }

    void onPlaybackDone() {
        playing_.stop();
        status_.set_text("ready");
        talk_.set_label("🎤\nHold to talk");
        setBusy(false);
    }

    Gtk::Button talk_;
    Gtk::Label status_, heard_;
    Glib::Dispatcher playbackDone_;
    Capture capture_;
    Playback playing_{playbackDone_};
    bool talking_ = false, capturing_ = false;
};

}  // namespace

int main(int argc, char** argv) {
    loadConfig();
    PwInitGuard pw;
    auto app = Gtk::Application::create("com.leestripp.chimpecho");
    app->signal_activate().connect([&] {
        auto* win = new EchoWindow();
        win->set_application(app);
        win->present();
    });
    return app->run(argc, argv);
}
