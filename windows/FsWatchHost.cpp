#include "pch.h"
#include "FsWatchHost.h"

#include <windows.h>

namespace fulcrum::windows {
namespace {

constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME |
                                FILE_NOTIFY_CHANGE_DIR_NAME |
                                FILE_NOTIFY_CHANGE_LAST_WRITE;
constexpr DWORD kReadBufferSize = 64 * 1024;

// FILE_NOTIFY_INFORMATION is variable-length; walk it and collect the
// changed directory for every event (the file's parent directory).
void CollectDirectories(std::byte const* buffer, DWORD bytes,
                        std::wstring const& root,
                        std::set<std::wstring>& into) {
  auto const* entry = reinterpret_cast<FILE_NOTIFY_INFORMATION const*>(buffer);
  for (;;) {
    if (entry->Action == FILE_ACTION_ADDED ||
        entry->Action == FILE_ACTION_MODIFIED ||
        entry->Action == FILE_ACTION_RENAMED_OLD_NAME) {
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
  if (started_.exchange(true)) {
    return;
  }
  api_ = std::move(api);
  batch_semaphore_ = ::CreateSemaphoreW(nullptr, 0, LONG_MAX, nullptr);
  stopping_.store(false, std::memory_order_release);
  reconcile_thread_ = std::thread([this]() { ReconcileLoop(); });
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
    watches_.push_back(std::move(watch));
    raw->thread = std::thread([this, raw]() { WatchRootLoop(raw); });
  }
}

void FsWatchHost::Stop() {
  if (!started_.exchange(false)) {
    return;
  }
  stopping_.store(true, std::memory_order_release);
  for (auto& watch : watches_) {
    if (watch->directory != INVALID_HANDLE_VALUE) {
      ::CancelIoEx(watch->directory, nullptr);
    }
    if (watch->stop_event != nullptr) {
      ::SetEvent(watch->stop_event);
    }
  }
  for (auto& watch : watches_) {
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
  watches_.clear();
  // Let the reconcile worker drain (and observe the stop) before teardown.
  if (batch_semaphore_ != nullptr) {
    ::ReleaseSemaphore(batch_semaphore_, 1, nullptr);
  }
  if (reconcile_thread_.joinable()) {
    reconcile_thread_.join();
  }
  if (batch_semaphore_ != nullptr) {
    ::CloseHandle(batch_semaphore_);
    batch_semaphore_ = nullptr;
  }
}

void FsWatchHost::WatchRootLoop(WatchRoot* watch) {
  OVERLAPPED overlapped{};
  overlapped.hEvent = watch->read_event;
  for (;;) {
    if (stopping_.load(std::memory_order_acquire)) {
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
      EnqueueDirectory(directory);
    }
  }
}

void FsWatchHost::EnqueueDirectory(std::wstring const& directory) {
  {
    std::lock_guard lock(batch_mutex_);
    if (pending_dirs_.insert(directory).second) {
      batch_.push_back(directory);
    }
  }
  if (batch_semaphore_ != nullptr) {
    ::ReleaseSemaphore(batch_semaphore_, 1, nullptr);
  }
}

void FsWatchHost::ReconcileLoop() {
  // One reconcile worker: pops directories, dedupes, forwards each to the
  // backend's fs-reconcile RPC on its own short thread (the RPC can take
  // hundreds of milliseconds; the worker keeps draining meanwhile).
  while (!stopping_.load(std::memory_order_acquire)) {
    if (::WaitForSingleObject(batch_semaphore_, 500) != WAIT_OBJECT_0) {
      continue;
    }
    for (;;) {
      std::wstring directory;
      {
        std::lock_guard lock(batch_mutex_);
        if (batch_.empty()) {
          break;
        }
        directory = std::move(batch_.front());
        batch_.pop_front();
      }
      if (stopping_.load(std::memory_order_acquire) || !api_) {
        continue;
      }
      try {
        (void)api_->fs_reconcile(Utf8FromWide(directory)).get();
      } catch (...) {
        // A reconcile failure (backend busy, transient SQLite lock) is
        // healed by the next change in that directory; never surface it.
      }
    }
  }
}

}  // namespace fulcrum::windows
