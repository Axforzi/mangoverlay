// Tests for the lsfg-vk [global] block handling in overlay_menu.cpp.
//
// The functions under test are extracted from the real source by
// extract_functions.sh, so this exercises shipped code, not a copy of it.
//
// Background: three bugs in this area shared one shape. The menu wrote a value
// into conf.toml without asking the right question, so nothing failed at build
// time and nothing failed loudly at runtime:
//   - the `dll` key vanished on a config rebuild, and the frame generation
//     layer froze the game looking for a shader library it could not find
//   - a stale Vulkan layer manifest made the loader warn on every process start
//   - `allow_fp16` was hardcoded true on devices with no fp16 units
// These tests pin the behaviour that fixes them.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// --- stubs ------------------------------------------------------------------
// spdlog is available in the real build but not here; the format is fmt-style
// so these must not go through a printf-style check.
static std::vector<std::string> g_errors;
static std::vector<std::string> g_infos;
static void record(std::vector<std::string>& sink, const char* msg)
{ sink.emplace_back(msg); }
#define SPDLOG_ERROR(...) do { record(g_errors, "logged"); } while (0)
#define SPDLOG_INFO(...)  do { record(g_infos,  "logged"); } while (0)

// Stands in for the real definition in vulkan.cpp, which cannot be linked here.
// The tests flip g_fp16 to drive both sides of the capability check.
static bool g_fp16 = true;
static bool device_supports_fp16() { return g_fp16; }

#include "extracted.inc"

static int failures = 0;

static void check(bool cond, const char* what)
{
   printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond) failures++;
}

// Value of `key` inside [global], or "" when the key is absent.
static std::string global_value(const std::vector<std::string>& lines,
                                const std::string& key)
{
   size_t i = 0;
   if (!find_global_key_line(lines, key, i)) return "";
   return menu_toml_value(lines[i]);
}

int main()
{
   const std::string base = "lsfg-test-work";
   std::filesystem::create_directories(base + "/home");
   const std::string dll = base + "/fake-lsfg-vk.dll";
   { std::ofstream o(dll); o << "x"; }

   setenv("HOME", (base + "/home").c_str(), 1);
   unsetenv("XDG_CONFIG_HOME");
   unsetenv("XDG_DATA_HOME");
   unsetenv("MANGOVERLAY_LSFG_DLL");
   setenv("LSFGVK_CONFIG", (base + "/conf.toml").c_str(), 1);

   // A config with [global] present but no dll key, as a rebuild produces.
   auto fresh = [&] {
      return std::vector<std::string>{ "version = 2", "", "[global]",
                                       "allow_fp16 = true", "" };
   };

   // --- dll key: never invented, never lost, never overwritten ---------------
   {
      std::vector<std::string> lines = fresh();
      g_errors.clear();
      lsfg_ensure_dll_key(lines);
      check(global_value(lines, "dll").empty(),
            "no dll key invented when nothing is discoverable");
      check(!g_errors.empty(),
            "a missing dll is reported instead of failing silently");
   }
   {
      std::ofstream(base + "/dll.path") << dll << "\n";
      std::vector<std::string> lines = fresh();
      lsfg_ensure_dll_key(lines);
      check(global_value(lines, "dll") == dll,
            "dll key restored from the path the installer recorded");
   }
   {
      std::vector<std::string> lines{ "version = 2", "", "[global]",
                                      "dll = \"/my/own.dll\"", "" };
      lsfg_ensure_dll_key(lines);
      check(global_value(lines, "dll") == "/my/own.dll",
            "an existing dll key is preserved verbatim");
   }
   {
      setenv("MANGOVERLAY_LSFG_DLL", dll.c_str(), 1);
      check(lsfg_find_dll() == dll, "MANGOVERLAY_LSFG_DLL resolves the dll");
      setenv("MANGOVERLAY_LSFG_DLL", (base + "/missing.dll").c_str(), 1);
      check(lsfg_find_dll() == dll,
            "a non-existent override falls through to the next source");
      unsetenv("MANGOVERLAY_LSFG_DLL");
   }
   {
      std::ofstream(base + "/dll.path") << "/does/not/exist.dll\n";
      check(lsfg_find_dll().empty(), "a dangling dll.path is not trusted");
      std::ofstream(base + "/dll.path") << dll << "\n";
   }

   // --- [global] parsing must not leak across tables -------------------------
   {
      std::vector<std::string> lines{ "[global]", "allow_fp16 = true",
                                      "[other]", "dll = \"wrong.dll\"" };
      size_t i = 0;
      check(!find_global_key_line(lines, "dll", i),
            "[global] does not swallow the keys of a later table");
   }
   {
      std::vector<std::string> lines{ "[global]", "allow_fp16 = true", "",
                                      "[[profile]]", "dll = \"profile.dll\"" };
      size_t i = 0;
      check(!find_global_key_line(lines, "dll", i),
            "a profile-scoped dll key is not mistaken for a global one");
   }

   // --- allow_fp16 must follow the device, not a hardcoded string ------------
   {
      g_fp16 = true;   // Vega, RDNA, Turing and newer
      std::vector<std::string> lines{ "version = 2", "", "[global]", "" };
      lsfg_ensure_fp16_key(lines);
      check(global_value(lines, "allow_fp16") == "true",
            "a device with fp16 units gets allow_fp16 = true");
   }
   {
      g_fp16 = false;  // Polaris / GCN4 and older
      std::vector<std::string> lines{ "version = 2", "", "[global]", "" };
      lsfg_ensure_fp16_key(lines);
      check(global_value(lines, "allow_fp16") == "false",
            "a device without fp16 units gets allow_fp16 = false");
   }
   {
      g_fp16 = true;
      std::vector<std::string> lines{ "[global]", "allow_fp16 = false", "" };
      lsfg_ensure_fp16_key(lines);
      check(global_value(lines, "allow_fp16") == "false",
            "a user-set false is kept even on a capable device");
   }
   {
      g_fp16 = false;
      std::vector<std::string> lines{ "[global]", "allow_fp16 = true", "" };
      lsfg_ensure_fp16_key(lines);
      check(global_value(lines, "allow_fp16") == "true",
            "a user-set true is kept even on a device without fp16");
   }
   {
      g_fp16 = false;
      std::vector<std::string> lines{ "version = 2", "", "[global]", "" };
      lsfg_ensure_fp16_key(lines);
      g_errors.clear();
      lsfg_ensure_fp16_key(lines);
      check(g_errors.empty(),
            "no error is logged when the key is already present");
   }
   {
      g_fp16 = true;
      std::vector<std::string> lines{ "version = 2", "allow_fp16 = true" };
      g_errors.clear();
      lsfg_ensure_fp16_key(lines);
      check(!g_errors.empty(),
            "a missing [global] block is reported");
      check(lines.size() == 2,
            "nothing is inserted when there is no [global] to insert into");
   }

   // --- a rebuilt [global] ends up complete ---------------------------------
   {
      g_fp16 = false;
      std::vector<std::string> lines{ "version = 2", "", "[global]", "" };
      lsfg_ensure_dll_key(lines);
      lsfg_ensure_fp16_key(lines);
      check(global_value(lines, "dll") == dll &&
            global_value(lines, "allow_fp16") == "false",
            "a fresh [global] receives both keys");
      /* TOML is order-independent, so the invariant is placement: neither key
       * may land after the [global] section ends. */
      size_t hdr = 0, end = lines.size();
      find_global_header_line(lines, hdr);
      for (size_t i = hdr + 1; i < lines.size(); i++)
         if (!menu_trim(lines[i]).empty() && menu_trim(lines[i]).front() == '[')
            end = i;
      size_t di = 0, ai = 0;
      check(find_global_key_line(lines, "dll", di) && di < end &&
            find_global_key_line(lines, "allow_fp16", ai) && ai < end,
            "both keys land inside [global], not after it");
   }

   std::filesystem::remove_all(base);
   printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "ALL PASS", failures);
   return failures ? 1 : 0;
}
