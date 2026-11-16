# Bundled fonts

The component ships two sizes of Terminus, the console font by Dimitar
Zhechev, cut down to the Latin script: ASCII and the Latin-1 supplement,
192 characters each.

| File | Font | Cell | Coverage |
| --- | --- | --- | --- |
| `font_terminus_8x16.c` | Terminus Medium 8x16 | 8x16 | U+0020..U+007E, U+00A0..U+00FF |
| `font_terminus_12x24.c` | Terminus Medium 12x24 | 12x24 | U+0020..U+007E, U+00A0..U+00FF |

Both are generated from the BDF files of Terminus 4.49 and are distributed
under the SIL Open Font License, see `../LICENSE-OFL`. The C files are
generated, do not edit them.

## Making your own font

Any BDF or PSF file works, compressed with gzip or not, and the draft needs
Python 3 only:

```sh
# see what the file contains
python3 tools/pixelfont.py ter-u16n.bdf --list

# Latin, Greek and Cyrillic, as a C file in the component
python3 tools/pixelfont.py ter-u16n.bdf \
    --ranges 0x20-0x7e,0xa0-0xff,0x370-0x3ff,0x400-0x45f \
    --name pixelfont_font_terminus_8x16_cyrillic \
    -o fonts/font_terminus_8x16_cyrillic.c

# only the characters of a text, which is the way to keep the font small
python3 tools/pixelfont.py myfont.bdf --text labels.txt --name myfont -o fonts/font_myfont.c
```

`--ranges` takes code points and closed intervals of them, separated by
commas, and always keeps `?` and U+FFFD, the characters the renderer
substitutes for what the font does not have. The generated file declares a
`const pixelfont_t` whose name is the `--name` value.

Then add the name to `CONFIG_PIXELFONT_FONTS`, without the `font_` prefix and
the `.c` suffix, for example:

```
terminus_8x16,terminus_12x24,terminus_8x16_cyrillic
```

The build fails with a readable message when a name has no file.

## Terminus

Terminus was designed by Dimitar Zhechev for the Linux console. The source
files are at <https://terminus-font.sourceforge.net/>, the Debian package
`xfonts-terminus` carries the PCF files, which `pcf2bdf` converts to BDF:

```sh
dpkg-deb -x xfonts-terminus_4.49-1_all.deb .
pcf2bdf -o ter-u16n.bdf < xfonts-terminus/usr/share/fonts/X11/misc/ter-u16n_unicode.pcf.gz
```
