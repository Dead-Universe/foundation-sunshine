#include "file_clipboard.h"
#ifdef _WIN32
#include "file_clipboard/file_clipboard_pipe.h"
#include "config.h"
#include "logging.h"
#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>
#include <openssl/rand.h>
#include <filesystem>
#include <map>

namespace file_clipboard {
namespace {
namespace wire = streaming::file_clipboard::wire;
using clock = std::chrono::steady_clock;
using response = std::shared_ptr<web::ServerBase<nvhttp::SunshineHTTPS>::Response>;
using request = nvhttp::req_https_t;
const web::CaseInsensitiveMultimap headers {{"Content-Type", "application/octet-stream"}, {"Cache-Control", "no-store"}};
void write(response out, web::StatusCode status, const wire::bytes &data = {}) {
  out->write(status, std::string_view(reinterpret_cast<const char *>(data.data()), data.size()), headers);
}
std::string random_key() {
  unsigned char value[32]; if (RAND_bytes(value, sizeof(value)) != 1) return {};
  const char hex[] = "0123456789abcdef"; std::string out;
  for (auto c : value) { out += hex[c >> 4]; out += hex[c & 15]; } return out;
}
struct owned_handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~owned_handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::shared_ptr<wire::pipe> launch_agent(DWORD session, HANDLE &process) {
  auto failure = [](const char *stage, DWORD error) {
    static auto next_log = clock::time_point {};
    if (clock::now() >= next_log) {
      BOOST_LOG(warning) << "File clipboard: helper startup failed at " << stage << " (status " << error << ")";
      next_log = clock::now() + std::chrono::seconds(30);
    }
    return std::shared_ptr<wire::pipe> {};
  };
  owned_handle token;
  if (!WTSQueryUserToken(session, &token.value)) return failure("user token", GetLastError());
  DWORD size = 0; GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> user(size);
  if (!size || !GetTokenInformation(token.value, TokenUser, user.data(), size, &size)) return failure("token identity", GetLastError());
  auto sid = reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid;
  LPWSTR sid_string = nullptr;
  if (!ConvertSidToStringSidW(sid, &sid_string)) return failure("user SID", GetLastError());
  // A SYSTEM token's default owner may be the Administrators group. Pin the
  // pipe owner explicitly so the standard-user helper can authenticate it.
  std::wstring acl = L"O:SYD:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid_string) + L")"; LocalFree(sid_string);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return failure("pipe security", GetLastError());
  SECURITY_ATTRIBUTES security {sizeof(security), descriptor, FALSE};
  auto key = random_key(); if (key.empty()) { LocalFree(descriptor); return {}; }
  std::wstring name = L"\\\\.\\pipe\\SunshineFileClipboard-" + std::wstring(key.begin(), key.end());
  HANDLE handle = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1024 * 1024, 1024 * 1024, 0, &security);
  const DWORD pipe_error = GetLastError();
  LocalFree(descriptor); if (handle == INVALID_HANDLE_VALUE) return failure("pipe creation", pipe_error);
  auto pipe = std::make_shared<wire::pipe>(handle, true);
  OVERLAPPED connect {}; owned_handle event; event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr); connect.hEvent = event.value;
  if (!event.value) return failure("pipe event", GetLastError());
  BOOL connected = ConnectNamedPipe(handle, &connect); auto connect_error = connected ? ERROR_SUCCESS : GetLastError();
  auto cancel_connect = [&] {
    pipe->stop();
    if (!connected && connect_error == ERROR_IO_PENDING) GetOverlappedResult(handle, &connect, &size, TRUE);
  };
  wchar_t executable[32768] {}; GetModuleFileNameW(nullptr, executable, 32768);
  auto helper = std::filesystem::path(executable).parent_path() / L"tools" / L"sunshine-file-clipboard.exe";
  std::wstring command = L"\"" + helper.wstring() + L"\" \"" + name + L"\" " + std::to_wstring(GetCurrentProcessId());
  STARTUPINFOW startup {}; startup.cb = sizeof(startup); startup.lpDesktop = const_cast<wchar_t *>(L"winsta0\\default");
  PROCESS_INFORMATION child {}; void *environment = nullptr;
  if (!CreateEnvironmentBlock(&environment, token.value, FALSE)) { auto error = GetLastError(); cancel_connect(); return failure("user environment", error); }
  BOOL created = CreateProcessAsUserW(token.value, helper.c_str(), command.data(), nullptr, nullptr, FALSE,
      CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB,
      environment, helper.parent_path().c_str(), &startup, &child);
  const DWORD creation_error = GetLastError();
  if (environment) DestroyEnvironmentBlock(environment);
  if (!created) { cancel_connect(); return failure("process creation", creation_error); }
  CloseHandle(child.hThread); process = child.hProcess;
  if (!connected && connect_error == ERROR_IO_PENDING) connected = WaitForSingleObject(event.value, 5000) == WAIT_OBJECT_0;
  else if (connect_error == ERROR_PIPE_CONNECTED) connected = TRUE;
  if (!connected) { cancel_connect(); return failure("pipe connection", WAIT_TIMEOUT); }
  ULONG client_pid = 0, client_session = MAXDWORD;
  bool valid = GetNamedPipeClientProcessId(handle, &client_pid) && client_pid == child.dwProcessId &&
      GetNamedPipeClientSessionId(handle, &client_session) && client_session == session;
  // ImpersonateNamedPipeClient authenticates the last message actually read.
  // Read the fixed hello before querying its token, with a bounded startup wait.
  if (valid) {
    DWORD available = 0; const auto deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline && WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT) {
      if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr) || available >= 16) break;
      Sleep(10);
    }
    wire::message hello;
    valid = available >= 16 && pipe->receive(hello) && hello.type == wire::kind::hello && hello.id == 1 && hello.body.empty();
  }
  if (valid && ImpersonateNamedPipeClient(handle)) {
    owned_handle client_token;
    valid = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &client_token.value);
    DWORD bytes = 0;
    if (valid) {
      GetTokenInformation(client_token.value, TokenUser, nullptr, 0, &bytes);
      std::vector<unsigned char> actual(bytes);
      valid = bytes && GetTokenInformation(client_token.value, TokenUser, actual.data(), bytes, &bytes) &&
          EqualSid(sid, reinterpret_cast<TOKEN_USER *>(actual.data())->User.Sid);
    }
    if (!RevertToSelf()) std::terminate();
  } else valid = false;
  if (!valid) {
    DWORD exit = STILL_ACTIVE; GetExitCodeProcess(child.hProcess, &exit);
    pipe->stop(); return failure("helper authentication", exit);
  }
  return pipe;
}
class hub {
public:
  hub() { thread_ = std::thread([this] { run(); }); }
  ~hub() { stop(); }
  void stop() {
    stopped_ = true;
    wake_.notify_all();
    { std::lock_guard<std::mutex> lock(mutex_); if (pipe_) pipe_->stop(); }
    if (thread_.joinable()) thread_.join();
  }
  void started(std::uint32_t id, const std::string &uuid) {
    std::lock_guard<std::mutex> lock(mutex_); sessions_[id] = uuid;
  }
  void ended(std::uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_); sessions_.erase(id); if (id == session_) revoke();
  }
  void authorize_with(std::function<bool(std::string_view)> paired) {
    std::lock_guard<std::mutex> lock(mutex_); paired_ = std::move(paired);
  }
  void open(response out, request in) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = std::find_if(sessions_.begin(), sessions_.end(), [&](const auto &value) { return value.second == in->client_uuid; });
    if (found == sessions_.end() || !config::input.clipboard_sync) { write(out, web::StatusCode::conflict); return; }
    if (!uuid_.empty() && uuid_ != in->client_uuid) { write(out, web::StatusCode::conflict); return; }
    if (uuid_.empty()) {
      key_ = random_key(); if (key_.empty()) { write(out, web::StatusCode::service_unavailable); return; }
      uuid_ = in->client_uuid; session_ = found->first;
    }
    last_seen_ = clock::now();
    if (!pipe_ || !pipe_->alive() || !ready_) { write(out, web::StatusCode::service_unavailable); return; }
    wire::bytes result(key_.begin(), key_.end()); write(out, web::StatusCode::ok, result);
  }
  bool authorized(const request &in) {
    auto key = in->header.find("X-Clipboard-Session");
    if (uuid_.empty() || in->client_uuid != uuid_ || key == in->header.end() || key->second != key_ ||
        !sessions_.count(session_) || !config::input.clipboard_sync) return false;
    last_seen_ = clock::now(); return true;
  }
  void events(response out, request in) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!authorized(in)) { write(out, web::StatusCode::forbidden); return; }
    if (event_response_) write(std::move(event_response_), web::StatusCode::conflict);
    event_response_ = std::move(out); event_deadline_ = clock::now() + std::chrono::seconds(10);
    flush();
  }
  void send(response out, request in, bool read_file) {
    const auto body = in->content.string(); wire::message message;
    if (body.size() > wire::max_manifest_bytes + 16 || !wire::decode(wire::bytes(body.begin(), body.end()), message)) {
      write(out, web::StatusCode::bad_request); return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!authorized(in)) { write(out, web::StatusCode::forbidden); return; }
    if (!pipe_ || !ready_) { write(out, web::StatusCode::service_unavailable); return; }
    if (read_file) {
      wire::read_query query;
      if (message.type != wire::kind::request || !wire::query(message.body, query) || !wire::within(query, source_generation_, source_entries_) || reads_.size() >= 4) {
        write(out, web::StatusCode::conflict); return;
      }
      const auto id = ++request_id_;
      reads_[id] = {std::move(out), query.count, clock::now() + std::chrono::seconds(15)};
      enqueue({wire::kind::request, id, std::move(message.body)});
      return;
    }
    if (message.type == wire::kind::offer) {
      std::vector<streaming::file_clipboard::entry> entries;
      if (message.id && !wire::manifest(message.body, entries)) { write(out, web::StatusCode::bad_request); return; }
      if (!message.id && !message.body.empty()) { write(out, web::StatusCode::bad_request); return; }
      client_generation_ = message.id; client_entries_ = std::move(entries);
      source_generation_ = 0; source_entries_.clear();
      enqueue({wire::kind::publish, message.id, std::move(message.body)});
    } else if (message.type == wire::kind::reply) {
      auto expected = client_reads_.find(message.id);
      if (expected == client_reads_.end() || message.body.size() < 4 || message.body.size() > expected->second.count + 4) {
        write(out, web::StatusCode::conflict); return;
      }
      client_reads_.erase(expected); enqueue(std::move(message));
    } else { write(out, web::StatusCode::bad_request); return; }
    write(out, web::StatusCode::ok);
  }
private:
  struct read_response { response out; std::uint32_t count; clock::time_point deadline; };
  void fail_reads() {
    for (auto &[id, value] : reads_) write(std::move(value.out), web::StatusCode::gone);
    reads_.clear(); client_reads_.clear();
  }
  void revoke() {
    key_.clear(); uuid_.clear(); session_ = 0; source_generation_ = 0; client_generation_ = 0;
    source_entries_.clear(); client_entries_.clear(); events_.clear(); outgoing_.clear(); fail_reads();
    if (event_response_) write(std::move(event_response_), web::StatusCode::gone);
    if (pipe_) pipe_->stop();
  }
  void enqueue(wire::message value) {
    if (outgoing_.size() < 16) outgoing_.push_back(std::move(value)); else if (pipe_) pipe_->stop();
    wake_.notify_one();
  }
  void event(wire::message value) {
    if (value.type == wire::kind::offer) {
      events_.erase(std::remove_if(events_.begin(), events_.end(), [](const auto &item) { return item.type == wire::kind::offer; }), events_.end());
    }
    if (events_.size() < 16) events_.push_back(std::move(value)); else if (pipe_) pipe_->stop();
    flush();
  }
  void flush() {
    if (!event_response_) return;
    if (!events_.empty()) {
      auto value = wire::encode(events_.front()); events_.pop_front();
      write(std::move(event_response_), web::StatusCode::ok, value);
    } else if (clock::now() >= event_deadline_) write(std::move(event_response_), web::StatusCode::no_content);
  }
  void received(const std::shared_ptr<wire::pipe> &connection, wire::message message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pipe_ != connection || !connection->alive() || uuid_.empty()) return;
    if (message.type == wire::kind::hello) { ready_ = message.id == 1; return; }
    if (message.type == wire::kind::offer) {
      std::vector<streaming::file_clipboard::entry> entries;
      if (message.id && !wire::manifest(message.body, entries)) { if (pipe_) pipe_->stop(); return; }
      source_generation_ = message.id; source_entries_ = std::move(entries);
      client_generation_ = 0; client_entries_.clear(); event(std::move(message));
    } else if (message.type == wire::kind::request) {
      wire::read_query query;
      if (!wire::query(message.body, query) || !wire::within(query, client_generation_, client_entries_) || client_reads_.size() >= 4) {
        enqueue({wire::kind::reply, message.id, wire::reply(STG_E_REVERTED)}); return;
      }
      client_reads_[message.id] = {nullptr, query.count, clock::now() + std::chrono::seconds(15)}; event(std::move(message));
    } else if (message.type == wire::kind::reply) {
      auto found = reads_.find(message.id); if (found == reads_.end()) return;
      auto value = std::move(found->second); reads_.erase(found);
      if (message.body.size() < 4 || message.body.size() > value.count + 4) write(value.out, web::StatusCode::bad_gateway);
      else write(value.out, web::StatusCode::ok, message.body);
    }
  }
  void run() {
    DWORD active_session = MAXDWORD; HANDLE child = nullptr; std::thread reader;
    clock::time_point next_pair_check {}; std::string last_pair_uuid; bool pairing_valid = true;
    auto retire = [&] {
      std::shared_ptr<wire::pipe> old;
      { std::lock_guard<std::mutex> lock(mutex_); old = std::move(pipe_); ready_ = false;
        source_generation_ = 0; source_entries_.clear(); client_generation_ = 0; client_entries_.clear(); outgoing_.clear();
        fail_reads(); if (!uuid_.empty()) event({wire::kind::offer, 0, {}}); }
      if (old) old->stop(); if (reader.joinable()) reader.join();
      if (child) { if (WaitForSingleObject(child, 2000) == WAIT_TIMEOUT) TerminateProcess(child, 0); CloseHandle(child); child = nullptr; }
      active_session = MAXDWORD;
    };
    while (!stopped_) {
      bool wanted = false; DWORD target = WTSGetActiveConsoleSessionId(); std::shared_ptr<wire::pipe> current;
      std::string checked_uuid; std::function<bool(std::string_view)> paired;
      { std::lock_guard<std::mutex> lock(mutex_); checked_uuid = uuid_; paired = paired_; }
      // Pairing storage has its own lock. Never call it with the hub lock held.
      if (checked_uuid != last_pair_uuid || clock::now() >= next_pair_check) {
        pairing_valid = checked_uuid.empty() || (paired && paired(checked_uuid));
        last_pair_uuid = checked_uuid; next_pair_check = clock::now() + std::chrono::milliseconds(20);
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!uuid_.empty() && (clock::now() - last_seen_ > std::chrono::seconds(35) || !sessions_.count(session_) ||
            !config::input.clipboard_sync || (uuid_ == checked_uuid && !pairing_valid))) revoke();
        wanted = !uuid_.empty() && config::input.clipboard_sync && target != MAXDWORD && target != 0;
        current = pipe_; flush();
        for (auto it = reads_.begin(); it != reads_.end();) {
          if (clock::now() >= it->second.deadline) { write(it->second.out, web::StatusCode::gateway_timeout); it = reads_.erase(it); } else ++it;
        }
        for (auto it = client_reads_.begin(); it != client_reads_.end();) {
          if (clock::now() >= it->second.deadline) { enqueue({wire::kind::reply, it->first, wire::reply(STG_E_REVERTED)}); it = client_reads_.erase(it); } else ++it;
        }
      }
      if (current && (!wanted || target != active_session || !current->alive())) { retire(); current.reset(); }
      if (wanted && !current) {
        auto connection = launch_agent(target, child);
        if (connection) {
          active_session = target;
          { std::lock_guard<std::mutex> lock(mutex_); pipe_ = connection; ready_ = true; }
          reader = std::thread([this, connection] { wire::message message; while (connection->receive(message)) received(connection, std::move(message)); });
          BOOST_LOG(info) << "File clipboard: user-session helper connected";
        } else { retire(); std::this_thread::sleep_for(std::chrono::seconds(2)); }
      }
      for (;;) {
        wire::message next; std::shared_ptr<wire::pipe> connection;
        { std::lock_guard<std::mutex> lock(mutex_); if (outgoing_.empty() || !pipe_) break;
          next = std::move(outgoing_.front()); outgoing_.pop_front(); connection = pipe_; }
        if (!connection->send(next)) break;
      }
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait_for(lock, std::chrono::milliseconds(20), [this] { return stopped_ || !outgoing_.empty(); });
    }
    { std::lock_guard<std::mutex> lock(mutex_); revoke(); }
    retire();
  }
  std::mutex mutex_; std::condition_variable wake_; std::atomic<bool> stopped_ {false}; std::thread thread_;
  std::map<std::uint32_t, std::string> sessions_; std::string uuid_, key_; std::uint32_t session_ = 0;
  std::function<bool(std::string_view)> paired_;
  clock::time_point last_seen_ {}, event_deadline_ {}; response event_response_;
  std::shared_ptr<wire::pipe> pipe_; bool ready_ = false; std::uint64_t request_id_ = 0;
  std::uint64_t source_generation_ = 0, client_generation_ = 0;
  std::vector<streaming::file_clipboard::entry> source_entries_, client_entries_;
  std::deque<wire::message> events_, outgoing_;
  std::map<std::uint64_t, read_response> reads_, client_reads_;
};
hub &instance() { static hub value; return value; }
}
void session_started(std::uint32_t id, const std::string &uuid) { instance().started(id, uuid); }
void session_stopped(std::uint32_t id) { instance().ended(id); }
void stop() { instance().stop(); }
void register_routes(web::Server<nvhttp::SunshineHTTPS> &server, std::function<bool(std::string_view)> paired) {
  instance().authorize_with(paired);
  auto secured = [paired](auto handler) {
    return [paired, handler](response out, request in) {
      if (!paired(in->client_uuid)) { write(out, web::StatusCode::forbidden); return; }
      handler(std::move(out), std::move(in));
    };
  };
  server.resource["^/api/v1/clipboard/files/open$"]["POST"] = secured([](auto out, auto in) { instance().open(out, in); });
  server.resource["^/api/v1/clipboard/files/events$"]["GET"] = secured([](auto out, auto in) { instance().events(out, in); });
  server.resource["^/api/v1/clipboard/files/send$"]["POST"] = secured([](auto out, auto in) { instance().send(out, in, false); });
  server.resource["^/api/v1/clipboard/files/read$"]["POST"] = secured([](auto out, auto in) { instance().send(out, in, true); });
}
}
#else
namespace file_clipboard {
void session_started(std::uint32_t, const std::string &) {}
void session_stopped(std::uint32_t) {}
void register_routes(web::Server<nvhttp::SunshineHTTPS> &, std::function<bool(std::string_view)>) {}
void stop() {}
}
#endif
