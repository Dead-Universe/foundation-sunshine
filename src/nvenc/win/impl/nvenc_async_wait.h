#pragma once

#include <windows.h>

namespace nvenc::async_completion {
  struct result_t {
    DWORD status;
    DWORD error;
    bool extended;
  };

  // A delayed completion still owns the submitted picture and bitstream.
  // Give it one bounded grace interval before destroying the encoder. Normal
  // signaled events return immediately; invalid handles must never be retried.
  inline result_t wait(HANDLE event, DWORD initial_ms) {
    DWORD status = WaitForSingleObject(event, initial_ms);
    if (status != WAIT_TIMEOUT) {
      return {status, status == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS, false};
    }
    status = WaitForSingleObject(event, 400);
    return {status, status == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS, true};
  }
}
