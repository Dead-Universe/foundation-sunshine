// Read the VDD producer identity and test named-object reopen permissions.
// All objects created here have unique diagnostic names and are closed on exit.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>
#include <sddl.h>
#include <aclapi.h>
#include <cstdio>
#include <string>
#include <vector>
#ifdef TEST_CURSOR_PRODUCER_REPAIR
#include "src/platform/windows/vdd_cursor_security.h"
#endif

static unsigned int failures = 0;
static unsigned int cases = 0;

static std::string token_sid(HANDLE token) {
  DWORD bytes = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
  std::vector<BYTE> data(bytes);
  if (!bytes || !GetTokenInformation(token, TokenUser, data.data(), bytes, &bytes)) return {};
  LPSTR sid = nullptr;
  if (!ConvertSidToStringSidA(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid, &sid)) return {};
  std::string result(sid); LocalFree(sid); return result;
}

static void test_reopen(HANDLE token, DWORD pid, bool owner_rights, bool event) {
  ++cases;
  const std::wstring name = L"Local\\CodexStreamingCursorAcl_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
    std::to_wstring(pid) + (owner_rights ? L"_fixed" : L"_original") + (event ? L"_event" : L"_map");
  const std::wstring sddl = std::wstring(L"D:(A;;GA;;;SY)(A;;GA;;;BA)") +
#ifndef TEST_CURSOR_PRODUCER_REPAIR
    (owner_rights ? L"(A;;GA;;;OW)" : L"") +
#endif
    (event ? L"(A;;0x00100000;;;IU)" : L"(A;;0x00000004;;;IU)");
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) return;
  SECURITY_ATTRIBUTES sa {sizeof(sa), sd, FALSE};
  auto create = [&]() { return event ? CreateEventW(&sa, FALSE, FALSE, name.c_str()) :
    CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, 4096, name.c_str()); };
  if (!ImpersonateLoggedOnUser(token)) {
    std::printf("{\"pid\":%lu,\"impersonateError\":%lu}\n",pid,GetLastError()); LocalFree(sd); return;
  }
  HANDLE producer = create();
  const DWORD first_error = producer ? 0 : GetLastError();
  RevertToSelf();
  HANDLE consumer = producer ? (event ? OpenEventW(SYNCHRONIZE, FALSE, name.c_str()) :
    OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str())) : nullptr;
  const DWORD consumer_error = consumer ? 0 : GetLastError();
#ifdef TEST_CURSOR_PRODUCER_REPAIR
  if (owner_rights && consumer) {
    bool changed = false;
    const DWORD repair = platf::dxgi::vdd_cursor_security::preserve_producer_access(name.c_str(), event, changed);
    std::printf("{\"pid\":%lu,\"repairError\":%lu,\"changed\":%s}\n",pid,repair,changed?"true":"false");
    if (repair || !changed) ++failures;
    const DWORD repeated = platf::dxgi::vdd_cursor_security::preserve_producer_access(name.c_str(), event, changed);
    if (repeated || changed) ++failures;
  }
#endif
  if (producer) CloseHandle(producer);
  HANDLE reopened = nullptr;
  DWORD reopen_error = 0;
  if (consumer && ImpersonateLoggedOnUser(token)) {
    reopened = create(); reopen_error = reopened ? 0 : GetLastError(); RevertToSelf();
  }
  std::printf("{\"pid\":%lu,\"event\":%s,\"ownerRightsACE\":%s,\"firstCreateError\":%lu,\"consumerOpenError\":%lu,\"producerReopenError\":%lu,\"reopenSucceeded\":%s}\n",
    pid,event?"true":"false",owner_rights?"true":"false",first_error,consumer_error,reopen_error,reopened?"true":"false");
  if (first_error || consumer_error || (owner_rights ? !reopened : (reopen_error != ERROR_ACCESS_DENIED))) ++failures;
  if (reopened) CloseHandle(reopened);
  if (consumer) CloseHandle(consumer);
  LocalFree(sd);
}

#ifdef TEST_CURSOR_PRODUCER_REPAIR
static void test_rejected(HANDLE token, DWORD pid, bool extra_access) {
  const std::wstring name=L"Local\\CodexCursorRejected_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(pid);
  const wchar_t *sddl=extra_access ? L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00000004;;;IU)(A;;0x00000004;;;WD)" :
    L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00000004;;;IU)";
  PSECURITY_DESCRIPTOR sd=nullptr;
  if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,SDDL_REVISION_1,&sd,nullptr)){++failures;return;}
  SECURITY_ATTRIBUTES sa {sizeof(sa),sd,FALSE};
  if(token&&!ImpersonateLoggedOnUser(token)){LocalFree(sd);++failures;return;}
  HANDLE object=CreateFileMappingW(INVALID_HANDLE_VALUE,&sa,PAGE_READWRITE,0,4096,name.c_str());
  if(token)RevertToSelf();
  if(!object){LocalFree(sd);++failures;return;}
  bool changed=false;
  DWORD status=platf::dxgi::vdd_cursor_security::preserve_producer_access(name.c_str(),false,changed);
  std::printf("{\"pid\":%lu,\"negativeCase\":\"%s\",\"status\":%lu,\"changed\":%s}\n",pid,extra_access?"unexpected-ACE":"unexpected-owner",status,changed?"true":"false");
  if(status!=ERROR_NOT_SUPPORTED||changed)++failures;
  CloseHandle(object);LocalFree(sd);
}
#endif

int main() {
  HANDLE own = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &own)) {
    TOKEN_PRIVILEGES privileges {}; privileges.PrivilegeCount=1;
    if (LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &privileges.Privileges[0].Luid)) {
      privileges.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
      AdjustTokenPrivileges(own,FALSE,&privileges,0,nullptr,nullptr);
    }
    CloseHandle(own);
  }
  HANDLE map=OpenFileMappingW(FILE_MAP_READ|READ_CONTROL,FALSE,L"Global\\ZakoVDD_CursorMeta_0");
  if(map){
    PSECURITY_DESCRIPTOR sd=nullptr;
    if(GetSecurityInfo(map,SE_KERNEL_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,nullptr,nullptr,nullptr,nullptr,&sd)==ERROR_SUCCESS){
      LPSTR text=nullptr;
      if(ConvertSecurityDescriptorToStringSecurityDescriptorA(sd,SDDL_REVISION_1,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&text,nullptr)){
        std::printf("{\"existingCursorObjectSDDL\":\"%s\"}\n",text);LocalFree(text);
      }
      LocalFree(sd);
    }
    CloseHandle(map);
  }
  HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
  if(snapshot==INVALID_HANDLE_VALUE)return 1;
#ifdef TEST_CURSOR_PRODUCER_REPAIR
  test_rejected(nullptr,0,false);
#endif
  PROCESSENTRY32W entry {};entry.dwSize=sizeof(entry);
  if(Process32FirstW(snapshot,&entry)) do {
    if(_wcsicmp(entry.szExeFile,L"WUDFHost.exe")!=0)continue;
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID);
    HANDLE token=nullptr;
    if(process&&OpenProcessToken(process,TOKEN_QUERY|TOKEN_DUPLICATE,&token)){
      std::printf("{\"pid\":%lu,\"producerUserSID\":\"%s\"}\n",entry.th32ProcessID,token_sid(token).c_str());
      HANDLE impersonation=nullptr;
      if(DuplicateTokenEx(token,TOKEN_QUERY|TOKEN_IMPERSONATE,nullptr,SecurityImpersonation,TokenImpersonation,&impersonation)){
        for(bool event:{false,true})for(bool fixed:{false,true})test_reopen(impersonation,entry.th32ProcessID,fixed,event);
#ifdef TEST_CURSOR_PRODUCER_REPAIR
        test_rejected(impersonation,entry.th32ProcessID,true);
#endif
        CloseHandle(impersonation);
      }
      CloseHandle(token);
    }else std::printf("{\"pid\":%lu,\"tokenError\":%lu}\n",entry.th32ProcessID,GetLastError());
    if(process)CloseHandle(process);
  }while(Process32NextW(snapshot,&entry));
  CloseHandle(snapshot);return failures || cases < 4 ? 2 : 0;
}
