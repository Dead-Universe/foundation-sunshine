#pragma once

#include <optional>
#include <string>
#include <utility>

namespace display_device {

  // Empty means Auto and is a valid saved selection, not an absent snapshot.
  class vdd_output_state_t {
  public:
    bool remember(const std::string &output_name) {
      if (original_) {
        return false;
      }
      original_ = output_name;
      return true;
    }

    bool restore(std::string &output_name) {
      if (!original_) {
        return false;
      }
      output_name = std::move(*original_);
      original_.reset();
      return true;
    }

  private:
    std::optional<std::string> original_;
  };

}  // namespace display_device
