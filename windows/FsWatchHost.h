#pragma once

#include "pch.h"
#include "GeneratedBackend.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
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
//
// Lifetime: all mutable state lives in a shared_ptr<State> captured by the
// worker threads, and Stop() never joins on the caller thread. The reconcile
// worker can sit inside a synchronous fs-reconcile RPC for a long time while
// the backend is busy (a full catalog scan holds SQLite write locks); joining
// it from QuitApplication froze the UI thread and wedged the whole process
// (packaged acceptance: main HWND stopped pumping, settings window survived).
// A detached cleanup thread owns the state and joins the workers off the UI
// thread, so the host object — and the window that owns it — can die safely.
class FsWatchHost {
public:
  FsWatchHost() = default;
  ~FsWatchHost();
  FsWatchHost(FsWatchHost const&) = delete;
  FsWatchHost& operator=(FsWatchHost const&) = delete;

  // Roots are the absolute directories to watch recursively (no trailing
  // separator). One watcher thread per root; one reconcile worker drains
  // the batch. Idempotent: Start while already started is a no-op.
  void Start(std::shared_ptr<rivet_app::API> api, std::vector<std::wstring> roots);
  // Signals the workers and returns immediately; a detached cleanup thread
  // joins them and closes the handles. Safe to call from the UI thread even
  // while a reconcile RPC is in flight.
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

  struct State {
    std::shared_ptr<rivet_app::API> api;
    std::vector<std::unique_ptr<WatchRoot>> watches;
    // Batch of changed directories, guarded by batch_mutex and released by
    // batch_semaphore for the reconcile worker.
    std::mutex batch_mutex;
    std::set<std::wstring> pending_dirs;
    std::deque<std::wstring> batch;
    // Delete-pending re-check ladder (dir -> due timestamps, ms). Owned by
    // the reconcile thread alone; no lock needed.
    std::map<std::wstring, std::vector<std::int64_t>> recheck;
    HANDLE batch_semaphore{nullptr};
    std::atomic<bool> stopping{false};
    std::thread reconcile_thread;
  };

  // Worker bodies are static and take the shared state, never `this`: they
  // must stay valid after the owning window (and this object) is destroyed.
  static void WatchRootLoop(std::shared_ptr<State> state, WatchRoot* watch);
  static void EnqueueDirectory(State& state, std::wstring const& directory);
  static void ReconcileLoop(std::shared_ptr<State> state);

  std::shared_ptr<State> state_;
};

}  // namespace fulcrum::windows