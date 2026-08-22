#!/usr/bin/env python3
from pathlib import Path

path = Path("src/video.cpp")
text = path.read_text(encoding="utf-8")

old = '''    const auto configured_capture_backend = config::video.capture;

    BOOST_LOG(info) << "Trying encoder ["sv << encoder.name << ']';
'''
new = '''    const auto configured_capture_backend = config::video.capture;
    auto effective_probe_capture_override = probe_capture_override;

    BOOST_LOG(info) << "Trying encoder ["sv << encoder.name << ']';
'''
if old not in text:
    raise SystemExit("expected validate_encoder prologue not found")
text = text.replace(old, new, 1)

old = '''    if (probe_capture_override) {
      BOOST_LOG(info) << "Temporarily using capture backend ["sv << *probe_capture_override
                      << "] for encoder probe while configured capture backend is ["sv
                      << configured_capture_backend << "]"sv;
    }
'''
new = '''    if (effective_probe_capture_override) {
      BOOST_LOG(info) << "Temporarily using capture backend ["sv << *effective_probe_capture_override
                      << "] for encoder probe while configured capture backend is ["sv
                      << configured_capture_backend << "]"sv;
    }
'''
if old not in text:
    raise SystemExit("expected validate_encoder probe logging block not found")
text = text.replace(old, new, 1)

old = '''    if (probe_capture_override) {
      config_max_ref_frames.capture_backend_override = *probe_capture_override;
      config_autoselect.capture_backend_override = *probe_capture_override;
    }

    // If the encoder isn't supported at all (not even H.264), bail early
    reset_display(disp, encoder.platform_formats->dev_type, probe_display_name, config_autoselect);
    if (!disp) {
      return false;
    }
'''
new = '''    if (effective_probe_capture_override) {
      config_max_ref_frames.capture_backend_override = *effective_probe_capture_override;
      config_autoselect.capture_backend_override = *effective_probe_capture_override;
    }

    // Keep upstream's DDX-first probe behavior. Hyper-V GPU-PV can enumerate
    // a DXGI output but still fail when reset_display() actually opens it.
    // Retry with VDD only after the configured target resolves to a real OS
    // display name. During cold start the Zako VDD is not present yet, so the
    // probe target is empty and we intentionally do not attempt VDD here.
    reset_display(disp, encoder.platform_formats->dev_type, probe_display_name, config_autoselect);
    if (!disp &&
        configured_capture_backend == "vdd" &&
        !probe_display_name.empty() &&
        effective_probe_capture_override &&
        *effective_probe_capture_override == "ddx") {
      BOOST_LOG(warning) << "DDX encoder probe could not create capture display ["sv
                         << probe_display_name
                         << "]; retrying the same encoder with configured VDD direct capture "
                         << "while preserving the VDD backend's runtime DDX fallback"sv;

      effective_probe_capture_override.reset();
      config_max_ref_frames.capture_backend_override.clear();
      config_autoselect.capture_backend_override.clear();
      reset_display(disp, encoder.platform_formats->dev_type, probe_display_name, config_autoselect);
    }
    if (!disp) {
      return false;
    }
'''
if old not in text:
    raise SystemExit("expected validate_encoder initial display probe block not found")
text = text.replace(old, new, 1)

old = '''        if (probe_capture_override) {
          generic_hdr_config.capture_backend_override = *probe_capture_override;
        }
'''
new = '''        if (effective_probe_capture_override) {
          generic_hdr_config.capture_backend_override = *effective_probe_capture_override;
        }
'''
if old not in text:
    raise SystemExit("expected validate_encoder HDR override block not found")
text = text.replace(old, new, 1)

path.write_text(text, encoding="utf-8")
print("Applied DDX-first / active-target VDD-on-open-failure encoder-probe recovery patch to src/video.cpp")
