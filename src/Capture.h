#pragma once
#include <windows.h>
#include <unknwn.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// WGC로 화면을 받아서 마지막 프레임을 텍스처로 들고 있음
class Capture {
public:
    Capture();
    ~Capture();
    Capture(const Capture&) = delete;              // 복사 금지 (이벤트 핸들을 두 번 닫지 않게)
    Capture& operator=(const Capture&) = delete;

    static bool IsSupported();

    bool StartMonitor(HMONITOR hmon, ID3D11Device* device);
    bool StartWindow(HWND hwnd, ID3D11Device* device);
    void Stop();

    // 새 프레임이 왔으면 내부 텍스처를 갱신하고 true
    bool Update(ID3D11DeviceContext* context);

    // 새 프레임이 도착하면 켜지는 신호 (WaitForSingleObject로 기다릴 수 있음)
    HANDLE FrameEvent() const { return m_frameEvent; }

    ID3D11ShaderResourceView* SRV() const { return m_srv.Get(); }
    UINT Width() const { return m_width; }
    UINT Height() const { return m_height; }

    // 마지막으로 꺼낸 프레임이 만들어진 시각 (WGC SystemRelativeTime, QPC 기준, 100ns 단위)
    // 지연 측정용: QueryPerformanceCounter를 100ns 단위로 바꾼 값과 빼면 "캡처된 지 얼마나 지났는지"가 된다
    int64_t LastFrameTime100ns() const { return m_lastFrameTime100ns; }

private:
    bool StartItem(winrt::Windows::Graphics::Capture::GraphicsCaptureItem item, ID3D11Device* device);
    void CreateTexture(UINT width, UINT height);

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{ nullptr };
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };
    winrt::Windows::Graphics::SizeInt32 m_poolSize{};
    winrt::event_token m_frameArrivedToken{};
    HANDLE m_frameEvent = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_latest;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    UINT m_width = 0;
    UINT m_height = 0;
    int64_t m_lastFrameTime100ns = 0;
};