#include "tray.h"
bool RegisterTrayIcon(TrayIcon& tray, NotifyIconCall notify) {
    const DWORD operation = tray.registered ? NIM_MODIFY : NIM_ADD;
    tray.registered = notify(operation, &tray.data) != FALSE;
    if (!tray.registered) tray.registered = notify(operation == NIM_ADD ? NIM_MODIFY : NIM_ADD, &tray.data) != FALSE;
    if (!tray.registered) { tray.modern = false; return false; }
    tray.data.uVersion = NOTIFYICON_VERSION_4;
    tray.modern = notify(NIM_SETVERSION, &tray.data) != FALSE;
    return true;
}
void RemoveTrayIcon(TrayIcon& tray) {
    Shell_NotifyIconW(NIM_DELETE, &tray.data); tray.registered = tray.modern = false;
}
bool TrayEventOpensPanel(LPARAM event) {
    const UINT message = LOWORD(event);
    return message == NIN_SELECT || message == NIN_KEYSELECT || message == WM_CONTEXTMENU ||
        message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_LBUTTONDBLCLK;
}
