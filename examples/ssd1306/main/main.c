/*
 * Draws on a 128x64 monochrome OLED with the SSD1306 panel driver that comes
 * with ESP-IDF, esp_lcd_panel_ssd1306.
 *
 * The panel driver takes one bit per pixel, in the same page-major order as the
 * memory of the controller: a byte per column, bit 0 is the topmost row of the
 * eight-row page. That is exactly what pixelfont writes with
 * PIXELFONT_PACK_PAGE, so the canvas works right in the buffer of the
 * application and a page goes to the panel without any conversion.
 */

#include <stdio.h>
#include <string.h>
#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_ssd1306.h>
#include <pixelfont.h>
#include <pixelfont_fonts.h>

static const char *TAG = "example";

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64

// The picture in the page-major format of the controller
static uint8_t framebuffer[DISPLAY_WIDTH * DISPLAY_HEIGHT / 8];

// An 8x8 bitmap: a heart, one byte per row
static const uint8_t heart[] =
{
    0x00, 0x66, 0xff, 0xff, 0xff, 0x7e, 0x3c, 0x18,
};

static esp_lcd_panel_handle_t panel = NULL;

/*
 * Sends the picture to the panel, page by page.
 *
 * A page is eight rows, and the driver writes the columns of the page as they
 * are in the memory, so the bytes of the page go as is: no temporary buffer and
 * no bit shuffling.
 */
static esp_err_t flush(void)
{
    for (int y = 0; y < DISPLAY_HEIGHT; y += 8)
    {
        esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, y, DISPLAY_WIDTH, y + 8,
                                                  framebuffer + (y / 8) * DISPLAY_WIDTH);
        if (err != ESP_OK)
            return err;
    }

    return ESP_OK;
}

void app_main(void)
{
    // the I2C bus
    i2c_master_bus_config_t bus_config =
    {
        .i2c_port = -1,
        .sda_io_num = CONFIG_EXAMPLE_I2C_SDA,
        .scl_io_num = CONFIG_EXAMPLE_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));

    // the panel, the SSD1306 speaks I2C with a control byte that carries the
    // command/data bit in bit 6
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_config =
    {
        .dev_addr = CONFIG_EXAMPLE_I2C_ADDR,
        .scl_speed_hz = 400000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags.dc_low_on_data = false,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus, &io_config, &io));

    esp_lcd_panel_ssd1306_config_t ssd1306_config =
    {
        .height = DISPLAY_HEIGHT,
    };
    esp_lcd_panel_dev_config_t panel_config =
    {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &ssd1306_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    // the canvas works right in the buffer
    pixelfont_canvas_t canvas =
    {
        .width = DISPLAY_WIDTH,
        .height = DISPLAY_HEIGHT,
        .pack = PIXELFONT_PACK_PAGE,
        .data = framebuffer,
        .size = sizeof(framebuffer),
    };

    pixelfont_style_t style = PIXELFONT_STYLE_DEFAULT;

    while (true)
    {
        ESP_ERROR_CHECK(pixelfont_fill_rect(&canvas, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, false));

        // the title
        ESP_ERROR_CHECK(pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, 28, 0, "Pixelfont", &style));

        // a line of text
        ESP_ERROR_CHECK(pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, 4, 16,
                                            "1 bit = 1 pixel", &style));

        // the same text twice the size, the cell is 16x32 here
        style.scale = 2;
        ESP_ERROR_CHECK(pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, 4, 32, "Hello", &style));
        style.scale = 1;

        // a bitmap
        ESP_ERROR_CHECK(pixelfont_draw_bitmap(&canvas, 92, 48, 8, 8, heart, 1, false, false));

        // inverse video, the cell is filled and the glyph leaves holes in it
        style.opaque = true;
        style.reversed = true;
        ESP_ERROR_CHECK(pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, 92, 32, "INV", &style));
        style = (pixelfont_style_t)PIXELFONT_STYLE_DEFAULT;

        ESP_ERROR_CHECK(flush());

        // the same picture with every pixel inverted, like a negative
        ESP_LOGI(TAG, "The same picture, inverted");
        ESP_ERROR_CHECK(pixelfont_invert(&canvas));
        ESP_ERROR_CHECK(flush());

        // and back, this time by the controller itself: the panel can invert the
        // memory on its way to the glass
        ESP_ERROR_CHECK(pixelfont_invert(&canvas));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
        ESP_ERROR_CHECK(flush());
        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));

        // a terminal in the lower part of the screen: the text area is clipped,
        // the text is drawn again on every frame one pixel higher, so the lines
        // leave the area through its top edge and enter through its bottom edge a
        // pixel at a time, and the title above the area stays untouched
        const int area_y = DISPLAY_HEIGHT / 4;
        const int area_h = DISPLAY_HEIGHT - area_y;
        const int line_height = 16;
        const int rows = area_h / line_height;

        style = (pixelfont_style_t)PIXELFONT_STYLE_DEFAULT;

        ESP_ERROR_CHECK(pixelfont_fill_rect(&canvas, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, false));
        ESP_ERROR_CHECK(pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, 0, 0, "Terminal", &style));

        ESP_ERROR_CHECK(pixelfont_set_clip(&canvas, 0, area_y, DISPLAY_WIDTH, area_h));

        for (int step = 0; step < 3 * line_height; step++)
        {
            const int offset = step % line_height;
            const int first = step / line_height;

            // the rectangle only: the title is outside of it
            ESP_ERROR_CHECK(pixelfont_fill_rect(&canvas, 0, area_y, DISPLAY_WIDTH, area_h, false));

            // one row more than fits, its tail is clipped by the rectangle
            for (int row = 0; row <= rows; row++)
            {
                char text[16];
                snprintf(text, sizeof(text), "Line %d", first + row + 1);
                ESP_ERROR_CHECK(pixelfont_draw_text(PIXELFONT_FONT_DEFAULT, &canvas, 4,
                                                    area_y + row * line_height - offset, text, &style));
            }

            ESP_ERROR_CHECK(flush());
            vTaskDelay(pdMS_TO_TICKS(25));
        }

        ESP_ERROR_CHECK(pixelfont_reset_clip(&canvas));
        ESP_LOGI(TAG, "Scrolled 3 lines");


        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGI(TAG, "Done");
}
