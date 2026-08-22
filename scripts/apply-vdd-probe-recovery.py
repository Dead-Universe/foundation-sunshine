#!/usr/bin/env python3
from pathlib import Path

path = Path("src/video.cpp")
text = path.read_text(encoding="utf-8")

# Keep the upstream DDX-first probe selection intact.  The recovery belongs
# inside validate_encoder(), where we can distinguish "DDX enumerated an
# output" from "DDX actually managed to create a capture display".
old = """    const auto configured_capture_backend = config::video.capture;

    BOOST_LOG(info) << \"Trying encoder [\"sv << encoder.name << ']';
"""
new = """    const auto configured_capture_backend = config::video.capture;
    auto effective_probe_capture_override = probe_capture_override;

    BOOST_LOG(info) << \"Trying encoder [\"sv << encoder.name << ']';
"""
if old not in text:
    raise SystemExit("expected validate_encoder prologue not found")
text = text.replace(old, new, 1)

old = """    if (probe_capture_override) {
      BOOST_LOG(info) << \"Temporarily using capture backend [\"sv << *probe_capture_override
                      << \"] for encoder probe while configured capture backend is [\"sv
                      << configured_capture_backend << \"]\"sv;
    }
"""
new = """    if (effective_probe_capture_override) {
      BOOST_LOG(info) << \"Temporarily using capture backend [\"sv << *effective_probe_capture_override
                      << \"] for encoder probe while configured capture backend is [\"sv
                      << configured_capture_backend << \"]\"sv;
    }
"""
if old not in text:
    raise SystemExit("expected validate_encoder probe logging block not found")
text = text.replace(old, new, 1)

old = """    if (probe_capture_override) {
      config_max_ref_frames.capture_backend_override = *probe_capture_override;
      config_autoselect.capture_backend_override = *probe_capture_override;
    }

    // If the encoder isn't supported at all (not even H.264), bail early
    reset_display(disp, encoder.platform_formats->dev_type, probe_display_name, config_autoselect);
    if (!disp) {
      return false;
    }
"""
new = """    if (effective_probe_capture_override) {
      config_max_ref_frames.capture_backend_override = *effective_probe_capture_override;
      config_autoselect.capture_backend_override = *effective_probe_capture_override;
    }

    // If the encoder isn't supported at all (not even H.264), bail early.
    // Keep upstream's DDX-first cold-start behavior.  However, Hyper-V GPU-PV
    // can enumerate a DXGI output successfully and still fail DuplicateOutput()
    // when reset_display() actually opens it.  In that narrow case, and only
    // when the configured runtime backend is VDD, retry this same encoder once
    // with the real VDD backend.  If DDX succeeds, nothing changes.  If VDD
    // direct capture is unsupported, the Windows VDD backend retains its own
    // VDD -> DDX fallback, so the existing safety net is not removed.
    reset_display(disp, encoder.platform_formats->dev_type, probe_display_name, config_autoselect);
    if (!disp &&
        configured_capture_backend == \"vdd\" &&
        effective_probe_capture_override &&
        *effective_probe_capture_override == \"ddx\") {
      BOOST_LOG(warning) << \"DDX encoder probe could not create a capture display; \"
                         << \"retrying the same encoder with configured VDD direct capture \"
                         << \"while preserving the VDD backend's runtime DDX fallback\"sv;

      effective_probe_capture_override.reset();
      config_max_ref_frames.capture_backend_override.clear();
      config_autoselect.capture_backend_override.clear();
      reset_display(disp, encoder.platform_formats->dev_type, probe_display_name, config_autoselect);
    }
    if (!disp) {
      return false;
    }
"""
if old not in text:
    raise SystemExit("expected validate_encoder initial display probe block not found")
text = text.replace(old, new, 1)

old = """        if (probe_capture_override) {
          generic_hdr_config.capture_backend_override = *probe_capture_override;
        }
"""
new = """        if (effective_probe_capture_override) {
          generic_hdr_config.capture_backend_override = *effective_probe_capture_override;
        }
"""
if old not in text:
    raise SystemExit("expected validate_encoder HDR override block not found")
text = text.replace(old, new, 1)

path.write_text(text, encoding="utf-8")
print("Applied DDX-first / VDD-on-open-failure encoder-probe recovery patch to src/video.cpp")
