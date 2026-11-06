#pragma once
#ifndef MANGOHUD_GAMEPAD_H
#define MANGOHUD_GAMEPAD_H

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

/* Minimal evdev gamepad reader. No SDL, no XInput, no config: it reads the
 * standard Linux input devices (/dev/input/event*) emitted for every gamepad
 * (Xbox, DualShock/DualSense, Switch Pro, generic pads) and exposes the few
 * buttons the menu needs. All helpers are cheap enough to call every frame.
 *
 * Reading is passive: Steam Input, Lutris or the game itself keep working
 * normally. The one exception is gamepad_grab(), which the overlay menu uses
 * while it is open so the game underneath does not react to the same
 * navigation keys. See its comment for what that costs and how it is undone.
 *
 * Steam may re-expose the pad as a "Steam Virtual Gamepad", which is still a
 * normal evdev device and is picked up the same way.
 */

/* Returns true once at least one gamepad has been successfully opened. */
bool gamepad_available();

/* True while ALL the given key codes are held down on the SAME device.
 * codes is an array of BTN_* values (linux/input.h). */
bool gamepad_buttons_down(const uint16_t* codes, size_t count);

/* True while the two "menu chord" buttons are held together on one pad.
 * Roughly "start + select".
 *
 * This is a level, not an edge: it stays true for as long as the chord is
 * held. Callers that toggle something must go through gamepad_edge(), or the
 * action repeats for as long as the user keeps their fingers down. */
bool gamepad_chord_pressed();

/* True only on the transition of `down` from false to true, and updates
 * `was_down` to match. Holding a chord down must not repeat: the menu used to
 * reopen and close on every keyPressDelay while the chord was held, which
 * grabbed and released the pad about 2.5 times a second. */
bool gamepad_edge(bool down, bool& was_down);

/* D-pad: returns -1, 0 or 1 per axis (ABS_HAT0X / ABS_HAT0Y), merging pads
 * that report the D-pad as buttons (BTN_DPAD_*) instead of an axis. */
int gamepad_dpad_x();
int gamepad_dpad_y();

/* One axis reading mapped to -1/0/1. Full-scale values clamp, so an axis
 * reported on a -32768..32767 range behaves the same as a -1..1 hat. */
int gamepad_hat_dir(int value);

/* Face button (BTN_SOUTH = A/cross, BTN_EAST = B/circle, ...) held on any pad. */
bool gamepad_face_down(uint16_t code);

/* What a rescan of /dev/input must do. `drop` are indices into the currently
 * open pads that must be closed; `open` are device paths that must be opened. */
struct gamepad_scan
{
   std::vector<size_t> drop;
   std::vector<std::string> open;
};

/* Decide what a rescan must do.
 *
 * `present` is what the directory lists right now, `known` is what is already
 * open, and alive[i] says whether known[i] still answers on its descriptor.
 *
 * A pad whose descriptor died, or whose path vanished, is dropped. Crucially,
 * a dead pad whose path is still present is opened AGAIN: the kernel hands out
 * the lowest free event number, so a replugged pad almost always comes back on
 * the same /dev/input/eventN path. Treating "path already known" as "already
 * open" is what used to leave the reader holding a dead descriptor -- and the
 * menu chord dead -- for the rest of the process lifetime.
 *
 * Pure by design: no syscalls, so the hotplug rules are testable without a
 * kernel device. */
gamepad_scan gamepad_scan_plan(const std::vector<std::string>& present,
                               const std::vector<std::string>& known,
                               const std::vector<bool>& alive);

/* Grabs (true) or releases (false) every open pad with EVIOCGRAB. While
 * grabbed only we receive the events, so the game under the menu stops seeing
 * the controller -- the same effect as the X11 keyboard grab. No-op if another
 * process (e.g. Steam in exclusive mode) already holds the grab.
 *
 * Events produced during the grab are delivered to nobody, so releasing also
 * re-reads each pad's real state from the kernel: otherwise our own d-pad and
 * buttons keep whatever was last seen, and the menu cursor runs away in the
 * direction the user was holding when they closed it. */
void gamepad_grab(bool grab);

/* Re-reads events from the kernel. Call once per frame. */
void gamepad_poll();

#endif // MANGOHUD_GAMEPAD_H
