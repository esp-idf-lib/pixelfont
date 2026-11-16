pixelfont: bitmap font renderer for ESP-IDF
============================================

A display wants pixels, a font is a picture of characters. The library turns a
font into data and draws it into whatever the display calls a frame, so the
same code draws any font, at any size and scale.

Features
--------

* Text in UTF-8 with scaling, letter spacing, transparency and inverse video.
* Bitmaps of any size, scaled the same way, so icons live next to the text.
* Three block formats and the page format of the SSD1306 GDDRAM, whose buffer
  the library can write into directly.
* Two ways to draw: into a buffer, or straight to a callback, block by block.
* Fonts as data: bundled Terminus, any BDF or PSF file through
  ``tools/pixelfont.py``, and the same files loadable at runtime.

Bundled fonts
-------------

.. list-table::
    :header-rows: 1

    * - Font
      - Cell
      - Coverage
    * - Terminus Medium 8x16
      - 8x16
      - Latin: U+0020..U+007E, U+00A0..U+00FF
    * - Terminus Medium 12x24
      - 12x24
      - Latin: U+0020..U+007E, U+00A0..U+00FF

``CONFIG_PIXELFONT_FONTS`` lists the fonts to compile in,
``CONFIG_PIXELFONT_DEFAULT_FONT`` picks the one available as
``PIXELFONT_FONT_DEFAULT``. The fonts are under the SIL Open Font License, see
``LICENSE-OFL``; the library is under the MIT license.

Other scripts are a generation away: ``tools/pixelfont.py`` cuts a font by code
point ranges or by the characters of a text, so Cyrillic, Greek or box drawing
cost only what they use. See ``fonts/README.md``.

What a font costs in the image
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A bundled font takes its glyph index and its bitmaps in the firmware only if
the application uses it. The files listed in ``CONFIG_PIXELFONT_FONTS`` are
compiled into the library, but the linker does not pull an object out of a
library when nothing refers to it, and ``--gc-sections`` drops the data that is
linked but never reached. A font that is only selected in the menuconfig,
therefore, does not cost a byte of the flash image; ``nm build/*.elf | grep
pixelfont_font`` shows what really got in.

The ``digits`` example relies on that: it draws with a font loaded from the
file system and never mentions ``PIXELFONT_FONT_DEFAULT``, so its image
contains no built-in font at all, and the only font data on the flash is the
BDF file in the file system image. To make this explicit, or to save the build
time of the font files, set ``CONFIG_PIXELFONT_FONTS`` to an empty string: then
no font is compiled, and ``fonts/pixelfont_fonts.h`` deliberately fails to
compile if something still includes it.

Two ways to draw
----------------

A canvas either has a buffer or a callback. With a buffer the pixels are
written in place, so a text can be drawn over a picture; without an opaque
style only the set pixels of the glyphs touch the buffer. Without a buffer the
renderer composes the drawing in blocks and passes each of them to the
callback, which is the only way with a display that cannot be read back and has
no RAM for a frame.

.. code-block:: c

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

Boxed canvases come from ``pixelfont_canvas_new()`` and go away with
``pixelfont_canvas_free()``.

Clipping
--------

Everything is clipped to the surface: a glyph, a bitmap or a rectangle that
leaves the edge is drawn up to the edge and cut there, so a long string can run
off the screen instead of being dropped as a whole or wrapped. Coordinates may
be negative, which is handy to scroll a line out of view.

Inverse video
-------------

A style with ``opaque`` and ``reversed`` draws the cells filled and the glyphs
as holes in them, so white text on a black panel becomes black text on white,
which is what a highlighted menu item or a selected line looks like.
``pixelfont_draw_bitmap()`` takes the same flag, and ``pixelfont_invert()``
turns a whole buffered canvas inside out, which is the same as a panel with
inverted colours.

Formats
-------

The packing of a buffer and of the blocks in the callback is one of

.. list-table::
    :header-rows: 1

    * - Packing
      - Byte order
      - Bit of the first pixel
    * - ``PIXELFONT_PACK_ROW``
      - rows left to right, bytes across
      - 7
    * - ``PIXELFONT_PACK_PAGE``
      - pages of eight rows, bytes down a column
      - 0 (SSD1306 GDDRAM)
    * - ``PIXELFONT_PACK_COLUMN_MSB``
      - columns top to bottom, bytes down a column
      - 7
    * - ``PIXELFONT_PACK_COLUMN_LSB``
      - columns top to bottom, bytes down a column
      - 0

Drawing
-------

``pixelfont_draw_text()`` walks the string byte by byte, decodes UTF-8 and
replaces a character the font does not have with ``?``, or U+FFFD, or skips it
when the font has neither. ``pixelfont_draw_glyph()`` draws one code point,
``pixelfont_find_glyph()`` looks one up, ``pixelfont_draw_bitmap()`` draws a 1
bit per pixel image and ``pixelfont_fill_rect()`` fills a rectangle. Everything
is clipped against the canvas, so a text may start outside of it.

``pixelfont_set_clip()`` narrows the drawing to a rectangle and
``pixelfont_reset_clip()`` removes it. The rectangle is intersected with the
surface, everything drawn after that stops at its edges, and the pixels outside
of it are not touched, so a text area can be kept apart from the rest of the
picture:

.. code-block:: c

    pixelfont_set_clip(&canvas, 0, 16, 128, 48);

    pixelfont_fill_rect(&canvas, 0, 16, 128, 48, false);  // only the area
    pixelfont_draw_text(font, &canvas, 4, 16, "line 1", NULL);

    pixelfont_reset_clip(&canvas);

The flush callback and ``pixelfont_invert()`` work on the whole canvas.

Scrolling
~~~~~~~~~

A text area scrolls smoothly when its lines are drawn again on every frame one
pixel higher, with the clip rectangle cutting what has left the area and what
has not entered it yet:

.. code-block:: c

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

``pixelfont_scroll_up()`` and ``pixelfont_scroll_down()`` move the pixels of a
rectangle by any number of pixels instead, without redrawing the content, which
suits a picture. The rows that leave the rectangle are lost, the rows that
become free are the background, and the rest of the canvas does not change.
Scrolling needs the canvas buffer, so a canvas that sends the picture to a
driver block by block cannot be scrolled — give the canvas a buffer and send it
with ``pixelfont_flush()``.

``pixelfont_text_width()`` and ``pixelfont_text_height()`` measure a string, so
a layout does not have to count characters. Like the functions that draw, they
return an error code and write the result through a pointer, and everything a
caller passes in is checked for validity.

Fonts at runtime
----------------

``pixelfont_load()`` reads a BDF or PSF file from a file system of ESP-IDF and
builds a font in memory, ``pixelfont_free()`` releases it. The loader takes
BDF, PSF1 and PSF2; compressed files are not unpacked, so mount the file system
as is or convert the font at build time.

Display drivers
---------------

The library produces one bit per pixel, so the driver of the display turns that
into what the panel wants. Two integrations live in ``examples/``: the
SSD1306 panel driver of ESP-IDF, whose page-major memory is exactly
``PIXELFONT_PACK_PAGE``, so the picture travels to the panel without a
conversion, and the colour expansion of a one-bit block into 16-bit lines for
``esp_lcd_ili9341``. The other ``esp_lcd`` panel drivers take the same code.
``examples/digits`` shows the other half of the picture: the font is not a part
of the application, ``tools/pixelfont.py`` cuts the digits of Terminus 16x32
into a BDF, the file goes into the file system image of the flash, and the
application loads it with ``pixelfont_load()`` when the screen needs it.

Examples
--------

Four examples, all built with ESP-IDF the same way:

* ``examples/default`` needs no hardware: it draws a frame on a surface that
  imitates the GDDRAM of a SSD1306 and prints the frame as text. The frame
  shows text, a bitmap, inverse video, clipping at the edges and a
  pixel-by-pixel scroll of a text inside a clipped rectangle.
* ``examples/ssd1306`` drives a real 128x64 OLED through the SSD1306 panel
  driver of ESP-IDF, ``esp_lcd_new_panel_ssd1306()``. The canvas works right in
  the page-major buffer of the panel, so a page goes to the display as it is,
  and the text inside the clipped area scrolls smoothly.
* ``examples/ili9341`` drives a colour SPI panel through
  ``espressif/esp_lcd_ili9341``. The 1-bit canvas is expanded to RGB565 line by
  line, which is all a monochrome renderer needs on a colour display; the frame
  shows scaling, colour changes and inverse video.
* ``examples/digits`` keeps the font out of the firmware: a BDF file in the
  SPIFFS partition is loaded with ``pixelfont_load()``. Four Spleen 32x64
  digits fill the screen, and a digit that changes rolls in and out like the
  wheel of a mechanical counter.

.. code-block:: sh

    cd examples/ssd1306
    idf.py set-target esp32
    idf.py menuconfig     # Example configuration: the I2C pins and the address
    idf.py build
    idf.py -p /dev/ttyUSB0 flash monitor

The pins and the address of the display are in ``menuconfig``, and every
example has a README with the details of its wiring.

API reference
-------------

 .. doxygenfile:: pixelfont.h
