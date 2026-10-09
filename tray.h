#pragma once
#include "framework.h"
struct TrayIcon {
    NOTIFYICONDATAW data{sizeof(data)};
    bool registered = false, modern = false;
};
using NotifyIconCall = BOOL(WINAPI*)(DWORD, PNOTIFYICONDATAW);
bool RegisterTrayIcon(TrayIcon& tray, NotifyIconCall notify = Shell_NotifyIconW);
void RemoveTrayIcon(TrayIcon& tray);
bool TrayEventOpensPanel(LPARAM event);
