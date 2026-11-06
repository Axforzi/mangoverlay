#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
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

/* Does the descriptor still point at a live device?
 *
 * After a USB disconnect the fd stays valid -- close() is the only thing that
 * frees it -- but every ioctl on it fails with ENODEV. Asking for the device
 * name is the cheapest way to tell a dead pad from a live one, and it does not
 * consume a queued event the way read() would. */
bool fd_is_alive(int fd) {
   char name[256];
   return ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0;
}

void apply_event(PadDev& p, const struct input_event& ev) {
   if (ev.type == EV_KEY && ev.code < KEY_MAX && ev.value != 2) {
      p.btn[ev.code] = (ev.value == 1);
      if (ev.code == BTN_SOUTH || ev.code == BTN_EAST ||
          ev.code == BTN_WEST || ev.code == BTN_NORTH)
         p.face[ev.code] = p.btn[ev.code];
   } else if (ev.type == EV_ABS) {
      if (ev.code == ABS_HAT0X)
         p.dpad_x = gamepad_hat_dir(ev.value);
      else if (ev.code == ABS_HAT0Y)
         p.dpad_y = gamepad_hat_dir(ev.value);
   }
}

/* Re-read the pad's real state from the kernel.
 *
 * Needed around EVIOCGRAB: events produced while another client holds the grab
 * reach nobody, and the kernel does not replay them on release. Without this
 * the cached d-pad and buttons keep whatever was last seen, so the menu cursor
 * runs away in the direction that was held when the menu closed. */
void resync_pad(PadDev& p) {
   std::memset(p.btn, 0, sizeof(p.btn));
   std::memset(p.face, 0, sizeof(p.face));

   unsigned long keybits[KEY_MAX / (8 * sizeof(unsigned long)) + 1] = {};
   if (ioctl(p.fd, EVIOCGKEY(sizeof(keybits)), keybits) >= 0) {
      for (int c = 0; c < KEY_MAX; c++)
         p.btn[c] = has_bit(keybits, c);
      for (int c = BTN_SOUTH; c <= BTN_NORTH; c++)
         p.face[c] = p.btn[c];
   }

   struct input_absinfo abs = {};
   p.dpad_x = ioctl(p.fd, EVIOCGABS(ABS_HAT0X), &abs) >= 0
                 ? gamepad_hat_dir(abs.value)
                 : 0;
   p.dpad_y = ioctl(p.fd, EVIOCGABS(ABS_HAT0Y), &abs) >= 0
                 ? gamepad_hat_dir(abs.value)
                 : 0;
}

/* Drain every queued event. Returns false once the device is gone, which is
 * the only reliable unplug signal: read() returns 0 or ENODEV, and the fd
 * itself stays allocated forever if it is not closed here. */
bool drain_pad(PadDev& p) {
   struct input_event ev;
   for (;;) {
      ssize_t n = read(p.fd, &ev, sizeof(ev));
      if (n == (ssize_t)sizeof(ev)) {
         apply_event(p, ev);
         continue;
      }
      if (n < 0) {
         if (errno == EINTR)
            continue;
         if (errno == EAGAIN || errno == EWOULDBLOCK)
            return true;
         return false; /* ENODEV / EIO: the pad is gone. */
      }
      if (n == 0)
         return false; /* clean EOF: the device went away. */
      /* A short read cannot be a whole event; keep the pad, drop the rest. */
      return true;
   }
}

bool open_pad(const std::string& path) {
   int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
   if (fd < 0)
      return false;
   if (!is_gamepad_event(fd)) {
      close(fd);
      return false;
   }

   PadDev dev = {};
   dev.fd = fd;
   dev.path = path;
   /* Start from what the device reports now, not from all-zero. */
   resync_pad(dev);
   g_pads.push_back(dev);
   return true;
}

/* List /dev/input/event*, compare against what is open, and act on the plan.
 *
 * This is the hotplug path. The kernel hands out the lowest free event number,
 * so a replugged pad almost always comes back on the same
 * /dev/input/eventN path as before: "the path is already known" says nothing
 * about whether its descriptor is still alive. gamepad_scan_plan() keeps that
 * decision pure and testable. */
void rescan_pads() {
   std::vector<std::string> present;
   DIR* dir = opendir("/dev/input");
   if (dir) {
      struct dirent* ent;
      while ((ent = readdir(dir))) {
         if (strncmp(ent->d_name, "event", 5) != 0)
            continue;
         present.push_back(std::string("/dev/input/") + ent->d_name);
      }
      closedir(dir);
   }
   /* readdir order is filesystem-dependent; the plan is index based. */
   std::sort(present.begin(), present.end());

   std::vector<std::string> known;
   std::vector<bool> alive;
   known.reserve(g_pads.size());
   alive.reserve(g_pads.size());
   for (const PadDev& p : g_pads) {
      known.push_back(p.path);
      alive.push_back(fd_is_alive(p.fd));
   }

   gamepad_scan plan = gamepad_scan_plan(present, known, alive);

   /* Erase from the back so the earlier indices stay valid. A dropped pad that
    * is still present is reopened right below, on the same path. */
   for (auto it = plan.drop.rbegin(); it != plan.drop.rend(); ++it) {
      close(g_pads[*it].fd);
      g_pads.erase(g_pads.begin() + *it);
   }

   for (const std::string& path : plan.open)
      open_pad(path);
}

} // namespace

gamepad_scan gamepad_scan_plan(const std::vector<std::string>& present,
                               const std::vector<std::string>& known,
                               const std::vector<bool>& alive) {
   gamepad_scan plan;

   for (size_t i = 0; i < known.size(); i++) {
      const bool listed = std::find(present.begin(), present.end(), known[i]) != present.end();
      /* A caller that supplies no liveness report cannot vouch for the pad,
       * so it is treated as dead and reopened. */
      const bool live = i < alive.size() ? alive[i] : false;
      if (!listed || !live)
         plan.drop.push_back(i);
   }

   for (const std::string& path : present) {
      auto it = std::find(known.begin(), known.end(), path);
      if (it == known.end()) {
         plan.open.push_back(path); /* new pad */
         continue;
      }
      const size_t idx = (size_t)(it - known.begin());
      if (std::find(plan.drop.begin(), plan.drop.end(), idx) != plan.drop.end())
         plan.open.push_back(path); /* same path, new device behind it */
   }

   return plan;
}

bool gamepad_edge(bool down, bool& was_down) {
   const bool edge = down && !was_down;
   was_down = down;
   return edge;
}

int gamepad_hat_dir(int value) {
   if (value < 0)
      return -1;
   if (value > 0)
      return 1;
   return 0;
}

void gamepad_poll() {
   /* Far enough in the past that the very first poll scans immediately. */
   static auto last_scan =
       std::chrono::steady_clock::now() - std::chrono::hours(1);
   auto now = std::chrono::steady_clock::now();

   /* With nothing open, look more often so a plugged pad is picked up quickly.
    * On the steady state a 3 s rescan is enough to notice a replug. */
   const auto interval = g_pads.empty() ? std::chrono::milliseconds(500)
                                        : std::chrono::seconds(3);
   if (now - last_scan > interval) {
      last_scan = now;
      rescan_pads();
   }

   bool lost = false;
   for (size_t i = 0; i < g_pads.size();) {
      if (drain_pad(g_pads[i])) {
         i++;
      } else {
         close(g_pads[i].fd);
         g_pads.erase(g_pads.begin() + i);
         lost = true;
      }
   }

   /* Reopen immediately instead of waiting for the periodic scan: a replugged
    * pad usually reappears on the same event number, and until it is reopened
    * the menu chord stays dead. */
   if (lost) {
      last_scan = now;
      rescan_pads();
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
      if (!grab)
         resync_pad(p);
   }
}
