#include "PreviewShare.h"
#include <iostream>

using Microsoft::WRL::ComPtr;

PreviewShare::~PreviewShare() {
    ReleaseReader();
    for (HANDLE& h : m_handles) {
        if (h) {
            CloseHandle(h);
            h = nullptr;
        }
    }
}

// ---------- 쓰기 쪽 ----------
void PreviewShare::CreateWriterSlots(ID3D11Device* device, UINT width, UINT height) {
    for (int i = 0; i < kSlots; ++i) {
        m_writeTex[i].Reset();
        m_writeMutex[i].Reset();
    }
    m_writeW = width;    // 실패해도 같은 크기로 계속 재시도하지 않게 먼저 기록
    m_writeH = height;
    m_writeNext = 0;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    HANDLE newHandles[kSlots] = {};
    for (int i = 0; i < kSlots; ++i) {
        if (FAILED(device->CreateTexture2D(&td, nullptr, &m_writeTex[i]))) {
            std::cout << "공유 텍스처 생성 실패\n";
            for (int j = 0; j < kSlots; ++j) {
                if (newHandles[j]) CloseHandle(newHandles[j]);
                m_writeTex[j].Reset();
                m_writeMutex[j].Reset();
            }
            return;
        }
        m_writeTex[i].As(&m_writeMutex[i]);

        ComPtr<IDXGIResource1> resource;
        m_writeTex[i].As(&resource);
        resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                     nullptr, &newHandles[i]);
    }

    // 새 열쇠로 교체하고, 읽기 쪽에 "새로 열어라"고 알림
    std::lock_guard<std::mutex> guard(m_lock);
    for (int i = 0; i < kSlots; ++i) {
        if (m_handles[i]) CloseHandle(m_handles[i]);
        m_handles[i] = newHandles[i];
    }
    m_width = width;
    m_height = height;
    m_generation++;
    m_latest = -1;
}

void PreviewShare::Publish(ID3D11Device* device, ID3D11DeviceContext* context,
                           ID3D11Texture2D* source, UINT width, UINT height) {
    if (width != m_writeW || height != m_writeH) {
        CreateWriterSlots(device, width, height);
    }

    int slot = m_writeNext;
    if (!m_writeTex[slot]) return;

    // 미리보기가 마침 이 칸을 잡고 있으면 이번 화면은 건너뜀 (기다리지 않음)
    if (m_writeMutex[slot]->AcquireSync(0, 0) != S_OK) return;
    context->CopyResource(m_writeTex[slot].Get(), source);
    m_writeMutex[slot]->ReleaseSync(0);

    m_writeNext = (slot + 1) % kSlots;

    std::lock_guard<std::mutex> guard(m_lock);
    m_latest = slot;
}

void PreviewShare::ReleaseWriter() {
    for (int i = 0; i < kSlots; ++i) {
        m_writeTex[i].Reset();
        m_writeMutex[i].Reset();
    }
    m_writeW = 0;
    m_writeH = 0;
}

// ---------- 읽기 쪽 ----------
ID3D11ShaderResourceView* PreviewShare::BeginRead(ID3D11Device* device, UINT& width, UINT& height) {
    int slot = -1;
    {
        std::lock_guard<std::mutex> guard(m_lock);

        // 공유 텍스처가 새로 만들어졌으면 다시 열기
        if (m_generation != m_readGeneration) {
            ReleaseReader();
            ComPtr<ID3D11Device1> device1;
            device->QueryInterface(IID_PPV_ARGS(&device1));
            for (int i = 0; i < kSlots; ++i) {
                if (!m_handles[i] || !device1) continue;
                ComPtr<ID3D11Texture2D> tex;
                if (FAILED(device1->OpenSharedResource1(m_handles[i], IID_PPV_ARGS(&tex)))) continue;
                tex.As(&m_readMutex[i]);
                device->CreateShaderResourceView(tex.Get(), nullptr, &m_readSrv[i]);
            }
            m_readGeneration = m_generation;
        }

        slot = m_latest;
        width = m_width;
        height = m_height;
    }

    if (slot < 0 || !m_readMutex[slot] || !m_readSrv[slot]) return nullptr;
    if (m_readMutex[slot]->AcquireSync(0, 0) != S_OK) return nullptr;
    m_reading = slot;
    return m_readSrv[slot].Get();
}

void PreviewShare::EndRead() {
    if (m_reading >= 0) {
        m_readMutex[m_reading]->ReleaseSync(0);
        m_reading = -1;
    }
}

void PreviewShare::ReleaseReader() {
    EndRead();
    for (int i = 0; i < kSlots; ++i) {
        m_readMutex[i].Reset();
        m_readSrv[i].Reset();
    }
}