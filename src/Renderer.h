#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include "Scaler.h"

// 미리보기 창: D3D11 장치 + 스왑체인
class Renderer {
public:
    bool Init(HWND hwnd);
    void Resize(UINT width, UINT height);

    // 텍스처를 비율 유지해서 창에 그림 (srv가 없으면 검은 화면)
    void Draw(ID3D11ShaderResourceView* srv, UINT srcWidth, UINT srcHeight);

    // 그린 화면을 내보냄
    void Present();

    ID3D11Device* Device() const { return m_device.Get(); }
    ID3D11DeviceContext* Context() const { return m_context.Get(); }

private:
    void CreateRTV();

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    Scaler m_scaler;
    UINT m_bbWidth = 0;
    UINT m_bbHeight = 0;
};