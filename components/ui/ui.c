/**
 * @file ui.c
 *
 * @brief UI module implementation:
 * All display layout + helper print functions.
 *
 * No full-screen clears are performed here.
 * The caller (main_app) performs a one-time st7789h2_fill() at boot.
 */
#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "st7789h2.h"

/* Font setup for display writing (same values you used in main_app.c) */
#define FONT_W 7 // pixels per character at scale=1 (check your font!)
#define FONT_H 5

/* Common colors used throughout UI */
#define UI_FG 0xFFFF
#define UI_BG 0x0000

/* A couple extra colors for prompts */
#define UI_WARN 0xF800   // red
#define UI_DIM  0x7BEF   // gray-ish
#define UI_ACC  0x001F   // blue

/**
 * Helper: draw a string that overwrites its background.
 * Because bg color is supplied, this naturally "clears" the previous characters
 * behind the text, without clearing the entire screen.
 */
static inline void ui_draw_text(uint16_t x, uint16_t y, uint8_t scale, const char *s)
{
    st7789h2_draw_string_scaled(x, y, s, UI_FG, UI_BG, scale);
}

/**
 * Helper: printf-style formatting into a local buffer and draw.
 * IMPORTANT SAFETY:
 * Always use a real format string ("%s", ...) when printing other strings to avoid
 * accidental format string interpretation.
 */
static inline void ui_draw_printf(uint16_t x, uint16_t y, uint8_t scale,
                                  char *buf, size_t buf_sz,
                                  const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, buf_sz, fmt, ap);
    va_end(ap);

    ui_draw_text(x, y, scale, buf);
}

/*
 * NEW helper: padded printf line (clears old longer text).
 * This is important for prompt lines where the text length can change.
 */
static inline void ui_draw_printf_padded(uint16_t x, uint16_t y, uint8_t scale,
                                         char *buf, size_t buf_sz,
                                         int pad_width,
                                         const char *fmt, ...)
{
    char tmp[64];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    // Left pad to fixed width with spaces so old text is overwritten.
    // Example: "%-28s" prints at least 28 characters.
    snprintf(buf, buf_sz, "%-*s", pad_width, tmp);

    ui_draw_text(x, y, scale, buf);
}

void ui_render_frame(const ui_layout_t *layout,
                     float ambient_temp_c,
                     const struct bme68x_data *data,
                     const baro_forecast_t *baro,
                     const air_quality_out_t *aq_out)
{
    uint16_t x = layout->x_pos;
    uint16_t y = layout->y_pos_start;
    uint8_t  scale = layout->scale;
    uint16_t lh = layout->line_height;

    char buf[64];

    /* ---------------------------------------------------------------------- */
    /* Temperature (Including raised "0" and "C")                             */
    /* ---------------------------------------------------------------------- */
    ui_draw_printf(x, y, scale, buf, sizeof(buf), "Temp: %.1f", ambient_temp_c);

    /* Draw raised "0" and then "C" (preserved idea from your original code) */
    int text_width = (int)strlen(buf) * FONT_W * scale - 10;

    st7789h2_draw_string_scaled(x + text_width,
                               y - (scale * 4),
                               "0",
                               UI_FG, UI_BG,
                               (scale > 1) ? (scale - 1) : 1);

    st7789h2_draw_string_scaled(x + text_width + (FONT_W * ((scale > 1) ? (scale - 1) : 1)),
                               y,
                               "C",
                               UI_FG, UI_BG,
                               scale);

    y += lh;

    /* ---------------------------------------------------------------------- */
    /* Relative humidity                                                      */
    /* ---------------------------------------------------------------------- */
    ui_draw_printf(x, y, scale, buf, sizeof(buf), "Hum:  %.1f %%RH", data->humidity);
    y += lh;

    /* ---------------------------------------------------------------------- */
    /* Barometer output (SLP + Forecast + Alerts/Trend)                        */
    /* ---------------------------------------------------------------------- */
    ui_draw_printf(x, y, scale, buf, sizeof(buf), "SLP:  %.0f hPa", baro_forecast_slp_hpa(baro));
    y += lh;

    y += lh;
    ui_draw_text(x, y, scale, "Forecast:");
    y += lh;

    ui_draw_printf(x, y, scale, buf, sizeof(buf), "%s", baro_forecast_text(baro));
    y += lh;

    storm_level_t storm = baro_forecast_storm_level(baro);
    if (storm != STORM_NONE)
    {
        ui_draw_text(x, y, scale, "Alerts:");
        y += lh;

        ui_draw_printf(x, y, scale, buf, sizeof(buf), "%s", storm_level_str(storm));
        y += lh;
    }
    else
    {
        ui_draw_text(x, y, scale, "Trend:");
        y += lh;

        ui_draw_printf(x, y, scale, buf, sizeof(buf), "%s", baro_trend_str(baro_forecast_trend(baro)));
        y += lh;
    }

    /* ---------------------------------------------------------------------- */
    /* Air quality (THIS IS THE "REAL TEXT": aq_out->text)                    */
    /* ---------------------------------------------------------------------- */
    ui_draw_text(x, y, scale, "Air Quality:");
    y += lh;

    /* If not ready, aq_out->text is "Warming up..." (from module) */
    ui_draw_printf(x, y, scale, buf, sizeof(buf), "%s", aq_out->text);
}

/* -------------------------------------------------------------------------- */
/* NEW: Screen 2 renderer (MIN/MAX + confirmation prompt)                      */
/* -------------------------------------------------------------------------- */
void ui_render_minmax(const ui_layout_t *layout,
                      const minmax_stats_t *s,
                      bool confirm_active,
                      ui_confirm_target_t confirm_target)
{
    uint16_t x = layout->x_pos-10;
    uint16_t y = layout->y_pos_start-10;
    uint8_t  scale = layout->scale;
    uint16_t lh = layout->line_height;

    char buf[64];

    /* ------------------------------------------------------------ */
    /* Min / Max values (or placeholders if not valid yet)          */
    /* ------------------------------------------------------------ */
    if (s && s->valid) {

        ui_draw_printf_padded(x, y, scale, buf, sizeof(buf), 28, "      MIN   MAX    ");
        y += lh;
        ui_draw_printf_padded(x, y, scale, buf, sizeof(buf), 28, "T(C):%5.1f %5.1f", s->temp_min_c, s->temp_max_c );
        y += lh;
        ui_draw_printf_padded(x, y, scale, buf, sizeof(buf), 28, "H(%%):%5.1f %5.1f", s->rh_min, s->rh_max);
        y += lh;
        ui_draw_printf_padded(x, y, scale, buf, sizeof(buf), 28, "P(h):%6.1f %6.1f", s->press_min_hpa, s->press_max_hpa);          
        y += lh;     

    } else {



        /* No data yet – still show something */
        ui_draw_printf_padded(x, y, scale, buf, sizeof(buf), 28, "      MIN   MAX    ");
        y += lh;
        ui_draw_printf_padded(x, y, scale, buf, sizeof(buf), 28, "T(C): ----  ----", s->temp_min_c, s->temp_max_c );
        y += lh;
        ui_draw_printf_padded(x, y, scale, buf, sizeof(buf), 28, "H(%%): ----  ----", s->rh_min, s->rh_max);
        y += lh;
        ui_draw_printf_padded(x, y, scale, buf, sizeof(buf), 28, "P(h):------ ------", s->press_min_hpa, s->press_max_hpa);          
        y += lh; 
    }

    /* ------------------------------------------------------------ */
    /* Confirmation prompt (THIS IS WHAT CHANGES ON BUTTON PRESS)   */
    /* ------------------------------------------------------------ */
    if (confirm_active) 
    {
        const char *what = "???";
        const char *btn  = "???";

        switch (confirm_target) {
            case UI_CONFIRM_TEMP:  what = "Tmp. min/max";   btn = "LEFT";  break;
            case UI_CONFIRM_RH:    what = "Hum. min/max";   btn = "RIGHT"; break;
            case UI_CONFIRM_PRESS: what = "Prs. min/max";   btn = "DOWN";  break;
            default: break;
        }

        /* Line 1: "Reset xxx?" — overwrite whole line */
        y += lh;
        snprintf(buf, sizeof(buf), "Reset %s?", what);
        ui_draw_printf_padded(
            x, y, scale,
            buf, sizeof(buf),
            28,            // same pad width you already use
            "%s", buf
        );

        y += lh;

        /* Line 2: "Press xxx again" — overwrite whole line */
        snprintf(buf, sizeof(buf), "Press %s again", btn);
        ui_draw_printf_padded(
            x, y, scale,
            buf, sizeof(buf),
            28,
            "%s", buf
        );

    } else {
        /* Clear old confirmation area (unchanged from your code) */
        ui_draw_printf_padded(x, y, scale, buf, sizeof(buf), 28, "");
        y += lh;
        ui_draw_printf_padded(x, y, scale, buf, sizeof(buf), 28, "");
    }
}
    
