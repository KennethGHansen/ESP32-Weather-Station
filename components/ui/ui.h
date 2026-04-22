#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "baro_forecast.h"
#include "bme68x.h"
#include "air_quality.h"

/* NEW includes for min/max screen */
#include "minmax_stats.h"
#include "ui_controller.h"

/**
 * @file ui.h
 *
 * @brief UI module:
 * - Owns display layout (x/y positioning, line spacing)
 * - Owns helper string formatting and drawing
 *
 * IMPORTANT:
 * We do NOT clear the entire screen per update because the display is pixel-write over SPI
 * and that would cause blinking. Each printed line is drawn with background color, so new
 * text overwrites old text cleanly.
 */
typedef struct
{
    uint8_t  scale;
    uint16_t line_height;
    uint16_t y_pos_start;
    uint16_t x_pos;
} ui_layout_t;

// Small enum for timing logic between rendering inside or outside temp/hum on the display
typedef enum {
    UI_VIEW_INDOOR = 0,
    UI_VIEW_OUTDOOR
} ui_view_mode_t;


/**
 * Render one full “frame” worth of UI text.
 * This function overwrites the same text regions each update.
 */
void ui_render_frame(const ui_layout_t *layout,
                     ui_view_mode_t view,
                     float ambient_temp_c,
                     float shelly_temp_c,
                     float shelly_rh_pct,
                     bool shelly_valid,
                     const struct bme68x_data *data,
                     const baro_forecast_t *baro,
                     const air_quality_out_t *aq_out);


/* Render Screen 2 (min/max + confirm prompt) */
void ui_render_minmax(const ui_layout_t *layout,
                      const minmax_stats_t *s,
                      bool confirm_active,
                      ui_confirm_target_t confirm_target);