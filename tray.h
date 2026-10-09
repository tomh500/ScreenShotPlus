#pragma once
#include "framework.h"
struct TrayIcon {
    NOTIFYICONDATAW data{sizeof(data)};
    bool registered = false, modern = false;
    DWORD diagnosticError = 0;
};
using NotifyIconCall = BOOL(WINAPI*)(DWORD, PNOTIFYICONDATAW);
bool RegisterTrayIcon(TrayIcon& tray, NotifyIconCall notify = Shell_NotifyIconW);
void RemoveTrayIcon(TrayIcon& tray);
bool TrayEventOpensPanel(LPARAM event);
// MAXDWORD means that the access token could not be queried.
DWORD CurrentProcessIntegrity();
std::wstring TrayFailureMessage(const TrayIcon& tray);
