#include "../src/text_chunk.h"
#include "test_util.h"

#include <stdexcept>
#include <string>
#include <vector>

using chimpflow::splitLongForm;

int main() {
    CHECK((splitLongForm("Hello world.") == std::vector<std::string>{"Hello world."}));
    CHECK(splitLongForm(" \n\t ").empty());

    const auto paragraphs = splitLongForm("First paragraph.\n\nSecond paragraph.", 100);
    CHECK((paragraphs == std::vector<std::string>{"First paragraph.", "Second paragraph."}));

    const auto packed = splitLongForm("One. Two. Three.", 12);
    CHECK((packed == std::vector<std::string>{"One. Two.", "Three."}));

    const auto words = splitLongForm("aaaa bbbb cccc", 11);
    CHECK((words == std::vector<std::string>{"aaaa bbbb", "cccc"}));

    const std::string unicode = "éééééé";
    const auto utf8 = splitLongForm(unicode, 7);
    std::string joined;
    for (const auto& chunk : utf8) {
        CHECK(!chunk.empty());
        CHECK((static_cast<unsigned char>(chunk.front()) & 0xc0) != 0x80);
        joined += chunk;
    }
    CHECK(joined == unicode);

    for (std::size_t budget = 1; budget <= 4; ++budget) {
        const auto tiny = splitLongForm("😀😀", budget);
        CHECK((tiny == std::vector<std::string>{"😀", "😀"}));
    }

    const auto ellipsis = splitLongForm("Wait… Next.", 10);
    CHECK((ellipsis == std::vector<std::string>{"Wait…", "Next."}));

    const auto quotes = splitLongForm("Pi is 3.14. “Exactly!” Next.", 100);
    CHECK(quotes.size() == 1);
    const auto sentences = splitLongForm("Pi is 3.14. “Exactly!” Next.", 20);
    CHECK((sentences == std::vector<std::string>{"Pi is 3.14.", "“Exactly!” Next."}));

    CHECK((splitLongForm("One\r\nline.\r\n\r\nNext paragraph.") ==
           std::vector<std::string>{"One line.", "Next paragraph."}));

    bool rejectedZero = false;
    try {
        (void)splitLongForm("text", 0);
    } catch (const std::invalid_argument&) {
        rejectedZero = true;
    }
    CHECK(rejectedZero);
    return test::summary("text chunking");
}
