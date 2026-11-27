#pragma once
#include <windows.h>

// Win32 창 하나를 담당
class Window {
public:
    bool Create(const wchar_t* title, int width, int height);

    // 쌓인 메시지를 처리. 창이 닫혔으면 false
    bool ProcessMessages();

    // 창 크기가 바뀌었으면 true와 새 크기를 돌려줌 (한 번 꺼내면 초기화)
    bool ConsumeResize(UINT& width, UINT& height);

    HWND Handle() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    bool m_resized = false;
    UINT m_width = 0;
    UINT m_height = 0;
};