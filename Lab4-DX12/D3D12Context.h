#pragma once

#include <windows.h>
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>

class D3D12Context
{
public:
    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();

    void OnResize(uint32_t width, uint32_t height);
    void Draw();

private:
    bool CreateDevice();
    bool CreateCommandObjects();
    bool CreateSwapChain();
    bool CreateDescriptorHeaps();
    bool CreateRtvForBackBuffers();
    bool CreateDepthStencil();
    void FlushCommandQueue();

    D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferRTV() const;
    ID3D12Resource* CurrentBackBuffer() const;

private:
    static constexpr uint32_t kSwapChainBufferCount = 2;

    bool m_initialized = false;

    HWND m_hwnd = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    Microsoft::WRL::ComPtr<IDXGIFactory4> m_factory;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;

    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_cmdQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_cmdAlloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmdList;

    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    uint64_t m_fenceValue = 0;
    HANDLE m_fenceEvent = nullptr;

    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_swapChainBuffers[kSwapChainBufferCount];
    uint32_t m_currBackBuffer = 0;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;

    uint32_t m_rtvDescriptorSize = 0;
    uint32_t m_dsvDescriptorSize = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthStencilBuffer;

    D3D12_VIEWPORT m_viewport{};
    D3D12_RECT m_scissorRect{};
};
