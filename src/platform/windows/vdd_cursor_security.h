#pragma once

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

namespace platf::dxgi::vdd_cursor_security {
  // ZakoVDD <= 0.18.2 creates cursor objects as LocalService, but its DACL
  // omits the producer. A consumer can keep an old object alive across a
  // swap-chain replacement, making the next Create* fail with access denied.
  // Preserve the owner's access only for that exact legacy DACL. Interactive
  // consumers retain their existing read/synchronize permissions.
  inline DWORD preserve_producer_access(const wchar_t *name, bool event, bool &changed) {
    changed = false;
    HANDLE object = event ? OpenEventW(READ_CONTROL | WRITE_DAC | SYNCHRONIZE, FALSE, name) :
      OpenFileMappingW(READ_CONTROL | WRITE_DAC | FILE_MAP_READ, FALSE, name);
    if (!object) return GetLastError();

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL updated_acl = nullptr;
    PSID owner_rights = nullptr;
    const DWORD result = [&]() -> DWORD {
      PSID owner = nullptr;
      PACL acl = nullptr;
      DWORD status = GetSecurityInfo(object, SE_KERNEL_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr, &descriptor);
      if (status != ERROR_SUCCESS) return status;
      if (!owner || !IsWellKnownSid(owner, WinLocalServiceSid) || !acl ||
          (acl->AceCount != 3 && acl->AceCount != 4)) return ERROR_NOT_SUPPORTED;
      if (!ConvertStringSidToSidW(L"S-1-3-4", &owner_rights)) return GetLastError();

      const DWORD full_access = event ? EVENT_ALL_ACCESS : FILE_MAP_ALL_ACCESS;
      unsigned int seen = 0;
      for (WORD i = 0; i < acl->AceCount; ++i) {
        void *raw_ace = nullptr;
        if (!GetAce(acl, i, &raw_ace)) return GetLastError();
        const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(raw_ace);
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || ace->Header.AceFlags) return ERROR_NOT_SUPPORTED;
        PSID sid = const_cast<DWORD *>(&ace->SidStart);
        unsigned int role = 0;
        if (IsWellKnownSid(sid, WinLocalSystemSid)) role = 1;
        else if (IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) role = 2;
        else if (IsWellKnownSid(sid, WinInteractiveSid)) role = 4;
        else if (EqualSid(sid, owner_rights)) role = 8;
        if (!role || (seen & role)) return ERROR_NOT_SUPPORTED;
        const DWORD expected = role == 4 ? (event ? SYNCHRONIZE : FILE_MAP_READ) : full_access;
        if (ace->Mask != expected && !(role != 4 && ace->Mask == GENERIC_ALL)) return ERROR_NOT_SUPPORTED;
        seen |= role;
      }
      if (seen == 15) return ERROR_SUCCESS;
      if (seen != 7) return ERROR_NOT_SUPPORTED;

      EXPLICIT_ACCESSW access {};
      access.grfAccessPermissions = GENERIC_ALL;
      access.grfAccessMode = GRANT_ACCESS;
      access.grfInheritance = NO_INHERITANCE;
      access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
      access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
      access.Trustee.ptstrName = static_cast<LPWSTR>(owner_rights);
      status = SetEntriesInAclW(1, &access, acl, &updated_acl);
      if (status != ERROR_SUCCESS) return status;
      status = SetSecurityInfo(object, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, updated_acl, nullptr);
      changed = status == ERROR_SUCCESS;
      return status;
    }();
    if (updated_acl) LocalFree(updated_acl);
    if (owner_rights) LocalFree(owner_rights);
    if (descriptor) LocalFree(descriptor);
    CloseHandle(object);
    return result;
  }
}
