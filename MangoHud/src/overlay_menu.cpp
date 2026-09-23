#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#if defined(HAVE_X11) || defined(HAVE_WAYLAND)
#include <xkbcommon/xkbcommon.h>
#endif

#include "overlay_menu.h"
#include "gamepad.h"
#include "linux/input.h"
#include "mangohud_dbg.h"
#include "overlay.h"
#include "gpu.h"
#include "keybinds.h"
#include "fps_limiter.h"
#include "timing.hpp"
#include "logging.h"

/* ---------------------------------------------------------------------------
 * Keyboard handling
 *
 * keys_are_pressed() polls the X11 keymap / Wayland keysym set through the
 * same mechanism the keybinds use, so the menu works without any mouse or
 * ImGui input state. Selection is driven by plain int indices; ImGui is only
 * used to draw text rows.
 * ------------------------------------------------------------------------- */

#if defined(HAVE_X11) || defined(HAVE_WAYLAND)
static bool key_pressed(KeySym ks)
{
   return keys_are_pressed(std::vector<KeySym>{ ks });
}
static bool key_up_pressed()    { return key_pressed(XKB_KEY_Up)    || key_pressed(XKB_KEY_KP_Up); }
static bool key_down_pressed()  { return key_pressed(XKB_KEY_Down)  || key_pressed(XKB_KEY_KP_Down); }
static bool key_left_pressed()  { return key_pressed(XKB_KEY_Left)  || key_pressed(XKB_KEY_KP_Left); }
static bool key_right_pressed() { return key_pressed(XKB_KEY_Right) || key_pressed(XKB_KEY_KP_Right); }
static bool key_enter_pressed() { return key_pressed(XKB_KEY_Return) || key_pressed(XKB_KEY_KP_Enter); }
static bool key_esc_pressed()   { return key_pressed(XKB_KEY_Escape); }
#elif defined(_WIN32)
static bool key_pressed(KeySym ks)
{
   return keys_are_pressed(std::vector<KeySym>{ ks });
}
static bool key_up_pressed()    { return key_pressed(VK_UP); }
static bool key_down_pressed()  { return key_pressed(VK_DOWN); }
static bool key_left_pressed()  { return key_pressed(VK_LEFT); }
static bool key_right_pressed() { return key_pressed(VK_RIGHT); }
static bool key_enter_pressed() { return key_pressed(VK_RETURN); }
static bool key_esc_pressed()   { return key_pressed(VK_ESCAPE); }
#else
static bool key_up_pressed()    { return false; }
static bool key_down_pressed()  { return false; }
static bool key_left_pressed()  { return false; }
static bool key_right_pressed() { return false; }
static bool key_enter_pressed() { return false; }
static bool key_esc_pressed()   { return false; }
#endif

/* ---------------------------------------------------------------------------
 * X11 keyboard grab
 *
 * While the menu is open the game must not receive key events (Enter, arrows,
 * Esc would otherwise keep controlling the game underneath the menu). We hold
 * an active keyboard grab on the root window with owner_events=False, so all
 * key events are redirected to us and dropped (this overlay has no X event
 * loop; navigation still works because keys_are_pressed() polls the keymap
 * via XQueryKeymap, which is unaffected by grabs).
 *
 * Under Wayland-native there is no X display and the grab simply never
 * happens; that is acceptable.
 * ------------------------------------------------------------------------- */

#if defined(HAVE_X11)
static bool s_keyboard_grabbed = false;

/* A failed grab can surface as an async BadAccess X error (e.g. when another
 * client already holds the keyboard). The default X error handler exits the
 * process, so install a temporary no-op handler around the grab call. */
static int
menu_x11_error_noop(Display*, XErrorEvent*)
{
   return 0;
}

static void
menu_grab_keyboard(bool grab)
{
   if (!init_x11())
      return;

   auto libx11 = get_libx11();
   Display* dpy = get_xdisplay();
   if (!libx11 || !libx11->IsLoaded() || !dpy)
      return;

   if (grab) {
      if (s_keyboard_grabbed)
         return;
      /* on failure (AlreadyGrabbed by another client, GrabNotViewable, ...)
       * just log and keep the menu working without the grab */
      auto prev_handler = libx11->XSetErrorHandler(menu_x11_error_noop);
      int status = libx11->XGrabKeyboard(dpy,
                                         libx11->XDefaultRootWindow(dpy),
                                         False /* owner_events */,
                                         GrabModeAsync, GrabModeAsync,
                                         CurrentTime);
      libx11->XSetErrorHandler(prev_handler);
      if (status == GrabSuccess) {
         s_keyboard_grabbed = true;
         SPDLOG_DEBUG("Menu: X keyboard grabbed");
      } else {
         SPDLOG_DEBUG("Menu: XGrabKeyboard failed (status {})", status);
      }
   } else {
      if (!s_keyboard_grabbed)
         return;
      libx11->XUngrabKeyboard(dpy, CurrentTime);
      s_keyboard_grabbed = false;
      SPDLOG_DEBUG("Menu: X keyboard released");
   }
}
#endif

/* Menu state. Kept here (not in overlay_params) so it survives config
 * reloads, which reset the params struct in place. */
static bool s_open = false;
static int s_tab = 0;       /* 0 = HUD, 1 = lsfg-vk, 2 = Other */
static int s_sel = 0;       /* row selected in the current view */
static int s_profile = -1;  /* lsfg-vk: -1 = profile list, >= 0 = editing that profile */
static int s_hud_view = -1; /* HUD: -1 = main list, 0 = GPU subview, 1 = stats subview */

/* Feedback flash after an env change: which row changed, to what state, and
 * when. Displayed as "on/off (applies on next launch)" for a few seconds so
 * the user knows the change is not live until the game restarts. */
static int s_env_flash_row = -1;
static bool s_env_flash_on = false;
static Clock::time_point s_env_flash_at;

#define MENU_TAB_HUD      0
#define MENU_TAB_LSFGVK   1
#define MENU_TAB_ENV      2
#define MENU_TAB_OTHER    3
#define MENU_TAB_COUNT    4

#define MENU_HUD_VIEW_GPU   0
#define MENU_HUD_VIEW_STATS 1

/* ---------------------------------------------------------------------------
 * lsfg-vk config editing
 *
 * The config lives at ~/.config/lsfg-vk/conf.toml (or the path resolved by
 * the same rules as lsfg-vk's defaultConfigPath()). Only the multiplier and
 * pacing_mode of one [[profile]] are edited; the file is rewritten line by
 * line so every other key, value and comment is preserved byte for byte
 * (including keys this fork of lsfg-vk does not know about). The rewrite is
 * atomic (tmp file + rename) because the layer watches the file with inotify
 * (IN_MOVED_TO) and hot-reloads it on every present.
 * ------------------------------------------------------------------------- */

struct LsfgProfile {
   std::string name;
   std::string active_in;
   int multiplier = -1;
   int frame_limit = -1;             /* -1 = unknown / absent, 0 = off */
   std::string pacing;
   float flow_scale = -1.0f;         /* -1 = unknown / unparsed */
   bool performance_mode = false;
   bool unknown_performance = true;  /* true while the key is absent */
   bool override_present_mode = true;
   bool unknown_override = true;     /* true while the key is absent */
};

struct LsfgConfig {
   bool ok = false;
   std::string error;
   std::string path;
   std::vector<std::string> lines;     /* raw file content, one line each */
   std::vector<LsfgProfile> profiles;
};

static LsfgConfig s_lsfg;

static std::string
menu_trim(const std::string& s)
{
   size_t b = s.find_first_not_of(" \t\r\n");
   if (b == std::string::npos)
      return "";
   size_t e = s.find_last_not_of(" \t\r\n");
   return s.substr(b, e - b + 1);
}

/* key of a toml line, i.e. everything before '=' */
static std::string
menu_toml_key(const std::string& trimmed)
{
   auto eq = trimmed.find('=');
   if (eq == std::string::npos)
      return "";
   return menu_trim(trimmed.substr(0, eq));
}

/* value of a toml line, quotes stripped if the whole value is quoted */
static std::string
menu_toml_value(const std::string& line)
{
   auto eq = line.find('=');
   if (eq == std::string::npos)
      return "";
   std::string v = menu_trim(line.substr(eq + 1));
   if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
      v = v.substr(1, v.size() - 2);
   return v;
}

static int
menu_parse_int(const std::string& s)
{
   try {
      return std::stoi(s);
   } catch (...) {
      return -1;
   }
}

static float
menu_parse_float(const std::string& s)
{
   try {
      return std::stof(s);
   } catch (...) {
      return -1.0f;
   }
}

/* Path resolution mirrors lsfg-vk's utils::defaultConfigPath() */
static std::string
lsfg_config_path()
{
   if (const char* env = getenv("LSFGVK_CONFIG"))
      if (*env)
         return env;

   const char* home = getenv("HOME");
   const std::string home_str = home ? home : "";
   const bool is_flatpak = getenv("LSFGVK_FLATPAK") != nullptr;
   if (const char* xdg = getenv("XDG_CONFIG_HOME"))
      if (*xdg && !is_flatpak)
         return std::string(xdg) + "/lsfg-vk/conf.toml";
   if (!home_str.empty())
      return home_str + "/.config/lsfg-vk/conf.toml";
   return "/etc/lsfg-vk/conf.toml";
}

static bool
read_lsfg_config(LsfgConfig& cfg)
{
   cfg = LsfgConfig{};
   cfg.path = lsfg_config_path();

   std::ifstream in(cfg.path);
   if (!in) {
      cfg.error = "Cannot open " + cfg.path;
      return false;
   }

   std::string line;
   while (std::getline(in, line))
      cfg.lines.push_back(line);
   if (in.bad()) {
      cfg.error = "Error reading " + cfg.path;
      return false;
   }

   for (const auto& raw : cfg.lines) {
      std::string t = menu_trim(raw);
      if (t.rfind("[[profile]]", 0) == 0) {
         cfg.profiles.emplace_back();
         continue;
      }
      if (cfg.profiles.empty())
         continue;
      auto& p = cfg.profiles.back();
      std::string key = menu_toml_key(t);
      if (key == "name")
         p.name = menu_toml_value(t);
      else if (key == "active_in")
         p.active_in = menu_toml_value(t);
      else if (key == "multiplier")
         p.multiplier = menu_parse_int(menu_toml_value(t));
      else if (key == "pacing_mode" || key == "pacing")
         p.pacing = menu_toml_value(t);
      else if (key == "flow_scale")
         p.flow_scale = menu_parse_float(menu_toml_value(t));
      else if (key == "performance_mode") {
         p.performance_mode = (menu_toml_value(t) == "true");
         p.unknown_performance = false;
      } else if (key == "override_present_mode") {
         p.override_present_mode = (menu_toml_value(t) == "true");
         p.unknown_override = false;
      } else if (key == "frame_limit")
         p.frame_limit = menu_parse_int(menu_toml_value(t));
   }

   cfg.ok = true;
   return true;
}

static bool
find_profile_key_line(const std::vector<std::string>& lines, int profile_idx,
                      const std::string& key, size_t& out)
{
   int cur = -1;
   for (size_t i = 0; i < lines.size(); i++) {
      std::string t = menu_trim(lines[i]);
      if (t.rfind("[[profile]]", 0) == 0) {
         cur++;
         continue;
      }
      if (cur != profile_idx)
         continue;
      if (menu_toml_key(t) == key) {
         out = i;
         return true;
      }
   }
   return false;
}

/* Line index of the [[profile]] header of profile_idx */
static bool
find_profile_header_line(const std::vector<std::string>& lines, int profile_idx,
                         size_t& out)
{
   int cur = -1;
   for (size_t i = 0; i < lines.size(); i++) {
      std::string t = menu_trim(lines[i]);
      if (t.rfind("[[profile]]", 0) == 0) {
         cur++;
         if (cur == profile_idx) {
            out = i;
            return true;
         }
      }
   }
   return false;
}

static bool
apply_lsfg_change(int profile_idx, const std::string& key, const std::string& value)
{
   if (!s_lsfg.ok)
      return false;

   const std::string path = s_lsfg.path;

   /* Re-read right before writing so a concurrent edit made by the lsfg GUI
    * is not clobbered with a stale copy. */
   if (!read_lsfg_config(s_lsfg))
      return false;

   size_t line_idx = 0;
   if (!find_profile_key_line(s_lsfg.lines, profile_idx, key, line_idx)) {
      /* Key not present in this profile (e.g. old config without flow_scale):
       * insert it right after the profile's [[profile]] header instead of
       * failing, so the layer picks it up on the next hot reload. */
      if (!find_profile_header_line(s_lsfg.lines, profile_idx, line_idx)) {
         SPDLOG_ERROR("Could not find profile {} in {}", profile_idx, s_lsfg.path);
         return false;
      }
      s_lsfg.lines.insert(s_lsfg.lines.begin() + (line_idx + 1), key + " = " + value);
   } else {
      s_lsfg.lines[line_idx] = key + " = " + value;
   }

   const std::string tmp = path + ".mangohud.tmp";
   std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
   if (!out) {
      SPDLOG_ERROR("Failed to open {} for writing", tmp);
      return false;
   }
   for (const auto& l : s_lsfg.lines)
      out << l << '\n';
   out.flush();
   out.close();

   if (std::rename(tmp.c_str(), path.c_str()) != 0) {
      SPDLOG_ERROR("Failed to rename {} -> {}: {}", tmp, path, strerror(errno));
      ::unlink(tmp.c_str());
      return false;
   }

   /* refresh the in-memory model so the menu shows the new values */
   read_lsfg_config(s_lsfg);
   return true;
}

/* ---------------------------------------------------------------------------
 * lsfg-vk game detection
 *
 * Mirrors utils::identifyProcess() from lsfg-vk-config: the running game is
 * matched in the same order the layer itself uses (exe suffix -> wine .exe
 * suffix -> process name -> SteamAppId), so the menu opens the profile that
 * lsfg-vk would actually apply. When no profile matches, a new one is
 * created for the current game with everything off (multiplier=1 disables
 * the layer pipeline, pacing=vsync, override_present_mode=false) so nothing
 * changes until the user tweaks it from this tab.
 * ------------------------------------------------------------------------- */

static std::string
lsfg_basename(const std::string& path)
{
   auto slash = path.find_last_of('/');
   return slash == std::string::npos ? path : path.substr(slash + 1);
}

static bool
lsfg_active_matches(const std::string& full, const std::string& active)
{
   if (active.empty())
      return false;
   return full.size() >= active.size() &&
          full.compare(full.size() - active.size(), active.size(), active) == 0;
}

struct LsfgDetect {
   bool ok = false;             /* a usable active_in candidate was found */
   std::string exe_path;        /* readlink /proc/self/exe */
   std::string wine_exe;        /* first .exe path from /proc/self/maps */
   std::string process_name;    /* /proc/self/comm */
   std::string steam_appid;     /* $SteamAppId */
   std::string best_match;      /* active_in to use when creating a profile */
};

static LsfgDetect
lsfg_detect_process()
{
   LsfgDetect d;

   char buf[4096];
   ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
   if (n > 0) {
      buf[n] = '\0';
      d.exe_path = buf;
   }

   /* Wine/Proton: the real game is the first .exe mapped into the process */
   if (d.exe_path.find("wine") != std::string::npos ||
       d.exe_path.find("proton") != std::string::npos) {
      std::ifstream maps("/proc/self/maps");
      std::string line;
      while (std::getline(maps, line)) {
         if (!lsfg_active_matches(line, ".exe"))
            continue;
         size_t path_begin = line.find_first_of('/');
         if (path_begin == std::string::npos) {
            path_begin = line.find_first_of(' ');
            if (path_begin == std::string::npos)
               continue;
         }
         d.wine_exe = line.substr(path_begin);
         break;
      }
   }

   std::ifstream comm("/proc/self/comm");
   if (comm.is_open())
      std::getline(comm, d.process_name);

   const char* appid = std::getenv("SteamAppId");
   if (appid && *appid != '\0')
      d.steam_appid = appid;

   /* Same priority as identifyProcess(): wine .exe beats the wrapper exe */
   if (!d.wine_exe.empty())
      d.best_match = lsfg_basename(d.wine_exe);
   else if (!d.exe_path.empty())
      d.best_match = lsfg_basename(d.exe_path);
   else if (!d.process_name.empty())
      d.best_match = d.process_name;
   else if (!d.steam_appid.empty())
      d.best_match = d.steam_appid;

   d.ok = !d.best_match.empty();
   return d;
}

/* Index of the profile lsfg-vk would pick for this process, or -1. */
static int
lsfg_find_matching_profile(const LsfgDetect& d)
{
   if (std::getenv("LSFGVK_ENV"))
      return s_lsfg.profiles.empty() ? -1 : 0;

   const char* env_profile = std::getenv("LSFGVK_PROFILE");
   if (env_profile && *env_profile != '\0') {
      for (size_t i = 0; i < s_lsfg.profiles.size(); i++)
         if (s_lsfg.profiles[i].name == env_profile)
            return (int)i;
      return -1;
   }

   if (!d.exe_path.empty()) {
      for (size_t i = 0; i < s_lsfg.profiles.size(); i++)
         if (lsfg_active_matches(d.exe_path, s_lsfg.profiles[i].active_in))
            return (int)i;
   }

   if (!d.wine_exe.empty()) {
      for (size_t i = 0; i < s_lsfg.profiles.size(); i++)
         if (lsfg_active_matches(d.wine_exe, s_lsfg.profiles[i].active_in))
            return (int)i;
   }

   if (!d.process_name.empty()) {
      for (size_t i = 0; i < s_lsfg.profiles.size(); i++)
         if (s_lsfg.profiles[i].active_in == d.process_name)
            return (int)i;
   }

   if (!d.steam_appid.empty()) {
      for (size_t i = 0; i < s_lsfg.profiles.size(); i++)
         if (s_lsfg.profiles[i].active_in == d.steam_appid)
            return (int)i;
   }

   return -1;
}

/* Append a [[profile]] linked to the given active_in, everything off.
 * Returns its index in s_lsfg.profiles, or -1 on failure. When the config
 * file does not exist yet, a minimal lsfg-vk config is created from scratch
 * (version + global + this profile). */
static int
lsfg_create_profile_for(const std::string& active_in)
{
   const std::string path = lsfg_config_path();

   /* Re-read so a concurrent edit by the lsfg GUI is not clobbered. */
   if (!read_lsfg_config(s_lsfg)) {
      /* Missing file: build a fresh config containing this profile.
       * A file that exists but cannot be read is left untouched. */
      if (std::filesystem::exists(path))
         return -1;

      s_lsfg = LsfgConfig{};
      s_lsfg.path = path;
      s_lsfg.lines.push_back("version = 2");
      s_lsfg.lines.push_back("");
      s_lsfg.lines.push_back("[global]");
      s_lsfg.lines.push_back("allow_fp16 = true");
      s_lsfg.lines.push_back("");
   } else {
      s_lsfg.lines.push_back("");
   }

   s_lsfg.lines.push_back("[[profile]]");
   s_lsfg.lines.push_back("active_in = \"" + active_in + "\"");
   s_lsfg.lines.push_back("flow_scale = 1.0");
   s_lsfg.lines.push_back("frame_limit = 0");
   s_lsfg.lines.push_back("multiplier = 1");
   s_lsfg.lines.push_back("name = \"" + active_in + "\"");
   s_lsfg.lines.push_back("override_present_mode = false");
   s_lsfg.lines.push_back("pacing_mode = \"vsync\"");
   s_lsfg.lines.push_back("performance_mode = false");
   s_lsfg.lines.push_back("preserve_swapchain_image_count = false");

   const std::string tmp = path + ".mangohud.tmp";
   std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
   if (!out) {
      SPDLOG_ERROR("Failed to open {} for writing", tmp);
      return -1;
   }
   for (const auto& l : s_lsfg.lines)
      out << l << '\n';
   out.flush();
   out.close();

   if (std::rename(tmp.c_str(), path.c_str()) != 0) {
      SPDLOG_ERROR("Failed to rename {} -> {}: {}", tmp, path, strerror(errno));
      ::unlink(tmp.c_str());
      return -1;
   }

   if (!read_lsfg_config(s_lsfg))
      return -1;
   return s_lsfg.profiles.empty() ? -1 : (int)s_lsfg.profiles.size() - 1;
}

/* Open the profile linked to the current game, creating it when missing.
 * Falls back to the profile list when the process cannot be identified or
 * the config cannot be written (s_profile stays -1). */
static void
lsfg_auto_open()
{
   if (!s_lsfg.ok)
      return;

   LsfgDetect d = lsfg_detect_process();
   int idx = lsfg_find_matching_profile(d);
   if (idx < 0 && d.ok)
      idx = lsfg_create_profile_for(d.best_match);

   if (idx >= 0) {
      s_profile = idx;
      s_sel = 0;
   }
}

/* ---------------------------------------------------------------------------
 * lsfg-vk env vars subview
 *
 * Edits ~/.config/lsfg-vk/env.conf (ini: one section per game; there is no
 * [global] section on purpose, every env var is game-specific so a var that
 * breaks one game never leaks into the others). The mangoverlay wrapper
 * reads this file at launch and exports the vars of the section matching
 * the running game; a per-game "disable_all_vars = true" makes the wrapper
 * skip all of it, which is the fallback when a var breaks the game. The
 * menu mirrors the same section naming (active_in of the current profile,
 * e.g. GenshinImpact.exe).
 *
 * The tweak list is the one goverlay ships (TWEAK_ROWS in tweaks_md3.pas).
 * Rows that goverlay stores as Config/launch flags (gamemode, game-
 * performance) are handled by mangoverlay at exec time; the other rows are
 * plain KEY=VALUE env vars exported before the game starts.
 * ------------------------------------------------------------------------- */

enum EnvCat {
   ENV_CAT_GENERAL = 0,
   ENV_CAT_PERF,
   ENV_CAT_GRAPHICS,
   ENV_CAT_LATENCY,
   ENV_CAT_COUNT,
};

static const char* env_cat_names[ENV_CAT_COUNT] = {
   "General", "Performance", "Graphics", "Latency",
};

struct EnvTweak {
   const char* var;    /* env var name without the value */
   const char* value;  /* value to write, or NULL for a plain flag (no '=')[^1] */
   /* [^1] kept to mirror goverlay's ParseTweakLine; the menu writes
    * "key=value" lines so mangoverlay's awk picks them up. */
   EnvCat cat;
   int multi;          /* 1 = goverlay writes extra vars in the same block */
   const char* desc;   /* one-line description (mirrors goverlay's) */
};

/* goverlay TWEAK_ROWS (tweaks_md3.pas). Order matters only for display.
 * "gamemode" and "game-performance" are flags mangoverlay executes as a
 * pre-wrapper (gamemoderun / game-performance when installed), not env vars. */
static const EnvTweak s_env_tweaks[] = {
   { "SteamDeck",                  "1",                  ENV_CAT_GENERAL, 0, "Simulate Steam Deck hardware", },
   { "PROTON_ENABLE_HDR",          "1",                  ENV_CAT_GENERAL, 1, "Enable HDR", },
   { "PROTON_ENABLE_WAYLAND",      "1",                  ENV_CAT_GENERAL, 0, "Enable Wayland", },
   { "PROTON_LOG",                 "1",                  ENV_CAT_GENERAL, 0, "Active Proton Logs", },
   { "PROTON_USE_SDL",             "1",                  ENV_CAT_GENERAL, 0, "Use SDL input instead steam input", },
   { "OBS_VKCAPTURE",              "1",                  ENV_CAT_GENERAL, 0, "Activate Vulkan capture for OBS Studio", },
   { "PROTON_DISCORD_BRIDGE",      "1",                  ENV_CAT_GENERAL, 0, "[proton-cachyos] Enable Discord's Rich Presence", },
   { "gamemode",                   "1",                  ENV_CAT_PERF,    0, "Use Feral Gamemode set of optimisations", },
   { "game-performance",           "1",                  ENV_CAT_PERF,    0, "[cachyos] Sets CPU governor to performance", },
   { "PROTON_USE_WOW64",           "1",                  ENV_CAT_PERF,    0, "Windows 64-bit compatibility", },
   { "PROTON_FORCE_LARGE_ADDRESS_AWARE", "1",            ENV_CAT_PERF,    0, "Allows 32-bit games to use more than 2GB RAM", },
   { "STAGING_SHARED_MEMORY",      "1",                  ENV_CAT_PERF,    0, "Memory optimization for AMD GPUs", },
   { "PROTON_NO_NTSYNC",           "1",                  ENV_CAT_PERF,    0, "Disable NTSYNC", },
   { "PROTON_HEAP_DELAY_FREE",     "1",                  ENV_CAT_PERF,    0, "Delay in heap allocation (Wine)", },
   { "PROTON_LOCAL_SHADER_CACHE",  "1",                  ENV_CAT_PERF,    0, "[proton-cachyos] Enable per-game shader cache", },
   { "RADV_PERFTEST",              "rt,emulate_rt",      ENV_CAT_GRAPHICS, 0, "Emulates RT on older AMD GPUs", },
   { "PROTON_HIDE_NVIDIA_GPU",     "1",                  ENV_CAT_GRAPHICS, 0, "Hide Nvidia GPU", },
   { "PROTON_ENABLE_NVAPI",        "1",                  ENV_CAT_GRAPHICS, 0, "Force enable NVAPI", },
   { "PROTON_USE_WINED3D",         "1",                  ENV_CAT_GRAPHICS, 0, "Use old WINED3D", },
   { "MESA_LOADER_DRIVER_OVERRIDE","zink",               ENV_CAT_GRAPHICS, 1, "Uses OpenGL over Vulkan translation (ZINK)", },
   { "RADV_DEBUG",                 "nofastclears",       ENV_CAT_GRAPHICS, 0, "Disables fast clear optimization (AMD)", },
   { "PROTON_FSR4_UPGRADE",        "1",                  ENV_CAT_GRAPHICS, 0, "Automatically upgrade FSR to the latest version", },
   { "PROTON_DLSS_UPGRADE",        "1",                  ENV_CAT_GRAPHICS, 0, "Automatically upgrade DLSS to the latest version", },
   { "PROTON_XESS_UPGRADE",        "1",                  ENV_CAT_GRAPHICS, 0, "Automatically upgrade XeSS to the latest version", },
   { "LOW_LATENCY_LAYER",          "1",                  ENV_CAT_LATENCY, 0, "[low_latency_layer] Expose to enable the layer", },
   { "LOW_LATENCY_LAYER_REFLEX",   "1",                  ENV_CAT_LATENCY, 0, "[low_latency_layer] Expose Reflex support", },
   { "LOW_LATENCY_LAYER_SPOOF_NVIDIA", "1",              ENV_CAT_LATENCY, 0, "[low_latency_layer] Report device as NVIDIA GPU", },
   { "DXVK_CONFIG",                "dxgi.customDeviceDescription=10de:2204,dxgi.hideAmdGpu=True", ENV_CAT_LATENCY, 0, "[low_latency_layer] Also hide AMD GPU", },
   { "ENABLE_LAYER_MESA_ANTI_LAG", "1",                  ENV_CAT_LATENCY, 0, "[MESA] Enable AMD Anti-Lag 2", },
   { "PROTON_VKD3D_LOWLATENCY",    "1",                  ENV_CAT_LATENCY, 0, "[proton-cachyos] low-latency frame pacing capabilities", },
};

#define MENU_ENV_TWEAKS_COUNT (sizeof(s_env_tweaks) / sizeof(s_env_tweaks[0]))

static const char* env_conf_path()
{
   static std::string cached;
   if (!cached.empty())
      return cached.c_str();
   const std::string base = lsfg_config_path();
   /* conf.toml lives next to env.conf in the same dir */
   std::string p = base;
   auto pos = p.rfind("conf.toml");
   if (pos != std::string::npos)
      p.replace(pos, 9, "env.conf");
   cached = p;
   return cached.c_str();
}

/* Raw lines of env.conf, commented/blank preserved. */
struct EnvConfig {
   bool ok = false;
   std::string error;
   std::vector<std::string> lines;
};

static EnvConfig s_env;

static bool
env_section_present(const std::vector<std::string>& lines, const std::string& sec,
                    size_t& out)
{
   for (size_t i = 0; i < lines.size(); i++) {
      std::string t = menu_trim(lines[i]);
      if (t.size() >= 2 && t.front() == '[' && t.back() == ']' &&
          menu_trim(t.substr(1, t.size() - 2)) == sec) {
         out = i;
         return true;
      }
   }
   return false;
}

/* key/value of an env.conf KEY=VALUE line (respects quoting like goverlay) */
static bool
env_line_pair(const std::string& line, std::string& key, std::string& val)
{
   std::string t = menu_trim(line);
   if (t.empty() || t.front() == '#' || t.front() == '[')
      return false;
   auto eq = t.find('=');
   if (eq == std::string::npos)
      return false;
   key = menu_trim(t.substr(0, eq));
   val = menu_trim(t.substr(eq + 1));
   if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
      val = val.substr(1, val.size() - 2);
   return !key.empty();
}

static bool read_env_config()
{
   s_env = EnvConfig{};
   s_env.error.clear();
   std::ifstream in(env_conf_path());
   if (!in) {
      s_env.ok = true; /* missing file is fine: nothing to edit yet */
      s_env.error = "Cannot open " + std::string(env_conf_path());
      return true;
   }
   std::string line;
   while (std::getline(in, line))
      s_env.lines.push_back(line);
   if (in.bad()) {
      s_env.ok = false;
      s_env.error = "Error reading " + std::string(env_conf_path());
      return false;
   }
   s_env.ok = true;
   return true;
}

/* Current env.conf section name: the active_in of the open profile. */
static std::string
env_active_section()
{
   if (s_profile >= 0 && (size_t)s_profile < s_lsfg.profiles.size())
      return s_lsfg.profiles[s_profile].active_in;
   return "";
}

/* True when 'key' exists in section 'sec' (any value). */
static bool
env_var_enabled(const std::string& sec, const std::string& key)
{
   if (sec.empty() || !s_env.ok)
      return false;
   size_t s = 0;
   if (!env_section_present(s_env.lines, sec, s))
      return false;
   std::string k, v;
   for (size_t i = s + 1; i < s_env.lines.size(); i++) {
      std::string t = menu_trim(s_env.lines[i]);
      if (!t.empty() && t.front() == '[')
         break;
      if (env_line_pair(s_env.lines[i], k, v) && k == key)
         return true;
   }
   return false;
}

/* Rewrite env.conf atomically: set or delete 'key = value' in section 'sec'.
 * If 'value' is NULL the pair is removed; if the section is absent it is
 * appended at the end with a blank line before it. */
static bool
apply_env_change(const std::string& sec, const std::string& key,
                 const char* value)
{
   if (sec.empty())
      return false;
   if (!read_env_config() || !s_env.ok)
      return false;

   size_t sec_idx = 0;
   bool have_sec = env_section_present(s_env.lines, sec, sec_idx);

   auto find_key = [&](size_t start) -> size_t {
      for (size_t i = start; i < s_env.lines.size(); i++) {
         std::string t = menu_trim(s_env.lines[i]);
         if (!t.empty() && t.front() == '[')
            break;
         std::string k, v;
         if (env_line_pair(s_env.lines[i], k, v) && k == key)
            return i;
      }
      return std::string::npos;
   };

   size_t key_idx = have_sec ? find_key(sec_idx + 1) : std::string::npos;
   if (value == nullptr) {
      if (key_idx != std::string::npos)
         s_env.lines.erase(s_env.lines.begin() + key_idx);
   } else {
      const std::string line = key + " = " + value;
      if (key_idx != std::string::npos) {
         s_env.lines[key_idx] = line;
      } else if (have_sec) {
         /* insert right after the section header */
         s_env.lines.insert(s_env.lines.begin() + (sec_idx + 1), line);
      } else {
         if (!s_env.lines.empty() && !menu_trim(s_env.lines.back()).empty())
            s_env.lines.emplace_back();
         s_env.lines.push_back("[" + sec + "]");
         s_env.lines.push_back(line);
      }
   }

   const std::string path = env_conf_path();
   const std::string tmp = path + ".mangohud.tmp";
   std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
   if (!out) {
      SPDLOG_ERROR("Failed to open {} for writing", tmp);
      return false;
   }
   for (const auto& l : s_env.lines)
      out << l << '\n';
   out.flush();
   out.close();

   if (std::rename(tmp.c_str(), path.c_str()) != 0) {
      SPDLOG_ERROR("Failed to rename {} -> {}: {}", tmp, path, strerror(errno));
      ::unlink(tmp.c_str());
      return false;
   }
   read_env_config();
   return true;
}

/* goverlay expands these single rows to several env vars at write time. */
static void
env_toggle_row(size_t tweak_idx, bool on)
{
   if (tweak_idx >= MENU_ENV_TWEAKS_COUNT)
      return;
   const EnvTweak& tw = s_env_tweaks[tweak_idx];
   const std::string sec = env_active_section();

   if (!tw.multi) {
      apply_env_change(sec, tw.var, on ? tw.value : nullptr);
      return;
   }

   if (!on) {
      /* remove the whole block goverlay writes for this row */
      apply_env_change(sec, tw.var, nullptr);
      if (strcmp(tw.var, "PROTON_ENABLE_HDR") == 0) {
         apply_env_change(sec, "ENABLE_HDR_WSI", nullptr);
         apply_env_change(sec, "DXVK_HDR", nullptr);
      } else if (strcmp(tw.var, "MESA_LOADER_DRIVER_OVERRIDE") == 0) {
         apply_env_change(sec, "__GLX_VENDOR_LIBRARY_NAME", nullptr);
      }
      return;
   }

   apply_env_change(sec, tw.var, tw.value);
   if (strcmp(tw.var, "PROTON_ENABLE_HDR") == 0) {
      apply_env_change(sec, "ENABLE_HDR_WSI", "1");
      apply_env_change(sec, "DXVK_HDR", "1");
   } else if (strcmp(tw.var, "MESA_LOADER_DRIVER_OVERRIDE") == 0) {
      /* goverlay adds __GLX_VENDOR_LIBRARY_NAME=mesa only on nvidia hosts */
      if (std::filesystem::exists("/proc/driver/nvidia/version"))
         apply_env_change(sec, "__GLX_VENDOR_LIBRARY_NAME", "mesa");
   }
}

/* Remove the current profile's whole env section (the "Clear all" row). */
static void
env_clear_section()
{
   const std::string sec = env_active_section();
   if (sec.empty())
      return;
   if (!read_env_config() || !s_env.ok)
      return;
   size_t sec_idx = 0;
   if (!env_section_present(s_env.lines, sec, sec_idx))
      return;
   s_env.lines.erase(s_env.lines.begin() + sec_idx);
   /* drop the trailing blank line right before the removed header */
   if (sec_idx > 0 && menu_trim(s_env.lines[sec_idx - 1]).empty())
      s_env.lines.erase(s_env.lines.begin() + sec_idx - 1);

   const std::string path = env_conf_path();
   const std::string tmp = path + ".mangohud.tmp";
   std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
   if (!out) {
      SPDLOG_ERROR("Failed to open {} for writing", tmp);
      return;
   }
   for (const auto& l : s_env.lines)
      out << l << '\n';
   out.flush();
   out.close();
   if (std::rename(tmp.c_str(), path.c_str()) != 0) {
      SPDLOG_ERROR("Failed to rename {} -> {}: {}", tmp, path, strerror(errno));
      ::unlink(tmp.c_str());
      return;
   }
   read_env_config();
}

/* ---------------------------------------------------------------------------
 * HUD tab subviews
 *
 * s_hud_view selects between the main HUD list and two subviews, following
 * the same list/subview pattern the lsfg-vk tab uses with s_profile
 * (negative = list, >= 0 = subview). Every subview has "(back)" as row 0.
 * ------------------------------------------------------------------------- */

struct MenuStat { int idx; const char* label; };

static const MenuStat s_menu_stats[] = {
   { OVERLAY_PARAM_ENABLED_fps,             "FPS" },
   { OVERLAY_PARAM_ENABLED_frame_timing,    "Frame timing" },
   { OVERLAY_PARAM_ENABLED_gpu_stats,       "GPU stats" },
   { OVERLAY_PARAM_ENABLED_gpu_temp,        "GPU temp" },
   { OVERLAY_PARAM_ENABLED_gpu_core_clock,  "GPU core clock" },
   { OVERLAY_PARAM_ENABLED_gpu_mem_clock,   "GPU mem clock" },
   { OVERLAY_PARAM_ENABLED_gpu_power,       "GPU power" },
   { OVERLAY_PARAM_ENABLED_vram,            "VRAM" },
   { OVERLAY_PARAM_ENABLED_cpu_stats,       "CPU stats" },
   { OVERLAY_PARAM_ENABLED_cpu_temp,        "CPU temp" },
   { OVERLAY_PARAM_ENABLED_cpu_power,       "CPU power" },
   { OVERLAY_PARAM_ENABLED_ram,             "RAM" },
   { OVERLAY_PARAM_ENABLED_time,            "Time" },
   { OVERLAY_PARAM_ENABLED_engine_version,  "Engine version" },
   { OVERLAY_PARAM_ENABLED_gpu_name,        "GPU name" },
   { OVERLAY_PARAM_ENABLED_histogram,       "Histogram" },
   { OVERLAY_PARAM_ENABLED_frame_count,     "Frame count" },
   { OVERLAY_PARAM_ENABLED_resolution,      "Resolution" },
};

#define MENU_STATS_COUNT (sizeof(s_menu_stats) / sizeof(s_menu_stats[0]))

static int
hud_gpu_count()
{
   return (gpus && !gpus->available_gpus.empty())
             ? (int)gpus->available_gpus.size() : 0;
}

/* Enter on a GPU row (index into available_gpus). Selecting an idle GPU makes
 * it the single active one (params->pci_dev + is_active flags); Entering the
 * active GPU again resets to automatic selection (empty pci_dev). */
static void
menu_gpu_select(int idx)
{
   if (!gpus || (size_t)idx >= gpus->available_gpus.size())
      return;

   auto sp = get_params();
   auto& gpu = gpus->available_gpus[idx];

   if (gpu->is_active) {
      sp->pci_dev.clear();
      for (auto& g : gpus->available_gpus)
         g->is_active = false;
      SPDLOG_DEBUG("Menu: GPU selection reset to auto");
      return;
   }

   sp->pci_dev = gpu->pci_dev;
   for (size_t i = 0; i < gpus->available_gpus.size(); i++)
      gpus->available_gpus[i]->is_active = (gpus->available_gpus[i] == gpu);
   SPDLOG_DEBUG("Menu: selected GPU {}", gpu->pci_dev);
}

/* ---------------------------------------------------------------------------
 * Navigation
 * ------------------------------------------------------------------------- */

static int
tab_row_count()
{
   if (s_tab == MENU_TAB_HUD) {
      if (s_hud_view == MENU_HUD_VIEW_GPU)
         return 1 + hud_gpu_count();
      if (s_hud_view == MENU_HUD_VIEW_STATS)
         return 1 + (int)MENU_STATS_COUNT;
      return 6;
   }
   if (s_tab == MENU_TAB_LSFGVK) {
      /* OpenGL games never load the Vulkan layer, so frame generation cannot
       * apply: show the explanatory notice + Back instead of the editor. */
      if (!HUDElements.is_vulkan)
         return 1;
      if (!s_lsfg.ok || s_profile < 0)
         return 1; /* Retry */
      /* Multiplier, Pacing, Flow scale, Performance mode, Override present mode, Frame limit */
      return 6;
   }
   if (s_tab == MENU_TAB_ENV) {
      /* Disable all vars, one row per tweak, Clear all */
      return 1 + (int)MENU_ENV_TWEAKS_COUNT + 1;
   }
   return 2; /* MENU_TAB_OTHER */
}

static void
menu_move_selection(int dir)
{
   int n = tab_row_count();
   if (n <= 0)
      return;
   s_sel = (s_sel + dir + n) % n;
}

static void
menu_switch_tab(int dir)
{
   s_tab = (s_tab + dir + MENU_TAB_COUNT) % MENU_TAB_COUNT;
   s_sel = 0;
   s_profile = -1;
   s_hud_view = -1;
   if (s_tab == MENU_TAB_LSFGVK) {
      read_lsfg_config(s_lsfg);
      lsfg_auto_open();
   }
   if (s_tab == MENU_TAB_ENV) {
      /* need the active game name for the env.conf section */
      read_lsfg_config(s_lsfg);
      lsfg_auto_open();
      read_env_config();
   }
}

/* Rebuild the ordered HUD element list after a menu toggle. The list is
 * normally built once in parse_overlay_config() from enabled[] (legacy) or
 * from the config options; mutating enabled[] alone does not change what the
 * HUD draws, which is why toggling stats appeared to do nothing. */
static void
rebuild_hud_elements(const std::shared_ptr<overlay_params>& sp)
{
   if (sp->enabled[OVERLAY_PARAM_ENABLED_legacy_layout]) {
      HUDElements.legacy_elements(sp.get());
   } else {
      HUDElements.ordered_functions.clear();
      for (auto& option : HUDElements.options)
         HUDElements.sort_elements(option);
   }
}

static void
menu_activate()
{
   if (s_tab == MENU_TAB_HUD) {
      auto sp = get_params();
      if (s_hud_view == MENU_HUD_VIEW_GPU) {
         if (s_sel == 0) { /* (back) */
            s_hud_view = -1;
            s_sel = 0;
         } else {
            menu_gpu_select(s_sel - 1);
         }
         return;
      }
      if (s_hud_view == MENU_HUD_VIEW_STATS) {
         if (s_sel == 0) { /* (back) */
            s_hud_view = -1;
            s_sel = 0;
} else if ((size_t)s_sel - 1 < MENU_STATS_COUNT) {
            int idx = s_menu_stats[s_sel - 1].idx;
            sp->enabled[idx] = !sp->enabled[idx];
            rebuild_hud_elements(sp);
          }
         return;
      }
      switch (s_sel) {
      case 0:
         mangohud_dbg("MENU_TOGGLE: no_display %d -> %d", sp->no_display, !sp->no_display);
         sp->no_display = !sp->no_display;
         /* Keep the menu open: toggling the HUD off should not close or
          * shrink the menu. Renders continue while the menu is open because
          * the swapchain display gate tests (no_display && !menu_is_open()).
          * The window size below uses the stable io.DisplaySize, so the menu
          * never degenerates when the HUD is hidden. */
         break;
      case 1: {
         if (!fps_limiter)
            break;
         static const float s_common_limits[] = { 0.0f, 30.0f, 60.0f, 90.0f,
                                                  120.0f, 144.0f, 240.0f };
         const auto& v = get_params()->fps_limit;
         if (v.size() > 1) {
            /* user-configured multi-value list: keep cycling their values */
            fps_limiter->next_limit();
            break;
         }
         /* default {0} (or a single custom value): cycle the common presets,
          * wrapping back to unlimited after 240 */
         float cur = v.empty() ? 0.0f : (float)fps_limiter->current_limit();
         float next = s_common_limits[0];
         for (float common : s_common_limits) {
            if (cur < common) {
               next = common;
               break;
            }
         }
         fps_limiter->set_limit(next);
         break;
      }
      case 2: {
         unsigned v = sp->vsync;
         unsigned next = (v >= 4) ? 0u : v + 1u;
         sp->vsync = next;
         HUDElements.cur_present_mode = HUDElements.presentModes[next];
         break;
      }
      case 3:
         next_hud_position();
         break;
      case 4: /* GPU subview */
         s_hud_view = MENU_HUD_VIEW_GPU;
         s_sel = 0;
         break;
      case 5: /* Stats subview */
         s_hud_view = MENU_HUD_VIEW_STATS;
         s_sel = 0;
         break;
      }
      return;
   }

   if (s_tab == MENU_TAB_LSFGVK) {
      /* OpenGL games: the LSFGVK tab is a notice with a single Back row;
       * Enter closes the menu. */
      if (!HUDElements.is_vulkan) {
         menu_close();
         return;
      }
      if (!s_lsfg.ok) {
         read_lsfg_config(s_lsfg);
         lsfg_auto_open();
         return;
      }
      if (s_profile < 0) {
         lsfg_auto_open(); /* retry detection/creation */
         return;
      }
if ((size_t)s_profile >= s_lsfg.profiles.size()) {
      s_profile = -1;
      s_sel = 0;
      return;
   }
   switch (s_sel) {
   case 0: { /* multiplier 1..4 */
      auto& p = s_lsfg.profiles[s_profile];
      int n = (p.multiplier < 1 || p.multiplier >= 4) ? 1 : p.multiplier + 1;
      apply_lsfg_change(s_profile, "multiplier", std::to_string(n));
      break;
   }
case 1: { /* pacing mode: fixed to vsync (upstream only accepts vsync) */
       auto& p = s_lsfg.profiles[s_profile];
       if (p.pacing != "vsync")
          apply_lsfg_change(s_profile, "pacing_mode", "\"vsync\"");
       break;
    }
   case 2: { /* flow scale 0.25 -> 0.5 -> 0.75 -> 1.0 -> 0.25 */
      auto& p = s_lsfg.profiles[s_profile];
      float v = p.flow_scale;
      if (v < 0.25f || v >= 1.0f)
         v = 0.25f;
      else
         v += 0.25f;
      char buf[16];
      snprintf(buf, sizeof(buf), "%.2f", v);
      apply_lsfg_change(s_profile, "flow_scale", buf);
      break;
   }
   case 3: { /* performance mode toggle (unknown -> lsfg default: off) */
      auto& p = s_lsfg.profiles[s_profile];
      bool n = p.unknown_performance ? true : !p.performance_mode;
      apply_lsfg_change(s_profile, "performance_mode", n ? "true" : "false");
      break;
   }
   case 4: { /* override present mode toggle (unknown -> lsfg default: on) */
      auto& p = s_lsfg.profiles[s_profile];
      bool n = p.unknown_override ? false : !p.override_present_mode;
      apply_lsfg_change(s_profile, "override_present_mode", n ? "true" : "false");
      break;
   }
   case 5: { /* frame limit cycles 0 -> 15 -> 30 -> 45 -> 60 -> 90 -> 120 -> 144 -> 0 */
      auto& p = s_lsfg.profiles[s_profile];
      static constexpr int limits[] = {0, 15, 30, 45, 60, 90, 120, 144};
      constexpr size_t limit_count = sizeof(limits) / sizeof(limits[0]);
      int n = limits[1]; /* unknown/-1 or 0 -> first value above 0 */
      for (size_t i = 0; i < limit_count; i++) {
         if (p.frame_limit == limits[i]) {
            n = limits[(i + 1) % limit_count];
            break;
         }
      }
      apply_lsfg_change(s_profile, "frame_limit", std::to_string(n));
      break;
   }
   }
   return;
   }

   if (s_tab == MENU_TAB_ENV) {
      if (!s_lsfg.ok) {
         read_lsfg_config(s_lsfg);
         lsfg_auto_open();
         return;
      }
      if (s_profile < 0) {
         lsfg_auto_open(); /* retry detection/creation */
         return;
      }
      if ((size_t)s_profile >= s_lsfg.profiles.size()) {
         s_profile = -1;
         s_sel = 0;
         return;
      }
      if (s_sel == 0) { /* disable all vars for this game */
         const std::string sec = env_active_section();
         bool was = env_var_enabled(sec, "disable_all_vars");
         was ? apply_env_change(sec, "disable_all_vars", nullptr)
             : apply_env_change(sec, "disable_all_vars", "true");
         s_env_flash_row = 0;
         s_env_flash_on = !was;
         s_env_flash_at = Clock::now();
         return;
      }
      if ((size_t)s_sel - 1 < MENU_ENV_TWEAKS_COUNT) {
         bool on = env_var_enabled(env_active_section(),
                                   s_env_tweaks[s_sel - 1].var);
         env_toggle_row((size_t)s_sel - 1, !on);
         s_env_flash_row = (int)s_sel;
         s_env_flash_on = !on;
         s_env_flash_at = Clock::now();
         return;
      }
      if (s_sel == 1 + (int)MENU_ENV_TWEAKS_COUNT) { /* Clear all */
         env_clear_section();
         s_env_flash_row = 1 + (int)MENU_ENV_TWEAKS_COUNT;
         s_env_flash_on = false;
         s_env_flash_at = Clock::now();
         return;
      }
      return;
   }

   /* MENU_TAB_OTHER */
   switch (s_sel) {
   case 0: /* reload MangoHud config */
   {
      /* Parse into a fresh object; parse_overlay_config() atomically
       * publishes the new snapshot (get_params()) for every live reader. */
      auto fresh = std::make_shared<overlay_params>();
      mangohud_dbg("MENU_OTHER_RELOAD: before parse");
      parse_overlay_config(fresh.get(), getenv("MANGOHUD_CONFIG"), false);
      break;
   }
   case 1: { /* background alpha 0.0 -> 1.0 in 0.1 steps */
      auto sp = get_params();
      float a = std::round((sp->background_alpha + 0.1f) * 10.0f) / 10.0f;
      if (a > 1.0f)
         a = 0.0f;
      sp->background_alpha = a;
      break;
   }
   }
}

void menu_toggle_open()
{
   s_open = !s_open;
   if (s_open) {
      s_profile = -1;
      s_sel = 0;
      s_hud_view = -1;
      if (s_tab == MENU_TAB_LSFGVK) {
         read_lsfg_config(s_lsfg);
         lsfg_auto_open();
      }
      if (s_tab == MENU_TAB_ENV) {
         read_lsfg_config(s_lsfg);
         lsfg_auto_open();
         read_env_config();
      }
#if defined(HAVE_X11)
      menu_grab_keyboard(true);
#endif
      gamepad_grab(true);
      gamepad_poll();
   } else {
#if defined(HAVE_X11)
      menu_grab_keyboard(false);
#endif
      gamepad_grab(false);
   }
}

void menu_close()
{
   s_open = false;
#if defined(HAVE_X11)
   menu_grab_keyboard(false);
#endif
   gamepad_grab(false);
}

bool menu_is_open()
{
   return s_open;
}

void handle_menu_navigation()
{
   if (!s_open)
      return;

   gamepad_poll();

   using namespace std::chrono_literals;
   auto now = Clock::now();

   static Clock::time_point last_nav_move, last_tab_switch, last_activate, last_close;

   const auto navDelay = 120ms;
   const auto actionDelay = 400ms;

   /* Gamepad mapping: D-pad moves the selection, B (BTN_EAST) closes like
    * Esc, A (BTN_SOUTH) activates like Enter. */
   int gx = gamepad_dpad_x();
   int gy = gamepad_dpad_y();
   bool gp_back = gamepad_face_down(BTN_EAST);
   bool gp_enter = gamepad_face_down(BTN_SOUTH);

   if (now - last_close >= actionDelay && (key_esc_pressed() || gp_back)) {
      last_close = now;
      menu_close();
      return;
   }
   if (now - last_activate >= actionDelay && (key_enter_pressed() || gp_enter)) {
      last_activate = now;
      menu_activate();
      return;
   }
   if (now - last_tab_switch >= navDelay && (key_left_pressed() || gx < 0)) {
      last_tab_switch = now;
      menu_switch_tab(-1);
      return;
   }
   if (now - last_tab_switch >= navDelay && (key_right_pressed() || gx > 0)) {
      last_tab_switch = now;
      menu_switch_tab(1);
      return;
   }
   if (now - last_nav_move >= navDelay && (key_up_pressed() || gy < 0)) {
      last_nav_move = now;
      menu_move_selection(-1);
      return;
   }
   if (now - last_nav_move >= navDelay && (key_down_pressed() || gy > 0)) {
      last_nav_move = now;
      menu_move_selection(1);
      return;
   }
}

/* ---------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------- */

static const char* menu_tab_names[MENU_TAB_COUNT] = { "HUD", "lsfg-vk", "ENV", "Other" };
static const char* menu_position_names[] = {
   "top-left", "top-center", "top-right",
   "middle-left", "middle-right",
   "bottom-left", "bottom-center", "bottom-right",
};
static const char* menu_vsync_names[] = {
   "FIFO Relaxed", "IMMEDIATE", "MAILBOX", "FIFO",
};

static void
menu_row(bool selected, const char* label, const std::string& value)
{
   ImVec4 label_col = selected ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
                               : ImVec4(0.62f, 0.62f, 0.62f, 0.9f);
   ImVec4 value_col = selected ? ImVec4(0.53f, 1.0f, 0.45f, 1.0f)
                               : ImVec4(0.42f, 0.78f, 0.38f, 0.9f);
   ImGui::TextColored(label_col, "%s", label);
   if (!value.empty()) {
      ImGui::SameLine();
      ImGui::TextColored(value_col, ": %s", value.c_str());
   }
}

static void
draw_hud_tab(struct overlay_params& params)
{
   (void)params;
   auto sp = get_params();

   if (s_hud_view == MENU_HUD_VIEW_GPU) {
      ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 0.9f), "GPU selection");
      ImGui::Spacing();
      menu_row(s_sel == 0, "(back)", "");
      if (!gpus || gpus->available_gpus.empty()) {
         ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 0.9f), "No GPUs found");
      } else {
         for (size_t i = 0; i < gpus->available_gpus.size(); i++) {
            auto& g = gpus->available_gpus[i];
            char label[80];
            if (g->vendor_id || g->device_id)
               snprintf(label, sizeof(label), "GPU %zu (%04x:%04x)",
                        i + 1, g->vendor_id, g->device_id);
            else
               snprintf(label, sizeof(label), "GPU %zu (%s)",
                        i + 1, g->drm_node.c_str());
            if (s_sel == (int)i + 1)
               ImGui::SetScrollHereY(0.5f);
            menu_row(s_sel == (int)i + 1, label, g->is_active ? "active" : "idle");
         }
      }
      ImGui::Spacing();
      ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 0.9f),
                         "Enter: select GPU, Enter again: auto");
      return;
   }

   if (s_hud_view == MENU_HUD_VIEW_STATS) {
      ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 0.9f), "HUD stats");
      ImGui::Spacing();
      menu_row(s_sel == 0, "(back)", "");
      for (size_t i = 0; i < MENU_STATS_COUNT; i++) {
         bool on = sp->enabled[s_menu_stats[i].idx];
         bool selected = s_sel == (int)i + 1;
         if (selected)
            ImGui::SetScrollHereY(0.5f);
         menu_row(selected, s_menu_stats[i].label, on ? "on" : "off");
      }
      ImGui::Spacing();
      ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 0.9f), "Enter: toggle");
      return;
   }

   menu_row(s_sel == 0, "Toggle display", sp->no_display ? "off" : "on");

   std::string limit = "n/a";
   if (fps_limiter && fps_limiter->active) {
      float l = fps_limiter->current_limit();
      if (l <= 0.f)
         limit = "unlimited";
      else if (std::abs(l - std::round(l)) < 0.05f)
         limit = std::to_string((int)std::round(l)) + " fps";
      else {
         char buf[32];
         snprintf(buf, sizeof(buf), "%.1f fps", l);
         limit = buf;
      }
   }
   menu_row(s_sel == 1, "FPS limit", limit);

   unsigned v = sp->vsync;
   menu_row(s_sel == 2, "Vsync", (v < 4) ? menu_vsync_names[v] : "auto");

   unsigned pos = sp->position < LAYER_POSITION_COUNT ? sp->position : 0;
   menu_row(s_sel == 3, "HUD position", menu_position_names[pos]);

   menu_row(s_sel == 4, "GPU", sp->pci_dev.empty() ? "auto" : sp->pci_dev);
   menu_row(s_sel == 5, "Stats", "");
}

static void
draw_lsfg_tab(struct overlay_params& params)
{
   (void)params;

   if (!HUDElements.is_vulkan) {
      /* OpenGL games never load the Vulkan layer, so the lsfg-vk frame
       * generation can NOT be enabled for them: there is no Vulkan
       * swapchain to inject into. The MangoHud HUD and the ENV vars still
       * apply, so only this tab is replaced by an explanatory notice. */
      ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.3f, 1.0f),
                         "Frame generation requires Vulkan");
      ImGui::Spacing();
      ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 0.95f),
                         "This game renders through OpenGL, so the");
      ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 0.95f),
                         "lsfg-vk layer is not loaded and no frame");
      ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 0.95f),
                         "generation profile can apply to it.");
      ImGui::Spacing();
      ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 0.9f),
                         "MangoHud HUD options and the ENV tab still work.");
      ImGui::Spacing();
      menu_row(s_sel == 0, "Back", "");
      return;
   }

   if (!s_lsfg.ok) {
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", s_lsfg.error.c_str());
      ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 0.9f), "Path: %s", s_lsfg.path.c_str());
      ImGui::Spacing();
      menu_row(s_sel == 0, "Retry", "");
      return;
   }

   ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 0.9f), "Config: %s", s_lsfg.path.c_str());
   ImGui::Spacing();

   if (s_profile < 0) {
      /* Process not identified (or profile could not be created/written yet). */
      LsfgDetect detect = lsfg_detect_process();
      std::string game = detect.ok ? detect.best_match : "(unknown)";
      ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.3f, 1.0f), "Game: %s", game.c_str());
      if (detect.ok)
         ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 0.9f),
                            "No profile for this game yet");
      else
         ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 0.9f),
                            "Could not identify the running game");
      ImGui::Spacing();
      menu_row(s_sel == 0, detect.ok ? "Create profile" : "Retry", "");
      return;
   }

   if ((size_t)s_profile >= s_lsfg.profiles.size()) {
      s_profile = -1;
      s_sel = 0;
      return;
   }

   const auto& p = s_lsfg.profiles[s_profile];
   std::string header = p.name.empty() ? "profile " + std::to_string(s_profile + 1) : p.name;

   menu_row(s_sel == 0, "Multiplier", p.multiplier > 0 ? std::to_string(p.multiplier) : "?");
   menu_row(s_sel == 1, "Pacing", "vsync (fijo)");

   if (p.flow_scale < 0.0f) {
      menu_row(s_sel == 2, "Flow scale", "?");
   } else {
      char buf[16];
      snprintf(buf, sizeof(buf), "%.2f", p.flow_scale);
      menu_row(s_sel == 2, "Flow scale", buf);
   }
   menu_row(s_sel == 3, "Performance mode",
            p.unknown_performance ? "?" : (p.performance_mode ? "on" : "off"));
menu_row(s_sel == 4, "Override present mode",
         p.unknown_override ? "?" : (p.override_present_mode ? "on" : "off"));
   menu_row(s_sel == 5, "Frame limit",
            p.frame_limit > 0 ? std::to_string(p.frame_limit) : "off");
   ImGui::Spacing();
   ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 0.9f), "Profile: %s", header.c_str());
   ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 0.9f), "Changes are written to the config file");
}

/* True within ~3s after an env change flash was armed. */
static bool
env_flash_active(int row)
{
   if (s_env_flash_row < 0 || row != s_env_flash_row)
      return false;
   using namespace std::chrono_literals;
   return Clock::now() - s_env_flash_at < 3000ms;
}

/* Value label for an env row, appending the flash marker when the row was
 * just changed so the user knows the var applies only on next launch. */
static std::string
env_row_value(int row, bool on)
{
   std::string base = on ? "on" : "off";
   if (env_flash_active(row))
      base += " (applies on next launch)";
   return base;
}

static void
draw_env_tab(struct overlay_params& params)
{
   (void)params;

   if (!s_lsfg.ok) {
      menu_row(s_sel == 0, "Retry", "");
      return;
   }
   if (s_profile < 0) {
      /* Process not identified (or profile could not be created/written yet). */
      LsfgDetect detect = lsfg_detect_process();
      std::string game = detect.ok ? detect.best_match : "(unknown)";
      ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.3f, 1.0f), "Game: %s", game.c_str());
      if (detect.ok)
         ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 0.9f),
                            "No profile for this game yet");
      else
         ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 0.9f),
                            "Could not identify the running game");
      ImGui::Spacing();
      menu_row(s_sel == 0, detect.ok ? "Create profile" : "Retry", "");
      return;
   }
   if ((size_t)s_profile >= s_lsfg.profiles.size()) {
      s_profile = -1;
      s_sel = 0;
      return;
   }

   const std::string sec = env_active_section();
   ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 0.9f), "Env vars: %s",
                      sec.empty() ? "(no game)" : sec.c_str());
   ImGui::Spacing();

   menu_row(s_sel == 0, "Disable all vars for this game",
            env_row_value(0, env_var_enabled(sec, "disable_all_vars")));

   int row = 1;
   EnvCat cur_cat = ENV_CAT_COUNT;
   for (size_t i = 0; i < MENU_ENV_TWEAKS_COUNT; i++) {
      if (s_env_tweaks[i].cat != cur_cat) {
         cur_cat = s_env_tweaks[i].cat;
         ImGui::Spacing();
         ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 0.9f), "%s",
                            env_cat_names[cur_cat]);
         ImGui::Spacing();
      }
      bool on = env_var_enabled(sec, s_env_tweaks[i].var);
      if (s_sel == row)
         ImGui::SetScrollHereY(0.5f);
      menu_row(s_sel == row, s_env_tweaks[i].var, env_row_value(row, on));
      row++;
   }

   ImGui::Spacing();
   menu_row(s_sel == row, "Clear all for this game",
            env_row_value(row, false));
}

static void
draw_other_tab(struct overlay_params& params)
{
   (void)params;
   menu_row(s_sel == 0, "Reload MangoHud config", "");

   char buf[16];
   snprintf(buf, sizeof(buf), "%.1f", get_params()->background_alpha);
   menu_row(s_sel == 1, "Background alpha", buf);
}

void draw_overlay_menu(struct overlay_params& params)
{
   if (!s_open)
      return;

   /* io.DisplaySize is set once at context creation from the swapchain
    * extent. Do NOT chase GetMainViewport()->Size here: it can report 0/tiny
    * while the HUD is toggled off (we return before ImGui::NewFrame on those
    * frames), and a corrupt size makes the window constraints degenerate and
    * the menu shrink. Use the stable DisplaySize, clamped to a sane range. */
   const ImVec2 io_display = ImGui::GetIO().DisplaySize;
   const float dmin = std::min(io_display.x, io_display.y);
   const float dmax = std::max(io_display.x, io_display.y);
   const float disp_x = (dmin >= 100.0f) ? io_display.x : 1920.0f;
   const float disp_y = (dmin >= 100.0f) ? io_display.y : (dmax >= 100.0f ? dmax : 1080.0f);
   ImVec2 display(disp_x, disp_y);
   /* Fixed window size (not auto-fit): the menu must never change size when
    * the HUD state changes. Auto-size + child widgets whose heights depend on
    * GetContentRegionAvail() fed back into the window size, which made the
    * window shrink when toggling no_display. */
   const float win_w = std::min(std::max(display.x * 0.92f, 320.0f), 920.0f);
   const float win_h = std::min(std::max(display.y * 0.62f, 360.0f), 760.0f);
   const float start_y = std::max(0.05f * display.y,
                                  std::min(0.28f * display.y, display.y - win_h));
   ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, start_y), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
   ImGui::SetNextWindowBgAlpha(get_params()->background_alpha);
   ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_Always);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 10.0f));

   const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
      | ImGuiWindowFlags_NoMove
      | ImGuiWindowFlags_NoSavedSettings
      | ImGuiWindowFlags_NoFocusOnAppearing
      | ImGuiWindowFlags_NoNav;

   if (!ImGui::Begin("MangoHud Menu", nullptr, flags)) {
      ImGui::PopStyleVar();
      return;
   }

   ImGui::TextColored(ImVec4(0.53f, 1.0f, 0.45f, 1.0f), "MangoHud Menu");
   ImGui::SameLine();
   ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 0.8f), "  (Shift+F9)");
   ImGui::Separator();

   /* tabs */
   for (int i = 0; i < MENU_TAB_COUNT; i++) {
      if (i > 0) {
         ImGui::SameLine();
         ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 0.8f), "  |  ");
      }
      ImGui::SameLine();
      ImGui::TextColored(s_tab == i ? ImVec4(0.53f, 1.0f, 0.45f, 1.0f)
                                    : ImVec4(0.6f, 0.6f, 0.6f, 0.85f),
                         "%s", menu_tab_names[i]);
   }
   ImGui::Separator();

   /* scrollable content area: the window has a fixed size, so any tab whose
    * rows do not fit scrolls here (profile lists, stat toggles, GPU list).
    * The ENV tab reserves one extra footer line for the selected tweak
    * description, which must stay visible without scrolling. */
   float footer_h = (s_tab == MENU_TAB_ENV || s_tab == MENU_TAB_LSFGVK || s_tab == MENU_TAB_HUD)
                       ? 46.0f : 30.0f;
   if (ImGui::BeginChild("##menu_content",
                         ImVec2(0.0f, ImGui::GetContentRegionAvail().y - footer_h),
                         false)) {
      if (s_tab == MENU_TAB_HUD)
         draw_hud_tab(params);
      else if (s_tab == MENU_TAB_LSFGVK)
         draw_lsfg_tab(params);
      else if (s_tab == MENU_TAB_ENV)
         draw_env_tab(params);
      else
         draw_other_tab(params);
   }
   ImGui::EndChild();

   ImGui::Separator();
   const char* desc = nullptr;
   if (s_tab == MENU_TAB_ENV) {
      /* selected tweak description (fixed footer, never scrolled away) */
      if (s_sel == 0)
         desc = "disable_all_vars: skips every var of this game on next launch";
      else if (s_sel >= 1 && (size_t)s_sel - 1 < MENU_ENV_TWEAKS_COUNT)
         desc = s_env_tweaks[s_sel - 1].desc;
      else if (s_sel == 1 + (int)MENU_ENV_TWEAKS_COUNT)
         desc = "Clear all: removes this game's whole env.conf section";
   } else if (s_tab == MENU_TAB_LSFGVK) {
      if (!HUDElements.is_vulkan) {
         desc = "Frame generation needs a Vulkan swapchain; OpenGL games cannot use it. Back closes the menu";
      } else if (!s_lsfg.ok) {
         desc = "Retry: reload the lsfg-vk config file";
      } else if (s_profile < 0) {
         desc = "No profile for this game yet; Enter creates one";
      } else if ((size_t)s_profile < s_lsfg.profiles.size()) {
         switch (s_sel) {
         case 0:
            desc = "Multiplier: each game frame is launched N times (x2 => 120 fps from 60)";
            break;
         case 1:
            desc = "Pacing: vsync spaces generated frames evenly between real frames";
            break;
         case 2:
            desc = "Flow scale: optical-flow strength used to build generated frames";
            break;
         case 3:
            desc = "Performance mode: lower-quality flow, cheaper generated frames";
            break;
         case 4:
            desc = "Override present mode: sets the mode of the generated swapchain";
            break;
         case 5:
            desc = "Frame limit: caps the GAME rate pre-generation; HUD fps_limit caps the final rate";
            break;
         default:
            break;
         }
      }
   } else if (s_tab == MENU_TAB_HUD) {
      if (s_hud_view == MENU_HUD_VIEW_GPU) {
         desc = s_sel == 0 ? "Back to the HUD list"
                           : "Enter: monitor this GPU (Enter again on it: auto)";
      } else if (s_hud_view == MENU_HUD_VIEW_STATS) {
         desc = s_sel == 0 ? "Back to the HUD list"
                           : "Enter: toggle this stat on the HUD";
      } else {
         switch (s_sel) {
         case 0:
            desc = "Toggle display: shows or hides the HUD (same as the toggle_hud keybind)";
            break;
         case 1:
            desc = "FPS limit: MangoHud limiter, caps the final presented rate (after frame generation)";
            break;
         case 2:
            desc = "Vsync: force a present mode on the game swapchain";
            break;
         case 3:
            desc = "HUD position: screen corner where the HUD is drawn";
            break;
         case 4:
            desc = "GPU: GPU monitored by the HUD (auto: first GPU)";
            break;
         case 5:
            desc = "Stats: choose which stats the HUD shows";
            break;
         default:
            break;
         }
      }
   }
   if (desc)
      ImGui::TextColored(ImVec4(0.82f, 0.82f, 0.82f, 0.95f), "%s", desc);

   if (s_tab == MENU_TAB_ENV) {
      ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 0.9f),
                         "Vars apply on next launch    Up/Down: select    L/R: tab    Enter: activate    Esc: close");
   } else if (s_tab == MENU_TAB_LSFGVK) {
      ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 0.9f),
                         "Changes are written to the config file    Up/Down: select    L/R: tab    Enter: activate    Esc: close");
   } else {
      ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 0.9f),
                         "Up/Down: select    L/R: tab    Enter: activate    Esc: close");
   }

   ImGui::End();
   ImGui::PopStyleVar();
}