#!/usr/bin/env python3
from pathlib import Path

path = Path("src/video.cpp")
text = path.read_text(encoding="utf-8")

old_decl = """    const auto probe_capture_override = capture_override_for_encoder_probe();
"""
new_decl = """    auto probe_capture_override = capture_override_for_encoder_probe();
"""
if old_decl not in text:
    raise SystemExit("expected probe_capture_override declaration not found")
text = text.replace(old_decl, new_decl, 1)

old_block = """      const auto capture_ready_displays = encoder_list.empty() ?
                                            std::vector<std::string> {} :
                                            platf::display_names(encoder_list.front()->platform_formats->dev_type);
      const bool exact_target_unavailable = target_requires_exact_resolution &&
                                            std::ranges::find(capture_ready_displays, configured_display_name) == capture_ready_displays.end();
      if (exact_target_unavailable) {
        last_encoder_probe_result = {
          probe_error_e::no_active_display,
          \"The requested display is not available to the encoder probe capture backend.\",
          \"Connect or enable the selected display, then try again.\"
        };
        BOOST_LOG(error) << \"Requested output [\"sv << configured_output_name
                         << \"] is unavailable to temporary capture backend [\"sv
                         << *probe_capture_override << \" ]\"sv;
        return -1;
      }
      probe_display_name = select_encoder_probe_display(configured_display_name, capture_ready_displays);
"""

# Upstream currently has no space before the closing bracket in the log string.
# Keep a second exact form so the patcher is strict without being whitespace-fragile.
old_block_actual = """      const auto capture_ready_displays = encoder_list.empty() ?
                                            std::vector<std::string> {} :
                                            platf::display_names(encoder_list.front()->platform_formats->dev_type);
      const bool exact_target_unavailable = target_requires_exact_resolution &&
                                            std::ranges::find(capture_ready_displays, configured_display_name) == capture_ready_displays.end();
      if (exact_target_unavailable) {
        last_encoder_probe_result = {
          probe_error_e::no_active_display,
          \"The requested display is not available to the encoder probe capture backend.\",
          \"Connect or enable the selected display, then try again.\"
        };
        BOOST_LOG(error) << \"Requested output [\"sv << configured_output_name
                         << \"] is unavailable to temporary capture backend [\"sv
                         << *probe_capture_override << \"]\"sv;
        return -1;
      }
      probe_display_name = select_encoder_probe_display(configured_display_name, capture_ready_displays);
"""

new_block = """      const auto capture_ready_displays = encoder_list.empty() ?
                                            std::vector<std::string> {} :
                                            platf::display_names(encoder_list.front()->platform_formats->dev_type);

      // Preserve the existing DDX-first cold-start behavior whenever DDX can
      // capture at least one output. Hyper-V GPU-PV can expose active displays
      // while Desktop Duplication is unavailable for every output
      // (DuplicateOutput(E_INVALIDARG)). In that narrow case only, stop forcing
      // the probe through DDX and let the configured VDD backend run. The
      // Windows VDD backend still keeps its own vdd -> ddx runtime fallback, so
      // hosts where direct ZakoVDD capture is unsupported do not lose the old
      // safety net.
      if (config::video.capture == \"vdd\" && capture_ready_displays.empty()) {
        BOOST_LOG(warning) << \"No DDX capture-ready display is available for encoder probing; \"
                           << \"retrying with configured VDD direct capture while preserving runtime DDX fallback\"sv;
        probe_capture_override.reset();
        probe_display_name = configured_display_name;
      }
      else {
        const bool exact_target_unavailable = target_requires_exact_resolution &&
                                              std::ranges::find(capture_ready_displays, configured_display_name) == capture_ready_displays.end();
        if (exact_target_unavailable) {
          last_encoder_probe_result = {
            probe_error_e::no_active_display,
            \"The requested display is not available to the encoder probe capture backend.\",
            \"Connect or enable the selected display, then try again.\"
          };
          BOOST_LOG(error) << \"Requested output [\"sv << configured_output_name
                           << \"] is unavailable to temporary capture backend [\"sv
                           << *probe_capture_override << \" ]\"sv;
          return -1;
        }
        probe_display_name = select_encoder_probe_display(configured_display_name, capture_ready_displays);
      }
"""

# Normalize the one intended upstream log token in the replacement after matching.
matched = old_block_actual if old_block_actual in text else old_block if old_block in text else None
if matched is None:
    raise SystemExit("expected encoder probe display-selection block not found")
text = text.replace(matched, new_block, 1)
text = text.replace('<< *probe_capture_override << " ]"sv;', '<< *probe_capture_override << "]"sv;', 1)

path.write_text(text, encoding="utf-8")
print("Applied conservative VDD encoder-probe recovery patch to src/video.cpp")
