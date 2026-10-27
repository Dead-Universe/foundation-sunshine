/**
 * @brief Keep sealed-VDD streaming alive while Windows moves the console.
 * Input helpers use anonymous duplicated handles; no named listener or credentials.
 */
#pragma once
#include <Windows.h>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include "console_input_queue.h"
#include "../../../tools/sunshinesvc_state.h"

#ifndef PROC_THREAD_ATTRIBUTE_HANDLE_LIST
  #define PROC_THREAD_ATTRIBUTE_HANDLE_LIST 0x00020002
#endif

namespace platf::console_input_bridge {
  inline bool persistent_service_core = false;
  inline std::atomic<std::uintptr_t> service_event {0};
  inline std::atomic<bool> sealed_capture_ready {false};
  inline std::atomic<bool> streaming_active {false};

  inline bool system_token() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    alignas(TOKEN_USER) unsigned char buffer[256] {};
    DWORD bytes = 0;
    const bool result = GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &bytes) &&
                        IsWellKnownSid(reinterpret_cast<TOKEN_USER *>(buffer)->User.Sid, WinLocalSystemSid);
    CloseHandle(token);
    return result;
  }

  inline bool parse_handle(std::string_view text, std::uintptr_t &value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc {} && result.ptr == text.data() + text.size() && value != 0;
  }

  inline bool bind_service_event(DWORD parent_pid, HANDLE remote_event) {
    if (!system_token()) return false;
    auto parent = OpenProcess(PROCESS_DUP_HANDLE, FALSE, parent_pid);
    if (!parent) return false;
    HANDLE event = nullptr;
    const bool copied = DuplicateHandle(parent, remote_event, GetCurrentProcess(), &event, EVENT_MODIFY_STATE, FALSE, 0);
    CloseHandle(parent);
    if (copied) service_event.store(reinterpret_cast<std::uintptr_t>(event), std::memory_order_release);
    return copied;
  }

  struct helper_status {
    volatile LONG ready = 0;
    volatile LONG error = ERROR_SUCCESS;
    volatile LONG session = -1;
    input_queue inputs;
  };

  inline int run_helper(HANDLE remote_event, HANDLE remote_status, DWORD owner_pid) {
    if (!system_token() || owner_pid == GetCurrentProcessId()) return ERROR_ACCESS_DENIED;
    auto owner = OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, owner_pid);
    if (!owner) return GetLastError();
    HANDLE data_event = nullptr;
    HANDLE status_handle = nullptr;
    const bool copied = DuplicateHandle(owner, remote_event, GetCurrentProcess(), &data_event, SYNCHRONIZE, FALSE, 0) &&
                        DuplicateHandle(owner, remote_status, GetCurrentProcess(), &status_handle, FILE_MAP_READ | FILE_MAP_WRITE, FALSE, 0);
    if (!copied || WaitForSingleObject(data_event, 0) == WAIT_FAILED) {
      if (data_event) CloseHandle(data_event);
      if (status_handle) CloseHandle(status_handle);
      CloseHandle(owner);
      return ERROR_INVALID_HANDLE;
    }
    auto status = static_cast<helper_status *>(MapViewOfFile(status_handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(helper_status)));
    if (!status) { const auto error = GetLastError(); CloseHandle(data_event); CloseHandle(status_handle); CloseHandle(owner); return error; }
    DWORD session = MAXDWORD;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    HDESK desktop = nullptr;
    const auto sync_desktop = [&]() {
      auto next = OpenInputDesktop(0, FALSE, MAXIMUM_ALLOWED);
      if (!next) return false;
      if (!SetThreadDesktop(next)) { CloseDesktop(next); return false; }
      if (desktop) CloseDesktop(desktop);
      desktop = next;
      return true;
    };
    const bool ready = owner && sync_desktop();
    if (ready) SetThreadExecutionState(ES_DISPLAY_REQUIRED);
    InterlockedExchange(&status->session, static_cast<LONG>(session));
    InterlockedExchange(&status->error, ready ? ERROR_SUCCESS : static_cast<LONG>(GetLastError()));
    InterlockedExchange(&status->ready, ready ? 1 : -1);
    bool active = ready;
    HANDLE waits[] {owner, data_event};
    while (active && WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
      SetThreadExecutionState(ES_DISPLAY_REQUIRED);
      INPUT input {};
      while (status->inputs.pop(input)) {
        // Fail closed if the console changed after the parent selected this helper.
        if (WTSGetActiveConsoleSessionId() != session ||
            (input.type != INPUT_KEYBOARD && input.type != INPUT_MOUSE)) { active = false; break; }
        if (!sync_desktop()) { InterlockedExchange(&status->error, GetLastError()); continue; }
        if (WTSGetActiveConsoleSessionId() != session) { active = false; break; }
        if (input.type == INPUT_KEYBOARD) input.ki.dwExtraInfo = 0;
        else input.mi.dwExtraInfo = 0;
        InterlockedExchange(&status->error, SendInput(1, &input, sizeof(input)) == 1 ? ERROR_SUCCESS : static_cast<LONG>(GetLastError()));
      }
    }
    if (owner) CloseHandle(owner);
    UnmapViewOfFile(status);
    CloseHandle(data_event);
    CloseHandle(status_handle);
    // The input desktop is released by process exit; closing the assigned
    // desktop from this thread would be invalid.
    return 0;
  }

  class helper_process {
    HANDLE event_ = nullptr;
    HANDLE mapping_ = nullptr;
    HANDLE job_ = nullptr;
    HANDLE process_ = nullptr;
    helper_status *status_ = nullptr;
    DWORD session_ = MAXDWORD;
    std::mutex mutex_;

    void close() {
      if (job_) CloseHandle(job_);
      if (process_) CloseHandle(process_);
      if (status_) UnmapViewOfFile(status_);
      if (mapping_) CloseHandle(mapping_);
      if (event_) CloseHandle(event_);
      event_ = mapping_ = job_ = process_ = nullptr;
      status_ = nullptr;
      session_ = MAXDWORD;
    }

    bool open(DWORD session) {
      close();
      event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!event_) return false;
      mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(helper_status), nullptr);
      if (!mapping_) return false;
      status_ = static_cast<helper_status *>(MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(helper_status)));
      if (!status_) return false;
      *status_ = {};

      HANDLE current = nullptr;
      HANDLE token = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE, &current)) return false;
      const bool duplicated = DuplicateTokenEx(current, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &token);
      CloseHandle(current);
      if (!duplicated) return false;
      if (!SetTokenInformation(token, TokenSessionId, &session, sizeof(session))) { CloseHandle(token); return false; }

      job_ = CreateJobObjectW(nullptr, nullptr);
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
      limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { CloseHandle(token); return false; }
      wchar_t exe[32768] {};
      const auto length = GetModuleFileNameW(nullptr, exe, 32768);
      if (length == 0 || length >= 32768) { CloseHandle(token); return false; }
      auto helper_exe = std::wstring(exe);
      const auto separator = helper_exe.find_last_of(L"\\/");
      if (separator == std::wstring::npos) { CloseHandle(token); return false; }
      helper_exe.resize(separator + 1);
      helper_exe += L"tools\\sunshinesvc.exe";
      auto command = L"\"" + helper_exe + L"\" --console-input-agent " +
                     std::to_wstring(reinterpret_cast<std::uintptr_t>(event_)) + L" " +
                     std::to_wstring(reinterpret_cast<std::uintptr_t>(mapping_)) + L" " + std::to_wstring(GetCurrentProcessId());
      // Cross-session inheritance is unsupported. The child duplicates only
      // these two anonymous objects from its still-running SYSTEM parent.
      STARTUPINFOW startup {};
      startup.cb = sizeof(startup);
      startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
      PROCESS_INFORMATION child {};
      const bool launched = CreateProcessAsUserW(token, helper_exe.c_str(), command.data(), nullptr, nullptr, FALSE,
          CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_BREAKAWAY_FROM_JOB,
          nullptr, nullptr, &startup, &child);
      CloseHandle(token);
      if (!launched) return false;
      process_ = child.hProcess;
      bool assigned = AssignProcessToJobObject(job_, process_) != FALSE;
      if (!assigned || ResumeThread(child.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(process_, ERROR_PROCESS_ABORTED);
        CloseHandle(child.hThread);
        return false;
      }
      CloseHandle(child.hThread);
      const auto deadline = GetTickCount64() + 500;
      while (InterlockedCompareExchange(&status_->ready, 0, 0) == 0 && GetTickCount64() < deadline && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) Sleep(1);
      if (InterlockedCompareExchange(&status_->ready, 0, 0) != 1 || static_cast<DWORD>(status_->session) != session) return false;
      session_ = session;
      return true;
    }

    bool ensure(DWORD session) {
      if (session == MAXDWORD) return false;
      if (session_ == session && process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) return true;
      if (open(session)) return true;
      close();
      return false;
    }

  public:
    ~helper_process() { close(); }
    bool prepare() {
      std::lock_guard lock(mutex_);
      return ensure(WTSGetActiveConsoleSessionId()) && SetEvent(event_);
    }
    bool send(const INPUT &input) {
      std::lock_guard lock(mutex_);
      const auto target = WTSGetActiveConsoleSessionId();
      if (!ensure(target)) return false;
      if (WTSGetActiveConsoleSessionId() != target) return false;
      // Queue publication and event signalling cannot wait on the child's read.
      // A full queue fails immediately instead of blocking the input thread.
      return status_->inputs.push(input) && SetEvent(event_);
    }
  };

  inline helper_process &helper() { static helper_process instance; return instance; }

  inline bool needs_forwarding() {
    if (service_event.load(std::memory_order_acquire) == 0) return false;
    DWORD own_session = MAXDWORD;
    return ProcessIdToSessionId(GetCurrentProcessId(), &own_session) && WTSGetActiveConsoleSessionId() != own_session;
  }

  inline bool update_service_event() {
    const auto event = service_event.load(std::memory_order_acquire);
    if (!event) return false;
    const bool active = streaming_active.load(std::memory_order_acquire);
    const bool sealed = sealed_capture_ready.load(std::memory_order_acquire);
    const bool input_ready = active && sealed && system_token() && helper().prepare();
    if (sunshinesvc::should_retain_transport(active, sealed, input_ready)) return SetEvent(reinterpret_cast<HANDLE>(event));
    ResetEvent(reinterpret_cast<HANDLE>(event));
    return false;
  }

  inline bool enable_for_sealed_vdd() {
    sealed_capture_ready.store(true, std::memory_order_release);
    if (persistent_service_core) {
      update_service_event();
      return system_token() && helper().prepare();
    }
    return update_service_event();
  }

  inline void set_streaming_active(bool active) {
    streaming_active.store(active, std::memory_order_release);
    update_service_event();
  }

  inline void disable_for_standard_capture() {
    sealed_capture_ready.store(false, std::memory_order_release);
    update_service_event();
  }
}  // namespace platf::console_input_bridge
