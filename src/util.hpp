#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace bme
{

template <class String>
concept StringViewCompatible = requires(String &&value) { std::basic_string_view{std::forward<String>(value)}; };

template <StringViewCompatible Left, StringViewCompatible Right>
bool ascii_case_insensitive_equal(Left &&left, Right &&right) noexcept
{
    std::basic_string_view left_view{std::forward<Left>(left)};
    std::basic_string_view right_view{std::forward<Right>(right)};

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

// String overloads mutate in place.
// View overloads return a narrowed view that callers must capture. Both use whitespace by default and accept predicates for custom matching.
template <class Pred>
void ltrim(std::string &input, Pred &&predicate)
{
    input.erase(input.begin(), std::ranges::find_if_not(input, std::ref(predicate)));
}

inline void ltrim(std::string &input)
{
    ltrim(input, [](char character) noexcept { return std::isspace((std::uint8_t)character) != 0; });
}

template <class Pred>
void rtrim(std::string &input, Pred &&predicate)
{
    input.erase(std::ranges::find_if_not(input | std::views::reverse, std::ref(predicate)).base(), input.end());
}

inline void rtrim(std::string &input)
{
    rtrim(input, [](char character) noexcept { return std::isspace((std::uint8_t)character) != 0; });
}

template <class Pred>
void trim(std::string &input, Pred &&predicate)
{
    ltrim(input, predicate);
    rtrim(input, std::forward<Pred>(predicate));
}

inline void trim(std::string &input)
{
    ltrim(input);
    rtrim(input);
}

template <class Pred>
std::string_view ltrim(std::string_view input, Pred &&predicate)
{
    auto first = std::ranges::find_if_not(input, std::ref(predicate));
    input.remove_prefix((std::size_t)(first - input.begin()));

    return input;
}

inline std::string_view ltrim(std::string_view input) noexcept
{
    return ltrim(input, [](char character) noexcept { return std::isspace((std::uint8_t)character) != 0; });
}

template <class Pred>
std::string_view rtrim(std::string_view input, Pred &&predicate)
{
    auto last = std::ranges::find_if_not(input | std::views::reverse, std::ref(predicate)).base();
    input.remove_suffix((std::size_t)(input.end() - last));

    return input;
}

inline std::string_view rtrim(std::string_view input) noexcept
{
    return rtrim(input, [](char character) noexcept { return std::isspace((std::uint8_t)character) != 0; });
}

template <class Pred>
std::string_view trim(std::string_view input, Pred &&predicate)
{
    return rtrim(ltrim(input, predicate), std::forward<Pred>(predicate));
}

inline std::string_view trim(std::string_view input) noexcept { return rtrim(ltrim(input)); }

} // namespace bme
