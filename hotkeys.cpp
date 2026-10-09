#include "hotkeys.h"
using namespace std;
wstring RegisterConfiguredHotkeys(HWND window, Hotkeys& state, const Config& config) {
    wstring warning;
    state.primaryRegistered = RegisterHotKey(window, state.primaryId, config.modifiers | MOD_NOREPEAT, config.key) != FALSE;
    if (!state.primaryRegistered) warning += L"快捷键已被占用，请在设置中修改；仍可点击截图。";
    if (config.printScreen) {
        state.printRegistered = RegisterHotKey(window, 2, MOD_NOREPEAT, VK_SNAPSHOT) != FALSE;
        if (!state.printRegistered) warning += L" PrintScreen 注册失败。";
    }
    return warning;
}
bool ApplyConfiguredSettings(HWND window, Hotkeys& state, Config& current, const Config& next,
    const filesystem::path& file, wstring& error) {
    if (!ValidConfig(next)) { error = L"设置无效，请重新选择目录或快捷键。"; return false; }
    const bool newPrimary = !state.primaryRegistered || next.key != current.key || next.modifiers != current.modifiers;
    const bool newPrint = next.printScreen && !state.printRegistered;
    const int candidateId = state.primaryId == 1 ? 3 : 1;
    if (newPrimary && !RegisterHotKey(window, candidateId, next.modifiers | MOD_NOREPEAT, next.key)) {
        error = L"该快捷键已被占用，原设置保持有效。"; return false;
    }
    if (newPrint && !RegisterHotKey(window, 2, MOD_NOREPEAT, VK_SNAPSHOT)) {
        if (newPrimary) UnregisterHotKey(window, candidateId);
        error = L"PrintScreen 无法注册，原设置保持有效。"; return false;
    }
    if (!SaveConfig(file, next, error)) {
        if (newPrimary) UnregisterHotKey(window, candidateId);
        if (newPrint) UnregisterHotKey(window, 2);
        return false;
    }
    if (newPrimary) {
        if (state.primaryRegistered) UnregisterHotKey(window, state.primaryId);
        state.primaryId = candidateId; state.primaryRegistered = true;
    }
    if (state.printRegistered && !next.printScreen) UnregisterHotKey(window, 2);
    state.printRegistered = next.printScreen; current = next;
    return true;
}
void ReleaseHotkeys(HWND window, const Hotkeys& state) {
    if (state.primaryRegistered) UnregisterHotKey(window, state.primaryId);
    if (state.printRegistered) UnregisterHotKey(window, 2);
}
