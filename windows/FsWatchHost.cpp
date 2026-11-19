#include "pch.h"
#include "FsWatchHost.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace fulcrum::windows {
namespace {

constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME |
                                FILE_NOTIFY_CHANGE_DIR_NAME |
                                FILE_NOTIFY_CHANGE_LAST_WRITE;
constexpr DWORD kReadBufferSize = 64 * 1024;

// FILE_NOTIFY_INFORMATION is variable-length; walk it and collect the
// changed directory for every event (the file's parent directory).
// All five actions matter: REMOVED is the only notification a pure
// deletion produces (ignoring it left deleted files searchable until an
// unrelated change touched the directory), and a cross-directory move
// delivers RENAMED_NEW_NAME to the destination — the old-name half only
// reconciles the source.
void CollectDirectories(std::byte const* buffer, DWORD bytes,
                        std::wstring const& root,
                        std::set<std::wstring>& into) {
  auto const* entry = reinterpret_cast<FILE_NOTIFY_INFORMATION const*>(buffer);
  for (;;) {
    if (entry->Action == FILE_ACTION_ADDED ||
        entry->Action == FILE_ACTION_REMOVED ||
        entry->Action == FILE_ACTION_MODIFIED ||
        entry->Action == FILE_ACTION_RENAMED_OLD_NAME ||
        entry->Action == FILE_ACTION_RENAMED_NEW_NAME) {
      auto const length = entry->FileNameLength / sizeof(WCHAR);
      std::wstring relative(entry->FileName, length);
      std::wstring directory = root;
      if (!directory.empty() && directory.back() != L'\\') {
        directory.push_back(L'\\');
      }
      auto const slash = relative.find_last_of(L'\\');
      if (slash == std::wstring::npos) {
        into.insert(directory);
      } else {
        into.insert(directory + relative.substr(0, slash) + L"\\");
      }
    }
    if (entry->NextEntryOffset == 0) {
      break;
    }
    entry = reinterpret_cast<FILE_NOTIFY_INFORMATION const*>(
        reinterpret_cast<std::byte const*>(entry) + entry->NextEntryOffset);
  }
}

// The RVT1 wire is UTF-8; directory names come from ReadDirectoryChangesW
// as UTF-16. Same conversion the other host files use (each TU keeps its
// own copy — the shared ones live in the windows' anonymous namespaces).
std::string Utf8FromWide(std::wstring const& wide) {
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

}  // namespace

FsWatchHost::~FsWatchHost() {
  Stop();
}

void FsWatchHost::Start(std::shared_ptr<rivet_app::API> api,
                        std::vector<std::wstring> roots) {
  if (state_) {
    return;  // already watching; Stop() first to re-arm from scratch
  }
  auto state = std::make_shared<State>();
  state->api = std::move(api);
  state->batch_semaphore = ::CreateSemaphoreW(nullptr, 0, LONG_MAX, nullptr);
  state->reconcile_thread = std::thread([state]() { ReconcileLoop(state); });
  for (auto& root : roots) {
    auto watch = std::make_unique<WatchRoot>();
    watch->root = root;
    watch->directory = ::CreateFileW(
        root.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (watch->directory == INVALID_HANDLE_VALUE) {
      continue;  // the root vanished between discovery and open: skip it
    }
    watch->stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    watch->read_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    watch->buffer = std::make_unique<std::byte[]>(kReadBufferSize);
    auto* raw = watch.get();
    state->watches.push_back(std::move(watch));
    raw->thread = std::thread([state, raw]() { WatchRootLoop(state, raw); });
  }
  state_ = std::move(state);
}

void FsWatchHost::Stop() {
  auto state = std::move(state_);
  if (!state) {
    return;
  }
  state->stopping.store(true, std::memory_order_release);
  for (auto& watch : state->watches) {
    if (watch->directory != INVALID_HANDLE_VALUE) {
      ::CancelIoEx(watch->directory, nullptr);
    }
    if (watch->stop_event != nullptr) {
      ::SetEvent(watch->stop_event);
    }
  }
  // Wake the reconcile worker so it observes the stop between RPCs.
  if (state->batch_semaphore != nullptr) {
    ::ReleaseSemaphore(state->batch_semaphore, 1, nullptr);
  }
  // NEVER join on the caller thread. The reconcile worker can sit inside a
  // synchronous fs-reconcile RPC while the backend is busy (a full catalog
  // scan holds SQLite write locks); joining here from QuitApplication froze
  // the UI thread, stopped the message pump and wedged the whole process.
  // The detached cleanup owns the shared state — the workers keep it (and
  // the backend API) alive until they exit, so the owning window can die.
  std::thread([state]() mutable {
    for (auto& watch : state->watches) {
      if (watch->thread.joinable()) {
        watch->thread.join();
      }
      if (watch->directory != INVALID_HANDLE_VALUE) {
        ::CloseHandle(watch->directory);
      }
      if (watch->stop_event != nullptr) {
        ::CloseHandle(watch->stop_event);
      }
      if (watch->read_event != nullptr) {
        ::CloseHandle(watch->read_event);
      }
    }
    state->watches.clear();
    if (state->reconcile_thread.joinable()) {
      state->reconcile_thread.join();
    }
    if (state->batch_semaphore != nullptr) {
      ::CloseHandle(state->batch_semaphore);
      state->batch_semaphore = nullptr;
    }
  }).detach();
}

void FsWatchHost::WatchRootLoop(std::shared_ptr<State> state, WatchRoot* watch) {
  OVERLAPPED overlapped{};
  overlapped.hEvent = watch->read_event;
  for (;;) {
    if (state->stopping.load(std::memory_order_acquire)) {
      return;
    }
    DWORD bytes = 0;
    ::ResetEvent(watch->read_event);
    BOOL const queued = ::ReadDirectoryChangesW(
        watch->directory, watch->buffer.get(), kReadBufferSize, TRUE,
        kNotifyFilter, &bytes, &overlapped, nullptr);
    if (!queued) {
      // The root was removed or is unavailable: nothing left to watch.
      return;
    }
    HANDLE const waits[] = {watch->read_event, watch->stop_event};
    DWORD const signaled = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (signaled == WAIT_OBJECT_0 + 1) {
      // Stop requested: cancel the pending read and exit. The read
      // completes with ERROR_OPERATION_ABORTED; GetOverlappedResult is
      // not needed on this path.
      ::CancelIoEx(watch->directory, &overlapped);
      DWORD ignored = 0;
      ::GetOverlappedResult(watch->directory, &overlapped, &ignored, TRUE);
      return;
    }
    DWORD result_bytes = 0;
    if (!::GetOverlappedResult(watch->directory, &overlapped, &result_bytes,
                               FALSE)) {
      continue;  // transient failure: re-arm and keep watching
    }
    if (result_bytes == 0) {
      // An overflow drains the buffer without events; re-arm. Mutations
      // in the gap are healed by the next scan or the next event batch.
      continue;
    }
    std::set<std::wstring> directories;
    CollectDirectories(watch->buffer.get(), result_bytes, watch->root,
                       directories);
    for (auto const& directory : directories) {
      EnqueueDirectory(*state, directory);
    }
  }
}

void FsWatchHost::EnqueueDirectory(State& state, std::wstring const& directory) {
  {
    std::lock_guard lock(state.batch_mutex);
    if (state.pending_dirs.insert(directory).second) {
      state.batch.push_back(directory);
    }
  }
  if (state.batch_semaphore != nullptr) {
    ::ReleaseSemaphore(state.batch_semaphore, 1, nullptr);
  }
}

void FsWatchHost::ReconcileLoop(std::shared_ptr<State> state) {
  // One reconcile worker: pops directories, dedupes, forwards each to the
  // backend's fs-reconcile RPC (the RPC can take hundreds of milliseconds;
  // the worker keeps draining meanwhile).
  using clock = std::chrono::steady_clock;
  auto const now_ms = [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               clock::now().time_since_epoch())
        .count();
  };
  auto const reconcile = [&state](std::wstring const& directory) {
    if (state->stopping.load(std::memory_order_acquire) || !state->api) {
      return;
    }
    try {
      (void)state->api->fs_reconcile(Utf8FromWide(directory)).get();
    } catch (...) {
      // A reconcile failure (backend busy, transient SQLite lock) is
      // healed by the next change in that directory; never surface it.
    }
  };
  while (!state->stopping.load(std::memory_order_acquire)) {
    // A timeout is not a skip: due delete-pending re-checks below must
    // fire even when no fresh event batch arrives.
    (void)::WaitForSingleObject(state->batch_semaphore, 500);
    // Due deferred re-checks first. A Windows delete notification can
    // fire while the file still sits in delete-pending state (antivirus
    // or an indexer holds a handle): the immediate reconcile still lists
    // it, and the vanish after the handle releases carries NO second
    // notification. The same 2/4/6/8 s ladder the backend's Racket
    // flusher uses spans that window.
    auto const now = now_ms();
    for (auto it = state->recheck.begin(); it != state->recheck.end();) {
      auto& deadlines = it->second;
      auto const due = static_cast<std::size_t>(
          std::count_if(deadlines.begin(), deadlines.end(),
                        [now](std::int64_t ms) { return ms <= now; }));
      if (due == 0) {
        ++it;
        continue;
      }
      reconcile(it->first);
      deadlines.erase(deadlines.begin(),
                      deadlines.begin() + static_cast<std::ptrdiff_t>(due));
      if (deadlines.empty()) {
        it = state->recheck.erase(it);
      } else {
        ++it;
      }
    }
    for (;;) {
      std::wstring directory;
      {
        std::lock_guard lock(state->batch_mutex);
        if (state->batch.empty()) {
          break;
        }
        directory = std::move(state->batch.front());
        state->batch.pop_front();
        // Release the dedupe slot as the directory leaves the batch: a
        // change arriving during (or after) its reconcile must enqueue
        // again. Holding the slot forever reconciled every directory at
        // most once per session and silently dropped every later change.
        state->pending_dirs.erase(directory);
      }
      reconcile(directory);
      auto const scheduled = now_ms();
      state->recheck[directory] = {scheduled + 2000, scheduled + 4000,
                                   scheduled + 6000, scheduled + 8000};
    }
  }
}

}  // namespace fulcrum::windows