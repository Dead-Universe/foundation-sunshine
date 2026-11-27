// Single producer/consumer queue in an anonymous shared mapping. No pipe I/O.
#pragma once
#include <Windows.h>

namespace platf::console_input_bridge {
  struct input_queue {
    static constexpr ULONG capacity = 128;
    volatile LONG produced = 0;
    volatile LONG consumed = 0;
    INPUT entries[capacity] {};

    bool push(const INPUT &input) {
      const auto p = static_cast<ULONG>(InterlockedCompareExchange(&produced, 0, 0));
      const auto c = static_cast<ULONG>(InterlockedCompareExchange(&consumed, 0, 0));
      if (p - c >= capacity) return false;
      entries[p % capacity] = input;
      InterlockedExchange(&produced, static_cast<LONG>(p + 1u));
      return true;
    }

    bool pop(INPUT &input) {
      const auto c = static_cast<ULONG>(InterlockedCompareExchange(&consumed, 0, 0));
      const auto p = static_cast<ULONG>(InterlockedCompareExchange(&produced, 0, 0));
      if (p == c) return false;
      input = entries[c % capacity];
      InterlockedExchange(&consumed, static_cast<LONG>(c + 1u));
      return true;
    }
  };
}
