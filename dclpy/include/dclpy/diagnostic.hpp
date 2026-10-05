#ifndef DCLPY_DIAGNOSTIC_HPP_
#define DCLPY_DIAGNOSTIC_HPP_
#include <cstddef>
namespace dclpy {
inline void copy_diagnostic(char (&buffer)[256], const char* message) noexcept {
    std::size_t destination = 0;
    // Copy complete UTF-8 code points into the caller-owned buffer. Invalid
    // diagnostic bytes are replaced, and truncation never splits a code point.
    const auto* source = reinterpret_cast<const unsigned char*>(message);
    while (*source && destination + 1 < sizeof(buffer)) {
        std::size_t length = *source < 0x80 ? 1 : (*source >= 0xC2 && *source <= 0xDF ? 2 :
                              (*source >= 0xE0 && *source <= 0xEF ? 3 :
                              (*source >= 0xF0 && *source <= 0xF4 ? 4 : 0)));
        bool valid = length != 0;
        for (std::size_t index = 1; valid && index < length; ++index)
            valid = source[index] != 0 && (source[index] & 0xC0) == 0x80;
        if (valid && length > 2)
            valid = !(*source == 0xE0 && source[1] < 0xA0) &&
                    !(*source == 0xED && source[1] >= 0xA0) &&
                    !(*source == 0xF0 && source[1] < 0x90) &&
                    !(*source == 0xF4 && source[1] >= 0x90);
        if (!valid) {
            buffer[destination++] = '?';
            ++source;
            continue;
        }
        if (destination + length >= sizeof(buffer)) break;
        for (std::size_t index = 0; index < length; ++index)
            buffer[destination++] = static_cast<char>(*source++);
    }
    buffer[destination] = '\0';
}
}  // namespace dclpy
#endif  // DCLPY_DIAGNOSTIC_HPP_
