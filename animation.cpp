#include "animation.h"
#include "ui.h"
#include <mutex>
#include <thread>
#include <new>
#include <atomic>
using namespace std;
using namespace Gdiplus;

namespace ui {
namespace {
constexpr UINT frameMessage = WM_APP + 70;
constexpr UINT optionsMessage = WM_APP + 74;
constexpr wchar_t stateProperty[] = L"ScreenShotPlus.Motion";
atomic<unsigned> motionLevel{2}, motionSpeed{1};
atomic<bool> systemMotionEnabled{true};
double Now() { return chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count(); }
bool AnimationsEnabled() {
    BOOL enabled = TRUE; HIGHCONTRASTW contrast{sizeof(contrast)};
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    return enabled && !(contrast.dwFlags & HCF_HIGHCONTRASTON);
}
struct FrameSlot {
    HWND window = nullptr;
    UINT_PTR cookie = 0;
    double rate = 120, next = 0;
    bool queued = false;
};
// One sleeping worker serves all windows. It only posts messages; all painting
// and curves stay on the owning UI thread. One queued message per window max.
struct FrameClock {
    HANDLE wake = nullptr, timer = nullptr;
    mutex lock;
    vector<FrameSlot> slots;
    thread worker;
    bool stopping = false;
    UINT_PTR nextCookie = 0;
    FrameClock() {
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        if (wake && timer) {
            try { worker = thread([this] { Run(); }); }
            catch (const system_error&) { CloseHandle(timer); timer = nullptr; }
        }
    }
    ~FrameClock() {
        { lock_guard guard(lock); stopping = true; }
        if (wake) SetEvent(wake);
        if (worker.joinable()) worker.join();
        if (timer) CloseHandle(timer);
        if (wake) CloseHandle(wake);
    }
    bool Ready() const { return worker.joinable(); }
    UINT_PTR Cookie() { lock_guard guard(lock); return ++nextCookie; }
    void Schedule(HWND window, UINT_PTR cookie, double rate, bool active) {
        lock_guard guard(lock);
        auto it = find_if(slots.begin(), slots.end(), [&](const auto& s) { return s.cookie == cookie; });
        if (!active) { if (it != slots.end()) slots.erase(it); }
        else if (it == slots.end()) slots.push_back({window, cookie, rate, Now() + 1.0 / rate, false});
        else if (it->rate != rate) { it->rate = rate; it->next = Now() + 1.0 / rate; }
        if (wake) SetEvent(wake);
    }
    void Acknowledge(UINT_PTR cookie) {
        lock_guard guard(lock);
        for (auto& slot : slots) if (slot.cookie == cookie) slot.queued = false;
        SetEvent(wake);
    }
    void Run() {
        const HANDLE handles[]{wake, timer};
        for (;;) {
            double deadline = 0;
            {
                lock_guard guard(lock);
                if (stopping) return;
                const double now = Now();
                for (auto& slot : slots) {
                    if (slot.queued) continue;
                    if (slot.next <= now) {
                        // Advance from the previous deadline, skip missed frames.
                        const double period = 1.0 / slot.rate;
                        slot.next += (floor((now - slot.next) / period) + 1) * period;
                        slot.queued = PostMessageW(slot.window, frameMessage, slot.cookie, 0) != FALSE;
                        if (slot.queued) continue;
                    }
                    if (!deadline || slot.next < deadline) deadline = slot.next;
                }
            }
            if (!deadline) {
                CancelWaitableTimer(timer);
                if (WaitForSingleObject(wake, INFINITE) != WAIT_OBJECT_0) return;
            } else {
                LARGE_INTEGER due{};
                due.QuadPart = -max<LONGLONG>(1, static_cast<LONGLONG>((deadline - Now()) * 10000000.0));
                if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(wake, max<DWORD>(1, static_cast<DWORD>(ceil(max(0.0, deadline - Now()) * 1000))));
                } else if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_FAILED) return;
            }
        }
    }
};
FrameClock& Clock() { static FrameClock clock; return clock; }
struct Control {
    RectF rect;
    Tween hover, press, selected;
    unsigned serial = 0;
};
struct ValueTrack { unsigned key = 0, serial = 0; Tween tween; bool always = false; };
struct Motion {
    UINT_PTR cookie = 0;
    HMONITOR monitor = nullptr;
    double rate = 120, paintTime = 0;
    unsigned long long frames = 0;
    unsigned serial = 0;
    bool enabled = true, running = false, down = false, continuous = false;
    bool localRegion = false, controlActive = false;
    RECT region{};
    POINT pressed{-1, -1}, mouse{-1, -1};
    Tween entrance;
    vector<Control> controls;
    vector<ValueTrack> values;
};
Motion* State(HWND hwnd) { return static_cast<Motion*>(GetPropW(hwnd, stateProperty)); }
void Stop(HWND hwnd, Motion& state) {
    Clock().Schedule(hwnd, state.cookie, state.rate, false); state.running = false;
}
void UpdateRate(HWND hwnd, Motion& state) {
    state.monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    state.rate = RefreshRate(hwnd);
    if (state.running) Clock().Schedule(hwnd, state.cookie, state.rate, true);
}
void Settle(Motion& state, bool keepAlways = false) {
    const auto settle = [](Tween& t) { t.from = t.target; t.duration = 0; };
    settle(state.entrance);
    for (auto& c : state.controls) { settle(c.hover); settle(c.press); settle(c.selected); }
    for (auto& value : state.values) if (!keepAlways || !value.always) settle(value.tween);
}
void SchedulePaint(HWND hwnd, Motion& state, double now) {
    bool active = state.entrance.Active(now), forced = false;
    state.controlActive = false;
    for (const auto& c : state.controls) state.controlActive |= c.hover.Active(now) || c.press.Active(now) || c.selected.Active(now);
    active |= state.controlActive;
    for (const auto& value : state.values) {
        if (value.always) forced |= value.tween.Active(now);
        else active |= value.tween.Active(now);
    }
    // Pointer feedback and the intensity selector are independent of the
    // decorative-motion policy, including Windows' reduced-motion setting.
    state.running = (state.continuous || forced || (state.enabled && active)) &&
        IsWindowVisible(hwnd) && !IsIconic(hwnd);
    Clock().Schedule(hwnd, state.cookie, state.rate, state.running);
}
void UpdatePolicy(HWND hwnd, Motion& state) {
    state.enabled = DecorativeMotion() && Clock().Ready();
    if (!state.enabled) Settle(state, true);
    SchedulePaint(hwnd, state, Now());
    InvalidateRect(hwnd, nullptr, FALSE);
}
}

void SetMotionOptions(unsigned level, unsigned speed) {
    motionLevel.store(min(level, 4u)); motionSpeed.store(min(speed, 2u));
    systemMotionEnabled.store(AnimationsEnabled());
    // Window state stays on each window's owning UI thread. Send immediately
    // on this thread and post elsewhere without borrowing another thread's
    // Motion pointer or holding a lock while invoking a window procedure.
    EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(hwnd, &process);
        if (process == GetCurrentProcessId() && GetPropW(hwnd, stateProperty)) {
            if (thread == GetCurrentThreadId()) SendMessageW(hwnd, optionsMessage, 0, 0);
            else PostMessageW(hwnd, optionsMessage, 0, 0);
        }
        return TRUE;
    }, 0);
}
unsigned MotionLevel() { return motionLevel.load(); }
float MotionAmount() {
    constexpr float amounts[]{0, 0.4f, 0.75f, 1, 1.3f};
    return amounts[MotionLevel()];
}
double MotionDuration(double seconds) {
    constexpr double speeds[]{0.72, 1, 1.28}, levels[]{0.75, 0.85, 1, 1.12, 1.24};
    return max(0.0, seconds) * speeds[motionSpeed.load()] * levels[MotionLevel()];
}
bool DecorativeMotion(unsigned minimum) {
    const auto level = MotionLevel();
    return level && level >= minimum && systemMotionEnabled.load();
}

float Tween::Value(double now) const {
    if (duration <= 0 || now >= start + duration) return target;
    const float t = static_cast<float>(clamp((now - start) / duration, 0.0, 1.0));
    const float eased = 1 - (1 - t) * (1 - t) * (1 - t);
    return from + (target - from) * eased;
}
bool Tween::Active(double now) const { return from != target && duration > 0 && now < start + duration; }
void Tween::Set(float value, double now, double seconds, bool animate) {
    if (target == value && animate) return;
    from = animate ? Value(now) : value; target = value; start = now;
    duration = animate ? max(0.0, seconds) : 0;
}
double RefreshRateValue(UINT32 numerator, UINT32 denominator) {
    if (!denominator) return 120;
    const double value = static_cast<double>(numerator) / denominator;
    return isfinite(value) && value > 1 && value <= 1000 ? value : 120;
}
double MonitorRate(HMONITOR display) {
    MONITORINFOEXW monitor{}; monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(display, &monitor)) return 120;
    // CCD preserves rates such as 60000/1001 instead of rounding to 60.
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathsCount = 0, modesCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathsCount, &modesCount) != ERROR_SUCCESS ||
            !pathsCount || !modesCount) break;
        vector<DISPLAYCONFIG_PATH_INFO> paths(pathsCount);
        vector<DISPLAYCONFIG_MODE_INFO> modes(modesCount);
        const LONG result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathsCount, paths.data(), &modesCount, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER) continue;
        if (result != ERROR_SUCCESS) break;
        double rate = 0;
        for (UINT32 i = 0; i < pathsCount; ++i) {
            const auto& path = paths[i];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), path.sourceInfo.adapterId, path.sourceInfo.id};
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || wcscmp(source.viewGdiDeviceName, monitor.szDevice)) continue;
            const auto& refresh = path.targetInfo.refreshRate;
            if (refresh.Denominator && refresh.Numerator > refresh.Denominator) {
                const double candidate = static_cast<double>(refresh.Numerator) / refresh.Denominator;
                // Mirrored displays share a source: use the slowest valid target.
                if (candidate <= 1000 && (!rate || candidate < rate)) rate = candidate;
            }
        }
        if (rate) return rate;
        break;
    }
    DEVMODEW mode{}; mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) && (mode.dmFields & DM_DISPLAYFREQUENCY))
        return RefreshRateValue(mode.dmDisplayFrequency, 1);
    return 120;
}
double RefreshRate(HWND hwnd) { return MonitorRate(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)); }
bool MotionMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCDESTROY) ReleasePaintBuffer(hwnd);
    if (message == WM_NCCREATE) {
        auto state = new (nothrow) Motion;
        if (state) {
            systemMotionEnabled.store(AnimationsEnabled());
            state->cookie = Clock().Cookie(); state->enabled = DecorativeMotion() && Clock().Ready();
            if (!SetPropW(hwnd, stateProperty, state)) delete state;
        }
    }
    auto state = State(hwnd);
    if (!state) return message == frameMessage;
    switch (message) {
    case frameMessage:
        if (wparam == state->cookie) {
            Clock().Acknowledge(state->cookie);
            if (IsWindowVisible(hwnd) && !IsIconic(hwnd) && state->running) {
                ++state->frames;
                InvalidateRect(hwnd, state->localRegion && !state->controlActive ? &state->region : nullptr, FALSE);
                // WM_PAINT is low priority; paint now so a busy pointer queue
                // cannot starve a scheduled animation frame.
                UpdateWindow(hwnd);
            } else Stop(hwnd, *state);
        }
        return true;
    case WM_SHOWWINDOW:
        if (wparam) RestartMotion(hwnd);
        else { state->down = false; state->continuous = false; Settle(*state); Stop(hwnd, *state); }
        break;
    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) Stop(hwnd, *state);
        break;
    case WM_WINDOWPOSCHANGED:
        if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != state->monitor) UpdateRate(hwnd, *state);
        break;
    case WM_DISPLAYCHANGE: case WM_DPICHANGED:
        UpdateRate(hwnd, *state); break;
    case WM_SETTINGCHANGE:
        UpdateRate(hwnd, *state);
        systemMotionEnabled.store(AnimationsEnabled());
        UpdatePolicy(hwnd, *state); break;
    case optionsMessage:
        UpdatePolicy(hwnd, *state); return true;
    case WM_MOUSEMOVE:
        state->mouse = Mouse(hwnd, lparam); break;
    case WM_LBUTTONDOWN:
        state->mouse = state->pressed = Mouse(hwnd, lparam); state->down = true;
        InvalidateRect(hwnd, nullptr, FALSE); break;
    case WM_LBUTTONUP: case WM_CAPTURECHANGED: case WM_CANCELMODE:
        state->down = false; InvalidateRect(hwnd, nullptr, FALSE); break;
    case WM_MOUSELEAVE:
        state->mouse = {-1, -1}; break;
    case WM_NCDESTROY:
        Stop(hwnd, *state); RemovePropW(hwnd, stateProperty); delete state;
        break;
    }
    return false;
}
void RestartMotion(HWND hwnd, double seconds) {
    if (auto state = State(hwnd)) {
        systemMotionEnabled.store(AnimationsEnabled());
        state->enabled = DecorativeMotion() && Clock().Ready();
        state->down = false; state->mouse = {-1, -1}; state->controls.clear(); state->values.clear();
        UpdateRate(hwnd, *state);
        state->entrance = {}; state->entrance.Set(1, Now(), MotionDuration(seconds), state->enabled);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}
void BeginMotionPaint(HWND hwnd) {
    if (auto state = State(hwnd)) { state->paintTime = Now(); ++state->serial; }
}
void EndMotionPaint(HWND hwnd) {
    auto state = State(hwnd);
    if (!state) return;
    erase_if(state->controls, [&](const auto& control) { return control.serial != state->serial; });
    erase_if(state->values, [&](const auto& value) { return value.serial != state->serial; });
    SchedulePaint(hwnd, *state, state->paintTime);
}
float Entrance(HWND hwnd) {
    auto state = State(hwnd); return state ? state->entrance.Value(state->paintTime) : 1;
}
float EntranceProgress(HWND hwnd) {
    auto state = State(hwnd);
    if (!state || state->entrance.duration <= 0) return 1;
    return static_cast<float>(clamp((state->paintTime - state->entrance.start) / state->entrance.duration, 0.0, 1.0));
}
float SpringReveal(float progress) {
    const float t = clamp(progress, 0.0f, 1.0f);
    if (t == 0 || t == 1) return t;
    const auto curve = [](float p) { return 1 - exp(-8 * p) * (cos(10 * p) + 0.8f * sin(10 * p)); };
    return curve(t) / curve(1);
}
float AnimateValue(HWND hwnd, unsigned key, float target, double seconds, float initial, bool always) {
    auto state = State(hwnd);
    if (!state) return target;
    auto it = find_if(state->values.begin(), state->values.end(), [&](const auto& value) { return value.key == key; });
    if (it == state->values.end()) {
        state->values.push_back({key}); it = prev(state->values.end());
        it->tween.Set(initial, state->paintTime, 0, false);
    }
    it->serial = state->serial;
    it->always = always;
    it->tween.Set(target, state->paintTime, MotionDuration(seconds), always ? Clock().Ready() : state->enabled);
    return it->tween.Value(state->paintTime);
}
void ContinuousMotion(HWND hwnd, bool active) {
    if (auto state = State(hwnd)) {
        state->continuous = active;
        if (active) { state->running = IsWindowVisible(hwnd) != FALSE; Clock().Schedule(hwnd, state->cookie, state->rate, state->running); }
    }
}
void MotionMonitor(HWND hwnd, HMONITOR monitor) {
    if (auto state = State(hwnd); state && monitor && state->monitor != monitor) {
        state->monitor = monitor; state->rate = MonitorRate(monitor);
        if (state->running) Clock().Schedule(hwnd, state->cookie, state->rate, true);
    }
}
void MotionRegion(HWND hwnd, const RECT* region) {
    if (auto state = State(hwnd)) {
        state->localRegion = region != nullptr;
        if (region) state->region = *region;
    }
}
Interaction AnimateControl(HWND hwnd, RectF rect, bool hovered, bool selected) {
    auto state = State(hwnd);
    if (!state) return {hovered ? 1.0f : 0.0f, 0, selected ? 1.0f : 0.0f};
    auto it = find_if(state->controls.begin(), state->controls.end(), [&](const auto& c) { return c.rect.Equals(rect); });
    if (it == state->controls.end()) {
        state->controls.push_back({rect}); it = prev(state->controls.end());
        it->selected.Set(selected ? 1.0f : 0.0f, state->paintTime, 0, false);
    }
    it->serial = state->serial;
    const double now = state->paintTime;
    it->hover.Set(hovered ? 1.0f : 0.0f, now, MotionDuration(0.16), state->enabled);
    const bool pressing = state->enabled && state->down && Inside(state->pressed, rect) && Inside(state->mouse, rect);
    it->press.Set(pressing ? 1.0f : 0.0f, now, MotionDuration(pressing ? 0.075 : 0.19), state->enabled);
    it->selected.Set(selected ? 1.0f : 0.0f, now, MotionDuration(0.20), state->enabled);
    return {it->hover.Value(now), it->press.Value(now), it->selected.Value(now)};
}
MotionInfo GetMotionInfo(HWND hwnd) {
    auto state = State(hwnd);
    return state ? MotionInfo{state->rate, state->frames, state->enabled, state->running} : MotionInfo{};
}
}
