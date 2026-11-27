#pragma once
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <mutex>

// 인코딩 스레드(쓰기)와 미리보기(읽기)가 서로 다른 D3D11 장치에서
// 같은 화면을 주고받기 위한 공유 버퍼. 여러 장을 돌려가며 씀
class PreviewShare {
public:
    static const int kSlots = 4;
    ~PreviewShare();

    // ---------- 인코딩 스레드 쪽 ----------
    // source 텍스처를 공유 버퍼의 빈 칸에 복사
    void Publish(ID3D11Device* device, ID3D11DeviceContext* context,
                 ID3D11Texture2D* source, UINT width, UINT height);
    void ReleaseWriter();

    // ---------- 미리보기(메인 스레드) 쪽 ----------
    // 가장 최근 화면을 읽기 시작. 없거나 사용 중이면 nullptr
    ID3D11ShaderResourceView* BeginRead(ID3D11Device* device, UINT& width, UINT& height);
    void EndRead();
    void ReleaseReader();

private:
    void CreateWriterSlots(ID3D11Device* device, UINT width, UINT height);

    // 두 스레드가 같이 보는 정보 (m_lock으로 보호)
    std::mutex m_lock;
    HANDLE m_handles[kSlots] = {};   // 상대 장치가 열 때 쓰는 열쇠
    UINT m_width = 0;
    UINT m_height = 0;
    uint64_t m_generation = 0;       // 공유 텍스처를 새로 만들 때마다 +1
    int m_latest = -1;               // 가장 최근에 다 쓴 칸 (-1이면 아직 없음)

    // 쓰기 쪽만 사용
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_writeTex[kSlots];
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> m_writeMutex[kSlots];
    UINT m_writeW = 0;
    UINT m_writeH = 0;
    int m_writeNext = 0;

    // 읽기 쪽만 사용
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> m_readMutex[kSlots];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_readSrv[kSlots];
    uint64_t m_readGeneration = 0;
    int m_reading = -1;
};