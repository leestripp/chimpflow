#include "audio_format.h"
#include "test_util.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> makeToneWav() {
    constexpr std::uint32_t rate = 24000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bits = 16;
    constexpr std::uint32_t samples = rate / 20;  // 50 ms keeps this test quick.
    constexpr std::uint32_t dataBytes = samples * channels * bits / 8;
    constexpr std::uint32_t byteRate = rate * channels * bits / 8;
    constexpr std::uint16_t blockAlign = channels * bits / 8;

    std::vector<std::uint8_t> wav(44 + dataBytes, 0);
    std::memcpy(wav.data(), "RIFF", 4);
    std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
    std::memcpy(wav.data() + 36, "data", 4);
    const std::uint32_t riffBytes = static_cast<std::uint32_t>(wav.size() - 8);
    const std::uint32_t formatBytes = 16;
    const std::uint16_t pcm = 1;
    std::memcpy(wav.data() + 4, &riffBytes, 4);
    std::memcpy(wav.data() + 16, &formatBytes, 4);
    std::memcpy(wav.data() + 20, &pcm, 2);
    std::memcpy(wav.data() + 22, &channels, 2);
    std::memcpy(wav.data() + 24, &rate, 4);
    std::memcpy(wav.data() + 28, &byteRate, 4);
    std::memcpy(wav.data() + 32, &blockAlign, 2);
    std::memcpy(wav.data() + 34, &bits, 2);
    std::memcpy(wav.data() + 40, &dataBytes, 4);
    // Silence is valid PCM and avoids platform-specific math in this unit test.
    return wav;
}

int runTests() {
    const auto wav = makeToneWav();
    const auto mp3 = chimpflow::encodeMp3(wav, "/usr/bin/ffmpeg");
    CHECK(mp3.size() > 100);
    if (mp3.size() >= 3) {
        const bool id3 = std::memcmp(mp3.data(), "ID3", 3) == 0;
        const bool frame = mp3[0] == 0xff && (mp3[1] & 0xe0) == 0xe0;
        CHECK(id3 || frame);
    }

    bool rejectedEmpty = false;
    try {
        (void)chimpflow::encodeMp3({}, "/usr/bin/ffmpeg");
    } catch (const std::runtime_error&) {
        rejectedEmpty = true;
    }
    CHECK(rejectedEmpty);

    bool rejectedInvalidWav = false;
    try {
        const std::array<std::uint8_t, 16> invalid{};
        (void)chimpflow::encodeMp3(invalid, "/usr/bin/ffmpeg");
    } catch (const std::runtime_error&) {
        rejectedInvalidWav = true;
    }
    CHECK(rejectedInvalidWav);

    bool rejectedMissingEncoder = false;
    try {
        (void)chimpflow::encodeMp3(wav, "/definitely/not/a/real/ffmpeg");
    } catch (const std::runtime_error&) {
        rejectedMissingEncoder = true;
    }
    CHECK(rejectedMissingEncoder);
    return 0;
}

}  // namespace

namespace {
int runJoinerTests() {
    const auto wav = makeToneWav();
    chimpflow::WavJoiner joiner;
    joiner.append(wav);
    joiner.append(wav);
    const auto joined = joiner.finish();
    CHECK(joiner.pcmBytes() == 2 * (wav.size() - 44));
    CHECK(joined.size() == 44 + 2 * (wav.size() - 44));
    CHECK(std::memcmp(joined.data(), "RIFF", 4) == 0);

    bool rejectedMismatch = false;
    try {
        auto otherRate = wav;
        const std::uint32_t rate = 48000;
        const std::uint32_t byteRate = 96000;
        std::memcpy(otherRate.data() + 24, &rate, 4);
        std::memcpy(otherRate.data() + 28, &byteRate, 4);
        joiner.append(otherRate);
    } catch (const std::runtime_error&) {
        rejectedMismatch = true;
    }
    CHECK(rejectedMismatch);

    bool rejectedInvalid = false;
    try {
        const std::array<std::uint8_t, 16> invalid{};
        chimpflow::WavJoiner invalidJoiner;
        invalidJoiner.append(invalid);
    } catch (const std::runtime_error&) {
        rejectedInvalid = true;
    }
    CHECK(rejectedInvalid);

    bool rejectedEmpty = false;
    try {
        chimpflow::WavJoiner emptyJoiner;
        (void)emptyJoiner.finish();
    } catch (const std::runtime_error&) {
        rejectedEmpty = true;
    }
    CHECK(rejectedEmpty);
    return 0;
}
}  // namespace

int main() {
    runTests();
    runJoinerTests();
    return test::summary("unit_audio_format");
}

