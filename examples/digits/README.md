# Big digits loaded from a file

Shows a counter in Spleen 32x64 on a 128x64 OLED: four digits of 32 pixels fill
the width of the screen exactly. The digits are not compiled into the application: the font is a BDF file
in the file system of the storage partition, and the application loads it at
run time with `pixelfont_load()` and drops it with `pixelfont_free()` when the
screen switches to something else.

## The font

The file was made from the Spleen font with the utility of the component, and
only the ten digits are kept, which is 7 KiB instead of the whole face:

```sh
python3 ../../tools/pixelfont.py spleen-32x64.bdf \
    --ranges 0x30-0x39 --name spleen_32x64 --bdf-out fonts/digits.bdf
```

`spleen-32x64.bdf` comes from the [Spleen](https://github.com/fcambus/spleen)
project by Frederic Cambus. The build does not need it: the result is in
`fonts/digits.bdf`. To make the font from the original during the build, pass
the file to it:

```sh
idf.py -DDIGITS_BDF=/path/to/spleen-32x64.bdf build
```

Then the utility runs before the file system image is built, and the image gets
the fresh `digits.bdf`.

## How it is wired

The example does not use the fonts of the component: the firmware has no
built-in font at all, and the only font data in the flash is this BDF file
(about 7 KiB). See the "What a font costs in the image" section of the
component README.

The example uses the SSD1306 panel driver of ESP-IDF, and the canvas works right
in the page-major buffer of the display, so a page goes to the panel as it is:

```c
pixelfont_t *big = NULL;
ESP_ERROR_CHECK(pixelfont_load("/storage/digits.bdf", &big));
pixelfont_draw_text(big, &canvas, 32, 16, "1234", NULL);
// ... flush the pages ...
pixelfont_free(big);
```

The file system is a SPIFFS image of the `storage` partition, created by
`spiffs_create_partition_image()` from the `fonts` directory, so the font is
part of the flash image and no card or network is needed.

## Build and run

```sh
idf.py set-target esp32
idf.py menuconfig     # Example configuration: the I2C pins and the address
idf.py -p /dev/ttyUSB0 flash monitor
```
