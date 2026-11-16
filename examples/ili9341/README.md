# pixelfont on an ILI9341 TFT

Draws text and bitmaps on a 320x240 colour panel through the
[espressif/esp_lcd_ili9341](https://components.espressif.com/components/espressif/esp_lcd_ili9341)
component.

## How it is wired

The panel takes 16-bit colour pixels, while pixelfont produces one bit per
pixel. The flush callback expands a block: a set pixel becomes the foreground
colour, a clear one the background, and the line goes to
`esp_lcd_panel_draw_bitmap()`:

```c
for (int row = 0; row < h; row++)
{
    const uint8_t *src = bits + row * stride;
    uint16_t *dst = tft->line;

    for (int col = 0; col < w; col++)
        *dst++ = src[col >> 3] & (0x80 >> (col & 7)) ? tft->fg : tft->bg;

    esp_lcd_panel_draw_bitmap(tft->panel, x, y + row, x + w, y + row + 1, tft->line);
}
```

Because only the blocks that are drawn travel to the panel, a frame that
changes a line of text costs a few hundred pixels, not the whole screen.

The same code works with the other `esp_lcd` panel drivers, `esp_lcd_gc9a01`
and `esp_lcd_st7796` among them, only the `esp_lcd_new_panel_*()` call and the
pin configuration macros differ.

## Build and run

```sh
idf.py set-target esp32
idf.py menuconfig     # Example configuration: the SPI pins
idf.py -p /dev/ttyUSB0 flash monitor
```
