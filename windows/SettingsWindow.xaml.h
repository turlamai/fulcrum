#pragma once

#include "pch.h"
#include "SettingsWindow.g.h"
#include "GeneratedBackend.hpp"

#include "winui_window.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace winrt::RivetHost::implementation {

// One settings-window row as `settings-ui` describes it: the key, its
// section, the control a host draws and the options (a select's values, a
// number's inclusive bounds). Labels never travel — hosts resolve
// settings.<key> / settings.hint.<key> / settings.opt.<key>.<option>.
struct SettingRow {
  std::string key;
  std::string section;
  std::string control;
  std::vector<std::string> options;
  std::string value;
};

// One field of a host form dialog (add/edit a list entry, the AI key).
struct DialogField {
  std::wstring label;
  std::wstring value;
  std::wstring placeholder;
  std::wstring help;
  bool multiline{false};
  bool browse{false};  // a folder picker beside the box
};

// A form dialog and its boxes, in field order.
struct FormDialog {
  Microsoft::UI::Xaml::Controls::ContentDialog dialog{nullptr};
  Microsoft::UI::Xaml::Controls::TextBlock error{nullptr};
  std::vector<Microsoft::UI::Xaml::Controls::TextBox> boxes;
};

// A sidebar page: its id, its group and its sections in display order.
struct SettingsPage {
  std::string id;
  std::string group;
  std::vector<std::pair<std::string, std::vector<SettingRow>>> sections;

  std::vector<SettingRow> const* FindSection(std::string const& name) const {
    for (auto const& section : sections) {
      if (section.first == name) return &section.second;
    }
    return nullptr;
  }
};

// The settings window: a data-driven form over the backend settings store.
// The sidebar, pages, sections and rows come from `settings-ui`; the
// commands and workflow editors and the list pages stay native. Edits go
// through settings-set as they happen, the backend validates (hotkey
// grammar, ranges, provider names), and a refusal surfaces inline under
// the row while the form reloads the backend's truth. The AI key rides the
// ai.key action and never comes back over the wire — ai-status reports
// readiness instead.
struct SettingsWindow : SettingsWindowT<SettingsWindow> {
  SettingsWindow();

  // Hooked up by MainWindow once the backend is ready (and again on every
  // show, which reloads). The API outlives this window: MainWindow owns it.
  void Attach(std::shared_ptr<rivet_app::API> api);
  // The Sync & Updates button opens the dedicated update window — the same
  // surface the tray entry and the launcher update rows open (update-flow
  // parity, design/DESIGN.md). MainWindow owns that window and the install
  // handoff; this window only asks for it to be shown.
  void SetUpdateOpener(std::function<void()> opener);
  // A backend language push repaints an already-open window: static chrome
  // plus the dynamic command/workflow rows.
  void RefreshLanguage(std::string const& language);

  // The grouped sidebar is built from the backend's page rows; selecting
  // one shows its page. The same backend keys are used by macOS, Windows
  // and Linux; only the host controls differ.
  void Navigation_SelectionChanged(
      Microsoft::UI::Xaml::Controls::NavigationView const&,
      Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
  void Hotkey_KeyDown(
      winrt::Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
  void Search_TextChanged(
      winrt::Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
  void Search_KeyDown(
      winrt::Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
  // Ctrl+F on the root grid focuses the settings search.
  void Root_KeyDown(winrt::Windows::Foundation::IInspectable const&,
                    Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
  void Highlight_Tick(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Windows::Foundation::IInspectable const&);
  // Sizes the window once, on first activation, in effective pixels.
  void First_Activated(winrt::Windows::Foundation::IInspectable const&,
                       Microsoft::UI::Xaml::WindowActivatedEventArgs const&);

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
  void WorkflowCreate_Click(winrt::Windows::Foundation::IInspectable const&,
                            Microsoft::UI::Xaml::RoutedEventArgs const&);
  void WorkflowRefresh_Click(winrt::Windows::Foundation::IInspectable const&,
                             Microsoft::UI::Xaml::RoutedEventArgs const&);
  void WorkflowPrevious_Click(winrt::Windows::Foundation::IInspectable const&,
                              Microsoft::UI::Xaml::RoutedEventArgs const&);
  void WorkflowNext_Click(winrt::Windows::Foundation::IInspectable const&,
                          Microsoft::UI::Xaml::RoutedEventArgs const&);
  void WorkflowStepPrevious_Click(winrt::Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::RoutedEventArgs const&);
  void WorkflowStepNext_Click(winrt::Windows::Foundation::IInspectable const&,
                              Microsoft::UI::Xaml::RoutedEventArgs const&);
  void WorkflowAdd_Click(winrt::Windows::Foundation::IInspectable const&,
                         Microsoft::UI::Xaml::RoutedEventArgs const&);
  void WorkflowDry_Click(winrt::Windows::Foundation::IInspectable const&,
                         Microsoft::UI::Xaml::RoutedEventArgs const&);
  void WorkflowRun_Click(winrt::Windows::Foundation::IInspectable const&,
                         Microsoft::UI::Xaml::RoutedEventArgs const&);
  void WorkflowDelete_Click(winrt::Windows::Foundation::IInspectable const&,
                            Microsoft::UI::Xaml::RoutedEventArgs const&);
  void WorkflowTracePrevious_Click(winrt::Windows::Foundation::IInspectable const&,
                                   Microsoft::UI::Xaml::RoutedEventArgs const&);
  void WorkflowTraceNext_Click(winrt::Windows::Foundation::IInspectable const&,
                               Microsoft::UI::Xaml::RoutedEventArgs const&);
  void CheckUpdatesNow_Click(winrt::Windows::Foundation::IInspectable const&,
                             Microsoft::UI::Xaml::RoutedEventArgs const&);

 private:
  void Load();
  void Apply(std::map<std::string, std::string> values, std::string ai_status);
  void Write(std::string key, std::string value);
  void RefreshAiStatus();
  void RefreshAiStatusText();
  void LoadBindings();
  void ShowBindings(std::vector<std::vector<std::string>> commands,
                    std::vector<std::vector<std::string>> bindings);
  void SaveBinding(std::string id, std::string alias, std::string hotkey);
  void ClearBinding(std::string id);
  void LoadWorkflowList();
  void LoadWorkflowDetail();
  void WorkflowCommand(std::string id, std::string arg);
  void NavigateTo(std::string const& page);
  void RenderPageIfVisible();
  Microsoft::UI::Xaml::UIElement MakeSearchResult(std::string const& page, std::string const& key,
                                                  std::wstring const& label,
                                                  std::wstring const& help);
  // Shared chrome of the native extras: a section title with an optional Add
  // button, its help line, and the card whose rows the caller fills.
  Microsoft::UI::Xaml::Controls::StackPanel BeginCard(
      std::string const& title_key, std::string const& help_key,
      Microsoft::UI::Xaml::Controls::StackPanel const& host, std::string const& add_key,
      std::function<void()> on_add);
  Microsoft::UI::Xaml::Media::SolidColorBrush ErrorBrush();
  // A backend refusal ("<code>: <detail>") mapped to the host's sentence.
  std::wstring RefusalText(std::string const& raw, std::string const& fallback_key,
                           std::vector<std::pair<std::string, std::string>> const& codes) const;
  void Notice(std::wstring const& message);
  // Dialogs (run on the UI thread). `then` runs only after the user
  // accepted and, for a form, after its inline validation passed; `error`
  // is the refusal that reopened a form, shown under its fields.
  void ConfirmThen(std::wstring title, std::wstring body,
                                     std::string primary_key, std::function<void()> then);
  void ShowForm(std::wstring title, std::vector<DialogField> fields,
                                  std::string primary_key, std::wstring error,
                                  std::function<bool(FormDialog&)> validate,
                                  std::function<void(std::vector<std::string>)> then);
  winrt::fire_and_forget PickFolderThen(std::function<void(std::string)> then);
  winrt::fire_and_forget PickSaveFileThen(std::function<void(std::string)> then);
  winrt::fire_and_forget PickOpenFileThen(std::function<void(std::string)> then);
  void EditNamedListDialog(std::string const& kind, std::string const& id, std::string const& name,
                           std::string const& path, std::wstring error);
  void EditCollectionDialog(bool snippets, std::string const& id, std::string const& name,
                            std::string const& keyword, std::string const& body, std::wstring error);
  void ExportDialog();
  void ExportConfiguration(std::string categories, std::string path);
  winrt::fire_and_forget ShowDialogThen(Microsoft::UI::Xaml::Controls::ContentDialog const& dialog,
                       std::function<void(Microsoft::UI::Xaml::Controls::ContentDialogResult)> then);
  Microsoft::UI::Xaml::Style CardStyle();
  void RunImport(std::string payload);
  // The active language's text for one shared i18n key.
  std::wstring T(std::string const& key) const;
  void ApplyLocalization(std::string const& language);
  void SaveKey();
  void CommitText(Microsoft::UI::Xaml::Controls::TextBox const& box);
  void ShowError(std::wstring const& message);
  void ClearError();
  void ShowRowError(std::string const& key, std::wstring const& message);
  void ClearRowError(std::string const& key);

  // ---- schema-driven pages ---------------------------------------------------
  // Parses `settings-ui` rows into pages_ and builds the sidebar once.
  void SetSchema(std::vector<std::vector<std::string>> rows);
  SettingsPage const* FindPage(std::string const& id) const;
  // Paints the current page: title, description, one card per section with
  // its rows (plus the host-owned rows), then the page's native extras.
  void RenderPage();
  // One row of a setting: label + hint on the left, the control on the
  // right. The hint doubles as the row's inline error surface.
  Microsoft::UI::Xaml::UIElement MakeRow(SettingRow const& row);
  Microsoft::UI::Xaml::UIElement MakeStartupRow();
  Microsoft::UI::Xaml::UIElement MakeSectionTitle(std::string const& text);
  // The native parts of a page the schema does not describe.
  void BuildExtras(std::string const& page, Microsoft::UI::Xaml::Controls::StackPanel const& host);
  void BuildNamedList(std::string const& kind, Microsoft::UI::Xaml::Controls::StackPanel const& host);
  void BuildCollections(bool snippets, Microsoft::UI::Xaml::Controls::StackPanel const& host);
  void BuildPlugins(Microsoft::UI::Xaml::Controls::StackPanel const& host);
  void BuildAiKey(Microsoft::UI::Xaml::Controls::StackPanel const& host);
  void BuildClearButton(Microsoft::UI::Xaml::Controls::StackPanel const& host,
                        std::string const& title_key, std::string const& confirm_key, bool clipboard);
  void BuildTransfer(Microsoft::UI::Xaml::Controls::StackPanel const& host);
  void LoadNamedList(std::string const& kind, Microsoft::UI::Xaml::Controls::StackPanel const& rows);
  void LoadCollections(bool snippets, Microsoft::UI::Xaml::Controls::StackPanel const& rows);
  void LoadPlugins(Microsoft::UI::Xaml::Controls::StackPanel const& rows);
  void ShowNamedList(std::string const& kind, Microsoft::UI::Xaml::Controls::StackPanel const& rows,
                     std::vector<std::vector<std::string>> items);
  void ShowCollections(bool snippets, Microsoft::UI::Xaml::Controls::StackPanel const& rows,
                       std::vector<std::vector<std::string>> items);
  void ShowPlugins(Microsoft::UI::Xaml::Controls::StackPanel const& rows,
                   std::vector<std::vector<std::string>> plugins);
  void EditNamedList(std::string const& kind, std::string const& id, std::string const& name,
                     std::string const& path);
  void RemoveNamedList(std::string const& kind, std::string const& id, std::string const& name);
  void SetNamedListEnabled(std::string const& kind, std::string const& id, std::string const& name,
                           std::string const& path, bool enabled);
  void EditCollection(bool snippets, std::string const& id, std::string const& name,
                      std::string const& keyword, std::string const& body);
  void DeleteCollection(bool snippets, std::string const& id, std::string const& name);
  void SetPluginEnabled(std::string const& id, bool enabled);
  void SaveNamedList(std::string const& kind, std::string const& id, std::string const& name,
                      std::string const& path, std::string const& enabled);
  void DeleteNamedList(std::string const& kind, std::string const& id);
  void SaveCollection(bool snippets, std::string const& id, std::string const& name,
                       std::string const& keyword, std::string const& body);
  void RemoveCollection(bool snippets, std::string const& id);
  void ClearHistory(bool clipboard);
  void ImportConfiguration();
  // Settings search: the hits the current query matched, in result order.
  void RenderSearch();
  void JumpToRow(std::string const& page, std::string const& key);

  std::shared_ptr<rivet_app::API> api_;
  std::function<void()> update_opener_;
  // True while Apply pushes values into the controls, so their change
  // handlers do not write them straight back.
  bool loading_{false};
  std::map<std::string, std::string> values_;
  // Active UI language ("system"/"zh"/"en"); dynamic rows read it at paint
  // time and RefreshLanguage repaints them on a backend push.
  std::string language_{"system"};
  // The schema as last fetched: pages in sidebar order, their sections.
  std::vector<SettingsPage> pages_;
  std::string current_page_{"general"};
  // The hint under each painted setting row, so a refusal lands on its row.
  std::map<std::string, Microsoft::UI::Xaml::Controls::TextBlock> row_hints_;
  std::map<std::string, Microsoft::UI::Xaml::UIElement> row_controls_;
  // The row a search hit jumps to: (page, key) by hit index.
  std::vector<std::pair<std::string, std::string>> search_hits_;
  // Page id -> sidebar item, so a search hit can select the page.
  std::map<std::string, Microsoft::UI::Xaml::Controls::NavigationViewItem> nav_items_;
  // The painted row itself (search highlight) and its hint (inline errors).
  std::map<std::string, Microsoft::UI::Xaml::FrameworkElement> row_roots_;
  std::map<std::string, std::wstring> row_errors_;
  Microsoft::UI::Xaml::Controls::ToggleSwitch startup_toggle_{nullptr};
  Microsoft::UI::Xaml::Controls::PasswordBox key_box_{nullptr};
  Microsoft::UI::Xaml::Controls::TextBlock ai_status_text_{nullptr};
  Microsoft::UI::Xaml::Controls::TextBlock key_notice_{nullptr};
  std::string ai_status_;
  // Extras: the rows panel each list currently paints into (answers that
  // arrive after the page changed are dropped) and the last data shown, so
  // revisiting a page is immediate before the refresh lands.
  std::map<std::string, Microsoft::UI::Xaml::Controls::StackPanel> list_hosts_;
  std::map<std::string, std::vector<std::vector<std::string>>> list_cache_;
  Microsoft::UI::Xaml::DispatcherTimer highlight_timer_;
  Microsoft::UI::Xaml::Controls::Grid highlight_row_{nullptr};
  bool jumping_{false};
  bool sized_{false};
  Microsoft::UI::Xaml::Style card_style_{nullptr};
  std::string workflow_selected_;
  int workflow_list_page_{1};
  int workflow_step_page_{1};
  int workflow_trace_page_{1};
  bool workflow_copy_safe_{false};
};

}  // namespace winrt::RivetHost::implementation

namespace winrt::RivetHost::factory_implementation {

struct SettingsWindow : SettingsWindowT<SettingsWindow, implementation::SettingsWindow> {};

}  // namespace winrt::RivetHost::factory_implementation
