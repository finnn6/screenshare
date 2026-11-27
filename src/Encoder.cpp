#include "Encoder.h"
#include <iostream>

typedef NVENCSTATUS(NVENCAPI* PFN_CreateInstance)(NV_ENCODE_API_FUNCTION_LIST*);
typedef NVENCSTATUS(NVENCAPI* PFN_GetMaxVersion)(uint32_t*);

bool Encoder::Init(ID3D11Device* device) {
    m_device = device;

    m_lib = LoadLibrary(L"nvEncodeAPI64.dll");
    if (!m_lib) {
        std::cout << "nvEncodeAPI64.dll을 찾을 수 없음 (NVIDIA 드라이버 확인)\n";
        return false;
    }

    auto getMaxVersion = reinterpret_cast<PFN_GetMaxVersion>(
        GetProcAddress(m_lib, "NvEncodeAPIGetMaxSupportedVersion"));
    auto createInstance = reinterpret_cast<PFN_CreateInstance>(
        GetProcAddress(m_lib, "NvEncodeAPICreateInstance"));
    if (!getMaxVersion || !createInstance) {
        std::cout << "NVENC 함수를 찾을 수 없음\n";
        return false;
    }

    uint32_t driverVersion = 0;
    getMaxVersion(&driverVersion);
    uint32_t headerVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (headerVersion > driverVersion) {
        std::cout << "드라이버가 헤더보다 오래됨. 그래픽 드라이버를 업데이트하세요\n";
        return false;
    }

    if (createInstance(&m_nvenc) != NV_ENC_SUCCESS) {
        std::cout << "NVENC 함수 목록 생성 실패\n";
        return false;
    }
    return true;
}

bool Encoder::OpenSession() {
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    params.device = m_device.Get();
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.apiVersion = NVENCAPI_VERSION;

    NVENCSTATUS st = m_nvenc.nvEncOpenEncodeSessionEx(&params, &m_encoder);
    if (st != NV_ENC_SUCCESS) {
        std::cout << "인코딩 세션 열기 실패: " << st << "\n";
        m_encoder = nullptr;
        return false;
    }
    return true;
}

bool Encoder::Start(const EncoderSettings& s) {
    Stop();
    if (!m_lib) return false;
    if (!OpenSession()) return false;
    m_settings = s;

    // 1) 프리셋 가져오기
    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    NVENCSTATUS st = m_nvenc.nvEncGetEncodePresetConfigEx(
        m_encoder, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID,
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset);
    if (st != NV_ENC_SUCCESS) {
        std::cout << "프리셋 가져오기 실패: " << st << "\n";
        Stop();
        return false;
    }

    // 2) 실시간 공유에 맞게 고치기
    NV_ENC_CONFIG config = preset.presetCfg;
    config.gopLength = NVENC_INFINITE_GOPLENGTH;   // 키프레임은 필요할 때만
    config.frameIntervalP = 1;                     // B프레임 없음

    // 목표 화질 + 상한: 평소엔 quality를 유지, 상한을 넘을 것 같을 때만 화질을 낮춤
    uint32_t maxBits = s.bitrateKbps * 1000;
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    config.rcParams.averageBitRate = 0;            // 0 = 평균 목표 없음 (화질 기준으로 동작)
    config.rcParams.targetQuality = (uint8_t)s.quality;
    config.rcParams.maxBitRate = maxBits;
    // 순간적으로 몰려도 되는 양. 크면 키프레임이 한꺼번에 커져서 네트워크에서 손실이 나기 쉽다.
    // 0.1초 분량으로 제한해서 키프레임도 상한 근처 크기로 나오게 한다.
    config.rcParams.vbvBufferSize = maxBits / 10;
    config.rcParams.vbvInitialDelay = config.rcParams.vbvBufferSize;

    config.encodeCodecConfig.h264Config.idrPeriod = config.gopLength;
    config.encodeCodecConfig.h264Config.repeatSPSPPS = 1;

    NV_ENC_INITIALIZE_PARAMS init = { NV_ENC_INITIALIZE_PARAMS_VER };
    init.encodeGUID = NV_ENC_CODEC_H264_GUID;
    init.presetGUID = NV_ENC_PRESET_P4_GUID;
    init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init.encodeWidth = s.width;
    init.encodeHeight = s.height;
    init.darWidth = s.width;
    init.darHeight = s.height;
    init.frameRateNum = s.fps;
    init.frameRateDen = 1;
    init.enablePTD = 1;
    init.encodeConfig = &config;

    st = m_nvenc.nvEncInitializeEncoder(m_encoder, &init);
    if (st != NV_ENC_SUCCESS) {
        std::cout << "인코더 초기화 실패: " << st << "\n";
        Stop();
        return false;
    }

    // 3) 입력 텍스처
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = s.width;
    td.Height = s.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_inputTexture))) {
        std::cout << "인코더 입력 텍스처 생성 실패\n";
        Stop();
        return false;
    }
    m_device->CreateRenderTargetView(m_inputTexture.Get(), nullptr, &m_inputRTV);

    // 4) NVENC에 등록
    NV_ENC_REGISTER_RESOURCE reg = { NV_ENC_REGISTER_RESOURCE_VER };
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.resourceToRegister = m_inputTexture.Get();
    reg.width = s.width;
    reg.height = s.height;
    reg.pitch = 0;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    st = m_nvenc.nvEncRegisterResource(m_encoder, &reg);
    if (st != NV_ENC_SUCCESS) {
        std::cout << "입력 텍스처 등록 실패: " << st << "\n";
        Stop();
        return false;
    }
    m_registered = reg.registeredResource;

    // 5) 출력 버퍼
    NV_ENC_CREATE_BITSTREAM_BUFFER bb = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
    st = m_nvenc.nvEncCreateBitstreamBuffer(m_encoder, &bb);
    if (st != NV_ENC_SUCCESS) {
        std::cout << "출력 버퍼 생성 실패: " << st << "\n";
        Stop();
        return false;
    }
    m_bitstream = bb.bitstreamBuffer;

    m_frameIndex = 0;
    m_forceIdr = true;
    return true;
}

void Encoder::Stop() {
    if (m_encoder) {
        if (m_bitstream)  { m_nvenc.nvEncDestroyBitstreamBuffer(m_encoder, m_bitstream); m_bitstream = nullptr; }
        if (m_registered) { m_nvenc.nvEncUnregisterResource(m_encoder, m_registered);    m_registered = nullptr; }
        m_nvenc.nvEncDestroyEncoder(m_encoder);
        m_encoder = nullptr;
    }
    m_inputRTV.Reset();
    m_inputTexture.Reset();
}

void Encoder::Shutdown() {
    Stop();
    m_device.Reset();
    if (m_lib) {
        FreeLibrary(m_lib);
        m_lib = nullptr;
    }
}

bool Encoder::EncodeFrame(std::vector<uint8_t>& out, bool& isKeyframe) {
    if (!m_registered || !m_bitstream) return false;

    NV_ENC_MAP_INPUT_RESOURCE map = { NV_ENC_MAP_INPUT_RESOURCE_VER };
    map.registeredResource = m_registered;
    if (m_nvenc.nvEncMapInputResource(m_encoder, &map) != NV_ENC_SUCCESS) return false;

    NV_ENC_PIC_PARAMS pic = { NV_ENC_PIC_PARAMS_VER };
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.inputWidth = m_settings.width;
    pic.inputHeight = m_settings.height;
    pic.outputBitstream = m_bitstream;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = m_frameIndex++;
    if (m_forceIdr) {
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        m_forceIdr = false;
    }

    bool ok = false;
    NVENCSTATUS st = m_nvenc.nvEncEncodePicture(m_encoder, &pic);
    if (st == NV_ENC_SUCCESS) {
        NV_ENC_LOCK_BITSTREAM lock = { NV_ENC_LOCK_BITSTREAM_VER };
        lock.outputBitstream = m_bitstream;
        if (m_nvenc.nvEncLockBitstream(m_encoder, &lock) == NV_ENC_SUCCESS) {
            auto data = static_cast<uint8_t*>(lock.bitstreamBufferPtr);
            out.assign(data, data + lock.bitstreamSizeInBytes);
            isKeyframe = (lock.pictureType == NV_ENC_PIC_TYPE_IDR);
            m_nvenc.nvEncUnlockBitstream(m_encoder, m_bitstream);
            ok = true;
        }
    }
    else {
        std::cout << "인코딩 실패: " << st << "\n";
    }

    m_nvenc.nvEncUnmapInputResource(m_encoder, map.mappedResource);
    return ok;
}