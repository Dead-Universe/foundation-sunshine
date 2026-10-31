#pragma once
#include "file_clipboard_wire.h"
#include <chrono>

namespace streaming::file_clipboard::wire {
class pipe {
public:
  explicit pipe(HANDLE handle, bool server = false): handle_(handle), server_(server) {}
  ~pipe() { stop(); if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
  bool alive() const { return alive_.load(); }
  void stop() {
    if (alive_.exchange(false)) {
      CancelIoEx(handle_, nullptr);
      if (server_) DisconnectNamedPipe(handle_);
    }
  }
  bool send(const message &value) {
    auto data = encode(value);
    std::lock_guard<std::mutex> lock(write_mutex_);
    return !data.empty() && transfer(data.data(), static_cast<DWORD>(data.size()), true);
  }
  bool receive(message &value) {
    bytes data(16);
    if (!transfer(data.data(), 16, false)) return false;
    decoder header(data); std::uint64_t type, id, length;
    if (!header.number(type, 4) || !header.number(id, 8) || !header.number(length, 4) || length > max_manifest_bytes) { stop(); return false; }
    data.resize(16 + static_cast<std::size_t>(length));
    if (length && !transfer(data.data() + 16, static_cast<DWORD>(length), false)) return false;
    if (!decode(data, value)) { stop(); return false; }
    return true;
  }
private:
  bool transfer(void *buffer, DWORD bytes, bool writing) {
    auto *position = static_cast<unsigned char *>(buffer);
    while (bytes && alive_) {
      OVERLAPPED operation {}; operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!operation.hEvent) { stop(); return false; }
      DWORD count = 0;
      BOOL complete = writing ? WriteFile(handle_, position, bytes, &count, &operation) : ReadFile(handle_, position, bytes, &count, &operation);
      DWORD error = complete ? ERROR_SUCCESS : GetLastError();
      if (!complete && error == ERROR_IO_PENDING) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (alive_) {
          auto wait = WaitForSingleObject(operation.hEvent, 250);
          if (wait == WAIT_OBJECT_0) break;
          if (wait != WAIT_TIMEOUT || (writing && std::chrono::steady_clock::now() >= deadline)) { stop(); break; }
        }
        if (!alive_) CancelIoEx(handle_, &operation);
        complete = GetOverlappedResult(handle_, &operation, &count, TRUE);
      }
      CloseHandle(operation.hEvent);
      if (!complete || !count) { stop(); return false; }
      bytes -= count; position += count;
    }
    return bytes == 0 && alive_;
  }
  HANDLE handle_; bool server_; std::atomic<bool> alive_ {true}; std::mutex write_mutex_;
};
}
