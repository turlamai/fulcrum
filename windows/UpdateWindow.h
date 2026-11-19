#pragma once

#include "pch.h"
#include "GeneratedBackend.hpp"
#include "winui_window.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace winrt::RivetHost::implementation {

// The dedicated update window — the Windows twin of the macOS
// UpdateController/UpdatePanelView pair (update-flow parity,
// design/DESIGN.md and docs/cross-platform-interaction.md). Every entry
// point (tray "Check for Updates…", the launcher update rows, the Settings
// Sync & Updates button) opens THIS window; the launcher's transient status
// line is no longer an update surface.
//
// Phases mirror macOS: idle → checking → up-to-date / available →
// downloading → downloaded (Windows: verified MSI handoff needs an explicit
// Quit and Install) / failed. Close hides the window and cancels a pending
// check; a running download continues and the window can be reopened.
// Cancel-download is macOS-only: there the host owns the transfer, while
// the Windows transfer is backend-owned (declared platform difference).
class UpdateWindow : public std::enable_shared_from_this<UpdateWindow> {
 public:
  // Runs on the UI thread with the verified MSI path + manifest version.
  // Returns an empty string on success (the app quits into the installer);
  // any other value is the failure detail shown in the window.
  using InstallHandoff =
      std::function<std::wstring(std::wstring const&, std::wstring const&)>;

  UpdateWindow(std::shared_ptr<rivet_app::API> api, InstallHandoff handoff);

  // Show the window and run a manual check (no-op check while a check or
  // download is already in flight — the window still comes forward).
  void ShowAndCheck();
  // Show without starting a check (the silent startup check already has a
  // result, or a download is running and the user reopened the window).
  void Show();
  // The silent startup check found an eligible update: present it without
  // a second network round-trip (macOS silent-check parity).
  void PresentAvailable(std::wstring version, std::int64_t size_bytes);
  // Acceptance-only: present a completed check without touching the network
  // (the macOS presentCompletedCheckForAcceptance twin).
  void PresentCompletedCheckForAcceptance();
  // A backend language push repaints an open window.
  void RefreshLanguage(std::string const& language);
  // QuitApplication calls this before Application.Exit: the hide-on-close
  // veto must not block an explicit application shutdown.
  void AllowClose();

 private:
  enum class Phase {
    idle,
    checking,
    up_to_date,
    available,
    downloading,
    downloaded,
    failed,
  };

  void EnsureWindow();
  void Dismiss();
  void Paint();
  void StartCheck();
  void StartDownload();
  void StartInstall();
  void OnPollTick();
  void StopPoll();

  std::shared_ptr<rivet_app::API> api_;
  InstallHandoff handoff_;
  std::string language_{"system"};

  winrt::Microsoft::UI::Xaml::Window window_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock title_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock status_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ProgressRing ring_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ProgressBar bar_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button check_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button close_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button download_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button install_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button retry_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer poll_{nullptr};

  Phase phase_{Phase::idle};
  int percent_{0};
  std::wstring available_version_;
  std::int64_t available_size_{0};
  std::wstring error_;
  // A dismissed check must not repaint (or reopen) the window: every check
  // captures the generation it started under, Dismiss bumps it.
  std::atomic<std::uint64_t> check_generation_{0};
  std::atomic<bool> polling_{false};
  std::atomic<bool> allow_close_{false};
};

}  // namespace winrt::RivetHost::implementation