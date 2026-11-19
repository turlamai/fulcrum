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
      // 720x520 is the shared settings window size (docs/design-system.md);
      // the older 820x580 drifted from the macOS/GTK hosts.
      appWindow.Resize(winrt::Windows::Graphics::SizeInt32{720, 520});
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
  WorkflowsSection().Visibility(section == "workflows" ? mx::Visibility::Visible : mx::Visibility::Collapsed);
  SyncSection().Visibility(section == "sync" ? mx::Visibility::Visible : mx::Visibility::Collapsed);
  if (section == "commands") LoadBindings();
  if (section == "workflows") { LoadWorkflowList(); LoadWorkflowDetail(); }
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

void SettingsWindow::SetUpdateOpener(std::function<void()> opener) {
  update_opener_ = std::move(opener);
}

void SettingsWindow::RefreshLanguage(std::string const& language) {
  language_ = language.empty() ? "system" : language;
  values_["language"] = language_;
  loading_ = true;
  select_choice(LanguageBox(), language_);
  loading_ = false;
  ApplyLocalization(language_);
  // Dynamic rows were painted in the previous language; rebuild whichever
  // section is on screen so the whole window follows the push.
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
  language_ = get("language").empty() ? "system" : get("language");
  ApplyLocalization(language_);
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
  AiStatusText().Text(ai_status == "ready"
                          ? translated_setting(language_, "settings.ai-ready")
                          : to_wide(ai_status));
  loading_ = false;
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
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto api = api_;
  std::string const lang = language_;
  std::thread([weak, api, dispatcher, key, value, lang]() mutable {
    std::wstring error;
    try {
      if (!api->settings_set(key, value).get()) {
        error = key == "hotkey"
            ? translated_setting(lang, "settings.hotkey.unsupported")
            : frame0(translated_setting(lang, "settings.value.unsupported"),
                     to_wide(key));
      }
    } catch (...) {
      error = frame0(translated_setting(lang, "settings.write.failed"),
                     to_wide(key));
    }
    dispatcher.TryEnqueue([weak, key, error]() mutable {
      if (auto window = weak.get()) {
        if (error.empty()) {
          window->ClearError();
          if (key == "ai-provider") {
            window->RefreshAiStatus();
          }
          if (key == "language") {
            // Repaint the dynamic rows in the accepted language.
            window->RefreshLanguage(window->values_["language"]);
          }
        } else {
          // The backend refused the value (grammar, range): say so and
          // fall back to what it actually holds.
          window->ShowError(error);
          window->Load();
        }
      }
    });
  }).detach();
}

void SettingsWindow::ApplyLocalization(std::string const& language) {
  auto const tr = [&language](std::string_view key) {
    return translated_setting(language, key);
  };
  // General: labels, hints, placeholders and option items.
  LanguageLabel().Text(tr("setting.language"));
  ThemeLabel().Text(tr("setting.theme"));
  StartAtLoginLabel().Text(tr("settings.start-at-login"));
  StartAtLoginHint().Text(tr("settings.hint.start-at-login"));
  HotkeyLabel().Text(tr("setting.hotkey"));
  HotkeyBox().PlaceholderText(tr("settings.hotkey.placeholder"));
  HotkeyHint().Text(tr("settings.hint.hotkey"));
  MaxResultsLabel().Text(tr("setting.max-results"));
  MaxResultsHint().Text(tr("settings.hint.max-results"));
  LangSystemItem().Content(winrt::box_value(tr("settings.language.system")));
  LangEnItem().Content(winrt::box_value(tr("settings.language.en")));
  LangZhItem().Content(winrt::box_value(tr("settings.language.zh")));
  ThemeSystemItem().Content(winrt::box_value(tr("settings.theme.system")));
  ThemeLightItem().Content(winrt::box_value(tr("settings.theme.light")));
  ThemeDarkItem().Content(winrt::box_value(tr("settings.theme.dark")));
  // Features.
  ClipboardLabel().Text(tr("setting.clipboard-enabled"));
  ClipboardLimitLabel().Text(tr("settings.clipboard-limit"));
  ClipboardLimitHint().Text(tr("settings.hint.clipboard-limit"));
  WebSearchLabel().Text(tr("setting.web-search-enabled"));
  DefaultBangLabel().Text(tr("settings.default-engine"));
  ExpansionLabel().Text(tr("setting.expansion-enabled"));
  PluginsLabel().Text(tr("setting.plugins-enabled"));
  PluginTimeoutLabel().Text(tr("settings.plugins-timeout"));
  PluginTimeoutHint().Text(tr("settings.hint.plugins-timeout"));
  // AI.
  ProviderLabel().Text(tr("setting.ai-provider"));
  ProviderOffItem().Content(winrt::box_value(tr("settings.provider.off")));
  ModelLabel().Text(tr("settings.ai-model"));
  ModelBox().PlaceholderText(tr("settings.ai-model.placeholder"));
  BaseUrlLabel().Text(tr("settings.ai-base-url"));
  BaseUrlBox().PlaceholderText(tr("settings.ai-model.placeholder"));
  PersonaLabel().Text(tr("settings.ai-persona"));
  PersonaBox().PlaceholderText(tr("settings.ai-persona.placeholder"));
  ApiKeyLabel().Text(tr("settings.ai-api-key"));
  KeyBox().PlaceholderText(tr("settings.ai-key.placeholder"));
  SaveKeyButton().Content(winrt::box_value(tr("settings.save")));
  AiKeysHint().Text(tr("settings.hint.ai-keys"));
  // Navigation and section titles: the shared six-section IA.
  NavGeneral().Content(winrt::box_value(tr("settings.nav.general")));
  NavFeatures().Content(winrt::box_value(tr("settings.nav.features")));
  NavAi().Content(winrt::box_value(tr("settings.nav.ai")));
  NavCommands().Content(winrt::box_value(tr("settings.nav.commands")));
  NavWorkflows().Content(winrt::box_value(tr("settings.nav.workflows")));
  NavSync().Content(winrt::box_value(tr("settings.nav.sync")));
  GeneralTitle().Text(tr("settings.nav.general"));
  FeaturesTitle().Text(tr("settings.nav.features"));
  AiTitle().Text(tr("settings.nav.ai"));
  CommandsTitle().Text(tr("settings.nav.commands"));
  WorkflowsTitle().Text(tr("settings.wf.title"));
  SyncTitle().Text(tr("settings.nav.sync"));
  // Commands.
  CommandsHint().Text(tr("settings.hint.commands"));
  CommandsLoadingText().Text(tr("settings.commands.loading"));
  // Workflows editor chrome (dynamic rows localize at paint time).
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
  // Sync & Updates: note + one button opening the dedicated update window.
  SyncFolderLabel().Text(tr("settings.sync-folder"));
  SyncRootBox().PlaceholderText(tr("settings.sync-folder.placeholder"));
  SyncFolderHint().Text(tr("settings.sync-folder.description"));
  UpdatesNoteHint().Text(tr("settings.updates-note.win"));
  CheckUpdatesNowButton().Content(
      winrt::box_value(tr("settings.check-updates-now")));
}

void SettingsWindow::RefreshAiStatus() {
  if (api_ == nullptr) {
    return;
  }
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto api = api_;
  std::string const lang = language_;
  std::thread([weak, api, dispatcher, lang]() mutable {
    std::string status;
    try {
      status = api->ai_status().get();
    } catch (...) {
    }
    dispatcher.TryEnqueue([weak, status, lang]() mutable {
      if (auto window = weak.get()) {
        window->AiStatusText().Text(
            status == "ready" ? translated_setting(lang, "settings.ai-ready")
                              : to_wide(status));
      }
    });
  }).detach();
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
  std::string const lang = language_;
  std::thread([weak, api, dispatcher, key, lang]() mutable {
    std::string status;
    try {
      status = api->run_action("ai.key", key).get();
    } catch (...) {
      status = "error";
    }
    dispatcher.TryEnqueue([weak, status, lang]() mutable {
      if (auto window = weak.get()) {
        window->KeyNotice().Text(
            status == "ok" ? translated_setting(lang, "settings.ai-key.saved")
                           : translated_setting(lang, "settings.ai-key.failed"));
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
      ShowError(translated_setting(language_, "settings.login.failed.win"));
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

void SettingsWindow::CheckUpdatesNow_Click(
    winrt::Windows::Foundation::IInspectable const&,
    mx::RoutedEventArgs const&) {
  // Update-flow parity: the Sync page button opens the same dedicated
  // update window as the tray entry and the launcher update rows. The
  // install handoff lives in MainWindow, not in this form.
  if (update_opener_) update_opener_();
}

}  // namespace winrt::RivetHost::implementation