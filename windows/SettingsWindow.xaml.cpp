#include "pch.h"
#include "SettingsWindow.xaml.h"
#include "Autostart.h"
#include "GeneratedI18n.hpp"
#if __has_include("SettingsWindow.g.cpp")
#include "SettingsWindow.g.cpp"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <shobjidl_core.h>
#include <thread>
#include <utility>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Windowing.h>

namespace winrt::RivetHost::implementation {
namespace {

namespace mx = winrt::Microsoft::UI::Xaml;
namespace mxc = winrt::Microsoft::UI::Xaml::Controls;
namespace mxa = winrt::Microsoft::UI::Xaml::Automation;
namespace mw = winrt::Microsoft::UI::Windowing;
namespace wf = winrt::Windows::Foundation;

using Rows = std::vector<std::vector<std::string>>;

std::wstring to_wide(std::string const& utf8_text) {
  if (utf8_text.empty()) return {};
  auto const size = ::MultiByteToWideChar(CP_UTF8, 0, utf8_text.data(),
                                          static_cast<int>(utf8_text.size()),
                                          nullptr, 0);
  std::wstring result(static_cast<std::size_t>(size > 0 ? size : 0), L'\0');
  if (size > 0) {
    ::MultiByteToWideChar(CP_UTF8, 0, utf8_text.data(),
                          static_cast<int>(utf8_text.size()),
                          result.data(), size);
  }
  return result;
}

// "system" follows the OS UI language, same resolution as the launcher's
// set_i18n_language: primary language id 0x04 is Chinese, else English.
std::string effective_language(std::string const& language) {
  if (language != "system") {
    return language;
  }
  return ((::GetUserDefaultUILanguage() & 0x3FF) == 0x0004) ? "zh" : "en";
}

rivet_app::I18nEntry const* find_i18n(std::string_view key) {
  for (auto const& row : rivet_app::kI18nTable) {
    if (row.key == key) return &row;
  }
  return nullptr;
}

bool has_i18n(std::string_view key) { return find_i18n(key) != nullptr; }

std::wstring translated_setting(std::string const& language, std::string_view key) {
  std::string const effective = effective_language(language);
  if (auto const* row = find_i18n(key)) {
    return to_wide(std::string(effective == "en" ? row->en : row->zh));
  }
  return to_wide(std::string(key));
}

// Positional {0} substitution for the shared i18n frames.
std::wstring frame0(std::wstring text, std::wstring const& value) {
  auto const at = text.find(L"{0}");
  if (at == std::wstring::npos) return text;
  return text.replace(at, 3, value);
}

std::string to_utf8(std::wstring const& wide) {
  if (wide.empty()) return {};
  auto const size = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                          static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
  if (size > 0) {
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                          result.data(), size, nullptr, nullptr);
  }
  return result;
}

std::string trimmed(std::string value) {
  auto const begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  auto const end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::wstring trimmed(std::wstring value) {
  auto const begin = value.find_first_not_of(L" \t\r\n");
  if (begin == std::wstring::npos) return {};
  auto const end = value.find_last_not_of(L" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::vector<std::string> split(std::string const& text, char separator) {
  std::vector<std::string> parts;
  std::string part;
  for (auto const c : text) {
    if (c == separator) {
      parts.push_back(part);
      part.clear();
    } else {
      part += c;
    }
  }
  parts.push_back(part);
  return parts;
}

bool contains(std::string const& text, std::string const& part) {
  return text.find(part) != std::string::npos;
}

bool contains(std::wstring const& text, std::wstring const& part) {
  return text.find(part) != std::wstring::npos;
}

// ASCII-only case fold: enough to match the Latin labels, and it leaves
// every other script untouched.
std::wstring to_lower(std::wstring text) {
  for (auto& c : text) {
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
  }
  return text;
}

// The backend key a control carries in its Tag ("" when there is none).
std::string tag_key(wf::IInspectable const& sender) {
  auto element = sender.try_as<mx::FrameworkElement>();
  if (element == nullptr) return {};
  auto tag = element.Tag();
  if (tag == nullptr) return {};
  return to_utf8(std::wstring(winrt::unbox_value<winrt::hstring>(tag)));
}

void set_tag(mx::FrameworkElement const& element, std::string const& key) {
  element.Tag(winrt::box_value(winrt::hstring(to_wide(key))));
}

// A ComboBoxItem's backend value lives in its Tag; the Off provider item
// carries the empty string, which XAML may hand back as a null Tag.
std::string item_value(wf::IInspectable const& item) {
  return tag_key(item);
}

void select_choice(mxc::ComboBox const& box, std::string const& value) {
  auto items = box.Items();
  for (uint32_t i = 0; i < items.Size(); ++i) {
    if (item_value(items.GetAt(i)) == value) {
      box.SelectedIndex(static_cast<int32_t>(i));
      return;
    }
  }
  box.SelectedIndex(-1);
}

double number_or(std::string const& value, double fallback) {
  try {
    return std::stod(value);
  } catch (...) {
    return fallback;
  }
}

// Canonical hotkey grammar shared with the Racket backend. Only modified
// keys are accepted; Esc cancels, and the backend rejects collisions.
std::string recorded_hotkey(winrt::Windows::System::VirtualKey key) {
  unsigned const vk = static_cast<unsigned>(key);
  if (vk == VK_ESCAPE) return "<cancel>";
  std::string name;
  if (vk >= 'A' && vk <= 'Z') {
    name += static_cast<char>(vk + ('a' - 'A'));
  } else if (vk >= '0' && vk <= '9') {
    name += static_cast<char>(vk);
  } else if (vk >= VK_F1 && vk <= VK_F12) {
    name = "f" + std::to_string(vk - VK_F1 + 1);
  } else {
    switch (vk) {
      case VK_SPACE: name = "space"; break;
      case VK_TAB: name = "tab"; break;
      case VK_OEM_COMMA: name = "comma"; break;
      case VK_OEM_PERIOD: name = "period"; break;
      case VK_OEM_MINUS: name = "minus"; break;
      case VK_OEM_PLUS: name = "equal"; break;
      default: return {};
    }
  }
  std::string modifiers;
  if (::GetKeyState(VK_CONTROL) & 0x8000) modifiers += "ctrl+";
  if (::GetKeyState(VK_MENU) & 0x8000) modifiers += "alt+";
  if (::GetKeyState(VK_SHIFT) & 0x8000) modifiers += "shift+";
  if ((::GetKeyState(VK_LWIN) | ::GetKeyState(VK_RWIN)) & 0x8000)
    modifiers += "win+";
  return modifiers.empty() ? std::string() : modifiers + name;
}

// What the exception in flight says, in UTF-8. A refused RPC arrives as a
// std::runtime_error whose message is the backend's "<code>: <detail>".
std::string rpc_error_text() {
  try {
    throw;
  } catch (std::exception const& e) {
    return e.what();
  } catch (...) {
    return "error";
  }
}

// The outcome of one backend call: its value, or the refusal's text.
template <typename T>
struct Outcome {
  T value{};
  std::string error;
};

template <typename T, typename Call>
std::function<Outcome<T>()> attempt(Call call) {
  return [call = std::move(call)] {
    try {
      return Outcome<T>{call(), {}};
    } catch (...) {
      return Outcome<T>{T{}, rpc_error_text()};
    }
  };
}

// Runs `work` on a worker thread — the generated API blocks on the backend
// pipe — and hands the outcome to `then` on the UI thread, if the window is
// still alive. Nothing of the window is touched off the UI thread.
template <typename T>
void off_thread(SettingsWindow& window, std::function<Outcome<T>()> work,
                std::function<void(SettingsWindow&, Outcome<T>)> then) {
  auto const weak = window.get_weak();
  auto const dispatcher = window.DispatcherQueue();
  std::thread([weak, dispatcher, work = std::move(work), then = std::move(then)]() mutable {
    Outcome<T> outcome = work();
    dispatcher.TryEnqueue([weak, then = std::move(then), outcome = std::move(outcome)]() mutable {
      if (auto live = weak.get()) then(*live, std::move(outcome));
    });
  }).detach();
}

mxc::TextBlock make_text(std::wstring const& text, double size = 0, bool dim = false) {
  mxc::TextBlock block;
  block.Text(text);
  block.TextWrapping(mx::TextWrapping::Wrap);
  if (size > 0) block.FontSize(size);
  if (dim) block.Opacity(0.7);
  return block;
}

mxc::TextBlock make_heading(std::wstring const& text) {
  auto block = make_text(text);
  block.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  return block;
}

// The two-column shape of every setting and list row: a label stack on the
// left, the control on the right.
mxc::Grid make_row() {
  mxc::Grid grid;
  grid.MinHeight(52);
  auto star = mxc::ColumnDefinition{};
  star.Width(mx::GridLength{1, mx::GridUnitType::Star});
  grid.ColumnDefinitions().Append(star);
  auto tail = mxc::ColumnDefinition{};
  tail.Width(mx::GridLength{0, mx::GridUnitType::Auto});
  grid.ColumnDefinitions().Append(tail);
  return grid;
}

mxc::StackPanel make_labels(std::wstring const& label) {
  mxc::StackPanel labels;
  labels.Spacing(2);
  labels.Margin(mx::Thickness{0, 8, 16, 8});
  labels.VerticalAlignment(mx::VerticalAlignment::Center);
  labels.Children().Append(make_text(label));
  return labels;
}

mxc::Button make_button(std::wstring const& label) {
  mxc::Button button;
  button.Content(winrt::box_value(label));
  button.VerticalAlignment(mx::VerticalAlignment::Center);
  return button;
}

mxc::TextBlock make_hint(std::wstring const& text) {
  // Sits under a title, a section card or a row of buttons with one gap.
  auto hint = make_text(text, 12, true);
  hint.Margin(mx::Thickness{0, 10, 0, 4});
  return hint;
}

}  // namespace

SettingsWindow::SettingsWindow() {
  InitializeComponent();
  highlight_timer_ = mx::DispatcherTimer{};
  highlight_timer_.Tick({this, &SettingsWindow::Highlight_Tick});
  Title(L"Fulcrum Settings");
  try {
    SystemBackdrop(mx::Media::MicaBackdrop());
    if (auto appWindow = this->AppWindow()) {
      appWindow.Resize(winrt::Windows::Graphics::SizeInt32{760, 640});
    }
    // AppWindow sizes are physical pixels; the first activation knows the
    // monitor's scale and resizes to 760x640 effective (clamped to the work
    // area), so a scaled display does not squeeze the rows.
    Activated({this, &SettingsWindow::First_Activated});
    // Keep current-main's Rivet-owned icon fix: passing an EXE path to
    // SetIcon(String) was incorrect, and raw HICON ownership is unsafe.
    (void)rivet::system::ApplyAppIcon(*this);
  } catch (...) {
    // Native backdrop and titlebar adornments are optional.
  }
}

void SettingsWindow::First_Activated(wf::IInspectable const&, mx::WindowActivatedEventArgs const&) {
  if (sized_) return;
  sized_ = true;
  try {
    // Fifteen grouped destinations and a description line per page need more
    // room than the old six-item form; the three hosts keep one size. The
    // window's own DPI (the content's XamlRoot is still unset this early).
    auto const dpi = ::GetDpiForWindow(rivet::system::NativeWindowHandle(*this));
    auto const scale = dpi == 0 ? 1.0 : static_cast<double>(dpi) / 96.0;
    auto appWindow = this->AppWindow();
    auto const work = mw::DisplayArea::GetFromWindowId(appWindow.Id(), mw::DisplayAreaFallback::Nearest)
                         .WorkArea();
    auto const fit = [&](double wanted, int available) {
      return std::max(320, std::min(static_cast<int>(wanted), available));
    };
    appWindow.Resize(winrt::Windows::Graphics::SizeInt32{
        fit(760 * scale, work.Width), fit(640 * scale, work.Height)});
  } catch (...) {
    // Sizing is cosmetic; the window stays usable at its default size.
  }
}

std::wstring SettingsWindow::T(std::string const& key) const {
  return translated_setting(language_, key);
}

mx::Style SettingsWindow::CardStyle() {
  if (card_style_ == nullptr) {
    card_style_ = Content().as<mx::FrameworkElement>()
                      .Resources()
                      .Lookup(winrt::box_value(L"Card"))
                      .as<mx::Style>();
  }
  return card_style_;
}

// ---- schema-driven pages ------------------------------------------------------

void SettingsWindow::SetSchema(Rows rows) {
  pages_.clear();
  for (auto const& row : rows) {
    if (row.empty()) continue;
    if (row[0] == "page" && row.size() >= 3) {
      pages_.push_back(SettingsPage{row[1], row[2], {}});
      continue;
    }
    if (pages_.empty()) continue;
    auto& page = pages_.back();
    if (row[0] == "section" && row.size() >= 3) {
      if (row[1] == page.id) page.sections.push_back({row[2], {}});
      continue;
    }
    if (row[0] != "setting" || row.size() < 7 || row[2] != page.id) continue;
    // A row whose section line never arrived still gets its section.
    if (page.sections.empty() || page.sections.back().first != row[3]) {
      page.sections.push_back({row[3], {}});
    }
    auto options = split(row[5], ',');
    if (options.size() == 1 && options[0].empty()) options.clear();
    page.sections.back().second.push_back(
        SettingRow{row[1], row[3], row[4], std::move(options), row[6]});
  }
  // The sidebar lists exactly the schema's pages under their group headers;
  // a page the backend adds later shows up without a host change.
  auto items = SettingsNavigation().MenuItems();
  items.Clear();
  nav_items_.clear();
  std::string group;
  for (auto const& page : pages_) {
    if (page.group != group) {
      group = page.group;
      mxc::NavigationViewItemHeader header;
      header.Content(winrt::box_value(T("settings.group." + group)));
      set_tag(header, group);
      items.Append(header);
    }
    mxc::NavigationViewItem item;
    item.Content(winrt::box_value(T("settings.nav." + page.id)));
    set_tag(item, page.id);
    mxa::AutomationProperties::SetAutomationId(item, to_wide("settings-sidebar-" + page.id));
    items.Append(item);
    nav_items_[page.id] = item;
  }
  if (nav_items_.empty()) return;
  // (Re)selecting the current page keeps the user where they were.
  auto const it = nav_items_.find(current_page_);
  SettingsNavigation().SelectedItem(it != nav_items_.end() ? it->second : nav_items_.begin()->second);
}

SettingsPage const* SettingsWindow::FindPage(std::string const& id) const {
  for (auto const& page : pages_) {
    if (page.id == id) return &page;
  }
  return nullptr;
}

void SettingsWindow::NavigateTo(std::string const& page) {
  current_page_ = page;
  SearchSection().Visibility(mx::Visibility::Collapsed);
  ClearError();
  NoticeText().Visibility(mx::Visibility::Collapsed);
  // Commands and Workflows keep their native editors as whole pages; every
  // other page is the schema's rows plus its native extras.
  bool const native = page == "commands" || page == "workflows";
  SchemaSection().Visibility(native ? mx::Visibility::Collapsed : mx::Visibility::Visible);
  CommandsSection().Visibility(page == "commands" ? mx::Visibility::Visible : mx::Visibility::Collapsed);
  WorkflowsSection().Visibility(page == "workflows" ? mx::Visibility::Visible : mx::Visibility::Collapsed);
  if (!native) RenderPage();
  if (page == "commands") LoadBindings();
  if (page == "workflows") { LoadWorkflowList(); LoadWorkflowDetail(); }
}

void SettingsWindow::RenderPageIfVisible() {
  if (SchemaSection().Visibility() == mx::Visibility::Visible) {
    RenderPage();
  } else if (SearchSection().Visibility() == mx::Visibility::Visible) {
    RenderSearch();
  }
}

void SettingsWindow::Navigation_SelectionChanged(
    mxc::NavigationView const&,
    mxc::NavigationViewSelectionChangedEventArgs const& args) {
  if (auto item = args.SelectedItem().try_as<mxc::NavigationViewItem>()) {
    auto const page = tag_key(item);
    if (!page.empty() && page != current_page_) NavigateTo(page);
  }
}

void SettingsWindow::Root_KeyDown(wf::IInspectable const&, mx::Input::KeyRoutedEventArgs const& args) {
  // Ctrl+F focuses the settings search from anywhere in the window.
  if (args.Key() == winrt::Windows::System::VirtualKey::F && (::GetKeyState(VK_CONTROL) & 0x8000)) {
    SettingsSearch().Focus(mx::FocusState::Programmatic);
    SettingsSearch().SelectAll();
    args.Handled(true);
  }
}

mx::UIElement SettingsWindow::MakeSectionTitle(std::string const& text) {
  auto block = make_heading(T(text));
  block.Margin(mx::Thickness{0, 18, 0, 6});
  return block;
}

mx::UIElement SettingsWindow::MakeStartupRow() {
  // Start at login is device-local and never in the schema; it sits before
  // the hotkey, as on macOS and Linux.
  auto grid = make_row();
  auto labels = make_labels(T("settings.start-at-login"));
  auto hint = make_text(T("settings.start-at-login-local"), 12, true);
  labels.Children().Append(hint);
  grid.Children().Append(labels);
  auto toggle = mxc::ToggleSwitch{};
  toggle.OnContent(winrt::box_value(L""));
  toggle.OffContent(winrt::box_value(L""));
  toggle.MinWidth(0);
  toggle.VerticalAlignment(mx::VerticalAlignment::Center);
  set_tag(toggle, "local-login-startup");
  mxa::AutomationProperties::SetAutomationId(toggle, L"FulcrumStartAtLogin");
  mxa::AutomationProperties::SetName(toggle, T("settings.start-at-login"));
  toggle.Toggled({this, &SettingsWindow::Toggle_Toggled});
  mxc::Grid::SetColumn(toggle, 1);
  grid.Children().Append(toggle);
  row_hints_["local-login-startup"] = hint;
  row_roots_.insert_or_assign("local-login-startup", grid);
  startup_toggle_ = toggle;
  return grid;
}

mx::UIElement SettingsWindow::MakeRow(SettingRow const& row) {
  std::string const label_key = "settings." + row.key;
  std::string const hint_key = "settings.hint." + row.key;
  auto const label = T(label_key);
  auto grid = make_row();
  auto labels = make_labels(label);
  auto hint = make_text(T(hint_key), 12, true);
  labels.Children().Append(hint);
  grid.Children().Append(labels);
  row_hints_[row.key] = hint;
  row_roots_.insert_or_assign(row.key, grid);

  mx::FrameworkElement control{nullptr};
  if (row.control == "toggle") {
    auto toggle = mxc::ToggleSwitch{};
    toggle.OnContent(winrt::box_value(L""));
    toggle.OffContent(winrt::box_value(L""));
    toggle.MinWidth(0);
    toggle.IsOn(row.value == "on");
    toggle.Toggled({this, &SettingsWindow::Toggle_Toggled});
    control = toggle;
  } else if (row.control == "select") {
    auto box = mxc::ComboBox{};
    box.MinWidth(160);
    for (auto const& option : row.options) {
      mxc::ComboBoxItem item;
      // The empty option is the "Off" provider; its key spelling is `off`.
      item.Content(winrt::box_value(
          T("settings.opt." + row.key + "." + (option.empty() ? "off" : option))));
      set_tag(item, option);
      box.Items().Append(item);
    }
    box.SelectionChanged({this, &SettingsWindow::Choice_Changed});
    control = box;
    select_choice(box, row.value);
  } else if (row.control == "number") {
    auto box = mxc::NumberBox{};
    box.MinWidth(160);
    box.SpinButtonPlacementMode(mxc::NumberBoxSpinButtonPlacementMode::Inline);
    if (row.options.size() >= 2) {
      // options = the validator's inclusive (min max).
      box.Minimum(number_or(row.options[0], 0));
      box.Maximum(number_or(row.options[1], 1000000));
    }
    box.Value(number_or(row.value, 0));
    box.ValueChanged({this, &SettingsWindow::Number_Changed});
    control = box;
  } else if (row.control == "hotkey") {
    auto box = mxc::TextBox{};
    box.MinWidth(200);
    box.IsReadOnly(true);
    box.PlaceholderText(T("settings.hotkey.placeholder"));
    box.Text(to_wide(row.value));
    mxa::AutomationProperties::SetAutomationId(box, L"settings-record-hotkey");
    box.KeyDown({this, &SettingsWindow::Hotkey_KeyDown});
    control = box;
  } else {
    auto box = mxc::TextBox{};
    box.MinWidth(220);
    std::string const placeholder = label_key + ".placeholder";
    if (has_i18n(placeholder)) box.PlaceholderText(T(placeholder));
    box.Text(to_wide(row.value));
    box.LostFocus({this, &SettingsWindow::Text_LostFocus});
    box.KeyDown({this, &SettingsWindow::Text_KeyDown});
    control = box;
  }
  set_tag(control, row.key);
  mxa::AutomationProperties::SetName(control, label);
  control.VerticalAlignment(mx::VerticalAlignment::Center);
  row_controls_.insert_or_assign(row.key, control);
  mxc::Grid::SetColumn(control, 1);
  grid.Children().Append(control);
  return grid;
}

void SettingsWindow::RenderPage() {
  auto const* page = FindPage(current_page_);
  if (page == nullptr) return;
  loading_ = true;
  row_hints_.clear();
  row_roots_.clear();
  row_controls_.clear();
  list_hosts_.clear();
  SchemaTitle().Text(T("settings.nav." + page->id));
  auto const description = "settings.page." + page->id;
  SchemaDescription().Text(has_i18n(description) ? T(description) : L"");
  auto const host = PageHost();
  host.Children().Clear();
  // A row paints the backend's current truth (values_), not the copy the
  // schema arrived with: a write that just succeeded must survive a repaint.
  for (auto const& section : page->sections) {
    host.Children().Append(MakeSectionTitle("settings.section." + section.first));
    mxc::StackPanel rows;
    mxc::Border card;
    card.Style(CardStyle());
    card.Child(rows);
    for (auto const& row : section.second) {
      if (row.key == "hotkey") rows.Children().Append(MakeStartupRow());
      SettingRow current = row;
      auto const it = values_.find(row.key);
      if (it != values_.end()) current.value = it->second;
      rows.Children().Append(MakeRow(current));
    }
    host.Children().Append(card);
  }
  BuildExtras(page->id, host);
  loading_ = false;
  // A refusal stays on its row across the reload that follows it.
  for (auto const& [key, message] : row_errors_) ShowRowError(key, message);
}

void SettingsWindow::Hotkey_KeyDown(wf::IInspectable const& sender,
                                    mx::Input::KeyRoutedEventArgs const& args) {
  if (loading_) return;
  auto box = sender.try_as<mxc::TextBox>();
  if (box == nullptr) return;
  auto const key = tag_key(sender);
  auto const spec = recorded_hotkey(args.Key());
  auto const it = values_.find(key);
  if (spec == "<cancel>") {
    box.Text(to_wide(it == values_.end() ? "" : it->second));
  } else if (!spec.empty()) {
    box.Text(to_wide(spec));
    Write(key, spec);
  }
  args.Handled(true);
}

void SettingsWindow::Attach(std::shared_ptr<rivet_app::API> api) {
  api_ = std::move(api);
  Load();
}

void SettingsWindow::SetUpdateOpener(std::function<void()> opener) {
  update_opener_ = std::move(opener);
}

void SettingsWindow::RefreshLanguage(std::string const& language) {
  language_ = language.empty() ? "system" : language;
  values_["language"] = language_;
  ApplyLocalization(language_);
  // Dynamic rows were painted in the previous language; rebuild the page
  // and the native editors so the whole window follows the push.
  RenderPageIfVisible();
  if (CommandsSection().Visibility() == mx::Visibility::Visible) LoadBindings();
  if (WorkflowsSection().Visibility() == mx::Visibility::Visible) {
    LoadWorkflowList();
    LoadWorkflowDetail();
  }
}

void SettingsWindow::Load() {
  if (api_ == nullptr) {
    return;
  }
  auto api = api_;
  auto const weak = get_weak();
  auto const dispatcher = DispatcherQueue();
  std::thread([weak, api, dispatcher]() mutable {
    std::map<std::string, std::string> values;
    std::string status;
    Rows schema;
    try {
      // settings-list rows: (key value description …); booleans read on/off.
      for (auto const& row : api->settings_list().get()) {
        if (row.size() >= 2) {
          values[row[0]] = row[1];
        }
      }
    } catch (...) {
    }
    try {
      status = api->ai_status().get();
    } catch (...) {
    }
    try {
      schema = api->settings_ui().get();
    } catch (...) {
    }
    dispatcher.TryEnqueue([weak, values = std::move(values), status,
                           schema = std::move(schema)]() mutable {
      if (auto window = weak.get()) {
        if (!schema.empty()) window->SetSchema(std::move(schema));
        window->Apply(std::move(values), status);
      }
    });
  }).detach();
}

void SettingsWindow::Apply(std::map<std::string, std::string> values,
                           std::string ai_status) {
  values_ = std::move(values);
  language_ = values_.count("language") && !values_["language"].empty()
                  ? values_["language"] : "system";
  ApplyLocalization(language_);
  loading_ = true;
  if (startup_toggle_ != nullptr) {
    startup_toggle_.IsOn(fulcrum::windows::AutostartEnabled());
  }
  loading_ = false;
  ai_status_ = ai_status;
  RefreshAiStatusText();
  RenderPageIfVisible();
}

void SettingsWindow::Write(std::string key, std::string value) {
  if (api_ == nullptr || key.empty()) {
    return;
  }
  values_[key] = value;
  if (key == "language") {
    language_ = value.empty() ? "system" : value;
    ApplyLocalization(language_);
  }
  auto api = api_;
  std::string const lang = language_;
  off_thread<bool>(*this,
      attempt<bool>([api, key, value] { return api->settings_set(key, value).get(); }),
      [lang, key](SettingsWindow& window, Outcome<bool> out) {
        // The backend refused the value (grammar, range): say so on the row
        // and fall back to what it actually holds.
        if (!out.error.empty()) {
          window.ShowRowError(key, frame0(translated_setting(lang, "settings.write.failed"),
                                          to_wide(key)));
          window.Load();
          return;
        }
        if (!out.value) {
          window.ShowRowError(key, key == "hotkey"
              ? translated_setting(lang, "settings.hotkey.unsupported")
              : frame0(translated_setting(lang, "settings.value.unsupported"), to_wide(key)));
          window.Load();
          return;
        }
        window.ClearRowError(key);
        if (key == "ai-provider") window.RefreshAiStatus();
        if (key == "language") window.RefreshLanguage(window.values_["language"]);
      });
}

void SettingsWindow::ShowError(std::wstring const& message) {
  ErrorText().Text(message);
  ErrorText().Visibility(mx::Visibility::Visible);
}

void SettingsWindow::ClearError() {
  ErrorText().Text(L"");
  ErrorText().Visibility(mx::Visibility::Collapsed);
}

void SettingsWindow::Notice(std::wstring const& message) {
  NoticeText().Text(message);
  NoticeText().Visibility(mx::Visibility::Visible);
}

mx::Media::SolidColorBrush SettingsWindow::ErrorBrush() {
  return mx::Media::SolidColorBrush{
      winrt::Windows::UI::ColorHelper::FromArgb(255, 0xD1, 0x34, 0x38)};
}

void SettingsWindow::ShowRowError(std::string const& key, std::wstring const& message) {
  row_errors_[key] = message;
  auto const it = row_hints_.find(key);
  if (it == row_hints_.end()) {
    // The row is not on screen (another page): the window says it instead.
    ShowError(message);
    return;
  }
  it->second.Text(message);
  it->second.Foreground(ErrorBrush());
  it->second.Opacity(1.0);
}

void SettingsWindow::ClearRowError(std::string const& key) {
  row_errors_.erase(key);
  auto const it = row_hints_.find(key);
  if (it == row_hints_.end()) return;
  it->second.Text(T("settings.hint." + key));
  it->second.Foreground(nullptr);
  it->second.Opacity(0.7);
}

void SettingsWindow::RefreshAiStatus() {
  if (api_ == nullptr) {
    return;
  }
  auto api = api_;
  off_thread<std::string>(*this,
      attempt<std::string>([api] { return api->ai_status().get(); }),
      [](SettingsWindow& window, Outcome<std::string> out) {
        if (out.error.empty()) {
          window.ai_status_ = out.value;
          window.RefreshAiStatusText();
        }
      });
}

void SettingsWindow::RefreshAiStatusText() {
  if (ai_status_text_ == nullptr) return;
  // The backend reports one of three states; the two refusals carry a
  // reason, not a localized sentence.
  std::wstring text;
  if (ai_status_ == "ready") {
    text = T("settings.ai-ready");
  } else if (ai_status_ == "no provider configured") {
    text = T("settings.ai-status.no-provider");
  } else if (ai_status_.rfind("no API key for ", 0) == 0) {
    text = T("settings.ai-status.no-key") + L" " + to_wide(ai_status_.substr(16));
  } else if (!ai_status_.empty()) {
    text = to_wide(ai_status_);
  }
  ai_status_text_.Text(text);
}

void SettingsWindow::ApplyLocalization(std::string const& language) {
  auto const tr = [&language](std::string_view key) {
    return translated_setting(language, key);
  };
  // Group headers and destinations re-label in place, in sidebar order.
  for (auto const& entry : SettingsNavigation().MenuItems()) {
    auto const key = tag_key(entry);
    if (auto header = entry.try_as<mxc::NavigationViewItemHeader>()) {
      header.Content(winrt::box_value(T("settings.group." + key)));
    } else if (auto item = entry.try_as<mxc::NavigationViewItem>()) {
      item.Content(winrt::box_value(T("settings.nav." + key)));
    }
  }
  // Sidebar chrome.
  SettingsSearch().Header(winrt::box_value(tr("settings.search.label")));
  SettingsSearch().PlaceholderText(tr("settings.search.label"));
  SearchTitle().Text(tr("settings.search.label"));
  // Commands.
  CommandsTitle().Text(tr("settings.nav.commands"));
  CommandsHint().Text(tr("settings.hint.commands"));
  CommandsLoadingText().Text(tr("settings.commands.loading"));
  // Workflows editor chrome (dynamic rows localize at paint time).
  WorkflowsTitle().Text(tr("settings.nav.workflows"));
  WorkflowsHint().Text(tr("settings.wf.hint"));
  WorkflowNameBox().PlaceholderText(tr("settings.wf.new-name"));
  WorkflowCreateButton().Content(winrt::box_value(tr("settings.wf.create")));
  WorkflowRefreshButton().Content(winrt::box_value(tr("settings.wf.refresh")));
  WorkflowListLoadingText().Text(tr("settings.wf.loading"));
  WorkflowListPrevButton().Content(winrt::box_value(tr("settings.wf.prev")));
  WorkflowListNextButton().Content(winrt::box_value(tr("settings.wf.next")));
  WorkflowListPage().Text(frame0(tr("settings.wf.page"),
                                 std::to_wstring(workflow_list_page_)));
  if (workflow_selected_.empty()) {
    WorkflowSelectedText().Text(tr("settings.wf.select"));
  }
  WorkflowStepPrevButton().Content(winrt::box_value(tr("settings.wf.prev")));
  WorkflowStepNextButton().Content(winrt::box_value(tr("settings.wf.next")));
  WorkflowStepPage().Text(frame0(tr("settings.wf.steps-page"),
                                 std::to_wstring(workflow_step_page_)));
  WorkflowStepBox().PlaceholderText(tr("settings.wf.step-placeholder"));
  WorkflowAddStepButton().Content(winrt::box_value(tr("settings.wf.add-step")));
  WorkflowInputLabel().Text(tr("settings.wf.input"));
  WorkflowInputBox().PlaceholderText(tr("settings.wf.clipboard-note"));
  WorkflowDryRunButton().Content(winrt::box_value(tr("settings.wf.dry-run")));
  WorkflowRunCopyButton().Content(winrt::box_value(tr("settings.wf.run-copy")));
  WorkflowDeleteConfirm().Content(winrt::box_value(tr("settings.wf.confirm-delete")));
  WorkflowDeleteButton().Content(winrt::box_value(tr("settings.wf.delete")));
  WorkflowClipboardNote().Text(tr("settings.wf.clipboard-note"));
  WorkflowTraceTitle().Text(tr("settings.wf.trace"));
  WorkflowTracePrevButton().Content(winrt::box_value(tr("settings.wf.prev")));
  WorkflowTraceNextButton().Content(winrt::box_value(tr("settings.wf.next")));
  WorkflowTracePage().Text(frame0(tr("settings.wf.trace-page"),
                                  std::to_wstring(workflow_trace_page_)));
}

void SettingsWindow::LoadBindings() {
  if (api_ == nullptr) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto api = api_;
  std::string const lang = language_;
  std::thread([weak, api, dispatcher, lang]() mutable {
    std::vector<std::vector<std::string>> commands, bindings;
    try {
      commands = api->bindings_commands().get();
      bindings = api->bindings_list().get();
    } catch (...) {
      dispatcher.TryEnqueue([weak, lang] {
        if (auto window = weak.get()) {
          window->BindingErrorText().Text(
              translated_setting(lang, "settings.commands.load-failed"));
          window->BindingErrorText().Visibility(mx::Visibility::Visible);
        }
      });
      return;
    }
    dispatcher.TryEnqueue([weak, commands = std::move(commands),
                           bindings = std::move(bindings)]() mutable {
      if (auto window = weak.get())
        window->ShowBindings(std::move(commands), std::move(bindings));
    });
  }).detach();
}

void SettingsWindow::ShowBindings(
    std::vector<std::vector<std::string>> commands,
    std::vector<std::vector<std::string>> bindings) {
  auto const tr = [this](std::string_view key) {
    return translated_setting(language_, key);
  };
  std::map<std::string, std::pair<std::string, std::string>> existing;
  for (auto const& row : bindings) {
    if (row.size() >= 3) existing[row[0]] = {row[1], row[2]};
  }
  auto children = CommandRows().Children();
  children.Clear();
  for (auto const& command : commands) {
    if (command.empty()) continue;
    auto const id = command[0];
    auto const current = existing.find(id);
    std::string const alias = current == existing.end() ? "" : current->second.first;
    std::string const hotkey = current == existing.end() ? "" : current->second.second;
    mxc::StackPanel row;
    row.Orientation(mxc::Orientation::Horizontal);
    row.Spacing(8);
    mxc::TextBlock label;
    label.Text(to_wide(command.size() >= 2 ? command[1] : id));
    label.Width(145);
    label.TextTrimming(mx::TextTrimming::CharacterEllipsis);
    label.VerticalAlignment(mx::VerticalAlignment::Center);
    row.Children().Append(label);
    mxc::TextBox aliasBox;
    aliasBox.Width(105);
    aliasBox.Text(to_wide(alias));
    aliasBox.PlaceholderText(tr("settings.commands.alias"));
    row.Children().Append(aliasBox);
    mxc::TextBox hotkeyBox;
    hotkeyBox.Width(125);
    hotkeyBox.Text(to_wide(hotkey));
    hotkeyBox.PlaceholderText(tr("settings.commands.press-keys"));
    hotkeyBox.IsReadOnly(true);
    hotkeyBox.KeyDown([hotkeyBox, previous = to_wide(hotkey)]
                      (auto const&, mx::Input::KeyRoutedEventArgs const& args) {
      auto spec = recorded_hotkey(args.Key());
      if (spec == "<cancel>") hotkeyBox.Text(previous);
      else if (!spec.empty()) hotkeyBox.Text(to_wide(spec));
      args.Handled(true);
    });
    row.Children().Append(hotkeyBox);
    mxc::Button save;
    save.Content(winrt::box_value(tr("settings.save")));
    save.Click([weak = get_weak(), id, aliasBox, hotkeyBox](auto const&, auto const&) {
      if (auto window = weak.get())
        window->SaveBinding(id, trimmed(to_utf8(std::wstring(aliasBox.Text()))),
                             to_utf8(std::wstring(hotkeyBox.Text())));
    });
    row.Children().Append(save);
    mxc::Button clear;
    clear.Content(winrt::box_value(tr("settings.commands.clear")));
    clear.Click([weak = get_weak(), id](auto const&, auto const&) {
      if (auto window = weak.get()) window->ClearBinding(id);
    });
    row.Children().Append(clear);
    children.Append(row);
  }
}

void SettingsWindow::SaveBinding(std::string id, std::string alias,
                                 std::string hotkey) {
  if (!api_) return;
  auto const weak = get_weak();
  auto const dispatcher = DispatcherQueue();
  auto api = api_;
  std::string const lang = language_;
  std::thread([weak, api, dispatcher, id, alias, hotkey, lang]() mutable {
    std::wstring error;
    try {
      if (!api->binding_set(id, alias, hotkey).get())
        error = translated_setting(lang, "settings.commands.rejected");
    } catch (...) {
      error = translated_setting(lang, "settings.commands.save-failed");
    }
    dispatcher.TryEnqueue([weak, error] {
      if (auto window = weak.get()) {
        window->BindingErrorText().Text(error);
        window->BindingErrorText().Visibility(
            error.empty() ? mx::Visibility::Collapsed : mx::Visibility::Visible);
        window->LoadBindings();
      }
    });
  }).detach();
}

void SettingsWindow::ClearBinding(std::string id) {
  if (!api_) return;
  auto const weak = get_weak();
  auto const dispatcher = DispatcherQueue();
  auto api = api_;
  std::string const lang = language_;
  std::thread([weak, api, dispatcher, id, lang]() mutable {
    std::wstring error;
    try {
      if (!api->binding_clear(id).get())
        error = translated_setting(lang, "settings.commands.clear-failed");
    } catch (...) {
      error = translated_setting(lang, "settings.commands.clear-failed");
    }
    dispatcher.TryEnqueue([weak, error] {
      if (auto window = weak.get()) {
        window->BindingErrorText().Text(error);
        window->BindingErrorText().Visibility(
            error.empty() ? mx::Visibility::Collapsed : mx::Visibility::Visible);
        window->LoadBindings();
      }
    });
  }).detach();
}

// ---- Workflow Pro native editor -------------------------------------------
//
// Search and run_action are existing RVT1 endpoints. The backend remains the
// only workflow interpreter and JSON owner. Calls run on worker threads and
// return to DispatcherQueue before updating XAML controls.
void SettingsWindow::LoadWorkflowList() {
  if (!api_) return;
  auto dispatcher = DispatcherQueue();
  auto weak = get_weak();
  auto api = api_;
  int const page = workflow_list_page_;
  std::string const lang = language_;
  std::thread([weak, api, dispatcher, page, lang]() mutable {
    std::vector<std::vector<std::string>> rows;
    std::wstring error;
    try { rows = api->search("wf list " + std::to_string(page)).get(); }
    catch (...) { error = translated_setting(lang, "settings.wf.load-failed"); }
    dispatcher.TryEnqueue([weak, page, lang, rows = std::move(rows), error]() mutable {
      auto window = weak.get();
      if (!window || window->workflow_list_page_ != page) return;
      auto const tr = [&lang](std::string_view key) {
        return translated_setting(lang, key);
      };
      auto children = window->WorkflowListRows().Children();
      children.Clear();
      unsigned shown = 0;
      for (auto const& row : rows) {
        if (row.size() < 8 || row[3] != "Workflow" || row[7] != "Saved")
          continue;
        std::string const prefix = "Workflow · ";
        if (row[1].rfind(prefix, 0) != 0) continue;
        std::string const name = row[1].substr(prefix.size());
        mxc::Button entry;
        entry.Content(winrt::box_value(winrt::hstring(to_wide(name))));
        entry.HorizontalAlignment(mx::HorizontalAlignment::Stretch);
        entry.HorizontalContentAlignment(mx::HorizontalAlignment::Left);
        entry.Click([weak, name](auto const&, auto const&) {
          if (auto w = weak.get()) {
            w->workflow_selected_ = name;
            w->workflow_step_page_ = 1;
            w->workflow_trace_page_ = 1;
            w->WorkflowDeleteConfirm().IsChecked(
                winrt::box_value(false).as<winrt::Windows::Foundation::IReference<bool>>());
            w->LoadWorkflowDetail();
          }
        });
        children.Append(entry);
        ++shown;
      }
      if (shown == 0) {
        mxc::TextBlock empty;
        empty.Text(tr("settings.wf.empty"));
        children.Append(empty);
      }
      window->WorkflowListPage().Text(
          frame0(tr("settings.wf.page"), std::to_wstring(page)));
      if (!error.empty()) window->WorkflowStatusText().Text(error);
    });
  }).detach();
}

void SettingsWindow::LoadWorkflowDetail() {
  if (!api_) return;
  if (workflow_selected_.empty()) {
    WorkflowSelectedText().Text(translated_setting(language_, "settings.wf.select"));
    WorkflowStepRows().Children().Clear();
    WorkflowTraceRows().Children().Clear();
    WorkflowRunCopyButton().IsEnabled(false);
    return;
  }
  std::string const name = workflow_selected_;
  int const step_page = workflow_step_page_;
  int const trace_page = workflow_trace_page_;
  std::string const lang = language_;
  WorkflowSelectedText().Text(to_wide(name));
  auto dispatcher = DispatcherQueue();
  auto weak = get_weak();
  auto api = api_;
  std::thread([weak, api, dispatcher, name, step_page, trace_page, lang]() mutable {
    std::vector<std::vector<std::string>> steps, traces;
    std::wstring error;
    try {
      steps = api->search("wf show " + name + " " +
                          std::to_string(step_page)).get();
      traces = api->search("wf trace " + name + " " +
                           std::to_string(trace_page)).get();
    } catch (...) {
      error = translated_setting(lang, "settings.wf.load-failed");
    }
    dispatcher.TryEnqueue([weak, name, step_page, trace_page, lang,
                           steps = std::move(steps),
                           traces = std::move(traces), error]() mutable {
      auto window = weak.get();
      if (!window || window->workflow_selected_ != name ||
          window->workflow_step_page_ != step_page ||
          window->workflow_trace_page_ != trace_page) return;
      auto const tr = [&lang](std::string_view key) {
        return translated_setting(lang, key);
      };
      auto step_children = window->WorkflowStepRows().Children();
      step_children.Clear();
      bool copy_safe = false;
      for (auto const& row : steps) {
        if (row.size() < 8 || row[3] != "Workflow" || row[7] != "Step")
          continue;
        mxc::StackPanel line;
        line.Orientation(mxc::Orientation::Horizontal);
        line.Spacing(8);
        mxc::TextBlock text;
        text.Text(to_wide(row[1] + ": " + row[2]));
        text.TextWrapping(mx::TextWrapping::Wrap);
        text.VerticalAlignment(mx::VerticalAlignment::Center);
        text.Width(280);
        line.Children().Append(text);
        std::string const title = row[1];
        auto const pos = title.find_last_of(' ');
        std::string const index = pos == std::string::npos
                                      ? "" : title.substr(pos + 1);
        mxc::Button remove;
        remove.Content(winrt::box_value(tr("settings.wf.remove")));
        remove.Click([weak, name, index](auto const&, auto const&) {
          if (auto w = weak.get()) {
            w->WorkflowCommand("workflow.remove",
                               name + std::string(1, '\x1F') + index);
          }
        });
        line.Children().Append(remove);
        step_children.Append(line);
        if (row[2] == "copy") copy_safe = true;
      }
      window->workflow_copy_safe_ = copy_safe;
      window->WorkflowRunCopyButton().IsEnabled(copy_safe);
      window->WorkflowStepPage().Text(
          frame0(tr("settings.wf.steps-page"), std::to_wstring(step_page)));
      auto trace_children = window->WorkflowTraceRows().Children();
      trace_children.Clear();
      for (auto const& row : traces) {
        if (row.size() < 8 || row[3] != "Workflow" || row[7] != "Trace")
          continue;
        mxc::TextBlock line;
        line.Text(to_wide(row[1] + " · " + row[2]));
        line.TextWrapping(mx::TextWrapping::Wrap);
        trace_children.Append(line);
      }
      window->WorkflowTracePage().Text(
          frame0(tr("settings.wf.trace-page"), std::to_wstring(trace_page)));
      if (!error.empty()) window->WorkflowStatusText().Text(error);
    });
  }).detach();
}

void SettingsWindow::WorkflowCommand(std::string id, std::string arg) {
  if (!api_) return;
  auto dispatcher = DispatcherQueue();
  auto weak = get_weak();
  auto api = api_;
  std::string const lang = language_;
  WorkflowStatusText().Text(translated_setting(lang, "settings.wf.running"));
  std::thread([weak, api, dispatcher, lang, id = std::move(id),
               arg = std::move(arg)]() mutable {
    std::string status;
    std::wstring error;
    try { status = api->run_action(id, arg).get(); }
    catch (...) { error = translated_setting(lang, "settings.wf.action-failed"); }
    dispatcher.TryEnqueue([weak, id, status, error, lang] {
      if (auto w = weak.get()) {
        w->WorkflowStatusText().Text(
            error.empty() ? translated_setting(lang, "settings.wf.done") : error);
        if (error.empty() && id == "workflow.delete") {
          w->workflow_selected_.clear();
          w->WorkflowDeleteConfirm().IsChecked(
                winrt::box_value(false).as<winrt::Windows::Foundation::IReference<bool>>());
          w->workflow_list_page_ = 1;
          w->workflow_step_page_ = 1;
          w->workflow_trace_page_ = 1;
        }
        w->LoadWorkflowList();
        w->LoadWorkflowDetail();
      }
    });
  }).detach();
}

void SettingsWindow::WorkflowCreate_Click(
    winrt::Windows::Foundation::IInspectable const&, mx::RoutedEventArgs const&) {
  auto name = trimmed(to_utf8(std::wstring(WorkflowNameBox().Text())));
  if (name.empty()) {
    WorkflowStatusText().Text(translated_setting(language_, "settings.wf.enter-name"));
    return;
  }
  workflow_list_page_ = 1;
  WorkflowCommand("workflow.create", name);
}

void SettingsWindow::WorkflowRefresh_Click(
    winrt::Windows::Foundation::IInspectable const&, mx::RoutedEventArgs const&) {
  LoadWorkflowList();
  LoadWorkflowDetail();
}
void SettingsWindow::WorkflowPrevious_Click(
    winrt::Windows::Foundation::IInspectable const&, mx::RoutedEventArgs const&) {
  workflow_list_page_ = std::max(1, workflow_list_page_ - 1);
  LoadWorkflowList();
}
void SettingsWindow::WorkflowNext_Click(
    winrt::Windows::Foundation::IInspectable const&, mx::RoutedEventArgs const&) {
  ++workflow_list_page_;
  LoadWorkflowList();
}
void SettingsWindow::WorkflowStepPrevious_Click(
    winrt::Windows::Foundation::IInspectable const&, mx::RoutedEventArgs const&) {
  workflow_step_page_ = std::max(1, workflow_step_page_ - 1);
  LoadWorkflowDetail();
}
void SettingsWindow::WorkflowStepNext_Click(
    winrt::Windows::Foundation::IInspectable const&, mx::RoutedEventArgs const&) {
  ++workflow_step_page_;
  LoadWorkflowDetail();
}
void SettingsWindow::WorkflowTracePrevious_Click(
    winrt::Windows::Foundation::IInspectable const&, mx::RoutedEventArgs const&) {
  workflow_trace_page_ = std::max(1, workflow_trace_page_ - 1);
  LoadWorkflowDetail();
}
void SettingsWindow::WorkflowTraceNext_Click(
    winrt::Windows::Foundation::IInspectable const&, mx::RoutedEventArgs const&) {
  ++workflow_trace_page_;
  LoadWorkflowDetail();
}
void SettingsWindow::WorkflowAdd_Click(
    winrt::Windows::Foundation::IInspectable const&, mx::RoutedEventArgs const&) {
  if (workflow_selected_.empty()) return;
  auto step = trimmed(to_utf8(std::wstring(WorkflowStepBox().Text())));
  if (step.empty()) {
    WorkflowStatusText().Text(translated_setting(language_, "settings.wf.enter-step"));
    return;
  }
  WorkflowCommand("workflow.add",
                  workflow_selected_ + std::string(1, '\x1F') + step);
}
void SettingsWindow::WorkflowDry_Click(
    winrt::Windows::Foundation::IInspectable const&, mx::RoutedEventArgs const&) {
  if (workflow_selected_.empty()) return;
  workflow_trace_page_ = 1;
  WorkflowCommand("workflow.dry",
                  workflow_selected_ + std::string(1, '\x1F') +
                  to_utf8(std::wstring(WorkflowInputBox().Text())));
}
void SettingsWindow::WorkflowRun_Click(
    winrt::Windows::Foundation::IInspectable const&, mx::RoutedEventArgs const&) {
  if (workflow_selected_.empty() || !workflow_copy_safe_) return;
  workflow_trace_page_ = 1;
  WorkflowCommand("workflow.run",
                  workflow_selected_ + std::string(1, '\x1F') +
                  to_utf8(std::wstring(WorkflowInputBox().Text())));
}
void SettingsWindow::WorkflowDelete_Click(
    winrt::Windows::Foundation::IInspectable const&, mx::RoutedEventArgs const&) {
  if (workflow_selected_.empty()) return;
  auto checked = WorkflowDeleteConfirm().IsChecked();
  if (!checked || !checked.Value()) {
    WorkflowStatusText().Text(
        translated_setting(language_, "settings.wf.confirm-first"));
    return;
  }
  WorkflowCommand("workflow.delete", workflow_selected_);
}

// ---- native extras: list pages, keys, transfers -------------------------------

mxc::StackPanel SettingsWindow::BeginCard(std::string const& title_key, std::string const& help_key,
                                          mxc::StackPanel const& host, std::string const& add_key,
                                          std::function<void()> on_add) {
  if (!add_key.empty()) {
    // The Add button sits at the right end of the section title line.
    auto line = make_row();
    line.MinHeight(0);
    auto title = make_heading(T(title_key));
    title.Margin(mx::Thickness{0, 18, 6, 6});
    line.Children().Append(title);
    auto add = make_button(T(add_key));
    add.Margin(mx::Thickness{0, 12, 0, 2});
    mxa::AutomationProperties::SetAutomationId(add, L"settings-list-add");
    add.Click([weak = get_weak(), on_add = std::move(on_add)](auto const&, auto const&) {
      if (auto window = weak.get()) on_add();
    });
    mxc::Grid::SetColumn(add, 1);
    line.Children().Append(add);
    host.Children().Append(line);
  } else if (!title_key.empty()) {
    host.Children().Append(MakeSectionTitle(title_key));
  }
  if (has_i18n(help_key)) host.Children().Append(make_hint(T(help_key)));
  mxc::StackPanel rows;
  rows.Spacing(4);
  mxc::Border card;
  card.Style(CardStyle());
  card.Child(rows);
  host.Children().Append(card);
  return rows;
}

std::wstring SettingsWindow::RefusalText(
    std::string const& raw, std::string const& fallback_key,
    std::vector<std::pair<std::string, std::string>> const& codes) const {
  // The backend leads with a stable code word; the rest is detail for the
  // host's own sentence. Anything else is shown as the backend said it.
  auto const code = raw.substr(0, raw.find(':'));
  for (auto const& [known, key] : codes) {
    if (code == known) return T(key);
  }
  return frame0(T(fallback_key), to_wide(raw));
}

void SettingsWindow::BuildExtras(std::string const& page, mxc::StackPanel const& host) {
  if (page == "apps") {
    BuildNamedList("app-folders", host);
    BuildNamedList("apps", host);
  } else if (page == "extensions") {
    BuildPlugins(host);
  } else if (page == "snippets") {
    BuildCollections(true, host);
  } else if (page == "quicklinks") {
    BuildCollections(false, host);
  } else if (page == "ai") {
    BuildAiKey(host);
  } else if (page == "clipboard") {
    BuildClearButton(host, "settings.clipboard.clear", "settings.clipboard.clear-confirm", true);
  } else if (page == "privacy") {
    host.Children().Append(make_hint(T("settings.privacy.stays")));
    BuildClearButton(host, "settings.privacy.clear", "settings.privacy.clear-confirm", false);
  } else if (page == "sync") {
    BuildTransfer(host);
  }
}

// ---- named lists (Application folders, Searchable applications) ---------------

void SettingsWindow::BuildNamedList(std::string const& kind, mxc::StackPanel const& host) {
  bool const folders = kind == "app-folders";
  auto rows = BeginCard(folders ? "settings.list.app-folders" : "settings.list.apps",
                        folders ? "settings.list.app-folders-help" : "settings.list.apps-help", host,
                        folders ? "settings.list.add" : "", [weak = get_weak()] {
                          if (auto window = weak.get()) window->EditNamedList("app-folders", "", "", "");
                        });
  list_hosts_[kind] = rows;
  mxa::AutomationProperties::SetAutomationId(rows, to_wide("settings-list-" + kind));
  auto const cached = list_cache_.find(kind);
  if (cached != list_cache_.end()) ShowNamedList(kind, rows, cached->second);
  LoadNamedList(kind, rows);
}

void SettingsWindow::LoadNamedList(std::string const& kind, mxc::StackPanel const& rows) {
  if (api_ == nullptr) return;
  auto api = api_;
  off_thread<Rows>(*this, attempt<Rows>([api, kind] { return api->named_list_items(kind).get(); }),
      [kind, rows](SettingsWindow& window, Outcome<Rows> out) {
        if (!out.error.empty()) {
          window.ShowError(window.RefusalText(out.error, "settings.list.failed",
                                              {{"duplicate-name", "settings.list.duplicate"},
                                               {"path-required", "settings.list.path-required"},
                                               {"name-required", "settings.list.name-required"}}));
          return;
        }
        window.list_cache_[kind] = out.value;
        window.ShowNamedList(kind, rows, out.value);
      });
}

void SettingsWindow::ShowNamedList(std::string const& kind, mxc::StackPanel const& rows, Rows items) {
  // The page was left before the answer arrived: there is nothing to paint into.
  auto const host = list_hosts_.find(kind);
  if (host == list_hosts_.end() || host->second != rows) return;
  bool const toggles = kind == "apps";
  rows.Children().Clear();
  if (items.empty()) {
    auto empty = make_text(T("settings.list.empty"), 0, true);
    empty.Margin(mx::Thickness{0, 10, 0, 10});
    rows.Children().Append(empty);
  }
  for (auto const& item : items) {
    if (item.size() < 5) continue;
    auto const& id = item[0];
    auto const& name = item[1];
    auto const& path = item[2];
    auto grid = make_row();
    auto labels = make_labels(to_wide(name));
    if (!path.empty()) labels.Children().Append(make_text(to_wide(path), 12, true));
    grid.Children().Append(labels);
    mx::FrameworkElement control{nullptr};
    if (toggles) {
      auto toggle = mxc::ToggleSwitch{};
      toggle.OnContent(winrt::box_value(L""));
      toggle.OffContent(winrt::box_value(L""));
      toggle.MinWidth(0);
      toggle.IsOn(item[4] == "on");
      mxa::AutomationProperties::SetName(toggle, to_wide(name));
      toggle.Toggled([weak = get_weak(), kind, id, name, path](wf::IInspectable const& sender,
                                                               mx::RoutedEventArgs const&) {
        if (auto window = weak.get()) {
          window->SetNamedListEnabled(kind, id, name, path,
                                      sender.as<mxc::ToggleSwitch>().IsOn());
        }
      });
      control = toggle;
    } else {
      auto actions = mxc::StackPanel{};
      actions.Orientation(mxc::Orientation::Horizontal);
      actions.Spacing(8);
      auto edit = make_button(T("settings.list.edit"));
      edit.Click([weak = get_weak(), kind, id, name, path](auto const&, auto const&) {
        if (auto window = weak.get()) window->EditNamedList(kind, id, name, path);
      });
      actions.Children().Append(edit);
      auto remove = make_button(T("settings.list.remove"));
      remove.Click([weak = get_weak(), kind, id, name](auto const&, auto const&) {
        if (auto window = weak.get()) window->RemoveNamedList(kind, id, name);
      });
      actions.Children().Append(remove);
      control = actions;
    }
    control.VerticalAlignment(mx::VerticalAlignment::Center);
    mxc::Grid::SetColumn(control, 1);
    grid.Children().Append(control);
    rows.Children().Append(grid);
  }
}

void SettingsWindow::EditNamedList(std::string const& kind, std::string const& id,
                                   std::string const& name, std::string const& path) {
  EditNamedListDialog(kind, id, name, path, L"");
}

// The named-list refusal codes map to the host's own sentences.
std::vector<std::pair<std::string, std::string>> const kListCodes{
    {"duplicate-name", "settings.list.duplicate"},
    {"path-required", "settings.list.path-required"},
    {"name-required", "settings.list.name-required"},
};
// The add/edit form. `error` is the refusal that reopened it, so one field
// is corrected instead of the entry retyped.
void SettingsWindow::EditNamedListDialog(std::string const& kind, std::string const& id,
                                         std::string const& name, std::string const& path,
                                         std::wstring error) {
  DialogField name_field;
  name_field.label = T("settings.list.name");
  name_field.value = to_wide(name);
  std::vector<DialogField> fields{name_field};
  bool const folders = kind == "app-folders";
  if (folders) {
    DialogField path_field;
    path_field.label = T("settings.list.path");
    path_field.value = to_wide(path);
    path_field.browse = true;
    fields.push_back(path_field);
  }
  // An edit keeps the entry's switch where it was.
  std::string const enabled = [this, kind, &id] {
    for (auto const& item : list_cache_[kind]) {
      if (item.size() >= 5 && item[0] == id) return item[4];
    }
    return std::string("on");
  }();
  auto const lang = language_;
  ShowForm(id.empty() ? T("settings.list.add-title") : T("settings.list.edit-title"), fields,
           "settings.save", std::move(error),
           [folders, lang](FormDialog& form) {
             auto const fail = [&form, lang](char const* key) {
               form.error.Text(translated_setting(lang, key));
               form.error.Visibility(mx::Visibility::Visible);
             };
             if (to_utf8(trimmed(std::wstring(form.boxes[0].Text()))).empty()) {
               fail("settings.list.name-required");
               return false;
             }
             if (folders && to_utf8(trimmed(std::wstring(form.boxes[1].Text()))).empty()) {
               fail("settings.list.path-required");
               return false;
             }
             return true;
           },
           [weak = get_weak(), kind, id, folders, enabled](std::vector<std::string> values) {
             if (auto window = weak.get()) {
               window->SaveNamedList(kind, id, values[0], folders ? values[1] : "", enabled);
             }
           });
}

void SettingsWindow::SaveNamedList(std::string const& kind, std::string const& id,
                                   std::string const& name, std::string const& path,
                                   std::string const& enabled) {
  if (api_ == nullptr) return;
  auto api = api_;
  off_thread<std::string>(*this,
      attempt<std::string>([api, kind, id, name, path, enabled] {
        return api->named_list_set(kind, id, name, path, "", enabled).get();
      }),
      [kind, id, name, path](SettingsWindow& window, Outcome<std::string> out) {
        if (!out.error.empty()) {
          if (id.empty()) {
            // Keep the entry on screen with the reason under its fields.
            window.EditNamedListDialog(kind, id, name, path,
                                       window.RefusalText(out.error, "settings.list.failed", kListCodes));
          } else {
            window.ShowError(window.RefusalText(out.error, "settings.list.failed", kListCodes));
          }
          return;
        }
        window.ClearError();
        auto const host = window.list_hosts_.find(kind);
        if (host != window.list_hosts_.end()) window.LoadNamedList(kind, host->second);
      });
}

void SettingsWindow::RemoveNamedList(std::string const& kind, std::string const& id,
                                     std::string const& name) {
  // Removing an entry cannot be undone from the window: ask once.
  ConfirmThen(T("settings.list.remove"), frame0(T("settings.list.remove-confirm"), to_wide(name)),
              "settings.list.remove", [weak = get_weak(), kind, id] {
                if (auto window = weak.get()) window->DeleteNamedList(kind, id);
              });
}

void SettingsWindow::DeleteNamedList(std::string const& kind, std::string const& id) {
  if (api_ == nullptr) return;
  auto api = api_;
  off_thread<bool>(*this, attempt<bool>([api, kind, id] { return api->named_list_remove(kind, id).get(); }),
      [kind](SettingsWindow& window, Outcome<bool> out) {
        if (!out.error.empty()) {
          window.ShowError(window.RefusalText(out.error, "settings.list.failed", kListCodes));
          return;
        }
        window.ClearError();
        auto const host = window.list_hosts_.find(kind);
        if (host != window.list_hosts_.end()) window.LoadNamedList(kind, host->second);
      });
}

void SettingsWindow::SetNamedListEnabled(std::string const& kind, std::string const& id,
                                         std::string const& name, std::string const& path,
                                         bool enabled) {
  SaveNamedList(kind, id, name, path, enabled ? "on" : "off");
}

// ---- snippets and quicklinks ---------------------------------------------------

namespace {

std::string const kSnippets = "snippets";
std::string const kQuicklinks = "quicklinks";

std::string collection_kind(bool snippets) { return snippets ? kSnippets : kQuicklinks; }

// A quicklink target: an HTTP(S) URL, a file URL, or an absolute path. The
// backend stays the authority; this only spares the user a round trip.
bool plausible_target(std::string const& text) {
  if (text.rfind("http://", 0) == 0 || text.rfind("https://", 0) == 0 ||
      text.rfind("file://", 0) == 0 || text.rfind("\\\\", 0) == 0 ||
      text.rfind('/', 0) == 0) {
    return true;
  }
  return text.size() >= 3 && std::isalpha(static_cast<unsigned char>(text[0])) &&
         text[1] == ':' && (text[2] == '\\' || text[2] == '/');
}

}  // namespace

void SettingsWindow::BuildCollections(bool snippets, mxc::StackPanel const& host) {
  auto const kind = collection_kind(snippets);
  auto rows = BeginCard(snippets ? "settings.nav.snippets" : "settings.nav.quicklinks", "", host,
                        snippets ? "settings.col.add-snippet" : "settings.col.add-quicklink",
                        [weak = get_weak(), snippets] {
                          if (auto window = weak.get()) window->EditCollection(snippets, "", "", "", "");
                        });
  list_hosts_[kind] = rows;
  mxa::AutomationProperties::SetAutomationId(rows, to_wide("settings-list-" + kind));
  auto const cached = list_cache_.find(kind);
  if (cached != list_cache_.end()) ShowCollections(snippets, rows, cached->second);
  LoadCollections(snippets, rows);
}

void SettingsWindow::LoadCollections(bool snippets, mxc::StackPanel const& rows) {
  if (api_ == nullptr) return;
  auto api = api_;
  off_thread<Rows>(*this, attempt<Rows>([api, snippets] {
                     return snippets ? api->snippet_list().get() : api->quicklink_list().get();
                   }),
      [snippets, rows](SettingsWindow& window, Outcome<Rows> out) {
        if (!out.error.empty()) {
          window.ShowError(window.RefusalText(out.error, "settings.col.failed", {}));
          return;
        }
        window.list_cache_[collection_kind(snippets)] = out.value;
        window.ShowCollections(snippets, rows, out.value);
      });
}

void SettingsWindow::ShowCollections(bool snippets, mxc::StackPanel const& rows, Rows items) {
  auto const host = list_hosts_.find(collection_kind(snippets));
  if (host == list_hosts_.end() || host->second != rows) return;
  rows.Children().Clear();
  if (items.empty()) {
    auto empty = make_text(T(snippets ? "settings.col.empty-snippets" : "settings.col.empty-quicklinks"),
                           0, true);
    empty.Margin(mx::Thickness{0, 10, 0, 10});
    rows.Children().Append(empty);
  }
  for (auto const& item : items) {
    // Rows: (id name keyword body kind id "" "").
    if (item.size() < 4) continue;
    auto const& id = item[0];
    auto const& name = item[1];
    auto grid = make_row();
    auto labels = make_labels(to_wide(name));
    if (!item[3].empty()) {
      labels.Children().Append(make_text(to_wide(item[2] + (item[2].empty() ? "" : " · ") + item[3]), 12, true));
    }
    grid.Children().Append(labels);
    auto actions = mxc::StackPanel{};
    actions.Orientation(mxc::Orientation::Horizontal);
    actions.Spacing(8);
    auto edit = make_button(T("settings.list.edit"));
    edit.Click([weak = get_weak(), snippets, id, item](auto const&, auto const&) {
      if (auto window = weak.get()) {
        window->EditCollection(snippets, id, item[1], item[2], item[3]);
      }
    });
    actions.Children().Append(edit);
    auto remove = make_button(T("settings.list.remove"));
    remove.Click([weak = get_weak(), snippets, id, name = item[1]](auto const&, auto const&) {
      if (auto window = weak.get()) window->DeleteCollection(snippets, id, name);
    });
    actions.Children().Append(remove);
    actions.VerticalAlignment(mx::VerticalAlignment::Center);
    mxc::Grid::SetColumn(actions, 1);
    grid.Children().Append(actions);
    rows.Children().Append(grid);
  }
}

void SettingsWindow::EditCollection(bool snippets, std::string const& id, std::string const& name,
                                    std::string const& keyword, std::string const& body) {
  EditCollectionDialog(snippets, id, name, keyword, body, L"");
}

void SettingsWindow::EditCollectionDialog(bool snippets, std::string const& id,
                                          std::string const& name, std::string const& keyword,
                                          std::string const& body, std::wstring error) {
  // The backend has no group field for these lists, so the host keeps them
  // flat, like the prototype's collection cards.
  DialogField name_field;
  name_field.label = T("settings.col.name");
  name_field.value = to_wide(name);
  DialogField keyword_field;
  keyword_field.label = T("settings.col.keyword");
  keyword_field.value = to_wide(keyword);
  keyword_field.help = T("settings.col.keyword-help");
  DialogField content_field;
  content_field.label = T(snippets ? "settings.col.body" : "settings.col.url");
  content_field.value = to_wide(body);
  content_field.help = snippets ? L"" : T("settings.col.url-help");
  content_field.multiline = snippets;
  auto const lang = language_;
  ShowForm(T(snippets ? (id.empty() ? "settings.col.add-snippet" : "settings.col.edit-snippet")
                     : (id.empty() ? "settings.col.add-quicklink" : "settings.col.edit-quicklink")),
           {name_field, keyword_field, content_field}, "settings.save", std::move(error),
           [snippets, lang](FormDialog& form) {
             auto const fail = [&form, lang](char const* key) {
               form.error.Text(translated_setting(lang, key));
               form.error.Visibility(mx::Visibility::Visible);
             };
             for (auto const& box : form.boxes) {
               if (to_utf8(trimmed(std::wstring(box.Text()))).empty()) {
                 fail("settings.col.required");
                 return false;
               }
             }
             if (!snippets && !plausible_target(to_utf8(trimmed(std::wstring(form.boxes[2].Text()))))) {
               fail("settings.col.invalid-url");
               return false;
             }
             return true;
           },
           [weak = get_weak(), snippets, id](std::vector<std::string> values) {
             if (auto window = weak.get()) {
               window->SaveCollection(snippets, id, values[0], values[1], values[2]);
             }
           });
}

void SettingsWindow::SaveCollection(bool snippets, std::string const& id, std::string const& name,
                                    std::string const& keyword, std::string const& body) {
  if (api_ == nullptr) return;
  auto api = api_;
  off_thread<std::string>(*this, attempt<std::string>([api, snippets, id, name, keyword, body] {
                     return snippets ? api->snippet_save(id, name, keyword, body).get()
                                     : api->quicklink_save(id, name, keyword, body).get();
                   }),
      [snippets, id, name, keyword, body](SettingsWindow& window, Outcome<std::string> out) {
        if (!out.error.empty()) {
          if (id.empty()) {
            window.EditCollectionDialog(snippets, id, name, keyword, body,
                                        window.RefusalText(out.error, "settings.col.failed", {}));
          } else {
            window.ShowError(window.RefusalText(out.error, "settings.col.failed", {}));
          }
          return;
        }
        window.ClearError();
        auto const host = window.list_hosts_.find(collection_kind(snippets));
        if (host != window.list_hosts_.end()) window.LoadCollections(snippets, host->second);
      });
}

void SettingsWindow::DeleteCollection(bool snippets, std::string const& id, std::string const& name) {
  ConfirmThen(T("settings.list.remove"), frame0(T("settings.col.delete-confirm"), to_wide(name)),
              "settings.list.remove", [weak = get_weak(), snippets, id] {
                if (auto window = weak.get()) window->RemoveCollection(snippets, id);
              });
}

void SettingsWindow::RemoveCollection(bool snippets, std::string const& id) {
  if (api_ == nullptr) return;
  auto api = api_;
  off_thread<bool>(*this, attempt<bool>([api, snippets, id] {
                     return snippets ? api->snippet_delete(id).get() : api->quicklink_delete(id).get();
                   }),
      [snippets](SettingsWindow& window, Outcome<bool> out) {
        if (!out.error.empty()) {
          window.ShowError(window.RefusalText(out.error, "settings.col.failed", {}));
          return;
        }
        window.ClearError();
        auto const host = window.list_hosts_.find(collection_kind(snippets));
        if (host != window.list_hosts_.end()) window.LoadCollections(snippets, host->second);
      });
}

// ---- extensions ----------------------------------------------------------------

void SettingsWindow::BuildPlugins(mxc::StackPanel const& host) {
  // Per-plugin switches; the on/off set lives in the plugins-disabled
  // setting, so the state survives a relaunch.
  auto rows = BeginCard("", "settings.ext.help", host, "", nullptr);
  list_hosts_["plugins"] = rows;
  mxa::AutomationProperties::SetAutomationId(rows, L"settings-list-plugins");
  auto const cached = list_cache_.find("plugins");
  if (cached != list_cache_.end()) ShowPlugins(rows, cached->second);
  LoadPlugins(rows);
}

void SettingsWindow::LoadPlugins(mxc::StackPanel const& rows) {
  if (api_ == nullptr) return;
  auto api = api_;
  off_thread<Rows>(*this, attempt<Rows>([api] { return api->plugins_list().get(); }),
      [rows](SettingsWindow& window, Outcome<Rows> out) {
        if (!out.error.empty()) {
          window.ShowError(frame0(window.T("settings.list.failed"), to_wide(out.error)));
          return;
        }
        window.list_cache_["plugins"] = out.value;
        window.ShowPlugins(rows, out.value);
      });
}

void SettingsWindow::ShowPlugins(mxc::StackPanel const& rows, Rows plugins) {
  auto const host = list_hosts_.find("plugins");
  if (host == list_hosts_.end() || host->second != rows) return;
  rows.Children().Clear();
  bool shown = false;
  for (auto const& item : plugins) {
    // Rows: (id name version commands id "plugin" "" ""); a load error has
    // an empty id and its message in the name column.
    if (item.size() < 4) continue;
    if (item[0].empty()) {
      auto error = make_text(to_wide(item[1]));
      error.Foreground(ErrorBrush());
      error.Margin(mx::Thickness{0, 8, 0, 8});
      rows.Children().Append(error);
      continue;
    }
    shown = true;
    auto grid = make_row();
    auto labels = make_labels(to_wide(item[1]));
    labels.Children().Append(make_text(
        frame0(T("settings.ext.commands"), to_wide(item[3])) +
            (item[2].empty() ? L"" : L" · " + to_wide(item[2])),
        12, true));
    grid.Children().Append(labels);
    auto toggle = mxc::ToggleSwitch{};
    toggle.OnContent(winrt::box_value(L""));
    toggle.OffContent(winrt::box_value(L""));
    toggle.MinWidth(0);
    auto const disabled = split(values_.count("plugins-disabled") ? values_["plugins-disabled"] : "", ',');
    bool off = false;
    for (auto const& id : disabled) off = off || id == item[0];
    toggle.IsOn(!off);
    mxa::AutomationProperties::SetName(toggle, to_wide(item[1]));
    toggle.Toggled([weak = get_weak(), id = item[0]](wf::IInspectable const& sender,
                                                     mx::RoutedEventArgs const&) {
      if (auto window = weak.get()) {
        window->SetPluginEnabled(id, sender.as<mxc::ToggleSwitch>().IsOn());
      }
    });
    toggle.VerticalAlignment(mx::VerticalAlignment::Center);
    mxc::Grid::SetColumn(toggle, 1);
    grid.Children().Append(toggle);
    rows.Children().Append(grid);
  }
  if (!shown && plugins.empty()) {
    auto empty = make_text(T("settings.ext.empty"), 0, true);
    empty.Margin(mx::Thickness{0, 10, 0, 10});
    rows.Children().Append(empty);
  }
}

void SettingsWindow::SetPluginEnabled(std::string const& id, bool enabled) {
  if (api_ == nullptr) return;
  auto api = api_;
  off_thread<bool>(*this, attempt<bool>([api, id, enabled] {
                     return api->plugin_set_enabled(id, enabled ? "on" : "off").get();
                   }),
      [id, enabled](SettingsWindow& window, Outcome<bool> out) {
        if (!out.error.empty()) {
          window.ShowError(frame0(window.T("settings.list.failed"), to_wide(out.error)));
          auto const host = window.list_hosts_.find("plugins");
          if (host != window.list_hosts_.end()) window.LoadPlugins(host->second);
          return;
        }
        window.ClearError();
        // Mirror the backend's disabled set so a repaint keeps the switch.
        auto const disabled = split(window.values_["plugins-disabled"], ',');
        std::string next;
        for (auto const& known : disabled) {
          if (known.empty() || known == id) continue;
          if (!next.empty()) next += ",";
          next += known;
        }
        if (!enabled) {
          if (!next.empty()) next += ",";
          next += id;
        }
        window.values_["plugins-disabled"] = next;
      });
}

// ---- AI key, history, transfers ------------------------------------------------

void SettingsWindow::BuildAiKey(mxc::StackPanel const& host) {
  // The key rides the ai.key action and never comes back over the wire;
  // ai-status reports readiness instead.
  auto rows = BeginCard("settings.ai-api-key", "settings.hint.ai-keys", host, "", nullptr);
  auto grid = make_row();
  // The card title already names the row; the label is the readiness state.
  ai_status_text_ = make_text(L"", 0, false);
  RefreshAiStatusText();
  auto labels = make_labels(L"");
  labels.Children().Clear();
  labels.Children().Append(ai_status_text_);
  grid.Children().Append(labels);
  auto actions = mxc::StackPanel{};
  actions.Orientation(mxc::Orientation::Horizontal);
  actions.Spacing(8);
  key_box_ = mxc::PasswordBox{};
  key_box_.MinWidth(220);
  key_box_.PlaceholderText(T("settings.ai-key.placeholder"));
  mxa::AutomationProperties::SetAutomationId(key_box_, L"settings-ai-key");
  key_box_.KeyDown({this, &SettingsWindow::Key_KeyDown});
  actions.Children().Append(key_box_);
  auto save = make_button(T("settings.save"));
  mxa::AutomationProperties::SetAutomationId(save, L"settings-ai-key-save");
  save.Click({this, &SettingsWindow::SaveKey_Click});
  actions.Children().Append(save);
  actions.VerticalAlignment(mx::VerticalAlignment::Center);
  mxc::Grid::SetColumn(actions, 1);
  grid.Children().Append(actions);
  rows.Children().Append(grid);
  key_notice_ = make_text(L"", 12, true);
  key_notice_.Visibility(mx::Visibility::Collapsed);
  key_notice_.Margin(mx::Thickness{0, 6, 0, 8});
  rows.Children().Append(key_notice_);
}

void SettingsWindow::BuildClearButton(mxc::StackPanel const& host, std::string const& title_key,
                                      std::string const& confirm_key, bool clipboard) {
  // Clearing history is irreversible for the user: the host asks once.
  auto button = make_button(T(title_key));
  button.Margin(mx::Thickness{0, 18, 0, 0});
  button.HorizontalAlignment(mx::HorizontalAlignment::Left);
  mxa::AutomationProperties::SetAutomationId(
      button, clipboard ? L"settings-clipboard-clear" : L"settings-privacy-clear");
  button.Click([weak = get_weak(), title_key, confirm_key, clipboard](auto const&, auto const&) {
    if (auto window = weak.get()) {
      (*window).ConfirmThen((*window).T(title_key), (*window).T(confirm_key), title_key,
                          [weak, clipboard] {
                            if (auto live = weak.get()) (*live).ClearHistory(clipboard);
                          });
    }
  });
  host.Children().Append(button);
}

void SettingsWindow::ClearHistory(bool clipboard) {
  if (api_ == nullptr) return;
  auto api = api_;
  off_thread<std::int64_t>(*this, attempt<std::int64_t>([api, clipboard] {
                     return clipboard ? api->clipboard_clear().get()
                                      : (api->history_clear().get() ? 1 : 0);
                   }),
      [clipboard](SettingsWindow& window, Outcome<std::int64_t> out) {
        if (!out.error.empty()) {
          window.ShowError(frame0(window.T("settings.col.failed"), to_wide(out.error)));
          return;
        }
        window.ClearError();
        window.Notice(clipboard ? frame0(window.T("settings.clipboard.cleared"),
                                         std::to_wstring(out.value))
                                : window.T("settings.privacy.cleared"));
      });
}

void SettingsWindow::BuildTransfer(mxc::StackPanel const& host) {
  // Choose what to include. Import shows a summary before applying changes.
  host.Children().Append(make_hint(T("settings.sync.transfer-help")));
  auto actions = mxc::StackPanel{};
  actions.Orientation(mxc::Orientation::Horizontal);
  actions.Spacing(8);
  auto export_button = make_button(T("settings.sync.export"));
  mxa::AutomationProperties::SetAutomationId(export_button, L"settings-sync-export");
  export_button.Click([weak = get_weak()](auto const&, auto const&) {
    if (auto window = weak.get()) window->ExportDialog();
  });
  actions.Children().Append(export_button);
  auto import_button = make_button(T("settings.sync.import"));
  mxa::AutomationProperties::SetAutomationId(import_button, L"settings-sync-import");
  import_button.Click([weak = get_weak()](auto const&, auto const&) {
    if (auto window = weak.get()) window->ImportConfiguration();
  });
  actions.Children().Append(import_button);
  host.Children().Append(actions);
  // Updates keep the dedicated update window (update-flow parity).
  host.Children().Append(make_hint(T("settings.updates-note.win")));
  auto check = make_button(T("settings.check-updates-now"));
  check.HorizontalAlignment(mx::HorizontalAlignment::Left);
  mxa::AutomationProperties::SetAutomationId(check, L"settings-update-check");
  check.Click({this, &SettingsWindow::CheckUpdatesNow_Click});
  host.Children().Append(check);
}

void SettingsWindow::ExportDialog() {
  struct Category {
    char const* id;
    char const* key;
  };
  auto const categories = std::vector<Category>{
      {"appearance", "settings.transfer.appearance"}, {"features", "settings.transfer.features"},
      {"commands", "settings.transfer.commands"}, {"content", "settings.transfer.content"}};
  auto panel = mxc::StackPanel{};
  panel.Spacing(8);
  panel.MinWidth(320);
  std::vector<mxc::CheckBox> boxes;
  for (auto const& category : categories) {
    mxc::CheckBox box;
    box.Content(winrt::box_value(T(category.key)));
    box.IsChecked(true);
    panel.Children().Append(box);
    boxes.push_back(box);
  }
  auto dialog = mxc::ContentDialog{};
  dialog.Title(winrt::box_value(T("settings.sync.export")));
  dialog.Content(panel);
  dialog.PrimaryButtonText(T("settings.sync.export"));
  dialog.CloseButtonText(T("settings.cancel"));
  dialog.DefaultButton(mxc::ContentDialogButton::Primary);
  dialog.XamlRoot(Content().as<mx::FrameworkElement>().XamlRoot());
  auto const weak = get_weak();
  ShowDialogThen(dialog, [weak, categories, boxes](mxc::ContentDialogResult result) {
    if (result != mxc::ContentDialogResult::Primary) return;
    std::string wanted;
    for (std::size_t i = 0; i < categories.size(); ++i) {
      auto const checked = boxes[i].IsChecked();
      if (!checked || !checked.Value()) continue;
      if (!wanted.empty()) wanted += ",";
      wanted += categories[i].id;
    }
    if (wanted.empty()) return;
    if (auto window = weak.get()) window->PickSaveFileThen([weak, wanted](std::string path) {
      if (auto live = weak.get()) (*live).ExportConfiguration(wanted, path);
    });
  });
}

void SettingsWindow::ExportConfiguration(std::string categories, std::string path) {
  if (api_ == nullptr) return;
  auto api = api_;
  off_thread<bool>(*this, attempt<bool>([api, categories, path] {
    auto const payload = api->settings_export(categories).get();
    // The backend is not given a path: the host writes the file it picked.
    std::ofstream out(std::filesystem::path(to_wide(path)), std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("file");
    out << payload;
    out.close();
    if (!out) throw std::runtime_error("file");
    return true;
  }),
      [path](SettingsWindow& window, Outcome<bool> out) {
        window.ClearError();
        if (out.error == "file") {
          window.ShowError(window.T("settings.sync.file-failed"));
        } else if (!out.error.empty()) {
          window.ShowError(frame0(window.T("settings.sync.import-failed"), to_wide(out.error)));
        } else {
          window.Notice(frame0(window.T("settings.sync.exported"), to_wide(path)));
        }
      });
}

void SettingsWindow::ImportConfiguration() {
  PickOpenFileThen([weak = get_weak()](std::string path) {
    if (auto window = weak.get()) {
      std::ifstream in(std::filesystem::path(to_wide(path)), std::ios::binary);
      std::string payload((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      if (!in) {
        (*window).ShowError((*window).T("settings.sync.file-failed"));
        return;
      }
      (*window).RunImport(payload);
    }
  });
}

void SettingsWindow::RunImport(std::string payload) {
  if (api_ == nullptr) return;
  auto api = api_;
  // First ask the backend what the file would replace; nothing changes yet.
  off_thread<std::string>(*this, attempt<std::string>([api, payload] {
                     return api->settings_import(payload, "off").get();
                   }),
      [payload, api](SettingsWindow& window, Outcome<std::string> out) {
        // "plan:<category>:<count>,…" (content counts read "snippets+quicklinks").
        if (!out.error.empty() || out.value.rfind("plan:", 0) != 0) {
          window.ShowError(frame0(window.T("settings.sync.import-failed"),
                                  to_wide(out.error.empty() ? out.value : out.error)));
          return;
        }
        std::wstring body;
        for (auto const& part : split(out.value.substr(5), ',')) {
          auto const colon = part.find(':');
          if (colon == std::string::npos) continue;
          body += window.T("settings.transfer." + part.substr(0, colon)) + L": " +
                  to_wide(part.substr(colon + 1)) + L"\n";
        }
        auto const weak = window.get_weak();
        window.ConfirmThen(window.T("settings.sync.import-title"), body,
                           "settings.sync.import-apply", [weak, api, payload] {
          if (auto live = weak.get()) {
            off_thread<std::string>(*live, attempt<std::string>([api, payload] {
                               return api->settings_import(payload, "on").get();
                             }),
                [](SettingsWindow& target, Outcome<std::string> applied) {
                  if (!applied.error.empty()) {
                    target.ShowError(frame0(target.T("settings.sync.import-failed"),
                                            to_wide(applied.error)));
                    return;
                  }
                  target.ClearError();
                  target.Notice(target.T("settings.sync.imported"));
                  // The lists and rows now hold other content: drop what
                  // was painted and read everything back.
                  target.list_cache_.clear();
                  target.Load();
                });
          }
        });
      });
}

// ---- dialogs and pickers -------------------------------------------------------

// Shows `dialog` in this window and hands the button the user pressed to
// `then`, on the UI thread, if the window is still alive.
winrt::fire_and_forget SettingsWindow::ShowDialogThen(
    mxc::ContentDialog const& dialog, std::function<void(mxc::ContentDialogResult)> then) {
  auto const weak = get_weak();
  auto result = mxc::ContentDialogResult::None;
  try {
    result = co_await dialog.ShowAsync();
  } catch (...) {
    // Another dialog is already open in this window: the request is dropped.
  }
  if (auto window = weak.get()) then(result);
}

void SettingsWindow::ConfirmThen(std::wstring title, std::wstring body, std::string primary_key,
                                 std::function<void()> then) {
  auto dialog = mxc::ContentDialog{};
  dialog.Title(winrt::box_value(title));
  dialog.Content(make_text(body));
  dialog.PrimaryButtonText(T(primary_key));
  dialog.CloseButtonText(T("settings.cancel"));
  // The safe answer is the default: Enter on a fresh dialog cancels.
  dialog.DefaultButton(mxc::ContentDialogButton::Close);
  dialog.XamlRoot(Content().as<mx::FrameworkElement>().XamlRoot());
  ShowDialogThen(dialog, [then = std::move(then)](mxc::ContentDialogResult result) {
    if (result == mxc::ContentDialogResult::Primary) then();
  });
}

void SettingsWindow::ShowForm(
    std::wstring title, std::vector<DialogField> fields, std::string primary_key, std::wstring error,
    std::function<bool(FormDialog&)> validate, std::function<void(std::vector<std::string>)> then) {
  auto panel = mxc::StackPanel{};
  panel.Spacing(6);
  panel.MinWidth(380);
  panel.MaxWidth(480);
  auto form = std::make_shared<FormDialog>();
  for (auto const& field : fields) {
    if (!field.label.empty()) panel.Children().Append(make_heading(field.label));
    auto box = mxc::TextBox{};
    box.Text(field.value);
    box.PlaceholderText(field.placeholder);
    box.AcceptsReturn(field.multiline);
    box.TextWrapping(field.multiline ? mx::TextWrapping::Wrap : mx::TextWrapping::NoWrap);
    if (field.multiline) box.Height(120);
    if (field.browse) {
      auto line = make_row();
      line.MinHeight(0);
      line.Children().Append(box);
      auto browse = make_button(T("settings.browse"));
      browse.Click([weak = get_weak(), box](auto const&, auto const&) {
        if (auto window = weak.get()) {
          window->PickFolderThen([box](std::string path) { box.Text(to_wide(path)); });
        }
      });
      mxc::Grid::SetColumn(browse, 1);
      browse.Margin(mx::Thickness{8, 0, 0, 0});
      line.Children().Append(browse);
      panel.Children().Append(line);
    } else {
      panel.Children().Append(box);
    }
    if (!field.help.empty()) panel.Children().Append(make_text(field.help, 12, true));
    form->boxes.push_back(box);
  }
  form->error = make_text(error);
  form->error.Foreground(ErrorBrush());
  form->error.Visibility(error.empty() ? mx::Visibility::Collapsed : mx::Visibility::Visible);
  panel.Children().Append(form->error);
  auto dialog = mxc::ContentDialog{};
  dialog.Title(winrt::box_value(title));
  dialog.Content(panel);
  dialog.PrimaryButtonText(T(primary_key));
  dialog.CloseButtonText(T("settings.cancel"));
  dialog.DefaultButton(mxc::ContentDialogButton::Primary);
  dialog.XamlRoot(Content().as<mx::FrameworkElement>().XamlRoot());
  form->dialog = dialog;
  // A form with an empty required field stays open and says which one.
  dialog.PrimaryButtonClick([validate, form](auto const&, mxc::ContentDialogButtonClickEventArgs const& args) {
    form->error.Text(L"");
    form->error.Visibility(mx::Visibility::Collapsed);
    if (!validate(*form)) args.Cancel(true);
  });
  auto const weak = get_weak();
  ShowDialogThen(dialog, [weak, form, then = std::move(then)](mxc::ContentDialogResult result) {
    if (result != mxc::ContentDialogResult::Primary) return;
    if (auto window = weak.get()) {
      std::vector<std::string> values;
      for (auto const& box : form->boxes) {
        values.push_back(to_utf8(trimmed(std::wstring(box.Text()))));
      }
      then(std::move(values));
    }
  });
}

winrt::fire_and_forget SettingsWindow::PickFolderThen(std::function<void(std::string)> then) {
  namespace pickers = winrt::Windows::Storage::Pickers;
  auto const weak = get_weak();
  HWND hwnd = nullptr;
  try {
    hwnd = rivet::system::NativeWindowHandle(*this);
  } catch (...) {
  }
  auto folder = winrt::Windows::Storage::StorageFolder{nullptr};
  try {
    pickers::FolderPicker picker;
    // Desktop apps must tell the picker which window owns it.
    picker.as<::IInitializeWithWindow>()->Initialize(hwnd);
    picker.FileTypeFilter().Append(L"*");
    folder = co_await picker.PickSingleFolderAsync();
  } catch (...) {
    folder = nullptr;
  }
  if (auto window = weak.get(); window && folder != nullptr) {
    then(to_utf8(std::wstring(folder.Path())));
  }
}

winrt::fire_and_forget SettingsWindow::PickSaveFileThen(std::function<void(std::string)> then) {
  namespace pickers = winrt::Windows::Storage::Pickers;
  auto const weak = get_weak();
  HWND hwnd = nullptr;
  try {
    hwnd = rivet::system::NativeWindowHandle(*this);
  } catch (...) {
  }
  auto file = winrt::Windows::Storage::StorageFile{nullptr};
  try {
    pickers::FileSavePicker picker;
    picker.as<::IInitializeWithWindow>()->Initialize(hwnd);
    picker.FileTypeChoices()
        .Insert(L"JSON", winrt::single_threaded_vector<winrt::hstring>(std::vector<winrt::hstring>{L".json"}));
    picker.SuggestedFileName(L"fulcrum-settings");
    picker.DefaultFileExtension(L".json");
    file = co_await picker.PickSaveFileAsync();
  } catch (...) {
    file = nullptr;
  }
  if (auto window = weak.get(); window && file != nullptr) {
    then(to_utf8(std::wstring(file.Path())));
  }
}

winrt::fire_and_forget SettingsWindow::PickOpenFileThen(std::function<void(std::string)> then) {
  namespace pickers = winrt::Windows::Storage::Pickers;
  auto const weak = get_weak();
  HWND hwnd = nullptr;
  try {
    hwnd = rivet::system::NativeWindowHandle(*this);
  } catch (...) {
  }
  auto file = winrt::Windows::Storage::StorageFile{nullptr};
  try {
    pickers::FileOpenPicker picker;
    picker.as<::IInitializeWithWindow>()->Initialize(hwnd);
    picker.FileTypeFilter().Append(L".json");
    picker.SuggestedStartLocation(pickers::PickerLocationId::DocumentsLibrary);
    file = co_await picker.PickSingleFileAsync();
  } catch (...) {
    file = nullptr;
  }
  if (auto window = weak.get(); window && file != nullptr) {
    then(to_utf8(std::wstring(file.Path())));
  }
}

// ---- settings search -----------------------------------------------------------

void SettingsWindow::Search_TextChanged(wf::IInspectable const&, mxc::TextChangedEventArgs const&) {
  if (jumping_) return;
  RenderSearch();
}

void SettingsWindow::Search_KeyDown(wf::IInspectable const&, mx::Input::KeyRoutedEventArgs const& args) {
  auto const key = args.Key();
  if (key == winrt::Windows::System::VirtualKey::Escape && !jumping_) {
    // Esc leaves search and returns to the page that was showing.
    jumping_ = true;
    SettingsSearch().Text(L"");
    jumping_ = false;
    NavigateTo(current_page_);
    SettingsSearch().Focus(mx::FocusState::Programmatic);
    args.Handled(true);
  } else if (key == winrt::Windows::System::VirtualKey::Down) {
    if (SearchResults().Children().Size() > 0) {
      auto first = SearchResults().Children().GetAt(0).try_as<mxc::Button>();
      if (first != nullptr) first.Focus(mx::FocusState::Programmatic);
    }
    args.Handled(true);
  }
}

void SettingsWindow::RenderSearch() {
  auto const query = to_lower(trimmed(std::wstring(SettingsSearch().Text())));
  search_hits_.clear();
  auto results = SearchResults();
  results.Children().Clear();
  if (query.empty()) {
    NavigateTo(current_page_);
    return;
  }
  // Local filtering over the labels and hints the window already holds.
  for (auto const& page : pages_) {
    auto const page_title = T("settings.nav." + page.id);
    if (contains(to_lower(page_title), query)) {
      search_hits_.emplace_back(page.id, std::string());
      results.Children().Append(
          MakeSearchResult(page.id, "", page_title, T("settings.page." + page.id)));
    }
    for (auto const& section : page.sections) {
      for (auto const& row : section.second) {
        auto const label = T("settings." + row.key);
        if (!contains(to_lower(label + L" " + T("settings.hint." + row.key)), query)) continue;
        search_hits_.emplace_back(page.id, row.key);
        results.Children().Append(MakeSearchResult(
            page.id, row.key, label,
            page_title + L" / " + T("settings.section." + row.section)));
      }
    }
  }
  SchemaSection().Visibility(mx::Visibility::Collapsed);
  CommandsSection().Visibility(mx::Visibility::Collapsed);
  WorkflowsSection().Visibility(mx::Visibility::Collapsed);
  SearchSection().Visibility(mx::Visibility::Visible);
  ClearError();
  NoticeText().Visibility(mx::Visibility::Collapsed);
  if (search_hits_.empty()) {
    SearchTitle().Text(T("settings.search.empty"));
    results.Children().Append(make_text(T("settings.search.empty-help"), 0, true));
  } else {
    SearchTitle().Text(frame0(T("settings.search.results"), std::to_wstring(search_hits_.size())));
  }
}

mx::UIElement SettingsWindow::MakeSearchResult(std::string const& page, std::string const& key,
                                               std::wstring const& label, std::wstring const& help) {
  // One result: "row label" over "page / section"; a page hit has no second line.
  auto button = mxc::Button{};
  button.Style(Content().as<mx::FrameworkElement>()
                   .Resources()
                   .Lookup(winrt::box_value(L"SearchResult"))
                   .as<mx::Style>());
  auto panel = mxc::StackPanel{};
  panel.Spacing(2);
  panel.Children().Append(make_heading(label));
  if (!help.empty()) panel.Children().Append(make_text(help, 12, true));
  button.Content(panel);
  mxa::AutomationProperties::SetName(button, label);
  button.Click([weak = get_weak(), page, key](auto const&, auto const&) {
    if (auto window = weak.get()) window->JumpToRow(page, key);
  });
  return button;
}

void SettingsWindow::JumpToRow(std::string const& page, std::string const& key) {
  // Selecting the page rebuilds its rows; the highlight follows once laid out.
  jumping_ = true;
  SettingsSearch().Text(L"");
  jumping_ = false;
  auto const item = nav_items_.find(page);
  if (item != nav_items_.end()) SettingsNavigation().SelectedItem(item->second);
  NavigateTo(page);
  if (key.empty()) return;
  auto const row = row_roots_.find(key);
  if (row == row_roots_.end()) return;
  highlight_row_ = row->second.as<mxc::Grid>();
  auto const weak = get_weak();
  DispatcherQueue().TryEnqueue([weak, key] {
    if (auto window = weak.get()) {
      auto const target = window->row_roots_.find(key);
      if (target == window->row_roots_.end()) return;
      target->second.StartBringIntoView();
      window->highlight_row_.Background(mx::Media::SolidColorBrush{
          winrt::Windows::UI::ColorHelper::FromArgb(48, 0, 120, 212)});
      window->highlight_timer_.Interval(winrt::Windows::Foundation::TimeSpan{
          std::chrono::milliseconds{1800}});
      window->highlight_timer_.Start();
    }
  });
}

void SettingsWindow::Highlight_Tick(wf::IInspectable const&, wf::IInspectable const&) {
  highlight_timer_.Stop();
  if (highlight_row_ != nullptr) highlight_row_.Background(nullptr);
}


void SettingsWindow::SaveKey() {
  if (api_ == nullptr || key_box_ == nullptr) {
    return;
  }
  auto const key = trimmed(to_utf8(std::wstring(key_box_.Password())));
  if (key.empty()) {
    return;
  }
  // The key goes to the backend's ai.key action and is never read back; the
  // status line says whether the provider is now ready.
  auto api = api_;
  off_thread<std::string>(*this, attempt<std::string>([api, key] { return api->run_action("ai.key", key).get(); }),
      [](SettingsWindow& window, Outcome<std::string> out) {
        if (window.key_box_ != nullptr) window.key_box_.Password(L"");
        if (window.key_notice_ == nullptr) return;
        auto const ok = out.error.empty() && out.value == "ok";
        window.key_notice_.Text(ok ? window.T("settings.keys.saved")
                                   : to_wide(out.error.empty() ? out.value
                                                               : std::string("settings.ai-key.failed")));
        if (!out.error.empty()) window.key_notice_.Text(window.T("settings.ai-key.failed"));
        window.key_notice_.Visibility(mx::Visibility::Visible);
        window.RefreshAiStatus();
      });
}

void SettingsWindow::CommitText(mxc::TextBox const& box) {
  if (loading_ || box == nullptr) {
    return;
  }
  auto const key = tag_key(box);
  auto const value = trimmed(to_utf8(std::wstring(box.Text())));
  auto const it = values_.find(key);
  if (it != values_.end() && it->second == value) {
    return;
  }
  Write(key, value);
}

// ---- XAML handlers ----------------------------------------------------------

void SettingsWindow::Toggle_Toggled(
    wf::IInspectable const& sender,
    mx::RoutedEventArgs const&) {
  if (loading_) {
    return;
  }
  auto toggle = sender.try_as<mxc::ToggleSwitch>();
  if (toggle == nullptr) {
    return;
  }
  if (tag_key(sender) == "local-login-startup") {
    // Machine-local startup state never travels over settings-set.
    if (!fulcrum::windows::SetAutostart(toggle.IsOn())) {
      ShowError(T("settings.login.failed.win"));
      loading_ = true;
      toggle.IsOn(fulcrum::windows::AutostartEnabled());
      loading_ = false;
    } else {
      ClearError();
    }
    return;
  }
  Write(tag_key(sender), toggle.IsOn() ? "on" : "off");
}

void SettingsWindow::Choice_Changed(
    wf::IInspectable const& sender,
    mxc::SelectionChangedEventArgs const&) {
  if (loading_) {
    return;
  }
  auto box = sender.try_as<mxc::ComboBox>();
  if (box == nullptr || box.SelectedItem() == nullptr) {
    return;
  }
  Write(tag_key(sender), item_value(box.SelectedItem()));
}

void SettingsWindow::Number_Changed(
    mxc::NumberBox const& sender,
    mxc::NumberBoxValueChangedEventArgs const& args) {
  if (loading_) {
    return;
  }
  auto const value = args.NewValue();
  if (std::isnan(value)) {
    return;
  }
  Write(tag_key(sender), std::to_string(static_cast<long long>(value)));
}

void SettingsWindow::Text_LostFocus(
    wf::IInspectable const& sender,
    mx::RoutedEventArgs const&) {
  CommitText(sender.try_as<mxc::TextBox>());
}

void SettingsWindow::Text_KeyDown(
    wf::IInspectable const& sender,
    mx::Input::KeyRoutedEventArgs const& args) {
  if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
    CommitText(sender.try_as<mxc::TextBox>());
    args.Handled(true);
  }
}

void SettingsWindow::Key_KeyDown(
    wf::IInspectable const&,
    mx::Input::KeyRoutedEventArgs const& args) {
  if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
    SaveKey();
    args.Handled(true);
  }
}

void SettingsWindow::SaveKey_Click(
    wf::IInspectable const&,
    mx::RoutedEventArgs const&) {
  SaveKey();
}

void SettingsWindow::CheckUpdatesNow_Click(
    wf::IInspectable const&,
    mx::RoutedEventArgs const&) {
  // Update-flow parity: the Sync page button opens the same dedicated
  // update window as the tray entry and the launcher update rows. The
  // install handoff lives in MainWindow, not in this form.
  if (update_opener_) update_opener_();
}

}  // namespace winrt::RivetHost::implementation
