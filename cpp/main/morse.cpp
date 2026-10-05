#include "morse.hpp"

#include <algorithm>
#include <string_view>

namespace morse {
namespace {

struct Entry {
    std::string_view pattern;
    char ch;
};

// International Morse code, written as dot/dash strings.
constexpr Entry TABLE[] = {
    {".-", 'A'},     {"-...", 'B'},   {"-.-.", 'C'},   {"-..", 'D'},    {".", 'E'},
    {"..-.", 'F'},   {"--.", 'G'},    {"....", 'H'},   {"..", 'I'},     {".---", 'J'},
    {"-.-", 'K'},    {".-..", 'L'},   {"--", 'M'},     {"-.", 'N'},     {"---", 'O'},
    {".--.", 'P'},   {"--.-", 'Q'},   {".-.", 'R'},    {"...", 'S'},    {"-", 'T'},
    {"..-", 'U'},    {"...-", 'V'},   {".--", 'W'},    {"-..-", 'X'},   {"-.--", 'Y'},
    {"--..", 'Z'},   {"-----", '0'},  {".----", '1'},  {"..---", '2'},  {"...--", '3'},
    {"....-", '4'},  {".....", '5'},  {"-....", '6'},  {"--...", '7'},  {"---..", '8'},
    {"----.", '9'},  {".-.-.-", '.'}, {"--..--", ','}, {"..--..", '?'}, {".----.", '\''},
    {"-.-.--", '!'}, {"-..-.", '/'},  {"-.--.", '('},  {"-.--.-", ')'}, {".-...", '&'},
    {"---...", ':'}, {"-.-.-.", ';'}, {"-...-", '='},  {".-.-.", '+'},  {"-....-", '-'},
    {"..--.-", '_'}, {".-..-.", '"'}, {".--.-.", '@'},
};

}  // namespace

std::optional<char> translate(std::span<const Symbol> symbols) {
    for (const Entry& entry : TABLE) {
        if (entry.pattern.size() == symbols.size() &&
            std::equal(symbols.begin(), symbols.end(), entry.pattern.begin(),
                       [](Symbol s, char p) { return to_char(s) == p; })) {
            return entry.ch;
        }
    }
    return std::nullopt;
}

}  // namespace morse
