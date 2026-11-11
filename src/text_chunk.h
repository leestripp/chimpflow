#ifndef CHIMPFLOW_TEXT_CHUNK_H
#define CHIMPFLOW_TEXT_CHUNK_H

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace chimpflow {

// Split long-form prose without dropping its words. Blank lines delimit
// paragraphs; sentences are packed up to maxBytes, with oversized sentences
// divided at word boundaries. Valid UTF-8 code points are never cut in half.
std::vector<std::string> splitLongForm(std::string_view input,
                                       std::size_t maxBytes = 1200);

}  // namespace chimpflow

#endif  // CHIMPFLOW_TEXT_CHUNK_H
