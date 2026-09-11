#pragma once

#include <algorithm>
#include <string_view>
#include <utility>

namespace bme
{

template <class String>
concept StringViewCompatible = requires(String &&value) { std::basic_string_view{std::forward<String>(value)}; };

template <StringViewCompatible Left, StringViewCompatible Right>
bool ascii_case_insensitive_equal(Left &&left, Right &&right) noexcept
{
    auto left_view  = std::basic_string_view{std::forward<Left>(left)};
    auto right_view = std::basic_string_view{std::forward<Right>(right)};

    return std::ranges::equal(
        left_view, right_view,
        [](auto left_char, auto right_char) noexcept
        {
            auto lowercase = []<class Char>(Char character) noexcept
            { return character >= (Char)'A' && character <= (Char)'Z' ? (Char)(character + ((Char)'a' - (Char)'A')) : character; };

            return lowercase(left_char) == lowercase(right_char);
        }
    );
}

} // namespace bme
