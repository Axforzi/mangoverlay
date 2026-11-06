// Tests for the pure parts of the evdev gamepad reader in
// MangoHud/src/gamepad.{h,cpp}.
//
// The bugs these pin all shared one shape: the reader kept working, compiled,
// and looked right, but it desynchronised from the kernel without saying so.
//
//   - A replugged pad was never reopened. The kernel hands out the lowest free
//     event number, so a pad that comes back on the same /dev/input/eventN
//     path was treated as "already open" while its descriptor was dead. The
//     menu chord stayed dead until the whole game was restarted.
//   - Holding the menu chord repeated. There was a 400 ms delay but no edge
//     detection, so the menu opened and closed about 2.5 times a second, and
//     every toggle grabbed and released the pad with EVIOCGRAB.
//
// Both decisions are pure functions of plain data, so they are tested here
// without a kernel device. Nothing below touches /dev/input.

#include <cstdio>
#include <string>
#include <vector>

#include "gamepad.h"

static int failures = 0;

static void check(bool cond, const char* what)
{
   printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      failures++;
}

static std::string joined(const std::vector<std::string>& v)
{
   std::string s;
   for (const std::string& p : v) {
      if (!s.empty())
         s += ",";
      s += p;
   }
   return s;
}

static std::string joined(const std::vector<size_t>& v)
{
   std::string s;
   for (size_t n : v) {
      if (!s.empty())
         s += ",";
      s += std::to_string(n);
   }
   return s;
}

static void scan_case(const char* what,
                      std::vector<std::string> present,
                      std::vector<std::string> known,
                      std::vector<bool> alive,
                      const char* want_drop,
                      const char* want_open)
{
   gamepad_scan plan = gamepad_scan_plan(present, known, alive);
   const std::string got_drop = joined(plan.drop);
   const std::string got_open = joined(plan.open);
   const bool ok = got_drop == want_drop && got_open == want_open;
   printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
   if (!ok) {
      printf("      drop: got [%s] want [%s]\n", got_drop.c_str(), want_drop);
      printf("      open: got [%s] want [%s]\n", got_open.c_str(), want_open);
      failures++;
   }
}

int main()
{
   const std::string E6 = "/dev/input/event6";
   const std::string E7 = "/dev/input/event7";
   const std::string E9 = "/dev/input/event9";

   printf("-- gamepad_scan_plan --\n");

   // Nothing open yet: every pad the directory lists must be opened once.
   scan_case("first scan opens every pad",
             {E6, E7}, {}, {}, "", (E6 + "," + E7).c_str());

   // Steady state: a known, live pad must not be reopened on every rescan.
   scan_case("a live known pad is left alone",
             {E6}, {E6}, {true}, "", "");

   // The regression. A dead descriptor whose path is still present is the
   // exact shape of a replug that landed on the same event number, and it is
   // what used to leave the reader deaf for the rest of the process.
   scan_case("a dead pad on the same path is closed and reopened",
             {E6}, {E6}, {false}, "0", E6.c_str());

   // A second pad appearing next to a healthy one is additive only.
   scan_case("a new pad is opened without touching the live one",
             {E6, E7}, {E6}, {true}, "", E7.c_str());

   // Unplugged: the path is gone, so the descriptor is closed. Reopening is
   // not an option because the path no longer exists.
   scan_case("a vanished pad is dropped",
             {}, {E6}, {true}, "0", "");

   // Dead and gone must not produce a pointless reopen attempt.
   scan_case("a dead and vanished pad is dropped once",
             {}, {E6}, {false}, "0", "");

   // Defensive: a caller that supplies a shorter liveness list must not make
   // the reader keep a descriptor it cannot vouch for.
   scan_case("a pad with no liveness report counts as dead",
             {E6}, {E6}, {}, "0", E6.c_str());

   // Two pads, one replugged and one healthy: only the dead index moves.
   scan_case("only the dead index is replaced",
             {E6, E9}, {E6, E9}, {true, false}, "1", E9.c_str());

   printf("\n-- gamepad_edge --\n");

   {
      bool was = false;
      check(!gamepad_edge(false, was), "released on a released chord is not an edge");
      check(!was, "released chord leaves the previous state clear");
   }
   {
      bool was = false;
      check(gamepad_edge(true, was), "the press is one edge");
      check(was, "the press arms the previous state");
      check(!gamepad_edge(true, was), "holding the chord does not repeat");
      check(!gamepad_edge(true, was), "holding it longer still does not repeat");
      check(!gamepad_edge(false, was), "the release is not an edge");
      check(!was, "the release disarms the previous state");
      check(gamepad_edge(true, was), "a second press is an edge again");
   }

   printf("\n-- gamepad_hat_dir --\n");

   check(gamepad_hat_dir(-1) == -1, "minus one stays minus one");
   check(gamepad_hat_dir(0) == 0, "zero is neutral");
   check(gamepad_hat_dir(1) == 1, "one stays one");
   check(gamepad_hat_dir(-32768) == -1, "a full-scale negative axis clamps");
   check(gamepad_hat_dir(32767) == 1, "a full-scale positive axis clamps");

   printf("\n%s\n", failures ? "FAILURES" : "OK");
   return failures ? 1 : 0;
}
