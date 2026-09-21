#pragma once
#ifndef MANGOHUD_GAMEPAD_H
#define MANGOHUD_GAMEPAD_H

#include <stdint.h>

/* Minimal evdev gamepad reader. No SDL, no XInput, no config: it reads the
 * standard Linux input devices (/dev/input/event*) emitted for every gamepad
 * (Xbox, DualShock/DualSense, Switch Pro, generic pads) and exposes the few
 * buttons the menu needs. All helpers are cheap enough to call every frame.
 *
 * The handle is treated as passive: we never grab the device, so Steam Input,
 * Lutris or the game itself keep working normally. Steam may re-expose the
 * pad as a "Steam Virtual Gamepad", which is still a normal evdev device and
 * is picked up the same way.
 */

/* Returns true once at least one gamepad has been successfully opened. */
bool gamepad_available();

/* True while ALL the given key codes are held down on the SAME device.
 * codes is an array of BTN_* values (linux/input.h). */
bool gamepad_buttons_down(const uint16_t* codes, size_t count);

/* True while the two "menu chord" buttons are held together on one pad.
 * Roughly "start + select". */
bool gamepad_chord_pressed();

/* D-pad: returns -1, 0 or 1 per axis (ABS_HAT0X / ABS_HAT0Y), merging pads
 * that report the D-pad as buttons (BTN_DPAD_*) instead of an axis. */
int gamepad_dpad_x();
int gamepad_dpad_y();

/* Face button (BTN_SOUTH = A/cross, BTN_EAST = B/circle, ...) held on any pad. */
bool gamepad_face_down(uint16_t code);

/* Grabs (true) or releases (false) every open pad with EVIOCGRAB. While
 * grabbed only we receive the events, so the game under the menu stops
 * seeing the controller — same effect as the X11 keyboard grab. No-op if
 * another process (e.g. Steam in exclusive mode) already holds the grab. */
void gamepad_grab(bool grab);

/* Re-reads events from the kernel. Call once per frame. */
void gamepad_poll();

#endif // MANGOHUD_GAMEPAD_H