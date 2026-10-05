//! Morse code timing rules and the symbol → character translator.
//!
//! Everything in here is plain `core` code with no hardware access, so it can
//! be reasoned about (and tested) independently of the button and display.

use embassy_time::Duration;

/// Length of one Morse "unit". Every other timing is a multiple of this.
/// 200 ms is comfortable for a beginner (~6 WPM); lower it to key faster.
pub const UNIT: Duration = Duration::from_millis(200);

/// Presses or releases shorter than this are treated as contact bounce.
pub const DEBOUNCE: Duration = Duration::from_millis(20);

/// A press shorter than this is a dot, anything longer is a dash.
/// Standard dot = 1 unit, dash = 3 units, so split the difference at 2.
pub const DASH_THRESHOLD: Duration = Duration::from_millis(UNIT.as_millis() * 2);

/// Silence of this long after a release ends the current character (3 units).
pub const CHAR_GAP: Duration = Duration::from_millis(UNIT.as_millis() * 3);

/// Silence of this long after a release ends the current word (7 units).
pub const WORD_GAP: Duration = Duration::from_millis(UNIT.as_millis() * 7);

/// Longest valid pattern we keep. The longest entries in [`TABLE`] are 6.
pub const MAX_SYMBOLS: usize = 8;

#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub enum Symbol {
    Dot,
    Dash,
}

impl Symbol {
    pub const fn as_char(self) -> char {
        match self {
            Symbol::Dot => '.',
            Symbol::Dash => '-',
        }
    }
}

/// What the button task reports to the decoder.
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub enum MorseEvent {
    /// A completed press, already classified as a dot or dash.
    Symbol(Symbol),
    /// The pause after the last press was long enough to end a character.
    CharEnd,
    /// The pause kept going long enough to also end the word.
    WordEnd,
}

/// Classify how long the button was held.
pub fn classify_press(held: Duration) -> Symbol {
    if held < DASH_THRESHOLD {
        Symbol::Dot
    } else {
        Symbol::Dash
    }
}

/// International Morse code, written as dot/dash strings.
const TABLE: &[(&str, char)] = &[
    (".-", 'A'),
    ("-...", 'B'),
    ("-.-.", 'C'),
    ("-..", 'D'),
    (".", 'E'),
    ("..-.", 'F'),
    ("--.", 'G'),
    ("....", 'H'),
    ("..", 'I'),
    (".---", 'J'),
    ("-.-", 'K'),
    (".-..", 'L'),
    ("--", 'M'),
    ("-.", 'N'),
    ("---", 'O'),
    (".--.", 'P'),
    ("--.-", 'Q'),
    (".-.", 'R'),
    ("...", 'S'),
    ("-", 'T'),
    ("..-", 'U'),
    ("...-", 'V'),
    (".--", 'W'),
    ("-..-", 'X'),
    ("-.--", 'Y'),
    ("--..", 'Z'),
    ("-----", '0'),
    (".----", '1'),
    ("..---", '2'),
    ("...--", '3'),
    ("....-", '4'),
    (".....", '5'),
    ("-....", '6'),
    ("--...", '7'),
    ("---..", '8'),
    ("----.", '9'),
    (".-.-.-", '.'),
    ("--..--", ','),
    ("..--..", '?'),
    (".----.", '\''),
    ("-.-.--", '!'),
    ("-..-.", '/'),
    ("-.--.", '('),
    ("-.--.-", ')'),
    (".-...", '&'),
    ("---...", ':'),
    ("-.-.-.", ';'),
    ("-...-", '='),
    (".-.-.", '+'),
    ("-....-", '-'),
    ("..--.-", '_'),
    (".-..-.", '"'),
    (".--.-.", '@'),
];

/// Translate a recorded dot/dash sequence into an English character.
/// Returns `None` if the sequence isn't valid Morse.
pub fn translate(symbols: &[Symbol]) -> Option<char> {
    TABLE.iter().find_map(|&(pattern, ch)| {
        let matches = pattern.len() == symbols.len()
            && pattern
                .chars()
                .zip(symbols)
                .all(|(p, s)| p == s.as_char());
        matches.then_some(ch)
    })
}
