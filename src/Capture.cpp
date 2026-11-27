#include "Capture.h"
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <iostream>
#include <winrt/Windows.Foundation.Metadata.h>

using Microsoft::WRL::ComPtr;
namespace wgc   = winrt::Windows::Graphics::Capture;
namespace wgdx  = winrt::Windows::Graphics::DirectX;
namespace wgd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

static const auto kPixelFormat = wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;

// ---------- 이 파일 안에서만 쓰는 헬퍼 ----------
static void PrintError(const char* what, const winrt::hresult_error& e) {
    std::cout << what << ": 0x" << std::hex << e.code().value << std::dec << "\n";
}

static wgc::GraphicsCaptureItem CreateItemForMonitor(HMONITOR hmon) {
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(interop->CreateForMonitor(
        hmon, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
    return item;
}

static wgc::GraphicsCaptureItem CreateItemForWindow(HWND hwnd) {
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(interop->CreateForWindow(
        hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
    return item;
}

static wgd3d::IDirect3DDevice CreateWinRTDevice(ID3D11Device* d3dDevice) {
    ComPtr<IDXGIDevice> dxgiDevice;
    d3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));
    return inspectable.as<wgd3d::IDirect3DDevice>();
}

static ComPtr<ID3D11Texture2D> GetTexture(const wgc::Direct3D11CaptureFrame& frame) {
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> texture;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
    return texture;
}

// ---------- Capture ----------
Capture::Capture() {
    // 자동으로 꺼지는 신호: 한 번 기다려서 깨어나면 다시 꺼짐
    m_frameEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

Capture::~Capture() {
    Stop();
    if (m_frameEvent) CloseHandle(m_frameEvent);
}

bool Capture::IsSupported() {
    return wgc::GraphicsCaptureSession::IsSupported();
}

bool Capture::StartMonitor(HMONITOR hmon, ID3D11Device* device) {
    try {
        return StartItem(CreateItemForMonitor(hmon), device);
    }
    catch (const winrt::hresult_error& e) {
        PrintError("모니터 캡처 대상 생성 실패", e);
        return false;
    }
}

bool Capture::StartWindow(HWND hwnd, ID3D11Device* device) {
    try {
        return StartItem(CreateItemForWindow(hwnd), device);
    }
    catch (const winrt::hresult_error& e) {
        PrintError("창 캡처 대상 생성 실패", e);
        return false;
    }
}

bool Capture::StartItem(wgc::GraphicsCaptureItem item, ID3D11Device* device) {
    Stop();

    try {
        m_device = device;
        m_item = item;
        m_poolSize = m_item.Size();
        CreateTexture(m_poolSize.Width, m_poolSize.Height);

        m_winrtDevice = CreateWinRTDevice(device);
        m_framePool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            m_winrtDevice, kPixelFormat, 2, m_poolSize);

        // 새 프레임이 도착할 때마다 윈도우가 이 함수를 (다른 스레드에서) 불러줌 → 신호만 켬
        m_frameArrivedToken = m_framePool.FrameArrived([this](auto&&, auto&&) {
            SetEvent(m_frameEvent);
        });

        m_session = m_framePool.CreateCaptureSession(m_item);
        // 캡처 간격 제한 풀기 (기본은 약 60fps). Windows 11 24H2 이상에서만 지원
        using winrt::Windows::Foundation::Metadata::ApiInformation;
        if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession",
                                              L"MinUpdateInterval")) {
            m_session.MinUpdateInterval(std::chrono::milliseconds(1));
            std::cout << "캡처 간격 제한: 1ms\n";
        }
        else {
            std::cout << "이 윈도우 버전은 캡처 간격 설정을 지원하지 않음 (약 60fps 제한)\n";
        }
        m_session.StartCapture();
        return true;
    }
    catch (const winrt::hresult_error& e) {
        PrintError("캡처 시작 실패", e);
        Stop();
        return false;
    }
}

void Capture::Stop() {
    if (m_session) { m_session.Close(); m_session = nullptr; }
    if (m_framePool) {
        m_framePool.FrameArrived(m_frameArrivedToken);   // 알림 해제
        m_framePool.Close();
        m_framePool = nullptr;
    }
    m_item = nullptr;
    m_winrtDevice = nullptr;
    m_poolSize = {};
    m_srv.Reset();
    m_latest.Reset();
    m_device.Reset();
    m_width = 0;
    m_height = 0;
}

void Capture::CreateTexture(UINT width, UINT height) {
    m_srv.Reset();
    m_latest.Reset();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &m_latest);
    if (FAILED(hr)) {
        std::cout << "보관 텍스처 생성 실패: 0x" << std::hex << hr << std::dec << "\n";
        return;
    }
    hr = m_device->CreateShaderResourceView(m_latest.Get(), nullptr, &m_srv);
    if (FAILED(hr)) {
        std::cout << "SRV 생성 실패: 0x" << std::hex << hr << std::dec << "\n";
        return;
    }

    m_width = width;
    m_height = height;
}

bool Capture::Update(ID3D11DeviceContext* context) {
    if (!m_framePool) return false;

    auto frame = m_framePool.TryGetNextFrame();
    if (!frame) return false;
    m_lastFrameTime100ns = frame.SystemRelativeTime().count();

    auto contentSize = frame.ContentSize();
    if (contentSize.Width > 0 && contentSize.Height > 0 &&
        (contentSize.Width != m_poolSize.Width || contentSize.Height != m_poolSize.Height)) {
        frame.Close();
        m_poolSize = contentSize;
        m_framePool.Recreate(m_winrtDevice, kPixelFormat, 2, m_poolSize);
        CreateTexture(m_poolSize.Width, m_poolSize.Height);
        return false;
    }

    bool updated = false;
    auto texture = GetTexture(frame);
    D3D11_TEXTURE2D_DESC d;
    texture->GetDesc(&d);
    if (m_latest && d.Width == m_width && d.Height == m_height) {
        context->CopySubresourceRegion(m_latest.Get(), 0, 0, 0, 0, texture.Get(), 0, nullptr);
        context->GenerateMips(m_srv.Get());
        updated = true;
    }
    frame.Close();
    return updated;
}