# The example

The example draws a frame on a surface that imitates the GDDRAM of a SSD1306
display and prints the frame as text, so it runs anywhere: a real display is
not needed to see what the library does with the pixels.

## What it draws

* A one pixel frame around the 128x64 surface.
* The word "Pixel" in the bundled 8x16 font, twice the size.
* A line of text in the same font, at the normal size.
* A filled rectangle with an 8x8 smiley bitmap on it, twice the size.
* When the 12x24 font is compiled in, the same drawing with that font.

The frame is printed twice, two pixels per character: `#` for both pixels set,
`+` for one of the two, `.` for none.

## Build and run

```sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Nothing is connected to the board: the display, its bus and its address are
all in the code, and the only output is the log.

## Expected output

```
################################################################
+..............................................................+
+..............................................................+
...
Done. 611 pixels set in 1024 bytes.
```

The number of the set pixels and the frame itself depend on the fonts that are
compiled in and on the text.

## With a real display

The buffer of a SSD1306 driver has exactly the layout of
`PIXELFONT_PACK_PAGE`, so the canvas can be attached to it and the driver
flushes the frame when it is ready:

```c
pixelfont_canvas_t canvas = {
    .width = display.width,
    .height = display.height,
    .pack = PIXELFONT_PACK_PAGE,
    .data = display.fb,
    .size = SSD1306_FRAMEBUF_SIZE(&display),
};
```

Nothing else changes: the same calls draw into the buffer of the driver.
