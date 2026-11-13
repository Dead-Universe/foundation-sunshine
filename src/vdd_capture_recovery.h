#pragma once

#include <string>

namespace video {

  // Used by serialized launch preparation, before an active capture session.
  // Persist an Auto -> VDD decision only after display and encoder validation.
  class vdd_capture_recovery_t {
  public:
    explicit vdd_capture_recovery_t(std::string &capture, bool may_change = true):
        capture_ {capture}, original_ {capture},
        changed_ {may_change && (capture.empty() || capture == "auto")} {
      if (changed_) {
        capture_ = "vdd";
      }
    }

    ~vdd_capture_recovery_t() {
      if (changed_ && !committed_) {
        capture_.swap(original_);
      }
    }

    vdd_capture_recovery_t(const vdd_capture_recovery_t &) = delete;
    vdd_capture_recovery_t &operator=(const vdd_capture_recovery_t &) = delete;

    void commit() noexcept {
      committed_ = true;
    }

  private:
    std::string &capture_;
    std::string original_;
    bool changed_;
    bool committed_ = false;
  };

}  // namespace video
