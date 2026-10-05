#![no_std]
#![no_main]
#![deny(
    clippy::mem_forget,
    reason = "mem::forget is generally not safe to do with esp_hal types, especially those \
    holding buffers for the duration of a data transfer."
)]
#![deny(clippy::large_stack_frames)]

use morse_code_translator::morse::{
    self, CHAR_GAP, DEBOUNCE, MAX_SYMBOLS, MorseEvent, Symbol, WORD_GAP,
};
use morse_code_translator::sh1106::{self, Sh1106};
use defmt::{error, info, warn};
use embassy_executor::Spawner;
use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::channel::Channel;
use embassy_sync::mutex::Mutex;
use embassy_sync::signal::Signal;
use embassy_time::{Instant, Timer, WithTimeout};
use embedded_graphics::mono_font::MonoTextStyle;
use embedded_graphics::mono_font::ascii::FONT_6X10;
use embedded_graphics::pixelcolor::BinaryColor;
use embedded_graphics::prelude::*;
use embedded_graphics::primitives::{Line, PrimitiveStyle};
use embedded_graphics::text::{Baseline, Text};
use esp_hal::Async;
use esp_hal::clock::CpuClock;
use esp_hal::gpio::{Input, InputConfig, Pull};
use esp_hal::i2c::master::{Config as I2cConfig, I2c};
use esp_hal::time::Rate;
use esp_hal::timer::timg::TimerGroup;
use heapless::{Deque, String, Vec};
use panic_rtt_target as _;
use static_cell::StaticCell;

// This creates a default app-descriptor required by the esp-idf bootloader.
// For more information see: <https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/app_image_format.html#application-description>
esp_bootloader_esp_idf::esp_app_desc!();

// Screen layout using the 6x10 font on a 128x64 panel:
// 5 rows of text (50 px), a divider, then the in-progress pattern.
const CHAR_W: usize = 6;
const CHAR_H: usize = 10;
const COLS: usize = sh1106::WIDTH / CHAR_W; // 21
const TEXT_ROWS: usize = 5;
/// Decoded characters that fit on screen (one cell is reserved for the cursor).
const TEXT_CAPACITY: usize = COLS * TEXT_ROWS - 1;

/// Everything the display needs to draw, shared between the decoder and display tasks.
struct ScreenState {
    /// Decoded text; the oldest characters scroll off the front when full.
    text: Deque<char, TEXT_CAPACITY>,
    /// Dots and dashes of the character currently being keyed.
    pending: Vec<Symbol, MAX_SYMBOLS>,
}

/// Button task → decoder task.
static EVENTS: Channel<CriticalSectionRawMutex, MorseEvent, 16> = Channel::new();
/// Decoded text buffer and in-progress pattern.
static SCREEN: Mutex<CriticalSectionRawMutex, ScreenState> = Mutex::new(ScreenState {
    text: Deque::new(),
    pending: Vec::new(),
});
/// Decoder task → display task: "something changed, redraw".
/// A Signal coalesces, so a slow redraw never queues up stale frames.
static REDRAW: Signal<CriticalSectionRawMutex, ()> = Signal::new();
/// OLED framebuffer, kept in a static rather than on the display task's stack.
static FRAMEBUFFER: StaticCell<[u8; sh1106::BUFFER_LEN]> = StaticCell::new();

#[allow(
    clippy::large_stack_frames,
    reason = "it's not unusual to allocate larger buffers etc. in main"
)]
#[esp_rtos::main]
async fn main(spawner: Spawner) -> ! {
    // generator version: 1.3.0
    // generator parameters: --chip esp32s3 -o esp32s3-wroom-1-octal-psram -o unstable-hal -o embassy -o probe-rs -o defmt -o panic-rtt-target -o embedded-test -o neovim -o esp

    rtt_target::rtt_init_defmt!();

    let config = esp_hal::Config::default().with_cpu_clock(CpuClock::max());
    let peripherals = esp_hal::init(config);

    // The following pins are used to bootstrap the chip. They are available
    // for use, but check the datasheet of the module for more information on them.
    // - GPIO0
    // - GPIO3
    // - GPIO45
    // - GPIO46
    // These GPIO pins are in use by some feature of the module and should not be used.
    let _ = peripherals.GPIO27;
    let _ = peripherals.GPIO28;
    let _ = peripherals.GPIO29;
    let _ = peripherals.GPIO30;
    let _ = peripherals.GPIO31;
    let _ = peripherals.GPIO32;
    let _ = peripherals.GPIO33;
    let _ = peripherals.GPIO34;
    let _ = peripherals.GPIO35;
    let _ = peripherals.GPIO36;
    let _ = peripherals.GPIO37;

    let timg0 = TimerGroup::new(peripherals.TIMG0);
    let sw_interrupt =
        esp_hal::interrupt::software::SoftwareInterruptControl::new(peripherals.SW_INTERRUPT);
    esp_rtos::start(timg0.timer0, sw_interrupt.software_interrupt0);

    info!("Embassy initialized!");

    // Button is wired between 3.3V and GPIO4, so pull down: idle = low, pressed = high.
    let button = Input::new(
        peripherals.GPIO4,
        InputConfig::default().with_pull(Pull::Down),
    );

    // OLED: SCK/SCL on GPIO1, SDA on GPIO2.
    let i2c = I2c::new(
        peripherals.I2C0,
        I2cConfig::default().with_frequency(Rate::from_khz(400)),
    )
    .unwrap()
    .with_scl(peripherals.GPIO1)
    .with_sda(peripherals.GPIO2)
    .into_async();

    spawner.spawn(button_task(button).unwrap());
    spawner.spawn(decoder_task().unwrap());
    spawner.spawn(display_task(i2c).unwrap());

    loop {
        Timer::after_secs(60).await;
    }
}

/// Times button presses and the gaps between them, and turns them into [`MorseEvent`]s.
#[allow(
    clippy::large_stack_frames,
    reason = "embassy task futures live in static task storage, not on the stack"
)]
#[embassy_executor::task]
async fn button_task(mut button: Input<'static>) {
    loop {
        // Idle until the key goes down.
        button.wait_for_high().await;
        let pressed_at = Instant::now();
        Timer::after(DEBOUNCE).await;

        button.wait_for_low().await;
        let released_at = Instant::now();
        let held = released_at - pressed_at;
        if held < DEBOUNCE * 2 {
            // Too short to be intentional — a glitch rather than a dot.
            continue;
        }

        let symbol = morse::classify_press(held);
        info!("{} ({} ms)", symbol, held.as_millis());
        EVENTS.send(MorseEvent::Symbol(symbol)).await;
        Timer::after(DEBOUNCE).await;

        // Now time the silence. A press before CHAR_GAP is just the gap
        // between dots/dashes of the same character; loop back and time it.
        if button
            .wait_for_high()
            .with_deadline(released_at + CHAR_GAP)
            .await
            .is_ok()
        {
            continue;
        }
        EVENTS.send(MorseEvent::CharEnd).await;

        // Still silent: keep waiting to see if it becomes a word gap.
        if button
            .wait_for_high()
            .with_deadline(released_at + WORD_GAP)
            .await
            .is_ok()
        {
            continue;
        }
        EVENTS.send(MorseEvent::WordEnd).await;
    }
}

/// Collects symbols into a character, translates it on a character gap and
/// appends the result to the text buffer.
#[embassy_executor::task]
async fn decoder_task() {
    loop {
        let event = EVENTS.receive().await;
        {
            let mut screen = SCREEN.lock().await;
            match event {
                MorseEvent::Symbol(symbol) => {
                    if screen.pending.push(symbol).is_err() {
                        warn!("pattern too long, it will decode as '?'");
                    }
                }
                MorseEvent::CharEnd => {
                    let ch = morse::translate(&screen.pending).unwrap_or('?');
                    info!("decoded '{}'", ch);
                    screen.pending.clear();
                    push_char(&mut screen.text, ch);
                }
                MorseEvent::WordEnd => {
                    if screen.text.back().is_some_and(|&c| c != ' ') {
                        push_char(&mut screen.text, ' ');
                    }
                }
            }
        }
        REDRAW.signal(());
    }
}

fn push_char(text: &mut Deque<char, TEXT_CAPACITY>, ch: char) {
    if text.is_full() {
        text.pop_front();
    }
    let _ = text.push_back(ch);
}

/// Owns the OLED and redraws it whenever the decoder signals a change.
#[allow(
    clippy::large_stack_frames,
    reason = "embassy task futures live in static task storage, not on the stack"
)]
#[embassy_executor::task]
async fn display_task(i2c: I2c<'static, Async>) {
    let framebuffer = FRAMEBUFFER.init([0; sh1106::BUFFER_LEN]);
    let mut display = match Sh1106::new(i2c, framebuffer).await {
        Ok(display) => display,
        Err(e) => {
            error!("OLED not found on I2C ({}), running without a display", e);
            return;
        }
    };
    info!("OLED ready at {=u8:#x}", display.address());

    loop {
        // Drawing into RAM is quick, so do it under the lock, then release
        // the lock before the slow I2C flush.
        {
            let screen = SCREEN.lock().await;
            render(&mut display, &screen.text, &screen.pending);
        }
        if let Err(e) = display.flush().await {
            error!("OLED flush failed: {}", e);
        }
        REDRAW.wait().await;
    }
}

fn render<D: DrawTarget<Color = BinaryColor>>(
    display: &mut D,
    text: &Deque<char, TEXT_CAPACITY>,
    pending: &[Symbol],
) {
    let style = MonoTextStyle::new(&FONT_6X10, BinaryColor::On);
    let _ = display.clear(BinaryColor::Off);

    // Wrap the decoded text into rows, with a '_' cursor after the last character.
    let mut line: String<COLS> = String::new();
    let mut row = 0;
    for ch in text.iter().copied().chain(core::iter::once('_')) {
        if line.push(ch).is_err() {
            draw_line(display, &line, row, style);
            row += 1;
            line.clear();
            let _ = line.push(ch);
        }
    }
    draw_line(display, &line, row, style);

    // Divider, then the dots/dashes of the character being keyed.
    let divider_y = (TEXT_ROWS * CHAR_H + 1) as i32;
    let _ = Line::new(
        Point::new(0, divider_y),
        Point::new(sh1106::WIDTH as i32 - 1, divider_y),
    )
    .into_styled(PrimitiveStyle::with_stroke(BinaryColor::On, 1))
    .draw(display);

    let pattern: String<MAX_SYMBOLS> = pending.iter().map(|s| s.as_char()).collect();
    let _ = Text::with_baseline(
        &pattern,
        Point::new(0, divider_y + 2),
        style,
        Baseline::Top,
    )
    .draw(display);
}

fn draw_line<D: DrawTarget<Color = BinaryColor>>(
    display: &mut D,
    line: &str,
    row: usize,
    style: MonoTextStyle<'_, BinaryColor>,
) {
    let _ = Text::with_baseline(line, Point::new(0, (row * CHAR_H) as i32), style, Baseline::Top)
        .draw(display);
}
