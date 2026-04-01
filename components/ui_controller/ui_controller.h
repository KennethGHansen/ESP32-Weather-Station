#pragma once
/*
 * ui_controller.h
 *
 * Pure state machine for:
 *  - Screen selection (Overview vs Min/Max)
 *  - Two-press confirmation prompt for resets ("Are you sure?")
 *
 * This is intentionally hardware-independent so you can later call
 * the same actions from WiFi/remote endpoints.
 */

#include <stdbool.h>

typedef enum {
    UI_SCREEN_OVERVIEW = 0,
    UI_SCREEN_MINMAX   = 1
} ui_screen_t;

typedef enum {
    UI_CONFIRM_NONE = 0,
    UI_CONFIRM_TEMP,
    UI_CONFIRM_RH,
    UI_CONFIRM_PRESS
} ui_confirm_target_t;

typedef enum {
    UI_ACTION_UP = 0,
    UI_ACTION_LEFT,
    UI_ACTION_RIGHT,
    UI_ACTION_DOWN
} ui_action_t;

typedef struct
{
    ui_screen_t screen;

    bool confirm_armed;
    ui_confirm_target_t confirm_target;

} ui_controller_t;

void ui_controller_init(ui_controller_t *u);

ui_screen_t ui_controller_screen(const ui_controller_t *u);

bool ui_controller_confirm_active(const ui_controller_t *u);
ui_confirm_target_t ui_controller_confirm_target(const ui_controller_t *u);

/* returns true if UI state changed (redraw recommended) */
bool ui_controller_handle_action(ui_controller_t *u, ui_action_t act);

/* cancel prompt if active */
bool ui_controller_cancel_confirm(ui_controller_t *u);