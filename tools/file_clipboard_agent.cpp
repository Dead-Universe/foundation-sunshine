// Runs in the console user's token; the Sunshine service never opens user files.
#include "src/file_clipboard/file_clipboard_pipe.h"
#include <aclapi.h>
#include <wtsapi32.h>
#include <map>

namespace fc = streaming::file_clipboard;
namespace wire = fc::wire;
namespace {
struct pending_read {
  std::mutex mutex; std::condition_variable ready; bool done = false; wire::bytes body;
};
struct remote_reader final : fc::reader {
  std::function<HRESULT(std::size_t, std::uint64_t, void *, ULONG, ULONG &)> read;
  std::timed_mutex gate;
  HRESULT read_at(std::size_t index, std::uint64_t offset, void *buffer, ULONG count, ULONG &got) override {
    got = 0;
    std::unique_lock<std::timed_mutex> lock(gate, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(20))) return STG_E_INUSE;
    return read(index, offset, buffer, count, got);
  }
};
class agent {
public:
  explicit agent(std::shared_ptr<wire::pipe> connection): pipe_(std::move(connection)) {}
  ~agent() {
    stopped_ = true; pipe_->stop(); generation_++;
    publisher_.observe({}); cancel_reads(); publisher_.clear();
    // Drain callbacks while all state they capture still exists.
    workers_.reset();
  }
  int run() {
    workers_ = std::make_unique<wire::workers>();
    publisher_.observe([this](std::uint32_t, std::vector<std::wstring> paths) {
      auto epoch = ++generation_;
      cancel_reads();
      { std::lock_guard<std::mutex> lock(mutex_); source_.reset(); }
      pipe_->send({wire::kind::offer, 0, {}});
      if (paths.empty()) return;
      workers_->post([this, epoch, paths = std::move(paths)] {
        std::shared_ptr<fc::source_files> source;
        auto status = fc::source_files::collect(paths, [this, epoch] { return stopped_ || generation_ != epoch; }, source);
        if (FAILED(status) || generation_ != epoch || stopped_) return;
        auto manifest = wire::manifest(source->entries());
        if (manifest.empty()) return;
        { std::lock_guard<std::mutex> lock(mutex_); if (generation_ != epoch) return; source_ = source; }
        pipe_->send({wire::kind::offer, epoch, std::move(manifest)});
      });
    });
    if (!pipe_->send({wire::kind::hello, 1, {}})) return 1;
    wire::message message;
    while (pipe_->receive(message)) {
      if (message.type == wire::kind::reply) {
        std::shared_ptr<pending_read> read;
        { std::lock_guard<std::mutex> lock(mutex_); auto found = reads_.find(message.id);
          if (found != reads_.end()) { read = found->second; reads_.erase(found); } }
        if (read) { { std::lock_guard<std::mutex> lock(read->mutex); read->body = std::move(message.body); read->done = true; } read->ready.notify_all(); }
      } else if (message.type == wire::kind::publish) {
        auto value = std::make_shared<fc::offer>();
        if (!message.id || !wire::manifest(message.body, value->entries) || !fc::interactive_desktop_available()) {
          publisher_.clear(); cancel_reads(); continue;
        }
        generation_++; { std::lock_guard<std::mutex> lock(mutex_); source_.reset(); }
        cancel_reads();
        const auto remote_epoch = message.id;
        auto remote = std::make_shared<remote_reader>();
        remote->read = [this, remote_epoch](std::size_t index, std::uint64_t offset, void *buffer, ULONG count, ULONG &got) {
          got = 0;
          if (stopped_ || !fc::interactive_desktop_available()) return E_ACCESSDENIED;
          auto request = std::make_shared<pending_read>(); auto id = ++request_id_;
          { std::lock_guard<std::mutex> lock(mutex_); if (reads_.size() >= 4) return STG_E_INUSE; reads_[id] = request; }
          if (!pipe_->send({wire::kind::request, id, wire::query({remote_epoch, static_cast<std::uint32_t>(index), offset, count})})) return STG_E_REVERTED;
          std::unique_lock<std::mutex> lock(request->mutex);
          bool ready = request->ready.wait_for(lock, std::chrono::seconds(16), [&] { return request->done || stopped_; });
          lock.unlock();
          { std::lock_guard<std::mutex> guard(mutex_); reads_.erase(id); }
          if (!ready || stopped_) return STG_E_REVERTED;
          return wire::reply(request->body, buffer, count, got);
        };
        value->source = remote; publisher_.publish(std::move(value));
      } else if (message.type == wire::kind::request) {
        const auto id = message.id; wire::read_query query;
        std::shared_ptr<fc::source_files> source;
        { std::lock_guard<std::mutex> lock(mutex_); source = source_; }
        if (!wire::query(message.body, query) || !source || !wire::within(query, generation_, source->entries())) {
          pipe_->send({wire::kind::reply, id, wire::reply(STG_E_REVERTED)}); continue;
        }
        if (!workers_->post([this, source, query, id] {
          wire::bytes data(query.count); ULONG got = 0;
          auto status = generation_ == query.generation ? source->read_at(query.index, query.offset, data.data(), query.count, got) : STG_E_REVERTED;
          if (generation_ != query.generation) status = STG_E_REVERTED;
          pipe_->send({wire::kind::reply, id, wire::reply(status, data.data(), SUCCEEDED(status) ? got : 0)});
        })) pipe_->send({wire::kind::reply, id, wire::reply(STG_E_INUSE)});
      }
    }
    return 0;
  }
private:
  void cancel_reads() {
    std::map<std::uint64_t, std::shared_ptr<pending_read>> reads;
    { std::lock_guard<std::mutex> lock(mutex_); reads.swap(reads_); }
    for (auto &[id, read] : reads) {
      { std::lock_guard<std::mutex> lock(read->mutex); read->body = wire::reply(STG_E_REVERTED); read->done = true; }
      read->ready.notify_all();
    }
  }
  std::shared_ptr<wire::pipe> pipe_; std::mutex mutex_;
  std::atomic<bool> stopped_ {false}; std::atomic<std::uint64_t> generation_ {1}, request_id_ {1};
  std::shared_ptr<fc::source_files> source_; std::map<std::uint64_t, std::shared_ptr<pending_read>> reads_;
  fc::publisher publisher_; std::unique_ptr<wire::workers> workers_;
};
}
int wmain(int argc, wchar_t **argv) {
  if (argc != 3) return 2;
  const std::wstring name = argv[1];
  if (name.rfind(L"\\\\.\\pipe\\SunshineFileClipboard-", 0) != 0) return 2;
  wchar_t *end = nullptr; auto expected = wcstoul(argv[2], &end, 10);
  if (!expected || !end || *end) return 2;
  HANDLE handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return 3;
  ULONG actual = 0;
  if (!GetNamedPipeServerProcessId(handle, &actual) || actual != expected) { CloseHandle(handle); return 4; }
  // A standard user's token cannot inspect the restricted service process.
  // Authenticate the kernel pipe's SYSTEM owner and server session instead;
  // no process ACL or user privilege is relaxed for the clipboard helper.
  ULONG session = MAXDWORD; PSID owner = nullptr; PSECURITY_DESCRIPTOR security = nullptr;
  if (!GetNamedPipeServerSessionId(handle, &session) || session != 0) { CloseHandle(handle); return 5; }
  const auto status = GetSecurityInfo(handle, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
                                     nullptr, nullptr, nullptr, &security);
  bool valid = status == ERROR_SUCCESS && owner && IsWellKnownSid(owner, WinLocalSystemSid);
  if (security) LocalFree(security);
  if (!valid) { CloseHandle(handle); return status == ERROR_SUCCESS ? 6 : 0x10000 | status; }
  agent application(std::make_shared<wire::pipe>(handle)); return application.run();
}
