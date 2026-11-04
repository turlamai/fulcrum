#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "WindowsInstance.h"
#include "Autostart.h"

namespace winrt::RivetHost::implementation {

App::App() {
  InitializeComponent();

#if defined(_DEBUG) && !defined(DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION)
  UnhandledException([](winrt::Windows::Foundation::IInspectable const&,
                        Microsoft::UI::Xaml::UnhandledExceptionEventArgs const& e) {
    if (::IsDebuggerPresent()) {
      auto const message = e.Message();
      (void)message;
      __debugbreak();
    }
  });
#endif
}

void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&) {
  // One instance owns the global hotkey; second launches exit here (the
  // first-party rivet lease, not a hand-rolled mutex). Product acceptance
  // supplies a per-run id so the packaged candidate never forwards into an
  // installed production instance on the same runner.
  auto const application_id = fulcrum::windows::InstanceId();
  instance_lease_ = std::make_unique<rivet::system::SingleInstanceLease>(
      application_id);
  if (!instance_lease_->is_primary()) {
    // Login item activation must be silent when Fulcrum is already resident:
    // do not summon the launcher or show an install-recovery dialog.
    if (fulcrum::windows::BackgroundLaunch()) {
      Microsoft::UI::Xaml::Application::Current().Exit();
      return;
    }
    // MSI handoff/repair may launch while the prior process is still
    // releasing the lease, before its WinUI window is discoverable. Do not
    // silently exit a launch that has neither summoned the owner nor
    // acquired a usable lease (#148). Only the exact marked resident window
    // can receive our summon; never terminate another RivetHost.exe.
    bool summoned = false;
    // Bound the whole retry budget to a few seconds even when an existing
    // HWND is hung (SendMessageTimeout runs for every attempt).
    for (int attempt = 0; attempt < 20; ++attempt) {
      if (HWND existing = fulcrum::windows::FindResidentWindow()) {
        DWORD_PTR ignored{};
        if (::SendMessageTimeoutW(
                existing, fulcrum::windows::kSummonResidentMessage, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored) != 0) {
          summoned = true;
          break;
        }
      }
      // A shutdown in progress can free the single-instance ownership.
      // Recreate the lease so this invocation can become the new primary.
      ::Sleep(100);
      instance_lease_.reset();
      instance_lease_ = std::make_unique<rivet::system::SingleInstanceLease>(
          application_id);
      if (instance_lease_->is_primary()) break;
    }
    if (summoned) {
      Microsoft::UI::Xaml::Application::Current().Exit();
      return;
    }
    if (!instance_lease_->is_primary()) {
      ::MessageBoxW(
          nullptr,
          L"Fulcrum is still running but its launcher is not responding. "
          L"Please quit Fulcrum from the system tray, then open it again. "
          L"If an installation is in progress, finish it first.",
          L"Fulcrum could not start", MB_OK | MB_ICONWARNING | MB_TOPMOST);
      Microsoft::UI::Xaml::Application::Current().Exit();
      return;
    }
  }

  // Keep the initial HWND hidden until the backend's first results are ready.
  window_ = winrt::make<MainWindow>();
}

}  // namespace winrt::RivetHost::implementation
