# pixelfont

[![Build with ESP-IDF v6.0](https://github.com/esp-idf-lib/pixelfont/actions/workflows/build.yml/badge.svg)](https://github.com/esp-idf-lib/pixelfont/actions/workflows/build.yml)
[![Build docs](https://github.com/esp-idf-lib/pixelfont/actions/workflows/build-docs.yml/badge.svg)](https://github.com/esp-idf-lib/pixelfont/actions/workflows/build-docs.yml)
[![ESP-IDF component](https://img.shields.io/badge/ESP--IDF-component-green)](https://components.espressif.com/components/esp-idf-lib/pixelfont)
[![License](https://img.shields.io/github/license/esp-idf-lib/pixelfont.svg)](https://github.com/esp-idf-lib/pixelfont/blob/master/LICENSE)

Bitmap font renderer for monochrome and colour displays.

[Documentation](https://esp-idf-lib.github.io/pixelfont/) |
[Repository](https://github.com/esp-idf-lib/pixelfont) |
[Issues](https://github.com/esp-idf-lib/pixelfont/issues) |
[Discussion](https://github.com/esp-idf-lib/core/discussions) |
[Component registry](https://components.espressif.com/components/esp-idf-lib/pixelfont)

A display wants pixels, a font is a picture of characters: the library turns a
font into data and draws it into whatever the display calls a frame. There is
no code per font and no table per character: a glyph is 16 bytes of bitmap in
flash, the whole Latin set of a 8x16 console font is five kilobytes, and the
same code draws a different font, size and scale.

## Features

* Text in UTF-8, with scaling, letter spacing, transparency and inverse video.
* Bitmaps of any size, scaled the same way, so icons live next to the text.
* Three block formats plus the page format of the SSD1306 GDDRAM, whose
  buffer the library can write into directly.
* Two ways to draw: into a buffer, or straight to a callback, block by block,
  which suits displays that cannot be read back.
* Fonts as data: the bundled ones are Terminus, any BDF or PSF file can be
  turned into a C file by `tools/pixelfont.py`, and the same files can be
  loaded at runtime from a file system.
* No dynamic memory when drawing into a buffer: the canvas is a struct and the
  buffer belongs to the caller.

## Bundled fonts

| Font | Cell | Coverage |
| --- | --- | --- |
| Terminus Medium 8x16 | 8x16 | Latin: U+0020..U+007E, U+00A0..U+00FF |
| Terminus Medium 12x24 | 12x24 | Latin: U+0020..U+007E, U+00A0..U+00FF |

Which of them are compiled in is set by `CONFIG_PIXELFONT_FONTS`, the default
one by the `CONFIG_PIXELFONT_DEFAULT_FONT` choice. The fonts are distributed
under the SIL Open Font License, see `LICENSE-OFL`, the library itself under
the MIT license.

Other scripts are a generation away: `tools/pixelfont.py` cuts a font by code
point ranges or by the characters of a text, so Cyrillic, Greek or box drawing
cost only what they use. See [fonts/README.md](fonts/README.md).

### What a font costs in the image

A bundled font takes its glyph index and its bitmaps in the firmware only if
the application uses it. The files listed in `CONFIG_PIXELFONT_FONTS` are
compiled into the library, but the linker does not pull an object out of a
library when nothing refers to it, and `--gc-sections` drops the data that is
linked but never reached. A font that is only selected in the menuconfig,
therefore, does not cost a byte of the flash image; `nm build/*.elf | grep
pixelfont_font` shows what really got in.

The `digits` example relies on that: it draws with a font loaded from the file
system and never mentions `PIXELFONT_FONT_DEFAULT`, so its image contains no
built-in font at all, and the only font data on the flash is the BDF file in
the file system image. To make this explicit, or to save the build time of the
font files, set `CONFIG_PIXELFONT_FONTS` to an empty string: then no font is
compiled, and `fonts/pixelfont_fonts.h` deliberately fails to compile if
something still includes it.

## Two ways to draw

A canvas either has a buffer or a callback:

```c
// the buffer of the display driver, the format of the SSD1306 GDDRAM
static uint8_t framebuffer[128 * 64 / 8];

pixelfont_canvas_t canvas = {
    .width = 128,
    .height = 64,
    .pack = PIXELFONT_PACK_PAGE,
    .data = framebuffer,
    .size = sizeof(framebuffer),
};

pixelfont_style_t style = PIXELFONT_STYLE_DEFAULT;
style.scale = 2;

esp_err_t err = pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, 8, 4, "Hello", &style);
// ... and let the driver send the buffer: ssd1306_flush(&display)
```

With a buffer the pixels are written in place, so a text can be drawn over a
picture: without an opaque style only the set pixels of the glyphs touch the
buffer. Without a buffer the renderer composes the drawing in blocks and
passes each of them to the callback, which is the only way with a display that
cannot be read back and has no RAM for a frame:

```c
static esp_err_t flush(void *ctx, int x, int y, int w, int h, const uint8_t *bits)
{
    // bits is w * h pixels in the packing of the canvas, row by row
    return my_display_write_block(x, y, w, h, bits);
}

pixelfont_canvas_t canvas = {
    .width = 128,
    .height = 64,
    .pack = PIXELFONT_PACK_COLUMN_LSB,
    .flush = flush,
};
```

Boxed canvases come from `pixelfont_canvas_new()` and go away with
`pixelfont_canvas_free()`.

## Clipping

Everything is clipped to the surface: a glyph, a bitmap or a rectangle that
leaves the edge is drawn up to the edge and cut there, so a long string can run
off the screen instead of being dropped as a whole or wrapped. Coordinates may
be negative, which is handy to scroll a line out of view.

`pixelfont_set_clip()` narrows the drawing further to a rectangle, and
`pixelfont_reset_clip()` removes it. The rectangle is intersected with the
surface, everything the renderer draws after that — text, glyphs, bitmaps,
fills and scrolling — stops at its edges, and the pixels outside of it are not
touched, so a text area can be kept apart from the rest of the picture:

```c
pixelfont_set_clip(&canvas, 0, 16, 128, 48);

pixelfont_fill_rect(&canvas, 0, 16, 128, 48, false);  // only the area
pixelfont_draw_text(font, &canvas, 4, 16, "line 1", NULL);

pixelfont_reset_clip(&canvas);
```

The flush callback and `pixelfont_invert()` work on the whole canvas.

## Inverse video

A style with `opaque` and `reversed` draws the cells filled and the glyphs as
holes in them: white text on a black panel becomes black text on white, which
is what a highlighted menu item or a selected line looks like.

```c
pixelfont_style_t style = PIXELFONT_STYLE_DEFAULT;
style.opaque = true;
style.reversed = true;
pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, 8, 8, "Selected", &style);
```

Bitmaps take the same flag, `pixelfont_draw_bitmap(..., opaque, reversed)`, and
`pixelfont_invert()` turns a whole buffered canvas inside out, which is the
same as a panel with inverted colours and is useful to blink a frame or to
switch a display to a dark theme.

## Formats

The packing of a buffer and of the blocks in the callback is one of

| Packing | Byte order | Bit of the leftmost or topmost pixel |
| --- | --- | --- |
| `PIXELFONT_PACK_ROW` | rows left to right, bytes across | 7 |
| `PIXELFONT_PACK_PAGE` | pages of eight rows, bytes down a column | 0 (SSD1306 GDDRAM) |
| `PIXELFONT_PACK_COLUMN_MSB` | columns top to bottom, bytes down a column | 7 |
| `PIXELFONT_PACK_COLUMN_LSB` | columns top to bottom, bytes down a column | 0 |

## Drawing

* `pixelfont_draw_text()` walks the string byte by byte, decodes UTF-8 and
  replaces a character the font does not have with `?`, or U+FFFD, or skips
  it when the font has neither.
* `pixelfont_draw_glyph()` draws one code point, `pixelfont_find_glyph()`
  looks one up and says `ESP_ERR_NOT_FOUND` when the font has no such one.
* `pixelfont_draw_bitmap()` draws a 1 bit per pixel image, the same way as a
  glyph.
* `pixelfont_fill_rect()`, `pixelfont_clear()` and `pixelfont_flush()` round
  the set out.
* `pixelfont_text_width()` and `pixelfont_text_height()` measure a string, so
  a layout does not have to count characters; they write the result through a
  pointer, like the functions that draw.

Everything is clipped against the canvas, so a text may start outside of it.

### Scrolling

A text area scrolls smoothly when its lines are drawn again on every frame one
pixel higher, with the clip rectangle cutting what has left the area and what
has not entered it yet:

```c
pixelfont_set_clip(&canvas, 0, 16, 128, 48);

for (int step = 0; step < 3 * 16; step++)
{
    int offset = step % 16, first = step / 16;

    pixelfont_fill_rect(&canvas, 0, 16, 128, 48, false);   // the area only

    // one line more than fits: the extra one enters through the bottom edge
    for (int row = 0; row <= 3; row++)
    {
        char text[16];
        snprintf(text, sizeof(text), "Line %d", first + row + 1);
        pixelfont_draw_text(font, &canvas, 4, 16 + row * 16 - offset, text, NULL);
    }

    pixelfont_flush(&canvas);
    vTaskDelay(pdMS_TO_TICKS(25));
}

pixelfont_reset_clip(&canvas);
```

`pixelfont_scroll_up()` and `pixelfont_scroll_down()` move the pixels of a
rectangle by any number of pixels instead, without redrawing the content, which
suits a picture. The rows that leave the rectangle are lost, the rows that
become free are the background, and the rest of the canvas does not change.
Scrolling needs the canvas buffer, so a canvas that sends the picture to a
driver block by block cannot be scrolled — give the canvas a buffer and send it
with `pixelfont_flush()`.

## Fonts at runtime

`pixelfont_load()` reads a BDF or PSF file from a file system of ESP-IDF and
builds a font in memory, `pixelfont_free()` releases it:

```c
pixelfont_t *font = NULL;
ESP_ERROR_CHECK(pixelfont_load("/spiffs/ter-u16n.bdf", &font));
ESP_ERROR_CHECK(pixelfont_draw_text(font, &canvas, 0, 0, "Hello", NULL));
pixelfont_free(font);
```

The loader takes BDF, PSF1 and PSF2, compressed files are not unpacked: mount
the file system, or convert the font at build time.

## API

| Function | Purpose |
| --- | --- |
| `pixelfont_canvas_new()`, `pixelfont_canvas_free()` | Allocate and free a canvas with a buffer |
| `pixelfont_canvas_size()` | Size of a buffer in bytes |
| `pixelfont_clear()`, `pixelfont_fill_rect()` | Fill the canvas or a rectangle |
| `pixelfont_set_clip()`, `pixelfont_reset_clip()` | Limit the drawing to a rectangle |
| `pixelfont_draw_text()` | Draw a UTF-8 string |
| `pixelfont_draw_glyph()` | Draw one code point |
| `pixelfont_draw_bitmap()` | Draw a bitmap |
| `pixelfont_text_width()`, `pixelfont_text_height()` | Measure a string |
| `pixelfont_find_glyph()` | Look a code point up |
| `pixelfont_flush()` | Send a buffered canvas to the display |
| `pixelfont_load()`, `pixelfont_free()` | Load a font from a file, free it |

## Display drivers

The library produces one bit per pixel, so the driver of the display is the one
that turns that into what the panel wants. Two integrations are in
`examples/`:

* `examples/ssd1306` draws on a 128x64 monochrome OLED with the SSD1306 panel
  driver of ESP-IDF, `esp_lcd_panel_ssd1306`. The controller wants one bit per
  pixel in page-major order, which is exactly `PIXELFONT_PACK_PAGE`, so the
  canvas lives in the buffer of the application and each page goes to the panel
  as it is, without a conversion.
* `examples/digits` loads a font at run time: the digits of Terminus 16x32 are
  cut from the original by `tools/pixelfont.py` into a BDF, the file goes into
  the file system image of the flash, and the application calls
  `pixelfont_load()` and `pixelfont_free()` around the screen that needs them.
* `examples/ili9341` draws on a 320x240 colour panel through
  `espressif/esp_lcd_ili9341`. The flush callback expands a one-bit block into
  16-bit colour lines and sends them with `esp_lcd_panel_draw_bitmap()`. The
  other `esp_lcd` panel drivers, such as `esp_lcd_gc9a01` and
  `esp_lcd_st7796`, take the same code with a different `esp_lcd_new_panel_*()`
  call.

Both kinds of panel need about thirty lines of glue and nothing else: the
canvas is what the driver does not have.

## Examples

Four examples, all built with ESP-IDF the same way:

* [examples/default](examples/default) needs no hardware: it draws a frame on a
  surface that imitates the GDDRAM of a SSD1306 and prints the frame as text.
  The frame shows text, a bitmap, inverse video, clipping at the edges and a
  pixel-by-pixel scroll of a text inside a clipped rectangle.
* [examples/ssd1306](examples/ssd1306) drives a real 128x64 OLED through the
  SSD1306 panel driver of ESP-IDF, `esp_lcd_new_panel_ssd1306()`. The canvas
  works right in the page-major buffer of the panel, so a page goes to the
  display as it is, and the text inside the clipped area scrolls smoothly.
* [examples/ili9341](examples/ili9341) drives a colour SPI panel through
  `espressif/esp_lcd_ili9341`. The 1-bit canvas is expanded to RGB565 line by
  line, which is all a monochrome renderer needs on a colour display; the frame
  shows scaling, colour changes and inverse video.
* [examples/digits](examples/digits) keeps the font out of the firmware: a BDF
  file in the SPIFFS partition is loaded with `pixelfont_load()`. Four Spleen
  32x64 digits fill the screen, and a digit that changes rolls in and out like
  the wheel of a mechanical counter.

```sh
cd examples/ssd1306
idf.py set-target esp32
idf.py menuconfig     # Example configuration: the I2C pins and the address
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The pins and the address of the display are in `menuconfig`, and every example
has a README with the details of its wiring.

## Installation

```sh
idf.py add-dependency "esp-idf-lib/pixelfont^0.1.0"
```

## Support

* [FAQ](https://esp-idf-lib.github.io/faq.html)
* [Documentation](https://esp-idf-lib.github.io/pixelfont/)
* [Issues](https://github.com/esp-idf-lib/pixelfont/issues)

## Contributing

Please read [CONTRIBUTING.md](https://github.com/esp-idf-lib/core/blob/main/CONTRIBUTING.md).
