#include "CaptureSources.h"
#include <dwmapi.h>

// 모니터 하나마다 윈도우가 호출
static BOOL CALLBACK MonitorCallback(HMONITOR hmon, HDC, LPRECT, LPARAM lParam) {
    auto list = reinterpret_cast<std::vector<CaptureSource>*>(lParam);

    MONITORINFOEX info = {};
    info.cbSize = sizeof(info);
    GetMonitorInfo(hmon, &info);
    int w = info.rcMonitor.right - info.rcMonitor.left;
    int h = info.rcMonitor.bottom - info.rcMonitor.top;

    CaptureSource src;
    src.name = L"[화면] 모니터 " + std::to_wstring(list->size() + 1)
             + L" (" + std::to_wstring(w) + L"x" + std::to_wstring(h) + L")";
    if (info.dwFlags & MONITORINFOF_PRIMARY) src.name += L" - 주 모니터";
    src.hmon = hmon;
    list->push_back(src);
    return TRUE;   // 계속 다음 모니터로
}

// 목록에 보여줄 만한 창인지 판단
static bool IsCapturableWindow(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return false;                     // 안 보이는 창
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;       // 다른 창에 딸린 창 (대화상자 등)
    if (GetWindowLong(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;  // 작은 도구 창
    if (GetWindowTextLength(hwnd) == 0) return false;             // 제목 없는 창

    DWORD cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;                                    // 보이는 척하지만 숨겨진 창

    return true;
}

// 최상위 창 하나마다 윈도우가 호출
static BOOL CALLBACK WindowCallback(HWND hwnd, LPARAM lParam) {
    if (!IsCapturableWindow(hwnd)) return TRUE;

    auto list = reinterpret_cast<std::vector<CaptureSource>*>(lParam);

    int len = GetWindowTextLength(hwnd);
    std::wstring title(len + 1, L'\0');
    GetWindowText(hwnd, title.data(), len + 1);
    title.resize(len);

    CaptureSource src;
    src.name = L"[창] " + title;
    src.hwnd = hwnd;
    list->push_back(src);
    return TRUE;
}

std::vector<CaptureSource> ListCaptureSources() {
    std::vector<CaptureSource> list;
    EnumDisplayMonitors(nullptr, nullptr, MonitorCallback, reinterpret_cast<LPARAM>(&list));
    EnumWindows(WindowCallback, reinterpret_cast<LPARAM>(&list));
    return list;
}

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), size, nullptr, nullptr);
    return out;
}