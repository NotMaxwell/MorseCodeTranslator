// Morse code key -> text translator for ESP32-S3, ESP-IDF + FreeRTOS port.
//
//   button_task --queue<morse::Event>--> decoder_task --mutex + notify--> display_task
//
// Wiring: button between 3.3V and GPIO4, SH1106 OLED with SCL on GPIO1 and SDA on GPIO2.

#include <algorithm>
#include <array>
#include <span>
#include <string>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "morse.hpp"
#include "sh1106.hpp"

namespace {

constexpr const char* TAG = "morse";

constexpr gpio_num_t BUTTON_PIN = GPIO_NUM_4;
constexpr gpio_num_t SCL_PIN = GPIO_NUM_1;
constexpr gpio_num_t SDA_PIN = GPIO_NUM_2;

// Screen layout using the 5x7 font in 6x8 cells on a 128x64 panel:
// 6 rows of text (pages 0-5), a divider, then the in-progress pattern (page 7).
constexpr int TEXT_ROWS = 6;
constexpr int DIVIDER_Y = TEXT_ROWS * 8 + 3;
constexpr int PATTERN_PAGE = 7;
// Decoded characters that fit on screen (one cell is reserved for the cursor).
constexpr size_t TEXT_CAPACITY = Sh1106::COLS * TEXT_ROWS - 1;

// Everything the display needs to draw, shared between the decoder and display tasks.
struct ScreenState {
    // Decoded text; the oldest characters scroll off the front when full.
    std::string text;
    // Dots and dashes of the character currently being keyed.
    std::array<morse::Symbol, morse::MAX_SYMBOLS> pending{};
    size_t pending_len = 0;
};

ScreenState screen;
// Guards `screen`.
SemaphoreHandle_t screen_mutex;
// Button task -> decoder task.
QueueHandle_t events;

TaskHandle_t button_task_handle;
// Decoder task -> display task: "something changed, redraw". Task
// notifications coalesce, so a slow redraw never queues up stale frames.
TaskHandle_t display_task_handle;

Sh1106 display;

// Any edge on the button just wakes the button task, which re-reads the pin.
void IRAM_ATTR button_isr(void*) {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(button_task_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

constexpr int64_t NO_DEADLINE = -1;

// Block until the button reads `level`. Returns false if `deadline_us`
// (in esp_timer_get_time() time) passes first.
bool wait_for_level(int level, int64_t deadline_us) {
    while (gpio_get_level(BUTTON_PIN) != level) {
        TickType_t ticks = portMAX_DELAY;
        if (deadline_us != NO_DEADLINE) {
            const int64_t remaining_us = deadline_us - esp_timer_get_time();
            if (remaining_us <= 0) {
                return false;
            }
            ticks = std::max<TickType_t>(1, pdMS_TO_TICKS((remaining_us + 999) / 1000));
        }
        // An edge that fired between the level check and here is still
        // counted in the notification, so it can't be missed.
        ulTaskNotifyTake(pdTRUE, ticks);
    }
    return true;
}

void send_event(morse::Event::Kind kind, morse::Symbol symbol = morse::Symbol::Dot) {
    const morse::Event event{kind, symbol};
    xQueueSend(events, &event, portMAX_DELAY);
}

// Times button presses and the gaps between them, and turns them into morse::Events.
void button_task(void*) {
    while (true) {
        // Idle until the key goes down.
        wait_for_level(1, NO_DEADLINE);
        const int64_t pressed_at = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(morse::DEBOUNCE_US / 1000));

        wait_for_level(0, NO_DEADLINE);
        const int64_t released_at = esp_timer_get_time();
        const int64_t held = released_at - pressed_at;
        if (held < morse::DEBOUNCE_US * 2) {
            // Too short to be intentional, a glitch rather than a dot.
            continue;
        }

        const morse::Symbol symbol = morse::classify_press(held);
        ESP_LOGI(TAG, "%c (%lld ms)", morse::to_char(symbol), held / 1000);
        send_event(morse::Event::Kind::Symbol, symbol);
        vTaskDelay(pdMS_TO_TICKS(morse::DEBOUNCE_US / 1000));

        // Now time the silence. A press before CHAR_GAP is just the gap
        // between dots/dashes of the same character; loop back and time it.
        if (wait_for_level(1, released_at + morse::CHAR_GAP_US)) {
            continue;
        }
        send_event(morse::Event::Kind::CharEnd);

        // Still silent: keep waiting to see if it becomes a word gap.
        if (wait_for_level(1, released_at + morse::WORD_GAP_US)) {
            continue;
        }
        send_event(morse::Event::Kind::WordEnd);
    }
}

void push_char(std::string& text, char ch) {
    if (text.size() >= TEXT_CAPACITY) {
        text.erase(0, 1);
    }
    text.push_back(ch);
}

// Collects symbols into a character, translates it on a character gap and
// appends the result to the text buffer.
void decoder_task(void*) {
    morse::Event event;
    while (true) {
        xQueueReceive(events, &event, portMAX_DELAY);

        xSemaphoreTake(screen_mutex, portMAX_DELAY);
        switch (event.kind) {
            case morse::Event::Kind::Symbol:
                if (screen.pending_len < screen.pending.size()) {
                    screen.pending[screen.pending_len++] = event.symbol;
                } else {
                    ESP_LOGW(TAG, "pattern too long, it will decode as '?'");
                }
                break;
            case morse::Event::Kind::CharEnd: {
                const char ch =
                    morse::translate(std::span(screen.pending.data(), screen.pending_len))
                        .value_or('?');
                ESP_LOGI(TAG, "decoded '%c'", ch);
                screen.pending_len = 0;
                push_char(screen.text, ch);
                break;
            }
            case morse::Event::Kind::WordEnd:
                if (!screen.text.empty() && screen.text.back() != ' ') {
                    push_char(screen.text, ' ');
                }
                break;
        }
        xSemaphoreGive(screen_mutex);

        xTaskNotifyGive(display_task_handle);
    }
}

void render(const ScreenState& state) {
    display.clear();

    // Wrap the decoded text into rows, with a '_' cursor after the last character.
    const std::string with_cursor = state.text + '_';
    for (int row = 0; row < TEXT_ROWS; ++row) {
        const size_t start = static_cast<size_t>(row) * Sh1106::COLS;
        if (start >= with_cursor.size()) {
            break;
        }
        display.draw_text(0, row, std::string_view(with_cursor).substr(start, Sh1106::COLS));
    }

    // Divider, then the dots/dashes of the character being keyed.
    display.draw_hline(DIVIDER_Y);
    std::string pattern;
    for (size_t i = 0; i < state.pending_len; ++i) {
        pattern.push_back(morse::to_char(state.pending[i]));
    }
    display.draw_text(0, PATTERN_PAGE, pattern);
}

// Owns the OLED and redraws it whenever the decoder signals a change.
void display_task(void*) {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = SDA_PIN;
    bus_cfg.scl_io_num = SCL_PIN;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    if (const esp_err_t err = display.init(bus); err != ESP_OK) {
        ESP_LOGE(TAG, "OLED not found on I2C (%s), running without a display",
                 esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "OLED ready at 0x%02x", display.address());

    while (true) {
        // Drawing into RAM is quick, so do it under the lock, then release
        // the lock before the slow I2C flush.
        xSemaphoreTake(screen_mutex, portMAX_DELAY);
        render(screen);
        xSemaphoreGive(screen_mutex);

        if (const esp_err_t err = display.flush(); err != ESP_OK) {
            ESP_LOGE(TAG, "OLED flush failed: %s", esp_err_to_name(err));
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

}  // namespace

extern "C" void app_main() {
    screen.text.reserve(TEXT_CAPACITY);
    screen_mutex = xSemaphoreCreateMutex();
    events = xQueueCreate(16, sizeof(morse::Event));

    xTaskCreate(decoder_task, "decoder", 3072, nullptr, 5, nullptr);
    xTaskCreate(display_task, "display", 4096, nullptr, 4, &display_task_handle);
    // Highest priority so press/release timing isn't delayed by a display flush.
    xTaskCreate(button_task, "button", 3072, nullptr, 6, &button_task_handle);

    // Button is wired between 3.3V and GPIO4, so pull down: idle = low, pressed = high.
    gpio_config_t button_cfg = {};
    button_cfg.pin_bit_mask = 1ULL << BUTTON_PIN;
    button_cfg.mode = GPIO_MODE_INPUT;
    button_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    button_cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    button_cfg.intr_type = GPIO_INTR_ANYEDGE;
    ESP_ERROR_CHECK(gpio_config(&button_cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_PIN, button_isr, nullptr));

    ESP_LOGI(TAG, "ready, key away (unit = %lld ms)", morse::UNIT_US / 1000);
}
