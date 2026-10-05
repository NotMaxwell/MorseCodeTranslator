// Minimal driver for a 128x64 SH1106 OLED over I2C (the 1.3" monochrome OLED
// found in most Arduino kits). Drawing happens into an in-RAM framebuffer;
// Sh1106::flush() pushes it to the panel.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "driver/i2c_master.h"
#include "esp_err.h"

class Sh1106 {
public:
    static constexpr int WIDTH = 128;
    static constexpr int HEIGHT = 64;
    static constexpr int PAGES = HEIGHT / 8;

    // Character cell for draw_text(): 5 px glyph + 1 px spacing, one 8 px page tall.
    static constexpr int CHAR_W = 6;
    static constexpr int COLS = WIDTH / CHAR_W;  // 21

    // Probe the known addresses on `bus`, then run the init sequence on
    // whichever one answers.
    esp_err_t init(i2c_master_bus_handle_t bus);

    uint8_t address() const { return address_; }

    void clear() { buffer_.fill(0); }

    // Draw text with its top-left corner at column `x` of text row `page` (0..7).
    void draw_text(int x, int page, std::string_view text);

    // Draw a full-width horizontal line at pixel row `y`.
    void draw_hline(int y);

    // Send the framebuffer to the panel, one 128-byte page per I2C write.
    esp_err_t flush();

private:
    esp_err_t command(uint8_t byte);

    i2c_master_dev_handle_t dev_ = nullptr;
    uint8_t address_ = 0;
    // Page-major like the controller's RAM: each byte is a vertical strip of
    // 8 pixels (bit 0 = top), 128 bytes per 8-pixel-tall page.
    std::array<uint8_t, WIDTH * PAGES> buffer_{};
};
