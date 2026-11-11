#include "text_chunk.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace chimpflow {
namespace {

bool isSpace(const char ch) {
    return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

std::string collapseWhitespace(const std::string_view input) {
    std::string out;
    out.reserve(input.size());
    bool pendingSpace = false;
    for (const char ch : input) {
        if (isSpace(ch)) {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) out += ' ';
        pendingSpace = false;
        out += ch;
    }
    return out;
}

bool isContinuation(const char ch) {
    return (static_cast<unsigned char>(ch) & 0xc0) == 0x80;
}

bool isEllipsis(const std::string_view text, const std::size_t at) {
    return at + 2 < text.size() &&
           static_cast<unsigned char>(text[at]) == 0xe2 &&
           static_cast<unsigned char>(text[at + 1]) == 0x80 &&
           static_cast<unsigned char>(text[at + 2]) == 0xa6;
}

std::size_t utf8Boundary(const std::string_view text, std::size_t cut) {
    if (text.empty()) return 0;
    cut = std::clamp(cut, std::size_t{1}, text.size());
    while (cut > 0 && cut < text.size() && isContinuation(text[cut])) --cut;
    if (cut == 0) {
        // A code point wider than the byte budget must remain whole. Scan to
        // its end so oversized chunks still make forward progress.
        cut = 1;
        while (cut < text.size() && isContinuation(text[cut])) ++cut;
    }
    return cut;
}

bool isClosingPunctuation(const char ch) {
    switch (ch) {
        case '\'': case '"': case ')': case ']': case '}': return true;
        default: return false;
    }
}

// Return the byte after a sentence ending at start, or npos when the
// punctuation is internal (for example the decimal point in 3.14).
std::size_t sentenceEnd(const std::string_view text, const std::size_t start) {
    std::size_t end = start + (isEllipsis(text, start) ? 3 : 1);
    if (text[start] == '.' && start > 0 && end < text.size() &&
        std::isdigit(static_cast<unsigned char>(text[start - 1])) != 0 &&
        std::isdigit(static_cast<unsigned char>(text[end])) != 0) {
        return std::string_view::npos;
    }
    static constexpr std::string_view rightQuote = "\xE2\x80\x9D";  // ”
    static constexpr std::string_view rightSingle = "\xE2\x80\x99"; // ’
    while (end < text.size()) {
        if (isClosingPunctuation(text[end])) {
            ++end;
        } else if (text.substr(end).starts_with(rightQuote) ||
                   text.substr(end).starts_with(rightSingle)) {
            end += 3;
        } else {
            break;
        }
    }
    if (end < text.size() && !isSpace(text[end])) return std::string_view::npos;
    return end;
}

std::vector<std::string> sentences(const std::string& paragraph) {
    std::vector<std::string> out;
    std::size_t begin = 0;
    for (std::size_t i = 0; i < paragraph.size(); ++i) {
        const bool asciiTerminal = paragraph[i] == '.' || paragraph[i] == '!' ||
                                   paragraph[i] == '?';
        const bool ellipsis = isEllipsis(paragraph, i);
        if (!asciiTerminal && !ellipsis) continue;
        const auto end = sentenceEnd(paragraph, i);
        if (end == std::string_view::npos) continue;
        out.push_back(paragraph.substr(begin, end - begin));
        begin = end;
        while (begin < paragraph.size() && paragraph[begin] == ' ') ++begin;
        i = end - 1;
    }
    if (begin < paragraph.size()) out.push_back(paragraph.substr(begin));
    return out;
}

void appendOversized(std::vector<std::string>& out, std::string_view rest,
                     const std::size_t maxBytes) {
    while (rest.size() > maxBytes) {
        std::size_t cut = rest.rfind(' ', maxBytes);
        if (cut == std::string_view::npos || cut < maxBytes / 3) {
            cut = utf8Boundary(rest, maxBytes);
        }
        out.emplace_back(rest.substr(0, cut));
        rest.remove_prefix(cut);
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    }
    if (!rest.empty()) out.emplace_back(rest);
}

void appendParagraph(std::vector<std::string>& out, const std::string& paragraph,
                     const std::size_t maxBytes) {
    std::string current;
    for (const auto& sentence : sentences(paragraph)) {
        if (sentence.size() > maxBytes) {
            if (!current.empty()) {
                out.push_back(std::move(current));
                current.clear();
            }
            appendOversized(out, sentence, maxBytes);
            continue;
        }
        const auto extra = current.empty() ? sentence.size() : sentence.size() + 1;
        if (!current.empty() && current.size() + extra > maxBytes) {
            out.push_back(std::move(current));
            current.clear();
        }
        if (!current.empty()) current += ' ';
        current += sentence;
    }
    if (!current.empty()) out.push_back(std::move(current));
}

}  // namespace

std::vector<std::string> splitLongForm(const std::string_view input,
                                       const std::size_t maxBytes) {
    if (maxBytes == 0) throw std::invalid_argument("chunk byte budget must be positive");
    std::vector<std::string> out;
    std::size_t begin = 0;
    while (begin < input.size()) {
        while (begin < input.size() && isSpace(input[begin])) ++begin;
        if (begin == input.size()) break;
        std::size_t end = begin;
        while (end < input.size()) {
            if (input[end] != '\n') {
                ++end;
                continue;
            }
            std::size_t next = end + 1;
            while (next < input.size() && input[next] != '\n' && isSpace(input[next])) ++next;
            if (next < input.size() && input[next] == '\n') break;
            end = next;
        }
        const auto paragraph = collapseWhitespace(input.substr(begin, end - begin));
        if (!paragraph.empty()) appendParagraph(out, paragraph, maxBytes);
        begin = end + 1;
    }
    return out;
}

}  // namespace chimpflow
