#!/usr/bin/env python3
from pathlib import Path

video_path = Path("src/video.cpp")
text = video_path.read_text(encoding="utf-8")

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
    // If the earlier probe target was empty, re-resolve the configured ZakoHDR
    // sentinel at the moment DDX has failed. This catches a VDD that appeared
    // during display preparation without turning cold-start into repeated VDD
    // attempts when Windows still has no concrete Zako display to target.
    reset_display(disp, encoder.platform_formats->dev_type, probe_display_name, config_autoselect);
    auto vdd_retry_display_name = probe_display_name;
    if (!disp &&
        configured_capture_backend == "vdd" &&
        vdd_retry_display_name.empty()) {
      vdd_retry_display_name = display_device::get_display_name(config::video.output_name);
      if (!vdd_retry_display_name.empty()) {
        BOOST_LOG(info) << "Resolved live VDD probe target after DDX failure: ["sv
                        << vdd_retry_display_name << ']';
      }
    }
    if (!disp &&
        configured_capture_backend == "vdd" &&
        !vdd_retry_display_name.empty() &&
        effective_probe_capture_override &&
        *effective_probe_capture_override == "ddx") {
      BOOST_LOG(warning) << "DDX encoder probe could not create capture display ["sv
                         << vdd_retry_display_name
                         << "]; retrying the same encoder with configured VDD direct capture "
                         << "while preserving the VDD backend's runtime DDX fallback"sv;

      effective_probe_capture_override.reset();
      config_max_ref_frames.capture_backend_override.clear();
      config_autoselect.capture_backend_override.clear();
      reset_display(disp, encoder.platform_formats->dev_type, vdd_retry_display_name, config_autoselect);
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

video_path.write_text(text, encoding="utf-8")

# The VDD retry above only helps if display_base_t::init() can actually select
# the ZakoVDD output. On GPU-PV, requiring test_dxgi_duplication() for that
# output recreates the same DDX gate we are trying to recover from. Respect the
# per-display probe override here: the first DDX attempt still performs the DDX
# test, while the retry (override cleared, effective backend = vdd) may accept
# only the confirmed ZakoVDD output without Desktop Duplication validation.
display_path = Path("src/platform/windows/display_base.cpp")
display_text = display_path.read_text(encoding="utf-8")

old = '''    auto adapter_name = from_utf8(config::video.adapter_name);
    const bool is_rdp_session = !is_running_as_system_user && display_device::w_utils::is_any_rdp_session_active();
    auto output_name = is_rdp_session ? std::wstring {} : from_utf8(display_name);

    if (is_rdp_session) {
'''
new = '''    auto adapter_name = from_utf8(config::video.adapter_name);
    const bool is_rdp_session = !is_running_as_system_user && display_device::w_utils::is_any_rdp_session_active();
    auto output_name = is_rdp_session ? std::wstring {} : from_utf8(display_name);
    const auto &effective_capture_backend =
      config.capture_backend_override.empty() ? config::video.capture : config.capture_backend_override;
    const bool direct_vdd_capture = effective_capture_backend == "vdd";
    std::wstring vdd_output_name;
    if (direct_vdd_capture) {
      const auto vdd_device_id = display_device::find_device_by_friendlyname(ZAKO_NAME);
      if (!vdd_device_id.empty()) {
        vdd_output_name = from_utf8(display_device::get_display_name(vdd_device_id));
      }
    }

    if (is_rdp_session) {
'''
if old not in display_text:
    raise SystemExit("expected display_base_t::init capture selection prologue not found")
display_text = display_text.replace(old, new, 1)

old = '''          if (!is_rdp_session && !output_name.empty() && desc.DeviceName != output_name) {
            continue;
          }

          const bool output_accepted = is_rdp_session ||
                                       (desc.AttachedToDesktop && test_dxgi_duplication(adapter_tmp, output_tmp, false));

          if (output_accepted) {
            BOOST_LOG(is_rdp_session ? info : debug) << "[Display Init] Selected display: " << to_utf8(desc.DeviceName);
'''
new = '''          if (!is_rdp_session && !output_name.empty() && desc.DeviceName != output_name) {
            continue;
          }
          if (direct_vdd_capture &&
              output_name.empty() &&
              !vdd_output_name.empty() &&
              desc.DeviceName != vdd_output_name) {
            continue;
          }

          const bool is_selected_vdd_output =
            direct_vdd_capture &&
            !vdd_output_name.empty() &&
            desc.DeviceName == vdd_output_name;
          const bool output_accepted = is_rdp_session ||
                                       (desc.AttachedToDesktop &&
                                        (is_selected_vdd_output ||
                                         test_dxgi_duplication(adapter_tmp, output_tmp, false)));

          if (output_accepted) {
            if (is_selected_vdd_output) {
              BOOST_LOG(info) << "[vdd] Selected ZakoVDD output without requiring DXGI Desktop Duplication: "
                              << to_utf8(desc.DeviceName);
            }
            BOOST_LOG(is_rdp_session ? info : debug) << "[Display Init] Selected display: " << to_utf8(desc.DeviceName);
'''
if old not in display_text:
    raise SystemExit("expected display_base_t::init DXGI output acceptance block not found")
display_text = display_text.replace(old, new, 1)

display_path.write_text(display_text, encoding="utf-8")

# Preserve the real Win32 error immediately after CreateFileW. The previous
# diagnostic streamed GetLastError() through BOOST_LOG, and logging itself can
# call Win32 APIs before the value is formatted, which produced misleading
# "err=0" reports even though CreateFileW returned INVALID_HANDLE_VALUE.
ioctl_path = Path("src/display_device/vdd_ioctl.cpp")
ioctl_text = ioctl_path.read_text(encoding="utf-8")
old = '''        if (m_handle == INVALID_HANDLE_VALUE) {
          // Interface was enumerated (path resolved) but the kernel still
          // refused to give us a handle. Propagate the failure.
          BOOST_LOG(warning) << "vdd_ioctl: CreateFileW failed (err=" << GetLastError() << ")";
          return open_result::failed;
        }
'''
new = '''        if (m_handle == INVALID_HANDLE_VALUE) {
          // Interface was enumerated (path resolved) but the kernel still
          // refused to give us a handle. Capture the thread last-error value
          // before logging can disturb it.
          const DWORD create_error = GetLastError();
          BOOST_LOG(warning) << "vdd_ioctl: CreateFileW failed (err=" << create_error << ")";
          return open_result::failed;
        }
'''
if old not in ioctl_text:
    raise SystemExit("expected vdd_ioctl CreateFileW failure block not found")
ioctl_text = ioctl_text.replace(old, new, 1)
ioctl_path.write_text(ioctl_text, encoding="utf-8")

print("Applied DDX-first VDD recovery, live Zako target re-resolution, ZakoVDD-only DDX-test bypass, and stable IOCTL error logging")
