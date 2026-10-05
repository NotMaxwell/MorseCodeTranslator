//! Minimal async driver for a 128x64 SH1106 OLED over I2C (the 1.3" monochrome
//! OLED found in most Arduino kits). Drawing happens into an in-RAM framebuffer
//! through `embedded-graphics`; [`Sh1106::flush`] pushes it to the panel.

use embedded_graphics::pixelcolor::BinaryColor;
use embedded_graphics::prelude::*;
use embedded_hal_async::i2c::I2c;

pub const WIDTH: usize = 128;
pub const HEIGHT: usize = 64;

/// Framebuffer size in bytes (1 bit per pixel).
pub const BUFFER_LEN: usize = WIDTH * HEIGHT / 8;

/// Addresses these modules ship with (0x3C is by far the most common).
pub const ADDRESSES: [u8; 2] = [0x3C, 0x3D];

const CMD: u8 = 0x00;
const DATA: u8 = 0x40;

/// The SH1106 has 132 columns of RAM; the 128 visible ones start at column 2.
const COLUMN_OFFSET: u8 = 2;

const INIT: &[u8] = &[
    0xAE, // display off
    0xD5, 0x80, // clock divide / oscillator
    0xA8, 0x3F, // multiplex ratio = 64
    0xD3, 0x00, // display offset
    0x40, // start line 0
    0xAD, 0x8B, // DC-DC converter on
    0xA1, // segment remap (flip horizontally)
    0xC8, // COM scan direction reversed (flip vertically)
    0xDA, 0x12, // COM pins configuration
    0x81, 0xCF, // contrast
    0xD9, 0x22, // pre-charge period
    0xDB, 0x40, // VCOMH deselect level
    0xA4, // display follows RAM
    0xA6, // normal (not inverted)
    0xAF, // display on
];

pub struct Sh1106<'a, I> {
    i2c: I,
    address: u8,
    /// Page-major like the controller's RAM: each byte is a vertical strip of
    /// 8 pixels (bit 0 = top), 128 bytes per 8-pixel-tall page.
    buffer: &'a mut [u8; BUFFER_LEN],
}

impl<'a, I: I2c> Sh1106<'a, I> {
    /// Probe the known addresses, then run the init sequence on whichever one answers.
    /// The framebuffer is passed in (usually a `StaticCell`) to keep it off the stack.
    pub async fn new(mut i2c: I, buffer: &'a mut [u8; BUFFER_LEN]) -> Result<Self, I::Error> {
        let mut address = ADDRESSES[0];
        let mut last_err = None;
        for candidate in ADDRESSES {
            // 0xE3 is the SH1106 NOP command, a harmless way to check for an ACK.
            match i2c.write(candidate, &[CMD, 0xE3]).await {
                Ok(()) => {
                    address = candidate;
                    last_err = None;
                    break;
                }
                Err(e) => last_err = Some(e),
            }
        }
        if let Some(e) = last_err {
            return Err(e);
        }

        let mut display = Self {
            i2c,
            address,
            buffer,
        };
        display.clear_buffer();
        for &byte in INIT {
            display.command(byte).await?;
        }
        display.flush().await?;
        Ok(display)
    }

    pub fn address(&self) -> u8 {
        self.address
    }

    async fn command(&mut self, byte: u8) -> Result<(), I::Error> {
        self.i2c.write(self.address, &[CMD, byte]).await
    }

    pub fn clear_buffer(&mut self) {
        self.buffer.fill(0);
    }

    /// Send the framebuffer to the panel, one 128-byte page per I2C write.
    /// The SH1106 has no auto-wrapping across pages, so each page is addressed explicitly.
    pub async fn flush(&mut self) -> Result<(), I::Error> {
        let mut packet = [0u8; 1 + WIDTH];
        packet[0] = DATA;
        for page in 0..HEIGHT / 8 {
            self.command(0xB0 | page as u8).await?; // page address
            self.command(COLUMN_OFFSET & 0x0F).await?; // column low nibble
            self.command(0x10 | (COLUMN_OFFSET >> 4)).await?; // column high nibble
            packet[1..].copy_from_slice(&self.buffer[page * WIDTH..(page + 1) * WIDTH]);
            self.i2c.write(self.address, &packet).await?;
        }
        Ok(())
    }
}

impl<I> OriginDimensions for Sh1106<'_, I> {
    fn size(&self) -> Size {
        Size::new(WIDTH as u32, HEIGHT as u32)
    }
}

impl<I> DrawTarget for Sh1106<'_, I> {
    type Color = BinaryColor;
    type Error = core::convert::Infallible;

    fn draw_iter<P>(&mut self, pixels: P) -> Result<(), Self::Error>
    where
        P: IntoIterator<Item = Pixel<Self::Color>>,
    {
        for Pixel(point, color) in pixels {
            if point.x < 0 || point.y < 0 || point.x >= WIDTH as i32 || point.y >= HEIGHT as i32
            {
                continue;
            }
            let (x, y) = (point.x as usize, point.y as usize);
            let index = (y / 8) * WIDTH + x;
            let mask = 1 << (y % 8);
            match color {
                BinaryColor::On => self.buffer[index] |= mask,
                BinaryColor::Off => self.buffer[index] &= !mask,
            }
        }
        Ok(())
    }
}
