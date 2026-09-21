#include <cstdio>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "gamepad.h"

namespace {

struct PadDev {
   int fd;
   std::string path;
   bool btn[KEY_MAX];
   bool face[KEY_CNT];
   int dpad_x;
   int dpad_y;
};

std::vector<PadDev> g_pads;

bool has_bit(const unsigned long* bits, int code) {
   return (bits[code / (8 * sizeof(unsigned long))] &
           (1UL << (code % (8 * sizeof(unsigned long))))) != 0;
}

bool is_gamepad_event(int fd) {
   unsigned long evbits[EV_MAX / (8 * sizeof(unsigned long)) + 1] = {};
   if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0)
      return false;
   if (!has_bit(evbits, EV_KEY) && !has_bit(evbits, EV_ABS))
      return false;

   unsigned long keybits[KEY_MAX / (8 * sizeof(unsigned long)) + 1] = {};
   if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0)
      return false;

   /* A real gamepad exposes at least one BTN_* from the gamepad ranges
    * (BTN_SOUTH/EAST/WEST/NORTH or the classic BTN_A/B/X/Y, plus at least
    * DPAD / start / select on most). */
   bool has_gamepad_btn = false;
   for (int c = BTN_SOUTH; c <= BTN_DPAD_RIGHT; c++)
      has_gamepad_btn |= has_bit(keybits, c);
   if (!has_gamepad_btn)
      return false;

   unsigned long absbits[ABS_MAX / (8 * sizeof(unsigned long)) + 1] = {};
   if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0)
      return false;

   /* D-pad may be an axis (ABS_HAT0X/Y, xpad, DualShock) or buttons. Require
    * one stick (ABS_X/Y) OR a D-pad axis OR D-pad buttons, so keyboards and
    * mice (EV_REL, EV_LED, EV_MSC...) are skipped. */
   bool has_stick = has_bit(absbits, ABS_X) || has_bit(absbits, ABS_Y);
   bool has_hat = has_bit(absbits, ABS_HAT0X) || has_bit(absbits, ABS_HAT0Y);
   bool has_dpad_btn = has_bit(keybits, BTN_DPAD_UP) || has_bit(keybits, BTN_DPAD_DOWN);
   return has_stick || has_hat || has_dpad_btn;
}

void open_new_pads() {
   DIR* dir = opendir("/dev/input");
   if (!dir)
      return;

   struct dirent* ent;
   while ((ent = readdir(dir))) {
      if (strncmp(ent->d_name, "event", 5) != 0)
         continue;

      std::string path = std::string("/dev/input/") + ent->d_name;

      bool seen = false;
      for (const PadDev& p : g_pads)
         if (p.path == path)
            seen = true;
      if (seen)
         continue;

      int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
      if (fd < 0)
         continue;
      if (is_gamepad_event(fd)) {
         PadDev dev = {};
         dev.fd = fd;
         dev.path = path;
         g_pads.push_back(dev);
      } else {
         close(fd);
      }
   }
   closedir(dir);
}

} // namespace

void gamepad_poll() {
   static auto last_scan = std::chrono::steady_clock::now();
   auto now = std::chrono::steady_clock::now();
   if (g_pads.empty() || now - last_scan > std::chrono::seconds(3)) {
      last_scan = now;
      open_new_pads();
   }

   for (PadDev& p : g_pads) {
      struct input_event ev;
      for (;;) {
         ssize_t n = read(p.fd, &ev, sizeof(ev));
         if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
               break;
            break;
         }
         if ((size_t)n < sizeof(ev))
            break;

         if (ev.type == EV_KEY && ev.code < KEY_MAX && ev.value != 2) {
            p.btn[ev.code] = (ev.value == 1);
            if (ev.code == BTN_SOUTH || ev.code == BTN_EAST ||
                ev.code == BTN_WEST || ev.code == BTN_NORTH)
               p.face[ev.code] = p.btn[ev.code];
         } else if (ev.type == EV_ABS) {
            if (ev.code == ABS_HAT0X)
               p.dpad_x = (ev.value < 0 ? -1 : (ev.value > 0 ? 1 : 0));
            else if (ev.code == ABS_HAT0Y)
               p.dpad_y = (ev.value < 0 ? -1 : (ev.value > 0 ? 1 : 0));
         }
      }
   }
}

bool gamepad_available() {
   return !g_pads.empty();
}

bool gamepad_buttons_down(const uint16_t* codes, size_t count) {
   for (const PadDev& p : g_pads) {
      size_t held = 0;
      for (size_t i = 0; i < count; i++)
         if (p.btn[codes[i]])
            held++;
      if (held == count && count > 0)
         return true;
   }
   return false;
}

bool gamepad_chord_pressed() {
   static const uint16_t chord[] = { (uint16_t)BTN_START, (uint16_t)BTN_SELECT };
   return gamepad_buttons_down(chord, 2);
}

int gamepad_dpad_x() {
   int v = 0;
   for (const PadDev& p : g_pads) {
      if (p.dpad_x)
         v = p.dpad_x;
      if (p.btn[BTN_DPAD_LEFT])  v = -1;
      if (p.btn[BTN_DPAD_RIGHT]) v = 1;
   }
   return v;
}

int gamepad_dpad_y() {
   int v = 0;
   for (const PadDev& p : g_pads) {
      if (p.dpad_y)
         v = p.dpad_y;
      if (p.btn[BTN_DPAD_UP])   v = -1;
      if (p.btn[BTN_DPAD_DOWN]) v = 1;
   }
   return v;
}

bool gamepad_face_down(uint16_t code) {
   for (const PadDev& p : g_pads) {
      if (code < KEY_CNT && p.face[code])
         return true;
   }
   return false;
}

void gamepad_grab(bool grab) {
   for (PadDev& p : g_pads) {
      if (ioctl(p.fd, EVIOCGRAB, grab ? 1 : 0) < 0) {
         /* Another process may already hold the grab (Steam in exclusive
          * mode); not fatal, we keep reading events in that case. */
      }
   }
}