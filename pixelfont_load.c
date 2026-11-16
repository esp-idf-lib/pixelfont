/*
 * pixelfont_load.c
 *
 * Loading of the standard bitmap font formats: BDF, PSF1 and PSF2.
 *
 * Copyright (c) 2026 Ruslan V. Uss <unclerus@gmail.com>
 *
 * MIT Licensed as described in the file LICENSE
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <esp_log.h>
#include "pixelfont.h"

static const char *TAG = "pixelfont";

#define CHECK_ARG(cond) do { \
        if (!(cond)) \
        { \
            ESP_LOGE(TAG, "Invalid argument: " #cond); \
            return ESP_ERR_INVALID_ARG; \
        } \
    } while (0)

// PSF1
#define PSF1_MAGIC0 0x36
#define PSF1_MAGIC1 0x04
#define PSF1_MODE512    0x01
#define PSF1_MODEHASTAB 0x02

// PSF2
#define PSF2_MAGIC0 0x72
#define PSF2_MAGIC1 0xb5
#define PSF2_MAGIC2 0x4a
#define PSF2_MAGIC3 0x86
#define PSF2_HAS_UNICODE_TABLE 0x01

// The builder collects the glyphs with their bitmaps and then packs them into
// one heap block together with the index.
typedef struct
{
    pixelfont_glyph_t *glyphs;
    size_t count;
    size_t cap;
    uint8_t *bits;
    size_t bits_len;
    size_t bits_cap;
    int width;
    int height;
    int ascent;
} builder_t;

static void builder_free(builder_t *b)
{
    free(b->glyphs);
    free(b->bits);
    memset(b, 0, sizeof(*b));
}

static bool builder_add(builder_t *b, uint32_t codepoint, uint8_t advance,
                        const uint8_t *src, int src_stride, int src_x, int src_y, int src_w, int src_h)
{
    if (codepoint > 0x10ffff)
        return true;

    const int stride = (b->width + 7) / 8;
    const size_t size = stride * b->height;

    if (b->bits_len + size > b->bits_cap)
    {
        size_t cap = b->bits_cap ? b->bits_cap : 4096;
        while (cap < b->bits_len + size)
            cap *= 2;
        uint8_t *bits = realloc(b->bits, cap);
        if (!bits)
            return false;
        b->bits = bits;
        b->bits_cap = cap;
    }

    if (b->count == b->cap)
    {
        size_t cap = b->cap ? b->cap * 2 : 256;
        pixelfont_glyph_t *glyphs = realloc(b->glyphs, cap * sizeof(pixelfont_glyph_t));
        if (!glyphs)
            return false;
        b->glyphs = glyphs;
        b->cap = cap;
    }

    uint8_t *dst = b->bits + b->bits_len;
    memset(dst, 0, size);

    for (int y = 0; y < src_h; y++)
    {
        const int dy = src_y + y;
        if (dy < 0 || dy >= b->height)
            continue;
        for (int x = 0; x < src_w; x++)
        {
            const int dx = src_x + x;
            if (dx < 0 || dx >= b->width)
                continue;
            if (!(src[y * src_stride + (x >> 3)] & (0x80 >> (x & 7))))
                continue;
            dst[dy * stride + (dx >> 3)] |= 0x80 >> (dx & 7);
        }
    }

    pixelfont_glyph_t *glyph = b->glyphs + b->count++;
    glyph->codepoint = codepoint;
    glyph->offset = b->bits_len;
    glyph->advance = advance ? advance : b->width;
    memset(glyph->reserved, 0, sizeof(glyph->reserved));

    b->bits_len += size;
    return true;
}

static int glyph_cmp(const void *a, const void *b)
{
    const pixelfont_glyph_t *ga = a, *gb = b;
    if (ga->codepoint < gb->codepoint)
        return -1;
    if (ga->codepoint > gb->codepoint)
        return 1;
    return 0;
}

static esp_err_t builder_finish(builder_t *b, pixelfont_t **out)
{
    if (!b->count || !b->width || !b->height)
    {
        ESP_LOGE(TAG, "The font has no glyphs");
        return ESP_ERR_INVALID_RESPONSE;
    }

    qsort(b->glyphs, b->count, sizeof(pixelfont_glyph_t), glyph_cmp);

    // Drop the duplicates that PSF unicode tables may produce
    size_t count = 1;
    for (size_t i = 1; i < b->count; i++)
        if (b->glyphs[i].codepoint != b->glyphs[count - 1].codepoint)
            b->glyphs[count++] = b->glyphs[i];

    if (count > UINT16_MAX)
    {
        ESP_LOGE(TAG, "Too many glyphs: %u", (unsigned)count);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const int stride = (b->width + 7) / 8;
    const size_t bitmaps_size = (size_t)stride * b->height * count;
    const size_t glyphs_size = count * sizeof(pixelfont_glyph_t);

    // One block for the index and the bitmaps, since the offsets of the glyphs
    // change when the duplicates are dropped: rewrite them while copying.
    uint8_t *block = malloc(glyphs_size + bitmaps_size);
    if (!block)
        return ESP_ERR_NO_MEM;

    pixelfont_glyph_t *glyphs = (pixelfont_glyph_t *)block;
    uint8_t *bitmaps = block + glyphs_size;

    for (size_t i = 0; i < count; i++)
    {
        glyphs[i] = b->glyphs[i];
        glyphs[i].offset = i * stride * b->height;
        memcpy(bitmaps + glyphs[i].offset, b->bits + b->glyphs[i].offset, stride * b->height);
    }

    pixelfont_t *font = calloc(1, sizeof(pixelfont_t));
    if (!font)
    {
        free(block);
        return ESP_ERR_NO_MEM;
    }

    font->width = (uint8_t)b->width;
    font->height = (uint8_t)b->height;
    font->ascent = (int8_t)b->ascent;
    font->count = (uint16_t)count;
    font->glyphs = glyphs;
    font->bitmaps = bitmaps;
    font->owned = block;

    *out = font;
    return ESP_OK;
}

static bool read_file(const char *path, uint8_t **data, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return false;
    }

    long len = ftell(f);
    if (len <= 0)
    {
        fclose(f);
        return false;
    }

    if (fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return false;
    }

    uint8_t *buf = malloc((size_t)len + 1);
    if (!buf)
    {
        fclose(f);
        return false;
    }

    const size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);

    if (got != (size_t)len)
    {
        free(buf);
        return false;
    }

    buf[len] = 0;
    *data = buf;
    *size = (size_t)len;
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// BDF

static esp_err_t load_bdf(char *data, builder_t *b)
{
    int fbb_w = 0, fbb_h = 0, fbb_x = 0, fbb_y = 0;
    int ascent = 0, descent = 0;

    char *line = strtok(data, "\n");
    bool header = true;

    while (line && header)
    {
        int w, h, x, y;
        if (sscanf(line, "FONTBOUNDINGBOX %d %d %d %d", &w, &h, &x, &y) == 4)
        {
            fbb_w = w;
            fbb_h = h;
            fbb_x = x;
            fbb_y = y;
        }
        else if (sscanf(line, "FONT_ASCENT %d", &w) == 1)
            ascent = w;
        else if (sscanf(line, "FONT_DESCENT %d", &w) == 1)
            descent = w;
        else if (!strncmp(line, "STARTCHAR", 9))
            header = false;

        if (header)
            line = strtok(NULL, "\n");
    }

    if (fbb_w <= 0 || fbb_h <= 0 || fbb_w > 255 || fbb_h > 255)
    {
        ESP_LOGE(TAG, "Bad FONTBOUNDINGBOX %dx%d", fbb_w, fbb_h);
        return ESP_ERR_INVALID_RESPONSE;
    }

    b->width = fbb_w;
    b->height = fbb_h;
    b->ascent = ascent ? ascent : fbb_h + fbb_y;
    if (descent && b->ascent + descent > b->height)
        b->ascent = b->height - descent;

    while (line)
    {
        if (!strncmp(line, "ENCODING", 8))
        {
            long cp = strtol(line + 8, NULL, 10);
            int advance = fbb_w;
            int bbx_w = 0, bbx_h = 0, bbx_x = 0, bbx_y = 0;
            uint8_t *rows = NULL;
            int stride = 0;

            // ENCODING, DWIDTH, BBX and BITMAP, in this order
            while ((line = strtok(NULL, "\n")))
            {
                if (!strncmp(line, "DWIDTH", 6))
                {
                    int dx, dy;
                    if (sscanf(line + 6, "%d %d", &dx, &dy) == 2)
                        advance = dx;
                }
                else if (!strncmp(line, "BBX", 3))
                {
                    if (sscanf(line + 3, "%d %d %d %d", &bbx_w, &bbx_h, &bbx_x, &bbx_y) != 4)
                        return ESP_ERR_INVALID_RESPONSE;
                }
                else if (!strncmp(line, "BITMAP", 6))
                    break;
                else if (!strncmp(line, "ENDCHAR", 7))
                    break;
            }

            if (!line)
                break;

            if (!strncmp(line, "BITMAP", 6))
            {
                stride = (bbx_w + 7) / 8;
                if (bbx_w < 0 || bbx_h < 0 || bbx_w > 255 || bbx_h > 255 || !stride)
                    return ESP_ERR_INVALID_RESPONSE;

                rows = malloc((size_t)stride * bbx_h);
                if (!rows)
                    return ESP_ERR_NO_MEM;

                for (int r = 0; r < bbx_h; r++)
                {
                    line = strtok(NULL, "\n");
                    if (!line)
                    {
                        free(rows);
                        return ESP_ERR_INVALID_RESPONSE;
                    }

                    for (int i = 0; i < stride; i++)
                    {
                        const int hi = hex_value(line[i * 2]);
                        const int lo = hex_value(line[i * 2 + 1]);
                        if (hi < 0 || lo < 0)
                        {
                            free(rows);
                            return ESP_ERR_INVALID_RESPONSE;
                        }
                        rows[r * stride + i] = (uint8_t)(hi << 4 | lo);
                    }
                }
            }

            if (cp >= 0 && rows)
            {
                // Place the glyph into the cell. Both bounding boxes are
                // measured from the baseline, so the top of the cell is at
                // fbb_y + fbb_h and the top of the glyph at bbx_y + bbx_h.
                const int src_y = (fbb_y + fbb_h) - (bbx_y + bbx_h);
                const int src_x = bbx_x - fbb_x;
                if (!builder_add(b, (uint32_t)cp, (uint8_t)advance, rows, stride,
                                 src_x, src_y, bbx_w, bbx_h))
                {
                    free(rows);
                    return ESP_ERR_NO_MEM;
                }
            }

            free(rows);

            while (line && strncmp(line, "ENDCHAR", 7))
                line = strtok(NULL, "\n");
        }

        line = strtok(NULL, "\n");
    }

    return ESP_OK;
}

// PSF

static esp_err_t load_psf1(const uint8_t *data, size_t size, builder_t *b)
{
    if (size < 4)
        return ESP_ERR_INVALID_RESPONSE;

    const uint8_t mode = data[2];
    const int height = data[3];
    const int count = mode & PSF1_MODE512 ? 512 : 256;
    const int stride = 1;

    if (height <= 0 || size < 4 + (size_t)count * height)
        return ESP_ERR_INVALID_RESPONSE;

    b->width = 8;
    b->height = height;
    b->ascent = height;

    const uint8_t *glyphs = data + 4;
    const uint8_t *table = glyphs + (size_t)count * height;
    const size_t table_size = size - 4 - (size_t)count * height;

    // The unicode table is one sequence per glyph: little-endian code points
    // terminated by 0xffff, an empty sequence means "no mapping".
    const uint8_t *p = table;

    for (int i = 0; i < count; i++)
    {
        const uint8_t *glyph = glyphs + i * height;
        bool added = false;

        if ((mode & PSF1_MODEHASTAB) && table_size)
        {
            while ((size_t)(p - table) + 1 < table_size)
            {
                const uint32_t cp = p[0] | (p[1] << 8);
                p += 2;

                if (cp == 0xffff)
                    break;

                if (!builder_add(b, cp, 8, glyph, stride, 0, 0, 8, height))
                    return ESP_ERR_NO_MEM;
                added = true;
            }
        }

        if (!added && !builder_add(b, (uint32_t)i, 8, glyph, stride, 0, 0, 8, height))
            return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static uint32_t read_le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static esp_err_t load_psf2(const uint8_t *data, size_t size, builder_t *b)
{
    if (size < 32)
        return ESP_ERR_INVALID_RESPONSE;

    const uint32_t header_size = read_le32(data + 8);
    const uint32_t flags = read_le32(data + 12);
    const uint32_t count = read_le32(data + 16);
    const uint32_t char_size = read_le32(data + 20);
    const uint32_t height = read_le32(data + 24);
    const uint32_t width = read_le32(data + 28);

    if (!count || !char_size || !height || !width || width > 255 || height > 255
            || header_size < 32 || header_size > size
            || (size_t)header_size + (size_t)char_size * count > size)
        return ESP_ERR_INVALID_RESPONSE;

    const int stride = (width + 7) / 8;
    if ((size_t)stride * height > char_size)
        return ESP_ERR_INVALID_RESPONSE;

    b->width = width;
    b->height = height;
    b->ascent = height;

    const uint8_t *glyphs = data + header_size;
    const uint8_t *table = glyphs + (size_t)char_size * count;
    const size_t table_size = size - header_size - (size_t)char_size * count;

    if (!(flags & PSF2_HAS_UNICODE_TABLE) || !table_size)
    {
        for (uint32_t i = 0; i < count; i++)
            if (!builder_add(b, i, width, glyphs + i * char_size, stride, 0, 0, width, height))
                return ESP_ERR_NO_MEM;
        return ESP_OK;
    }

    // The table is one string per glyph, UTF-8, 0xff-terminated. A glyph may
    // map to several code points; every one of them gets an index entry.
    const uint8_t *p = table;
    for (uint32_t i = 0; i < count && (size_t)(p - table) < table_size; i++)
    {
        bool added = false;
        while ((size_t)(p - table) < table_size && *p != 0xff)
        {
            uint32_t cp = *p++;
            if ((cp & 0xe0) == 0xc0 && *p != 0xff)
                cp = ((cp & 0x1f) << 6) | (*p++ & 0x3f);
            else if ((cp & 0xf0) == 0xe0 && p[0] != 0xff && p[1] != 0xff)
            {
                cp = ((cp & 0x0f) << 12) | ((p[0] & 0x3f) << 6) | (p[1] & 0x3f);
                p += 2;
            }
            else if ((cp & 0xf8) == 0xf0 && p[0] != 0xff && p[1] != 0xff && p[2] != 0xff)
            {
                cp = ((cp & 0x07) << 18) | ((p[0] & 0x3f) << 12)
                     | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
                p += 3;
            }

            if (!builder_add(b, cp, width, glyphs + i * char_size, stride, 0, 0, width, height))
                return ESP_ERR_NO_MEM;
            added = true;
        }
        if ((size_t)(p - table) < table_size)
            p++; // 0xff

        if (!added && !builder_add(b, i, width, glyphs + i * char_size, stride, 0, 0, width, height))
            return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t pixelfont_load(const char *path, pixelfont_t **font)
{
    CHECK_ARG(path && font);
    *font = NULL;

    uint8_t *data = NULL;
    size_t size = 0;

    if (!read_file(path, &data, &size))
    {
        ESP_LOGE(TAG, "Could not read %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    builder_t b;
    memset(&b, 0, sizeof(b));

    esp_err_t err;

    if (size > 9 && !memcmp(data, "STARTFONT", 9))
        err = load_bdf((char *)data, &b);
    else if (size > 2 && data[0] == PSF1_MAGIC0 && data[1] == PSF1_MAGIC1)
        err = load_psf1(data, size, &b);
    else if (size > 4 && data[0] == PSF2_MAGIC0 && data[1] == PSF2_MAGIC1
             && data[2] == PSF2_MAGIC2 && data[3] == PSF2_MAGIC3)
        err = load_psf2(data, size, &b);
    else
    {
        ESP_LOGE(TAG, "%s is not a BDF, PSF1 or PSF2 font", path);
        err = ESP_ERR_INVALID_RESPONSE;
    }

    free(data);

    if (err == ESP_OK)
        err = builder_finish(&b, font);

    builder_free(&b);
    return err;
}

void pixelfont_free(pixelfont_t *font)
{
    if (!font)
        return;

    free(font->owned);
    free(font);
}
