#include "Renderer.h"
#include <iostream>

using Microsoft::WRL::ComPtr;

bool Renderer::Init(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &m_swapChain, &m_device, nullptr, &m_context);
    if (FAILED(hr)) {
        std::cout << "D3D11 초기화 실패: 0x" << std::hex << hr << std::dec << "\n";
        return false;
    }

    // 미리보기 프레임이 GPU 대기열에 1장까지만 쌓이게 (지연 줄이기 + 공유 버퍼를 빨리 놓아주기)
    ComPtr<IDXGIDevice1> dxgiDevice;
    if (SUCCEEDED(m_device.As(&dxgiDevice))) {
        dxgiDevice->SetMaximumFrameLatency(1);
    }

    CreateRTV();
    return m_scaler.Init(m_device.Get());
}

void Renderer::CreateRTV() {
    ComPtr<ID3D11Texture2D> backBuffer;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv);

    D3D11_TEXTURE2D_DESC d;
    backBuffer->GetDesc(&d);
    m_bbWidth = d.Width;
    m_bbHeight = d.Height;
}

void Renderer::Resize(UINT width, UINT height) {
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_rtv.Reset();
    m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    CreateRTV();
}

void Renderer::Draw(ID3D11ShaderResourceView* srv, UINT srcWidth, UINT srcHeight) {
    m_scaler.DrawFit(m_context.Get(), m_rtv.Get(), m_bbWidth, m_bbHeight,
                     srv, srcWidth, srcHeight, false);
}

void Renderer::Present() {
    m_swapChain->Present(1, 0);
}