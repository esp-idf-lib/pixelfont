/*
 * pixelfont.c
 *
 * Pixel font renderer for monochrome and colour displays.
 *
 * Copyright (c) 2026 Ruslan V. Uss <unclerus@gmail.com>
 *
 * MIT Licensed as described in the file LICENSE
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include "pixelfont.h"

static const char *TAG = "pixelfont";

static const pixelfont_glyph_t *find_glyph_impl(const pixelfont_t *font, uint32_t codepoint);
static const pixelfont_glyph_t *glyph_or_fallback(const pixelfont_t *font, uint32_t codepoint);

// The streaming renderer composes the pixels in blocks of this many rows: one
// page of a SSD1306 is eight rows, and the column packings need no more.
#define CHUNK_ROWS 8

#define MAX_SCALE 8

#define CHECK_ARG(cond) do { \
        if (!(cond)) \
        { \
            ESP_LOGE(TAG, "Invalid argument: " #cond); \
            return ESP_ERR_INVALID_ARG; \
        } \
    } while (0)

static inline bool get_src_bit(const uint8_t *bits, int stride, int x, int y)
{
    return bits[y * stride + (x >> 3)] & (0x80 >> (x & 7));
}

static void set_block_bit(uint8_t *block, pixelfont_pack_t pack, int w, int h, int x, int y)
{
    if (pack == PIXELFONT_PACK_ROW)
    {
        const int stride = (w + 7) / 8;
        block[y * stride + (x >> 3)] |= 0x80 >> (x & 7);
    }
    else
    {
        const int stride = (h + 7) / 8;
        const uint8_t mask = pack == PIXELFONT_PACK_COLUMN_MSB ? (0x80 >> (y & 7)) : (1 << (y & 7));
        block[x * stride + (y >> 3)] |= mask;
    }
}

static int block_size(pixelfont_pack_t pack, int w, int h)
{
    return pack == PIXELFONT_PACK_ROW ? ((w + 7) / 8) * h : ((h + 7) / 8) * w;
}

static bool pack_valid(pixelfont_pack_t pack)
{
    return pack == PIXELFONT_PACK_ROW || pack == PIXELFONT_PACK_PAGE ||
           pack == PIXELFONT_PACK_COLUMN_MSB || pack == PIXELFONT_PACK_COLUMN_LSB;
}

static size_t canvas_size_impl(int width, int height, pixelfont_pack_t pack)
{
    return pack == PIXELFONT_PACK_ROW ? (size_t)((width + 7) / 8) * height
           : (size_t)width * ((height + 7) / 8);
}

esp_err_t pixelfont_canvas_size(int width, int height, pixelfont_pack_t pack, size_t *size)
{
    CHECK_ARG(size);
    CHECK_ARG(width > 0 && height > 0);
    CHECK_ARG(pack_valid(pack));

    *size = canvas_size_impl(width, height, pack);
    return ESP_OK;
}

/**
 * Writes one pixel of a whole canvas buffer.
 */
static void set_canvas_bit(uint8_t *data, pixelfont_pack_t pack, int width, int height,
                           int x, int y, bool on)
{
    const uint8_t bit = on ? 1 : 0;

    switch (pack)
    {
        case PIXELFONT_PACK_ROW:
        {
            const int stride = (width + 7) / 8;
            const uint8_t mask = 0x80 >> (x & 7);
            if (bit)
                data[y * stride + (x >> 3)] |= mask;
            else
                data[y * stride + (x >> 3)] &= (uint8_t)~mask;
            break;
        }
        case PIXELFONT_PACK_PAGE:
        {
            const uint8_t mask = (uint8_t)(1 << (y & 7));
            if (bit)
                data[(y >> 3) * width + x] |= mask;
            else
                data[(y >> 3) * width + x] &= (uint8_t)~mask;
            break;
        }
        default:
        {
            const int stride = (height + 7) / 8;
            const uint8_t mask = pack == PIXELFONT_PACK_COLUMN_MSB
                                 ? (uint8_t)(0x80 >> (y & 7)) : (uint8_t)(1 << (y & 7));
            if (bit)
                data[x * stride + (y >> 3)] |= mask;
            else
                data[x * stride + (y >> 3)] &= (uint8_t)~mask;
            break;
        }
    }
}

static bool get_canvas_bit(const uint8_t *data, pixelfont_pack_t pack, int width,
                           int height, int x, int y)
{
    switch (pack)
    {
        case PIXELFONT_PACK_ROW:
        {
            const int stride = (width + 7) / 8;
            return (data[y * stride + (x >> 3)] & (0x80 >> (x & 7))) != 0;
        }
        case PIXELFONT_PACK_PAGE:
            return (data[(y >> 3) * width + x] & (1 << (y & 7))) != 0;
        default:
        {
            const int stride = (height + 7) / 8;
            const uint8_t mask = pack == PIXELFONT_PACK_COLUMN_MSB
                                 ? (uint8_t)(0x80 >> (y & 7)) : (uint8_t)(1 << (y & 7));
            return (data[x * stride + (y >> 3)] & mask) != 0;
        }
    }
}

static pixelfont_style_t normalize_style(const pixelfont_style_t *style)
{
    pixelfont_style_t st = PIXELFONT_STYLE_DEFAULT;
    if (style)
        st = *style;

    if (!st.scale)
        st.scale = 1;

    return st;
}

static uint32_t utf8_next(const uint8_t **cursor)
{
    const uint8_t *s = *cursor;
    uint32_t cp;
    int extra;

    if (s[0] < 0x80)
    {
        *cursor = s + 1;
        return s[0];
    }
    else if ((s[0] & 0xe0) == 0xc0)
    {
        cp = s[0] & 0x1f;
        extra = 1;
    }
    else if ((s[0] & 0xf0) == 0xe0)
    {
        cp = s[0] & 0x0f;
        extra = 2;
    }
    else if ((s[0] & 0xf8) == 0xf0)
    {
        cp = s[0] & 0x07;
        extra = 3;
    }
    else
    {
        *cursor = s + 1;
        return 0xfffd;
    }

    for (int i = 1; i <= extra; i++)
    {
        if ((s[i] & 0xc0) != 0x80)
        {
            *cursor = s + 1;
            return 0xfffd;
        }
        cp = (cp << 6) | (s[i] & 0x3f);
    }

    *cursor = s + extra + 1;
    return cp;
}

/*
 * Clips a rectangle against the canvas. Returns false if nothing is left.
 */
static bool clip(const pixelfont_canvas_t *canvas, int *x, int *y, int *w, int *h)
{
    if (*x < 0)
    {
        *w += *x;
        *x = 0;
    }
    if (*y < 0)
    {
        *h += *y;
        *y = 0;
    }
    if (canvas->width && *x + *w > canvas->width)
        *w = canvas->width - *x;
    if (canvas->height && *y + *h > canvas->height)
        *h = canvas->height - *y;

    if (canvas->clip_w > 0 && canvas->clip_h > 0)
    {
        if (*x < canvas->clip_x)
        {
            *w += *x - canvas->clip_x;
            *x = canvas->clip_x;
        }
        if (*y < canvas->clip_y)
        {
            *h += *y - canvas->clip_y;
            *y = canvas->clip_y;
        }
        if (*x + *w > canvas->clip_x + canvas->clip_w)
            *w = canvas->clip_x + canvas->clip_w - *x;
        if (*y + *h > canvas->clip_y + canvas->clip_h)
            *h = canvas->clip_y + canvas->clip_h - *y;
    }

    return *w > 0 && *h > 0;
}

/*
 * Draws an image of width x height source pixels into the canvas, scaled.
 *
 * When the canvas has a buffer the pixels are written into it, respecting the
 * transparency: with opaque == false only the set pixels of the image touch
 * the buffer, so a text leaves the picture behind it intact. Without a buffer
 * the pixels are composed in blocks and handed to the flush callback; such a
 * block is always a filled rectangle of the packing, the clear bits in it mean
 * the background.
 */
static esp_err_t draw_image(const pixelfont_canvas_t *canvas, int x, int y,
                            int width, int height, const uint8_t *bits, uint8_t scale, bool opaque,
                            bool reversed, uint8_t *scratch)
{
    const int src_stride = (width + 7) / 8;
    const int dst_width = width * scale;
    const int dst_height = height * scale;

    if (canvas->data)
    {
        int x0 = x, y0 = y, w = dst_width, h = dst_height;
        if (!clip(canvas, &x0, &y0, &w, &h))
            return ESP_OK;

        for (int by = 0; by < h; by++)
        {
            const int sy = (y0 - y + by) / scale;
            for (int bx = 0; bx < w; bx++)
            {
                bool on = get_src_bit(bits, src_stride, (x0 - x + bx) / scale, sy);
                if (reversed)
                    on = !on;
                if (!on && !opaque)
                    continue;
                set_canvas_bit(canvas->data, canvas->pack, canvas->width, canvas->height,
                               x0 + bx, y0 + by, on);
            }
        }

        return ESP_OK;
    }

    for (int row = 0; row < dst_height; row += CHUNK_ROWS)
    {
        const int chunk_h = dst_height - row < CHUNK_ROWS ? dst_height - row : CHUNK_ROWS;

        int x0 = x, y0 = y + row;
        int w = dst_width, h = chunk_h;

        if (!clip(canvas, &x0, &y0, &w, &h))
            continue;

        const int left = x0 - x;
        const int top = y0 - (y + row);
        const int size = block_size(canvas->pack, w, h);
        memset(scratch, 0, size);

        for (int by = 0; by < h; by++)
        {
            const int sy = (top + by) / scale;
            for (int bx = 0; bx < w; bx++)
            {
                bool on = get_src_bit(bits, src_stride, (left + bx) / scale, sy);
                if (reversed)
                    on = !on;
                if (!on && !opaque)
                    continue;
                if (on)
                    set_block_bit(scratch, canvas->pack, w, h, bx, by);
            }
        }

        esp_err_t err = canvas->flush(canvas->ctx, x0, y0, w, h, scratch);
        if (err != ESP_OK)
            return err;
    }

    return ESP_OK;
}

static uint8_t *alloc_scratch(const pixelfont_canvas_t *canvas, int width, uint8_t scale)
{
    if (canvas->data)
        return NULL;

    return malloc(block_size(canvas->pack, width * scale, CHUNK_ROWS));
}

esp_err_t pixelfont_draw_glyph(const pixelfont_t *font, const pixelfont_canvas_t *canvas,
                               int x, int y, uint32_t codepoint, const pixelfont_style_t *style)
{
    CHECK_ARG(font && font->glyphs && font->bitmaps && font->width && font->height);
    CHECK_ARG(canvas && (canvas->flush || canvas->data));

    const pixelfont_style_t st = normalize_style(style);
    CHECK_ARG(st.scale <= MAX_SCALE);

    const pixelfont_glyph_t *glyph = find_glyph_impl(font, codepoint);
    if (!glyph)
    {
        ESP_LOGD(TAG, "No glyph for U+%04" PRIX32, codepoint);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *scratch = alloc_scratch(canvas, font->width, st.scale);
    if (!scratch && !canvas->data)
    {
        ESP_LOGE(TAG, "Could not allocate the block buffer");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = draw_image(canvas, x, y, font->width, font->height,
                               font->bitmaps + glyph->offset, st.scale, st.opaque, st.reversed, scratch);

    free(scratch);
    return err;
}

esp_err_t pixelfont_draw_text(const pixelfont_t *font, const pixelfont_canvas_t *canvas,
                              int x, int y, const char *text, const pixelfont_style_t *style)
{
    CHECK_ARG(font && font->glyphs && font->bitmaps && font->width && font->height);
    CHECK_ARG(canvas && (canvas->flush || canvas->data));
    CHECK_ARG(text);

    const pixelfont_style_t st = normalize_style(style);
    CHECK_ARG(st.scale <= MAX_SCALE);

    uint8_t *scratch = alloc_scratch(canvas, font->width, st.scale);
    if (!scratch && !canvas->data)
    {
        ESP_LOGE(TAG, "Could not allocate the block buffer");
        return ESP_ERR_NO_MEM;
    }

    const uint8_t *cursor = (const uint8_t *)text;
    esp_err_t err = ESP_OK;

    while (*cursor)
    {
        uint32_t cp = utf8_next(&cursor);

        const pixelfont_glyph_t *glyph = glyph_or_fallback(font, cp);

        if (glyph)
        {
            err = draw_image(canvas, x, y, font->width, font->height,
                             font->bitmaps + glyph->offset, st.scale, st.opaque, st.reversed, scratch);
            if (err != ESP_OK)
                break;
        }

        x += (glyph ? glyph->advance : font->width) * st.scale + st.spacing;
    }

    free(scratch);
    return err;
}

esp_err_t pixelfont_draw_bitmap(const pixelfont_canvas_t *canvas,
                                int x, int y, int width, int height, const uint8_t *bits, uint8_t scale,
                                bool opaque, bool reversed)
{
    CHECK_ARG(canvas && (canvas->flush || canvas->data));
    CHECK_ARG(bits && width > 0 && height > 0);
    CHECK_ARG(scale > 0 && scale <= MAX_SCALE);

    uint8_t *scratch = alloc_scratch(canvas, width, scale);
    if (!scratch && !canvas->data)
    {
        ESP_LOGE(TAG, "Could not allocate the block buffer");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = draw_image(canvas, x, y, width, height, bits, scale, opaque, reversed, scratch);

    free(scratch);
    return err;
}

esp_err_t pixelfont_fill_rect(const pixelfont_canvas_t *canvas,
                              int x, int y, int width, int height, bool on)
{
    CHECK_ARG(canvas && (canvas->flush || canvas->data));
    CHECK_ARG(width > 0 && height > 0);

    if (canvas->data)
    {
        int x0 = x, y0 = y, w = width, h = height;
        if (!clip(canvas, &x0, &y0, &w, &h))
            return ESP_OK;

        for (int by = 0; by < h; by++)
            for (int bx = 0; bx < w; bx++)
                set_canvas_bit(canvas->data, canvas->pack, canvas->width, canvas->height,
                               x0 + bx, y0 + by, on);

        return ESP_OK;
    }

    // A solid rectangle is the same image as a one-pixel source, so reuse the
    // block composition: build a source of the requested size that is all ones.
    const int src_stride = (width + 7) / 8;
    const int size = src_stride * height;
    uint8_t *src = malloc(size);
    if (!src)
        return ESP_ERR_NO_MEM;

    memset(src, on ? 0xff : 0x00, size);
    // Clear the padding bits of the last byte of every row, otherwise the
    // renderer would draw them as pixels of the image
    const int tail = width & 7;
    if (tail)
        for (int i = 0; i < height; i++)
            src[i * src_stride + src_stride - 1] &= (uint8_t)(0xff << (8 - tail));

    uint8_t *scratch = alloc_scratch(canvas, width, 1);
    if (!scratch)
    {
        free(src);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = draw_image(canvas, x, y, width, height, src, 1, true, !on, scratch);

    free(scratch);
    free(src);
    return err;
}

esp_err_t pixelfont_invert(const pixelfont_canvas_t *canvas)
{
    CHECK_ARG(canvas);
    CHECK_ARG(canvas->data);

    for (size_t i = 0; i < canvas->size; i++)
        canvas->data[i] = (uint8_t)~canvas->data[i];

    return ESP_OK;
}

esp_err_t pixelfont_clear(const pixelfont_canvas_t *canvas)
{
    CHECK_ARG(canvas && (canvas->flush || canvas->data));

    return pixelfont_fill_rect(canvas, 0, 0, canvas->width, canvas->height, false);
}

/**
 * Moves the pixels of a rectangle, every row from a row `pixels` further.
 */
static void move_rect(const pixelfont_canvas_t *canvas, int x, int y, int w, int h, int pixels)
{
    // the rows are copied in the direction opposite to the motion, so a row is
    // never overwritten before it is read
    if (pixels > 0)
        for (int row = 0; row < h - pixels; row++)
            for (int col = 0; col < w; col++)
                set_canvas_bit(canvas->data, canvas->pack, canvas->width, canvas->height,
                               x + col, y + row,
                               get_canvas_bit(canvas->data, canvas->pack, canvas->width,
                                              canvas->height, x + col, y + row + pixels));
    else
        for (int row = h - 1; row >= -pixels; row--)
            for (int col = 0; col < w; col++)
                set_canvas_bit(canvas->data, canvas->pack, canvas->width, canvas->height,
                               x + col, y + row,
                               get_canvas_bit(canvas->data, canvas->pack, canvas->width,
                                              canvas->height, x + col, y + row + pixels));
}

static esp_err_t scroll(const pixelfont_canvas_t *canvas, int x, int y, int width, int height,
                        int pixels, bool up)
{
    CHECK_ARG(canvas);
    CHECK_ARG(width > 0 && height > 0);
    CHECK_ARG(pixels >= 0);

    // scrolling moves pixels that are already there, so there must be a buffer
    if (!canvas->data)
        return ESP_ERR_INVALID_STATE;

    int x0 = x, y0 = y, w = width, h = height;
    if (!clip(canvas, &x0, &y0, &w, &h))
        return ESP_OK;

    if (!pixels)
        return ESP_OK;

    if (pixels >= h)
        return pixelfont_fill_rect(canvas, x0, y0, w, h, false);

    move_rect(canvas, x0, y0, w, h, up ? pixels : -pixels);

    const int freed_y = up ? y0 + h - pixels : y0;
    return pixelfont_fill_rect(canvas, x0, freed_y, w, pixels, false);
}

esp_err_t pixelfont_scroll_up(const pixelfont_canvas_t *canvas,
                              int x, int y, int width, int height, int pixels)
{
    return scroll(canvas, x, y, width, height, pixels, true);
}

esp_err_t pixelfont_scroll_down(const pixelfont_canvas_t *canvas,
                                int x, int y, int width, int height, int pixels)
{
    return scroll(canvas, x, y, width, height, pixels, false);
}

esp_err_t pixelfont_flush(const pixelfont_canvas_t *canvas)
{
    CHECK_ARG(canvas);
    CHECK_ARG(canvas->data && canvas->flush);

    // The callback takes a block of the canvas packing, so the buffer is sent
    // as horizontal strips of up to CHUNK_ROWS rows: that is exactly one page
    // of a SSD1306 and keeps the strips contiguous for every packing.
    for (int y = 0; y < canvas->height; y += CHUNK_ROWS)
    {
        int h = canvas->height - y < CHUNK_ROWS ? canvas->height - y : CHUNK_ROWS;

        const int size = block_size(canvas->pack, canvas->width, h);
        uint8_t *block = malloc(size);
        if (!block)
            return ESP_ERR_NO_MEM;

        // Copy the pixels of the strip into a block of the same packing
        memset(block, 0, size);
        const int stride = (canvas->width + 7) / 8;
        for (int by = 0; by < h; by++)
        {
            for (int bx = 0; bx < canvas->width; bx++)
            {
                bool on;
                switch (canvas->pack)
                {
                    case PIXELFONT_PACK_ROW:
                        on = canvas->data[(y + by) * stride + (bx >> 3)] & (0x80 >> (bx & 7));
                        break;
                    case PIXELFONT_PACK_PAGE:
                        on = canvas->data[((y + by) >> 3) * canvas->width + bx] & (1 << ((y + by) & 7));
                        break;
                    case PIXELFONT_PACK_COLUMN_MSB:
                        on = canvas->data[bx * ((canvas->height + 7) / 8) + ((y + by) >> 3)]
                             & (0x80 >> ((y + by) & 7));
                        break;
                    default:
                        on = canvas->data[bx * ((canvas->height + 7) / 8) + ((y + by) >> 3)]
                             & (1 << ((y + by) & 7));
                        break;
                }
                if (on)
                    set_block_bit(block, canvas->pack, canvas->width, h, bx, by);
            }
        }

        esp_err_t err = canvas->flush(canvas->ctx, 0, y, canvas->width, h, block);
        free(block);
        if (err != ESP_OK)
            return err;
    }

    return ESP_OK;
}

esp_err_t pixelfont_canvas_new(int width, int height, pixelfont_pack_t pack,
                               pixelfont_canvas_t **canvas)
{
    CHECK_ARG(width > 0 && height > 0 && width <= 4096 && height <= 4096);
    CHECK_ARG(canvas && pack_valid(pack));

    const size_t size = canvas_size_impl(width, height, pack);
    if (!size)
        return ESP_ERR_INVALID_SIZE;

    pixelfont_canvas_t *c = calloc(1, sizeof(*c));
    if (!c)
        return ESP_ERR_NO_MEM;

    c->data = calloc(1, size);
    if (!c->data)
    {
        free(c);
        return ESP_ERR_NO_MEM;
    }

    c->width = width;
    c->height = height;
    c->pack = pack;
    c->size = size;

    *canvas = c;
    return ESP_OK;
}

void pixelfont_canvas_free(pixelfont_canvas_t *canvas)
{
    if (!canvas)
        return;

    free(canvas->data);
    free(canvas);
}

esp_err_t pixelfont_set_clip(pixelfont_canvas_t *canvas, int x, int y, int width, int height)
{
    CHECK_ARG(canvas && width > 0 && height > 0);

    canvas->clip_x = x;
    canvas->clip_y = y;
    canvas->clip_w = width;
    canvas->clip_h = height;

    return ESP_OK;
}

esp_err_t pixelfont_reset_clip(pixelfont_canvas_t *canvas)
{
    CHECK_ARG(canvas);

    canvas->clip_x = 0;
    canvas->clip_y = 0;
    canvas->clip_w = 0;
    canvas->clip_h = 0;

    return ESP_OK;
}

static const pixelfont_glyph_t *find_glyph_impl(const pixelfont_t *font, uint32_t codepoint)
{
    int left = 0, right = font->count - 1;
    while (left <= right)
    {
        const int mid = left + (right - left) / 2;
        const pixelfont_glyph_t *glyph = font->glyphs + mid;

        if (glyph->codepoint == codepoint)
            return glyph;
        if (glyph->codepoint < codepoint)
            left = mid + 1;
        else
            right = mid - 1;
    }

    return NULL;
}

static const pixelfont_glyph_t *glyph_or_fallback(const pixelfont_t *font, uint32_t codepoint)
{
    const pixelfont_glyph_t *glyph = find_glyph_impl(font, codepoint);
    if (!glyph)
        glyph = find_glyph_impl(font, '?');
    if (!glyph)
        glyph = find_glyph_impl(font, 0xfffd);

    return glyph;
}

esp_err_t pixelfont_text_width(const pixelfont_t *font, const char *text,
                               const pixelfont_style_t *style, int *width)
{
    CHECK_ARG(width);
    CHECK_ARG(font && font->glyphs && font->bitmaps && font->width && font->height);
    CHECK_ARG(text);

    const pixelfont_style_t st = normalize_style(style);
    if (st.scale > MAX_SCALE)
        return ESP_ERR_INVALID_SIZE;

    int advance = 0;
    int count = 0;
    const uint8_t *cursor = (const uint8_t *)text;
    while (*cursor)
    {
        uint32_t cp = utf8_next(&cursor);
        const pixelfont_glyph_t *glyph = glyph_or_fallback(font, cp);

        advance += (glyph ? glyph->advance : font->width) * st.scale;
        count++;
    }

    if (count > 1)
        advance += (count - 1) * st.spacing;

    *width = advance;
    return ESP_OK;
}

esp_err_t pixelfont_text_height(const pixelfont_t *font, const pixelfont_style_t *style,
                                int *height)
{
    CHECK_ARG(height);
    CHECK_ARG(font && font->width && font->height);

    const pixelfont_style_t st = normalize_style(style);
    if (st.scale > MAX_SCALE)
        return ESP_ERR_INVALID_SIZE;

    *height = font->height * st.scale;
    return ESP_OK;
}

esp_err_t pixelfont_find_glyph(const pixelfont_t *font, uint32_t codepoint,
                               const pixelfont_glyph_t **glyph)
{
    CHECK_ARG(glyph);
    CHECK_ARG(font && font->glyphs && font->count);

    const pixelfont_glyph_t *found = find_glyph_impl(font, codepoint);
    if (!found)
        return ESP_ERR_NOT_FOUND;

    *glyph = found;
    return ESP_OK;
}

