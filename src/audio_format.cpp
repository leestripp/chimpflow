#include "audio_format.h"

#include <algorithm>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace chimpflow {
namespace {

constexpr std::size_t kMaxEncodedBytes = 64 * 1024 * 1024;
constexpr auto kEncoderTimeout = std::chrono::seconds(60);

class TempFile {
public:
    TempFile()
        : m_path((std::filesystem::temp_directory_path() / "chimpflow-mp3-XXXXXX").string()) {
        m_fd = ::mkstemp(m_path.data());
        if (m_fd < 0) {
            throw std::runtime_error("cannot create MP3 temporary file: " +
                                     std::string(std::strerror(errno)));
        }
    }

    ~TempFile() {
        close();
        if (!m_path.empty()) ::unlink(m_path.c_str());
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    int fd() const { return m_fd; }
    const std::string& path() const { return m_path; }

    void close() noexcept {
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }

private:
    int m_fd = -1;
    std::string m_path;
};

void writeAll(int fd, std::span<const std::uint8_t> bytes) {
    while (!bytes.empty()) {
        const auto written = ::write(fd, bytes.data(), bytes.size());
        if (written < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("cannot write WAV for MP3 encoding: " +
                                     std::string(std::strerror(errno)));
        }
        bytes = bytes.subspan(static_cast<std::size_t>(written));
    }
}

void waitForEncoder(pid_t pid) {
    const auto deadline = std::chrono::steady_clock::now() + kEncoderTimeout;
    int status = 0;
    while (true) {
        const auto result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return;
            if (WIFEXITED(status)) {
                throw std::runtime_error("ffmpeg exited with status " +
                                         std::to_string(WEXITSTATUS(status)));
            }
            throw std::runtime_error("ffmpeg terminated before producing MP3 audio");
        }
        if (result < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("cannot wait for ffmpeg: " +
                                     std::string(std::strerror(errno)));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            throw std::runtime_error("ffmpeg timed out while producing MP3 audio");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

std::vector<std::uint8_t> readEncodedFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("ffmpeg did not produce an MP3 file");
    const auto end = input.tellg();
    if (end <= 0 || static_cast<std::uint64_t>(end) > kMaxEncodedBytes) {
        throw std::runtime_error("ffmpeg produced an invalid MP3 size");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) throw std::runtime_error("cannot read ffmpeg's MP3 output");
    return bytes;
}

}  // namespace

std::vector<std::uint8_t> encodeMp3(std::span<const std::uint8_t> wav,
                                    std::string_view ffmpegPath) {
    if (wav.empty()) throw std::runtime_error("cannot encode an empty WAV body");

    TempFile input;
    TempFile output;
    writeAll(input.fd(), wav);
    input.close();
    output.close();

    std::string executable(ffmpegPath);
    std::string inputPath = input.path();
    std::string outputPath = output.path();
    char* const args[] = {
        executable.data(),
        const_cast<char*>("-nostdin"),
        const_cast<char*>("-hide_banner"),
        const_cast<char*>("-loglevel"), const_cast<char*>("error"),
        const_cast<char*>("-y"),
        const_cast<char*>("-i"), inputPath.data(),
        const_cast<char*>("-map_metadata"), const_cast<char*>("-1"),
        const_cast<char*>("-vn"),
        const_cast<char*>("-codec:a"), const_cast<char*>("libmp3lame"),
        const_cast<char*>("-b:a"), const_cast<char*>("128k"),
        const_cast<char*>("-f"), const_cast<char*>("mp3"),
        outputPath.data(),
        nullptr,
    };

    pid_t pid = -1;
    const int spawnResult = ::posix_spawnp(&pid, executable.c_str(), nullptr, nullptr,
                                           args, environ);
    if (spawnResult != 0) {
        throw std::runtime_error("cannot start ffmpeg: " +
                                 std::error_code(spawnResult, std::generic_category()).message());
    }
    waitForEncoder(pid);
    return readEncodedFile(output.path());
}

}  // namespace chimpflow

namespace chimpflow {
namespace {

constexpr std::size_t kMaxJoinedPcmBytes = 256 * 1024 * 1024;

std::uint16_t read16(const std::span<const std::uint8_t> bytes, const std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at]) |
           static_cast<std::uint16_t>(bytes[at + 1] << 8);
}

std::uint32_t read32(const std::span<const std::uint8_t> bytes, const std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

void write16(std::vector<std::uint8_t>& bytes, const std::size_t at, const std::uint16_t value) {
    bytes[at] = static_cast<std::uint8_t>(value);
    bytes[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

void write32(std::vector<std::uint8_t>& bytes, const std::size_t at, const std::uint32_t value) {
    bytes[at] = static_cast<std::uint8_t>(value);
    bytes[at + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[at + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[at + 3] = static_cast<std::uint8_t>(value >> 24);
}

bool fourCc(const std::span<const std::uint8_t> bytes, const std::size_t at,
            const char (&expected)[5]) {
    return std::memcmp(bytes.data() + at, expected, 4) == 0;
}

struct WavView {
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::span<const std::uint8_t> pcm;
};

WavView parseWav(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 44 || !fourCc(bytes, 0, "RIFF") || !fourCc(bytes, 8, "WAVE")) {
        throw std::runtime_error("engine returned a malformed WAV body");
    }
    bool haveFormat = false;
    bool haveData = false;
    WavView view;
    std::size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const auto size = static_cast<std::size_t>(read32(bytes, at + 4));
        const auto dataAt = at + 8;
        if (size > bytes.size() - dataAt) {
            throw std::runtime_error("engine returned a truncated WAV chunk");
        }
        if (fourCc(bytes, at, "fmt ")) {
            if (size < 16 || read16(bytes, dataAt) != 1 || read16(bytes, dataAt + 14) != 16) {
                throw std::runtime_error("engine WAV is not 16-bit PCM");
            }
            view.channels = read16(bytes, dataAt + 2);
            view.sampleRate = read32(bytes, dataAt + 4);
            const auto byteRate = read32(bytes, dataAt + 8);
            const auto blockAlign = read16(bytes, dataAt + 12);
            if (view.channels == 0 || view.sampleRate == 0 ||
                blockAlign != view.channels * 2 || byteRate != view.sampleRate * blockAlign) {
                throw std::runtime_error("engine returned invalid WAV format parameters");
            }
            haveFormat = true;
        } else if (fourCc(bytes, at, "data")) {
            if (haveData) throw std::runtime_error("engine returned multiple WAV data chunks");
            view.pcm = bytes.subspan(dataAt, size);
            haveData = true;
        }
        at = dataAt + size + (size % 2);
    }
    if (!haveFormat || !haveData || view.pcm.empty() ||
        view.pcm.size() % (view.channels * 2) != 0) {
        throw std::runtime_error("engine returned incomplete WAV audio");
    }
    return view;
}

}  // namespace

void WavJoiner::append(const std::span<const std::uint8_t> wav) {
    const auto view = parseWav(wav);
    if (m_pcm.empty()) {
        m_channels = view.channels;
        m_sampleRate = view.sampleRate;
    } else if (m_channels != view.channels || m_sampleRate != view.sampleRate) {
        throw std::runtime_error("engine changed WAV format between text chunks");
    }
    if (m_pcm.size() > kMaxJoinedPcmBytes ||
        view.pcm.size() > kMaxJoinedPcmBytes - m_pcm.size()) {
        throw std::runtime_error("joined long-form WAV exceeds the 256 MiB limit");
    }
    m_pcm.insert(m_pcm.end(), view.pcm.begin(), view.pcm.end());
}

std::vector<std::uint8_t> WavJoiner::finish() const {
    if (m_pcm.empty() || m_channels == 0 || m_sampleRate == 0) {
        throw std::runtime_error("no synthesized WAV audio to join");
    }
    std::vector<std::uint8_t> out(44 + m_pcm.size(), 0);
    std::memcpy(out.data(), "RIFF", 4);
    std::memcpy(out.data() + 8, "WAVEfmt ", 8);
    std::memcpy(out.data() + 36, "data", 4);
    write32(out, 4, static_cast<std::uint32_t>(out.size() - 8));
    write32(out, 16, 16);
    write16(out, 20, 1);
    write16(out, 22, m_channels);
    write32(out, 24, m_sampleRate);
    write32(out, 28, m_sampleRate * m_channels * 2);
    write16(out, 32, m_channels * 2);
    write16(out, 34, 16);
    write32(out, 40, static_cast<std::uint32_t>(m_pcm.size()));
    std::copy(m_pcm.begin(), m_pcm.end(), out.begin() + 44);
    return out;
}

}  // namespace chimpflow
