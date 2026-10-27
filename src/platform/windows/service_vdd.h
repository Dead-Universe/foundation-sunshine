// A sealed VDD Core can live in service session 0; only CCD runs in the console.
#pragma once
#include "console_input_bridge.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/display_device/parsed_config.h"
#include "src/display_device/vdd_ioctl.h"
#include "src/display_device/vdd_utils.h"
#include <atomic>
#include <thread>

namespace platf::service_vdd {
  inline std::atomic<bool> single_display {false};
  inline std::atomic<UINT64> capture_epoch {1};
  inline bool enabled() { return console_input_bridge::persistent_service_core; }

  inline DWORD prepare_console(DWORD session) {
    HANDLE current=nullptr,token=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_DUPLICATE,&current))return GetLastError();
    const bool copied=DuplicateTokenEx(current,TOKEN_ALL_ACCESS,nullptr,SecurityImpersonation,TokenPrimary,&token);
    CloseHandle(current);if(!copied)return GetLastError();
    if(!SetTokenInformation(token,TokenSessionId,&session,sizeof(session))){auto error=GetLastError();CloseHandle(token);return error;}
    wchar_t executable[32768] {};const auto length=GetModuleFileNameW(nullptr,executable,32768);
    if(!length||length>=32768){CloseHandle(token);return ERROR_INVALID_NAME;}
    std::wstring helper(executable);const auto separator=helper.find_last_of(L"\\/");
    if(separator==std::wstring::npos){CloseHandle(token);return ERROR_INVALID_NAME;}
    helper.resize(separator+1);helper+=L"tools\\sunshinesvc.exe";
    auto command=L"\""+helper+L"\" --console-vdd-display";
    auto job=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job||!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){if(job)CloseHandle(job);CloseHandle(token);return ERROR_INVALID_HANDLE;}
    STARTUPINFOW startup {};startup.cb=sizeof(startup);startup.lpDesktop=const_cast<LPWSTR>(L"winsta0\\default");
    PROCESS_INFORMATION child {};
    const bool started=CreateProcessAsUserW(token,helper.c_str(),command.data(),nullptr,nullptr,FALSE,
        CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|CREATE_SUSPENDED|CREATE_BREAKAWAY_FROM_JOB,nullptr,nullptr,&startup,&child);
    CloseHandle(token);if(!started){auto error=GetLastError();CloseHandle(job);return error;}
    DWORD result=ERROR_PROCESS_ABORTED;
    if(AssignProcessToJobObject(job,child.hProcess)&&ResumeThread(child.hThread)!=static_cast<DWORD>(-1)) {
      if(WaitForSingleObject(child.hProcess,3000)==WAIT_OBJECT_0)GetExitCodeProcess(child.hProcess,&result);
      else result=ERROR_TIMEOUT;
    }else{TerminateProcess(child.hProcess,ERROR_PROCESS_ABORTED);}
    CloseHandle(job);CloseHandle(child.hThread);CloseHandle(child.hProcess);return result;
  }

  inline void start_display_follower() {
    static std::jthread follower([](std::stop_token stop) {
      DWORD prepared=MAXDWORD;ULONGLONG retry=0;
      DWORD observed=MAXDWORD,last_result=ERROR_GEN_FAILURE;
      while(!stop.stop_requested()) {
        if(enabled()&&console_input_bridge::streaming_active.load()) {
          const auto session=WTSGetActiveConsoleSessionId();
          if(session!=MAXDWORD && (session!=prepared || GetTickCount64()>=retry)) {
            const auto result=single_display.load()?prepare_console(session):ERROR_SUCCESS;
            if(observed!=session || last_result!=result) {
              BOOST_LOG(result==ERROR_SUCCESS?info:warning) << "[service-vdd] Console display preparation: session="
                  << session << " result=" << result;
              observed=session;last_result=result;
            }
            if(result==ERROR_SUCCESS && prepared!=session)capture_epoch.fetch_add(1,std::memory_order_acq_rel);
            prepared=result==ERROR_SUCCESS?session:MAXDWORD;
            retry=GetTickCount64()+(result==ERROR_SUCCESS?30000:2000);
          }
        }else{prepared=MAXDWORD;retry=0;}
        Sleep(250);
      }
    });
  }

  inline bool prepare_monitor(display_device::parsed_config_t config,int width,int height,int fps,
      const display_device::vdd_utils::hdr_brightness_t &brightness={},
      const display_device::vdd_utils::physical_size_t &physical={}) {
    if(!enabled()||width<=0||height<=0||width>16384||height>16384||fps<=0||fps>1000)return false;
    if(!config.resolution)config.resolution=display_device::resolution_t{static_cast<unsigned>(width),static_cast<unsigned>(height)};
    if(!config.refresh_rate)config.refresh_rate=display_device::refresh_rate_t{static_cast<unsigned>(fps),1};
    if(!config.refresh_rate->denominator || !config.resolution->width || !config.resolution->height ||
        config.resolution->width>16384 || config.resolution->height>16384)return false;
    const auto hz=(config.refresh_rate->numerator+config.refresh_rate->denominator/2)/config.refresh_rate->denominator;
    if(!hz||hz>1000)return false;
    // One advertised mode makes the new IddCx source and target dimensions agree
    // without querying a DXGI output belonging to another Windows session.
    const display_device::vdd_utils::VddSettings modes{{*config.resolution},{hz}};
    // Warm the actual console desktop before waiting for the IddCx producer.
    // A service-session execution-state request alone cannot replace this.
    if(!console_input_bridge::helper().prepare())return false;
    if(display_device::vdd_utils::set_vdd_session_mode(config,modes)!=display_device::vdd_utils::set_vdd_result::ok)return false;
    if(display_device::vdd_ioctl::send_command(L"DESTROYMONITOR")!=display_device::vdd_ioctl::result::success)return false;
    Sleep(500);
    if(!display_device::vdd_utils::create_vdd_monitor("persistent_service_vdd",brightness,physical))return false;
    capture_epoch.fetch_add(1,std::memory_order_acq_rel);
    single_display.store(config.device_prep==display_device::parsed_config_t::device_prep_e::ensure_only_display ||
        config.vdd_prep==display_device::parsed_config_t::vdd_prep_e::display_off);
    Sleep(200);return true;
  }
}
