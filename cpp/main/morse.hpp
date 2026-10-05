// Morse code timing rules and the symbol -> character translator.
//
// Everything in here is plain C++ with no hardware access, so it can be
// reasoned about (and tested) independently of the button and display.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace morse {

// Length of one Morse "unit". Every other timing is a multiple of this.
// 200 ms is comfortable for a beginner (~6 WPM); lower it to key faster.
inline constexpr int64_t UNIT_US = 200'000;

// Presses or releases shorter than this are treated as contact bounce.
inline constexpr int64_t DEBOUNCE_US = 20'000;

// A press shorter than this is a dot, anything longer is a dash.
// Standard dot = 1 unit, dash = 3 units, so split the difference at 2.
inline constexpr int64_t DASH_THRESHOLD_US = UNIT_US * 2;

// Silence of this long after a release ends the current character (3 units).
inline constexpr int64_t CHAR_GAP_US = UNIT_US * 3;

// Silence of this long after a release ends the current word (7 units).
inline constexpr int64_t WORD_GAP_US = UNIT_US * 7;

// Longest valid pattern we keep. The longest table entries are 6.
inline constexpr size_t MAX_SYMBOLS = 8;

enum class Symbol : uint8_t { Dot, Dash };

constexpr char to_char(Symbol s) { return s == Symbol::Dot ? '.' : '-'; }

// What the button task reports to the decoder.
struct Event {
    enum class Kind : uint8_t {
        Symbol,   // A completed press, already classified as a dot or dash.
        CharEnd,  // The pause after the last press was long enough to end a character.
        WordEnd,  // The pause kept going long enough to also end the word.
    };
    Kind kind;
    Symbol symbol;  // Only meaningful when kind == Kind::Symbol.
};

// Classify how long the button was held.
constexpr Symbol classify_press(int64_t held_us) {
    return held_us < DASH_THRESHOLD_US ? Symbol::Dot : Symbol::Dash;
}

// Translate a recorded dot/dash sequence into an English character.
// Returns std::nullopt if the sequence isn't valid Morse.
std::optional<char> translate(std::span<const Symbol> symbols);

}  // namespace morse
