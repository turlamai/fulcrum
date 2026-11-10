#include "pch.h"
#include "SettingsWindow.xaml.h"
#include "Autostart.h"
#include "GeneratedI18n.hpp"
#if __has_include("SettingsWindow.g.cpp")
#include "SettingsWindow.g.cpp"
#endif

#include <cmath>
#include <thread>
#include <utility>

namespace winrt::RivetHost::implementation {
namespace {

namespace mx = winrt::Microsoft::UI::Xaml;
namespace mxc = winrt::Microsoft::UI::Xaml::Controls;

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
static std::string effective_language(std::string const& language) {
  if (language != "system") {
    return language;
  }
  return ((::GetUserDefaultUILanguage() & 0x3FF) == 0x0004) ? "zh" : "en";
}

std::wstring translated_setting(std::string const& language, std::string_view key) {
  std::string const effective = effective_language(language);
  for (auto const& row : rivet_app::kI18nTable) {
    if (row.key == key) {
      return to_wide(std::string(effective == "en" ? row.en : row.zh));
    }
  }
  return to_wide(std::string(key));
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

// The backend key a control carries in its Tag ("" when there is none).
std::string tag_key(winrt::Windows::Foundation::IInspectable const& sender) {
  auto element = sender.try_as<mx::FrameworkElement>();
  if (element == nullptr) return {};
  auto tag = element.Tag();
  if (tag == nullptr) return {};
  return to_utf8(std::wstring(winrt::unbox_value<winrt::hstring>(tag)));
}

// A ComboBoxItem's backend value lives in its Tag; the Off provider item
// carries the empty string, which XAML may hand back as a null Tag.
std::string item_value(winrt::Windows::Foundation::IInspectable const& item) {
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

}  // namespace

SettingsWindow::SettingsWindow() {
  InitializeComponent();
  loading_ = true;
  LoginStartupSwitch().IsOn(fulcrum::windows::AutostartEnabled());
  loading_ = false;
  Title(L"Fulcrum Settings");
  try {
    SystemBackdrop(mx::Media::MicaBackdrop());
    if (auto appWindow = this->AppWindow()) {
      appWindow.Resize(winrt::Windows::Graphics::SizeInt32{820, 580});
    }
    // Keep current-main's Rivet-owned icon fix: passing an EXE path to
    // SetIcon(String) was incorrect, and raw HICON ownership is unsafe.
    (void)rivet::system::ApplyAppIcon(*this);
  } catch (...) {
    // Native backdrop and titlebar adornments are optional.
  }
  SettingsNavigation().SelectedItem(SettingsNavigation().MenuItems().GetAt(0));
}

void SettingsWindow::NavigateTo(std::string const& section) {
  GeneralSection().Visibility(section == "general" ? mx::Visibility::Visible : mx::Visibility::Collapsed);
  FeaturesSection().Visibility(section == "features" ? mx::Visibility::Visible : mx::Visibility::Collapsed);
  AiSection().Visibility(section == "ai" ? mx::Visibility::Visible : mx::Visibility::Collapsed);
  CommandsSection().Visibility(section == "commands" ? mx::Visibility::Visible : mx::Visibility::Collapsed);
  SyncSection().Visibility(section == "sync" ? mx::Visibility::Visible : mx::Visibility::Collapsed);
  if (section == "commands") LoadBindings();
}

void SettingsWindow::Navigation_SelectionChanged(
    mxc::NavigationView const&,
    mxc::NavigationViewSelectionChangedEventArgs const& args) {
  if (auto item = args.SelectedItem().try_as<mxc::NavigationViewItem>()) {
    NavigateTo(tag_key(item));
  }
}

void SettingsWindow::Hotkey_KeyDown(
    winrt::Windows::Foundation::IInspectable const&,
    mx::Input::KeyRoutedEventArgs const& args) {
  if (loading_) return;
  auto const spec = recorded_hotkey(args.Key());
  if (spec == "<cancel>") {
    auto const it = values_.find("hotkey");
    HotkeyBox().Text(to_wide(it == values_.end() ? "" : it->second));
  } else if (!spec.empty()) {
    HotkeyBox().Text(to_wide(spec));
    Write("hotkey", spec);
  }
  args.Handled(true);
}

void SettingsWindow::Attach(std::shared_ptr<rivet_app::API> api) {
  api_ = std::move(api);
  Load();
  if (CommandsSection().Visibility() == mx::Visibility::Visible) LoadBindings();
}

void SettingsWindow::Load() {
  if (api_ == nullptr) {
    return;
  }
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto api = api_;
  std::thread([weak, api, dispatcher]() mutable {
    std::map<std::string, std::string> values;
    std::string status;
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
    dispatcher.TryEnqueue([weak, values = std::move(values), status]() mutable {
      if (auto window = weak.get()) {
        window->Apply(std::move(values), status);
      }
    });
  }).detach();
}

void SettingsWindow::Apply(std::map<std::string, std::string> values,
                           std::string ai_status) {
  values_ = std::move(values);
  loading_ = true;
  auto const get = [this](char const* key) -> std::string {
    auto const it = values_.find(key);
    return it == values_.end() ? std::string() : it->second;
  };
  ApplyLocalization(get("language"));
  select_choice(LanguageBox(), get("language"));
  select_choice(ThemeBox(), get("theme"));
  HotkeyBox().Text(to_wide(get("hotkey")));
  MaxResultsBox().Value(number_or(get("max-results"), 12));
  LoginStartupSwitch().IsOn(fulcrum::windows::AutostartEnabled());
  ClipboardSwitch().IsOn(get("clipboard-enabled") == "on");
  ClipboardLimitBox().Value(number_or(get("clipboard-limit"), 1000));
  WebSearchSwitch().IsOn(get("web-search-enabled") == "on");
  select_choice(DefaultBangBox(), get("default-engine"));
  ExpansionSwitch().IsOn(get("expansion-enabled") == "on");
  PluginsSwitch().IsOn(get("plugins-enabled") == "on");
  PluginTimeoutBox().Value(number_or(get("plugins-timeout-ms"), 2000));
  select_choice(ProviderBox(), get("ai-provider"));
  ModelBox().Text(to_wide(get("ai-model")));
  BaseUrlBox().Text(to_wide(get("ai-base-url")));
  PersonaBox().Text(to_wide(get("ai-persona")));
  SyncRootBox().Text(to_wide(get("sync-root")));
  AiStatusText().Text(
      to_wide(ai_status == "ready" ? std::string("Ready") : ai_status));
  loading_ = false;
}

void SettingsWindow::Write(std::string key, std::string value) {
  if (api_ == nullptr || key.empty()) {
    return;
  }
  values_[key] = value;
  if (key == "language") ApplyLocalization(value);
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto api = api_;
  std::thread([weak, api, dispatcher, key, value]() mutable {
    std::string error;
    try {
      if (!api->settings_set(key, value).get()) {
        error = key == "hotkey"
            ? "that shortcut is not supported"
            : "that value is not supported";
      }
    } catch (std::exception const& e) {
      error = e.what();
    } catch (...) {
      error = "settings-set failed";
    }
    dispatcher.TryEnqueue([weak, key, error]() mutable {
      if (auto window = weak.get()) {
        if (error.empty()) {
          window->ClearError();
          if (key == "ai-provider") {
            window->RefreshAiStatus();
          }
        } else {
          // The backend refused the value (grammar, range): say so and
          // fall back to what it actually holds.
          window->ShowError(to_wide(key + ": " + error));
          window->Load();
        }
      }
    });
  }).detach();
}

void SettingsWindow::ApplyLocalization(std::string const& language) {
  LanguageLabel().Text(translated_setting(language, "setting.language"));
  ThemeLabel().Text(translated_setting(language, "setting.theme"));
  HotkeyLabel().Text(translated_setting(language, "setting.hotkey"));
  MaxResultsLabel().Text(translated_setting(language, "setting.max-results"));
  ClipboardLabel().Text(translated_setting(language, "setting.clipboard-enabled"));
  WebSearchLabel().Text(translated_setting(language, "setting.web-search-enabled"));
  ExpansionLabel().Text(translated_setting(language, "setting.expansion-enabled"));
  PluginsLabel().Text(translated_setting(language, "setting.plugins-enabled"));
  ProviderLabel().Text(translated_setting(language, "setting.ai-provider"));
}

void SettingsWindow::RefreshAiStatus() {
  if (api_ == nullptr) {
    return;
  }
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto api = api_;
  std::thread([weak, api, dispatcher]() mutable {
    std::string status;
    try {
      status = api->ai_status().get();
    } catch (...) {
    }
    dispatcher.TryEnqueue([weak, status]() mutable {
      if (auto window = weak.get()) {
        window->AiStatusText().Text(
            to_wide(status == "ready" ? std::string("Ready") : status));
      }
    });
  }).detach();
}


void SettingsWindow::LoadBindings() {
  if (api_ == nullptr) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto api = api_;
  std::thread([weak, api, dispatcher]() mutable {
    std::vector<std::vector<std::string>> commands, bindings;
    try {
      commands = api->bindings_commands().get();
      bindings = api->bindings_list().get();
    } catch (...) {
      dispatcher.TryEnqueue([weak] {
        if (auto window = weak.get()) {
          window->BindingErrorText().Text(L"Could not load command bindings.");
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
    aliasBox.PlaceholderText(L"alias");
    row.Children().Append(aliasBox);
    mxc::TextBox hotkeyBox;
    hotkeyBox.Width(125);
    hotkeyBox.Text(to_wide(hotkey));
    hotkeyBox.PlaceholderText(L"Press keys");
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
    save.Content(winrt::box_value(L"Save"));
    save.Click([weak = get_weak(), id, aliasBox, hotkeyBox](auto const&, auto const&) {
      if (auto window = weak.get())
        window->SaveBinding(id, trimmed(to_utf8(std::wstring(aliasBox.Text()))),
                             to_utf8(std::wstring(hotkeyBox.Text())));
    });
    row.Children().Append(save);
    mxc::Button clear;
    clear.Content(winrt::box_value(L"Clear"));
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
  std::thread([weak, api, dispatcher, id, alias, hotkey]() mutable {
    std::string error;
    try {
      if (!api->binding_set(id, alias, hotkey).get())
        error = "Binding rejected: shortcut already used, summon key, or unsupported command.";
    } catch (std::exception const& e) { error = e.what(); }
    catch (...) { error = "Could not save command binding."; }
    dispatcher.TryEnqueue([weak, error] {
      if (auto window = weak.get()) {
        window->BindingErrorText().Text(to_wide(error));
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
  std::thread([weak, api, dispatcher, id]() mutable {
    std::string error;
    try {
      if (!api->binding_clear(id).get()) error = "Could not clear command binding.";
    } catch (std::exception const& e) { error = e.what(); }
    catch (...) { error = "Could not clear command binding."; }
    dispatcher.TryEnqueue([weak, error] {
      if (auto window = weak.get()) {
        window->BindingErrorText().Text(to_wide(error));
        window->BindingErrorText().Visibility(
            error.empty() ? mx::Visibility::Collapsed : mx::Visibility::Visible);
        window->LoadBindings();
      }
    });
  }).detach();
}

void SettingsWindow::SaveKey() {
  if (api_ == nullptr) {
    return;
  }
  auto const key = trimmed(to_utf8(std::wstring(KeyBox().Password())));
  if (key.empty()) {
    return;
  }
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto api = api_;
  std::thread([weak, api, dispatcher, key]() mutable {
    std::string status;
    try {
      status = api->run_action("ai.key", key).get();
    } catch (std::exception const& e) {
      status = e.what();
    } catch (...) {
      status = "ai.key failed";
    }
    dispatcher.TryEnqueue([weak, status]() mutable {
      if (auto window = weak.get()) {
        window->KeyNotice().Text(
            status == "ok"
                ? std::wstring(L"Key saved — stored on this PC only, never synced.")
                : to_wide(status));
        window->KeyBox().Password(L"");
        window->RefreshAiStatus();
      }
    });
  }).detach();
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

void SettingsWindow::ShowError(std::wstring const& message) {
  ErrorText().Text(message);
  ErrorText().Visibility(mx::Visibility::Visible);
}

void SettingsWindow::ClearError() {
  ErrorText().Text(L"");
  ErrorText().Visibility(mx::Visibility::Collapsed);
}

// ---- XAML handlers ----------------------------------------------------------

void SettingsWindow::Toggle_Toggled(
    winrt::Windows::Foundation::IInspectable const& sender,
    mx::RoutedEventArgs const&) {
  if (loading_) {
    return;
  }
  auto toggle = sender.try_as<mxc::ToggleSwitch>();
  if (toggle == nullptr) {
    return;
  }
  if (tag_key(sender) == "local-login-startup") {
    if (!fulcrum::windows::SetAutostart(toggle.IsOn())) {
      ShowError(L"Could not change this PC's login startup setting.");
      loading_ = true;
      LoginStartupSwitch().IsOn(fulcrum::windows::AutostartEnabled());
      loading_ = false;
    } else {
      ClearError();
    }
    return;  // never send machine-local startup state over settings-set
  }
  Write(tag_key(sender), toggle.IsOn() ? "on" : "off");
}

void SettingsWindow::Choice_Changed(
    winrt::Windows::Foundation::IInspectable const& sender,
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
    winrt::Windows::Foundation::IInspectable const& sender,
    mx::RoutedEventArgs const&) {
  CommitText(sender.try_as<mxc::TextBox>());
}

void SettingsWindow::Text_KeyDown(
    winrt::Windows::Foundation::IInspectable const& sender,
    mx::Input::KeyRoutedEventArgs const& args) {
  if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
    CommitText(sender.try_as<mxc::TextBox>());
    args.Handled(true);
  }
}

void SettingsWindow::Key_KeyDown(
    winrt::Windows::Foundation::IInspectable const&,
    mx::Input::KeyRoutedEventArgs const& args) {
  if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
    SaveKey();
    args.Handled(true);
  }
}

void SettingsWindow::SaveKey_Click(
    winrt::Windows::Foundation::IInspectable const&,
    mx::RoutedEventArgs const&) {
  SaveKey();
}

void SettingsWindow::OpenUpdates() {
  NavigateTo("sync");
  RunUpdateCheck();
}

void SettingsWindow::UpdateCheck_Click(
    winrt::Windows::Foundation::IInspectable const&,
    mx::RoutedEventArgs const&) {
  RunUpdateCheck();
}

void SettingsWindow::SetUpdateStatus(std::wstring const& message) {
  UpdateStatusText().Text(message);
  UpdateStatusText().Visibility(mx::Visibility::Visible);
}

void SettingsWindow::RunUpdateCheck() {
  if (api_ == nullptr || update_polling_.exchange(true)) {
    return;
  }
  UpdateCheckButton().IsEnabled(false);
  UpdateDownloadButton().Visibility(mx::Visibility::Collapsed);
  UpdateInstallButton().Visibility(mx::Visibility::Collapsed);
  UpdateProgressBar().Visibility(mx::Visibility::Collapsed);
  SetUpdateStatus(L"Checking for updates…");
  auto const dispatcher = DispatcherQueue();
  auto weak = get_weak();
  auto api = api_;
  std::thread([weak, api, dispatcher]() mutable {
    std::wstring line;
    try {
      auto const check = api->update_check(true).get();
      if (check.status == "available") {
        auto const version = to_wide(check.available_version.value_or("?"));
        auto const size_mb = check.size_bytes
                                 ? std::to_wstring((*check.size_bytes + (1 << 20) - 1) >> 20) + L" MB"
                                 : L"";
        line = L"Fulcrum " + version + L" is available" +
               (size_mb.empty() ? L"" : L" (" + size_mb + L")") +
               L" — download it below.";
        dispatcher.TryEnqueue([weak, dispatcher]() mutable {
          if (auto window = weak.get()) {
            window->UpdateDownloadButton().Visibility(mx::Visibility::Visible);
          }
        });
      } else if (check.status == "up-to-date") {
        line = L"Fulcrum " + to_wide(check.current_version) + L" is up to date.";
      } else if (check.status == "throttled") {
        line = L"Update check throttled — try again shortly.";
      } else {
        line = L"Update check failed: " +
               to_wide(check.error.value_or("check failed"));
      }
    } catch (std::exception const& e) {
      line = to_wide(std::string("Update check failed: ") + e.what());
    } catch (...) {
      line = L"Update check failed.";
    }
    dispatcher.TryEnqueue([weak, dispatcher, line]() mutable {
      if (auto window = weak.get()) {
        window->SetUpdateStatus(line);
        window->UpdateCheckButton().IsEnabled(true);
        window->update_polling_.store(false, std::memory_order_release);
      }
    });
  }).detach();
}

void SettingsWindow::UpdateDownload_Click(
    winrt::Windows::Foundation::IInspectable const&,
    mx::RoutedEventArgs const&) {
  if (api_ == nullptr || update_polling_.exchange(true)) {
    return;
  }
  UpdateDownloadButton().IsEnabled(false);
  UpdateProgressBar().Visibility(mx::Visibility::Visible);
  SetUpdateStatus(L"Downloading…");
  auto const dispatcher = DispatcherQueue();
  update_timer_ = DispatcherQueue().CreateTimer();
  update_timer_.Interval(std::chrono::milliseconds{700});
  update_timer_.Tick({this, &SettingsWindow::UpdatePollTick});
  update_timer_.Start();
  auto weak = get_weak();
  auto api = api_;
  std::thread([weak, api]() mutable {
    try {
      api->update_download().get();
    } catch (...) {
    }
  }).detach();
}

void SettingsWindow::UpdatePollTick(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Windows::Foundation::IInspectable const&) {
  if (api_ == nullptr) {
    return;
  }
  auto const state = api_->update_state().get();
  if (state.phase == "downloading") {
    auto const percent = state.percent < 0 ? 0 : (state.percent > 100 ? 100 : state.percent);
    UpdateProgressBar().Value(static_cast<double>(percent));
    SetUpdateStatus(L"Downloading… " + std::to_wstring(percent) + L"%");
    return;
  }
  // Any other phase means the transfer left `downloading` (done, failed,
  // or superseded): stop polling and re-read the truth.
  update_timer_.Stop();
  update_polling_.store(false, std::memory_order_release);
  UpdateProgressBar().Visibility(mx::Visibility::Collapsed);
  if (state.phase == "downloaded") {
    SetUpdateStatus(L"Update downloaded — quit and install when ready.");
    UpdateInstallButton().Visibility(mx::Visibility::Visible);
    return;
  }
  if (state.phase == "error") {
    SetUpdateStatus(to_wide(state.message.value_or("Update download failed.")));
    UpdateDownloadButton().IsEnabled(true);
    return;
  }
  SetUpdateStatus(L"");
}

void SettingsWindow::UpdateInstall_Click(
    winrt::Windows::Foundation::IInspectable const&,
    mx::RoutedEventArgs const&) {
  if (api_ == nullptr) {
    return;
  }
  UpdateInstallButton().IsEnabled(false);
  auto api = api_;
  std::thread([api]() mutable {
    try {
      (void)api->run_action("update.install", "").get();
    } catch (...) {
    }
  }).detach();
}

}  // namespace winrt::RivetHost::implementation
}  // namespace winrt::RivetHost::implementation
