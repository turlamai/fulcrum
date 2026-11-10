#pragma once

#include "pch.h"
#include "GeneratedBackend.hpp"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fulcrum::windows {

// Watches the launcher's catalog roots for filesystem changes and forwards
// changed directories to the backend's fs-reconcile RPC. The watcher threads
// live here — in the host process, outside the embedded runtime whose
// scheduler starves watcher threads in the packaged build (issue #211). The
// backend stays the single owner of the catalog database; the host never
// touches it, it only names changed directories over RVT1.
//
// Coverage mirrors the catalog's roots (profile + fixed local volumes);
// directories outside any root come back "outside" from the RPC and are
// dropped. Each root is watched recursively (subtree).
class FsWatchHost {
public:
  FsWatchHost() = default;
  ~FsWatchHost();
  FsWatchHost(FsWatchHost const&) = delete;
  FsWatchHost& operator=(FsWatchHost const&) = delete;

  // Roots are the absolute directories to watch recursively (no trailing
  // separator). One watcher thread per root; one reconcile worker drains
  // the batch. Idempotent: Start after Start re-arms from scratch.
  void Start(std::shared_ptr<rivet_app::API> api, std::vector<std::wstring> roots);
  void Stop();

 private:
  struct WatchRoot {
    HANDLE directory{nullptr};
    HANDLE stop_event{nullptr};
    HANDLE read_event{nullptr};
    std::unique_ptr<std::byte[]> buffer;
    std::thread thread;
    std::wstring root;
  };

  void WatchRootLoop(WatchRoot* watch);
  void EnqueueDirectory(std::wstring const& directory);
  void ReconcileLoop();

  std::shared_ptr<rivet_app::API> api_;
  std::vector<std::unique_ptr<WatchRoot>> watches_;
  // Batch of changed directories, guarded by batch_mutex_ and released by
  // batch_semaphore_ for the reconcile worker.
  std::mutex batch_mutex_;
  std::set<std::wstring> pending_dirs_;
  std::deque<std::wstring> batch_;
  HANDLE batch_semaphore_{nullptr};
  std::atomic<bool> stopping_{false};
  std::thread reconcile_thread_;
  std::atomic<bool> started_{false};
};

}  // namespace fulcrum::windows
