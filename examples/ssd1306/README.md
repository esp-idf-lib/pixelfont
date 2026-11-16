# pixelfont on a SSD1306 OLED

Draws text and a bitmap on a 128x64 monochrome OLED with the SSD1306 panel
driver of ESP-IDF itself, `esp_lcd_panel_ssd1306`. No extra component is
needed: the driver is a part of `esp_lcd` and talks to the panel over I2C.

## How it is wired

The controller takes one bit per pixel in the order of its own memory: a byte
per column, bit 0 is the topmost row of the eight-row page. That is exactly what
pixelfont writes with `PIXELFONT_PACK_PAGE`, so the canvas works right in the
buffer of the application and a page goes to the panel without any conversion:

```c
static uint8_t framebuffer[DISPLAY_WIDTH * DISPLAY_HEIGHT / 8];

pixelfont_canvas_t canvas = {
    .width = 128, .height = 64,
    .pack = PIXELFONT_PACK_PAGE,
    .data = framebuffer, .size = sizeof(framebuffer),
};

// ... draw ...

for (int y = 0; y < 64; y += 8)
    esp_lcd_panel_draw_bitmap(panel, 0, y, 128, y + 8, framebuffer + (y / 8) * 128);
```

The panel driver sends the columns of a page in the order they are in memory,
so the bytes of the page travel as they are. Only the pages that changed have
to be sent, so a blinking cursor or a counter costs one page of 128 bytes, not
the whole screen.

The rest is the usual `esp_lcd` setup: `i2c_new_master_bus()`,
`esp_lcd_new_panel_io_i2c()` with the control byte of the SSD1306 in bit 6,
`esp_lcd_new_panel_ssd1306()`, then `reset`, `init` and `disp_on_off`.

The example also shows that the controller can invert the picture itself:
`esp_lcd_panel_invert_color(panel, true)` does the same as
`pixelfont_invert()` but costs no traffic at all.

## Build and run

```sh
idf.py set-target esp32
idf.py menuconfig     # Example configuration: the I2C pins and the address
idf.py -p /dev/ttyUSB0 flash monitor
```

The address is 0x3c when the D/C pin of the module is tied to ground and 0x3d
when it is tied to VCC.
