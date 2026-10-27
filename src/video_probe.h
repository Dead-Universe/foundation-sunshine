/**
 * @file src/video_probe.h
 * @brief Platform-independent selection policy for encoder probe displays.
 */
#pragma once

#include <algorithm>
#include <span>
#include <string>

namespace video {

  inline std::string
  select_encoder_probe_display(
    const std::string &configured_display,
    std::span<const std::string> capture_ready_displays) {
    if (std::ranges::find(capture_ready_displays, configured_display) != capture_ready_displays.end()) {
      return configured_display;
    }

    // An empty display preserves backend auto-selection, including adapter
    // filtering, display wake-up retries, and scanning subsequent outputs.
    return {};
  }

  inline bool
  should_try_vdd_auto_fallback(
    bool running_as_system,
    bool output_is_explicit,
    bool ddx_capture_ready,
    bool standard_probe_finished,
    bool standard_probe_succeeded) {
    if (output_is_explicit) {
      return false;
    }

    if (running_as_system) {
      // WGC is unavailable in service mode. If no output passed the DDX
      // preflight, VDD is the only remaining Windows capture path.
      return !ddx_capture_ready;
    }

    // In an interactive session, preserve the existing DDX -> WGC order first.
    // Only try VDD after that complete standard probe has actually failed.
    return standard_probe_finished && !standard_probe_succeeded;
  }

}  // namespace video
