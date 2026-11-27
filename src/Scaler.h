#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

// 텍스처를 다른 텍스처(또는 화면)에 비율 유지해서 그려주는 도구
class Scaler {
public:
    bool Init(ID3D11Device* device);

    void DrawFit(ID3D11DeviceContext* context,
                 ID3D11RenderTargetView* target, UINT targetW, UINT targetH,
                 ID3D11ShaderResourceView* srv, UINT srcW, UINT srcH, bool allowUpscale);

private:
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
};