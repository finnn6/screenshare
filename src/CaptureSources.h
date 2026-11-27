#pragma once
#include <windows.h>
#include <string>
#include <vector>

// 공유할 수 있는 대상 하나 (모니터 또는 창)
struct CaptureSource {
    std::wstring name;
    HWND hwnd = nullptr;       // 창이면 채워짐
    HMONITOR hmon = nullptr;   // 모니터면 채워짐
};

// 모니터 → 창 순서로 목록을 만들어 돌려줌
std::vector<CaptureSource> ListCaptureSources();

// 유니코드(와이드) 문자열을 콘솔 출력용 UTF-8로 변환
std::string ToUtf8(const std::wstring& s);