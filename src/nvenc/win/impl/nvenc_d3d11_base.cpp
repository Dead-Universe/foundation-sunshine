/**
 * @file src/nvenc/win/impl/nvenc_d3d11_base.cpp
 * @brief Definitions for abstract Direct3D11 NVENC encoder.
 */
#include "nvenc_d3d11_base.h"
#include "nvenc_async_wait.h"
#include "src/logging.h"

#ifdef NVENC_NAMESPACE
namespace NVENC_NAMESPACE {
#else
namespace nvenc {
#endif
  nvenc_d3d11_base::nvenc_d3d11_base(NV_ENC_DEVICE_TYPE device_type, shared_dll dll):
      nvenc_base(device_type),
      dll(dll) {
    async_event_handle = CreateEvent(NULL, FALSE, FALSE, NULL);
  }

  nvenc_d3d11_base::~nvenc_d3d11_base() {
    if (async_event_handle) {
      CloseHandle(async_event_handle);
    }
  }

  bool nvenc_d3d11_base::init_library() {
    if (nvenc) return true;
    if (!dll) return false;

    if (auto create_instance = (decltype(NvEncodeAPICreateInstance) *) GetProcAddress(dll.get(), "NvEncodeAPICreateInstance")) {
      auto new_nvenc = std::make_unique<NV_ENCODE_API_FUNCTION_LIST>();
      new_nvenc->version = NV_ENCODE_API_FUNCTION_LIST_VER;
      if (nvenc_failed(create_instance(new_nvenc.get()))) {
        BOOST_LOG(error) << "NvEnc: NvEncodeAPICreateInstance() failed: " << last_nvenc_error_string;
      }
      else {
        nvenc = std::move(new_nvenc);
        return true;
      }
    }
    else {
      BOOST_LOG(error) << "NvEnc: No NvEncodeAPICreateInstance() in dynamic library";
    }

    return false;
  }

  bool nvenc_d3d11_base::wait_for_async_event(uint32_t timeout_ms) {
    const auto started = GetTickCount64();
    const auto result = ::nvenc::async_completion::wait(async_event_handle, timeout_ms);
    if (result.status == WAIT_OBJECT_0 && result.extended) {
      BOOST_LOG(warning) << "NvEnc: delayed completion recovered without encoder reset after "
                         << GetTickCount64() - started << " ms";
    }
    else if (result.status == WAIT_FAILED) {
      BOOST_LOG(error) << "NvEnc: completion wait failed, Windows error " << result.error;
    }
    return result.status == WAIT_OBJECT_0;
  }
}
