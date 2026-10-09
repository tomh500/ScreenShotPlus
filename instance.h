#pragma once
#include "framework.h"
inline constexpr wchar_t INSTANCE_MUTEX[] = L"Local\\ScreenShotPlus.Singleton";
inline constexpr wchar_t INSTANCE_PANEL_MARKER[] = L"ScreenShotPlus.PrimaryInstance";
struct InstanceLock {
    HANDLE handle = nullptr;
    bool first = false;
    ~InstanceLock();
    InstanceLock() = default;
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;
};
bool AcquireInstanceLock(InstanceLock& lock, const wchar_t* name = INSTANCE_MUTEX);
bool ActivateExistingInstance(unsigned timeoutMs = 3000, const wchar_t* panelClass = PANEL_CLASS);
