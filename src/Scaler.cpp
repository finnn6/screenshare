#include "Scaler.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

using Microsoft::WRL::ComPtr;

static const char* kShaderSource = R"(
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD;
};

VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2(id & 1, id >> 1);
    o.uv  = uv;
    o.pos = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
    return o;
}

Texture2D    tex  : register(t0);
SamplerState samp : register(s0);

float4 ps_main(VSOut i) : SV_TARGET {
    return tex.Sample(samp, i.uv);
}
)";

static ComPtr<ID3DBlob> CompileShader(const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3DCompile(kShaderSource, strlen(kShaderSource), nullptr, nullptr, nullptr,
                            entry, target, 0, 0, &blob, &error);
    if (FAILED(hr)) {
        if (error) std::cout << (const char*)error->GetBufferPointer() << "\n";
        return nullptr;
    }
    return blob;
}

bool Scaler::Init(ID3D11Device* device) {
    auto vsBlob = CompileShader("vs_main", "vs_5_0");
    auto psBlob = CompileShader("ps_main", "ps_5_0");
    if (!vsBlob || !psBlob) {
        std::cout << "셰이더 컴파일 실패\n";
        return false;
    }
    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vs);
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_ps);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, &m_sampler);
    return true;
}

void Scaler::DrawFit(ID3D11DeviceContext* context,
                     ID3D11RenderTargetView* target, UINT targetW, UINT targetH,
                     ID3D11ShaderResourceView* srv, UINT srcW, UINT srcH, bool allowUpscale) {
    float black[4] = { 0, 0, 0, 1 };
    context->OMSetRenderTargets(1, &target, nullptr);
    context->ClearRenderTargetView(target, black);

    if (srv && srcW > 0 && srcH > 0) {
        float scale = std::min((float)targetW / srcW, (float)targetH / srcH);
        if (!allowUpscale) scale = std::min(scale, 1.0f);
        float drawW = std::floor(srcW * scale);
        float drawH = std::floor(srcH * scale);

        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = std::floor((targetW - drawW) / 2);
        vp.TopLeftY = std::floor((targetH - drawH) / 2);
        vp.Width = drawW;
        vp.Height = drawH;
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);

        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->IASetInputLayout(nullptr);
        context->VSSetShader(m_vs.Get(), nullptr, 0);
        context->PSSetShader(m_ps.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, &srv);
        context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
        context->Draw(4, 0);
    }

    // 다 그렸으면 연결을 풀어둠
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->PSSetShaderResources(0, 1, &nullSrv);
    context->OMSetRenderTargets(0, nullptr, nullptr);
}