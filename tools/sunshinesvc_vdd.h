// Hyper-V service-session selection and transient console-local VDD topology.
#pragma once
#include <Windows.h>
#include <SetupAPI.h>
#include <cwchar>
#include <string>
#include <vector>
#include "../src/display_device/vdd_control_ioctl.h"

namespace sunshinesvc::vdd {
  inline bool registry_string(const wchar_t *name,const wchar_t *expected) {
    wchar_t value[256] {};DWORD bytes=sizeof(value);
    return RegGetValueW(HKEY_LOCAL_MACHINE,L"HARDWARE\\DESCRIPTION\\System\\BIOS",name,
        RRF_RT_REG_SZ,nullptr,value,&bytes)==ERROR_SUCCESS && _wcsicmp(value,expected)==0;
  }

  inline bool persistent_core_supported() {
    if(!registry_string(L"SystemManufacturer",L"Microsoft Corporation") ||
       !registry_string(L"SystemProductName",L"Virtual Machine")) return false;
    constexpr GUID control=ZAKO_VDD_CONTROL_GUID_INIT;
    auto devices=SetupDiGetClassDevsW(&control,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(devices==INVALID_HANDLE_VALUE)return false;
    SP_DEVICE_INTERFACE_DATA iface {};iface.cbSize=sizeof(iface);
    bool supported=false;
    if(SetupDiEnumDeviceInterfaces(devices,nullptr,&control,0,&iface)) {
      DWORD required=0;SetupDiGetDeviceInterfaceDetailW(devices,&iface,nullptr,0,&required,nullptr);
      if(required>=sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)&&required<=65536) {
        std::vector<BYTE> buffer(required);auto detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
        detail->cbSize=sizeof(*detail);
        if(SetupDiGetDeviceInterfaceDetailW(devices,&iface,detail,required,nullptr,nullptr)) {
          auto device=CreateFileW(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
          if(device!=INVALID_HANDLE_VALUE) {
            VDD_FRAME_CHANNEL_CAPS caps {};caps.Size=sizeof(caps);DWORD bytes=0;
            supported=DeviceIoControl(device,IOCTL_VDD_QUERY_FRAME_CHANNEL_CAPS,nullptr,0,&caps,sizeof(caps),&bytes,nullptr) &&
                bytes>=sizeof(caps) && caps.Size>=sizeof(caps) && caps.Version==VDD_FRAME_CHANNEL_CAPS_VERSION &&
                (caps.Flags&7u)==7u && caps.MaxSharedSlots>0 && caps.MaxSharedSlots<=VDD_FRAME_CHANNEL_MAX_SLOTS && caps.MetadataSize>=128;
            CloseHandle(device);
          }
        }
      }
    }
    SetupDiDestroyDeviceInfoList(devices);return supported;
  }

  inline DWORD configure_console_display() {
    DWORD own=MAXDWORD;ProcessIdToSessionId(GetCurrentProcessId(),&own);
    if(own==0 || own!=WTSGetActiveConsoleSessionId())return ERROR_RETRY;
    auto desktop=OpenInputDesktop(0,FALSE,MAXIMUM_ALLOWED);
    if(!desktop)return GetLastError();
    if(!SetThreadDesktop(desktop)){auto error=GetLastError();CloseDesktop(desktop);return error;}
    // This bounded helper exits immediately; the assigned desktop closes then.
    for(unsigned attempt=0;attempt<3;++attempt) {
      UINT32 path_count=0,mode_count=0;
      auto result=GetDisplayConfigBufferSizes(QDC_ALL_PATHS,&path_count,&mode_count);
      if(result!=ERROR_SUCCESS)return result;
      std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
      result=QueryDisplayConfig(QDC_ALL_PATHS,&path_count,paths.data(),&mode_count,modes.data(),nullptr);
      if(result==ERROR_INSUFFICIENT_BUFFER)continue;
      if(result!=ERROR_SUCCESS)return result;
      DISPLAYCONFIG_PATH_INFO selected {};bool found=false;unsigned active=0;
      for(UINT32 i=0;i<path_count;++i) {
        const auto &path=paths[i];if(path.flags&DISPLAYCONFIG_PATH_ACTIVE)++active;
        DISPLAYCONFIG_TARGET_DEVICE_NAME name {};name.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        name.header.size=sizeof(name);name.header.adapterId=path.targetInfo.adapterId;name.header.id=path.targetInfo.id;
        if(!path.targetInfo.targetAvailable || DisplayConfigGetDeviceInfo(&name.header)!=ERROR_SUCCESS ||
           _wcsicmp(name.monitorFriendlyDeviceName,L"Zako HDR")!=0)continue;
        if(found && (selected.targetInfo.id!=path.targetInfo.id ||
           selected.targetInfo.adapterId.LowPart!=path.targetInfo.adapterId.LowPart ||
           selected.targetInfo.adapterId.HighPart!=path.targetInfo.adapterId.HighPart))return ERROR_DUP_NAME;
        if(!found || (path.flags&DISPLAYCONFIG_PATH_ACTIVE))selected=path;
        found=true;
      }
      if(!found)return ERROR_NOT_FOUND;
      if(active==1 && (selected.flags&DISPLAYCONFIG_PATH_ACTIVE))return ERROR_SUCCESS;
      if(own!=WTSGetActiveConsoleSessionId())return ERROR_RETRY;
      selected.flags=DISPLAYCONFIG_PATH_ACTIVE;
      selected.sourceInfo.modeInfoIdx=DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
      selected.targetInfo.modeInfoIdx=DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
      // Apply only to this live console. Do not save a permanent display layout.
      return SetDisplayConfig(1,&selected,0,nullptr,SDC_APPLY|SDC_USE_SUPPLIED_DISPLAY_CONFIG|SDC_ALLOW_CHANGES);
    }
    return ERROR_INSUFFICIENT_BUFFER;
  }
}
