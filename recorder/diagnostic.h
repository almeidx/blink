#pragma once

#include <cstdio>
#include <string>
#include <string_view>

namespace blink::recorder {

// One line on stderr, prefixed with the program name.
inline void diagnostic(std::string_view text)
{
    std::string line = "blink-recorder: ";
    line += text;
    line += '\n';
    std::fputs(line.c_str(), stderr);
}

} // namespace blink::recorder
