// Fulcrum Linux host — GTK4 floating launcher over one embedded Racket CS
// backend, speaking RVT1 through Rivet's platform/linux runtime.
//
// Mirrors the official Linux scaffold's threading model: boot the runtime
// off the main loop behind a mutex-guarded startup handoff, dispatch every
// completion to the main loop before touching widgets, and keep shutdown
// idempotent. Single-instance activation rides Rivet's first-party
// SingleInstanceLease (`fulcrum --toggle` forwards to the primary). The
// launcher-specific parts that have no rivet counterpart stay honest about
// their platform reach: X11 grabs keys and sets EWMH state directly;
// Wayland compositors own those decisions, and the status bar says so.
#include <gdk/x11/gdkx.h>
#include <gtk/gtk.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include "../GeneratedI18n.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
#include <unistd.h>
#include <array>
#include <cctype>
#include <functional>

#include "GeneratedBackend.hpp"
#include "system_services.hpp"
#include "update_handoff.hpp"
#include "autostart.hpp"
#include "x11_hotkey.hpp"

namespace {

struct ResultRow {
  std::string id;
  std::string title;
  std::string subtitle;
  std::string kind;
  std::string arg;
  std::string icon;
  std::string hint;
  std::string badge;
  // Optional 9th wire column (design v2): hit-highlight ranges (t:/s:,
  // code-point start-len runs) and the sensitive flag (x:sensitive).
  std::string meta;
};

ResultRow parse_row(std::vector<std::string> const& cells) {
  ResultRow row;
  if (cells.size() >= 8) {
    row.id = cells[0];
    row.title = cells[1];
    row.subtitle = cells[2];
    row.kind = cells[3];
    row.arg = cells[4];
    row.icon = cells[5];
    row.hint = cells[6];
    row.badge = cells[7];
    if (cells.size() >= 9) {
      row.meta = cells[8];
    }
  }
  return row;
}

// Hit ranges from the meta column: `t:` (title) or `s:` (subtitle),
// comma-separated start-len runs in Unicode code points — Pango byte
// offsets are derived at render time.
std::vector<std::pair<std::size_t, std::size_t>> meta_ranges(
    std::string const& meta, char field) {
  std::vector<std::pair<std::size_t, std::size_t>> out;
  std::string const key = std::string(1, field) + ":";
  std::size_t cursor = 0;
  while (cursor < meta.size()) {
    auto semicolon = meta.find(';', cursor);
    auto part = meta.substr(cursor, semicolon == std::string::npos
                                         ? std::string::npos
                                         : semicolon - cursor);
    if (part.rfind(key, 0) == 0) {
      std::size_t pc = key.size();  // skip the "t:"/"s:" key prefix
      while (pc < part.size()) {
        auto comma = part.find(',', pc);
        auto piece = part.substr(pc, comma == std::string::npos
                                         ? std::string::npos
                                         : comma - pc);
        auto dash = piece.find('-');
        if (dash != std::string::npos) {
          out.emplace_back(
              static_cast<std::size_t>(std::stoul(piece.substr(0, dash))),
              static_cast<std::size_t>(std::stoul(piece.substr(dash + 1))));
        }
        if (comma == std::string::npos) break;
        pc = comma + 1;
      }
    }
    if (semicolon == std::string::npos) break;
    cursor = semicolon + 1;
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool meta_sensitive(std::string const& meta) {
  return meta.find("x:sensitive") != std::string::npos;
}

// Payload for the actions-mode idle apply: rows plus the originating row
// the mode bar shows and Esc restores by item id. Namespace scope: the
// Launcher methods and their idle callbacks both name it.
struct ActionsJob {
  std::vector<ResultRow> rows;
  std::string parent_id;
  std::string parent_arg;
  std::string parent_title;
  std::string parent_badge;
};

// Code-point offset -> UTF-8 byte offset (Pango attributes are byte-based).
std::size_t byte_offset(std::string const& text, std::size_t code_points) {
  if (code_points >= text.size()) return text.size();
  return g_utf8_offset_to_pointer(text.c_str(), code_points) - text.c_str();
}

gboolean panel_is_dark() {
  gboolean dark = FALSE;
  if (GtkSettings* settings = gtk_settings_get_default()) {
    g_object_get(settings, "gtk-application-prefer-dark-theme", &dark,
                 nullptr);
  }
  return dark;
}

// Matched runs render in the readable amber at weight 650 — color +
// weight double cue, never a background fill (design v2 invariant 6).
void apply_hit_attributes(GtkLabel* label, std::string const& meta,
                          char field, std::string const& text) {
  auto ranges = meta_ranges(meta, field);
  if (ranges.empty()) return;
  gboolean dark = panel_is_dark();
  PangoAttrList* attrs = pango_attr_list_new();
  for (auto const& [start, length] : ranges) {
    auto const begin = byte_offset(text, start);
    auto const end = byte_offset(text, start + length);
    if (end <= begin || end > text.size()) continue;
    pango_attr_list_insert(attrs, pango_attr_weight_new(
        static_cast<PangoWeight>(650)));
    pango_attr_list_insert(
        attrs, pango_attr_foreground_new(
                   dark ? 0xF0F0 : 0x9A9A, dark ? 0xA6A6 : 0x4B4B,
                   dark ? 0x4B4B : 0x0606));
  }
  gtk_label_set_attributes(label, attrs);
  pango_attr_list_unref(attrs);
}

// ---- panel styling ----------------------------------------------------------
//
// One stylesheet drives the panel look — glass background, amber (#D97706)
// rows, the ↵ chip on the selected row only — the same design tokens the
// macOS host renders through vibrancy and the Windows host through Desktop
// Acrylic. GTK4 does not guarantee a blurred background: GNOME can show
// sharp window text and icons directly through a transparent launcher.
// Keep the surface almost opaque so results remain legible even without a
// compositor; rounded corners, border and selection retain the family look.
// Re-applied when the backend pushes the theme state.

static GtkCssProvider* g_panel_css = nullptr;

static void apply_panel_css() {
  gboolean dark = FALSE;
  if (GtkSettings* settings = gtk_settings_get_default()) {
    g_object_get(settings, "gtk-application-prefer-dark-theme", &dark,
                 nullptr);
  }
  char const* glass_css = dark
      ? R"CSS(
window.fulcrum-window {
  background-color: rgba(28,28,30,0.98);
  border-radius: 12px;
  border: 1px solid rgba(255,255,255,0.10);
  color: #f2f2f2;
}
)CSS"
      : R"CSS(
window.fulcrum-window {
  background-color: rgba(250,250,249,0.98);
  border-radius: 12px;
  border: 1px solid rgba(0,0,0,0.10);
  color: #1d1d1f;
}
)CSS";
  // Theme-dependent text tokens (docs/design-system.md): hover is a neutral
  // whisper so it never competes with the amber selection; glyphs/badges use
  // the readable amber pair (#9A4B06 / #F0A64B, both >= 4.5:1); hint and
  // status use the mute/faint ink-alpha tiers.
  char const* tokens_css = dark
      ? R"CSS(
window.fulcrum-window list row:hover { background-color: rgba(255,255,255,0.07); }
.fulcrum-row-icon { color: #F0A64B; }
.fulcrum-badge { color: #F0A64B; }
.fulcrum-enter { color: #F0A64B; }
.fulcrum-slot-header { color: #F0A64B; }
.fulcrum-hint { color: rgba(242,242,242,0.66); }
.fulcrum-status { color: rgba(242,242,242,0.52); }
window.fulcrum-window list row:focus { outline: 2px solid #F0A64B; outline-offset: -2px; }
window.fulcrum-window list.actions-mode row:selected {
  background-color: rgba(255,255,255,0.07);
  box-shadow: inset 0 0 0 1.5px #F0A64B;
}
window.fulcrum-window .fulcrum-modebar { border-bottom: 1px solid rgba(255,255,255,0.08); }
window.fulcrum-window .fulcrum-mode-title { color: rgba(242,242,242,0.66); }
)CSS"
      : R"CSS(
window.fulcrum-window list row:hover { background-color: rgba(0,0,0,0.05); }
.fulcrum-row-icon { color: #9A4B06; }
.fulcrum-badge { color: #9A4B06; }
.fulcrum-enter { color: #9A4B06; }
.fulcrum-slot-header { color: #9A4B06; }
.fulcrum-hint { color: rgba(29,29,31,0.66); }
.fulcrum-status { color: rgba(29,29,31,0.62); }
window.fulcrum-window list row:focus { outline: 2px solid #9A4B06; outline-offset: -2px; }
window.fulcrum-window list.actions-mode row:selected {
  background-color: rgba(0,0,0,0.05);
  box-shadow: inset 0 0 0 1.5px #9A4B06;
}
window.fulcrum-window .fulcrum-modebar { border-bottom: 1px solid rgba(0,0,0,0.08); }
window.fulcrum-window .fulcrum-mode-title { color: rgba(29,29,31,0.66); }
)CSS";
  static char const* const shared_css = R"CSS(
window.fulcrum-window entry {
  background: none;
  border: none;
  box-shadow: none;
  outline: none;
  font-size: 19px;
  padding: 10px 4px;
}
window.fulcrum-window list {
  padding: 2px 8px 8px;
  background: none;
}
window.fulcrum-window list row {
  min-height: 44px;
  border-radius: 8px;
  margin: 1px 2px;
  padding: 4px 8px;
  background-color: transparent;
}
window.fulcrum-window list row:selected {
  background-color: alpha(#D97706, 0.14);
}
.fulcrum-row-title {
  font-size: 13px;
  font-weight: 500;
}
.fulcrum-row-subtitle {
  font-size: 11.5px;
}
.fulcrum-row-icon {
  background-color: alpha(#D97706, 0.12);
  border-radius: 6px;
  padding: 5px;
}
.fulcrum-badge {
  background-color: alpha(#D97706, 0.14);
  border-radius: 9px;
  padding: 1px 7px;
  font-size: 10px;
  font-weight: 600;
}
.fulcrum-enter {
  font-size: 11px;
  font-weight: 600;
  opacity: 0;
}
list row:selected .fulcrum-enter {
  opacity: 1;
}
.fulcrum-slot-header {
  font-size: 11px;
  font-weight: 600;
}
.fulcrum-body {
  font-size: 12.5px;
}
.fulcrum-hint {
  font-size: 11px;
}
.fulcrum-status {
  font-size: 11px;
}
.fulcrum-modebar {
  padding: 4px 6px;
}
.fulcrum-modebar button {
  font-size: 10.5px;
  padding: 1px 6px;
  border-radius: 5px;
  min-height: 0;
}
.fulcrum-mode-title {
  font-size: 11px;
}
)CSS";

  GdkDisplay* display = gdk_display_get_default();
  if (display == nullptr) {
    return;
  }
  if (g_panel_css != nullptr) {
    gtk_style_context_remove_provider_for_display(
        display, GTK_STYLE_PROVIDER(g_panel_css));
    g_object_unref(g_panel_css);
    g_panel_css = nullptr;
  }
  g_panel_css = gtk_css_provider_new();
  gtk_css_provider_load_from_string(
      g_panel_css,
      (std::string(glass_css) + tokens_css + shared_css).c_str());
  gtk_style_context_add_provider_for_display(
      display, GTK_STYLE_PROVIDER(g_panel_css),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

// Symbolic icon candidates per row kind, checked against the actual theme
// so a bare system never paints the "missing icon" box.
static char const* pick_icon(std::initializer_list<char const*> candidates) {
  GtkIconTheme* theme =
      gtk_icon_theme_get_for_display(gdk_display_get_default());
  if (theme == nullptr) {
    return nullptr;
  }
  for (char const* name : candidates) {
    if (gtk_icon_theme_has_icon(theme, name)) {
      return name;
    }
  }
  return nullptr;
}

static char const* row_icon_name(ResultRow const& row) {
  if (row.kind == "Calculator") {
    return pick_icon({"accessories-calculator-symbolic"});
  }
  if (row.kind == "Clipboard") {
    return pick_icon({"edit-paste-symbolic"});
  }
  if (row.kind == "Snippet") {
    return pick_icon({"text-editor-symbolic"});
  }
  if (row.kind == "Quicklink") {
    return pick_icon({"web-browser-symbolic", "link-symbolic"});
  }
  if (row.kind == "Web Search") {
    return pick_icon({"web-browser-symbolic", "system-search-symbolic"});
  }
  if (row.kind == "Plugin") {
    return pick_icon({"view-grid-symbolic", "system-run-symbolic"});
  }
  if (row.kind == "System") {
    return row.id == "sys.lock"
               ? pick_icon({"system-lock-screen-symbolic"})
               : pick_icon({"preferences-system-symbolic",
                            "emblem-system-symbolic"});
  }
  if (row.kind == "Setting") {
    return pick_icon({"preferences-system-symbolic"});
  }
  if (row.kind == "Window") {
    return pick_icon({"window-new-symbolic", "view-grid-symbolic"});
  }
  if (row.kind == "Update") {
    return pick_icon({"document-save-symbolic", "go-down-symbolic"});
  }
  if (row.kind == "AI") {
    return pick_icon({"starred-symbolic", "system-search-symbolic"});
  }
  if (row.kind == "File") {
    return pick_icon({"folder-documents-symbolic"});
  }
  return pick_icon({"application-x-executable-symbolic"});
}

// Desktop-entry Icon values are supplied by the backend in column 5 of
// application rows. GTK resolves themed names (e.g. org.gnome.Terminal) and
// absolute icon file paths; a missing or broken icon falls back to the same
// semantic glyph as other hosts. Do not pass an arbitrary relative path to
// the filesystem: freedesktop only specifies an absolute path or theme name.
static GtkWidget* application_icon(ResultRow const& row) {
  if (row.kind != "Application" || row.icon.empty() || row.icon == "app") {
    return nullptr;
  }
  GtkWidget* image = nullptr;
  if (g_path_is_absolute(row.icon.c_str())) {
    if (g_file_test(row.icon.c_str(), G_FILE_TEST_IS_REGULAR)) {
      GFile* file = g_file_new_for_path(row.icon.c_str());
      GIcon* file_icon = g_file_icon_new(file);
      image = gtk_image_new_from_gicon(file_icon);
      g_object_unref(file_icon);
      g_object_unref(file);
    }
  } else {
    GtkIconTheme* theme =
        gtk_icon_theme_get_for_display(gdk_display_get_default());
    if (theme != nullptr &&
        gtk_icon_theme_has_icon(theme, row.icon.c_str())) {
      image = gtk_image_new_from_icon_name(row.icon.c_str());
    }
  }
  if (image != nullptr) {
    gtk_image_set_pixel_size(GTK_IMAGE(image), 28);
    gtk_widget_set_size_request(image, 28, 28);
    gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(image, "fulcrum-app-icon");
  }
  return image;
}

// Rivet's POSIX shutdown plumbing (defined after Launcher). Installed once
// the embedded runtime has booted — see on_backend_ready for the ordering.
void install_shutdown_hook_once();

// Tray menu composition (defined after Launcher). Called when the backend
// `language` setting lands so the labels follow it without a restart.
void apply_tray_language();

// Settings window (defined after Launcher): the tray menu and the leading
// settings row open it.
void show_settings_window();

// Original X11 pointer-monitor integration: @linyubin, PR #61.
// Work-area parity / GDK monitor lifetime fixes: maintainers.
void place_panel_on_pointer_monitor(GtkWindow* panel);

std::filesystem::path executable_path() {
  return std::filesystem::read_symlink("/proc/self/exe");
}

fulcrum::X11Hotkey g_x11_hotkey;

// Portal-side global shortcuts (rivet GlobalShortcuts adapter). Held for
// the process lifetime: the object owns the portal session and its async
// bind must not be destroyed mid-flight.
std::unique_ptr<rivet::system::GlobalShortcuts> g_portal_shortcuts;

// The launcher hotkey as (X11 modifier mask, keysym). The grammar is one
// spec shared with the backend (settings.rkt) and the other hosts:
// modifiers alt/ctrl/shift/cmd/win/super, then space | a-z | 0-9 |
// f1-f12 | comma | period | minus | equal | tab.
struct HotkeySpec {
  unsigned int mods{Mod1Mask};
  KeySym keysym{XK_space};
};

bool parse_hotkey(std::string const& spec, HotkeySpec& out) {
  unsigned int mods = 0;
  bool has_key = false;
  std::string key;
  std::string lowered = spec;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  std::size_t start = 0;
  while (start <= lowered.size()) {
    std::size_t plus = lowered.find('+', start);
    std::string token = lowered.substr(
        start, plus == std::string::npos ? std::string::npos : plus - start);
    if (token.empty()) {
      return false;
    }
    if (token == "alt" || token == "option") {
      mods |= Mod1Mask;
    } else if (token == "ctrl" || token == "control") {
      mods |= ControlMask;
    } else if (token == "shift") {
      mods |= ShiftMask;
    } else if (token == "cmd" || token == "win" || token == "super") {
      mods |= Mod4Mask;
    } else if (!has_key) {
      has_key = true;
      key = token;
    } else {
      return false;
    }
    if (plus == std::string::npos) {
      break;
    }
    start = plus + 1;
  }
  if (!has_key || mods == 0) {
    return false;
  }
  KeySym sym = NoSymbol;
  if (key == "space") {
    sym = XK_space;
  } else if (key == "tab") {
    sym = XK_Tab;
  } else if (key == "comma") {
    sym = XK_comma;
  } else if (key == "period") {
    sym = XK_period;
  } else if (key == "minus") {
    sym = XK_minus;
  } else if (key == "equal") {
    sym = XK_equal;
  } else if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z') {
    sym = XK_a + static_cast<KeySym>(key[0] - 'a');
  } else if (key.size() == 1 && key[0] >= '0' && key[0] <= '9') {
    sym = XK_0 + static_cast<KeySym>(key[0] - '0');
  } else if (key.size() >= 2 && key[0] == 'f' &&
             key.find_first_not_of("0123456789", 1) == std::string::npos) {
    int n = std::atoi(key.c_str() + 1);
    if (n < 1 || n > 12) {
      return false;
    }
    sym = XK_F1 + static_cast<KeySym>(n - 1);
  } else {
    return false;
  }
  out.mods = mods;
  out.keysym = sym;
  return true;
}

// Rebinds the X11 grab to `spec`. Returns a human-readable note on failure
// ("" on success). A spec that does not parse is ignored quietly: the
// previous binding stays and the hotkey never becomes unreachable.
std::string apply_x11_hotkey(std::string const& spec) {
  HotkeySpec parsed;
  if (!parse_hotkey(spec, parsed)) {
    return "";
  }
  std::string const failure = g_x11_hotkey.bind(parsed.keysym, parsed.mods);
  if (failure.empty()) {
    return "";
  }

  // A launcher must never become unreachable because its preferred chord is
  // already owned by the desktop. macOS and Windows both fall back to the
  // three-modifier chord; keep Linux on the same interaction contract. The
  // X11Hotkey object retains the previous binding until this replacement has
  // been grabbed successfully, so a failed fallback is still non-destructive.
  HotkeySpec fallback;
  if (parse_hotkey("ctrl+alt+space", fallback) &&
      !(parsed.keysym == fallback.keysym && parsed.mods == fallback.mods) &&
      g_x11_hotkey.bind(fallback.keysym, fallback.mods).empty()) {
    return "Requested hotkey unavailable; using Ctrl+Alt+Space.";
  }
  return failure;
}

// ---- native window management (win.* actions) -----------------------------
//
// The backend ranks the rows; the host performs the move, because only a
// client with an X connection can reach other apps' windows. Targets the
// active window, tiles relative to the monitor's _NET_WORKAREA, and asks
// the window manager via _NET_MOVERESIZE_WINDOW (the EWMH-sanctioned
// request that animates and respects constraints). Compile-tested like the
// rest of the Linux host; runtime verification tracks the Linux beta.

namespace wincmd {

struct Geometry {
  int x{0}, y{0}, w{0}, h{0};
};

std::map<unsigned long, Geometry>& saved() {
  static std::map<unsigned long, Geometry> map;
  return map;
}

bool send_moveresize(Display* dpy, Window target, int x, int y, int w, int h) {
  XEvent xev{};
  xev.xclient.type = ClientMessage;
  xev.xclient.window = target;
  xev.xclient.message_type =
      XInternAtom(dpy, "_NET_MOVERESIZE_WINDOW", False);
  xev.xclient.format = 32;
  // Bits 8-10: gravity (0 = use window's), bit 12: source indication
  // (1 = application).
  xev.xclient.data.l[0] = (1 << 12);
  xev.xclient.data.l[1] = x;
  xev.xclient.data.l[2] = y;
  xev.xclient.data.l[3] = w;
  xev.xclient.data.l[4] = h;
  return XSendEvent(dpy, DefaultRootWindow(dpy), False,
                    SubstructureRedirectMask | SubstructureNotifyMask,
                    &xev) != 0;
}

bool run_window_command(std::string const& id) {
  // GTK4 owns the Display; borrow it (nullptr on pure Wayland: the honest
  // failure path).
  GdkDisplay* gdk_display = gdk_display_get_default();
  if (gdk_display == nullptr || !GDK_IS_X11_DISPLAY(gdk_display)) {
    return false;
  }
  Display* dpy = gdk_x11_display_get_xdisplay(GDK_X11_DISPLAY(gdk_display));
  if (dpy == nullptr) {
    return false;
  }
  Window root = DefaultRootWindow(dpy);
  Atom active_atom = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True);
  if (active_atom == None) {
    return false;
  }
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;
  if (XGetWindowProperty(dpy, root, active_atom, 0, 1, False, XA_WINDOW,
                         &actual_type, &actual_format, &nitems, &bytes_after,
                         &data) != Success ||
      nitems < 1) {
    return false;
  }
  Window target = reinterpret_cast<unsigned long*>(data)[0];
  XFree(data);
  if (target == None) {
    return false;
  }

  // Work area of the monitor holding the window (root-relative).
  int x{0}, y{0};
  unsigned int w{0}, h{0}, bw{0}, depth{0};
  Window child = None;
  if (!XGetGeometry(dpy, target, &child, &x, &y, &w, &h, &bw, &depth)) {
    return false;
  }
  // Tiling area: the root _NET_WORKAREA (single-head accurate; per-head
  // workareas need XRandR probing, a later refinement).
  Atom workarea_atom = XInternAtom(dpy, "_NET_WORKAREA", True);
  long wx{0}, wy{0}, ww{0}, wh{0};
  if (workarea_atom != None) {
    unsigned char* wa = nullptr;
    if (XGetWindowProperty(dpy, root, workarea_atom, 0, 4, False, XA_CARDINAL,
                           &actual_type, &actual_format, &nitems,
                           &bytes_after, &wa) == Success &&
        nitems >= 4) {
      long* values = reinterpret_cast<long*>(wa);
      wx = values[0]; wy = values[1]; ww = values[2]; wh = values[3];
      XFree(wa);
    }
  }

  bool need_saved = id != "win.restore";
  if (need_saved) {
    saved()[target] = {x, y, static_cast<int>(w), static_cast<int>(h)};
  }

  if (id == "win.left") {
    return send_moveresize(dpy, target, static_cast<int>(wx), static_cast<int>(wy),
                           static_cast<int>(ww) / 2, static_cast<int>(wh));
  }
  if (id == "win.right") {
    return send_moveresize(dpy, target, static_cast<int>(wx) + static_cast<int>(ww) / 2,
                           static_cast<int>(wy), static_cast<int>(ww) / 2,
                           static_cast<int>(wh));
  }
  if (id == "win.maximize") {
    return send_moveresize(dpy, target, static_cast<int>(wx), static_cast<int>(wy),
                           static_cast<int>(ww), static_cast<int>(wh));
  }
  if (id == "win.almost-max") {
    int const mw = static_cast<int>(ww) * 9 / 10;
    int const mh = static_cast<int>(wh) * 88 / 100;
    return send_moveresize(dpy, target, static_cast<int>(wx) + (static_cast<int>(ww) - mw) / 2,
                           static_cast<int>(wy) + (static_cast<int>(wh) - mh) / 2, mw, mh);
  }
  if (id == "win.center") {
    return send_moveresize(dpy, target,
                           static_cast<int>(wx) + (static_cast<int>(ww) - static_cast<int>(w)) / 2,
                           static_cast<int>(wy) + (static_cast<int>(wh) - static_cast<int>(h)) / 2,
                           static_cast<int>(w), static_cast<int>(h));
  }
  if (id == "win.restore") {
    auto it = saved().find(target);
    if (it == saved().end()) {
      return false;
    }
    Geometry g = it->second;
    saved().erase(it);
    return send_moveresize(dpy, target, g.x, g.y, g.w, g.h);
  }
  return false;
}

}  // namespace wincmd

// ---- i18n (about + update surface) ------------------------------------------
//
// Strings are keyed exactly like shared/i18n/*.json — the single source of
// truth shared with the macOS host (I18nService) and the Windows host. The
// files are flat one-key-per-line JSON objects, so a line-based parser is
// enough. rivet.rktd declares shared/i18n under `resources`, so builds stage
// the files beside the host; the embedded fallback table below keeps the
// same keys for runs where nothing is staged. Lookup: active language → the
// other language → the key itself. The active language follows the backend's
// `language` setting, refreshed after settings changes as well as at startup.
using I18nTable = std::map<std::string, std::string>;

struct I18nEntry {
  char const* key;
  char const* zh;
  char const* en;
};

// Only the keys this host renders. The staged JSON (or the mac host's copy
// of the same table) carries the rest of the about/update surface.
I18nEntry const kI18nFallback[] = {
    {"updateAvailableStatus", "发现新版本：Fulcrum v{0} — 输入 “update” 下载",
     "Update available: Fulcrum v{0} — type \"update\" to download"},
    {"updateDownloadedStatusLinux",
     "更新已下载 — 输入 “update” 打开文件夹，解压 tar.gz 完成升级",
     "Update downloaded — type \"update\" to open the folder, then extract "
     "the tar.gz to upgrade"},
    {"updateDownloadFailed", "更新下载失败：{0}",
     "Update download failed: {0}"},
    {"updateUnknownError", "未知错误", "unknown error"},
    {"updateDownloadPercent", "正在下载更新… {0}%",
     "Downloading update… {0}%"},
    {"aboutContent",
     "一键即达 — 应用、剪贴板、片段、窗口管理与 BYOK AI，尽在一个面板",
     "One keystroke, everything within reach — apps, clipboard, snippets, "
     "window management and BYOK AI in one panel"},
    {"aboutLicense", "以 Business Source License 1.1 授权发布",
     "Released under the Business Source License 1.1"},
    {"aboutWebsite", "访问官网", "Website"},
    {"aboutReportIssue", "问题反馈", "Report an Issue"},
    {"trayOpen", "打开 Fulcrum", "Open Fulcrum"},
    {"traySettings", "设置…", "Settings…"},
    {"trayCheckUpdates", "检查更新…", "Check for Updates…"},
    {"trayAbout", "关于 Fulcrum", "About Fulcrum"},
    {"trayQuit", "退出 Fulcrum", "Quit Fulcrum"},
    {"appMenu", "应用菜单", "Application menu"},
    {"searchPlaceholder", "搜索应用、剪贴板、片段、网页…",
     "Search apps, clipboard, snippets, the web…"},
    {"navigationHint", "↑↓ 选择 · 回车执行 · Ctrl+K 操作 · Esc 清空/隐藏",
     "↑↓ navigate · ↵ run · ctrl+k actions · esc clear/hide"},
    {"navigationHintActions", "↑↓ 选择 · 回车执行 · Esc 返回",
     "↑↓ navigate · ↵ run · esc back"},
    {"noSecondaryActions", "此结果没有次要操作",
     "No secondary actions for this result"},
    {"ready", "就绪 — 使用热键唤出，或再次启动 Fulcrum",
     "Ready — summon with the hotkey, or launch Fulcrum again"},
    {"settingUpdated", "设置已更新", "Setting updated"},
    {"notSet", "未设置", "Not set"},
    {"setting.language", "语言", "Language"},
    {"setting.theme", "主题", "Theme"},
    {"setting.hotkey", "全局快捷键", "Hotkey"},
    {"setting.max-results", "候选数量", "Maximum results"},
    {"setting.clipboard-enabled", "剪贴板历史", "Clipboard history"},
    {"setting.expansion-enabled", "片段自动展开", "Snippet expansion"},
    {"setting.web-search-enabled", "网页搜索", "Web search"},
    {"setting.plugins-enabled", "插件", "Plugins"},
    {"setting.ai-provider", "AI 服务商", "AI provider"},
    {"setting.language.description", "回车切换中文与英文，立即生效",
     "Select to switch between Chinese and English immediately"},
    {"setting.theme.description", "回车切换跟随系统、浅色、深色，立即生效",
     "Select to cycle system, light and dark immediately"},
    {"setting.hotkey.description", "回车切换全局唤出快捷键",
     "Select to cycle the global summon hotkey"},
    {"setting.max-results.description", "回车切换候选数量",
     "Select to cycle the maximum results"},
    {"setting.clipboard-enabled.description", "回车切换剪贴板历史记录",
     "Select to toggle clipboard history"},
    {"setting.expansion-enabled.description", "自动展开在 Linux 上不受支持",
     "Automatic expansion is not supported on Linux"},
    {"setting.web-search-enabled.description", "回车切换网页搜索候选",
     "Select to toggle web search results"},
    {"setting.plugins-enabled.description", "回车切换插件加载",
     "Select to toggle plugin loading"},
    {"setting.ai-provider.description", "回车切换未设置、OpenAI、Anthropic、Ollama",
     "Select to cycle not set, OpenAI, Anthropic and Ollama"},
    {"updateUpToDate", "已是最新版本", "Fulcrum is up to date"},
    {"updateCheckFailed", "检查更新失败：{0}", "Update check failed: {0}"},
    {"trayFixHotkey", "修复 Alt+Space 冲突", "Fix Alt+Space conflict"},
    {"hotkeyShadowed",
     "唤出热键 {0} 被 GNOME 的窗口菜单占用 — 托盘菜单中的「修复 Alt+Space」可解除",
     "Summon hotkey {0} is shadowed by GNOME's window menu — \"Fix "
     "Alt+Space\" in the tray menu frees it"},
    {"hotkeyFreed",
     "已解除 Alt+Space 占用（GNOME 窗口菜单绑定已清空），现在试试热键",
     "Alt+Space freed — GNOME's window-menu binding cleared; try the "
     "hotkey now"},
};

I18nTable g_i18n[2];                // 0 = zh, 1 = en
std::atomic<int> g_i18n_active{1};  // "en" until pull_host_settings reports

// Index of the closing quote for the JSON string starting at `start`
// (which points at '"'), honoring backslash escapes.
std::size_t i18n_string_end(std::string const& line, std::size_t start) {
  for (std::size_t i = start + 1; i < line.size(); ++i) {
    if (line[i] == '\\') {
      ++i;
      continue;
    }
    if (line[i] == '"') {
      return i;
    }
  }
  return std::string::npos;
}

void i18n_append_utf8(std::string& out, unsigned int code) {
  if (code < 0x80) {
    out.push_back(static_cast<char>(code));
  } else if (code < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else if (code < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
}

std::string i18n_unescape(std::string const& raw) {
  std::string out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] != '\\') {
      out.push_back(raw[i]);
      continue;
    }
    if (++i >= raw.size()) {
      break;
    }
    switch (raw[i]) {
      case '"': out.push_back('"'); break;
      case '\\': out.push_back('\\'); break;
      case '/': out.push_back('/'); break;
      case 'n': out.push_back('\n'); break;
      case 't': out.push_back('\t'); break;
      case 'r': out.push_back('\r'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'u': {
        auto const hex4 = [&](std::size_t at, unsigned int& code) -> bool {
          if (at + 4 > raw.size()) return false;
          code = 0;
          for (std::size_t d = 0; d < 4; ++d) {
            char const h = raw[at + d];
            code <<= 4;
            if (h >= '0' && h <= '9') code += static_cast<unsigned int>(h - '0');
            else if (h >= 'a' && h <= 'f') code += static_cast<unsigned int>(h - 'a') + 10;
            else if (h >= 'A' && h <= 'F') code += static_cast<unsigned int>(h - 'A') + 10;
            else return false;
          }
          return true;
        };
        unsigned int code = 0;
        if (!hex4(i + 1, code)) {
          i = raw.size();
          break;
        }
        i += 4;
        if (code >= 0xD800 && code <= 0xDBFF && i + 6 < raw.size() &&
            raw[i + 1] == '\\' && raw[i + 2] == 'u') {
          unsigned int low = 0;
          if (hex4(i + 3, low) && low >= 0xDC00 && low <= 0xDFFF) {
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            i += 6;
          }
        }
        i18n_append_utf8(out, code);
        break;
      }
      default:
        out.push_back(raw[i]);
        break;
    }
  }
  return out;
}

// Tolerant loader for the flat "key": "value" table. Returns true when at
// least one entry was read; a malformed line is skipped, never fatal.
bool load_i18n_file(char const* path, int lang) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::string bytes((std::istreambuf_iterator<char>(file)),
                    std::istreambuf_iterator<char>());
  if (bytes.size() >= 3 && bytes[0] == '\xEF' && bytes[1] == '\xBB' &&
      bytes[2] == '\xBF') {
    bytes.erase(0, 3);  // tolerate a BOM; the generator does not emit one
  }
  if (bytes.empty()) {
    return false;
  }
  auto& table = g_i18n[lang];
  bool loaded_any = false;
  std::size_t begin = 0;
  while (begin < bytes.size()) {
    auto end = bytes.find('\n', begin);
    if (end == std::string::npos) {
      end = bytes.size();
    }
    std::string line = bytes.substr(begin, end - begin);
    begin = end + 1;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    auto const key_start = line.find('"');
    if (key_start == std::string::npos) {
      continue;
    }
    auto const key_end = i18n_string_end(line, key_start);
    if (key_end == std::string::npos) {
      continue;
    }
    auto const colon = line.find(':', key_end);
    if (colon == std::string::npos) {
      continue;
    }
    auto const value_start = line.find('"', colon + 1);
    if (value_start == std::string::npos) {
      continue;
    }
    auto const value_end = i18n_string_end(line, value_start);
    if (value_end == std::string::npos) {
      continue;
    }
    table[line.substr(key_start + 1, key_end - key_start - 1)] =
        i18n_unescape(line.substr(value_start + 1,
                                  value_end - value_start - 1));
    loaded_any = true;
  }
  return loaded_any;
}

void load_i18n(std::filesystem::path const& exe_dir) {
  char const* names[] = {"zh.json", "en.json"};
  for (int lang = 0; lang < 2; ++lang) {
    auto& table = g_i18n[lang];
    table.clear();
    for (auto const& entry : rivet_app::kI18nTable) {
      table.emplace(std::string(entry.key),
                    std::string(lang == 0 ? entry.zh : entry.en));
    }
    // Handwritten entries predate generated i18n and remain a small bootstrap
    // safety net. emplace deliberately cannot override the generated table.
    for (auto const& entry : kI18nFallback) {
      table.emplace(entry.key, lang == 0 ? entry.zh : entry.en);
    }
    // Staged product resources: rivet.rktd declares shared/i18n under
    // `resources`, so packages carry it at <dir>/app/shared/i18n; the dev
    // tree wins for `raco rivet dev` runs where nothing is packaged yet
    // (.rivet/stage holds the host binary one level below the repo root).
    std::filesystem::path const candidates[] = {
        exe_dir / "app" / "shared" / "i18n",
        exe_dir / ".." / "shared" / "i18n",  // dev tree
    };
    for (auto const& dir : candidates) {
      auto const path = dir / names[lang];
      std::error_code ec;
      if (!std::filesystem::exists(path, ec)) {
        continue;
      }
      if (load_i18n_file(path.c_str(), lang)) {
        break;
      }
    }
  }
}

void set_i18n_language(std::string const& lang);
void refresh_settings_language();
void show_update_window(bool check = true);
void update_window_refresh_language();
void update_window_present_available(std::string version, std::int64_t size);

std::string tr(char const* key) {
  int const active = g_i18n_active.load(std::memory_order_relaxed);
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto const& table = g_i18n[(active + attempt) % 2];
    auto const it = table.find(key);
    if (it != table.end()) {
      return it->second;
    }
  }
  return key;
}

// Positional formatting: first argument replaces {0}, and so on.
std::string tr_format(char const* key, std::vector<std::string> const& args) {
  std::string out = tr(key);
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string const token = "{" + std::to_string(i) + "}";
    auto const pos = out.find(token);
    if (pos != std::string::npos) {
      out.replace(pos, token.size(), args[i]);
    }
  }
  return out;
}

// ---- about ----------------------------------------------------------------

// The launcher row `about.open` (the backend ranks it, the host renders
// it): a native GTK about dialog — version, tagline, feedback pointer,
// license, website.
void show_about_dialog(GtkWindow* parent) {
  auto* dialog = gtk_about_dialog_new();
  gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog), "Fulcrum");
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), rivet_app::kVersion);
  std::string const comments =
      tr("aboutContent") + "\n" + tr("aboutReportIssue") +
      ": github.com/turlamai/fulcrum/issues";
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog), comments.c_str());
  std::string const license = tr("aboutLicense");
  gtk_about_dialog_set_license(GTK_ABOUT_DIALOG(dialog), license.c_str());
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog),
                               "https://fulcrum.jrtx.site");
  std::string const website_label = tr("aboutWebsite");
  gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog),
                                     website_label.c_str());
  gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_present(GTK_WINDOW(dialog));
}

// ---- application state ---------------------------------------------------

struct Launcher {
  GtkWindow* window{nullptr};
  GtkSearchEntry* search{nullptr};
  GtkListBox* results{nullptr};
  GtkLabel* status{nullptr};
  GtkButton* hotkey_fix{nullptr};
  GtkLabel* navigation_hint{nullptr};
  // Live AI answer panel (hidden until the backend pushes ai-stream).
  GtkWidget* ai_panel{nullptr};
  GtkLabel* ai_phase{nullptr};
  GtkLabel* ai_text{nullptr};
  // FPP2 plugin detail panel (hidden until a detail row runs).
  GtkWidget* detail_panel{nullptr};
  GtkLabel* detail_text{nullptr};
  GtkLabel* detail_meta{nullptr};

  std::unique_ptr<rivet::linux_runtime::Backend> backend;
  std::unique_ptr<rivet_app::API> api;
  // Tray presence: the visible way back once the panel hides (close
  // button, Esc, focus loss). StatusNotifierItem where a watcher runs —
  // GNOME's AppIndicator extension, KDE, wlroots trays; sessions without
  // one keep the portal hotkey and `fulcrum --toggle` as summon paths.
  std::unique_ptr<rivet::system::TrayIcon> tray;
  std::string tray_menu_lang{"en"};
  // GNOME's stock activate-window-menu binding owns Alt+Space: the tray
  // grows a one-click fix while the conflict stands.
  bool hotkey_shadowed{false};
  // A failed portal bind must remain visible after the backend finishes
  // starting or language/settings changes re-paint the routine Ready text.
  std::optional<std::string> hotkey_diagnostic;
  GtkApplication* app{nullptr};
  std::vector<ResultRow> rows;
  guint search_source{0};
  guint focus_loss_source{0};
  // Focus-loss dismissal starts only after the compositor has activated this
  // presentation at least once. Wayland may report a newly presented surface
  // as visible-but-inactive while it resolves activation; hiding during that
  // handoff made the v0.13.1 summon hotkey look completely dead on Ubuntu.
  bool activated_since_show{false};
  // The first visible frame must contain real backend rows or a boot error.
  // A login-item start deliberately has no pending summon.
  bool initial_results_ready{false};
  bool summon_when_ready{false};
  int host_settings_generation{0};
  int theme_revision{0};
  int hotkey_revision{0};
  // Matches the macOS/Windows hosts: only the newest query may paint, so a
  // slow completion can never overwrite a newer result set.
  std::atomic<int> search_generation{0};
  // Clipboard-history capture: the GTK host has no change notification, so
  // a 1s poll (same cadence as the macOS pasteboard timer) diffs the
  // clipboard text and records new values backend-side.
  std::string last_clipboard_text;

  // Official scaffold startup handoff: the boot thread publishes either a
  // started backend or an error; the main loop adopts it exactly once.
  std::mutex startup_mutex;
  std::thread startup_thread;
  std::unique_ptr<rivet::linux_runtime::Backend> startup_backend;
  std::string startup_error;
  std::atomic<bool> shutting_down{false};
  // Teardown is reachable from three threads — the GTK main loop
  // (GApplication shutdown, tray quit) and the shutdown-hook watcher
  // (SIGTERM/SIGINT) — so the stop itself is serialized, and the backend
  // stop runs once.
  std::mutex stop_mutex;
  bool backend_stopped_{false};
  // Ctrl+K mode: `rows` currently holds secondary actions and
  // search_rows remembers the live result list.
  bool actions_mode{false};
  std::vector<ResultRow> search_rows;
  // Design v2 actions layer: the mode bar (esc pill + parent row) and the
  // originating row identity Esc restores by item id.
  GtkWidget* actions_modebar{nullptr};
  GtkLabel* actions_mode_title{nullptr};
  GtkLabel* actions_mode_badge{nullptr};
  std::string actions_parent_id;
  std::string actions_parent_arg;
  // IME composition guard: true while the query IM context holds preedit.
  bool query_composing{false};

  static Launcher& instance() {
    static Launcher launcher;
    return launcher;
  }

  void set_status(std::string const& message) {
    gtk_label_set_text(status, message.c_str());
  }

  void set_ready_status() {
    set_status(hotkey_diagnostic.value_or(tr("ready")));
  }

  void set_hotkey_diagnostic(std::string message) {
    hotkey_diagnostic = std::move(message);
    set_ready_status();
  }

  void clear_hotkey_diagnostic() {
    hotkey_diagnostic.reset();
  }

  // Bind outcomes can arrive from a portal worker. Marshal updates to the
  // GTK thread and record the diagnostic before applying the label.
  static void post_hotkey_diagnostic(std::string message) {
    g_idle_add([](gpointer user_data) -> int {
      std::unique_ptr<std::string> diagnostic(
          static_cast<std::string*>(user_data));
      Launcher::instance().set_hotkey_diagnostic(std::move(*diagnostic));
      return G_SOURCE_REMOVE;
    }, new std::string(std::move(message)));
  }

  static void post_hotkey_bound(std::string message) {
    g_idle_add([](gpointer user_data) -> int {
      std::unique_ptr<std::string> text(
          static_cast<std::string*>(user_data));
      auto& launcher = Launcher::instance();
      launcher.clear_hotkey_diagnostic();
      launcher.set_status(*text);
      return G_SOURCE_REMOVE;
    }, new std::string(std::move(message)));
  }

  // Marshal a status string from any worker thread onto the main loop.
  static void post_status(std::string message) {
    g_idle_add([](gpointer user_data) -> int {
      std::unique_ptr<std::string> text(static_cast<std::string*>(user_data));
      Launcher::instance().set_status(*text);
      return G_SOURCE_REMOVE;
    }, new std::string(std::move(message)));
  }

  // ---- backend-rendered settings -------------------------------------------

  // Explicit light/dark uses GTK's built-in theme; system clears overrides.
  static void apply_theme(std::string const& theme) {
    GtkSettings* settings = gtk_settings_get_default();
    if (settings == nullptr) {
      return;
    }
    if (theme == "system") {
      gtk_settings_reset_property(settings, "gtk-theme-name");
      gtk_settings_reset_property(settings, "gtk-application-prefer-dark-theme");
    } else {
      g_object_set(settings, "gtk-theme-name", "Adwaita",
                   "gtk-application-prefer-dark-theme", theme == "dark", nullptr);
    }
    apply_panel_css();
  }

  void apply_ai_stream(rivet_app::AiStream const& stream) {
    if (stream.phase == "idle") {
      dismiss_ai_panel();
      return;
    }
    gtk_widget_set_visible(ai_panel, TRUE);
    update_panel_height();
    gtk_label_set_text(ai_text, stream.text.c_str());
    if (stream.phase == "running") {
      gtk_label_set_text(ai_phase, "AI is answering…");
    } else if (stream.phase == "error") {
      gtk_label_set_text(ai_phase, "AI request failed");
    } else {
      gtk_label_set_text(ai_phase, "AI answer");
    }
  }

  void dismiss_ai_panel() {
    update_panel_height(); gtk_widget_set_visible(ai_panel, FALSE); }

  void apply_plugin_detail(rivet_app::DetailView const& view) {
    gtk_widget_set_visible(detail_panel, TRUE);
    gtk_label_set_text(detail_text, view.text.c_str());
    std::string meta;
    for (auto const& pair : view.metadata) {
      if (pair.size() >= 2) {
        if (!meta.empty()) meta += "\n";
        meta += pair[0] + "  ·  " + pair[1];
      }
    }
    gtk_label_set_text(detail_meta, meta.c_str());
  }

  void dismiss_detail_panel() {
    update_panel_height();
    gtk_widget_set_visible(detail_panel, FALSE);
  }

  // The backend bumped search-revision (FPP2 daemon push): refresh the
  // rows for whatever query is on screen, debounced through the same
  // 120 ms path as typing.
  void refresh_for_revision() {
    if (search_source != 0) {
      g_source_remove(search_source);
    }
    search_source =
        g_timeout_add(250, [](gpointer) -> int {
          Launcher& l = Launcher::instance();
          l.search_source = 0;
          l.run_search(gtk_editable_get_text(GTK_EDITABLE(l.search)));
          return G_SOURCE_REMOVE;
        }, nullptr);
  }

  struct HostSettings {
    int generation{0};
    int theme_revision{0};
    int hotkey_revision{0};
    std::string hotkey;
    std::string theme;
    std::string language;
  };

  void apply_host_settings(HostSettings const& values) {
    if (values.generation != host_settings_generation || shutting_down.load()) return;
    if (!values.language.empty()) set_i18n_language(values.language);
    if (!values.hotkey.empty() && values.hotkey_revision == hotkey_revision) {
      std::string note = apply_x11_hotkey(values.hotkey);
      if (!note.empty()) set_status(note);
    }
    if (!values.theme.empty() && values.theme_revision == theme_revision) {
      apply_theme(values.theme);
    }
  }

  // Read the backend-owned settings the host renders (hotkey,
  // theme) plus the `language` key that steers the i18n table. The backend
  // pushes hotkey/theme changes afterwards via the reserved $state events;
  // this also refreshes language after a settings action.
  void pull_host_settings() {
    if (api == nullptr) {
      return;
    }
    auto* api_raw = api.get();
    auto* values = new HostSettings;
    values->generation = ++host_settings_generation;
    values->theme_revision = theme_revision;
    values->hotkey_revision = hotkey_revision;
    std::thread([api_raw, values]() mutable {
      try {
        // Row shape per the backend settings-list contract: [key, value, …].
        // One RPC provides a coherent snapshot rather than three mixed reads.
        for (auto const& row : api_raw->settings_list().get()) {
          if (row.size() < 2) continue;
          if (row[0] == "language") values->language = row[1];
          else if (row[0] == "theme") values->theme = row[1];
          else if (row[0] == "hotkey") values->hotkey = row[1];
        }
      } catch (...) {
      }
      g_idle_add([](gpointer user_data) -> int {
        std::unique_ptr<HostSettings> values(static_cast<HostSettings*>(user_data));
        Launcher::instance().apply_host_settings(*values);
        return G_SOURCE_REMOVE;
      }, values);
    }).detach();
  }

  // ---- clipboard history capture --------------------------------------------

  static void on_clipboard_read(GObject* source, GAsyncResult* result,
                                gpointer) {
    Launcher& launcher = Launcher::instance();
    gchar* text =
        gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, nullptr);
    if (text == nullptr) {
      return;
    }
    if (text != launcher.last_clipboard_text) {
      launcher.last_clipboard_text = text;
      std::string payload = text;
      std::thread([payload]() mutable {
        Launcher& launcher = Launcher::instance();
        if (launcher.shutting_down.load(std::memory_order_acquire)) {
          return;
        }
        auto* api_raw = launcher.api.get();
        if (api_raw == nullptr) {
          return;
        }
        try {
          api_raw->clipboard_record(payload);
        } catch (...) {
          // Persistence must never take the launcher down.
        }
      }).detach();
    }
    g_free(text);
  }

  static gboolean poll_clipboard(gpointer) {
    Launcher& launcher = Launcher::instance();
    if (launcher.api == nullptr ||
        launcher.shutting_down.load(std::memory_order_acquire)) {
      return G_SOURCE_CONTINUE;
    }
    GdkClipboard* clipboard =
        gtk_widget_get_clipboard(GTK_WIDGET(launcher.window));
    gdk_clipboard_read_text_async(clipboard, nullptr, on_clipboard_read,
                                  nullptr);
    return G_SOURCE_CONTINUE;
  }

  // Updater 0.2: the 4-hour throttle and manifest verification are
  // backend-owned, so the silent launch check is one cheap RPC the backend
  // may decline ("throttled"). Failures are silent by design — a resident
  // launcher must never nag. An available update surfaces in the dedicated
  // update window (update-flow parity with macOS/Windows).
  void start_silent_update_check() {
    if (api == nullptr || std::getenv("FULCRUM_ACCEPTANCE") != nullptr) {
      return;
    }
    auto* api_raw = api.get();
    std::thread([api_raw]() mutable {
      try {
        auto const check = api_raw->update_check(false).get();
        if (check.status != "available") {
          return;
        }
        auto* boxed = new std::pair<std::string, std::int64_t>(
            check.available_version.value_or("?"),
            check.size_bytes.value_or(0));
        g_idle_add([](gpointer user_data) -> int {
          std::unique_ptr<std::pair<std::string, std::int64_t>> info(
              static_cast<std::pair<std::string, std::int64_t>*>(user_data));
          update_window_present_available(info->first, info->second);
          return G_SOURCE_REMOVE;
        }, boxed);
      } catch (...) {
        // silent: network down, feed unreachable — never nag
      }
    }).detach();
  }

  static std::string event_payload(rivet::Value const& value) {
    // Event values are bare strings per the backend contract: RVT1 event
    // frames arrive as [name, value] and the runtime hands us value.
    if (auto const* text = std::get_if<std::string>(&value.data)) {
      return *text;
    }
    return {};
  }

  // Paste-to-frontmost: the clipboard already carries the text (set on the
  // main thread in handle_event); after the panel hides and focus returns
  // to the previous app, synthesize Ctrl+V — XTEST on X11, `wtype` on
  // Wayland. GDK is main-thread-only, so the X11 check happens on the main
  // thread and this worker touches only Xlib (XInitThreads ran at startup)
  // or spawns a process.
  static void paste_keystroke_async(bool x11_session) {
    std::thread([x11_session]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(180));
      if (x11_session) {
        Display* dpy = XOpenDisplay(nullptr);
        if (dpy != nullptr) {
          KeyCode ctrl = XKeysymToKeycode(dpy, XK_Control_L);
          KeyCode v = XKeysymToKeycode(dpy, XK_v);
          if (ctrl != 0 && v != 0) {
            XTestFakeKeyEvent(dpy, ctrl, True, CurrentTime);
            XTestFakeKeyEvent(dpy, v, True, CurrentTime);
            XTestFakeKeyEvent(dpy, v, False, CurrentTime);
            XTestFakeKeyEvent(dpy, ctrl, False, CurrentTime);
            XFlush(dpy);
          }
          XCloseDisplay(dpy);
          return;
        }
      }
      GError* error = nullptr;
      if (!g_spawn_command_line_async("wtype -M ctrl -P v -p v -m ctrl",
                                      &error)) {
        Launcher::post_status(
            "Paste-to-frontmost on Wayland needs `wtype` (install it), or "
            "run Fulcrum in an X11 session.");
        if (error != nullptr) {
          g_error_free(error);
        }
      }
    }).detach();
  }

  void handle_state_push(rivet::Value const& value) {
    // Reserved "$state" frames carry [state-name, wire-value].
    auto const* fields = std::get_if<rivet::Value::List>(&value.data);
    if (fields == nullptr || fields->size() != 2) {
      return;
    }
    auto const* state_name = std::get_if<std::string>(&(fields->at(0).data));
    if (state_name == nullptr) {
      return;
    }
    if (*state_name == "ai-stream") {
      rivet_app::AiStream stream;
      try {
        stream = rivet_app::detail::decode_AiStream(fields->at(1));
      } catch (...) {
        return;
      }
      g_idle_add([](gpointer user_data) -> int {
        std::unique_ptr<rivet_app::AiStream> stream(
            static_cast<rivet_app::AiStream*>(user_data));
        Launcher::instance().apply_ai_stream(*stream);
        return G_SOURCE_REMOVE;
      }, new rivet_app::AiStream(std::move(stream)));
    } else if (*state_name == "theme") {
      if (auto const* theme = std::get_if<std::string>(&(fields->at(1).data))) {
        std::string value_copy = *theme;
        g_idle_add([](gpointer user_data) -> int {
          std::unique_ptr<std::string> theme(
              static_cast<std::string*>(user_data));
          auto& launcher = Launcher::instance();
          ++launcher.theme_revision;
          apply_theme(*theme);
          return G_SOURCE_REMOVE;
        }, new std::string(std::move(value_copy)));
      }
    } else if (*state_name == "search-revision") {
      g_idle_add([](gpointer) -> int {
        Launcher::instance().refresh_for_revision();
        return G_SOURCE_REMOVE;
      }, nullptr);
    } else if (*state_name == "language") {
      if (auto const* language =
              std::get_if<std::string>(&(fields->at(1).data))) {
        std::string value_copy = *language;
        g_idle_add([](gpointer user_data) -> int {
          std::unique_ptr<std::string> language(
              static_cast<std::string*>(user_data));
          set_i18n_language(*language);
          return G_SOURCE_REMOVE;
        }, new std::string(std::move(value_copy)));
      }
    } else if (*state_name == "hotkey") {
      if (auto const* spec = std::get_if<std::string>(&(fields->at(1).data))) {
        std::string spec_copy = *spec;
        g_idle_add([](gpointer user_data) -> int {
          std::unique_ptr<std::string> spec(
              static_cast<std::string*>(user_data));
          ++Launcher::instance().hotkey_revision;
          std::string note = apply_x11_hotkey(*spec);
          if (!note.empty()) {
            Launcher::instance().set_status(note);
          }
          return G_SOURCE_REMOVE;
        }, new std::string(std::move(spec_copy)));
      }
    }
  }

  void handle_event(std::string const& name, std::string const& payload) {
    if (name == "copy-to-clipboard") {
      if (GdkClipboard* clipboard =
              gtk_widget_get_clipboard(GTK_WIDGET(window))) {
        gdk_clipboard_set_text(clipboard, payload.c_str());
      }
      hide();
    } else if (name == "paste-to-frontmost") {
      if (GdkClipboard* clipboard =
              gtk_widget_get_clipboard(GTK_WIDGET(window))) {
        gdk_clipboard_set_text(clipboard, payload.c_str());
      }
      hide();
      bool const x11_session =
          GDK_IS_X11_DISPLAY(gdk_display_get_default());
      paste_keystroke_async(x11_session);
    } else if (name == "ai-finished") {
      // The answer lands on the clipboard, but the panel stays: the user
      // is reading the streamed text right now.
      if (GdkClipboard* clipboard =
              gtk_widget_get_clipboard(GTK_WIDGET(window))) {
        gdk_clipboard_set_text(clipboard, payload.c_str());
      }
      set_status("Answer copied to the clipboard");
    } else if (name == "open-url") {
      gtk_show_uri(GTK_WINDOW(window), payload.c_str(), GDK_CURRENT_TIME);
      hide();
    } else if (name == "update-available") {
      show_update_window(true);
    } else if (name == "notify") {
      // Action feedback rides the notify event (update check results,
      // settings toggles, plugin installs). Without this branch the
      // status line kept the host's "Running…" placeholder forever.
      set_status(payload);
    }
  }

  // All request/response traffic runs on short-lived worker threads; the
  // callbacks only marshal (boxed) results back through g_idle_add.

  struct SearchOutcome {
    bool ok{false};
    int generation{0};
    std::vector<ResultRow> rows;
    std::string error;
    std::string selected_arg;
  };

  void run_search(std::string const& query, std::string selected_arg = {}) {
    if (api == nullptr || shutting_down.load(std::memory_order_acquire)) {
      return;
    }
    auto* api_raw = api.get();
    auto* boxed = new SearchOutcome;
    boxed->generation = search_generation.fetch_add(1) + 1;
    boxed->selected_arg = std::move(selected_arg);
    std::thread([api_raw, query, boxed]() mutable {
      try {
        for (auto const& cells : api_raw->search(query).get()) {
          boxed->rows.push_back(parse_row(cells));
        }
        boxed->ok = true;
      } catch (std::exception const& e) {
        boxed->error = e.what();
      }
      g_idle_add([](gpointer user_data) -> int {
        std::unique_ptr<SearchOutcome> job(
            static_cast<SearchOutcome*>(user_data));
        Launcher& launcher = Launcher::instance();
        if (job->generation == launcher.search_generation.load()) {
          launcher.apply_search(std::move(*job));
        }
        return G_SOURCE_REMOVE;
      }, boxed);
    }).detach();
  }

  void apply_search(SearchOutcome result) {
    if (!result.ok) {
      set_status("Search failed: " + result.error);
      finish_initial_presentation();
      return;
    }
    rows = std::move(result.rows);
    refresh_results(result.selected_arg);
    finish_initial_presentation();
  }

  void scroll_to_selection() {
    auto* selected = gtk_list_box_get_selected_row(results);
    auto* scrolled = gtk_widget_get_ancestor(GTK_WIDGET(results),
                                            GTK_TYPE_SCROLLED_WINDOW);
    graphene_rect_t bounds{};
    if (selected == nullptr || scrolled == nullptr ||
        !gtk_widget_compute_bounds(GTK_WIDGET(selected), scrolled,
                                    &bounds)) return;
    auto* adjustment = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(scrolled));
    double const offset = gtk_adjustment_get_value(adjustment);
    gtk_adjustment_clamp_page(adjustment, offset + bounds.origin.y,
                             offset + bounds.origin.y + bounds.size.height);
  }

  void refresh_results(std::string const& selected_arg = {},
                       std::string const& selected_id = "settings.set") {
    // Rebuild the list; simple and correct beats incremental cleverness at
    // this list size (≤ max-results rows).
    for (GtkWidget* child = gtk_widget_get_first_child(GTK_WIDGET(results));
         child != nullptr;) {
      GtkWidget* next = gtk_widget_get_next_sibling(child);
      gtk_list_box_remove(results, child);
      child = next;
    }
    for (auto const& row : rows) {
      // Row grammar matches the macOS/Windows hosts: amber icon tile,
      // title over muted subtitle, badge chip, and the ↵ chip that the
      // stylesheet shows only on the selected row.
      auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
      gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

      // Application entries get their real themed or file icon. Only
      // missing icons use the generic amber-tinted semantic fallback.
      GtkWidget* icon = application_icon(row);
      if (icon == nullptr) {
        if (char const* icon_name = row_icon_name(row)) {
          icon = gtk_image_new_from_icon_name(icon_name);
          gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
          gtk_widget_add_css_class(icon, "fulcrum-row-icon");
        }
      }
      if (icon != nullptr) {
        gtk_box_append(GTK_BOX(box), icon);
      }

      auto* texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
      gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
      gtk_widget_set_hexpand(texts, TRUE);
      std::string title_text = row.title;
      std::string subtitle_text = row.subtitle.empty() ? row.kind : row.subtitle;
      if (row.id == "settings.set") {
        std::string const key = "setting." + row.arg;
        if (tr(key.c_str()) != key) {
          title_text = tr(key.c_str()) + " (" + row.arg + ")";
          subtitle_text = tr((key + ".description").c_str());
        }
      }
      auto* title = gtk_label_new(title_text.c_str());
      gtk_label_set_xalign(GTK_LABEL(title), 0.0);
      gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
      gtk_widget_add_css_class(title, "fulcrum-row-title");
      apply_hit_attributes(GTK_LABEL(title), row.meta, 't', title_text);
      auto* subtitle = gtk_label_new(subtitle_text.c_str());
      gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
      gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
      gtk_widget_add_css_class(subtitle, "fulcrum-row-subtitle");
      gtk_widget_add_css_class(subtitle, "dim-label");
      apply_hit_attributes(GTK_LABEL(subtitle), row.meta, 's',
                           subtitle_text);
      gtk_box_append(GTK_BOX(texts), title);
      gtk_box_append(GTK_BOX(texts), subtitle);
      gtk_box_append(GTK_BOX(box), texts);

      if (meta_sensitive(row.meta)) {
        // Masked credential row (design v2 invariant 9): the raw text
        // never reached the wire; the badge names the mask.
        auto* sensitive = gtk_label_new(tr("sensitiveBadge").c_str());
        gtk_widget_add_css_class(sensitive, "fulcrum-badge");
        gtk_widget_set_valign(sensitive, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), sensitive);
      }

      if (!row.badge.empty() || row.id == "settings.set") {
        auto* badge = gtk_label_new(row.badge.empty() ? tr("notSet").c_str() : row.badge.c_str());
        gtk_widget_add_css_class(badge, "fulcrum-badge");
        gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), badge);
      }

      auto* enter = gtk_label_new("\u21B5");
      gtk_widget_add_css_class(enter, "fulcrum-enter");
      gtk_widget_set_valign(enter, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(box), enter);

      gtk_list_box_append(results, box);
    }
    if (!rows.empty()) {
      auto selected = std::find_if(rows.begin(), rows.end(), [&](auto const& row) {
        return row.id == selected_id && row.arg == selected_arg;
      });
      int const index = selected == rows.end() ? 0 :
          static_cast<int>(std::distance(rows.begin(), selected));
      gtk_list_box_select_row(results,
                              gtk_list_box_get_row_at_index(results, index));
    }
    update_panel_height();
  }

  // The launcher uses one fixed geometry; result and overlay updates
  // must not ask the window manager to resize the visible search panel.
  void update_panel_height() {
    // No-op: initial GTK window size is set once at creation.
  }

  struct RunOutcome {
    bool success{false};
    std::string status;
    std::string action_id;
    std::string arg;
  };

  void show_actions_for_selection() {
    if (api == nullptr || rows.empty() || actions_mode) {
      return;
    }
    GtkListBoxRow* selected = gtk_list_box_get_selected_row(results);
    int index = selected != nullptr ? gtk_list_box_row_get_index(selected) : 0;
    if (index < 0 || index >= static_cast<int>(rows.size())) {
      index = 0;
    }
    ResultRow const row = rows[static_cast<std::size_t>(index)];
    search_rows = rows;
    auto* api_raw = api.get();
    std::thread([api_raw, row]() mutable {
      std::vector<ResultRow> actions;
      std::string error;
      try {
        for (auto const& cells : api_raw->row_actions(row.id, row.arg).get()) {
          actions.push_back(parse_row(cells));
        }
      } catch (std::exception const& e) {
        error = e.what();
      }
      auto* job = new ActionsJob{std::move(actions), row.id, row.arg,
                                 row.title, row.badge};
      g_idle_add([](gpointer user_data) -> int {
        std::unique_ptr<ActionsJob> job(static_cast<ActionsJob*>(user_data));
        Launcher& launcher = Launcher::instance();
        launcher.actions_mode = true;
        launcher.rows = std::move(job->rows);
        launcher.actions_parent_id = job->parent_id;
        launcher.actions_parent_arg = job->parent_arg;
        launcher.refresh_results();
        gtk_label_set_text(launcher.navigation_hint,
                           tr("navigationHintActions").c_str());
        // Mode bar: the layer's exit pill and the row it acts on —
        // "which layer am I in" is visible, not read (design v2).
        gtk_widget_set_visible(launcher.actions_modebar, TRUE);
        gtk_widget_add_css_class(GTK_WIDGET(launcher.results), "actions-mode");
        gchar* escaped =
            g_markup_escape_text(job->parent_title.c_str(), -1);
        std::string markup = std::string(tr("actionsLabel")) + " <b>" +
                             escaped + "</b>";
        gtk_label_set_markup(launcher.actions_mode_title, markup.c_str());
        g_free(escaped);
        gtk_label_set_text(launcher.actions_mode_badge,
                           job->parent_badge.c_str());
        gtk_widget_set_visible(GTK_WIDGET(launcher.actions_mode_badge),
                               !job->parent_badge.empty());
        if (launcher.rows.empty()) {
          launcher.set_status(tr("noSecondaryActions"));
        }
        return G_SOURCE_REMOVE;
      }, job);
      (void)error;
    }).detach();
  }

  void close_actions() {
    // Esc restores the originating row by item id — never the first row;
    // a row filtered away falls back to the primary action (index 0).
    std::string restore_id = std::move(actions_parent_id);
    std::string restore_arg = std::move(actions_parent_arg);
    actions_parent_id.clear();
    actions_parent_arg.clear();
    actions_mode = false;
    rows = std::move(search_rows);
    search_rows.clear();
    refresh_results(restore_arg, restore_id);
    gtk_label_set_text(navigation_hint, tr("navigationHint").c_str());
    gtk_widget_set_visible(actions_modebar, FALSE);
    gtk_widget_remove_css_class(GTK_WIDGET(results), "actions-mode");
    gtk_widget_grab_focus(GTK_WIDGET(search));
  }

  // Ctrl+Y on a masked clipboard row: fetch the raw text through the
  // local-only reveal RPC and swap it into the on-screen row. Reveal
  // lives in host memory until the next result rebuild.
  void reveal_sensitive_selection() {
    if (api == nullptr || rows.empty() || actions_mode) return;
    GtkListBoxRow* selected = gtk_list_box_get_selected_row(results);
    int index = selected != nullptr ? gtk_list_box_row_get_index(selected) : 0;
    if (index < 0 || index >= static_cast<int>(rows.size())) return;
    ResultRow const row = rows[static_cast<std::size_t>(index)];
    if (!meta_sensitive(row.meta)) return;
    auto* api_raw = api.get();
    struct RevealJob {
      std::string id;
      std::string arg;
      std::string title;
      std::string raw;
    };
    std::thread([api_raw, row]() mutable {
      std::string raw;
      try {
        raw = api_raw->clipboard_reveal(row.arg).get();
      } catch (...) {
        // Reveal is best-effort: the masked row simply stays masked.
        return;
      }
      g_idle_add([](gpointer user_data) -> int {
        std::unique_ptr<RevealJob> job(static_cast<RevealJob*>(user_data));
        Launcher& launcher = Launcher::instance();
        std::replace(job->raw.begin(), job->raw.end(), '\n', ' ');
        if (job->raw.size() > 120) {
          job->raw = job->raw.substr(0, 120) + "\u2026";
        }
        for (auto& r : launcher.rows) {
          if (r.id == job->id && r.arg == job->arg && r.title == job->title) {
            r.title = job->raw;
            break;
          }
        }
        auto* selected = gtk_list_box_get_selected_row(launcher.results);
        int const selected_index =
            selected != nullptr ? gtk_list_box_row_get_index(selected) : 0;
        launcher.refresh_results();
        if (auto* kept = gtk_list_box_get_row_at_index(launcher.results,
                                                       selected_index)) {
          gtk_list_box_select_row(launcher.results, kept);
        }
        return G_SOURCE_REMOVE;
      }, new RevealJob{row.id, row.arg, row.title, std::move(raw)});
    }).detach();
  }

  void run_selected() {
    if (api == nullptr || rows.empty()) {
      return;
    }
    GtkListBoxRow* selected = gtk_list_box_get_selected_row(results);
    int index = selected != nullptr ? gtk_list_box_row_get_index(selected) : 0;
    if (index < 0 || index >= static_cast<int>(rows.size())) {
      index = 0;
    }
    ResultRow const row = rows[static_cast<std::size_t>(index)];
    // Update rows open the dedicated update window (update-flow parity
    // with macOS/Windows); the transfer and install live there, not in
    // the launcher status line.
    if (row.id == "update.check" || row.id == "update.download" ||
        row.id == "update.install") {
      hide();
      show_update_window(true);
      return;
    }
    if (row.id == "settings.set") ++host_settings_generation;
    set_status("Running…");
    auto* api_raw = api.get();
    // Fast typing outruns async result repaints: the displayed rows can be
    // one generation behind the query box, so a `!g vflash 刷写` entered
    // mid-repaint would open the search for an earlier tail. Re-run the
    // final query first, then act on the freshest row for this selection —
    // same id+title falls through to the new top row.
    gchar const* query_chars =
        gtk_editable_get_text(GTK_EDITABLE(search));
    std::string const query_text =
        query_chars != nullptr ? std::string(query_chars) : std::string();
    std::thread([api_raw, row, query_text]() mutable {
      auto* boxed = new RunOutcome;
      auto run_row = row;
      if (!query_text.empty()) {
        auto fresh = api_raw->search(query_text).get();
        for (auto const& cells : fresh) {
          ResultRow const candidate = parse_row(cells);
          if (candidate.id == row.id && candidate.title == row.title) {
            run_row = candidate;
            break;
          }
        }
      }
      boxed->action_id = run_row.id;
      boxed->arg = run_row.arg;
      try {
        boxed->status = api_raw->run_action(run_row.id, run_row.arg).get();
        boxed->success = boxed->status == "ok" ||
                         boxed->status == "launched" ||
                         boxed->status == "copied" ||
                         boxed->status == "opened" ||
                         boxed->status == "started" ||
                         boxed->status == "detail" ||
                         boxed->status == "stay" ||
                         boxed->status == "delegated";
      } catch (std::exception const& e) {
        boxed->status = e.what();
      }
      g_idle_add([](gpointer user_data) -> int {
        std::unique_ptr<RunOutcome> job(static_cast<RunOutcome*>(user_data));
        if (job->success) {
          // About rides "delegated": the backend ranks the row, the host
          // renders its native about dialog (main thread: GTK owns the
          // widgets here).
          if (job->action_id == "about.open") {
            show_about_dialog(Launcher::instance().window);
            return G_SOURCE_REMOVE;
          }
          // The settings window rides "delegated" the same way.
          if (job->action_id == "settings.open") {
            Launcher::instance().hide();
            show_settings_window();
            return G_SOURCE_REMOVE;
          }
          // Window commands ride "delegated": the backend ranks the rows,
          // the host performs the native window move on the frontmost
          // X11 window (main thread: GDK owns the display here).
          if (job->action_id.rfind("win.", 0) == 0) {
            if (!wincmd::run_window_command(job->action_id)) {
              Launcher::instance().set_status(
                  "Window command failed (X11/Wayland window manager refused).");
              return G_SOURCE_REMOVE;
            }
          }
          if (job->status == "started") {
            // AI run: the request continues on a backend thread; the
            // ai-stream State drives the streaming panel.
            Launcher::instance().set_status("AI is answering…");
            return G_SOURCE_REMOVE;
          }
          if (job->status == "stay") {
            // The result is meant to be read (update check, ...): the
            // notify event already wrote the status line (selectable, so
            // errors can be copied); keep the panel open.
            return G_SOURCE_REMOVE;
          }
          if (job->action_id == "settings.set") {
            auto& launcher = Launcher::instance();
            launcher.set_status(tr("settingUpdated"));
            launcher.pull_host_settings();
            launcher.run_search(
                gtk_editable_get_text(GTK_EDITABLE(launcher.search)), job->arg);
            return G_SOURCE_REMOVE;
          }
          if (job->status == "detail") {
            // FPP2 plugin detail: fetch the payload and show it in the
            // panel slot. The RPC runs on a worker thread; the paint
            // marshals back through g_idle_add.
            Launcher::instance().dismiss_detail_panel();
            std::string const row_id = job->action_id;
            auto* api_raw = Launcher::instance().api.get();
            std::thread([api_raw, row_id, arg = job->arg]() mutable {
              try {
                auto view = api_raw->plugin_detail(row_id, arg).get();
                g_idle_add([](gpointer user_data) -> int {
                  std::unique_ptr<rivet_app::DetailView> view(
                      static_cast<rivet_app::DetailView*>(user_data));
                  Launcher::instance().apply_plugin_detail(*view);
                  return G_SOURCE_REMOVE;
                }, new rivet_app::DetailView(std::move(view)));
              } catch (std::exception const& e) {
                Launcher::post_status(std::string("Detail error: ") + e.what());
              }
            }).detach();
            return G_SOURCE_REMOVE;
          }
          if (Launcher::instance().actions_mode) {
            // A mutating action (pin, delete, copy) keeps the launcher
            // open, back on the search rows.
            Launcher::instance().close_actions();
          } else {
            Launcher::instance().hide();
          }
        } else {
          Launcher::instance().set_status("Action failed: " + job->status);
        }
        return G_SOURCE_REMOVE;
      }, boxed);
    }).detach();
  }

  // Complete the startup frame gate at most once. The backend may finish
  // after a user summon or during a quiet login item; only the former opens.
  void finish_initial_presentation() {
    if (initial_results_ready) return;
    initial_results_ready = true;
    if (summon_when_ready) {
      summon_when_ready = false;
      show(GDK_CURRENT_TIME, false);
    }
  }

  void show(guint32 user_time = GDK_CURRENT_TIME,
            bool start_search = false) {
    if (!initial_results_ready) {
      summon_when_ready = true;
      return;
    }
    cancel_focus_loss();
    activated_since_show = false;
    if (start_search) {
      // Explicit fresh-search resets keep today's behavior: actions mode
      // closes, panels dismiss, the query wipes to the default rows.
      actions_mode = false;
      if (navigation_hint != nullptr) {
        gtk_label_set_text(navigation_hint, tr("navigationHint").c_str());
      }
      dismiss_ai_panel();
      dismiss_detail_panel();
      gtk_editable_set_text(GTK_EDITABLE(search), "");
      run_search("");
    }
    // Plain summoning preserves the query and results: hide/show cycles
    // are navigation, not a new search. Esc clears the query explicitly
    // (clear first, hide when already empty).
    // Position before mapping to avoid showing the launcher on the wrong
    // monitor for a frame. Wayland compositor positioning remains unchanged.
    gtk_widget_realize(GTK_WIDGET(window));
    place_panel_on_pointer_monitor(window);
    gtk_widget_set_visible(GTK_WIDGET(window), TRUE);
    auto* display = gtk_widget_get_display(GTK_WIDGET(window));
    if (GDK_IS_X11_DISPLAY(display)) {
      guint32 const present_time =
          user_time != GDK_CURRENT_TIME
              ? user_time
              : gdk_x11_display_get_user_time(display);
      gtk_window_present_with_time(window, present_time);
    } else {
      gtk_window_present(window);
    }
    // X11 can grant focus synchronously; Wayland repeats this when the
    // is-active notification arrives below.
    gtk_widget_grab_focus(GTK_WIDGET(search));
    // A hidden window can remain the compositor's active toplevel. In that
    // case showing it again produces no is-active transition, so seed the
    // focus-loss guard from the state immediately after present.
    activated_since_show = gtk_window_is_active(window);
  }

  void cancel_focus_loss() {
    if (focus_loss_source != 0) {
      g_source_remove(focus_loss_source);
      focus_loss_source = 0;
    }
  }

  void hide() {
    cancel_focus_loss();
    summon_when_ready = false;
    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
  }

  void toggle(guint32 user_time = GDK_CURRENT_TIME) {
    if (gtk_widget_get_visible(GTK_WIDGET(window))) {
      hide();
    } else {
      show(user_time);
    }
  }

  // ---- backend lifecycle -------------------------------------------------

  void start_backend() {
    // raco rivet build/dev stages runtime/*.boot and res/core.zo beside the
    // executable; that is the only layout the official CLI produces.
    std::filesystem::path const root = executable_path().parent_path();
    std::filesystem::path const petite = root / "runtime" / "petite.boot";
    std::filesystem::path const scheme = root / "runtime" / "scheme.boot";
    std::filesystem::path const racket_boot = root / "runtime" / "racket.boot";
    std::filesystem::path const core = root / "res" / "core.zo";
    if (!std::filesystem::exists(petite) ||
        !std::filesystem::exists(scheme) ||
        !std::filesystem::exists(racket_boot) ||
        !std::filesystem::exists(core)) {
      set_status("Missing Rivet runtime layout (runtime/*.boot, res/core.zo) "
                 "next to the executable. Build with raco rivet build/dev.");
      finish_initial_presentation();
      return;
    }

    rivet::linux_runtime::RacketRuntimeConfig config;
    config.executable_path = executable_path().string();
    config.petite_boot = petite.string();
    config.scheme_boot = scheme.string();
    config.racket_boot = racket_boot.string();
    config.backend_bundle = core.string();
    config.module_name = rivet_app::kModuleName;
    config.entry_symbol = rivet_app::kEntryName;

    startup_thread = std::thread([config = std::move(config)]() mutable {
      auto backend =
          std::make_unique<rivet::linux_runtime::Backend>(std::move(config));
      try {
        backend->start();
        std::lock_guard lock(Launcher::instance().startup_mutex);
        Launcher::instance().startup_backend = std::move(backend);
      } catch (std::exception const& e) {
        std::lock_guard lock(Launcher::instance().startup_mutex);
        Launcher::instance().startup_error = e.what();
      }
      g_idle_add([](gpointer) -> int {
        Launcher::instance().on_backend_ready();
        return G_SOURCE_REMOVE;
      }, nullptr);
    });
  }

  void on_backend_ready() {
    // The embedded Racket runtime installs its own SIGTERM handler while
    // booting and would leave an earlier hook inert — the runtime's
    // handler is the last writer (the same lesson the SyncPilot host
    // logged with a sigaction interposer). Installing here, once the boot
    // thread has finished, on the main loop thread, is the last word:
    // SIGTERM/SIGINT now run the orderly stop and exit instead of being
    // absorbed into a half-dead process. Before this point a signal hits
    // the default disposition and simply kills the process — nothing
    // worth tearing down exists yet.
    install_shutdown_hook_once();

    std::lock_guard stop_lock(stop_mutex);
    if (startup_thread.joinable()) {
      startup_thread.join();
    }

    std::unique_ptr<rivet::linux_runtime::Backend> backend;
    std::string error;
    {
      std::lock_guard lock(startup_mutex);
      backend = std::move(startup_backend);
      error = std::move(startup_error);
    }

    if (shutting_down.load(std::memory_order_acquire)) {
      if (backend != nullptr) {
        backend->stop();
      }
      return;
    }
    if (!error.empty()) {
      set_status("Fulcrum backend failed to start: " + error);
      finish_initial_presentation();
      return;
    }
    if (backend == nullptr) {
      set_status("Fulcrum backend error: startup completed without a backend");
      finish_initial_presentation();
      return;
    }

    backend_ = std::move(backend);
    api = std::make_unique<rivet_app::API>(*backend_);

    backend_->set_event_handler(
        [](std::string const& name, rivet::Value const& value) {
          if (name == "$state") {
            // Reserved state pushes need the raw list payload; decode on
            // the main loop (handle_state_push marshals internally).
            auto* job = new std::pair<std::string, rivet::Value>(
                name, value);
            g_idle_add([](gpointer user_data) -> int {
              std::unique_ptr<std::pair<std::string, rivet::Value>> frame(
                  static_cast<std::pair<std::string, rivet::Value>*>(
                      user_data));
              Launcher::instance().handle_state_push(frame->second);
              return G_SOURCE_REMOVE;
            }, job);
            return;
          }
          auto* job = new std::pair<std::string, std::string>(
              name, event_payload(value));
          g_idle_add([](gpointer user_data) -> int {
            std::unique_ptr<std::pair<std::string, std::string>> payload(
                static_cast<std::pair<std::string, std::string>*>(user_data));
            Launcher::instance().handle_event(payload->first, payload->second);
            return G_SOURCE_REMOVE;
          }, job);
        });

    set_ready_status();
    run_search("");
    start_silent_update_check();
    pull_host_settings();
    g_timeout_add(1000, poll_clipboard, nullptr);
  }

  void stop_backend() {
    std::lock_guard lock(stop_mutex);
    shutting_down.store(true, std::memory_order_release);
    if (startup_thread.joinable()) {
      startup_thread.join();
    }
    if (backend_ != nullptr && !backend_stopped_) {
      backend_stopped_ = true;
      backend_->stop();
    }
  }

 private:
  std::unique_ptr<rivet::linux_runtime::Backend> backend_;
};

void set_i18n_language(std::string const& lang) {
  // "system" follows the OS locale: LC_ALL/LANG (XDG sessions always set
  // one); a zh prefix selects Chinese, anything else falls back to English.
  std::string effective = lang;
  if (effective == "system") {
    char const* locale = getenv("LC_ALL");
    if (locale == nullptr || *locale == '\0') {
      locale = getenv("LANG");
    }
    effective = (locale != nullptr && std::string(locale).rfind("zh", 0) == 0)
                    ? "zh"
                    : "en";
  }
  int const active = (effective == "en") ? 1 : 0;
  bool const changed = g_i18n_active.exchange(active) != active;
  auto& launcher = Launcher::instance();
  if (launcher.search != nullptr) {
    gtk_search_entry_set_placeholder_text(launcher.search,
                                          tr("searchPlaceholder").c_str());
  }
  if (launcher.navigation_hint != nullptr) {
    gtk_label_set_text(launcher.navigation_hint, tr("navigationHint").c_str());
  }
  if (launcher.hotkey_fix != nullptr) {
    gtk_button_set_label(launcher.hotkey_fix, tr("trayFixHotkey").c_str());
  }
  if (changed && launcher.results != nullptr) {
    auto* selected = gtk_list_box_get_selected_row(launcher.results);
    int index = selected != nullptr ? gtk_list_box_row_get_index(selected) : -1;
    std::string const arg = index >= 0 && index < static_cast<int>(launcher.rows.size())
        ? launcher.rows[static_cast<std::size_t>(index)].arg : "";
    std::string const id = index >= 0 && index < static_cast<int>(launcher.rows.size())
        ? launcher.rows[static_cast<std::size_t>(index)].id : "";
    launcher.refresh_results(arg, id);
  }
  if (changed && launcher.api != nullptr) launcher.set_ready_status();
  apply_tray_language();
  refresh_settings_language();
  update_window_refresh_language();
}

// ---- settings window ---------------------------------------------------------
//
// A form over the backend settings store — the discoverable twin of the
// `settings` query rows, reached from the tray menu and the leading
// settings row. Every control carries its backend key; edits go through
// settings-set as they happen, the backend validates (hotkey grammar,
// ranges, provider names), a refusal shows inline and the form reloads the
// backend's truth. The AI key rides the ai.key action and never comes back
// over the wire; ai-status reports readiness instead. One window, hidden on
// close and reloaded on every show.

enum class SettingKind { toggle, choice, number, text };

// Settings v2 (design/DESIGN.md "偏好设置 v2 宿主契约"): the backend's
// `settings-ui` rows after parsing. A page holds its sections in display
// order; a row names its control and, for choices, the backend's option
// values (empty = the "Off" provider) and for numbers the validator's
// inclusive (min max).
struct SettingsSchemaRow {
  std::string key;
  std::string section;
  std::string control;
  std::vector<std::string> options;
};

struct SettingsSchemaSection {
  std::string id;
  std::vector<SettingsSchemaRow> rows;
};

struct SettingsSchemaPage {
  std::string id;
  std::string group;
  std::vector<SettingsSchemaSection> sections;
};

struct SettingBinding {
  std::string key;
  SettingKind kind{SettingKind::text};
  std::vector<std::string> choices;  // choice: drop-down index → value
  // choice: per-option fallback label and optional shared/i18n key, so a
  // language change can repaint the drop-down without losing the selection.
  std::vector<std::string> choice_labels;
  std::vector<std::string> choice_keys;
  GtkWidget* widget{nullptr};
};

struct SettingsForm {
  GtkWindow* window{nullptr};
  GtkWidget* stack{nullptr};
  GtkWidget* commands_list{nullptr};
  GtkWidget* commands_message{nullptr};
  GtkWidget* workflow_list{nullptr};
  GtkWidget* workflow_steps{nullptr};
  GtkWidget* workflow_trace{nullptr};
  GtkWidget* workflow_name{nullptr};
  GtkWidget* workflow_step{nullptr};
  GtkWidget* workflow_input{nullptr};
  GtkWidget* workflow_selected_label{nullptr};
  GtkWidget* workflow_status{nullptr};
  GtkWidget* workflow_delete_confirm{nullptr};
  GtkWidget* workflow_run_copy{nullptr};
  std::string workflow_selected;
  int workflow_list_page{1};
  int workflow_step_page{1};
  int workflow_trace_page{1};
  bool workflow_copy_safe{false};
  std::vector<std::unique_ptr<SettingBinding>> bindings;
  // Keep backend-defined localized labels alive across language changes.
  std::vector<std::tuple<GtkLabel*, std::string, std::string>> localized_labels;
  std::vector<std::tuple<GtkButton*, std::string>> localized_buttons;
  std::vector<std::tuple<GtkEntry*, std::string>> localized_placeholders;
  // Stack page id → settings.nav.* key (sidebar titles follow the language).
  std::vector<std::pair<std::string, std::string>> localized_pages;
  GtkLabel* ai_status{nullptr};
  GtkLabel* key_notice{nullptr};
  GtkWidget* key_entry{nullptr};
  GtkLabel* error{nullptr};
  GtkLabel* notice{nullptr};
  std::map<std::string, std::string> values;
  // True while apply pushes values into the controls, so their change
  // handlers do not write them straight back.
  bool loading{false};
  // ---- settings v2 -------------------------------------------------------
  // The schema pages are rebuilt on every language change, so what they
  // register lives apart from the static Commands/Workflows pages above.
  std::vector<SettingsSchemaPage> schema;
  bool schema_scope{false};
  bool dynamic_scope{false};
  std::vector<std::tuple<GtkLabel*, std::string, std::string>> schema_labels;
  std::vector<std::tuple<GtkButton*, std::string>> schema_buttons;
  std::vector<std::tuple<GtkEntry*, std::string>> schema_placeholders;
  std::vector<GtkWidget*> nav_headings;
  std::string current_page{"general"};
  // Last non-search page, restored when the search box empties.
  std::string last_page{"general"};
  std::map<std::string, GtkWidget*> nav_buttons;
  // Buttons of the re-rendered list rows; dropped on every list render.
  std::vector<std::tuple<GtkButton*, std::string>> dynamic_buttons;
  GtkWidget* nav_box{nullptr};
  GtkWidget* search_title{nullptr};
  GtkWidget* search_empty{nullptr};
  bool nav_sync{false};
  // Row key → the row box (search jump highlight) and its refusal label.
  std::map<std::string, GtkWidget*> row_widgets;
  std::map<std::string, GtkWidget*> row_hints;
  std::map<std::string, GtkLabel*> row_errors;
  // Extras: kind → the box its entries render into.
  std::map<std::string, GtkWidget*> list_hosts;
  GtkWidget* search{nullptr};
  bool search_silent{false};
  GtkWidget* search_page{nullptr};
  GtkWidget* search_results{nullptr};
  std::string highlighted_key;
  std::string ai_status_raw;
};

SettingsForm* g_settings_form = nullptr;

void settings_load();
void settings_load_commands();
void settings_load_lists();
void settings_render_search();
void settings_build_schema();
void workflow_load_list();
void workflow_load_detail();
void workflow_action(std::string id, std::string arg);

// A key the shared table has text for (tr returns the key itself otherwise).
bool settings_has_i18n(std::string const& key) { return tr(key.c_str()) != key; }

void settings_show_error(std::string const& message) {
  if (g_settings_form == nullptr) return;
  gtk_label_set_text(g_settings_form->error, message.c_str());
  gtk_widget_set_visible(GTK_WIDGET(g_settings_form->error), !message.empty());
}

// Neutral footer line (what an export or clear did).
void settings_show_notice(std::string const& message) {
  if (g_settings_form == nullptr || g_settings_form->notice == nullptr) return;
  gtk_label_set_text(g_settings_form->notice, message.c_str());
  gtk_widget_set_visible(GTK_WIDGET(g_settings_form->notice), !message.empty());
}

// A refusal belongs next to its row, not in the footer.
void settings_show_row_error(std::string const& key, std::string const& message) {
  if (g_settings_form == nullptr) return;
  auto& form = *g_settings_form;
  auto it = form.row_errors.find(key);
  if (it == form.row_errors.end()) {
    if (message.empty()) return;
    auto row = form.row_widgets.find(key);
    if (row == form.row_widgets.end()) { settings_show_error(message); return; }
    auto* parent = GTK_BOX(gtk_widget_get_parent(row->second));
    auto* label = gtk_label_new(message.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_widget_add_css_class(label, "error");
    gtk_widget_set_margin_start(label, 14);
    gtk_widget_set_margin_end(label, 14);
    gtk_widget_set_margin_bottom(label, 8);
    // Under the row's hint when it has one, else under the row itself.
    GtkWidget* after = row->second;
    if (auto hint = form.row_hints.find(key); hint != form.row_hints.end()) {
      after = hint->second;
    }
    gtk_box_insert_child_after(parent, label, after);
    form.row_errors[key] = GTK_LABEL(label);
  } else if (message.empty()) {
    gtk_label_set_text(it->second, "");
  } else {
    gtk_label_set_text(it->second, message.c_str());
  }
}

std::string settings_ai_status_text(std::string const& raw) {
  // The backend reports one of three states; the two refusals carry a
  // reason, not a localized sentence.
  if (raw == "ready") return tr("settings.ai-ready");
  if (raw == "no provider configured") return tr("settings.ai-status.no-provider");
  std::string const prefix = "no API key for ";
  if (raw.rfind(prefix, 0) == 0) {
    return tr("settings.ai-status.no-key") + " " + raw.substr(prefix.size());
  }
  return raw;
}

void settings_refresh_ai_status() {
  auto* api_raw = Launcher::instance().api.get();
  if (api_raw == nullptr) return;
  std::thread([api_raw]() {
    std::string status;
    try {
      status = api_raw->ai_status().get();
    } catch (...) {
    }
    g_idle_add([](gpointer user_data) -> int {
      std::unique_ptr<std::string> text(static_cast<std::string*>(user_data));
      if (g_settings_form != nullptr && g_settings_form->ai_status != nullptr) {
        g_settings_form->ai_status_raw = *text;
        gtk_label_set_text(g_settings_form->ai_status,
                           settings_ai_status_text(*text).c_str());
      }
      return G_SOURCE_REMOVE;
    }, new std::string(std::move(status)));
  }).detach();
}

void settings_write(std::string const& key, std::string const& value) {
  if (g_settings_form == nullptr || key.empty()) return;
  // A form edit supersedes any host snapshot requested before this write.
  auto& launcher = Launcher::instance();
  ++launcher.host_settings_generation;
  auto* api_raw = launcher.api.get();
  if (api_raw == nullptr) return;
  g_settings_form->values[key] = value;
  std::thread([api_raw, key, value]() {
    std::string error;
    try {
      if (!api_raw->settings_set(key, value).get()) {
        error = key == "hotkey"
            ? tr("settings.hotkey.unsupported")
            : tr_format("settings.value.unsupported", {key});
      }
    } catch (std::exception const& e) {
      error = e.what();
    } catch (...) {
      error = tr_format("settings.write.failed", {key});
    }
    g_idle_add([](gpointer user_data) -> int {
      std::unique_ptr<std::pair<std::string, std::string>> result(
          static_cast<std::pair<std::string, std::string>*>(user_data));
      auto const& [written_key, written_error] = *result;
      if (g_settings_form == nullptr) return G_SOURCE_REMOVE;
      if (written_error.empty()) {
        settings_show_error("");
        settings_show_row_error(written_key, "");
        if (written_key == "ai-provider") {
          settings_refresh_ai_status();
        } else if (written_key == "language") {
          set_i18n_language(g_settings_form->values[written_key]);
          apply_tray_language();
        }
      } else {
        // The backend refused the value (grammar, range): say so under the
        // row and fall back to what it actually holds.
        settings_show_row_error(written_key, written_error);
        settings_load();
      }
      return G_SOURCE_REMOVE;
    }, new std::pair<std::string, std::string>(key, std::move(error)));
  }).detach();
}

std::string settings_trimmed(std::string value) {
  auto const begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  auto const end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

void settings_commit_entry(SettingBinding* binding) {
  if (g_settings_form == nullptr || g_settings_form->loading) return;
  std::string const value = settings_trimmed(
      gtk_editable_get_text(GTK_EDITABLE(binding->widget)));
  auto const it = g_settings_form->values.find(binding->key);
  if (it != g_settings_form->values.end() && it->second == value) return;
  settings_write(binding->key, value);
}

void on_setting_switch(GObject* object, GParamSpec*, gpointer user_data) {
  if (g_settings_form == nullptr || g_settings_form->loading) return;
  auto* binding = static_cast<SettingBinding*>(user_data);
  settings_write(binding->key,
                 gtk_switch_get_active(GTK_SWITCH(object)) ? "on" : "off");
}

void on_setting_choice(GObject* object, GParamSpec*, gpointer user_data) {
  if (g_settings_form == nullptr || g_settings_form->loading) return;
  auto* binding = static_cast<SettingBinding*>(user_data);
  guint const index = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
  if (index == GTK_INVALID_LIST_POSITION || index >= binding->choices.size()) return;
  settings_write(binding->key, binding->choices[index]);
}

void on_setting_number(GtkSpinButton* spin, gpointer user_data) {
  if (g_settings_form == nullptr || g_settings_form->loading) return;
  auto* binding = static_cast<SettingBinding*>(user_data);
  settings_write(binding->key,
                 std::to_string(gtk_spin_button_get_value_as_int(spin)));
}

void on_setting_entry_activate(GtkEntry*, gpointer user_data) {
  settings_commit_entry(static_cast<SettingBinding*>(user_data));
}

void on_setting_entry_leave(GtkEventControllerFocus*, gpointer user_data) {
  settings_commit_entry(static_cast<SettingBinding*>(user_data));
}

void settings_save_key() {
  if (g_settings_form == nullptr) return;
  auto* api_raw = Launcher::instance().api.get();
  if (api_raw == nullptr) return;
  std::string const key = settings_trimmed(
      gtk_editable_get_text(GTK_EDITABLE(g_settings_form->key_entry)));
  if (key.empty()) return;
  std::thread([api_raw, key]() {
    std::string status;
    try {
      status = api_raw->run_action("ai.key", key).get();
    } catch (std::exception const& e) {
      status = e.what();
    } catch (...) {
      status = "ai.key failed";
    }
    g_idle_add([](gpointer user_data) -> int {
      std::unique_ptr<std::string> text(static_cast<std::string*>(user_data));
      if (g_settings_form != nullptr) {
        gtk_label_set_text(
            g_settings_form->key_notice,
            *text == "ok" ? tr("settings.ai-key.saved").c_str()
                          : tr("settings.ai-key.failed").c_str());
        gtk_editable_set_text(GTK_EDITABLE(g_settings_form->key_entry), "");
        settings_refresh_ai_status();
      }
      return G_SOURCE_REMOVE;
    }, new std::string(std::move(status)));
  }).detach();
}

void on_save_key_clicked(GtkButton*, gpointer) { settings_save_key(); }
void on_key_entry_activate(GtkPasswordEntry*, gpointer) { settings_save_key(); }

// ---- form construction

GtkWidget* settings_row(GtkWidget* section, char const* label_text,
                        GtkWidget* control, char const* setting_key = nullptr,
                        char const* label_key = nullptr) {
  auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(row, 8);
  gtk_widget_set_margin_bottom(row, 8);
  gtk_widget_set_margin_start(row, 14);
  gtk_widget_set_margin_end(row, 14);
  // Canonical label key: an explicit settings.* key wins; otherwise the
  // launcher's setting.<backend-key> row label is reused when it exists.
  std::string const lookup = label_key != nullptr
      ? std::string(label_key)
      : (setting_key == nullptr ? "" : "setting." + std::string(setting_key));
  std::string const translated = lookup.empty() ? "" : tr(lookup.c_str());
  auto* label = gtk_label_new(!lookup.empty() && translated != lookup
                                  ? translated.c_str() : label_text);
  if (!lookup.empty()) {
    auto& labels = g_settings_form->schema_scope
        ? g_settings_form->schema_labels : g_settings_form->localized_labels;
    labels.emplace_back(GTK_LABEL(label), lookup, label_text);
  }
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(control, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(row), label);
  gtk_box_append(GTK_BOX(row), control);
  gtk_box_append(GTK_BOX(section), row);
  return row;
}

void settings_hint(GtkWidget* section, char const* key) {
  std::string const text = tr(key);
  auto* label = gtk_label_new(text.c_str());
  auto& labels = g_settings_form->schema_scope
      ? g_settings_form->schema_labels : g_settings_form->localized_labels;
  labels.emplace_back(GTK_LABEL(label), key, "");
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_widget_add_css_class(label, "caption");
  gtk_widget_add_css_class(label, "dim-label");
  gtk_widget_set_margin_start(label, 14);
  gtk_widget_set_margin_end(label, 14);
  gtk_widget_set_margin_bottom(label, 8);
  gtk_box_append(GTK_BOX(section), label);
  // The row's hint, so its refusal lands under the hint, not between.
  std::string const hint_key = key;
  std::string const prefix = "settings.hint.";
  if (g_settings_form->schema_scope && hint_key.rfind(prefix, 0) == 0) {
    g_settings_form->row_hints[hint_key.substr(prefix.size())] = label;
  }
}

GtkWidget* settings_section(GtkWidget* page, char const* title_key) {
  if (title_key != nullptr && *title_key != '\0') {
    auto* heading = gtk_label_new(tr(title_key).c_str());
    auto& labels = g_settings_form->schema_scope
        ? g_settings_form->schema_labels : g_settings_form->localized_labels;
    labels.emplace_back(GTK_LABEL(heading), title_key, "");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
    gtk_widget_add_css_class(heading, "heading");
    gtk_widget_set_margin_top(heading, 14);
    gtk_box_append(GTK_BOX(page), heading);
  } else {
    auto* gap = gtk_label_new("");
    gtk_widget_set_margin_top(gap, 6);
    gtk_box_append(GTK_BOX(page), gap);
  }
  auto* frame = gtk_frame_new(nullptr);
  auto* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_frame_set_child(GTK_FRAME(frame), body);
  gtk_box_append(GTK_BOX(page), frame);
  return body;
}

// A button whose label follows the shared/i18n language live.
GtkButton* settings_button(char const* key) {
  auto* button = gtk_button_new_with_label(tr(key).c_str());
  std::vector<std::tuple<GtkButton*, std::string>>* buttons = &g_settings_form->localized_buttons;
  if (g_settings_form->dynamic_scope) {
    buttons = &g_settings_form->dynamic_buttons;
  } else if (g_settings_form->schema_scope) {
    buttons = &g_settings_form->schema_buttons;
  }
  buttons->emplace_back(GTK_BUTTON(button), key);
  return GTK_BUTTON(button);
}

// Runs `fn` when the button is clicked; the callable lives with the button.
void settings_on_clicked(GtkWidget* button, std::function<void()> fn) {
  g_object_set_data_full(G_OBJECT(button), "settings-fn",
                         new std::function<void()>(std::move(fn)),
                         [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
  g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* clicked, gpointer) {
    auto* fn = static_cast<std::function<void()>*>(
        g_object_get_data(G_OBJECT(clicked), "settings-fn"));
    if (fn) (*fn)();
  }), nullptr);
}

SettingBinding* settings_bind(std::string key, SettingKind kind,
                              GtkWidget* widget,
                              std::vector<std::string> choices = {}) {
  auto binding = std::make_unique<SettingBinding>();
  binding->key = std::move(key);
  binding->kind = kind;
  binding->widget = widget;
  binding->choices = std::move(choices);
  auto* raw = binding.get();
  g_settings_form->bindings.push_back(std::move(binding));
  return raw;
}

GtkWidget* settings_add_toggle(GtkWidget* section, char const* label,
                               char const* key, char const* label_key = nullptr) {
  auto* toggle = gtk_switch_new();
  auto* binding = settings_bind(key, SettingKind::toggle, toggle);
  g_signal_connect(toggle, "notify::active", G_CALLBACK(on_setting_switch), binding);
  settings_row(section, label, toggle, key, label_key);
  return toggle;
}

void settings_add_choice(
    GtkWidget* section, char const* label, char const* key,
    std::vector<std::tuple<char const*, char const*, char const*>> const& options,
    char const* label_key = nullptr) {
  std::vector<std::string> values;
  std::vector<std::string> fallbacks;
  std::vector<std::string> keys;
  std::vector<std::string> display;
  display.reserve(options.size());
  for (auto const& [text, value, option_key] : options) {
    display.push_back(option_key != nullptr && *option_key != '\0'
                          ? tr(option_key) : std::string(text));
    values.push_back(value);
    fallbacks.push_back(text);
    keys.push_back(option_key == nullptr ? "" : option_key);
  }
  std::vector<char const*> labels;
  labels.reserve(display.size() + 1);
  for (auto const& text : display) labels.push_back(text.c_str());
  labels.push_back(nullptr);
  auto* drop = gtk_drop_down_new_from_strings(labels.data());
  auto* binding = settings_bind(key, SettingKind::choice, drop, std::move(values));
  binding->choice_labels = std::move(fallbacks);
  binding->choice_keys = std::move(keys);
  g_signal_connect(drop, "notify::selected", G_CALLBACK(on_setting_choice), binding);
  settings_row(section, label, drop, key, label_key);
}

void settings_add_number(GtkWidget* section, char const* label, char const* key,
                         double min, double max, char const* label_key = nullptr) {
  auto* spin = gtk_spin_button_new_with_range(min, max, 1);
  auto* binding = settings_bind(key, SettingKind::number, spin);
  g_signal_connect(spin, "value-changed", G_CALLBACK(on_setting_number), binding);
  settings_row(section, label, spin, key, label_key);
}

std::string linux_key_spec(guint keyval, GdkModifierType modifiers) {
  std::string result;
  if ((modifiers & GDK_CONTROL_MASK) != 0) result += "ctrl+";
  if ((modifiers & GDK_ALT_MASK) != 0) result += "alt+";
  if ((modifiers & GDK_SHIFT_MASK) != 0) result += "shift+";
  if ((modifiers & GDK_SUPER_MASK) != 0) result += "super+";
  if (result.empty()) return {};
  guint const key = gdk_keyval_to_lower(keyval);
  if (key >= GDK_KEY_a && key <= GDK_KEY_z) {
    return result + static_cast<char>('a' + (key - GDK_KEY_a));
  }
  if (key >= GDK_KEY_0 && key <= GDK_KEY_9) {
    return result + static_cast<char>('0' + (key - GDK_KEY_0));
  }
  if (key >= GDK_KEY_F1 && key <= GDK_KEY_F12) {
    return result + "f" + std::to_string(key - GDK_KEY_F1 + 1);
  }
  switch (key) {
    case GDK_KEY_space: return result + "space";
    case GDK_KEY_comma: return result + "comma";
    case GDK_KEY_period: return result + "period";
    case GDK_KEY_minus: return result + "minus";
    case GDK_KEY_equal: return result + "equal";
    case GDK_KEY_Tab: return result + "tab";
    default: return {};
  }
}

gboolean linux_record_key(GtkEventControllerKey*, guint keyval, guint,
                          GdkModifierType modifiers, gpointer data) {
  // Esc cancels the active recorder; a bare key is ignored rather than
  // producing an invalid intermediate backend setting.
  if (keyval == GDK_KEY_Escape) return TRUE;
  auto* entry = GTK_EDITABLE(data);
  if (auto const spec = linux_key_spec(keyval, modifiers); !spec.empty()) {
    gtk_editable_set_text(entry, spec.c_str());
  }
  return TRUE;
}

void linux_attach_key_recorder(GtkWidget* widget) {
  auto* controller = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
  g_signal_connect(controller, "key-pressed", G_CALLBACK(linux_record_key),
                   widget);
  gtk_widget_add_controller(widget, controller);
}

void settings_add_text(GtkWidget* section, char const* label, char const* key,
                       char const* placeholder_key,
                       char const* label_key = nullptr) {
  auto* entry = gtk_entry_new();
  if (placeholder_key != nullptr && *placeholder_key != '\0') {
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), tr(placeholder_key).c_str());
    auto& placeholders = g_settings_form->schema_scope
        ? g_settings_form->schema_placeholders : g_settings_form->localized_placeholders;
    placeholders.emplace_back(GTK_ENTRY(entry), placeholder_key);
  }
  gtk_widget_set_size_request(entry, 260, -1);
  if (std::string(key) == "hotkey") {
    linux_attach_key_recorder(entry);
  }
  auto* binding = settings_bind(key, SettingKind::text, entry);
  g_signal_connect(entry, "activate", G_CALLBACK(on_setting_entry_activate), binding);
  auto* focus = gtk_event_controller_focus_new();
  g_signal_connect(focus, "leave", G_CALLBACK(on_setting_entry_leave), binding);
  gtk_widget_add_controller(entry, focus);
  settings_row(section, label, entry, key, label_key);
}

void refresh_settings_language() {
  if (g_settings_form == nullptr) return;
  if (!g_settings_form->schema.empty()) {
    // The schema pages carry their labels in the builder's language: rebuild
    // them, selection kept, and read the lists back in the new language.
    settings_build_schema();
    settings_load_lists();
  }
  for (auto const& [label, key, fallback] : g_settings_form->schema_labels) {
    std::string const value = tr(key.c_str());
    gtk_label_set_text(label, value == key ? fallback.c_str() : value.c_str());
  }
  for (auto const& [button, key] : g_settings_form->schema_buttons) {
    gtk_button_set_label(button, tr(key.c_str()).c_str());
  }
  for (auto const& [entry, key] : g_settings_form->schema_placeholders) {
    gtk_entry_set_placeholder_text(entry, tr(key.c_str()).c_str());
  }
  for (auto const& [label, key, fallback] : g_settings_form->localized_labels) {
    std::string const value = tr(key.c_str());
    gtk_label_set_text(label, value == key ? fallback.c_str() : value.c_str());
  }
  for (auto const& [button, key] : g_settings_form->localized_buttons) {
    gtk_button_set_label(button, tr(key.c_str()).c_str());
  }
  for (auto const& [entry, key] : g_settings_form->localized_placeholders) {
    gtk_entry_set_placeholder_text(entry, tr(key.c_str()).c_str());
  }
  for (auto const& [id, key] : g_settings_form->localized_pages) {
    auto* stack = GTK_STACK(g_settings_form->stack);
    if (auto* child = gtk_stack_get_child_by_name(stack, id.c_str())) {
      if (auto* page = gtk_stack_get_page(stack, child)) {
        gtk_stack_page_set_title(page, tr(key.c_str()).c_str());
      }
    }
  }
  // Choice drop-downs: splice the string model in place so the current
  // selection index survives the repaint.
  for (auto const& binding : g_settings_form->bindings) {
    if (binding->kind != SettingKind::choice || binding->choice_keys.empty())
      continue;
    std::vector<std::string> display;
    display.reserve(binding->choice_keys.size());
    for (std::size_t i = 0; i < binding->choice_keys.size(); ++i) {
      std::string const& key = binding->choice_keys[i];
      std::string const value = key.empty() ? "" : tr(key.c_str());
      display.push_back(key.empty() || value == key
                            ? binding->choice_labels[i] : value);
    }
    std::vector<char const*> labels;
    labels.reserve(display.size() + 1);
    for (auto const& text : display) labels.push_back(text.c_str());
    labels.push_back(nullptr);
    auto* model = gtk_drop_down_get_model(GTK_DROP_DOWN(binding->widget));
    if (GTK_IS_STRING_LIST(model)) {
      gtk_string_list_splice(GTK_STRING_LIST(model), 0,
                             g_list_model_get_n_items(G_LIST_MODEL(model)),
                             labels.data());
    }
  }
  // Dynamic editors rebuild their rows in the new language.
  if (g_settings_form->workflow_selected.empty()) {
    gtk_label_set_text(GTK_LABEL(g_settings_form->workflow_selected_label),
                       tr("settings.wf.select").c_str());
  }
  gtk_label_set_text(GTK_LABEL(g_settings_form->workflow_status),
                     tr("settings.wf.idle").c_str());
  settings_load_commands();
  workflow_load_list();
  workflow_load_detail();
  settings_refresh_ai_status();
}

void on_linux_startup_toggle(GObject* object, GParamSpec*, gpointer) {
  if (!g_settings_form || g_settings_form->loading) return;
  bool const enabled = gtk_switch_get_active(GTK_SWITCH(object));
  char const* appimage = std::getenv("APPIMAGE");
  std::string const executable = appimage && *appimage
      ? std::string(appimage) : executable_path().string();
  std::string error;
  if (!fulcrum_set_autostart(enabled, executable, error)) {
    g_settings_form->loading = true;
    gtk_switch_set_active(GTK_SWITCH(object), !enabled);
    g_settings_form->loading = false;
    settings_show_error(tr_format("settings.login.failed.linux", {error}));
  } else {
    settings_show_error("");
  }
}

void settings_build() {
  g_settings_form = new SettingsForm();
  auto& form = *g_settings_form;
  auto* window = gtk_window_new();
  form.window = GTK_WINDOW(window);
  gtk_window_set_title(GTK_WINDOW(window), "Fulcrum Settings");
  // Fifteen grouped destinations and a description line per page need more
  // room than the old six-item form; the three hosts keep one size.
  gtk_window_set_default_size(GTK_WINDOW(window), 760, 640);
  gtk_window_set_hide_on_close(GTK_WINDOW(window), TRUE);

  // Search-jump highlight and the sidebar's own look (no GtkStackSidebar:
  // its rows are a flat list and cannot carry group headings).
  static GtkCssProvider* settings_css = nullptr;
  if (settings_css == nullptr) {
    settings_css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(settings_css, R"css(
      .setting-highlight { background: alpha(@accent_color, 0.22); border-radius: 6px; }
      .settings-nav { padding: 5px 8px; border-radius: 6px; }
      .settings-nav:checked { background: alpha(@theme_selected_bg_color, 0.25); }
      .settings-group { padding: 10px 8px 3px 8px; opacity: 0.8; font-size: 0.92em; }
      .settings-result { padding: 8px; border-radius: 6px; }
    )css");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(settings_css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }

  // Settings information architecture on every host: a search box over
  // grouped destinations on the left, one scrollable page on the right, a
  // shared footer under the pages for what the last action reported.
  auto* layout = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(content, TRUE);
  gtk_widget_set_vexpand(content, TRUE);

  auto* sidebar_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(sidebar_scroll, 208, -1);
  auto* sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class(sidebar, "navigation-sidebar");
  gtk_widget_set_margin_top(sidebar, 10);
  gtk_widget_set_margin_bottom(sidebar, 10);
  gtk_widget_set_margin_start(sidebar, 8);
  gtk_widget_set_margin_end(sidebar, 8);
  form.nav_box = sidebar;
  auto* search = gtk_search_entry_new();
  form.search = search;
  gtk_widget_set_margin_bottom(search, 8);
  gtk_accessible_update_property(GTK_ACCESSIBLE(search),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "settings-search", -1);
  g_signal_connect(search, "search-changed",
                   G_CALLBACK(+[](GtkEditable*, gpointer) { settings_render_search(); }),
                   nullptr);
  gtk_box_append(GTK_BOX(sidebar), search);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar_scroll), sidebar);

  auto* stack = gtk_stack_new();
  form.stack = stack;
  gtk_widget_set_hexpand(stack, TRUE);
  gtk_widget_set_vexpand(stack, TRUE);
  gtk_stack_set_hhomogeneous(GTK_STACK(stack), FALSE);
  gtk_stack_set_vhomogeneous(GTK_STACK(stack), FALSE);

  // Search results stand in for the current page while the box has text.
  auto* search_scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(search_scrolled),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  auto* search_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(search_page, 20);
  gtk_widget_set_margin_bottom(search_page, 24);
  gtk_widget_set_margin_start(search_page, 24);
  gtk_widget_set_margin_end(search_page, 24);
  auto* search_title = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(search_title), 0.0);
  gtk_widget_add_css_class(search_title, "heading");
  gtk_widget_set_margin_bottom(search_title, 6);
  gtk_box_append(GTK_BOX(search_page), search_title);
  form.search_title = search_title;
  auto* search_results = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand(search_results, TRUE);
  gtk_box_append(GTK_BOX(search_page), search_results);
  form.search_results = search_results;
  auto* search_empty = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(search_empty), 0.0);
  gtk_label_set_wrap(GTK_LABEL(search_empty), TRUE);
  gtk_widget_add_css_class(search_empty, "dim-label");
  form.search_empty = search_empty;
  gtk_box_append(GTK_BOX(search_page), search_empty);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(search_scrolled), search_page);
  gtk_stack_add_named(GTK_STACK(stack), search_scrolled, "__search");

  gtk_box_append(GTK_BOX(layout), sidebar_scroll);
  gtk_box_append(GTK_BOX(layout), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
  gtk_box_append(GTK_BOX(content), stack);
  gtk_box_append(GTK_BOX(layout), content);
  gtk_window_set_child(GTK_WINDOW(window), layout);

  // The stack is the source of truth for the current page: it drives the
  // sidebar toggles and remembers the page search returns to.
  g_signal_connect(stack, "notify::visible-child", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer) {
    auto* current = g_settings_form;
    if (!current) return;
    char const* raw = gtk_stack_get_visible_child_name(GTK_STACK(current->stack));
    std::string const page = raw != nullptr ? raw : "";
    current->nav_sync = true;
    for (auto const& [id, button] : current->nav_buttons) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), id == page);
    }
    current->nav_sync = false;
    if (page != "__search" && !page.empty()) current->last_page = page;
    current->current_page = page;
  }), nullptr);

  // Ctrl+F focuses the search from anywhere in the window; Esc empties it.
  auto* keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed",
                   G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint,
                                  GdkModifierType state, gpointer) -> gboolean {
    auto* current = g_settings_form;
    if (!current) return FALSE;
    bool const primary = (state & (GDK_CONTROL_MASK | GDK_META_MASK)) != 0;
    if (primary && (keyval == GDK_KEY_f || keyval == GDK_KEY_F)) {
      gtk_widget_grab_focus(current->search);
      return TRUE;
    }
    if (keyval == GDK_KEY_Escape) {
      char const* text = gtk_editable_get_text(GTK_EDITABLE(current->search));
      if (text != nullptr && *text != '\0') {
        gtk_editable_set_text(GTK_EDITABLE(current->search), "");
        return TRUE;
      }
    }
    return FALSE;
  }), nullptr);
  gtk_widget_add_controller(window, keys);

  auto make_page = [stack](char const* id, char const* title_key) -> GtkWidget* {
    auto* scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    auto* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(page, 8);
    gtk_widget_set_margin_bottom(page, 24);
    gtk_widget_set_margin_start(page, 24);
    gtk_widget_set_margin_end(page, 24);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), page);
    gtk_stack_add_titled(GTK_STACK(stack), scrolled, id, tr(title_key).c_str());
    g_settings_form->localized_pages.emplace_back(id, title_key);
    return page;
  };

  // Commands is a real backend-owned editor, not just a placeholder label.
  auto* commands_page = make_page("commands", "settings.nav.commands");
  auto* commands = settings_section(commands_page, "settings.nav.commands");
  settings_hint(commands, "settings.hint.commands.linux");
  auto* commands_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append(GTK_BOX(commands), commands_list);
  g_settings_form->commands_list = commands_list;
  auto* commands_message = gtk_label_new(tr("settings.commands.loading").c_str());
  gtk_label_set_xalign(GTK_LABEL(commands_message), 0);
  gtk_label_set_wrap(GTK_LABEL(commands_message), TRUE);
  gtk_box_append(GTK_BOX(commands), commands_message);
  g_settings_form->commands_message = commands_message;

  // Workflow editor: real GTK controls over the same backend actions that
  // serve WinUI and SwiftUI. No host-specific JSON editing or evaluator.
  auto* workflow_page = make_page("workflows", "settings.nav.workflows");
  auto* workflows = settings_section(workflow_page, "settings.wf.title");
  settings_hint(workflows, "settings.wf.hint");
  auto* wf_create = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  auto* wf_name = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(wf_name),
                                 tr("settings.wf.new-name").c_str());
  g_settings_form->localized_placeholders.emplace_back(
      GTK_ENTRY(wf_name), "settings.wf.new-name");
  gtk_widget_set_hexpand(wf_name, TRUE);
  gtk_accessible_update_property(GTK_ACCESSIBLE(wf_name),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "workflow-create-name", -1);
  g_settings_form->workflow_name = wf_name;
  auto* wf_create_button = settings_button("settings.wf.create");
  gtk_accessible_update_property(GTK_ACCESSIBLE(wf_create_button),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "workflow-create", -1);
  g_signal_connect(wf_create_button, "clicked",
                   G_CALLBACK(+[](GtkButton*, gpointer) {
    if (!g_settings_form) return;
    std::string name = settings_trimmed(
        gtk_editable_get_text(GTK_EDITABLE(g_settings_form->workflow_name)));
    if (name.empty()) {
      gtk_label_set_text(GTK_LABEL(g_settings_form->workflow_status),
                         tr("settings.wf.enter-name").c_str());
      return;
    }
    g_settings_form->workflow_list_page = 1;
    workflow_action("workflow.create", name);
  }), nullptr);
  auto* wf_refresh = settings_button("settings.wf.refresh");
  g_signal_connect(wf_refresh, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
    workflow_load_list(); workflow_load_detail();
  }), nullptr);
  gtk_box_append(GTK_BOX(wf_create), wf_name);
  gtk_box_append(GTK_BOX(wf_create), GTK_WIDGET(wf_create_button));
  gtk_box_append(GTK_BOX(wf_create), GTK_WIDGET(wf_refresh));
  gtk_box_append(GTK_BOX(workflows), wf_create);

  auto* wf_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  g_settings_form->workflow_list = wf_list;
  gtk_box_append(GTK_BOX(workflows), wf_list);
  auto* wf_list_navigation = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* wf_prev = GTK_WIDGET(settings_button("settings.wf.prev"));
  GtkWidget* wf_next = GTK_WIDGET(settings_button("settings.wf.next"));
  g_signal_connect(wf_prev, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
    if (!g_settings_form) return;
    g_settings_form->workflow_list_page =
        std::max(1, g_settings_form->workflow_list_page - 1);
    workflow_load_list();
  }), nullptr);
  g_signal_connect(wf_next, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
    if (!g_settings_form) return;
    ++g_settings_form->workflow_list_page;
    workflow_load_list();
  }), nullptr);
  gtk_box_append(GTK_BOX(wf_list_navigation), wf_prev);
  gtk_box_append(GTK_BOX(wf_list_navigation), wf_next);
  gtk_box_append(GTK_BOX(workflows), wf_list_navigation);

  auto* wf_selected = gtk_label_new(tr("settings.wf.select").c_str());
  gtk_label_set_xalign(GTK_LABEL(wf_selected), 0);
  gtk_widget_add_css_class(wf_selected, "heading");
  gtk_box_append(GTK_BOX(workflow_page), wf_selected);
  g_settings_form->workflow_selected_label = wf_selected;
  auto* wf_steps_section = settings_section(workflow_page, "settings.wf.steps");
  auto* wf_steps = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  g_settings_form->workflow_steps = wf_steps;
  gtk_box_append(GTK_BOX(wf_steps_section), wf_steps);
  auto* wf_step_navigation = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* wf_step_prev = GTK_WIDGET(settings_button("settings.wf.prev"));
  GtkWidget* wf_step_next = GTK_WIDGET(settings_button("settings.wf.next"));
  g_signal_connect(wf_step_prev, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
    if (!g_settings_form) return;
    g_settings_form->workflow_step_page =
        std::max(1, g_settings_form->workflow_step_page - 1);
    workflow_load_detail();
  }), nullptr);
  g_signal_connect(wf_step_next, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
    if (!g_settings_form) return;
    ++g_settings_form->workflow_step_page;
    workflow_load_detail();
  }), nullptr);
  gtk_box_append(GTK_BOX(wf_step_navigation), wf_step_prev);
  gtk_box_append(GTK_BOX(wf_step_navigation), wf_step_next);
  gtk_box_append(GTK_BOX(wf_steps_section), wf_step_navigation);
  auto* wf_add = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  auto* wf_step_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(wf_step_entry),
                                 tr("settings.wf.step-placeholder").c_str());
  g_settings_form->localized_placeholders.emplace_back(
      GTK_ENTRY(wf_step_entry), "settings.wf.step-placeholder");
  gtk_widget_set_hexpand(wf_step_entry, TRUE);
  g_settings_form->workflow_step = wf_step_entry;
  gtk_accessible_update_property(GTK_ACCESSIBLE(wf_step_entry),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "workflow-step-expression", -1);
  GtkWidget* wf_add_button = GTK_WIDGET(settings_button("settings.wf.add-step"));
  g_signal_connect(wf_add_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
    if (!g_settings_form || g_settings_form->workflow_selected.empty()) return;
    std::string step = settings_trimmed(
        gtk_editable_get_text(GTK_EDITABLE(g_settings_form->workflow_step)));
    if (step.empty()) {
      gtk_label_set_text(GTK_LABEL(g_settings_form->workflow_status),
                         tr("settings.wf.enter-step").c_str());
      return;
    }
    workflow_action("workflow.add", g_settings_form->workflow_selected +
        std::string(1, '\x1F') + step);
  }), nullptr);
  gtk_box_append(GTK_BOX(wf_add), wf_step_entry);
  gtk_box_append(GTK_BOX(wf_add), wf_add_button);
  gtk_box_append(GTK_BOX(wf_steps_section), wf_add);

  auto* wf_input_section = settings_section(workflow_page, "settings.wf.input");
  auto* wf_input_scroller = gtk_scrolled_window_new();
  gtk_widget_set_size_request(wf_input_scroller, -1, 90);
  auto* wf_input = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(wf_input), GTK_WRAP_WORD_CHAR);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(wf_input_scroller),
                                 wf_input);
  gtk_box_append(GTK_BOX(wf_input_section), wf_input_scroller);
  g_settings_form->workflow_input = wf_input;
  gtk_accessible_update_property(GTK_ACCESSIBLE(wf_input),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "workflow-input-multiline", -1);
  auto* wf_run_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* wf_dry = GTK_WIDGET(settings_button("settings.wf.dry-run"));
  g_signal_connect(wf_dry, "clicked", G_CALLBACK((+[](GtkButton*, gpointer) {
    if (!g_settings_form || g_settings_form->workflow_selected.empty()) return;
    auto* buffer = gtk_text_view_get_buffer(
        GTK_TEXT_VIEW(g_settings_form->workflow_input));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar* raw = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    std::string input = raw ? raw : "";
    g_free(raw);
    g_settings_form->workflow_trace_page = 1;
    workflow_action("workflow.dry", g_settings_form->workflow_selected +
        std::string(1, '\x1F') + input);
  })), nullptr);
  GtkWidget* wf_run_copy = GTK_WIDGET(settings_button("settings.wf.run-copy"));
  gtk_widget_set_sensitive(wf_run_copy, FALSE);
  g_settings_form->workflow_run_copy = wf_run_copy;
  g_signal_connect(wf_run_copy, "clicked", G_CALLBACK((+[](GtkButton*, gpointer) {
    if (!g_settings_form || g_settings_form->workflow_selected.empty() ||
        !g_settings_form->workflow_copy_safe) return;
    auto* buffer = gtk_text_view_get_buffer(
        GTK_TEXT_VIEW(g_settings_form->workflow_input));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar* raw = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    std::string input = raw ? raw : "";
    g_free(raw);
    g_settings_form->workflow_trace_page = 1;
    workflow_action("workflow.run", g_settings_form->workflow_selected +
        std::string(1, '\x1F') + input);
  })), nullptr);
  auto* wf_confirm = gtk_check_button_new_with_label(
      tr("settings.wf.confirm-delete").c_str());
  g_settings_form->localized_buttons.emplace_back(
      GTK_BUTTON(wf_confirm), "settings.wf.confirm-delete");
  g_settings_form->workflow_delete_confirm = wf_confirm;
  GtkWidget* wf_delete = GTK_WIDGET(settings_button("settings.wf.delete"));
  g_signal_connect(wf_delete, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
    if (!g_settings_form || g_settings_form->workflow_selected.empty()) return;
    if (!gtk_check_button_get_active(
            GTK_CHECK_BUTTON(g_settings_form->workflow_delete_confirm))) {
      gtk_label_set_text(GTK_LABEL(g_settings_form->workflow_status),
                         tr("settings.wf.confirm-first").c_str());
      return;
    }
    workflow_action("workflow.delete", g_settings_form->workflow_selected);
  }), nullptr);
  gtk_box_append(GTK_BOX(wf_run_buttons), wf_dry);
  gtk_box_append(GTK_BOX(wf_run_buttons), wf_run_copy);
  gtk_box_append(GTK_BOX(wf_run_buttons), wf_confirm);
  gtk_box_append(GTK_BOX(wf_run_buttons), wf_delete);
  gtk_box_append(GTK_BOX(wf_input_section), wf_run_buttons);
  settings_hint(wf_input_section, "settings.wf.clipboard-note");

  auto* wf_trace_section = settings_section(workflow_page, "settings.wf.trace");
  auto* wf_trace = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  g_settings_form->workflow_trace = wf_trace;
  gtk_box_append(GTK_BOX(wf_trace_section), wf_trace);
  auto* wf_trace_nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* wf_trace_prev = GTK_WIDGET(settings_button("settings.wf.prev"));
  GtkWidget* wf_trace_next = GTK_WIDGET(settings_button("settings.wf.next"));
  g_signal_connect(wf_trace_prev, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
    if (!g_settings_form) return;
    g_settings_form->workflow_trace_page =
        std::max(1, g_settings_form->workflow_trace_page - 1);
    workflow_load_detail();
  }), nullptr);
  g_signal_connect(wf_trace_next, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
    if (!g_settings_form) return;
    ++g_settings_form->workflow_trace_page;
    workflow_load_detail();
  }), nullptr);
  gtk_box_append(GTK_BOX(wf_trace_nav), wf_trace_prev);
  gtk_box_append(GTK_BOX(wf_trace_nav), wf_trace_next);
  gtk_box_append(GTK_BOX(wf_trace_section), wf_trace_nav);
  auto* wf_status = gtk_label_new(tr("settings.wf.idle").c_str());
  gtk_label_set_xalign(GTK_LABEL(wf_status), 0);
  gtk_label_set_wrap(GTK_LABEL(wf_status), TRUE);
  gtk_label_set_selectable(GTK_LABEL(wf_status), TRUE);
  gtk_box_append(GTK_BOX(workflow_page), wf_status);
  g_settings_form->workflow_status = wf_status;

  // Errors and results belong to a persistent shared footer, not an
  // arbitrary section. It stays under the content pages while users
  // navigate the sidebar.
  auto* notice = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(notice), 0.0);
  gtk_label_set_wrap(GTK_LABEL(notice), TRUE);
  gtk_label_set_selectable(GTK_LABEL(notice), TRUE);
  gtk_widget_add_css_class(notice, "caption");
  gtk_widget_add_css_class(notice, "dim-label");
  gtk_widget_set_margin_top(notice, 8);
  gtk_widget_set_margin_start(notice, 16);
  gtk_widget_set_margin_end(notice, 16);
  gtk_widget_set_visible(notice, FALSE);
  gtk_box_append(GTK_BOX(content), notice);
  g_settings_form->notice = GTK_LABEL(notice);

  auto* error = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(error), 0.0);
  gtk_label_set_wrap(GTK_LABEL(error), TRUE);
  gtk_label_set_selectable(GTK_LABEL(error), TRUE);
  gtk_widget_add_css_class(error, "error");
  gtk_widget_set_margin_top(error, 8);
  gtk_widget_set_margin_bottom(error, 8);
  gtk_widget_set_margin_start(error, 16);
  gtk_widget_set_margin_end(error, 16);
  gtk_widget_set_visible(error, FALSE);
  gtk_box_append(GTK_BOX(content), error);
  g_settings_form->error = GTK_LABEL(error);
}


// ---- settings v2 pages -------------------------------------------------------
//
// The sidebar, the pages, their sections and rows come from the backend's
// `settings-ui` data (parsed into SettingsForm::schema); Commands and
// Workflows keep their native editors. What each page shows beside its rows
// — application folders, plugins, snippets, the AI key, the transfers — is
// host code over the list/collection RPCs.

void workflow_clear_rows(GtkWidget* container);

std::string settings_lower(std::string text) {
  for (auto& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

std::vector<std::string> settings_split_csv(std::string const& text) {
  std::vector<std::string> out;
  std::string part;
  for (char const c : text) {
    if (c == ',') { out.push_back(part); part.clear(); } else { part += c; }
  }
  out.push_back(part);
  return out;
}

// `settings-ui` rows: ("page" <page> <group>) opens each page, then
// ("section" <page> <section>) and ("setting" <key> <page> <section>
// <control> <options> <value>) follow in display order.
std::vector<SettingsSchemaPage> settings_parse_schema(
    std::vector<std::vector<std::string>> const& rows) {
  std::vector<SettingsSchemaPage> pages;
  for (auto const& row : rows) {
    if (row.empty()) continue;
    if (row[0] == "page" && row.size() >= 3) {
      pages.push_back(SettingsSchemaPage{row[1], row[2], {}});
      continue;
    }
    if (pages.empty()) continue;
    auto& page = pages.back();
    if (row[0] == "section" && row.size() >= 3 && row[1] == page.id) {
      page.sections.push_back(SettingsSchemaSection{row[2], {}});
      continue;
    }
    if (row[0] != "setting" || row.size() < 7 || row[2] != page.id) continue;
    // A row whose section line never arrived still gets its section.
    if (page.sections.empty() || page.sections.back().id != row[3]) {
      page.sections.push_back(SettingsSchemaSection{row[3], {}});
    }
    page.sections.back().rows.push_back(SettingsSchemaRow{
        row[1], row[3], row[4],
        row[5].empty() ? std::vector<std::string>{} : settings_split_csv(row[5])});
  }
  return pages;
}

void settings_on_nav_toggled(GtkToggleButton* button, gpointer data) {
  auto* form = g_settings_form;
  if (!form || form->nav_sync || !gtk_toggle_button_get_active(button)) return;
  auto* id = static_cast<std::string*>(data);
  gtk_stack_set_visible_child_name(GTK_STACK(form->stack), id->c_str());
}

// Start at login is device-local and never in the schema; it sits before the
// hotkey row.
void settings_add_startup_row(GtkWidget* section) {
  auto* toggle = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(toggle), fulcrum_autostart_enabled());
  gtk_accessible_update_property(GTK_ACCESSIBLE(toggle),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Fulcrum start at login", -1);
  settings_row(section, "Start at login", toggle, nullptr,
               "settings.start-at-login");
  g_signal_connect(toggle, "notify::active",
                   G_CALLBACK(on_linux_startup_toggle), nullptr);
  settings_hint(section, "settings.hint.start-at-login");
}

void settings_add_schema_row(GtkWidget* section, SettingsSchemaRow const& row) {
  std::string const label_key = "settings." + row.key;
  if (row.control == "toggle") {
    settings_add_toggle(section, row.key.c_str(), row.key.c_str(), label_key.c_str());
  } else if (row.control == "select") {
    std::vector<std::string> option_keys;
    for (auto const& option : row.options) {
      // The empty option is the "Off" provider; its key spelling is `off`.
      std::string const key =
          "settings.opt." + row.key + "." + (option.empty() ? "off" : option);
      option_keys.push_back(settings_has_i18n(key) ? key : "");
    }
    std::vector<std::tuple<char const*, char const*, char const*>> options;
    for (std::size_t i = 0; i < row.options.size(); ++i) {
      options.emplace_back(row.options[i].c_str(), row.options[i].c_str(),
                           option_keys[i].c_str());
    }
    settings_add_choice(section, row.key.c_str(), row.key.c_str(), options,
                        label_key.c_str());
  } else if (row.control == "number") {
    // options = the validator's inclusive (min max).
    double low = 0, high = 1'000'000;
    try {
      if (row.options.size() >= 2) { low = std::stod(row.options[0]); high = std::stod(row.options[1]); }
    } catch (...) {
    }
    settings_add_number(section, row.key.c_str(), row.key.c_str(), low, high,
                        label_key.c_str());
  } else {
    std::string const placeholder_key = "settings." + row.key + ".placeholder";
    settings_add_text(section, row.key.c_str(), row.key.c_str(),
                      settings_has_i18n(placeholder_key) ? placeholder_key.c_str() : nullptr,
                      label_key.c_str());
  }
  auto* row_box = gtk_widget_get_last_child(section);
  if (row_box != nullptr) {
    g_settings_form->row_widgets[row.key] = row_box;
    // Snippet expansion has no engine on this host.
    if (row.key == "expansion-enabled" && gtk_widget_get_first_child(row_box) != nullptr) {
      gtk_widget_set_sensitive(gtk_widget_get_last_child(row_box), FALSE);
    }
  }
  std::string hint_key = "settings.hint." + row.key;
  if (row.key == "expansion-enabled") hint_key = "settings.hint.expansion.linux";
  if (settings_has_i18n(hint_key)) settings_hint(section, hint_key.c_str());
}

GtkWidget* settings_list_host(GtkWidget* section) {
  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(box, 14);
  gtk_widget_set_margin_end(box, 14);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_box_append(GTK_BOX(section), box);
  return box;
}

void settings_edit_named_list(std::string kind, std::string id, std::string name,
                              std::string path, std::string enabled);
void settings_edit_collection(bool snippets, std::string id, std::string name,
                              std::string keyword, std::string body);
void settings_confirm(GtkWindow* parent, std::string message,
                      std::string confirm_label, std::function<void()> action);
void settings_export_dialog();
void settings_import_file();
void settings_clear_history(bool clipboard);
void settings_set_plugin(std::string id, bool enabled);
void settings_remove_named_list(std::string kind, std::string id);
void settings_remove_collection(bool snippets, std::string id);

void settings_add_ai_key(GtkWidget* section) {
  // The AI key rides the ai.key action and never comes back over the wire;
  // ai-status reports readiness instead.
  auto* key_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  auto* key_entry = gtk_password_entry_new();
  gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(key_entry), TRUE);
  gtk_widget_set_size_request(key_entry, 240, -1);
  g_signal_connect(key_entry, "activate", G_CALLBACK(on_key_entry_activate), nullptr);
  auto* save = settings_button("settings.save");
  settings_on_clicked(GTK_WIDGET(save), settings_save_key);
  gtk_box_append(GTK_BOX(key_box), key_entry);
  gtk_box_append(GTK_BOX(key_box), GTK_WIDGET(save));
  g_settings_form->key_entry = key_entry;
  settings_row(section, "API key", key_box, nullptr, "settings.ai-api-key");
  auto* status = gtk_label_new(settings_ai_status_text(g_settings_form->ai_status_raw).c_str());
  gtk_label_set_xalign(GTK_LABEL(status), 0.0);
  gtk_widget_add_css_class(status, "dim-label");
  gtk_widget_set_margin_start(status, 14);
  gtk_widget_set_margin_end(status, 14);
  gtk_box_append(GTK_BOX(section), status);
  g_settings_form->ai_status = GTK_LABEL(status);
  auto* notice = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(notice), 0.0);
  gtk_label_set_wrap(GTK_LABEL(notice), TRUE);
  gtk_widget_add_css_class(notice, "caption");
  gtk_widget_add_css_class(notice, "dim-label");
  gtk_widget_set_margin_start(notice, 14);
  gtk_widget_set_margin_end(notice, 14);
  gtk_widget_set_margin_bottom(notice, 8);
  gtk_box_append(GTK_BOX(section), notice);
  g_settings_form->key_notice = GTK_LABEL(notice);
}

// What a page shows beside its schema rows.
void settings_add_page_extras(GtkWidget* page, std::string const& id) {
  auto* form = g_settings_form;
  if (id == "apps") {
    for (char const* kind : {"app-folders", "apps"}) {
      bool const folders = std::string(kind) == "app-folders";
      auto* body = settings_section(page, folders ? "settings.list.app-folders"
                                                  : "settings.list.apps");
      settings_hint(body, folders ? "settings.list.app-folders-help"
                                  : "settings.list.apps-help");
      form->list_hosts[kind] = settings_list_host(body);
      if (folders) {
        auto* add = settings_button("settings.list.add");
        gtk_widget_set_halign(GTK_WIDGET(add), GTK_ALIGN_START);
        gtk_widget_set_margin_start(GTK_WIDGET(add), 14);
        gtk_widget_set_margin_bottom(GTK_WIDGET(add), 8);
        settings_on_clicked(GTK_WIDGET(add), [] { settings_edit_named_list("app-folders", "", "", "", ""); });
        gtk_box_append(GTK_BOX(body), GTK_WIDGET(add));
      }
    }
  } else if (id == "extensions") {
    auto* body = settings_section(page, "settings.nav.extensions");
    settings_hint(body, "settings.ext.help");
    form->list_hosts["plugins"] = settings_list_host(body);
  } else if (id == "snippets" || id == "quicklinks") {
    bool const snippets = id == "snippets";
    auto* body = settings_section(page, snippets ? "settings.nav.snippets"
                                                 : "settings.nav.quicklinks");
    form->list_hosts[id] = settings_list_host(body);
    auto* add = settings_button(snippets ? "settings.col.add-snippet"
                                         : "settings.col.add-quicklink");
    gtk_widget_set_halign(GTK_WIDGET(add), GTK_ALIGN_START);
    gtk_widget_set_margin_start(GTK_WIDGET(add), 14);
    gtk_widget_set_margin_bottom(GTK_WIDGET(add), 8);
    settings_on_clicked(GTK_WIDGET(add), [snippets] {
      settings_edit_collection(snippets, "", "", "", "");
    });
    gtk_box_append(GTK_BOX(body), GTK_WIDGET(add));
  } else if (id == "clipboard") {
    auto* body = settings_section(page, "");
    auto* clear = settings_button("settings.clipboard.clear");
    gtk_widget_set_halign(GTK_WIDGET(clear), GTK_ALIGN_START);
    gtk_widget_set_margin_start(GTK_WIDGET(clear), 14);
    gtk_widget_set_margin_bottom(GTK_WIDGET(clear), 8);
    settings_on_clicked(GTK_WIDGET(clear), [] {
      auto* current = g_settings_form;
      settings_confirm(GTK_WINDOW(current->window), tr("settings.clipboard.clear-confirm"),
                        tr("settings.clipboard.clear"), [] { settings_clear_history(true); });
    });
    gtk_box_append(GTK_BOX(body), GTK_WIDGET(clear));
  } else if (id == "privacy") {
    auto* body = settings_section(page, "");
    settings_hint(body, "settings.privacy.stays");
    auto* clear = settings_button("settings.privacy.clear");
    gtk_widget_set_halign(GTK_WIDGET(clear), GTK_ALIGN_START);
    gtk_widget_set_margin_start(GTK_WIDGET(clear), 14);
    gtk_widget_set_margin_bottom(GTK_WIDGET(clear), 8);
    settings_on_clicked(GTK_WIDGET(clear), [] {
      auto* current = g_settings_form;
      settings_confirm(GTK_WINDOW(current->window), tr("settings.privacy.clear-confirm"),
                        tr("settings.privacy.clear"), [] { settings_clear_history(false); });
    });
    gtk_box_append(GTK_BOX(body), GTK_WIDGET(clear));
  } else if (id == "sync") {
    // Export / import a file the host picks; the backend never sees a path.
    auto* body = settings_section(page, "");
    settings_hint(body, "settings.sync.transfer-help");
    auto* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(buttons, 14);
    gtk_widget_set_margin_bottom(buttons, 8);
    auto* export_button = settings_button("settings.sync.export");
    auto* import_button = settings_button("settings.sync.import");
    settings_on_clicked(GTK_WIDGET(export_button), settings_export_dialog);
    settings_on_clicked(GTK_WIDGET(import_button), settings_import_file);
    gtk_box_append(GTK_BOX(buttons), GTK_WIDGET(export_button));
    gtk_box_append(GTK_BOX(buttons), GTK_WIDGET(import_button));
    gtk_box_append(GTK_BOX(body), buttons);
    // "Check for Updates Now" keeps opening the dedicated update window.
    auto* updates = settings_section(page, "settings.updates");
    settings_hint(updates, "settings.updates-note.linux");
    GtkWidget* check_now = GTK_WIDGET(settings_button("settings.check-updates-now"));
    gtk_accessible_update_property(GTK_ACCESSIBLE(check_now),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "settings-update-check", -1);
    gtk_widget_set_halign(check_now, GTK_ALIGN_START);
    gtk_widget_set_margin_start(check_now, 14);
    gtk_widget_set_margin_bottom(check_now, 8);
    settings_on_clicked(check_now, [] { show_update_window(true); });
    gtk_box_append(GTK_BOX(updates), check_now);
  }
}

void settings_build_schema() {
  auto* form = g_settings_form;
  if (!form || form->stack == nullptr) return;
  auto* stack = GTK_STACK(form->stack);
  std::string const keep = form->current_page == "__search" ? form->last_page
                                                            : form->current_page;
  // Drop the previous build entirely: its bindings, labels, sidebar rows
  // and pages go with the widgets.
  for (auto const& [id, button] : form->nav_buttons) {
    if (auto* parent = gtk_widget_get_parent(button)) {
      gtk_box_remove(GTK_BOX(parent), button);
    }
  }
  for (auto const& page : form->schema) {
    if (auto* child = gtk_stack_get_child_by_name(stack, page.id.c_str())) {
      gtk_stack_remove(stack, child);
    }
  }
  for (auto* heading : form->nav_headings) {
    if (auto* parent = gtk_widget_get_parent(heading)) gtk_box_remove(GTK_BOX(parent), heading);
  }
  form->nav_headings.clear();
  form->nav_buttons.clear();
  form->bindings.clear();
  form->schema_labels.clear();
  form->schema_buttons.clear();
  form->schema_placeholders.clear();
  form->row_widgets.clear();
  form->row_hints.clear();
  form->row_errors.clear();
  form->list_hosts.clear();
  form->key_entry = nullptr;
  form->ai_status = nullptr;
  form->key_notice = nullptr;

  form->schema_scope = true;
  GtkWidget* first_button = nullptr;
  for (std::size_t index = 0; index < form->schema.size(); ++index) {
    auto const& page = form->schema[index];
    // Sidebar: a group heading whenever the group changes, then the page.
    std::string const group_key = "settings.group." + page.group;
    if (index == 0 || form->schema[index - 1].group != page.group) {
      std::string const group_text = tr(group_key.c_str());
      auto* heading = gtk_label_new(group_text == group_key ? page.group.c_str()
                                                            : group_text.c_str());
      gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
      gtk_widget_add_css_class(heading, "settings-group");
      gtk_box_append(GTK_BOX(form->nav_box), heading);
      form->nav_headings.push_back(heading);
      if (group_text != group_key) {
        form->schema_labels.emplace_back(GTK_LABEL(heading), group_key, page.group);
      }
    }
    std::string const nav_key = "settings.nav." + page.id;
    auto* button = gtk_toggle_button_new_with_label(tr(nav_key.c_str()));
    gtk_widget_add_css_class(button, "settings-nav");
    gtk_widget_set_halign(button, GTK_ALIGN_FILL);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   ("settings-sidebar-" + page.id).c_str(), -1);
    form->schema_buttons.emplace_back(GTK_BUTTON(button), nav_key);
    auto* page_id = new std::string(page.id);
    g_object_set_data_full(G_OBJECT(button), "settings-page", page_id,
                           [](gpointer data) { delete static_cast<std::string*>(data); });
    g_signal_connect(button, "toggled", G_CALLBACK(settings_on_nav_toggled), page_id);
    gtk_box_append(GTK_BOX(form->nav_box), button);
    if (first_button == nullptr) first_button = button;
    else gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(button), GTK_TOGGLE_BUTTON(first_button));
    form->nav_buttons[page.id] = button;

    // Page: description, then one card per section with its rows.
    auto* scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 24);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), box);
    gtk_stack_add_named(stack, scrolled, page.id.c_str());
    std::string const description_key = "settings.page." + page.id;
    if (settings_has_i18n(description_key)) {
      auto* description = gtk_label_new(tr(description_key.c_str()));
      gtk_label_set_xalign(GTK_LABEL(description), 0.0);
      gtk_label_set_wrap(GTK_LABEL(description), TRUE);
      gtk_widget_add_css_class(description, "dim-label");
      form->schema_labels.emplace_back(GTK_LABEL(description), description_key, "");
      gtk_box_append(GTK_BOX(box), description);
    }
    for (auto const& section : page.sections) {
      std::string const section_key = "settings.section." + section.id;
      auto* body = settings_section(box, section_key.c_str());
      for (auto const& row : section.rows) {
        if (row.key == "hotkey") settings_add_startup_row(body);
        settings_add_schema_row(body, row);
      }
      if (page.id == "ai" && section.id == "provider") settings_add_ai_key(body);
    }
    settings_add_page_extras(box, page.id);
  }
  form->schema_scope = false;
  // Commands and Workflows keep their sidebar entries after the schema
  // pages; give them nav toggles the first time (and every time, since the
  // group was rebuilt).
  for (auto const& id_pair : form->localized_pages) {
    auto const& key = id_pair.second;
    auto* button = gtk_toggle_button_new_with_label(tr(key.c_str()));
    gtk_widget_add_css_class(button, "settings-nav");
    gtk_widget_set_halign(button, GTK_ALIGN_FILL);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   ("settings-sidebar-" + id).c_str(), -1);
    auto* id = new std::string(id_pair.first);
    g_object_set_data_full(G_OBJECT(button), "settings-page", id,
                           [](gpointer data) { delete static_cast<std::string*>(data); });
    g_signal_connect(button, "toggled", G_CALLBACK(settings_on_nav_toggled), id);
    gtk_box_append(GTK_BOX(form->nav_box), button);
    if (first_button == nullptr) first_button = button;
    else gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(button), GTK_TOGGLE_BUTTON(first_button));
    form->nav_buttons[id_pair.first] = button;
  }
  if (first_button != nullptr) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(first_button), TRUE);
  }
  if (gtk_stack_get_child_by_name(stack, keep.c_str())) {
    gtk_stack_set_visible_child_name(stack, keep.c_str());
  }
}

void settings_jump_to(std::string const& page, std::string const& key);

void settings_render_search() {
  auto* form = g_settings_form;
  if (!form || form->search_silent || form->search_results == nullptr) return;
  std::string const needle = settings_lower(
      settings_trimmed(gtk_editable_get_text(GTK_EDITABLE(form->search))));
  workflow_clear_rows(form->search_results);
  form->dynamic_buttons.clear();
  if (needle.empty()) {
    if (form->current_page == "__search") {
      gtk_stack_set_visible_child_name(GTK_STACK(form->stack), form->last_page.c_str());
    }
    return;
  }
  // Local filter over the labels and hints the window already holds — no
  // RPC. A result reads "label" over "page / section".
  int count = 0;
  for (auto const& page : form->schema) {
    for (auto const& section : page.sections) {
      for (auto const& row : section.rows) {
        std::string const label = tr(("settings." + row.key).c_str());
        std::string const haystack = settings_lower(
            label + " " + tr(("settings.hint." + row.key).c_str()) + " " + row.key);
        if (haystack.find(needle) == std::string::npos) continue;
        auto* button = gtk_button_new();
        gtk_widget_set_hexpand(button, TRUE);
        gtk_widget_add_css_class(button, "settings-result");
        auto* lines = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        auto* title = gtk_label_new(label.c_str());
        gtk_label_set_xalign(GTK_LABEL(title), 0.0);
        gtk_box_append(GTK_BOX(lines), title);
        auto* path = gtk_label_new((tr(("settings.nav." + page.id).c_str()) + " / " +
                                    tr(("settings.section." + section.id).c_str())).c_str());
        gtk_label_set_xalign(GTK_LABEL(path), 0.0);
        gtk_widget_add_css_class(path, "dim-label");
        gtk_widget_add_css_class(path, "caption");
        gtk_box_append(GTK_BOX(lines), path);
        gtk_button_set_child(GTK_BUTTON(button), lines);
        std::string const page_id = page.id;
        settings_on_clicked(button, [page_id, key = row.key] {
          settings_jump_to(page_id, key);
        });
        gtk_box_append(GTK_BOX(form->search_results), button);
        ++count;
      }
    }
  }
  gtk_label_set_text(GTK_LABEL(form->search_title),
                     tr_format("settings.search.results", {std::to_string(count)}).c_str());
  gtk_label_set_text(GTK_LABEL(form->search_empty), tr("settings.search.empty").c_str());
  gtk_widget_set_visible(form->search_empty, count == 0);
  gtk_stack_set_visible_child_name(GTK_STACK(form->stack), "__search");
}

gboolean settings_unhighlight(gpointer data) {
  std::unique_ptr<std::string> key(static_cast<std::string*>(data));
  auto* form = g_settings_form;
  if (form && form->highlighted_key == *key) {
    if (auto it = form->row_widgets.find(*key); it != form->row_widgets.end()) {
      gtk_widget_remove_css_class(it->second, "setting-highlight");
    }
    form->highlighted_key.clear();
  }
  return G_SOURCE_REMOVE;
}

gboolean settings_scroll_to_row(gpointer data) {
  std::unique_ptr<std::string> key(static_cast<std::string*>(data));
  auto* form = g_settings_form;
  if (!form) return G_SOURCE_REMOVE;
  auto it = form->row_widgets.find(*key);
  if (it == form->row_widgets.end()) return G_SOURCE_REMOVE;
  GtkWidget* row = it->second;
  auto* scrolled = gtk_widget_get_ancestor(row, GTK_TYPE_SCROLLED_WINDOW);
  graphene_rect_t bounds{};
  if (scrolled != nullptr && gtk_widget_compute_bounds(row, scrolled, &bounds)) {
    auto* adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
    double const page_size = gtk_adjustment_get_page_size(adjustment);
    double target = bounds.origin.y - (page_size - bounds.size.height) / 2;
    target = std::clamp(target, gtk_adjustment_get_lower(adjustment),
                        std::max(gtk_adjustment_get_lower(adjustment),
                                 gtk_adjustment_get_upper(adjustment) - page_size));
    gtk_adjustment_set_value(adjustment, target);
  }
  return G_SOURCE_REMOVE;
}

void settings_jump_to(std::string const& page, std::string const& key);

// Selecting a hit leaves search, shows its page and highlights the row.
void settings_jump_to(std::string const& page, std::string const& key) {
  auto* form = g_settings_form;
  if (!form) return;
  form->search_silent = true;
  gtk_editable_set_text(GTK_EDITABLE(form->search), "");
  form->search_silent = false;
  gtk_stack_set_visible_child_name(GTK_STACK(form->stack), page.c_str());
  auto it = form->row_widgets.find(key);
  if (it == form->row_widgets.end()) return;
  if (!form->highlighted_key.empty()) {
    if (auto old = form->row_widgets.find(form->highlighted_key); old != form->row_widgets.end()) {
      gtk_widget_remove_css_class(old->second, "setting-highlight");
    }
  }
  form->highlighted_key = key;
  gtk_widget_add_css_class(it->second, "setting-highlight");
  // The page is allocated on the next frame.
  g_idle_add(settings_scroll_to_row, new std::string(key));
  g_timeout_add(1800, settings_unhighlight, new std::string(key));
}


// ---- list pages: what the rows show beside the schema ------------------------

// What a background step reports back to the GTK thread: an error for the
// footer, and a neutral notice when something was done.
struct SettingsOutcome {
  std::string error;
  std::string notice;
};

// Runs `work` off the GTK thread, then hands its outcome to `after` on the
// GTK thread; without `after` the footer reports it.
void settings_background(std::function<SettingsOutcome()> work,
                         std::function<void(SettingsOutcome const&)> after = nullptr) {
  using Payload = std::pair<SettingsOutcome, std::function<void(SettingsOutcome const&)>>;
  std::thread([work = std::move(work), after = std::move(after)]() mutable {
    SettingsOutcome outcome;
    try {
      outcome = work();
    } catch (std::exception const& e) {
      outcome = SettingsOutcome{e.what(), ""};
    } catch (...) {
      outcome = SettingsOutcome{tr("settings.col.failed"), ""};
    }
    g_idle_add([](gpointer data) -> int {
      std::unique_ptr<Payload> payload(static_cast<Payload*>(data));
      if (g_settings_form != nullptr) {
        if (payload->second) payload->second(payload->first);
        else {
          settings_show_error(payload->first.error);
          settings_show_notice(payload->first.notice);
        }
      }
      return G_SOURCE_REMOVE;
    }, new Payload(std::move(outcome), std::move(after)));
  }).detach();
}

void settings_reload_lists(SettingsOutcome const&) { settings_load_lists(); }

// The backend leads a named-list refusal with a stable code word.
std::string settings_list_refusal(std::string const& raw) {
  std::string const code = raw.substr(0, raw.find(':'));
  if (code == "duplicate-name") return tr("settings.list.duplicate");
  if (code == "path-required") return tr("settings.list.path-required");
  if (code == "name-required") return tr("settings.list.name-required");
  return tr_format("settings.list.failed", {raw});
}

GtkWidget* settings_dim_label(std::string const& text) {
  auto* label = gtk_label_new(text.c_str());
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_widget_add_css_class(label, "caption");
  gtk_widget_add_css_class(label, "dim-label");
  return label;
}

GtkWidget* settings_caption(std::string const& text) {
  auto* label = gtk_label_new(text.c_str());
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_widget_add_css_class(label, "caption");
  gtk_widget_add_css_class(label, "dim-label");
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  return label;
}

void settings_set_plugin(std::string id, bool enabled) {
  settings_background([id = std::move(id), enabled]() -> SettingsOutcome {
    auto* api = Launcher::instance().api.get();
    if (api == nullptr) return {};
    SettingsOutcome outcome;
    try {
      if (!api->plugin_set_enabled(id, enabled ? "on" : "off").get()) {
        outcome.error = tr_format("settings.col.failed", {id});
      }
    } catch (std::exception const& e) {
      outcome.error = e.what();
    }
    return outcome;
  }, settings_reload_lists);
}

void settings_remove_named_list(std::string kind, std::string id) {
  settings_background([kind = std::move(kind), id = std::move(id)]() -> SettingsOutcome {
    auto* api = Launcher::instance().api.get();
    if (api == nullptr) return {};
    SettingsOutcome outcome;
    try {
      api->named_list_remove(kind, id).get();
    } catch (std::exception const& e) {
      outcome.error = settings_list_refusal(e.what());
    }
    return outcome;
  }, settings_reload_lists);
}

void settings_remove_collection(bool snippets, std::string id) {
  settings_background([snippets, id = std::move(id)]() -> SettingsOutcome {
    auto* api = Launcher::instance().api.get();
    if (api == nullptr) return {};
    SettingsOutcome outcome;
    try {
      if (snippets) api->snippet_delete(id).get();
      else api->quicklink_delete(id).get();
    } catch (std::exception const& e) {
      outcome.error = tr_format("settings.col.failed", {e.what()});
    }
    return outcome;
  }, settings_reload_lists);
}

// Clearing history is irreversible for the user; callers confirm first.
void settings_clear_history(bool clipboard) {
  settings_background([clipboard]() -> SettingsOutcome {
    auto* api = Launcher::instance().api.get();
    if (api == nullptr) return {};
    SettingsOutcome outcome;
    try {
      if (clipboard) {
        auto const count = api->clipboard_clear().get();
        outcome.notice = tr_format("settings.clipboard.cleared", {std::to_string(count)});
      } else {
        api->history_clear().get();
        outcome.notice = tr("settings.privacy.cleared");
      }
    } catch (std::exception const& e) {
      outcome.error = tr_format("settings.col.failed", {e.what()});
    }
    return outcome;
  });
}

// The switch of a plugin row.
void on_plugin_switch(GObject* object, GParamSpec*, gpointer) {
  auto* toggle = GTK_SWITCH(object);
  auto* id = static_cast<std::string*>(g_object_get_data(G_OBJECT(toggle), "plugin-id"));
  if (id == nullptr) return;
  settings_set_plugin(*id, gtk_switch_get_active(toggle));
}

// The "searchable" switch of an application row: the entry keeps its name and
// path, only the switch moves.
void on_app_switch(GObject* object, GParamSpec*, gpointer data) {
  auto* item = static_cast<std::vector<std::string>*>(data);
  if (item == nullptr || item->size() < 5) return;
  bool const enabled = gtk_switch_get_active(GTK_SWITCH(object));
  settings_background([item = *item, enabled]() -> SettingsOutcome {
    auto* api = Launcher::instance().api.get();
    if (api == nullptr) return {};
    SettingsOutcome outcome;
    try {
      api->named_list_set("apps", (*item)[0], (*item)[1], (*item)[2], "",
                          enabled ? "on" : "off").get();
    } catch (std::exception const& e) {
      outcome.error = settings_list_refusal(e.what());
    }
    return outcome;
  }, settings_reload_lists);
}

void settings_render_named_list(std::string const& kind,
                                std::vector<std::vector<std::string>> const& items) {
  auto* form = g_settings_form;
  if (!form) return;
  auto host = form->list_hosts.find(kind);
  if (host == form->list_hosts.end()) return;
  bool const folders = kind == "app-folders";
  if (items.empty()) {
    gtk_box_append(GTK_BOX(host->second), settings_dim_label(tr("settings.list.empty")));
  }
  for (auto const& item : items) {
    if (item.size() < 5) continue;
    auto const& id = item[0];
    auto const& name = item[1];
    auto const& path = item[2];
    auto const& enabled = item[4];
    auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(row, 2);
    gtk_widget_set_margin_bottom(row, 2);
    auto* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(text, TRUE);
    auto* title = gtk_label_new(name.c_str());
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_label_set_wrap(GTK_LABEL(title), TRUE);
    gtk_box_append(GTK_BOX(text), title);
    if (folders) gtk_box_append(GTK_BOX(text), settings_caption(path));
    gtk_box_append(GTK_BOX(row), text);
    if (!folders) {
      auto* toggle = gtk_switch_new();
      gtk_switch_set_active(GTK_SWITCH(toggle), enabled == "on");
      gtk_accessible_update_property(GTK_ACCESSIBLE(toggle),
                                     GTK_ACCESSIBLE_PROPERTY_LABEL, name.c_str(), -1);
      auto* stored = new std::vector<std::string>(item);
      g_object_set_data_full(G_OBJECT(toggle), "entry", stored,
                             [](gpointer data) { delete static_cast<std::vector<std::string>*>(data); });
      g_signal_connect(toggle, "notify::active", G_CALLBACK(on_app_switch), stored);
      gtk_box_append(GTK_BOX(row), toggle);
    } else {
      auto* edit = settings_button("settings.list.edit");
      auto* remove = settings_button("settings.list.remove");
      settings_on_clicked(GTK_WIDGET(edit), [kind, id, name, path, enabled] {
        settings_edit_named_list(kind, id, name, path, enabled);
      });
      settings_on_clicked(GTK_WIDGET(remove), [kind, id, name] {
        auto* current = g_settings_form;
        settings_confirm(GTK_WINDOW(current->window),
                          tr_format("settings.list.remove-confirm", {name}),
                          tr("settings.list.remove"), [kind, id] {
          settings_remove_named_list(kind, id);
        });
      });
      gtk_box_append(GTK_BOX(row), GTK_WIDGET(edit));
      gtk_box_append(GTK_BOX(row), GTK_WIDGET(remove));
    }
    gtk_box_append(GTK_BOX(host->second), row);
  }
}

void settings_render_plugins(std::vector<std::vector<std::string>> const& items,
                             std::vector<std::string> const& disabled) {
  auto* form = g_settings_form;
  if (!form) return;
  auto host = form->list_hosts.find("plugins");
  if (host == form->list_hosts.end()) return;
  if (items.empty()) {
    gtk_box_append(GTK_BOX(host->second), settings_dim_label(tr("settings.ext.empty")));
  }
  for (auto const& item : items) {
    // Rows: (id name version n-commands id "plugin" "" ""); a load error has
    // an empty id and its message in the name column.
    if (item.size() < 4) continue;
    if (item[0].empty()) {
      auto* message = gtk_label_new(item[1].c_str());
      gtk_label_set_xalign(GTK_LABEL(message), 0.0);
      gtk_label_set_wrap(GTK_LABEL(message), TRUE);
      gtk_widget_add_css_class(message, "error");
      gtk_box_append(GTK_BOX(host->second), message);
      continue;
    }
    auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    auto* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(text, TRUE);
    auto* title = gtk_label_new(item[1].c_str());
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_box_append(GTK_BOX(text), title);
    std::string const meta = tr_format("settings.ext.commands", {item[3]}) +
        (item[2].empty() ? "" : " · " + item[2]);
    gtk_box_append(GTK_BOX(text), settings_caption(meta));
    gtk_box_append(GTK_BOX(row), text);
    auto* toggle = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(toggle),
                          std::find(disabled.begin(), disabled.end(), item[0]) == disabled.end());
    gtk_accessible_update_property(GTK_ACCESSIBLE(toggle),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, item[1].c_str(), -1);
    g_object_set_data_full(G_OBJECT(toggle), "plugin-id",
                           new std::string(item[0]),
                           [](gpointer data) { delete static_cast<std::string*>(data); });
    g_signal_connect(toggle, "notify::active", G_CALLBACK(on_plugin_switch), nullptr);
    gtk_box_append(GTK_BOX(row), toggle);
    gtk_box_append(GTK_BOX(host->second), row);
  }
}

void settings_render_collection(bool snippets,
                                 std::vector<std::vector<std::string>> const& items) {
  auto* form = g_settings_form;
  if (!form) return;
  auto host = form->list_hosts.find(snippets ? "snippets" : "quicklinks");
  if (host == form->list_hosts.end()) return;
  if (items.empty()) {
    gtk_box_append(GTK_BOX(host->second),
                   settings_dim_label(tr(snippets ? "settings.col.empty-snippets"
                                                  : "settings.col.empty-quicklinks")));
  }
  for (auto const& item : items) {
    if (item.size() < 4) continue;
    auto const& id = item[0];
    auto const& name = item[1];
    auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(row, 2);
    gtk_widget_set_margin_bottom(row, 2);
    auto* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(text, TRUE);
    auto* title = gtk_label_new(name.c_str());
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_box_append(GTK_BOX(text), title);
    std::string const meta = item[2] + (item[2].empty() ? "" : " · ") + item[3];
    gtk_box_append(GTK_BOX(text), settings_caption(meta));
    gtk_box_append(GTK_BOX(row), text);
    auto* edit = settings_button("settings.col.edit");
    auto* remove = settings_button("settings.col.delete");
    settings_on_clicked(GTK_WIDGET(edit), [snippets, id, name, keyword = item[2], body = item[3]] {
      settings_edit_collection(snippets, id, name, keyword, body);
    });
    settings_on_clicked(GTK_WIDGET(remove), [snippets, id, name] {
      auto* current = g_settings_form;
      settings_confirm(GTK_WINDOW(current->window),
                        tr_format("settings.col.delete-confirm", {name}),
                        tr("settings.col.delete"), [snippets, id] {
        settings_remove_collection(snippets, id);
      });
    });
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(edit));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(remove));
    gtk_box_append(GTK_BOX(host->second), row);
  }
}

void settings_load_lists() {
  auto* api_raw = Launcher::instance().api.get();
  if (api_raw == nullptr || g_settings_form == nullptr) return;
  using Lists = std::map<std::string, std::vector<std::vector<std::string>>>;
  std::thread([api_raw] {
    auto* payload = new Lists;
    try { (*payload)["app-folders"] = api_raw->named_list_items("app-folders").get(); } catch (...) {}
    try { (*payload)["apps"] = api_raw->named_list_items("apps").get(); } catch (...) {}
    try { (*payload)["plugins"] = api_raw->plugins_list().get(); } catch (...) {}
    try { (*payload)["snippets"] = api_raw->snippet_list().get(); } catch (...) {}
    try { (*payload)["quicklinks"] = api_raw->quicklink_list().get(); } catch (...) {}
    g_idle_add([](gpointer data) -> int {
      std::unique_ptr<Lists> payload(static_cast<Lists*>(data));
      auto* form = g_settings_form;
      if (form == nullptr) return G_SOURCE_REMOVE;
      form->dynamic_scope = true;
      for (auto const& [kind, host] : form->list_hosts) workflow_clear_rows(host);
      form->dynamic_buttons.clear();
      std::vector<std::string> disabled;
      if (auto it = form->values.find("plugins-disabled"); it != form->values.end()) {
        disabled = settings_split_csv(it->second);
      }
      settings_render_named_list("app-folders", (*payload)["app-folders"]);
      settings_render_named_list("apps", (*payload)["apps"]);
      settings_render_plugins((*payload)["plugins"], disabled);
      settings_render_collection(true, (*payload)["snippets"]);
      settings_render_collection(false, (*payload)["quicklinks"]);
      form->dynamic_scope = false;
      return G_SOURCE_REMOVE;
    }, payload);
  }).detach();
}


// ---- dialogs -------------------------------------------------------------------

// An irreversible step the host asks about once; Cancel is the default and
// the destructive button never is.
void settings_confirm(GtkWindow* parent, std::string message,
                      std::string confirm_label, std::function<void()> action) {
  auto* dialog = gtk_alert_dialog_new("%s", message.c_str());
  std::string const cancel = tr("settings.cancel");
  std::vector<char const*> buttons{cancel.c_str(), confirm_label.c_str(), nullptr};
  gtk_alert_dialog_set_buttons(GTK_ALERT_DIALOG(dialog), buttons.data());
  gtk_alert_dialog_set_modal(GTK_ALERT_DIALOG(dialog), TRUE);
  gtk_alert_dialog_choose(GTK_ALERT_DIALOG(dialog), parent, nullptr,
                          +[](GObject* source, GAsyncResult* result, gpointer data) {
    std::unique_ptr<std::function<void()>> fn(
        static_cast<std::function<void()>*>(data));
    int const chosen = gtk_alert_dialog_choose_finish(
        GTK_ALERT_DIALOG(source), result, nullptr);
    if (chosen == 1) (*fn)();
  }, new std::function<void()>(std::move(action)));
  g_object_unref(dialog);
}

// A modal form window: heading, labelled fields, an error line under them,
// Cancel and a suggested primary button. `collect` reads the fields on the
// GTK thread (a refusal keeps the window open with the reason under the
// fields); `submit` it installs runs off-thread and reports through the
// outcome. The shared_ptr outlives the RPC even if the window closes.
struct SettingsFormDialog {
  GtkWidget* window{nullptr};
  GtkWidget* box{nullptr};
  GtkWidget* error_label{nullptr};
  std::string primary_label{"settings.save"};
  std::function<SettingsOutcome()> collect;
  std::function<SettingsOutcome()> submit;
  std::function<void()> on_saved;
  // Set by `collect` when it handled the primary itself (a file picker).
  bool handled{false};
  bool closed{false};
};

std::shared_ptr<SettingsFormDialog> settings_dialog_new(char const* title_key) {
  auto dialog = std::make_shared<SettingsFormDialog>();
  auto* window = gtk_window_new();
  gtk_window_set_transient_for(GTK_WINDOW(window), g_settings_form->window);
  gtk_window_set_modal(GTK_WINDOW(window), TRUE);
  gtk_window_set_title(GTK_WINDOW(window), tr(title_key));
  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(box, 20);
  gtk_widget_set_margin_bottom(box, 20);
  gtk_widget_set_margin_start(box, 20);
  gtk_widget_set_margin_end(box, 20);
  gtk_widget_set_size_request(box, 440, -1);
  gtk_window_set_child(GTK_WINDOW(window), box);
  auto* heading = gtk_label_new(tr(title_key));
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
  gtk_widget_add_css_class(heading, "heading");
  gtk_box_append(GTK_BOX(box), heading);
  auto* error = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(error), 0.0);
  gtk_label_set_wrap(GTK_LABEL(error), TRUE);
  gtk_widget_add_css_class(error, "error");
  dialog->window = window;
  dialog->box = box;
  dialog->error_label = error;
  return dialog;
}

GtkWidget* settings_dialog_field(SettingsFormDialog const& dialog, char const* label_key,
                                 std::string const& initial, bool multi_line = false) {
  auto* label = gtk_label_new(tr(label_key));
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_widget_add_css_class(label, "caption");
  gtk_box_append(GTK_BOX(dialog.box), label);
  if (multi_line) {
    auto* scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scrolled), TRUE);
    auto* view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),
                             initial.c_str(), -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), view);
    gtk_widget_set_size_request(scrolled, -1, 120);
    gtk_box_append(GTK_BOX(dialog.box), scrolled);
    return view;
  }
  auto* entry = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(entry), initial.c_str());
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_box_append(GTK_BOX(dialog.box), entry);
  return entry;
}

std::string settings_field_text(GtkWidget* field) {
  if (GTK_IS_TEXT_VIEW(field)) {
    GtkTextIter begin, end;
    auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(field));
    gtk_text_buffer_get_bounds(buffer, &begin, &end);
    char* text = gtk_text_buffer_get_text(buffer, &begin, &end, FALSE);
    std::string const out = text != nullptr ? text : "";
    g_free(text);
    return out;
  }
  char const* text = gtk_editable_get_text(GTK_EDITABLE(field));
  return text != nullptr ? text : "";
}

void settings_dialog_present(std::shared_ptr<SettingsFormDialog> dialog) {
  // The error line sits under the fields, above the buttons.
  gtk_box_append(GTK_BOX(dialog->box), dialog->error_label);
  gtk_box_append(GTK_BOX(dialog->box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  auto* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);
  auto* cancel = gtk_button_new_with_label(tr("settings.cancel"));
  auto* primary = gtk_button_new_with_label(tr(dialog->primary_label.c_str()));
  gtk_widget_add_css_class(primary, "suggested-action");
  // The state lives as long as the window; the shared_ptr in flight keeps
  // it valid past a close.
  g_object_set_data_full(G_OBJECT(dialog->window), "dialog-state",
                         new std::shared_ptr<SettingsFormDialog>(dialog),
                         [](gpointer data) {
                           delete static_cast<std::shared_ptr<SettingsFormDialog>*>(data);
                         });
  g_signal_connect(dialog->window, "close-request",
                   G_CALLBACK(+[](GtkWidget*, gpointer data) -> gboolean {
    static_cast<SettingsFormDialog*>(data)->closed = true;
    return FALSE;
  }), dialog.get());
  settings_on_clicked(GTK_WIDGET(cancel), [dialog] {
    dialog->closed = true;
    gtk_window_close(GTK_WINDOW(dialog->window));
  });
  settings_on_clicked(primary, [dialog] {
    if (dialog->collect) {
      SettingsOutcome const early = dialog->collect();
      if (dialog->handled) return;
      if (!early.error.empty()) {
        gtk_label_set_text(GTK_LABEL(dialog->error_label), early.error.c_str());
        return;
      }
    }
    settings_background([dialog]() -> SettingsOutcome {
      return dialog->submit ? dialog->submit() : SettingsOutcome{};
    }, [dialog](SettingsOutcome const& outcome) {
      if (dialog->closed) return;
      if (!outcome.error.empty()) {
        gtk_label_set_text(GTK_LABEL(dialog->error_label), outcome.error.c_str());
        return;
      }
      dialog->closed = true;
      if (dialog->on_saved) dialog->on_saved();
      gtk_window_close(GTK_WINDOW(dialog->window));
    });
  });
  gtk_box_append(GTK_BOX(buttons), GTK_WIDGET(cancel));
  gtk_box_append(GTK_BOX(buttons), GTK_WIDGET(primary));
  gtk_box_append(GTK_BOX(dialog->box), buttons);
  gtk_window_present(GTK_WINDOW(dialog->window));
}

// Application folders editor (the searchable-applications list has no form:
// its rows only switch).
void settings_edit_named_list(std::string kind, std::string id, std::string name,
                              std::string path, std::string enabled) {
  if (g_settings_form == nullptr) return;
  bool const folders = kind == "app-folders";
  auto dialog = settings_dialog_new(id.empty() ? "settings.list.add-title"
                                               : "settings.list.edit-title");
  auto* name_entry = settings_dialog_field(*dialog, "settings.list.name", name);
  GtkWidget* path_entry = nullptr;
  if (folders) {
    path_entry = settings_dialog_field(*dialog, "settings.list.path", path);
    auto* browse = gtk_button_new_with_label(tr("settings.browse"));
    gtk_widget_set_halign(browse, GTK_ALIGN_START);
    settings_on_clicked(browse, [dialog, path_entry] {
      auto* chooser = gtk_file_chooser_native_new(
          tr("settings.browse"), GTK_WINDOW(dialog->window),
          GTK_FILE_CHOOSER_SELECT_FOLDER, tr("settings.cancel"), tr("settings.browse"));
      g_signal_connect(GTK_NATIVE_DIALOG(chooser), "response",
                       G_CALLBACK(+[](GtkNativeDialog* native, gint response, gpointer data) {
        auto* entry = GTK_EDITABLE(data);
        if (response == GTK_RESPONSE_ACCEPT) {
          if (auto* file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(native))) {
            if (char* chosen = g_file_get_path(file)) {
              gtk_editable_set_text(entry, chosen);
              g_free(chosen);
            }
          }
        }
        g_object_unref(native);
      }), path_entry);
      gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
    });
    gtk_box_append(GTK_BOX(dialog->box), browse);
  }
  dialog->collect = [dialog, kind, id, name_entry, path_entry, folders, enabled]() mutable
      -> SettingsOutcome {
    std::string const new_name = settings_trimmed(gtk_editable_get_text(GTK_EDITABLE(name_entry)));
    std::string const new_path = path_entry != nullptr
        ? settings_trimmed(gtk_editable_get_text(GTK_EDITABLE(path_entry))) : "";
    if (new_name.empty()) return {tr("settings.list.name-required"), ""};
    if (folders && new_path.empty()) return {tr("settings.list.path-required"), ""};
    dialog->submit = [kind, id, new_name, new_path, folders, enabled]() -> SettingsOutcome {
      auto* api = Launcher::instance().api.get();
      if (api == nullptr) return {};
      SettingsOutcome outcome;
      try {
        // An edit keeps the entry's switch where it was.
        api->named_list_set(kind, id, new_name, folders ? new_path : "", "",
                            enabled.empty() ? "on" : enabled).get();
      } catch (std::exception const& e) {
        outcome.error = settings_list_refusal(e.what());
      }
      return outcome;
    };
    return {};
  };
  dialog->on_saved = settings_load_lists;
  settings_dialog_present(dialog);
}

// Snippet / quicklink editor. The backend has no group field for these
// lists, so the host keeps them flat, like the prototype's collection cards.
void settings_edit_collection(bool snippets, std::string id, std::string name,
                              std::string keyword, std::string body) {
  if (g_settings_form == nullptr) return;
  char const* title_key = snippets
      ? (id.empty() ? "settings.col.add-snippet" : "settings.col.edit-snippet")
      : (id.empty() ? "settings.col.add-quicklink" : "settings.col.edit-quicklink");
  auto dialog = settings_dialog_new(title_key);
  auto* name_entry = settings_dialog_field(*dialog, "settings.col.name", name);
  auto* keyword_entry = settings_dialog_field(*dialog, "settings.col.keyword", keyword);
  auto* body_field = settings_dialog_field(
      *dialog, snippets ? "settings.col.body" : "settings.col.url", body, snippets);
  if (!snippets) {
    gtk_box_append(GTK_BOX(dialog->box), settings_dim_label(tr("settings.col.url-help")));
  }
  dialog->collect = [dialog, snippets, id, name_entry, keyword_entry, body_field]() mutable
      -> SettingsOutcome {
    std::string const new_name = settings_trimmed(gtk_editable_get_text(GTK_EDITABLE(name_entry)));
    std::string const new_keyword = settings_trimmed(
        gtk_editable_get_text(GTK_EDITABLE(keyword_entry)));
    std::string const new_body = settings_trimmed(settings_field_text(body_field));
    if (new_name.empty() || new_keyword.empty() || new_body.empty()) {
      return {tr("settings.col.required"), ""};
    }
    dialog->submit = [snippets, id, new_name, new_keyword, new_body]() -> SettingsOutcome {
      auto* api = Launcher::instance().api.get();
      if (api == nullptr) return {};
      SettingsOutcome outcome;
      try {
        if (snippets) api->snippet_save(id, new_name, new_keyword, new_body).get();
        else api->quicklink_save(id, new_name, new_keyword, new_body).get();
      } catch (std::exception const& e) {
        outcome.error = tr_format("settings.col.failed", {e.what()});
      }
      return outcome;
    };
    return {};
  };
  dialog->on_saved = settings_load_lists;
  settings_dialog_present(dialog);
}

namespace {
std::string settings_read_file(std::string const& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// "plan:<category>:<count>,…" → one display line per category.
std::string settings_plan_summary(std::string const& plan) {
  std::string summary;
  for (auto const& part : settings_split_csv(plan.substr(5))) {
    auto const colon = part.find(':');
    if (colon == std::string::npos) continue;
    if (!summary.empty()) summary += "\n";
    summary += tr(("settings.transfer." + part.substr(0, colon)).c_str()) + ": " +
        part.substr(colon + 1);
  }
  return summary;
}
}  // namespace

// Export: choose what to include, then where to save. The backend never
// sees a path.
void settings_export_dialog() {
  if (g_settings_form == nullptr) return;
  auto dialog = settings_dialog_new("settings.sync.export");
  std::array<GtkCheckButton*, 4> checks{};
  std::array<char const*, 4> const keys{
      "settings.transfer.appearance", "settings.transfer.features",
      "settings.transfer.commands", "settings.transfer.content"};
  std::array<char const*, 4> const categories{"appearance", "features", "commands", "content"};
  for (std::size_t i = 0; i < keys.size(); ++i) {
    auto* check = gtk_check_button_new_with_label(tr(keys[i]));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), TRUE);
    gtk_widget_set_halign(check, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(dialog->box), check);
    checks[i] = GTK_CHECK_BUTTON(check);
  }
  gtk_box_append(GTK_BOX(dialog->box), settings_dim_label(tr("settings.sync.transfer-help")));
  dialog->primary_label = "settings.sync.export";
  dialog->collect = [dialog, checks, categories]() mutable -> SettingsOutcome {
    std::string chosen;
    for (std::size_t i = 0; i < checks.size(); ++i) {
      if (gtk_check_button_get_active(checks[i])) {
        if (!chosen.empty()) chosen += ",";
        chosen += categories[i];
      }
    }
    if (chosen.empty()) return {tr("settings.sync.transfer-help"), ""};
    dialog->handled = true;
    struct ChooserState {
      std::string categories;
      std::shared_ptr<SettingsFormDialog> dialog;
    };
    auto* state = new ChooserState{chosen, dialog};
    auto* chooser = gtk_file_chooser_native_new(
        tr("settings.sync.export"), GTK_WINDOW(dialog->window),
        GTK_FILE_CHOOSER_SAVE, tr("settings.cancel"), tr("settings.sync.export"));
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser), "fulcrum-settings.json");
    g_signal_connect(GTK_NATIVE_DIALOG(chooser), "response",
                     G_CALLBACK(+[](GtkNativeDialog* native, gint response, gpointer data) {
      std::unique_ptr<ChooserState> state(static_cast<ChooserState*>(data));
      auto* file = response == GTK_RESPONSE_ACCEPT
          ? gtk_file_chooser_get_file(GTK_FILE_CHOOSER(native)) : nullptr;
      char* path = file != nullptr ? g_file_get_path(file) : nullptr;
      g_object_unref(native);
      if (path == nullptr) return;
      std::string const target(path);
      g_free(path);
      settings_background([categories = state->categories, target]() -> SettingsOutcome {
        auto* api = Launcher::instance().api.get();
        if (api == nullptr) return {};
        SettingsOutcome outcome;
        try {
          std::string const payload = api->settings_export(categories).get();
          std::ofstream out(target, std::ios::binary);
          out << payload;
          out.close();
          if (!out) outcome.error = tr("settings.sync.file-failed");
          else outcome.notice = tr_format("settings.sync.exported", {target});
        } catch (std::exception const& e) {
          outcome.error = e.what();
        }
        return outcome;
      }, [dialog = state->dialog](SettingsOutcome const& outcome) {
        if (dialog->closed) return;
        if (!outcome.error.empty()) {
          gtk_label_set_text(GTK_LABEL(dialog->error_label), outcome.error.c_str());
          return;
        }
        dialog->closed = true;
        gtk_window_close(GTK_WINDOW(dialog->window));
        settings_show_notice(outcome.notice);
      });
    }), state);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
    return {};
  };
  settings_dialog_present(dialog);
}

// Import, step one: pick a file and ask the backend what it would replace;
// step two, after one deliberate confirmation: apply it and read back.
void settings_import_file() {
  if (g_settings_form == nullptr) return;
  auto* chooser = gtk_file_chooser_native_new(
      tr("settings.sync.import"), GTK_WINDOW(g_settings_form->window),
      GTK_FILE_CHOOSER_OPEN, tr("settings.cancel"), tr("settings.sync.import"));
  auto* filters = gtk_file_filter_new();
  gtk_file_filter_set_name(filters, "JSON");
  gtk_file_filter_add_pattern(filters, "*.json");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filters);
  g_signal_connect(GTK_NATIVE_DIALOG(chooser), "response",
                   G_CALLBACK(+[](GtkNativeDialog* native, gint response, gpointer) {
    auto* file = response == GTK_RESPONSE_ACCEPT
        ? gtk_file_chooser_get_file(GTK_FILE_CHOOSER(native)) : nullptr;
    char* path = file != nullptr ? g_file_get_path(file) : nullptr;
    g_object_unref(native);
    if (path == nullptr) return;
    std::string const source(path);
    g_free(path);
    settings_background([source]() -> SettingsOutcome {
      std::string const payload = settings_read_file(source);
      if (payload.empty()) return {tr("settings.sync.file-failed"), ""};
      auto* api = Launcher::instance().api.get();
      if (api == nullptr) return {};
      SettingsOutcome outcome;
      try {
        std::string const plan = api->settings_import(payload, "off").get();
        if (plan.rfind("plan:", 0) != 0) {
          outcome.error = tr_format("settings.sync.import-failed", {source});
        } else {
          // The summary rides the notice to the confirmation.
          outcome.notice = settings_plan_summary(plan);
        }
      } catch (std::exception const& e) {
        outcome.error = tr_format("settings.sync.import-failed", {source});
      }
      return outcome;
    }, [source](SettingsOutcome const& outcome) {
      if (!outcome.error.empty()) {
        settings_show_error(outcome.error);
        return;
      }
      auto* form = g_settings_form;
      settings_confirm(GTK_WINDOW(form->window), outcome.notice,
                        tr("settings.sync.import-apply"), [source] {
        settings_background([source]() -> SettingsOutcome {
          std::string const payload = settings_read_file(source);
          auto* api = Launcher::instance().api.get();
          if (api == nullptr) return {};
          SettingsOutcome applied;
          try {
            api->settings_import(payload, "on").get();
            applied.notice = tr("settings.sync.imported");
          } catch (std::exception const& e) {
            applied.error = tr_format("settings.sync.import-failed", {e.what()});
          }
          return applied;
        }, [](SettingsOutcome const& applied) {
          if (!applied.error.empty()) {
            settings_show_error(applied.error);
            return;
          }
          settings_show_notice(applied.notice);
          // The rows and lists now hold other content: read everything back.
          settings_load();
          settings_load_lists();
        });
      });
    });
  }), nullptr);
  gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
}


void settings_apply(std::map<std::string, std::string> values, std::string ai_status) {
  if (g_settings_form == nullptr) return;
  g_settings_form->values = std::move(values);
  g_settings_form->loading = true;
  for (auto const& binding : g_settings_form->bindings) {
    auto const it = g_settings_form->values.find(binding->key);
    std::string const value = it == g_settings_form->values.end() ? "" : it->second;
    switch (binding->kind) {
      case SettingKind::toggle:
        gtk_switch_set_active(GTK_SWITCH(binding->widget), value == "on");
        break;
      case SettingKind::choice: {
        guint selected = GTK_INVALID_LIST_POSITION;
        for (std::size_t i = 0; i < binding->choices.size(); ++i) {
          if (binding->choices[i] == value) {
            selected = static_cast<guint>(i);
            break;
          }
        }
        gtk_drop_down_set_selected(GTK_DROP_DOWN(binding->widget), selected);
        break;
      }
      case SettingKind::number: {
        double number = 0;
        try {
          number = std::stod(value);
        } catch (...) {
        }
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(binding->widget), number);
        break;
      }
      case SettingKind::text:
        gtk_editable_set_text(GTK_EDITABLE(binding->widget), value.c_str());
        break;
    }
  }
  g_settings_form->ai_status_raw = ai_status;
  if (g_settings_form->ai_status != nullptr) {
    gtk_label_set_text(g_settings_form->ai_status, settings_ai_status_text(ai_status).c_str());
  }
  g_settings_form->loading = false;
}

void settings_load() {
  auto* api_raw = Launcher::instance().api.get();
  if (api_raw == nullptr || g_settings_form == nullptr) return;
  std::thread([api_raw]() {
    struct Payload {
      std::map<std::string, std::string> values;
      std::string ai_status;
      std::vector<SettingsSchemaPage> schema;
      bool has_schema{false};
    };
    auto* payload = new Payload;
    try {
      // settings-list rows: (key value description …); booleans read on/off.
      for (auto const& row : api_raw->settings_list().get()) {
        if (row.size() >= 2) {
          payload->values[row[0]] = row[1];
        }
      }
    } catch (...) {
    }
    try {
      payload->ai_status = api_raw->ai_status().get();
    } catch (...) {
    }
    try {
      payload->schema = settings_parse_schema(api_raw->settings_ui().get());
      payload->has_schema = !payload->schema.empty();
    } catch (...) {
    }
    g_idle_add([](gpointer user_data) -> int {
      std::unique_ptr<Payload> loaded(static_cast<Payload*>(user_data));
      if (g_settings_form == nullptr) return G_SOURCE_REMOVE;
      if (loaded->has_schema) {
        // A new schema (first load, or pages changed) rebuilds the pages;
        // the values below then flow into the fresh controls.
        bool const changed = loaded->schema.size() != g_settings_form->schema.size();
        g_settings_form->schema = loaded->schema;
        if (changed) {
          settings_build_schema();
          settings_load_lists();
        }
      }
      settings_apply(std::move(loaded->values), std::move(loaded->ai_status));
      return G_SOURCE_REMOVE;
    }, payload);
  }).detach();
}

struct LinuxCommandRow {
  std::string id;
  GtkWidget* alias{nullptr};
  GtkWidget* hotkey{nullptr};
};

void settings_load_commands();

void settings_commit_binding(std::string id, std::string alias,
                             std::string hotkey, bool clear) {
  auto* api = Launcher::instance().api.get();
  if (!api || !g_settings_form) return;
  gtk_label_set_text(GTK_LABEL(g_settings_form->commands_message),
                     tr("settings.commands.saving").c_str());
  std::thread([api, id = std::move(id), alias = std::move(alias),
               hotkey = std::move(hotkey), clear] {
    std::string message;
    try {
      bool const ok = clear
          ? api->binding_clear(id).get()
          : api->binding_set(id, alias, hotkey).get();
      if (!ok) message = tr("settings.commands.rejected");
    } catch (std::exception const& e) { message = e.what(); }
    g_idle_add([](gpointer data) -> gboolean {
      std::unique_ptr<std::string> error(static_cast<std::string*>(data));
      if (g_settings_form) {
        if (error->empty()) {
          settings_load_commands();
        } else {
          gtk_label_set_text(GTK_LABEL(g_settings_form->commands_message),
                             error->c_str());
        }
      }
      return G_SOURCE_REMOVE;
    }, new std::string(std::move(message)));
  }).detach();
}

void settings_load_commands() {
  auto* api = Launcher::instance().api.get();
  if (!api || !g_settings_form || !g_settings_form->commands_list) return;
  using Commands = std::vector<std::vector<std::string>>;
  struct Payload { Commands commands; Commands bindings; std::string error; };
  std::thread([api] {
    auto* payload = new Payload;
    try {
      payload->commands = api->bindings_commands().get();
      payload->bindings = api->bindings_list().get();
    } catch (std::exception const& e) { payload->error = e.what(); }
    g_idle_add([](gpointer data) -> gboolean {
      std::unique_ptr<Payload> payload(static_cast<Payload*>(data));
      if (!g_settings_form) return G_SOURCE_REMOVE;
      auto* list = g_settings_form->commands_list;
      for (auto* child = gtk_widget_get_first_child(list); child;) {
        auto* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(list), child);
        child = next;
      }
      if (!payload->error.empty()) {
        gtk_label_set_text(GTK_LABEL(g_settings_form->commands_message),
                           payload->error.c_str());
        return G_SOURCE_REMOVE;
      }
      for (auto const& command : payload->commands) {
        if (command.size() < 2) continue;
        std::string alias, hotkey;
        for (auto const& saved : payload->bindings) {
          if (saved.size() >= 3 && saved[0] == command[0]) {
            alias = saved[1]; hotkey = saved[2]; break;
          }
        }
        auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_top(row, 6);
        gtk_widget_set_margin_bottom(row, 6);
        auto* title = gtk_label_new(command[1].c_str());
        gtk_label_set_xalign(GTK_LABEL(title), 0);
        gtk_widget_set_hexpand(title, TRUE);
        gtk_widget_set_size_request(title, 110, -1);
        auto* alias_entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(alias_entry),
                                       tr("settings.commands.alias").c_str());
        gtk_editable_set_text(GTK_EDITABLE(alias_entry), alias.c_str());
        gtk_widget_set_size_request(alias_entry, 115, -1);
        auto* hotkey_entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(hotkey_entry),
                                       tr("settings.commands.press-keys").c_str());
        gtk_editable_set_text(GTK_EDITABLE(hotkey_entry), hotkey.c_str());
        gtk_widget_set_size_request(hotkey_entry, 145, -1);
        gtk_widget_set_sensitive(hotkey_entry, FALSE);
        auto* save = gtk_button_new_with_label(tr("settings.save").c_str());
        auto* clear = gtk_button_new_with_label(tr("settings.commands.clear").c_str());
        auto* state = new LinuxCommandRow{command[0], alias_entry, hotkey_entry};
        g_object_set_data_full(G_OBJECT(row), "command-binding", state,
            [](gpointer data) { delete static_cast<LinuxCommandRow*>(data); });
        g_signal_connect(save, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
          auto* r = static_cast<LinuxCommandRow*>(data);
          settings_commit_binding(
              r->id,
              settings_trimmed(gtk_editable_get_text(GTK_EDITABLE(r->alias))),
              settings_trimmed(gtk_editable_get_text(GTK_EDITABLE(r->hotkey))),
              false);
        }), state);
        g_signal_connect(clear, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
          auto* r = static_cast<LinuxCommandRow*>(data);
          settings_commit_binding(r->id, "", "", true);
        }), state);
        gtk_box_append(GTK_BOX(row), title);
        gtk_box_append(GTK_BOX(row), alias_entry);
        gtk_box_append(GTK_BOX(row), hotkey_entry);
        gtk_box_append(GTK_BOX(row), save);
        gtk_box_append(GTK_BOX(row), clear);
        gtk_box_append(GTK_BOX(list), row);
      }
      gtk_label_set_text(GTK_LABEL(g_settings_form->commands_message),
          tr("settings.commands.unavailable-linux").c_str());
      return G_SOURCE_REMOVE;
    }, payload);
  }).detach();
}

// ---- Workflow native GTK editor ------------------------------------------
// Read only display rows through search; mutate only through the existing
// backend run_action. Keep expensive calls off the GTK UI thread.

using WorkflowWireRows = std::vector<std::vector<std::string>>;
struct WorkflowListPayload {
  WorkflowWireRows rows;
  std::string error;
  int page{1};
};
struct WorkflowDetailPayload {
  WorkflowWireRows steps;
  WorkflowWireRows trace;
  std::string error;
  std::string name;
  int step_page{1};
  int trace_page{1};
};
struct WorkflowCommandPayload {
  std::string id;
  std::string status;
  std::string error;
};

void workflow_clear_rows(GtkWidget* container) {
  if (!container) return;
  for (auto* child = gtk_widget_get_first_child(container); child;) {
    auto* next = gtk_widget_get_next_sibling(child);
    gtk_box_remove(GTK_BOX(container), child);
    child = next;
  }
}

void workflow_load_list() {
  auto* api = Launcher::instance().api.get();
  if (!api || !g_settings_form || !g_settings_form->workflow_list) return;
  int const page = g_settings_form->workflow_list_page;
  std::thread([api, page] {
    auto* payload = new WorkflowListPayload;
    payload->page = page;
    try {
      payload->rows = api->search("wf list " + std::to_string(page)).get();
    } catch (std::exception const& e) {
      payload->error = e.what();
    } catch (...) { payload->error = tr("settings.wf.load-failed"); }
    g_idle_add([](gpointer data) -> gboolean {
      std::unique_ptr<WorkflowListPayload> payload(
          static_cast<WorkflowListPayload*>(data));
      if (!g_settings_form || !g_settings_form->workflow_list ||
          payload->page != g_settings_form->workflow_list_page)
        return G_SOURCE_REMOVE;
      auto* list = g_settings_form->workflow_list;
      workflow_clear_rows(list);
      int count = 0;
      for (auto const& row : payload->rows) {
        if (row.size() < 8 || row[3] != "Workflow" || row[7] != "Saved")
          continue;
        std::string const prefix = "Workflow · ";
        if (row[1].rfind(prefix, 0) != 0) continue;
        std::string name = row[1].substr(prefix.size());
        auto* button = gtk_button_new_with_label(name.c_str());
        gtk_widget_set_hexpand(button, TRUE);
        gtk_widget_set_halign(button, GTK_ALIGN_FILL);
        auto* owned_name = new std::string(name);
        g_object_set_data_full(G_OBJECT(button), "workflow-name", owned_name,
                               [](gpointer p) { delete static_cast<std::string*>(p); });
        g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton*,
                                                           gpointer user_data) {
          if (!g_settings_form) return;
          g_settings_form->workflow_selected =
              *static_cast<std::string*>(user_data);
          g_settings_form->workflow_step_page = 1;
          g_settings_form->workflow_trace_page = 1;
          gtk_check_button_set_active(
              GTK_CHECK_BUTTON(g_settings_form->workflow_delete_confirm),
              FALSE);
          workflow_load_detail();
        }), owned_name);
        gtk_box_append(GTK_BOX(list), button);
        ++count;
      }
      if (count == 0)
        gtk_box_append(GTK_BOX(list),
                       gtk_label_new(tr("settings.wf.empty").c_str()));
      if (!payload->error.empty())
        gtk_label_set_text(GTK_LABEL(g_settings_form->workflow_status),
                           payload->error.c_str());
      return G_SOURCE_REMOVE;
    }, payload);
  }).detach();
}

void workflow_load_detail() {
  auto* api = Launcher::instance().api.get();
  if (!api || !g_settings_form || !g_settings_form->workflow_steps) return;
  std::string name = g_settings_form->workflow_selected;
  if (name.empty()) {
    gtk_label_set_text(GTK_LABEL(g_settings_form->workflow_selected_label),
                       tr("settings.wf.select").c_str());
    workflow_clear_rows(g_settings_form->workflow_steps);
    workflow_clear_rows(g_settings_form->workflow_trace);
    g_settings_form->workflow_copy_safe = false;
    gtk_widget_set_sensitive(g_settings_form->workflow_run_copy, FALSE);
    return;
  }
  gtk_label_set_text(GTK_LABEL(g_settings_form->workflow_selected_label),
                     name.c_str());
  int const step_page = g_settings_form->workflow_step_page;
  int const trace_page = g_settings_form->workflow_trace_page;
  std::thread([api, name, step_page, trace_page] {
    auto* payload = new WorkflowDetailPayload;
    payload->name = name;
    payload->step_page = step_page;
    payload->trace_page = trace_page;
    try {
      payload->steps = api->search("wf show " + name + " " +
                                   std::to_string(step_page)).get();
      payload->trace = api->search("wf trace " + name + " " +
                                   std::to_string(trace_page)).get();
    } catch (std::exception const& e) { payload->error = e.what(); }
    catch (...) { payload->error = tr("settings.wf.load-failed"); }
    g_idle_add([](gpointer data) -> gboolean {
      std::unique_ptr<WorkflowDetailPayload> payload(
          static_cast<WorkflowDetailPayload*>(data));
      if (!g_settings_form || payload->name != g_settings_form->workflow_selected ||
          payload->step_page != g_settings_form->workflow_step_page ||
          payload->trace_page != g_settings_form->workflow_trace_page)
        return G_SOURCE_REMOVE;
      workflow_clear_rows(g_settings_form->workflow_steps);
      workflow_clear_rows(g_settings_form->workflow_trace);
      g_settings_form->workflow_copy_safe = false;
      for (auto const& row : payload->steps) {
        if (row.size() < 8 || row[3] != "Workflow" || row[7] != "Step")
          continue;
        auto* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        auto* label = gtk_label_new((row[1] + " · " + row[2]).c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_box_append(GTK_BOX(line), label);
        auto const last_space = row[1].find_last_of(' ');
        std::string index = last_space == std::string::npos
                                ? "" : row[1].substr(last_space + 1);
        auto* remove = gtk_button_new_with_label(tr("settings.wf.remove").c_str());
        auto* action = new std::pair<std::string, std::string>(
            payload->name, std::move(index));
        g_object_set_data_full(G_OBJECT(remove), "workflow-step-action", action,
            [](gpointer p) {
              delete static_cast<std::pair<std::string, std::string>*>(p);
            });
        g_signal_connect(remove, "clicked", G_CALLBACK((+[](GtkButton*,
                                                            gpointer data) {
          auto* a = static_cast<std::pair<std::string, std::string>*>(data);
          workflow_action("workflow.remove", a->first +
                          std::string(1, '\x1F') + a->second);
        })), action);
        gtk_box_append(GTK_BOX(line), remove);
        gtk_box_append(GTK_BOX(g_settings_form->workflow_steps), line);
        if (row[2] == "copy") g_settings_form->workflow_copy_safe = true;
      }
      gtk_widget_set_sensitive(g_settings_form->workflow_run_copy,
                               g_settings_form->workflow_copy_safe);
      for (auto const& row : payload->trace) {
        if (row.size() < 8 || row[3] != "Workflow" || row[7] != "Trace")
          continue;
        auto* text = gtk_label_new((row[1] + " · " + row[2]).c_str());
        gtk_label_set_xalign(GTK_LABEL(text), 0);
        gtk_label_set_wrap(GTK_LABEL(text), TRUE);
        gtk_box_append(GTK_BOX(g_settings_form->workflow_trace), text);
      }
      if (!payload->error.empty())
        gtk_label_set_text(GTK_LABEL(g_settings_form->workflow_status),
                           payload->error.c_str());
      return G_SOURCE_REMOVE;
    }, payload);
  }).detach();
}

void workflow_action(std::string id, std::string arg) {
  auto* api = Launcher::instance().api.get();
  if (!api || !g_settings_form) return;
  gtk_label_set_text(GTK_LABEL(g_settings_form->workflow_status),
                     tr("settings.wf.running").c_str());
  std::thread([api, id = std::move(id), arg = std::move(arg)] {
    auto* payload = new WorkflowCommandPayload;
    payload->id = id;
    try { payload->status = api->run_action(id, arg).get(); }
    catch (std::exception const& e) { payload->error = e.what(); }
    catch (...) { payload->error = tr("settings.wf.action-failed"); }
    g_idle_add([](gpointer data) -> gboolean {
      std::unique_ptr<WorkflowCommandPayload> payload(
          static_cast<WorkflowCommandPayload*>(data));
      if (!g_settings_form) return G_SOURCE_REMOVE;
      gtk_label_set_text(GTK_LABEL(g_settings_form->workflow_status),
          payload->error.empty()
              ? tr("settings.wf.done").c_str()
              : payload->error.c_str());
      if (payload->error.empty() && payload->id == "workflow.delete") {
        g_settings_form->workflow_selected.clear();
        g_settings_form->workflow_list_page = 1;
        g_settings_form->workflow_step_page = 1;
        g_settings_form->workflow_trace_page = 1;
        gtk_check_button_set_active(
            GTK_CHECK_BUTTON(g_settings_form->workflow_delete_confirm),
            FALSE);
      }
      workflow_load_list();
      workflow_load_detail();
      return G_SOURCE_REMOVE;
    }, payload);
  }).detach();
}

void show_settings_window() {
  if (Launcher::instance().api == nullptr) {
    Launcher::instance().set_status("Settings open once the backend is ready…");
    return;
  }
  if (g_settings_form == nullptr) {
    settings_build();
  }
  settings_load();
  settings_load_lists();
  settings_load_commands();
  workflow_load_list();
  workflow_load_detail();
  gtk_window_present(g_settings_form->window);
}

// ---- dedicated update window ----------------------------------------------
//
// Update-flow parity with macOS/Windows: every entry point (tray Check for
// Updates, the launcher `update.*` rows, the Settings Sync & Updates button,
// the silent launch check) converges on this one independent window. The
// backend owns the signed transfer (no cancel RPC), so the window offers
// Close — which hides it while a download continues — instead of Cancel.
// AppImage installs hand off to the verified atomic replacement; tarball
// installs stop at the manual-install guidance.

enum class UpdatePhase {
  idle, checking, up_to_date, available, downloading, downloaded, failed
};

struct UpdateWindowState {
  GtkWindow* window{nullptr};
  GtkLabel* title{nullptr};
  GtkLabel* status{nullptr};
  GtkWidget* spinner{nullptr};
  GtkWidget* bar{nullptr};
  GtkWidget* check{nullptr};
  GtkWidget* close{nullptr};
  GtkWidget* download{nullptr};
  GtkWidget* install{nullptr};
  GtkWidget* retry{nullptr};
  UpdatePhase phase{UpdatePhase::idle};
  int percent{0};
  std::string version;
  std::int64_t size{0};
  std::string error;
  std::uint64_t generation{0};
  bool polling{false};
};

UpdateWindowState* g_update_window = nullptr;

std::string update_size_label(std::int64_t bytes) {
  char buffer[64];
  if (bytes >= 1024 * 1024) {
    std::snprintf(buffer, sizeof(buffer), "%.1f MB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    std::snprintf(buffer, sizeof(buffer), "%.1f KB",
                  static_cast<double>(bytes) / 1024.0);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%lld B",
                  static_cast<long long>(bytes));
  }
  return buffer;
}

void update_window_paint() {
  auto* state = g_update_window;
  if (state == nullptr) return;
  using Phase = UpdatePhase;
  bool const appimage = std::getenv("APPIMAGE") != nullptr &&
                        *std::getenv("APPIMAGE") != '\0';
  std::string title;
  std::string status;
  switch (state->phase) {
    case Phase::idle:
      break;
    case Phase::checking:
      status = tr("updateChecking");
      break;
    case Phase::up_to_date:
      status = tr("updateUpToDate");
      break;
    case Phase::available:
      title = tr("updateAvailableTitle");
      status = tr_format("updateAvailableBody",
                         {state->version, update_size_label(state->size)});
      break;
    case Phase::downloading:
      status = tr_format("updateDownloadPercent",
                         {std::to_string(state->percent)});
      break;
    case Phase::downloaded:
      status = tr("update.downloaded");
      if (!appimage) status += "\n" + tr("updateInstallHintLinux");
      break;
    case Phase::failed:
      status = state->error;
      break;
  }
  gtk_label_set_text(state->title, title.c_str());
  gtk_widget_set_visible(GTK_WIDGET(state->title), !title.empty());
  gtk_label_set_text(state->status, status.c_str());
  gtk_widget_set_visible(GTK_WIDGET(state->status),
                         state->phase != Phase::idle);
  gtk_widget_set_visible(state->spinner, state->phase == Phase::checking);
  if (state->phase == Phase::checking) {
    gtk_spinner_start(GTK_SPINNER(state->spinner));
  } else {
    gtk_spinner_stop(GTK_SPINNER(state->spinner));
  }
  gtk_widget_set_visible(state->bar, state->phase == Phase::downloading);
  if (state->phase == Phase::downloading) {
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->bar),
                                  std::clamp(state->percent, 0, 100) / 100.0);
  }
  gtk_widget_set_visible(state->check, state->phase == Phase::idle);
  gtk_widget_set_visible(state->download, state->phase == Phase::available);
  gtk_widget_set_visible(state->install,
                         state->phase == Phase::downloaded && appimage);
  gtk_widget_set_visible(state->retry, state->phase == Phase::failed);
  gtk_widget_set_visible(state->close, state->phase != Phase::idle);
  gtk_button_set_label(GTK_BUTTON(state->check), tr("updateCheckNow").c_str());
  gtk_button_set_label(GTK_BUTTON(state->close), tr("updateClose").c_str());
  gtk_button_set_label(GTK_BUTTON(state->download), tr("updateDownload").c_str());
  gtk_button_set_label(GTK_BUTTON(state->install),
                       tr("updateQuitAndInstall").c_str());
  gtk_button_set_label(GTK_BUTTON(state->retry), tr("updateRetry").c_str());
}

void update_window_dismiss() {
  auto* state = g_update_window;
  if (state == nullptr) return;
  ++state->generation;
  if (state->phase != UpdatePhase::downloading) {
    state->phase = UpdatePhase::idle;
  }
  gtk_widget_set_visible(GTK_WIDGET(state->window), FALSE);
}

gboolean update_window_close_request(GtkWindow*, gpointer) {
  update_window_dismiss();
  return TRUE;  // hide, never destroy: a download keeps running
}

struct UpdateCheckOutcome {
  std::uint64_t generation{0};
  std::string status;
  std::string version;
  std::int64_t size{0};
  std::string error;
};

void update_window_start_check() {
  auto* state = g_update_window;
  auto* api_raw = Launcher::instance().api.get();
  if (state == nullptr || api_raw == nullptr) return;
  if (state->phase == UpdatePhase::downloading ||
      state->phase == UpdatePhase::checking) {
    return;
  }
  state->phase = UpdatePhase::checking;
  std::uint64_t const generation = ++state->generation;
  update_window_paint();
  std::thread([api_raw, generation]() {
    auto* outcome = new UpdateCheckOutcome;
    outcome->generation = generation;
    try {
      auto const check = api_raw->update_check(true).get();
      outcome->status = check.status;
      outcome->version = check.available_version.value_or("?");
      outcome->size = check.size_bytes.value_or(0);
      outcome->error = check.error.value_or("");
    } catch (std::exception const& e) {
      outcome->status = "error";
      outcome->error = e.what();
    } catch (...) {
      outcome->status = "error";
    }
    g_idle_add([](gpointer user_data) -> int {
      std::unique_ptr<UpdateCheckOutcome> outcome(
          static_cast<UpdateCheckOutcome*>(user_data));
      auto* state = g_update_window;
      if (state == nullptr || outcome->generation != state->generation) {
        return G_SOURCE_REMOVE;
      }
      if (outcome->status == "available") {
        state->version = outcome->version;
        state->size = outcome->size;
        state->phase = UpdatePhase::available;
      } else if (outcome->status == "up-to-date") {
        state->phase = UpdatePhase::up_to_date;
      } else if (outcome->status == "throttled") {
        state->phase = UpdatePhase::failed;
        state->error = tr("update.check-throttled");
      } else {
        state->phase = UpdatePhase::failed;
        state->error = tr_format(
            "update.check-failed",
            {outcome->error.empty() ? tr("updateUnknownError") : outcome->error});
      }
      update_window_paint();
      return G_SOURCE_REMOVE;
    }, outcome);
  }).detach();
}

struct UpdateStateOutcome {
  std::string phase;
  int percent{0};
  std::string message;
};

gboolean update_window_poll_tick(gpointer) {
  auto* state = g_update_window;
  if (state == nullptr || !state->polling) return G_SOURCE_REMOVE;
  auto* api_raw = Launcher::instance().api.get();
  if (api_raw == nullptr) {
    state->polling = false;
    return G_SOURCE_REMOVE;
  }
  // No generation guard here: Close hides the window but the download
  // keeps running, and reopening must show live progress.
  std::thread([api_raw]() {
    auto* outcome = new UpdateStateOutcome;
    try {
      auto const snapshot = api_raw->update_state().get();
      outcome->phase = snapshot.phase;
      outcome->percent = static_cast<int>(snapshot.percent);
      outcome->message = snapshot.message.value_or("");
    } catch (...) {
      outcome->phase = "error";
    }
    g_idle_add([](gpointer user_data) -> int {
      std::unique_ptr<UpdateStateOutcome> outcome(
          static_cast<UpdateStateOutcome*>(user_data));
      auto* state = g_update_window;
      if (state == nullptr || !state->polling ||
          state->phase != UpdatePhase::downloading) {
        return G_SOURCE_REMOVE;
      }
      if (outcome->phase == "downloading") {
        state->percent = outcome->percent;
        update_window_paint();
      } else if (outcome->phase == "downloaded") {
        state->polling = false;
        state->phase = UpdatePhase::downloaded;
        update_window_paint();
      } else {
        state->polling = false;
        state->phase = UpdatePhase::failed;
        state->error = outcome->message.empty()
            ? tr("update.download-failed") : outcome->message;
        update_window_paint();
      }
      return G_SOURCE_REMOVE;
    }, outcome);
  }).detach();
  return G_SOURCE_CONTINUE;
}

void update_window_start_download() {
  auto* state = g_update_window;
  auto* api_raw = Launcher::instance().api.get();
  if (state == nullptr || api_raw == nullptr ||
      state->phase != UpdatePhase::available) {
    return;
  }
  state->phase = UpdatePhase::downloading;
  state->percent = 0;
  if (!state->polling) {
    state->polling = true;
    g_timeout_add(700, update_window_poll_tick, nullptr);
  }
  update_window_paint();
  // The backend owns the transfer; a refusal (e.g. package-managed Linux)
  // surfaces through update_state as the error phase.
  std::thread([api_raw]() {
    try {
      api_raw->update_download();
    } catch (...) {
    }
  }).detach();
}

struct UpdateInstallOutcome {
  bool handoff{false};
  std::string path;
  std::string version;
  std::string sha256;
  bool rollback_allowed{false};
  std::string error;
};

void update_window_start_install() {
  auto* state = g_update_window;
  auto* api_raw = Launcher::instance().api.get();
  if (state == nullptr || api_raw == nullptr ||
      state->phase != UpdatePhase::downloaded) {
    return;
  }
  gtk_widget_set_sensitive(state->install, FALSE);
  std::thread([api_raw]() {
    auto* outcome = new UpdateInstallOutcome;
    try {
      auto const status = api_raw->run_action("update.install", "").get();
      if (status == "quit-and-install") {
        // The signed manifest remains backend-owned. Never derive a
        // handoff digest from the downloaded file itself.
        auto const snapshot = api_raw->update_state().get();
        if (snapshot.phase != "downloaded" || !snapshot.downloaded_path ||
            !snapshot.available_version || !snapshot.artifact_sha256 ||
            !snapshot.installer || *snapshot.installer != "appimage") {
          outcome->error =
              "signed AppImage install ticket is missing; check updates again";
        } else {
          outcome->handoff = true;
          outcome->path = *snapshot.downloaded_path;
          outcome->version = *snapshot.available_version;
          outcome->sha256 = *snapshot.artifact_sha256;
          outcome->rollback_allowed = snapshot.rollback_allowed;
        }
      } else {
        outcome->error = status;
      }
    } catch (std::exception const& e) {
      outcome->error = e.what();
    } catch (...) {
      outcome->error = tr("updateUnknownError");
    }
    g_idle_add([](gpointer user_data) -> int {
      std::unique_ptr<UpdateInstallOutcome> outcome(
          static_cast<UpdateInstallOutcome*>(user_data));
      auto* state = g_update_window;
      if (state == nullptr) {
        return G_SOURCE_REMOVE;
      }
      if (!outcome->handoff) {
        state->phase = UpdatePhase::failed;
        state->error = outcome->error.empty()
            ? tr("update.download-failed")
            : tr_format("update.install-failed", {outcome->error});
        gtk_widget_set_sensitive(state->install, TRUE);
        update_window_paint();
        return G_SOURCE_REMOVE;
      }
      // Main thread: the first-party helper detaches before this GTK
      // process exits; shutdown releases the SingleInstanceLease before
      // the live executable is replaced.
      try {
        auto* running_image = std::getenv("APPIMAGE");
        if (running_image == nullptr || *running_image == '\0') {
          throw std::runtime_error(
              "AppImage replacement is only available from an AppImage");
        }
        std::filesystem::path const target(running_image);
        if (!target.is_absolute() ||
            !std::filesystem::is_regular_file(target)) {
          throw std::runtime_error(
              "cannot locate the running AppImage installation");
        }
        rivet::update::AppImageHandoffOptions options;
        options.version = outcome->version;
        options.artifact_sha256 = outcome->sha256;
        options.rollback_allowed = outcome->rollback_allowed;
        rivet::update::AppImageHandoff::LaunchReplacement(
            "site.jrtx.fulcrum", std::filesystem::path(outcome->path),
            target, {}, 30, options);
        g_application_quit(G_APPLICATION(Launcher::instance().app));
      } catch (std::exception const& e) {
        state->phase = UpdatePhase::failed;
        state->error = tr_format("update.install-failed", {e.what()});
        gtk_widget_set_sensitive(state->install, TRUE);
        update_window_paint();
      }
      return G_SOURCE_REMOVE;
    }, outcome);
  }).detach();
}

void update_window_build() {
  auto* state = new UpdateWindowState;
  g_update_window = state;
  auto* window = gtk_window_new();
  state->window = GTK_WINDOW(window);
  gtk_window_set_title(GTK_WINDOW(window), "Fulcrum");
  gtk_window_set_default_size(GTK_WINDOW(window), 380, 220);
  gtk_window_set_hide_on_close(GTK_WINDOW(window), TRUE);
  g_signal_connect(window, "close-request",
                   G_CALLBACK(update_window_close_request), nullptr);
  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(box, 18);
  gtk_widget_set_margin_bottom(box, 18);
  gtk_widget_set_margin_start(box, 18);
  gtk_widget_set_margin_end(box, 18);
  auto* title = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(title), 0.0);
  gtk_label_set_wrap(GTK_LABEL(title), TRUE);
  gtk_widget_add_css_class(title, "heading");
  state->title = GTK_LABEL(title);
  auto* status = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(status), 0.0);
  gtk_label_set_wrap(GTK_LABEL(status), TRUE);
  gtk_label_set_selectable(GTK_LABEL(status), TRUE);
  gtk_accessible_update_property(GTK_ACCESSIBLE(status),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "fulcrum-update-status", -1);
  state->status = GTK_LABEL(status);
  auto* spinner = gtk_spinner_new();
  gtk_widget_set_halign(spinner, GTK_ALIGN_START);
  state->spinner = spinner;
  auto* bar = gtk_progress_bar_new();
  gtk_accessible_update_property(GTK_ACCESSIBLE(bar),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "fulcrum-update-progress", -1);
  state->bar = bar;
  auto* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);
  gtk_widget_set_margin_top(buttons, 8);
  auto make_button = [](char const* key, char const* a11y) -> GtkWidget* {
    auto* button = gtk_button_new_with_label(tr(key).c_str());
    gtk_accessible_update_property(GTK_ACCESSIBLE(button),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL, a11y, -1);
    return button;
  };
  state->retry = make_button("updateRetry", "fulcrum-update-retry");
  state->install = make_button("updateQuitAndInstall", "fulcrum-update-install");
  state->download = make_button("updateDownload", "fulcrum-update-download");
  state->check = make_button("updateCheckNow", "fulcrum-update-check");
  state->close = make_button("updateClose", "fulcrum-update-close");
  g_signal_connect(state->check, "clicked",
                   G_CALLBACK(+[](GtkButton*, gpointer) {
    update_window_start_check();
  }), nullptr);
  g_signal_connect(state->download, "clicked",
                   G_CALLBACK(+[](GtkButton*, gpointer) {
    update_window_start_download();
  }), nullptr);
  g_signal_connect(state->install, "clicked",
                   G_CALLBACK(+[](GtkButton*, gpointer) {
    update_window_start_install();
  }), nullptr);
  g_signal_connect(state->retry, "clicked",
                   G_CALLBACK(+[](GtkButton*, gpointer) {
    update_window_start_check();
  }), nullptr);
  g_signal_connect(state->close, "clicked",
                   G_CALLBACK(+[](GtkButton*, gpointer) {
    update_window_dismiss();
  }), nullptr);
  gtk_box_append(GTK_BOX(buttons), state->retry);
  gtk_box_append(GTK_BOX(buttons), state->install);
  gtk_box_append(GTK_BOX(buttons), state->download);
  gtk_box_append(GTK_BOX(buttons), state->check);
  gtk_box_append(GTK_BOX(buttons), state->close);
  gtk_box_append(GTK_BOX(box), title);
  gtk_box_append(GTK_BOX(box), status);
  gtk_box_append(GTK_BOX(box), spinner);
  gtk_box_append(GTK_BOX(box), bar);
  gtk_box_append(GTK_BOX(box), buttons);
  gtk_window_set_child(GTK_WINDOW(window), box);
  update_window_paint();
}

void show_update_window(bool check) {
  if (Launcher::instance().api == nullptr) {
    Launcher::post_status("Update check once the backend is ready…");
    return;
  }
  if (g_update_window == nullptr) update_window_build();
  if (std::getenv("FULCRUM_ACCEPTANCE") != nullptr) {
    // Offline acceptance fixture: the completed manual check, no network.
    ++g_update_window->generation;
    g_update_window->phase = UpdatePhase::up_to_date;
    gtk_window_present(g_update_window->window);
    update_window_paint();
    return;
  }
  gtk_window_present(g_update_window->window);
  if (check) update_window_start_check();
  else update_window_paint();
}

void update_window_present_available(std::string version, std::int64_t size) {
  if (g_update_window == nullptr) update_window_build();
  auto* state = g_update_window;
  if (state->phase == UpdatePhase::downloading ||
      state->phase == UpdatePhase::checking) {
    return;
  }
  state->version = std::move(version);
  state->size = size;
  state->phase = UpdatePhase::available;
  gtk_window_present(state->window);
  update_window_paint();
}

void update_window_refresh_language() {
  if (g_update_window != nullptr) update_window_paint();
}

// ---- X11 launcher monitor placement --------------------------------------
//
// The pointer-monitor summon fix originated in @linyubin's #61. X11 WMs
// otherwise tend to map a utility window onto a default monitor.

struct PanelPlacement { int x; int y; };

struct PanelSize { int width; int height; };

// 680x640 is the design ceiling, not a fixed frame. Clamping alone left
// the panel covering 85% of a 1280x800 work area, so it keeps margins: at
// most 55% of the work-area width and 65% of its height (design/DESIGN.md
// "启动窗几何与多显示器"; the macOS and Windows fits use the same shares).
// Screens with room keep the design size; the results list scrolls.
PanelSize fit_panel_size(GdkRectangle const& work) {
  return {std::min(680, work.width * 55 / 100),
          std::min(640, work.height * 65 / 100)};
}

PanelPlacement upper_quarter_placement(GdkRectangle const& work,
                                       int width, int height) {
  // GDK/X11 uses a top-left origin. Same proportion as WinUI/AppKit:
  // a quarter of the free height below the screen's usable top edge.
  return {work.x + std::max(0, work.width - width) / 2,
          work.y + std::max(0, work.height - height) / 4};
}

bool monitor_contains_point(GdkRectangle const& g, int x, int y) {
  return x >= g.x && x < g.x + g.width &&
         y >= g.y && y < g.y + g.height;
}

// _NET_WORKAREA is a virtual-desktop-wide rectangle on X11. Intersect it
// with the chosen monitor as a best-effort allowance for docks/top panels;
// a WM with no usable EWMH property falls back to full monitor geometry.
GdkRectangle usable_x11_monitor_area(Display* dpy, Window root,
                                     GdkRectangle const& monitor) {
  Atom atom = XInternAtom(dpy, "_NET_WORKAREA", True);
  if (atom == None) return monitor;
  Atom type = None;
  int format = 0;
  unsigned long count = 0, bytes_left = 0;
  unsigned char* bytes = nullptr;
  GdkRectangle work = monitor;
  int status = XGetWindowProperty(dpy, root, atom, 0, 4, False, XA_CARDINAL,
                                  &type, &format, &count, &bytes_left, &bytes);
  if (status == Success && type == XA_CARDINAL && format == 32 &&
      count >= 4 && bytes != nullptr) {
    auto* values = reinterpret_cast<unsigned long const*>(bytes);
    // Xlib stores 32-bit property values in native unsigned longs.
    auto wx = static_cast<std::int32_t>(values[0]);
    auto wy = static_cast<std::int32_t>(values[1]);
    auto ww = static_cast<std::uint32_t>(values[2]);
    auto wh = static_cast<std::uint32_t>(values[3]);
    if (ww > 0 && wh > 0 && ww <= 100000 && wh <= 100000) {
      auto left = std::max<std::int64_t>(monitor.x, wx);
      auto top = std::max<std::int64_t>(monitor.y, wy);
      auto right = std::min<std::int64_t>(
          static_cast<std::int64_t>(monitor.x) + monitor.width,
          static_cast<std::int64_t>(wx) + ww);
      auto bottom = std::min<std::int64_t>(
          static_cast<std::int64_t>(monitor.y) + monitor.height,
          static_cast<std::int64_t>(wy) + wh);
      if (right > left && bottom > top) {
        work = {static_cast<int>(left), static_cast<int>(top),
                static_cast<int>(right - left),
                static_cast<int>(bottom - top)};
      }
    }
  }
  if (bytes != nullptr) XFree(bytes);
  return work;
}

void place_panel_on_pointer_monitor(GtkWindow* panel) {
  auto* widget = GTK_WIDGET(panel);
  auto* display = gtk_widget_get_display(widget);
  // Wayland forbids client-controlled toplevel coordinates.
  if (!GDK_IS_X11_DISPLAY(display)) return;
  auto* xdisplay = gdk_x11_display_get_xdisplay(GDK_X11_DISPLAY(display));
  Window root = DefaultRootWindow(xdisplay);
  GListModel* monitors = gdk_display_get_monitors(display);
  if (monitors == nullptr) return;
  guint count = g_list_model_get_n_items(monitors);
  if (count == 0) return;  // monitor hotplug / session shutdown

  Window root_return = None, child = None;
  int pointer_x = 0, pointer_y = 0, win_x = 0, win_y = 0;
  unsigned int mask = 0;
  bool pointer_known =
      XQueryPointer(xdisplay, root, &root_return, &child,
                    &pointer_x, &pointer_y, &win_x, &win_y, &mask) != False;

  GdkMonitor* chosen = nullptr;
  for (guint i = 0; i < count; ++i) {
    auto* candidate = GDK_MONITOR(g_list_model_get_item(monitors, i));
    if (candidate == nullptr) continue;
    GdkRectangle g{};
    gdk_monitor_get_geometry(candidate, &g);
    if (pointer_known && monitor_contains_point(g, pointer_x, pointer_y)) {
      chosen = candidate;
      break;
    }
    g_object_unref(candidate);
  }
  if (chosen == nullptr)
    chosen = GDK_MONITOR(g_list_model_get_item(monitors, 0));
  if (chosen == nullptr) return;

  GdkRectangle geometry{};
  gdk_monitor_get_geometry(chosen, &geometry);
  g_object_unref(chosen);  // get_item() owns a reference for every summon
  if (geometry.width <= 0 || geometry.height <= 0) return;

  GdkRectangle usable = usable_x11_monitor_area(xdisplay, root, geometry);
  // Reset the preferred dimensions on each summon. Do not strand a tiny
  // window after switching from a small monitor to a larger one.
  PanelSize const size = fit_panel_size(usable);
  gtk_window_set_default_size(panel, size.width, size.height);
  PanelPlacement pos = upper_quarter_placement(usable, size.width, size.height);

  auto* surface = gtk_native_get_surface(GTK_NATIVE(panel));
  if (!GDK_IS_X11_SURFACE(surface)) return;
  Window xid = gdk_x11_surface_get_xid(GDK_X11_SURFACE(surface));
  XSizeHints hints{};
  long supplied = 0;
  if (!XGetWMNormalHints(xdisplay, xid, &hints, &supplied))
    hints.flags = 0;
  hints.flags |= USPosition;
  hints.x = pos.x;
  hints.y = pos.y;
  XSetWMNormalHints(xdisplay, xid, &hints);
  XMoveWindow(xdisplay, xid, pos.x, pos.y);
  XFlush(xdisplay);
}

// ---- X11 global hotkey ----------------------------------------------------


std::string install_x11_hotkey(GdkDisplay* display) {
  if (GDK_IS_X11_DISPLAY(display) == 0) {
    return "Wayland session: compositors do not allow global key grabs. "
           "Bind `fulcrum --toggle` to a key in your compositor settings.";
  }
  std::string note = g_x11_hotkey.start(
      gdk_display_get_name(display), [](Time time) {
        Launcher::instance().toggle(static_cast<guint32>(time));
      });
  if (!note.empty()) return note;
  // Use the root on a dedicated connection so the grab works even when
  // the launcher is hidden, without changing GDK's event mask or queue.
  return apply_x11_hotkey("alt+space");
}

// ---- single instance via the first-party rivet lease ----------------------
// Desktop relaunch forwards --show and always presents the resident panel.
// Only explicit `fulcrum --toggle` forwards --toggle to toggle visibility.
// The handler runs on the lease's watcher thread and posts to the GTK loop.

gboolean activate_on_main(gpointer) {
  // A forwarded --toggle is a real toggle: the compositor keybinding
  // summons a hidden panel and dismisses a visible one.
  Launcher::instance().toggle();
  return G_SOURCE_REMOVE;
}

gboolean show_on_main(gpointer) {
  Launcher::instance().show();
  return G_SOURCE_REMOVE;
}

// A normal desktop launch should show the resident window. Only an explicit
// --toggle command (e.g. a Wayland compositor shortcut) should hide it.
bool toggle_activation_requested(std::vector<std::string> const& arguments) {
  return std::find(arguments.begin(), arguments.end(), "--toggle") !=
         arguments.end();
}

void on_activation(std::vector<std::string> arguments) {
  g_idle_add(toggle_activation_requested(arguments)
                 ? activate_on_main : show_on_main,
             nullptr);
}

// ---- tray ------------------------------------------------------------------
// The visible way back after the panel hides. Without it, a launcher that
// closes to the background has no summon affordance a mouse user can find:
// the portal hotkey needs a consent prompt that is easy to miss, and
// `fulcrum --toggle` is a terminal incantation.

void tray_quit() {
  // A real quit: GApplication's shutdown path stops the backend, and the
  // shutdown hook keeps SIGTERM equivalent.
  g_application_quit(G_APPLICATION(Launcher::instance().app));
}

// GNOME's stock "activate window menu" binding is <Alt>space — the
// compositor consumes the summon key before the portal shortcut ever
// sees it, and the consent UI does not resolve that for the user. When
// the active trigger is Alt+Space and that keybinding still exists, the
// tray grows a one-click fix (clearing the stock binding) instead of
// leaving every GNOME user to debug gsettings on their own.
bool trigger_is_alt_space(std::string const& trigger) {
  std::string lowered = trigger;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return lowered.find("alt") != std::string::npos &&
         lowered.find("space") != std::string::npos;
}

std::string gnome_window_menu_binding() {
  std::string out;
  FILE* gsettings = popen(
      "gsettings get org.gnome.desktop.wm.keybindings "
      "activate-window-menu 2>/dev/null",
      "r");
  if (gsettings == nullptr) {
    return out;
  }
  char buf[256];
  while (fgets(buf, sizeof buf, gsettings) != nullptr) {
    out += buf;
  }
  pclose(gsettings);
  return out;
}

void rebuild_tray_menu();

void tray_fix_hotkey() {
  int const rc = std::system(
      "gsettings set org.gnome.desktop.wm.keybindings "
      "activate-window-menu \"[]\"");
  Launcher& launcher = Launcher::instance();
  if (rc == 0) {
    launcher.hotkey_shadowed = false;
    if (launcher.hotkey_fix != nullptr) {
      gtk_widget_set_visible(GTK_WIDGET(launcher.hotkey_fix), FALSE);
    }
    rebuild_tray_menu();
    Launcher::post_status(tr("hotkeyFreed"));
  } else {
    Launcher::post_status(std::string("gsettings failed (rc=") +
                          std::to_string(rc) + ")");
  }
}

// Manual update check from the tray: opens the dedicated update window
// (update-flow parity with macOS/Windows) and starts a manual check, which
// bypasses the 4-hour family throttle. Download and install live in the
// same window, reached from any entry point.
void tray_check_updates() {
  show_update_window(true);
}

std::vector<rivet::system::TrayMenuItem> tray_menu_items() {
  std::vector<rivet::system::TrayMenuItem> items;
  items.push_back(rivet::system::TrayMenuItem(
      tr("trayOpen"), [] { Launcher::instance().show(); }));
  items.push_back(rivet::system::TrayMenuItem(
      tr("traySettings"), [] { show_settings_window(); }));
  // Check for Updates and About always travel together (the family rule).
  items.push_back(rivet::system::TrayMenuItem(
      tr("trayCheckUpdates"), [] { tray_check_updates(); }));
  items.push_back(rivet::system::TrayMenuItem(
      tr("trayAbout"),
      [] { show_about_dialog(Launcher::instance().window); }));
  if (Launcher::instance().hotkey_shadowed) {
    items.push_back(rivet::system::TrayMenuItem(
        tr("trayFixHotkey"), [] { tray_fix_hotkey(); }));
  }
  items.push_back(rivet::system::TrayMenuItem(
      rivet::system::TrayMenuItem::Type::separator));
  items.push_back(
      rivet::system::TrayMenuItem(tr("trayQuit"), [] { tray_quit(); }));
  return items;
}

void rebuild_tray_menu() {
  Launcher& launcher = Launcher::instance();
  if (launcher.tray) {
    launcher.tray->set_menu(tray_menu_items());
  }
}

// Runs once the portal bind names the compositor's chosen combo: a
// GNOME session whose window menu still owns Alt+Space keeps the summon
// dead no matter how correct the binding is — say so, and put the fix
// one tray click away.
void check_hotkey_shadowing(std::string const& trigger) {
  if (!trigger_is_alt_space(trigger)) {
    return;
  }
  std::string const binding = gnome_window_menu_binding();
  std::string lowered = binding;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  if (lowered.find("alt") == std::string::npos ||
      lowered.find("space") == std::string::npos) {
    return;
  }
  Launcher& launcher = Launcher::instance();
  launcher.hotkey_shadowed = true;
  if (launcher.hotkey_fix != nullptr) {
    gtk_widget_set_visible(GTK_WIDGET(launcher.hotkey_fix), TRUE);
  }
  rebuild_tray_menu();
  Launcher::post_status(tr_format("hotkeyShadowed", {trigger}));
}

// The menu is composed at tray construction with the startup language; the
// backend `language` setting lands shortly after boot and re-composes it
// (set_menu replaces the whole menu), so the labels never need a restart.
void apply_tray_language() {
  Launcher& launcher = Launcher::instance();
  std::string const active =
      (g_i18n_active.load(std::memory_order_relaxed) == 0) ? "zh" : "en";
  if (!launcher.tray || active == launcher.tray_menu_lang) {
    return;
  }
  launcher.tray_menu_lang = active;
  rebuild_tray_menu();
}

void build_tray() {
  try {
    if (!rivet::system::TrayIcon::available()) {
      return;
    }
    Launcher& launcher = Launcher::instance();
    launcher.tray = std::make_unique<rivet::system::TrayIcon>(
        rivet_app::kIdentifier, "Fulcrum", "fulcrum");
    launcher.tray->set_tooltip(
        "Fulcrum", tr("aboutContent").c_str());
    launcher.tray->set_menu(tray_menu_items());
    launcher.tray->set_activation_handler(
        [] { Launcher::instance().show(); });
  } catch (std::exception const& error) {
    // A session that cannot host the tray is supported: the portal hotkey
    // and `fulcrum --toggle` stay the summon paths.
    std::cerr << "fulcrum: tray unavailable: " << error.what() << "\n";
  }
}

// ---- shutdown hook ---------------------------------------------------------

void install_shutdown_hook_once() {
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;
  try {
    rivet::system::InstallShutdownHook(
        [] { Launcher::instance().stop_backend(); });
  } catch (std::exception const& error) {
    std::cerr << "fulcrum: shutdown hook unavailable: " << error.what()
              << "\n";
  }
}

// ---- GTK wiring -----------------------------------------------------------

// WM close = hide, exactly like the hosts' close buttons. Returning TRUE
// keeps GTK's hide-on-close default out of the picture: its path was
// observed killing the X11 surface behind GDK's back, after which the
// resident launcher could never be presented again.
gboolean on_close_request(GtkWindow*, gpointer) {
  Launcher::instance().hide();
  return TRUE;
}

void on_search_changed(GtkSearchEntry*, gpointer) {
  Launcher& launcher = Launcher::instance();
  if (launcher.search_source != 0) {
    g_source_remove(launcher.search_source);
  }
  // Debounce fast typing.
  launcher.search_source =
      g_timeout_add(120, [](gpointer) -> int {
        Launcher& l = Launcher::instance();
        l.search_source = 0;
        l.run_search(gtk_editable_get_text(GTK_EDITABLE(l.search)));
        return G_SOURCE_REMOVE;
      }, nullptr);
}

void on_row_activated(GtkListBox*, GtkListBoxRow*, gpointer) {
  Launcher::instance().run_selected();
}

// IME composition guard signal: GTK4 keeps the IM context inside the
// entry's internal GtkText and exposes no accessor, so preedit tracking
// hooks the GtkText::preedit-changed signal (found by walking the search
// entry's children). A non-empty preedit means the IM owns the keys.
static void on_query_preedit_changed(GtkText*, gchar* preedit, gpointer) {
  Launcher::instance().query_composing = preedit != nullptr && *preedit != '\0';
}

static void on_actions_back_clicked(GtkButton*, gpointer) {
  Launcher::instance().close_actions();
}

gboolean on_key_pressed(GtkEventControllerKey*, guint keyval, guint,
                        GdkModifierType state, gpointer) {
  // IME composition guard (docs/cross-platform-interaction.md): while the
  // query IM context holds preedit, Enter/arrows/Tab/Esc belong to the
  // input method — no execution, no selection move, no layer change; the
  // first Esc cancels only the composition. Pass the key through so the
  // IME and the entry handle it.
  if (Launcher::instance().query_composing) {
    const bool layer_key =
        keyval == GDK_KEY_Return || keyval == GDK_KEY_ISO_Enter ||
        keyval == GDK_KEY_Up || keyval == GDK_KEY_Down ||
        keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab ||
        keyval == GDK_KEY_Escape ||
        (keyval == GDK_KEY_k && (state & GDK_CONTROL_MASK) != 0);
    if (layer_key) {
      return FALSE;
    }
  }
  // Ctrl+K equivalent: the selected row's secondary actions.
  if (keyval == GDK_KEY_k && (state & GDK_CONTROL_MASK) != 0) {
    Launcher::instance().show_actions_for_selection();
    return TRUE;
  }
  // Ctrl+Y reveals the selected masked clipboard row (local-only RPC).
  if (keyval == GDK_KEY_y && (state & GDK_CONTROL_MASK) != 0) {
    Launcher::instance().reveal_sensitive_selection();
    return TRUE;
  }
  switch (keyval) {
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab: {
      Launcher& launcher = Launcher::instance();
      // Shift+Tab (ISO_Left_Tab): back to the query, strictly reversed.
      if (keyval == GDK_KEY_ISO_Left_Tab) {
        gtk_widget_grab_focus(GTK_WIDGET(launcher.search));
        return TRUE;
      }
      // Tab completes when a row offers it, else focus moves to the
      // results list — the list is the second tab stop as a whole
      // (design v2 focus contract).
      if (!launcher.actions_mode && !launcher.rows.empty()) {
        GtkListBoxRow* selected =
            gtk_list_box_get_selected_row(launcher.results);
        int index = selected != nullptr ? gtk_list_box_row_get_index(selected) : 0;
        if (index < 0 || index >= static_cast<int>(launcher.rows.size())) {
          index = 0;
        }
        gtk_editable_set_text(GTK_EDITABLE(launcher.search),
                              launcher.rows[static_cast<std::size_t>(index)]
                                  .title.c_str());
        gtk_editable_set_position(GTK_EDITABLE(launcher.search), -1);
        return TRUE;
      }
      gtk_widget_grab_focus(GTK_WIDGET(launcher.results));
      return TRUE;
    }
    case GDK_KEY_Escape:
      // Inside the action panel: back to the search rows first.
      if (Launcher::instance().actions_mode) {
        Launcher::instance().close_actions();
        return TRUE;
      }
      // A plugin detail panel or a finished AI answer dismisses first.
      if (gtk_widget_get_visible(Launcher::instance().detail_panel)) {
        Launcher::instance().dismiss_detail_panel();
        return TRUE;
      }
      if (gtk_widget_get_visible(Launcher::instance().ai_panel)) {
        Launcher::instance().dismiss_ai_panel();
        return TRUE;
      }
      // Alfred/Raycast convention: clear the query first, hide only when
      // it is already empty.
      if (const gchar* text =
              gtk_editable_get_text(GTK_EDITABLE(Launcher::instance().search));
          text != nullptr && *text != '\0') {
        gtk_editable_set_text(GTK_EDITABLE(Launcher::instance().search), "");
        Launcher::instance().run_search("");
      } else {
        Launcher::instance().hide();
      }
      return TRUE;
    case GDK_KEY_Down:
    case GDK_KEY_Up: {
      Launcher& launcher = Launcher::instance();
      int const delta = keyval == GDK_KEY_Down ? 1 : -1;
      GtkListBoxRow* current = gtk_list_box_get_selected_row(launcher.results);
      int index = current != nullptr ? gtk_list_box_row_get_index(current) : 0;
      index = std::clamp(index + delta, 0,
                         static_cast<int>(launcher.rows.size()) - 1);
      if (!launcher.rows.empty()) {
        gtk_list_box_select_row(launcher.results,
                                gtk_list_box_get_row_at_index(launcher.results,
                                                              index));
        launcher.scroll_to_selection();
      }
      return TRUE;
    }
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      Launcher::instance().run_selected();
      return TRUE;
    default:
      return FALSE;
  }
}

void on_window_active_changed(GObject* obj, GParamSpec*, gpointer) {
  auto* gtk_window = GTK_WINDOW(obj);
  auto& launcher = Launcher::instance();
  launcher.cancel_focus_loss();
  if (gtk_window_is_active(gtk_window)) {
    launcher.activated_since_show = true;
    gtk_widget_grab_focus(GTK_WIDGET(launcher.search));
    return;
  }
  if (!gtk_widget_get_visible(GTK_WIDGET(gtk_window)) ||
      !launcher.activated_since_show) return;
  // Allow focus handoffs to settle before dismissing the launcher.
  launcher.focus_loss_source = g_timeout_add(150, [](gpointer) -> gboolean {
    auto& launcher = Launcher::instance();
    if (gtk_widget_get_visible(GTK_WIDGET(launcher.window)) &&
        !gtk_window_is_active(launcher.window)) {
      // Window-manager moves can temporarily take focus while a button is held.
      auto* display = gtk_widget_get_display(GTK_WIDGET(launcher.window));
      auto* seat = gdk_display_get_default_seat(display);
      auto* pointer = seat != nullptr ? gdk_seat_get_pointer(seat) : nullptr;
      GdkModifierType modifiers = GdkModifierType(0);
      if (pointer != nullptr) {
        gdk_surface_get_device_position(
            gtk_native_get_surface(GTK_NATIVE(launcher.window)), pointer,
            nullptr, nullptr, &modifiers);
      }
      if ((modifiers & (GDK_BUTTON1_MASK | GDK_BUTTON2_MASK | GDK_BUTTON3_MASK)) != 0)
        return G_SOURCE_CONTINUE;
      auto* windows = gtk_window_get_toplevels();
      for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
        auto* candidate = GTK_WINDOW(g_list_model_get_item(windows, i));
        bool owned_active = false;
        if (gtk_window_is_active(candidate)) {
          for (auto* parent = gtk_window_get_transient_for(candidate);
               parent != nullptr; parent = gtk_window_get_transient_for(parent)) {
            if (parent == launcher.window) owned_active = true;
          }
        }
        g_object_unref(candidate);
        // Recheck when a launcher-owned dialog loses focus as well.
        if (owned_active) return G_SOURCE_CONTINUE;
      }
      launcher.focus_loss_source = 0;
      launcher.hide();
    }
    launcher.focus_loss_source = 0;
    return G_SOURCE_REMOVE;
  }, nullptr);
}

bool g_login_background_start = false;

void on_activate(GtkApplication* app, gpointer) {
  Launcher& launcher = Launcher::instance();
  launcher.app = app;  // tray quit routes through GApplication's shutdown
  // Deliberately no Restart/Quit actions in the search panel. Unlike
  // persistent tool windows, the launcher is a transient keyboard surface;
  // native resident menus already provide Quit, Settings and Updates.
  // GNOME hotkey recovery stays available as a tray item or inline notice.

  // The panel hides on focus loss; without a hold, GTK would treat the
  // hidden window as "last window closed" and shut the whole app down.
  g_application_hold(G_APPLICATION(app));

  auto* window = gtk_application_window_new(app);
  launcher.window = GTK_WINDOW(window);
  gtk_window_set_title(launcher.window, "Fulcrum");
  // Stable 680 x 640 panel ceiling; the result list scrolls when needed.
  // place_panel_on_pointer_monitor fits it to the work area on every summon.
  gtk_window_set_default_size(launcher.window, 680, 640);
  gtk_window_set_resizable(launcher.window, FALSE);
  // Chromeless like the other hosts: the panel content IS the window, and
  // the stylesheet rounds it. Close still hides (below) via Esc / the tray.
  gtk_window_set_decorated(launcher.window, FALSE);
  apply_panel_css();
  // The WM's close button HIDES, exactly like the other hosts: the
  // process stays resident (hotkey / --toggle / tray summon it back).
  // A close that quits would strand the user with no way to summon.
  // Handled explicitly: letting GTK's hide-on-close default run lets the
  // X11 surface die behind GDK's back ("GdkSurface unexpectedly
  // destroyed"), leaving a resident process with a window that can never
  // be presented again.
  gtk_window_set_hide_on_close(launcher.window, TRUE);
  g_signal_connect(window, "close-request",
                   G_CALLBACK(on_close_request), nullptr);
  gtk_widget_add_css_class(window, "fulcrum-window");

  auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  auto* search = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search),
                                        "Search apps, clipboard, snippets, the web…");
  gtk_accessible_update_property(GTK_ACCESSIBLE(search),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "FulcrumQuery", -1);
  gtk_widget_set_margin_top(search, 10);
  gtk_widget_set_margin_start(search, 10);
  gtk_widget_set_margin_end(search, 10);
  launcher.search = GTK_SEARCH_ENTRY(search);
  gtk_widget_set_hexpand(search, TRUE);
  auto* search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append(GTK_BOX(search_row), search);

  auto* status = gtk_label_new("Starting embedded Racket CS…");
  gtk_label_set_xalign(GTK_LABEL(status), 0.0);
  gtk_label_set_selectable(GTK_LABEL(status), TRUE);
  gtk_widget_add_css_class(status, "fulcrum-status");
  gtk_widget_set_margin_start(status, 12);
  gtk_widget_set_margin_top(status, 6);
  launcher.status = GTK_LABEL(status);

  auto* hotkey_fix = gtk_button_new_with_label(tr("trayFixHotkey").c_str());
  gtk_widget_set_visible(hotkey_fix, FALSE);
  gtk_widget_set_halign(hotkey_fix, GTK_ALIGN_START);
  gtk_widget_set_margin_start(hotkey_fix, 12);
  gtk_widget_set_margin_top(hotkey_fix, 4);
  g_signal_connect(hotkey_fix, "clicked",
                   G_CALLBACK(+[](GtkButton*, gpointer) { tray_fix_hotkey(); }),
                   nullptr);
  launcher.hotkey_fix = GTK_BUTTON(hotkey_fix);

  // Live AI answer panel: hidden until the backend pushes the ai-stream
  // state, then shows the growing answer (GTK propagates the natural
  // height so short answers never reserve dead space).
  auto* ai_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_visible(ai_panel, FALSE);
  gtk_widget_set_margin_start(ai_panel, 12);
  gtk_widget_set_margin_end(ai_panel, 12);
  gtk_widget_set_margin_top(ai_panel, 4);
  auto* ai_phase = gtk_label_new("AI is answering…");
  gtk_label_set_xalign(GTK_LABEL(ai_phase), 0.0);
  gtk_widget_add_css_class(ai_phase, "fulcrum-slot-header");
  auto* ai_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ai_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height(
      GTK_SCROLLED_WINDOW(ai_scroll), 170);
  gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(ai_scroll), TRUE);
  auto* ai_text = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(ai_text), TRUE);
  gtk_label_set_xalign(GTK_LABEL(ai_text), 0.0);
  gtk_label_set_selectable(GTK_LABEL(ai_text), TRUE);
  // 12.5 regular per the shared typography; the old monospace drifted from
  // the macOS/WinUI hosts.
  gtk_widget_add_css_class(ai_text, "fulcrum-body");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(ai_scroll), ai_text);
  gtk_box_append(GTK_BOX(ai_panel), ai_phase);
  gtk_box_append(GTK_BOX(ai_panel), ai_scroll);
  launcher.ai_panel = ai_panel;
  launcher.ai_phase = GTK_LABEL(ai_phase);
  launcher.ai_text = GTK_LABEL(ai_text);

  // FPP2 plugin detail panel: same slot, static payload (text + a
  // metadata block).
  auto* detail_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_visible(detail_panel, FALSE);
  gtk_widget_set_margin_start(detail_panel, 12);
  gtk_widget_set_margin_end(detail_panel, 12);
  gtk_widget_set_margin_top(detail_panel, 4);
  auto* detail_header = gtk_label_new("Details");
  gtk_label_set_xalign(GTK_LABEL(detail_header), 0.0);
  gtk_widget_add_css_class(detail_header, "fulcrum-slot-header");
  auto* detail_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(detail_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height(
      GTK_SCROLLED_WINDOW(detail_scroll), 180);
  gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(detail_scroll), TRUE);
  auto* detail_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  auto* detail_text = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(detail_text), TRUE);
  gtk_label_set_xalign(GTK_LABEL(detail_text), 0.0);
  gtk_label_set_selectable(GTK_LABEL(detail_text), TRUE);
  gtk_widget_add_css_class(detail_text, "fulcrum-body");
  auto* detail_meta = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(detail_meta), TRUE);
  gtk_label_set_xalign(GTK_LABEL(detail_meta), 0.0);
  gtk_label_set_selectable(GTK_LABEL(detail_meta), TRUE);
  gtk_widget_add_css_class(detail_meta, "dim-label");
  gtk_box_append(GTK_BOX(detail_box), detail_text);
  gtk_box_append(GTK_BOX(detail_box), detail_meta);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(detail_scroll),
                                detail_box);
  gtk_box_append(GTK_BOX(detail_panel), detail_header);
  gtk_box_append(GTK_BOX(detail_panel), detail_scroll);
  launcher.detail_panel = detail_panel;
  launcher.detail_text = GTK_LABEL(detail_text);
  launcher.detail_meta = GTK_LABEL(detail_meta);

  auto* scrolled = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  auto* results = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(results), GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(results), FALSE);
  launcher.results = GTK_LIST_BOX(results);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), results);

  auto* hint = gtk_label_new("↑↓ navigate · ↵ run · esc hide — Fulcrum");
  launcher.navigation_hint = GTK_LABEL(hint);
  gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
  gtk_widget_add_css_class(hint, "fulcrum-hint");
  gtk_widget_set_margin_start(hint, 12);
  gtk_widget_set_margin_top(hint, 4);
  gtk_widget_set_margin_bottom(hint, 8);

  // Actions-layer mode bar: esc pill (the layer's exit), the row it acts
  // on, and the layer badge. Hidden until ⌘K opens the actions layer.
  auto* modebar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_visible(modebar, FALSE);
  gtk_widget_add_css_class(modebar, "fulcrum-modebar");
  gtk_widget_set_margin_start(modebar, 12);
  gtk_widget_set_margin_end(modebar, 12);
  gtk_widget_set_margin_top(modebar, 2);
  auto* back_btn = gtk_button_new();
  auto* back_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_append(GTK_BOX(back_box),
                 gtk_image_new_from_icon_name("go-previous-symbolic"));
  gtk_box_append(GTK_BOX(back_box), gtk_label_new("esc"));
  gtk_button_set_child(GTK_BUTTON(back_btn), back_box);
  gtk_widget_add_css_class(back_btn, "flat");
  gtk_widget_set_tooltip_text(back_btn, "esc");
  gtk_widget_set_valign(back_btn, GTK_ALIGN_CENTER);
  g_signal_connect(back_btn, "clicked",
                   G_CALLBACK(on_actions_back_clicked), nullptr);
  auto* mode_title = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(mode_title), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(mode_title), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(mode_title, TRUE);
  gtk_widget_add_css_class(mode_title, "fulcrum-mode-title");
  auto* mode_badge = gtk_label_new("");
  gtk_widget_add_css_class(mode_badge, "fulcrum-badge");
  gtk_widget_set_valign(mode_badge, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(modebar), back_btn);
  gtk_box_append(GTK_BOX(modebar), mode_title);
  gtk_box_append(GTK_BOX(modebar), mode_badge);
  launcher.actions_modebar = modebar;
  launcher.actions_mode_title = GTK_LABEL(mode_title);
  launcher.actions_mode_badge = GTK_LABEL(mode_badge);

  // IME composition guard source: the entry's internal GtkText reports
  // preedit changes (docs/cross-platform-interaction.md). GTK4 exposes no
  // accessor for the IM context itself; the GtkText child is the hook.
  for (GtkWidget* child = gtk_widget_get_first_child(search);
       child != nullptr; child = gtk_widget_get_next_sibling(child)) {
    if (GTK_IS_TEXT(child)) {
      g_signal_connect(child, "preedit-changed",
                       G_CALLBACK(on_query_preedit_changed), nullptr);
      break;
    }
  }

  gtk_box_append(GTK_BOX(root), search_row);
  gtk_box_append(GTK_BOX(root), modebar);
  gtk_box_append(GTK_BOX(root), status);
  gtk_box_append(GTK_BOX(root), hotkey_fix);
  gtk_box_append(GTK_BOX(root), ai_panel);
  gtk_box_append(GTK_BOX(root), detail_panel);
  gtk_box_append(GTK_BOX(root), scrolled);
  gtk_box_append(GTK_BOX(root), hint);
  gtk_window_set_child(launcher.window, root);
  set_i18n_language(g_i18n_active.load() == 0 ? "zh" : "en");

  g_signal_connect(search, "search-changed",
                   G_CALLBACK(on_search_changed), nullptr);
  g_signal_connect(results, "row-activated",
                   G_CALLBACK(on_row_activated), nullptr);

  auto* controller = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
  g_signal_connect(controller, "key-pressed", G_CALLBACK(on_key_pressed),
                   nullptr);
  gtk_widget_add_controller(window, controller);

  g_signal_connect(window, "notify::is-active",
                   G_CALLBACK(on_window_active_changed), nullptr);

  // Realize once so the XID exists, then install the overlay chrome (X11
  // only, through EWMH) and the platform hotkey. Wayland compositors own
  // those decisions; the status message covers that honestly.
  gtk_widget_realize(window);
  if (GDK_IS_X11_DISPLAY(gtk_widget_get_display(window))) {
    Display* dpy =
        gdk_x11_display_get_xdisplay(gtk_widget_get_display(window));
    Window xid = gdk_x11_surface_get_xid(
        gtk_native_get_surface(GTK_NATIVE(window)));
    Atom wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    Atom skip_taskbar = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    Atom wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom utility = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    XChangeProperty(dpy, xid, wm_window_type, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&utility), 1);
    XChangeProperty(dpy, xid, wm_state, XA_ATOM, 32, PropModeAppend,
                    reinterpret_cast<unsigned char*>(&above), 1);
    XChangeProperty(dpy, xid, wm_state, XA_ATOM, 32, PropModeAppend,
                    reinterpret_cast<unsigned char*>(&skip_taskbar), 1);
  }
  // Wayland must use the portal. On X11 the native grab is authoritative:
  // stock Ubuntu exposes the portal even when its GlobalShortcuts backend
  // cannot bind, which previously made the tested X11 fallback unreachable.
  if (!GDK_IS_X11_DISPLAY(gtk_widget_get_display(window)) &&
      rivet::system::GlobalShortcuts::available()) {
    // The object must outlive the async bind; it owns the portal session.
    g_portal_shortcuts = std::make_unique<rivet::system::GlobalShortcuts>();
    g_portal_shortcuts->set_activation_handler(
        [](std::string const&, std::uint64_t, std::string const&) {
          Launcher::instance().toggle();
        });
    g_portal_shortcuts->bind(
        {rivet::system::GlobalShortcutSpec{
            "summon", "Summon Fulcrum", "ALT+space"}},
        std::string(),
        [](rivet::system::GlobalShortcutBindResult const& result) {
          switch (result.status) {
            case rivet::system::GlobalShortcutBindStatus::cancelled:
              Launcher::post_hotkey_diagnostic(
                  "Hotkey permission was dismissed. Reopen Fulcrum and allow "
                  "the shortcut, or bind `fulcrum --toggle` in Settings.");
              check_hotkey_shadowing("Alt+Space");
              // A login item stays resident even when portal consent is
              // unavailable; interactive starts still show the recovery UI.
              if (!g_login_background_start) {
                g_idle_add(show_on_main, nullptr);
              }
              break;
            case rivet::system::GlobalShortcutBindStatus::failed: {
              // Name this exact executable in the workaround so the
              // suggested command works on every install shape (deb ships
              // /opt/fulcrum/RivetHost, not a `fulcrum` on PATH).
              std::string toggle_cmd{"fulcrum --toggle"};
              std::array<char, 4096> exe_buf{};
              ssize_t exe_len =
                  readlink("/proc/self/exe", exe_buf.data(), exe_buf.size() - 1);
              if (exe_len > 0) {
                exe_buf[exe_len] = '\0';
                toggle_cmd =
                    std::string(exe_buf.data(), static_cast<size_t>(exe_len)) +
                    " --toggle";
              }
              Launcher::post_hotkey_diagnostic(
                  result.message.find("An app id is required") != std::string::npos
                  ? "GNOME denied the global shortcut because this process has "
                    "no recognized desktop app identity. In Settings > Keyboard "
                    "> Custom Shortcuts, bind `" + toggle_cmd +
                    "` as a workaround."
                  : "Portal hotkey failed: " + result.message +
                    ". Bind `" + toggle_cmd + "` in desktop Settings.");
              check_hotkey_shadowing("Alt+Space");
              // A login item stays resident even when portal consent is
              // unavailable; interactive starts still show the recovery UI.
              if (!g_login_background_start) {
                g_idle_add(show_on_main, nullptr);
              }
              break;
            }
            case rivet::system::GlobalShortcutBindStatus::bound:
              // The compositor owns the final combo; say what it chose —
              // and on GNOME check that the stock window menu is not
              // shadowing the very same combo.
              if (!result.bindings.empty() &&
                  !result.bindings[0].trigger_description.empty()) {
                std::string const trigger =
                    result.bindings[0].trigger_description;
                Launcher::post_hotkey_bound("Summon hotkey: " + trigger);
                check_hotkey_shadowing(trigger);
              }
              break;
            default:
              break;
          }
        });
    launcher.start_backend();
  } else {
    std::string hotkey_note = install_x11_hotkey(gtk_widget_get_display(window));
    if (!hotkey_note.empty()) {
      launcher.set_status(hotkey_note);
    }
    launcher.start_backend();
  }
  // Open on launch. A launcher that starts hidden is an invisible app on
  // every session where the summon path is not yet alive — Wayland before
  // the portal consent is granted, after a reboot, a rejected grab. The
  // panel is one Esc (or focus loss) away from hidden again.
  char const* acceptance_hotkey = std::getenv("FULCRUM_ACCEPTANCE_HOTKEY");
  if (acceptance_hotkey != nullptr && std::string(acceptance_hotkey) == "1") {
    // Product acceptance drives the same main-loop activation handoff used by
    // portal/X11 callbacks. Starting hidden makes this a real summon test;
    // the delay lets AT-SPI attach before the panel is presented.
    launcher.hide();
    g_timeout_add(400, [](gpointer) -> gboolean {
      activate_on_main(nullptr);
      return G_SOURCE_REMOVE;
    }, nullptr);
  } else if (g_login_background_start) {
    launcher.hide();
  } else {
    launcher.show();
  }

  // The tray rides last: menus render with the i18n table loaded in main,
  // and the language arrival re-composes them (apply_tray_language).
  build_tray();
  // Report a failed or interrupted process-external AppImage transaction
  // on the next working launch, rather than silently hiding the error.
  if (auto const* image = std::getenv("APPIMAGE"); image && *image) {
    try {
      rivet::update::AppImageHandoff::CleanupStaleAttempts(
          "site.jrtx.fulcrum");
      if (auto previous = rivet::update::AppImageHandoff::ConsumeFailure(
              "site.jrtx.fulcrum")) {
        launcher.set_status(
            "Previous AppImage update failed: " + previous->message);
      }
    } catch (std::exception const& e) {
      launcher.set_status(std::string("Update recovery warning: ") + e.what());
    }
  }
}

void on_shutdown(GApplication*, gpointer) {
  Launcher::instance().cancel_focus_loss();
  g_x11_hotkey.stop();
  Launcher::instance().stop_backend();
}

}  // namespace

int run_application(bool toggle_requested = false, bool login_background = false) {
  g_login_background_start = login_background;
  // The first-party rivet lease replaces the old pid-file + SIGUSR1 scheme:
  // second desktop launches show the primary. Explicit --toggle from a
  // compositor binding retains true toggle semantics.
  char const* isolated_id = std::getenv("FULCRUM_INSTANCE_ID");
  std::string const application_id =
      isolated_id != nullptr && *isolated_id != '\0'
          ? isolated_id
          : "site.jrtx.fulcrum";
  rivet::system::SingleInstanceLease lease(application_id);
  if (!lease.is_primary()) {
    // OS login startup must not summon an already running launcher.
    if (login_background) return 0;
    if (!lease.forward_arguments({toggle_requested ? "--toggle" : "--show"})) {
      std::cerr << "fulcrum: running instance not reachable\n";
      return 1;
    }
    return 0;
  }
  // A --toggle that arrives when no primary is running is a summon, not an
  // error: the fresh instance becomes the primary and activate() opens the
  // panel. GApplication never sees the flag — the app runs without argv so
  // the option parser cannot reject it.
  lease.set_activation_handler(on_activation);

  // Load the i18n tables before any UI string is composed: the staged
  // shared/i18n JSON when packaged, the embedded fallback otherwise.
  load_i18n(executable_path().parent_path());

  XInitThreads();
  auto* app =
      gtk_application_new("site.jrtx.fulcrum", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
  g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), nullptr);
  int const status =
      g_application_run(G_APPLICATION(app), 0, nullptr);
  g_object_unref(app);
  return status;
}

int main(int argc, char** argv) {
  bool const toggle_requested = argc > 1 &&
      std::string_view(argv[1]) == "--toggle";
  bool const login_background = argc > 1 &&
      std::string_view(argv[1]) == "--background";
  return run_application(toggle_requested, login_background);
}
