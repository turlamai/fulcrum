#include "pch.h"
#include "UpdateWindow.h"
#include "GeneratedI18n.hpp"

#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Text.h>

#include <thread>
#include <utility>

namespace winrt::RivetHost::implementation {
namespace {

namespace mx = winrt::Microsoft::UI::Xaml;
namespace mxc = winrt::Microsoft::UI::Xaml::Controls;
namespace mxa = winrt::Microsoft::UI::Xaml::Automation;

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

// "system" follows the OS UI language, same resolution as the launcher.
std::string effective_language(std::string const& language) {
  if (language != "system") return language;
  return ((::GetUserDefaultUILanguage() & 0x3FF) == 0x0004) ? "zh" : "en";
}

std::wstring translated(std::string const& language, std::string_view key) {
  std::string const effective = effective_language(language);
  for (auto const& row : rivet_app::kI18nTable) {
    if (row.key == key) {
      return to_wide(std::string(effective == "en" ? row.en : row.zh));
    }
  }
  return to_wide(std::string(key));
}

// Positional {0}/{1} substitution for the shared i18n frames.
std::wstring frame(std::wstring text, std::wstring const& v0,
                   std::wstring const& v1 = {}) {
  auto const at0 = text.find(L"{0}");
  if (at0 != std::wstring::npos) text.replace(at0, 3, v0);
  auto const at1 = text.find(L"{1}");
  if (at1 != std::wstring::npos) text.replace(at1, 3, v1);
  return text;
}

// Same human size ladder as the macOS panel (MB with one decimal, KB, B).
std::wstring size_label(std::int64_t bytes) {
  if (bytes >= 1024 * 1024) {
    wchar_t buffer[64]{};
    ::swprintf_s(buffer, L"%.1f MB",
                 static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buffer;
  }
  if (bytes >= 1024) return std::to_wstring(bytes / 1024) + L" KB";
  return std::to_wstring(bytes) + L" B";
}

void set_automation_id(mx::FrameworkElement const& element, wchar_t const* id) {
  mxa::AutomationProperties::SetAutomationId(element, id);
}

}  // namespace

UpdateWindow::UpdateWindow(std::shared_ptr<rivet_app::API> api,
                           InstallHandoff handoff)
    : api_(std::move(api)), handoff_(std::move(handoff)) {}

void UpdateWindow::EnsureWindow() {
  if (window_ != nullptr) return;

  title_ = mxc::TextBlock{};
  title_.FontSize(15);
  title_.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  title_.TextAlignment(mx::TextAlignment::Center);
  title_.TextWrapping(mx::TextWrapping::Wrap);

  status_ = mxc::TextBlock{};
  status_.TextAlignment(mx::TextAlignment::Center);
  status_.TextWrapping(mx::TextWrapping::Wrap);
  status_.IsTextSelectionEnabled(true);
  status_.Opacity(0.85);
  set_automation_id(status_, L"fulcrum-update-status");

  ring_ = mxc::ProgressRing{};
  ring_.IsActive(false);
  ring_.Width(32);
  ring_.Height(32);
  ring_.HorizontalAlignment(mx::HorizontalAlignment::Center);

  bar_ = mxc::ProgressBar{};
  bar_.Minimum(0);
  bar_.Maximum(100);
  set_automation_id(bar_, L"fulcrum-update-progress");

  auto const make_button = [this](wchar_t const* id) {
    mxc::Button button;
    set_automation_id(button, id);
    return button;
  };
  check_ = make_button(L"fulcrum-update-check");
  close_ = make_button(L"fulcrum-update-close");
  download_ = make_button(L"fulcrum-update-download");
  install_ = make_button(L"fulcrum-update-install");
  retry_ = make_button(L"fulcrum-update-retry");

  auto weak = weak_from_this();
  check_.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) self->StartCheck();
  });
  close_.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Dismiss();
  });
  download_.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) self->StartDownload();
  });
  install_.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) self->StartInstall();
  });
  retry_.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) self->StartCheck();
  });

  auto buttons = mxc::StackPanel{};
  buttons.Orientation(mxc::Orientation::Horizontal);
  buttons.Spacing(10);
  buttons.HorizontalAlignment(mx::HorizontalAlignment::Center);
  buttons.Children().Append(check_);
  buttons.Children().Append(download_);
  buttons.Children().Append(install_);
  buttons.Children().Append(retry_);
  buttons.Children().Append(close_);

  auto panel = mxc::StackPanel{};
  panel.Spacing(14);
  panel.HorizontalAlignment(mx::HorizontalAlignment::Center);
  panel.VerticalAlignment(mx::VerticalAlignment::Center);
  panel.Children().Append(title_);
  panel.Children().Append(ring_);
  panel.Children().Append(status_);
  panel.Children().Append(bar_);
  panel.Children().Append(buttons);

  auto content = mxc::Border{};
  content.Padding({24, 16, 24, 16});
  content.Child(panel);

  window_ = mx::Window{};
  // Same titled small window as macOS (380x220, title "Fulcrum"): the
  // launcher is transient, the update flow must survive focus changes.
  window_.Title(L"Fulcrum");
  window_.Content(content);
  try {
    if (auto appWindow = window_.AppWindow()) {
      appWindow.Resize(winrt::Windows::Graphics::SizeInt32{380, 220});
    }
    (void)rivet::system::ApplyAppIcon(window_);
  } catch (...) {
    // Sizing/icon are optional; never lose the update surface.
  }
  // Close hides, it does not destroy: a verified download keeps running
  // and every entry point can bring the same window back (macOS semantics).
  try {
    window_.AppWindow().Closing(
        [weak](winrt::Microsoft::UI::Windowing::AppWindow const&,
               winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs const&
                   args) {
          args.Cancel(true);
          if (auto self = weak.lock()) self->Dismiss();
        });
  } catch (...) {
  }

  poll_ = window_.DispatcherQueue().CreateTimer();
  poll_.Interval(std::chrono::milliseconds{700});
  poll_.Tick([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) self->OnPollTick();
  });

  Paint();
}

void UpdateWindow::Show() {
  EnsureWindow();
  try {
    window_.AppWindow().Show();
  } catch (...) {
  }
  window_.Activate();
  if (HWND hwnd = rivet::system::NativeWindowHandle(window_)) {
    ::SetForegroundWindow(hwnd);
  }
}

void UpdateWindow::ShowAndCheck() {
  Show();
  if (phase_ == Phase::downloading || phase_ == Phase::checking) return;
  StartCheck();
}

void UpdateWindow::PresentAvailable(std::wstring version,
                                    std::int64_t size_bytes) {
  if (phase_ == Phase::downloading || phase_ == Phase::checking) return;
  available_version_ = std::move(version);
  available_size_ = size_bytes;
  error_.clear();
  phase_ = Phase::available;
  Show();
  Paint();
}

void UpdateWindow::PresentCompletedCheckForAcceptance() {
  check_generation_.fetch_add(1);
  error_.clear();
  phase_ = Phase::up_to_date;
  Show();
  Paint();
}

void UpdateWindow::Dismiss() {
  // A dismissed check must not repaint or reopen the window; a download
  // is a verified transaction and keeps running (macOS dismissPanel).
  check_generation_.fetch_add(1);
  if (phase_ != Phase::downloading) {
    phase_ = Phase::idle;
  }
  try {
    window_.AppWindow().Hide();
  } catch (...) {
  }
}

void UpdateWindow::RefreshLanguage(std::string const& language) {
  language_ = language.empty() ? "system" : language;
  if (window_ != nullptr) Paint();
}

void UpdateWindow::Paint() {
  if (window_ == nullptr) return;
  auto const tr = [this](std::string_view key) {
    return translated(language_, key);
  };
  auto const show = [](mx::UIElement const& element, bool visible) {
    element.Visibility(visible ? mx::Visibility::Visible
                               : mx::Visibility::Collapsed);
  };
  check_.Content(winrt::box_value(tr("updateCheckNow")));
  close_.Content(winrt::box_value(tr("updateClose")));
  download_.Content(winrt::box_value(tr("updateDownload") + L" → " +
                                     tr("updateQuitAndInstall")));
  install_.Content(winrt::box_value(tr("updateQuitAndInstall")));
  retry_.Content(winrt::box_value(tr("updateRetry")));

  show(title_, phase_ == Phase::available);
  show(ring_, phase_ == Phase::checking);
  ring_.IsActive(phase_ == Phase::checking);
  show(bar_, phase_ == Phase::downloading);
  show(check_, phase_ == Phase::idle);
  show(download_, phase_ == Phase::available);
  show(install_, phase_ == Phase::downloaded);
  show(retry_, phase_ == Phase::failed);
  show(close_, phase_ != Phase::idle);
  show(status_, phase_ != Phase::idle);

  switch (phase_) {
    case Phase::idle:
      status_.Text(L"");
      break;
    case Phase::checking:
      status_.Text(tr("updateChecking"));
      break;
    case Phase::up_to_date:
      status_.Text(tr("updateUpToDate"));
      break;
    case Phase::available:
      title_.Text(tr("updateAvailableTitle"));
      status_.Text(frame(tr("updateAvailableBody"), available_version_,
                         size_label(available_size_)));
      break;
    case Phase::downloading:
      bar_.Value(static_cast<double>(percent_));
      status_.Text(frame(tr("updateDownloadPercent"),
                         std::to_wstring(percent_)));
      break;
    case Phase::downloaded:
      status_.Text(tr("update.downloaded"));
      break;
    case Phase::failed:
      status_.Text(error_);
      break;
  }
}

void UpdateWindow::StartCheck() {
  if (api_ == nullptr) return;
  auto const generation = check_generation_.fetch_add(1) + 1;
  phase_ = Phase::checking;
  error_.clear();
  Paint();
  auto weak = weak_from_this();
  auto api = api_;
  auto dispatcher = window_.DispatcherQueue();
  std::string const lang = language_;
  std::thread([weak, api, dispatcher, generation, lang]() mutable {
    Phase phase = Phase::failed;
    std::wstring version;
    std::int64_t size = 0;
    std::wstring error;
    try {
      auto const check = api->update_check(true).get();
      if (check.status == "available") {
        phase = Phase::available;
        version = to_wide(check.available_version.value_or("?"));
        size = check.size_bytes.value_or(0);
      } else if (check.status == "up-to-date") {
        phase = Phase::up_to_date;
      } else if (check.status == "throttled") {
        error = translated(lang, "update.check-throttled");
      } else {
        error = frame(translated(lang, "update.check-failed"),
                      to_wide(check.error.value_or("check failed")));
      }
    } catch (std::exception const& e) {
      error = frame(translated(lang, "update.check-failed"),
                    to_wide(e.what()));
    } catch (...) {
      error = frame(translated(lang, "update.check-failed"),
                    translated(lang, "updateUnknownError"));
    }
    dispatcher.TryEnqueue([weak, generation, phase, version, size, error] {
      auto self = weak.lock();
      if (!self || self->check_generation_.load() != generation) return;
      self->phase_ = phase;
      self->available_version_ = version;
      self->available_size_ = size;
      self->error_ = error;
      self->Paint();
    });
  }).detach();
}

void UpdateWindow::StartDownload() {
  if (api_ == nullptr || phase_ != Phase::available) return;
  phase_ = Phase::downloading;
  percent_ = 0;
  error_.clear();
  Paint();
  poll_.Start();
  auto weak = weak_from_this();
  auto api = api_;
  // The backend download thread owns the transfer; failures surface
  // through update-state, never through this call's return.
  std::thread([weak, api]() mutable {
    try {
      api->update_download().get();
    } catch (...) {
    }
  }).detach();
}

void UpdateWindow::OnPollTick() {
  if (api_ == nullptr || polling_.exchange(true)) return;
  auto weak = weak_from_this();
  auto api = api_;
  auto dispatcher = window_.DispatcherQueue();
  std::string const lang = language_;
  std::thread([weak, api, dispatcher, lang]() mutable {
    std::string phase;
    int percent = 0;
    std::wstring message;
    bool read_ok = false;
    try {
      auto const state = api->update_state().get();
      phase = state.phase;
      percent = static_cast<int>(state.percent);
      message = to_wide(state.message.value_or(""));
      read_ok = true;
    } catch (...) {
    }
    dispatcher.TryEnqueue([weak, phase, percent, message, read_ok, lang] {
      auto self = weak.lock();
      if (!self) return;
      self->polling_.store(false, std::memory_order_release);
      if (self->phase_ != Phase::downloading) {
        self->StopPoll();
        return;
      }
      if (!read_ok) {
        self->StopPoll();
        self->phase_ = Phase::failed;
        self->error_ = translated(lang, "update.download-failed");
        self->Paint();
        return;
      }
      if (phase == "downloading") {
        self->percent_ = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
        self->Paint();
        return;
      }
      self->StopPoll();
      if (phase == "downloaded") {
        self->percent_ = 100;
        self->phase_ = Phase::downloaded;
      } else if (phase == "error") {
        self->phase_ = Phase::failed;
        self->error_ = message.empty()
                           ? translated(lang, "update.download-failed")
                           : message;
      } else {
        // The transfer left downloading without a result (superseded or
        // never started): surface a retryable failure, never a dead panel.
        self->phase_ = Phase::failed;
        self->error_ = translated(lang, "update.download-failed");
      }
      self->Paint();
    });
  }).detach();
}

void UpdateWindow::StopPoll() {
  if (poll_ != nullptr) poll_.Stop();
}

void UpdateWindow::StartInstall() {
  if (api_ == nullptr || phase_ != Phase::downloaded) return;
  install_.IsEnabled(false);
  auto weak = weak_from_this();
  auto api = api_;
  auto dispatcher = window_.DispatcherQueue();
  std::string const lang = language_;
  std::thread([weak, api, dispatcher, lang]() mutable {
    std::wstring path;
    std::wstring version;
    std::wstring error;
    try {
      auto const status = api->run_action("update.install", "").get();
      if (status == "quit-and-install") {
        auto const state = api->update_state().get();
        if (state.downloaded_path && !state.downloaded_path->empty()) {
          path = to_wide(*state.downloaded_path);
        }
        if (state.available_version) version = to_wide(*state.available_version);
        if (path.empty() || version.empty()) {
          error = frame(translated(lang, "update.install-failed"),
                        translated(lang, "updateUnknownError"));
        }
      } else {
        // No verified installable update (development copy, portable
        // tarball): the backend hands back a human-readable explanation.
        error = to_wide(status);
      }
    } catch (std::exception const& e) {
      error = frame(translated(lang, "update.install-failed"),
                    to_wide(e.what()));
    } catch (...) {
      error = frame(translated(lang, "update.install-failed"),
                    translated(lang, "updateUnknownError"));
    }
    dispatcher.TryEnqueue([weak, path, version, error, lang] {
      auto self = weak.lock();
      if (!self) return;
      if (!error.empty()) {
        self->install_.IsEnabled(true);
        self->phase_ = Phase::failed;
        self->error_ = error;
        self->Paint();
        return;
      }
      // The handoff launches the verified MSI and quits the app; a
      // returned error means the running version is unaffected.
      std::wstring handoff_error;
      if (self->handoff_) handoff_error = self->handoff_(path, version);
      if (!handoff_error.empty()) {
        self->install_.IsEnabled(true);
        self->phase_ = Phase::failed;
        self->error_ = frame(translated(lang, "update.install-failed"),
                             handoff_error);
        self->Paint();
      }
    });
  }).detach();
}

}  // namespace winrt::RivetHost::implementation