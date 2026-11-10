#pragma once

#include "pch.h"
#include "SettingsWindow.g.h"
#include "GeneratedBackend.hpp"

#include "winui_window.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace winrt::RivetHost::implementation {

// The settings window: a form over the backend settings store, the
// discoverable twin of the `settings` query rows. Every control carries its
// backend key in Tag; edits go through settings-set as they happen, the
// backend validates (hotkey grammar, ranges, provider names), and a refusal
// surfaces inline while the form reloads the backend's truth. The AI key
// rides the ai.key action and never comes back over the wire — ai-status
// reports readiness instead.
struct SettingsWindow : SettingsWindowT<SettingsWindow> {
  SettingsWindow();

  // Hooked up by MainWindow once the backend is ready (and again on every
  // show, which reloads). The API outlives this window: MainWindow owns it.
  void Attach(std::shared_ptr<rivet_app::API> api);
  // Opens the Sync & Updates tab and runs an update check — the tray
  // "Check for updates" entry lands here so the result shows in a
  // visible window instead of a hidden launcher's status line.
  void OpenUpdates();

  // Native five-section settings IA. The same backend keys are used by
  // macOS, Windows and Linux; only the host controls differ.
  void Navigation_SelectionChanged(
      Microsoft::UI::Xaml::Controls::NavigationView const&,
      Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
  void Hotkey_KeyDown(
      winrt::Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);

  // XAML handlers (SettingsWindow.xaml).
  void Toggle_Toggled(winrt::Windows::Foundation::IInspectable const&,
                      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void Choice_Changed(winrt::Windows::Foundation::IInspectable const&,
                      Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
  void Number_Changed(Microsoft::UI::Xaml::Controls::NumberBox const&,
                      Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
  void Text_LostFocus(winrt::Windows::Foundation::IInspectable const&,
                      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void Text_KeyDown(winrt::Windows::Foundation::IInspectable const&,
                    Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
  void Key_KeyDown(winrt::Windows::Foundation::IInspectable const&,
                   Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
  void SaveKey_Click(winrt::Windows::Foundation::IInspectable const&,
                     Microsoft::UI::Xaml::RoutedEventArgs const&);
  void UpdateCheck_Click(winrt::Windows::Foundation::IInspectable const&,
                         Microsoft::UI::Xaml::RoutedEventArgs const&);
  void UpdateDownload_Click(winrt::Windows::Foundation::IInspectable const&,
                            Microsoft::UI::Xaml::RoutedEventArgs const&);
  void UpdateInstall_Click(winrt::Windows::Foundation::IInspectable const&,
                           Microsoft::UI::Xaml::RoutedEventArgs const&);

 private:
  void Load();
  void Apply(std::map<std::string, std::string> values, std::string ai_status);
  void Write(std::string key, std::string value);
  void RefreshAiStatus();
  void LoadBindings();
  void ShowBindings(std::vector<std::vector<std::string>> commands,
                    std::vector<std::vector<std::string>> bindings);
  void SaveBinding(std::string id, std::string alias, std::string hotkey);
  void ClearBinding(std::string id);
  void NavigateTo(std::string const& section);
  void ApplyLocalization(std::string const& language);
  void SaveKey();
  void CommitText(Microsoft::UI::Xaml::Controls::TextBox const& box);
  void ShowError(std::wstring const& message);
  void ClearError();
  void SetUpdateStatus(std::wstring const& message);
  void UpdatePollTick(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Windows::Foundation::IInspectable const&);

  std::shared_ptr<rivet_app::API> api_;
  // True while Apply pushes values into the controls, so their change
  // handlers do not write them straight back.
  bool loading_{false};
  std::map<std::string, std::string> values_;
  // Update card state: a download poller while a download runs; the flag
  // guards the poll against overlapping re-entries.
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer update_timer_{nullptr};
  std::atomic<bool> update_polling_{false};

  void RunUpdateCheck();
};

}  // namespace winrt::RivetHost::implementation

namespace winrt::RivetHost::factory_implementation {

struct SettingsWindow : SettingsWindowT<SettingsWindow, implementation::SettingsWindow> {};

}  // namespace winrt::RivetHost::factory_implementation
