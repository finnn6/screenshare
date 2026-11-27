#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include "nvEncodeAPI.h"

struct EncoderSettings {
    UINT width = 1920;
    UINT height = 1080;
    UINT fps = 60;
    UINT bitrateKbps = 20000;   // 최대 비트레이트 (상한)
    UINT quality = 18;          // 목표 화질 (0~51, 낮을수록 고화질)
};

// NVENC 하드웨어 인코더를 담당
class Encoder {
public:
    ~Encoder() { Shutdown(); }

    bool Init(ID3D11Device* device);
    bool Start(const EncoderSettings& settings);
    void Stop();
    void Shutdown();

    bool IsStarted() const { return m_encoder != nullptr; }
    ID3D11RenderTargetView* InputRTV() const { return m_inputRTV.Get(); }
    ID3D11Texture2D* InputTexture() const { return m_inputTexture.Get(); }
    const EncoderSettings& Settings() const { return m_settings; }

    void RequestKeyframe() { m_forceIdr = true; }
    bool EncodeFrame(std::vector<uint8_t>& out, bool& isKeyframe);

private:
    bool OpenSession();

    HMODULE m_lib = nullptr;
    NV_ENCODE_API_FUNCTION_LIST m_nvenc = { NV_ENCODE_API_FUNCTION_LIST_VER };
    void* m_encoder = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_inputTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_inputRTV;
    NV_ENC_REGISTERED_PTR m_registered = nullptr;
    NV_ENC_OUTPUT_PTR m_bitstream = nullptr;

    EncoderSettings m_settings;
    uint64_t m_frameIndex = 0;
    bool m_forceIdr = false;
};