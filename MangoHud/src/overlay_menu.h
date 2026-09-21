#pragma once
#ifndef MANGOHUD_OVERLAY_MENU_H
#define MANGOHUD_OVERLAY_MENU_H

#include "overlay_params.h"

/* Keyboard-driven ImGui settings menu (Shift+F9).
 *
 * The HUD has no mouse input support, so the menu is driven entirely by
 * keysyms polled through the same mechanism as the keybinds:
 *   Up/Down           move the selection
 *   Left/Right        switch tabs
 *   Enter             activate the selected row
 *   Esc               close the menu
 *
 * All state lives in overlay_menu.cpp so it survives config reloads
 * (parse_overlay_config() resets the params struct in place).
 */

bool menu_is_open();
void menu_toggle_open();
void menu_close();
void handle_menu_navigation();
void draw_overlay_menu(struct overlay_params& params);

#endif //MANGOHUD_OVERLAY_MENU_H