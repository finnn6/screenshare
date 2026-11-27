#include "Window.h"

bool Window::Create(const wchar_t* title, int width, int height) {
    HINSTANCE hInstance = GetModuleHandle(nullptr);

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"ScreenShareViewer";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&wc);

    // 마지막 인자로 this(이 Window 객체의 주소)를 넘김 → WndProc에서 꺼내 씀
    m_hwnd = CreateWindowEx(
        0, L"ScreenShareViewer", title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        nullptr, nullptr, hInstance, this);
    if (!m_hwnd) return false;

    ShowWindow(m_hwnd, SW_SHOW);
    return true;
}

bool Window::ProcessMessages() {
    MSG msg = {};
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return true;
}

bool Window::ConsumeResize(UINT& width, UINT& height) {
    if (!m_resized || m_width == 0 || m_height == 0) return false;  // 최소화 중이면 무시
    m_resized = false;
    width = m_width;
    height = m_height;
    return true;
}

LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // 창이 처음 만들어질 때, 넘겨받은 this를 창에 붙여둠
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    // 붙여둔 this를 다시 꺼냄
    auto self = reinterpret_cast<Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_SIZE:
        if (self) {
            self->m_width = LOWORD(lParam);
            self->m_height = HIWORD(lParam);
            self->m_resized = true;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}