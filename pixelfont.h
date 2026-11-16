/*
 * pixelfont.h
 *
 * Pixel font renderer for monochrome and colour displays.
 *
 * Copyright (c) 2026 Ruslan V. Uss <unclerus@gmail.com>
 *
 * MIT Licensed as described in the file LICENSE
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Layout of the bitmap handed to the flush callback
 *
 * The renderer composes a block of pixels in a small buffer and passes it to
 * the driver through ::pixelfont_flush_t. The layout of that buffer depends on
 * the display: a controller that takes bytes over SPI wants them row by row,
 * a SSD1306 wants a byte per column with the topmost pixel in the lowest bit.
 */
typedef enum
{
    PIXELFONT_PACK_ROW = 0,    //!< Row-major, bit 7 is the leftmost pixel
    PIXELFONT_PACK_PAGE,       //!< Page-major, bit 0 is the topmost pixel (SSD1306 GDDRAM)
    PIXELFONT_PACK_COLUMN_MSB, //!< Column-major, bit 7 is the topmost pixel
    PIXELFONT_PACK_COLUMN_LSB, //!< Column-major, bit 0 is the topmost pixel
} pixelfont_pack_t;

/**
 * @brief Write a block of pixels to the display
 *
 * Called by the renderer with a block already clipped against the canvas and
 * composed in the packing of the canvas. Every bit of the block is defined:
 * a set bit is a foreground pixel, a clear bit is a background pixel, so the
 * driver may write the block as is without clearing the window first.
 *
 * @param ctx Context of the canvas
 * @param x Left edge of the block, in pixels
 * @param y Top edge of the block, in pixels
 * @param w Width of the block, in pixels
 * @param h Height of the block, in pixels
 * @param bits The pixels: ((w + 7) / 8) * h bytes for PIXELFONT_PACK_ROW,
 *             ((h + 7) / 8) * w bytes for the column packings
 * @return `ESP_OK` on success, an error code of the bus otherwise
 */
typedef esp_err_t (*pixelfont_flush_t)(void *ctx, int x, int y, int w, int h, const uint8_t *bits);

/**
 * @brief A drawing surface
 *
 * There are two ways to use it. With a buffer (::pixelfont_canvas_new(), or the
 * buffer of the display driver attached by hand) the pixels are written into
 * that buffer and the driver sends it to the display when it likes, so a text
 * may be drawn over a picture. Without a buffer the renderer composes the
 * pixels of a glyph or of a bitmap and hands them over to the flush callback
 * block by block, which suits displays that cannot be read back.
 *
 * The size is used for clipping and may be zero, in which case nothing is
 * clipped.
 */
typedef struct
{
    int width;                //!< Width of the surface, pixels, 0 to disable clipping
    int height;               //!< Height of the surface, pixels, 0 to disable clipping
    pixelfont_pack_t pack;    //!< Layout of the buffer and of the blocks passed to flush()
    pixelfont_flush_t flush;  //!< Writes a block to the display, may be NULL with a buffer
    void *ctx;                //!< Passed to flush()
    uint8_t *data;            //!< Buffer in the packing, may be NULL
    size_t size;              //!< Size of the buffer, bytes
    int clip_x;               //!< Left edge of the clip rectangle, pixels
    int clip_y;               //!< Top edge of the clip rectangle, pixels
    int clip_w;               //!< Width of the clip rectangle, 0 to disable clipping
    int clip_h;               //!< Height of the clip rectangle, 0 to disable clipping
} pixelfont_canvas_t;

/**
 * @brief One glyph of a font
 *
 * The glyphs of a font are sorted by code point, so the renderer finds them
 * with a binary search.
 */
typedef struct
{
    uint32_t codepoint; //!< Unicode code point
    uint32_t offset;    //!< Offset of the bitmap in ::pixelfont_t.bitmaps, bytes
    uint8_t advance;    //!< Advance width of the glyph, pixels
    uint8_t reserved[3]; //!< Alignment
} pixelfont_glyph_t;

/**
 * @brief A font
 *
 * The bitmaps are one bit per pixel, row-major, the most significant bit is
 * the leftmost pixel, every row occupies (width + 7) / 8 bytes. A font is
 * either generated at build time into the flash (see tools/pixelfont.py) or
 * loaded from a file at run time with ::pixelfont_load().
 *
 * The cell is the bounding box of the font: the widest glyph and the tallest
 * one. Glyphs are placed in the cell by the loader, so the renderer never
 * needs the per-glyph offsets of the source format.
 */
typedef struct
{
    uint8_t width;              //!< Width of the cell, pixels
    uint8_t height;             //!< Height of the cell, pixels
    int8_t ascent;              //!< Pixels above the baseline
    uint16_t count;             //!< Number of glyphs
    const pixelfont_glyph_t *glyphs; //!< Glyphs, sorted by code point
    const uint8_t *bitmaps;     //!< Glyph bitmaps
    void *owned;                //!< Heap block of a font loaded from a file, NULL otherwise
} pixelfont_t;

/**
 * @brief How to draw a text
 */
typedef struct
{
    uint8_t scale;   //!< Integer scale, 1 to 8
    uint8_t spacing; //!< Extra pixels between the cells, not scaled
    bool opaque;     //!< Paint the background of the cells as well
    bool reversed;   //!< Swap the foreground and the background
} pixelfont_style_t;

/**
 * @brief Default style: scale 1, no spacing, transparent background
 */
#define PIXELFONT_STYLE_DEFAULT { .scale = 1, .spacing = 0, .opaque = false, .reversed = false }

/**
 * @brief Size of a canvas buffer, bytes
 *
 * Useful to share a buffer with a display driver that allocates it itself.
 *
 * @param[in] width Width, pixels
 * @param[in] height Height, pixels
 * @param[in] pack Packing of the buffer
 * @param[out] size Size in bytes
 * @return `ESP_OK` on success, `ESP_ERR_INVALID_ARG` for a bad argument
 */
esp_err_t pixelfont_canvas_size(int width, int height, pixelfont_pack_t pack, size_t *size);

/**
 * @brief Allocate a canvas with a buffer
 *
 * The buffer is cleared to the background.
 *
 * @param width Width, pixels
 * @param height Height, pixels
 * @param pack Packing of the buffer
 * @param[out] canvas The canvas
 * @return `ESP_OK` on success, `ESP_ERR_INVALID_ARG` for a bad argument,
 *         `ESP_ERR_NO_MEM` if there is not enough memory
 */
esp_err_t pixelfont_canvas_new(int width, int height, pixelfont_pack_t pack,
                               pixelfont_canvas_t **canvas);

/**
 * @brief Free a canvas allocated with ::pixelfont_canvas_new()
 *
 * @param canvas Canvas, NULL is allowed
 */
void pixelfont_canvas_free(pixelfont_canvas_t *canvas);

/**
 * @brief Limit the drawing to a rectangle
 *
 * Everything the renderer draws afterwards is cut at the edges of the
 * rectangle: the text that leaves it is clipped, the pixels outside it are not
 * touched. The rectangle is intersected with the canvas, so a text drawn partly
 * or completely outside of it draws nothing there. This is what makes a line of
 * a terminal enter from below the text area and leave above it a pixel at a
 * time, when the text is drawn at a moving offset.
 *
 * ::pixelfont_fill_rect() and ::pixelfont_scroll_up() obey the rectangle too,
 * so a rectangle may be cleared without touching the rest of the picture.
 * ::pixelfont_flush() and ::pixelfont_invert() work on the whole canvas.
 *
 * @param canvas Canvas
 * @param x Left edge, pixels
 * @param y Top edge, pixels
 * @param width Width, pixels, must be positive
 * @param height Height, pixels, must be positive
 * @return `ESP_OK` on success, `ESP_ERR_INVALID_ARG` for a bad argument
 */
esp_err_t pixelfont_set_clip(pixelfont_canvas_t *canvas, int x, int y, int width, int height);

/**
 * @brief Remove the rectangle set by ::pixelfont_set_clip()
 *
 * @param canvas Canvas
 * @return `ESP_OK` on success, `ESP_ERR_INVALID_ARG` for a bad argument
 */
esp_err_t pixelfont_reset_clip(pixelfont_canvas_t *canvas);

/**
 * @brief Invert the pixels of a canvas
 *
 * Turns the set pixels clear and the clear pixels set, the same result as a
 * display with an inverted panel. Works on a canvas with a buffer only.
 *
 * @param canvas Canvas with a buffer
 * @return `ESP_OK` on success, `ESP_ERR_INVALID_ARG` for a bad argument,
 *         `ESP_ERR_INVALID_STATE` when the canvas has no buffer
 */
esp_err_t pixelfont_invert(const pixelfont_canvas_t *canvas);

/**
 * @brief Send the buffer of a canvas to the display
 *
 * Calls ::pixelfont_flush_t for horizontal strips of up to eight rows, which is
 * one page of a SSD1306. Does nothing when the canvas has no callback.
 *
 * @param canvas Canvas with a buffer
 * @return `ESP_OK` on success, `ESP_ERR_INVALID_ARG` for a bad argument,
 *         an error code of the driver otherwise
 */
esp_err_t pixelfont_flush(const pixelfont_canvas_t *canvas);

/**
 * @brief Draw a UTF-8 string
 *
 * The glyphs are drawn left to right starting at (@p x, @p y), which is the
 * top left corner of the first cell. Code points that the font does not have
 * are substituted: '?' when the font has it, U+FFFD otherwise, and skipped if
 * the font has neither.
 *
 * Without an opaque style only the set pixels of the glyphs touch the canvas,
 * so a text can be drawn over a picture.
 *
 * @param font Font
 * @param canvas Drawing surface
 * @param x Left edge of the first cell, pixels
 * @param y Top edge of the cells, pixels
 * @param text NUL-terminated UTF-8 string
 * @param style Drawing style, NULL for ::PIXELFONT_STYLE_DEFAULT
 * @return `ESP_OK` on success, `ESP_ERR_INVALID_ARG` for a bad argument,
 *         an error code of the driver otherwise
 */
esp_err_t pixelfont_draw_text(const pixelfont_t *font, const pixelfont_canvas_t *canvas,
                              int x, int y, const char *text, const pixelfont_style_t *style);

/**
 * @brief Draw one code point
 *
 * @param font Font
 * @param canvas Drawing surface
 * @param x Left edge of the cell, pixels
 * @param y Top edge of the cell, pixels
 * @param codepoint Unicode code point
 * @param style Drawing style, NULL for ::PIXELFONT_STYLE_DEFAULT
 * @return `ESP_OK` on success, `ESP_ERR_INVALID_ARG` for a bad argument,
 *         `ESP_ERR_NOT_FOUND` if the font has no such glyph,
 *         an error code of the driver otherwise
 */
esp_err_t pixelfont_draw_glyph(const pixelfont_t *font, const pixelfont_canvas_t *canvas,
                               int x, int y, uint32_t codepoint, const pixelfont_style_t *style);

/**
 * @brief Draw a one-bit bitmap
 *
 * The bitmap is row-major, the most significant bit is the leftmost pixel,
 * every row occupies (width + 7) / 8 bytes.
 *
 * @param canvas Drawing surface
 * @param x Left edge, pixels
 * @param y Top edge, pixels
 * @param width Width of the bitmap, pixels
 * @param height Height of the bitmap, pixels
 * @param bits The bitmap
 * @param scale Integer scale, 1 to 8
 * @param opaque Paint the clear pixels of the bitmap as the background
 * @return `ESP_OK` on success, `ESP_ERR_INVALID_ARG` for a bad argument,
 *         an error code of the driver otherwise
 */
esp_err_t pixelfont_draw_bitmap(const pixelfont_canvas_t *canvas,
                                int x, int y, int width, int height, const uint8_t *bits, uint8_t scale,
                                bool opaque, bool reversed);

/**
 * @brief Fill a rectangle
 *
 * @param canvas Drawing surface
 * @param x Left edge, pixels
 * @param y Top edge, pixels
 * @param width Width, pixels
 * @param height Height, pixels
 * @param on `true` for the foreground, `false` for the background
 * @return `ESP_OK` on success, `ESP_ERR_INVALID_ARG` for a bad argument,
 *         an error code of the driver otherwise
 */
esp_err_t pixelfont_fill_rect(const pixelfont_canvas_t *canvas,
                              int x, int y, int width, int height, bool on);

/**
 * @brief Clear the whole canvas
 *
 * Equivalent to filling the whole canvas with the background.
 *
 * @param[in] canvas Canvas
 * @return `ESP_OK` on success, `ESP_ERR_INVALID_ARG` for a bad argument,
 *         an error code of the driver otherwise
 */
esp_err_t pixelfont_clear(const pixelfont_canvas_t *canvas);

/**
 * @brief Scroll a rectangle up by pixels
 *
 * The pixels of the rectangle move up, the rows that leave it at the top are
 * lost, and the rows that become free at the bottom are filled with the
 * background. Useful for a text area that behaves like a terminal: draw a new
 * line in the free rows, then scroll by the height of the line.
 *
 * The canvas must have its own buffer, so a canvas that sends the picture to a
 * driver block by block cannot be scrolled.
 *
 * @param[in] canvas Canvas with a buffer
 * @param x Left edge of the rectangle, pixels
 * @param y Top edge of the rectangle, pixels
 * @param width Width of the rectangle, pixels
 * @param height Height of the rectangle, pixels
 * @param pixels Distance, pixels; 0 does nothing, `height` or more clears the
 *               rectangle
 * @return `ESP_OK` on success, `ESP_ERR_INVALID_ARG` for a bad argument,
 *         `ESP_ERR_INVALID_STATE` when the canvas has no buffer
 */
esp_err_t pixelfont_scroll_up(const pixelfont_canvas_t *canvas,
                              int x, int y, int width, int height, int pixels);

/**
 * @brief Scroll a rectangle down by pixels
 *
 * The mirror image of pixelfont_scroll_up(): the pixels move down, the rows
 * that leave the rectangle at the bottom are lost, and the free rows at the
 * top become the background.
 *
 * @param[in] canvas Canvas with a buffer
 * @param x Left edge of the rectangle, pixels
 * @param y Top edge of the rectangle, pixels
 * @param width Width of the rectangle, pixels
 * @param height Height of the rectangle, pixels
 * @param pixels Distance, pixels; 0 does nothing, `height` or more clears the
 *               rectangle
 * @return `ESP_OK` on success, `ESP_ERR_INVALID_ARG` for a bad argument,
 *         `ESP_ERR_INVALID_STATE` when the canvas has no buffer
 */
esp_err_t pixelfont_scroll_down(const pixelfont_canvas_t *canvas,
                                int x, int y, int width, int height, int pixels);

/**
 * @brief Width of a text in pixels
 *
 * @param[in] font Font
 * @param[in] text NUL-terminated UTF-8 string, may be empty
 * @param[in] style Drawing style, NULL for ::PIXELFONT_STYLE_DEFAULT
 * @param[out] width Width in pixels, 0 for an empty string
 * @return `ESP_OK` on success, `ESP_ERR_INVALID_ARG` for a bad argument,
 *         `ESP_ERR_INVALID_SIZE` for a bad scale
 */
esp_err_t pixelfont_text_width(const pixelfont_t *font, const char *text,
                               const pixelfont_style_t *style, int *width);

/**
 * @brief Height of the cells of a font, scaled
 *
 * @param[in] font Font
 * @param[in] style Drawing style, NULL for ::PIXELFONT_STYLE_DEFAULT
 * @param[out] height Height in pixels
 * @return `ESP_OK` on success, `ESP_ERR_INVALID_ARG` for a bad argument,
 *         `ESP_ERR_INVALID_SIZE` for a bad scale
 */
esp_err_t pixelfont_text_height(const pixelfont_t *font, const pixelfont_style_t *style,
                                int *height);

/**
 * @brief Find a glyph
 *
 * @param[in] font Font
 * @param[in] codepoint Unicode code point
 * @param[out] glyph The glyph
 * @return `ESP_OK` on success, `ESP_ERR_INVALID_ARG` for a bad argument,
 *         `ESP_ERR_NOT_FOUND` if the font has no such code point
 */
esp_err_t pixelfont_find_glyph(const pixelfont_t *font, uint32_t codepoint,
                               const pixelfont_glyph_t **glyph);

/**
 * @brief Load a font from a file
 *
 * The format is recognised by the content, the usual extensions are only a
 * hint. Supported formats:
 *
 * - BDF (X11 bitmap distribution format), text: the cell is taken from
 *   FONTBOUNDINGBOX, the advance from DWIDTH, and the glyph bitmaps are placed
 *   in the cell. Proportional fonts are supported: every glyph keeps its own
 *   advance.
 * - PSF1 and PSF2 (Linux console fonts), binary: the cell is 8 pixels wide for
 *   PSF1 and arbitrary for PSF2, the advance is the cell width. When the font
 *   carries a Unicode table, the glyphs are indexed by it, otherwise the glyph
 *   index is the code point (so a PSF1 font without a table covers Latin-1).
 *
 * Compressed files (`.bdf.gz`, `.psfu.gz`) are not read: unpack them first,
 * or use tools/pixelfont.py, which accepts gzip and generates the font for
 * the build.
 *
 * PCF is not read directly: convert it with `pcf2bdf` first.
 *
 * The whole file is read into the heap, so the font lives until
 * ::pixelfont_free(). A file that does not fit into the heap is rejected with
 * `ESP_ERR_NO_MEM`: use tools/pixelfont.py to cut the font down to the
 * characters that are actually needed.
 *
 * @param[in] path Path of the file
 * @param[out] font Loaded font
 * @return `ESP_OK` on success, `ESP_ERR_INVALID_ARG` for a bad argument,
 *         `ESP_ERR_NOT_FOUND` if the file cannot be opened,
 *         `ESP_ERR_INVALID_RESPONSE` if the content is not a supported font,
 *         `ESP_ERR_NO_MEM` if there is not enough memory
 */
esp_err_t pixelfont_load(const char *path, pixelfont_t **font);

/**
 * @brief Free a font loaded with ::pixelfont_load()
 *
 * Fonts generated at build time live in the flash and must not be freed.
 *
 * @param font Font, NULL is allowed
 */
void pixelfont_free(pixelfont_t *font);

#ifdef __cplusplus
}
#endif
