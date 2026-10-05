#include "sh1106.hpp"

#include <algorithm>

#include "esp_check.h"
#include "font5x7.hpp"

namespace {

constexpr const char* TAG = "sh1106";

// Addresses these modules ship with (0x3C is by far the most common).
constexpr uint8_t ADDRESSES[] = {0x3C, 0x3D};

constexpr uint8_t CMD = 0x00;
constexpr uint8_t DATA = 0x40;

constexpr uint32_t SCL_HZ = 400'000;
constexpr int TIMEOUT_MS = 100;

// The SH1106 has 132 columns of RAM; the 128 visible ones start at column 2.
// Set this to 0 if your module turns out to be an SSD1306.
constexpr uint8_t COLUMN_OFFSET = 2;

constexpr uint8_t INIT[] = {
    0xAE,        // display off
    0xD5, 0x80,  // clock divide / oscillator
    0xA8, 0x3F,  // multiplex ratio = 64
    0xD3, 0x00,  // display offset
    0x40,        // start line 0
    0xAD, 0x8B,  // DC-DC converter on
    0xA1,        // segment remap (flip horizontally)
    0xC8,        // COM scan direction reversed (flip vertically)
    0xDA, 0x12,  // COM pins configuration
    0x81, 0xCF,  // contrast
    0xD9, 0x22,  // pre-charge period
    0xDB, 0x40,  // VCOMH deselect level
    0xA4,        // display follows RAM
    0xA6,        // normal (not inverted)
    0xAF,        // display on
};

}  // namespace

esp_err_t Sh1106::init(i2c_master_bus_handle_t bus) {
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (uint8_t candidate : ADDRESSES) {
        err = i2c_master_probe(bus, candidate, TIMEOUT_MS);
        if (err == ESP_OK) {
            address_ = candidate;
            break;
        }
    }
    ESP_RETURN_ON_ERROR(err, TAG, "no display answered on 0x3C/0x3D");

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = address_;
    dev_cfg.scl_speed_hz = SCL_HZ;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &dev_), TAG, "add device");

    for (uint8_t byte : INIT) {
        ESP_RETURN_ON_ERROR(command(byte), TAG, "init sequence");
    }
    clear();
    return flush();
}

esp_err_t Sh1106::command(uint8_t byte) {
    const uint8_t packet[] = {CMD, byte};
    return i2c_master_transmit(dev_, packet, sizeof(packet), TIMEOUT_MS);
}

void Sh1106::draw_text(int x, int page, std::string_view text) {
    if (page < 0 || page >= PAGES) {
        return;
    }
    uint8_t* row = &buffer_[page * WIDTH];
    for (char c : text) {
        const uint8_t* glyph = font5x7::glyph(c);
        for (int col = 0; col < font5x7::GLYPH_W; ++col) {
            if (x + col >= 0 && x + col < WIDTH) {
                row[x + col] = glyph[col];
            }
        }
        x += CHAR_W;
    }
}

void Sh1106::draw_hline(int y) {
    if (y < 0 || y >= HEIGHT) {
        return;
    }
    uint8_t* row = &buffer_[(y / 8) * WIDTH];
    const uint8_t mask = 1u << (y % 8);
    for (int x = 0; x < WIDTH; ++x) {
        row[x] |= mask;
    }
}

esp_err_t Sh1106::flush() {
    // The SH1106 has no auto-wrapping across pages, so each page is addressed explicitly.
    std::array<uint8_t, 1 + WIDTH> packet;
    packet[0] = DATA;
    for (int page = 0; page < PAGES; ++page) {
        ESP_RETURN_ON_ERROR(command(0xB0 | page), TAG, "page address");
        ESP_RETURN_ON_ERROR(command(COLUMN_OFFSET & 0x0F), TAG, "column low");
        ESP_RETURN_ON_ERROR(command(0x10 | (COLUMN_OFFSET >> 4)), TAG, "column high");
        std::copy_n(&buffer_[page * WIDTH], WIDTH, packet.begin() + 1);
        ESP_RETURN_ON_ERROR(i2c_master_transmit(dev_, packet.data(), packet.size(), TIMEOUT_MS),
                            TAG, "page data");
    }
    return ESP_OK;
}
