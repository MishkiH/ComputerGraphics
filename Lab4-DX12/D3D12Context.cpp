#include "D3D12Context.h"
#include <stdexcept>
#include <cstdio>

using Microsoft::WRL::ComPtr;

static void ThrowIfFailed(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s (hr=0x%08X)", what, (unsigned)hr);
        throw std::runtime_error(buf);
    }
}

bool D3D12Context::Initialize(HWND hwnd, uint32_t width, uint32_t height)
{
    m_initialized = false;

    m_hwnd = hwnd;
    m_width = width;
    m_height = height;

#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();
    }
#endif

    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&m_factory)), "CreateDXGIFactory1");

    CreateDevice();

    CreateCommandObjects();

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "CreateFence");
    m_fenceValue = 0;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
        throw std::runtime_error("CreateEvent for fence failed");

    CreateSwapChain();

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_dsvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    CreateDescriptorHeaps();
    CreateRtvForBackBuffers();
    CreateDepthStencil();

    m_viewport = { 0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f };
    m_scissorRect = { 0, 0, (LONG)m_width, (LONG)m_height };

    m_initialized = true;
    return true;
}

void D3D12Context::Shutdown()
{
    if (m_cmdQueue) FlushCommandQueue();

    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
}

bool D3D12Context::CreateDevice()
{
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
    if (FAILED(hr))
    {
        ComPtr<IDXGIAdapter> warp;
        ThrowIfFailed(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "EnumWarpAdapter");
        ThrowIfFailed(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)), "D3D12CreateDevice (WARP)");
    }
    return true;
}

bool D3D12Context::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(m_device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&m_cmdQueue)), "CreateCommandQueue");

    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmdAlloc)), "CreateCommandAllocator");
    ThrowIfFailed(m_device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_cmdAlloc.Get(),
        nullptr,
        IID_PPV_ARGS(&m_cmdList)
    ), "CreateCommandList");

    ThrowIfFailed(m_cmdList->Close(), "CommandList Close (initial)");
    return true;
}

bool D3D12Context::CreateSwapChain()
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Width = m_width;
    sd.BufferDesc.Height = m_height;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kSwapChainBufferCount;
    sd.OutputWindow = m_hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = 0;

    m_swapChain.Reset();

    ThrowIfFailed(m_factory->CreateSwapChain(
        m_cmdQueue.Get(),
        &sd,
        m_swapChain.GetAddressOf()
    ), "CreateSwapChain");

    m_currBackBuffer = 0;
    return true;
}

bool D3D12Context::CreateDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.NumDescriptors = kSwapChainBufferCount;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)), "Create RTV Heap");

    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
    dsvDesc.NumDescriptors = 1;
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&m_dsvHeap)), "Create DSV Heap");

    return true;
}

bool D3D12Context::CreateRtvForBackBuffers()
{
    auto rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (uint32_t i = 0; i < kSwapChainBufferCount; ++i)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_swapChainBuffers[i])), "SwapChain GetBuffer");
        m_device->CreateRenderTargetView(m_swapChainBuffers[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }
    return true;
}

bool D3D12Context::CreateDepthStencil()
{
    m_depthStencilBuffer.Reset();

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE optClear{};
    optClear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    optClear.DepthStencil.Depth = 1.0f;
    optClear.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COMMON,
        &optClear,
        IID_PPV_ARGS(&m_depthStencilBuffer)
    ), "CreateCommittedResource (DepthStencil)");

    ThrowIfFailed(m_cmdAlloc->Reset(), "CmdAlloc Reset (DepthStencil)");
    ThrowIfFailed(m_cmdList->Reset(m_cmdAlloc.Get(), nullptr), "CmdList Reset (DepthStencil)");

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_depthStencilBuffer.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(m_cmdList->Close(), "CmdList Close (DepthStencil)");
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);
    FlushCommandQueue();

    m_device->CreateDepthStencilView(
        m_depthStencilBuffer.Get(),
        nullptr,
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart()
    );

    return true;
}

void D3D12Context::OnResize(uint32_t width, uint32_t height)
{
    if (!m_initialized) return;
    if (width == 0 || height == 0) return;

    m_width = width;
    m_height = height;

    FlushCommandQueue();

    for (auto& b : m_swapChainBuffers) b.Reset();
    m_depthStencilBuffer.Reset();

    ThrowIfFailed(m_swapChain->ResizeBuffers(
        kSwapChainBufferCount,
        m_width,
        m_height,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        0
    ), "SwapChain ResizeBuffers");

    m_currBackBuffer = 0;

    CreateRtvForBackBuffers();
    CreateDepthStencil();

    m_viewport = { 0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f };
    m_scissorRect = { 0, 0, (LONG)m_width, (LONG)m_height };
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Context::CurrentBackBufferRTV() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (size_t)m_currBackBuffer * m_rtvDescriptorSize;
    return h;
}

ID3D12Resource* D3D12Context::CurrentBackBuffer() const
{
    return m_swapChainBuffers[m_currBackBuffer].Get();
}

void D3D12Context::Draw()
{
    ThrowIfFailed(m_cmdAlloc->Reset(), "CmdAlloc Reset");
    ThrowIfFailed(m_cmdList->Reset(m_cmdAlloc.Get(), nullptr), "CmdList Reset");

    m_cmdList->RSSetViewports(1, &m_viewport);
    m_cmdList->RSSetScissorRects(1, &m_scissorRect);

    D3D12_RESOURCE_BARRIER toRT{};
    toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRT.Transition.pResource = CurrentBackBuffer();
    toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRT.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &toRT);

    auto rtv = CurrentBackBufferRTV();
    auto dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_cmdList->OMSetRenderTargets(1, &rtv, TRUE, &dsv);

    const float clearColor[4] = { 0.10f, 0.10f, 0.35f, 1.0f };
    m_cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_RESOURCE_BARRIER toPresent = toRT;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    m_cmdList->ResourceBarrier(1, &toPresent);

    ThrowIfFailed(m_cmdList->Close(), "CmdList Close");

    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);

    ThrowIfFailed(m_swapChain->Present(1, 0), "SwapChain Present");
    m_currBackBuffer = (m_currBackBuffer + 1) % kSwapChainBufferCount;

    FlushCommandQueue();
}

void D3D12Context::FlushCommandQueue()
{
    const uint64_t fenceToWaitFor = ++m_fenceValue;
    ThrowIfFailed(m_cmdQueue->Signal(m_fence.Get(), fenceToWaitFor), "Fence Signal");

    if (m_fence->GetCompletedValue() < fenceToWaitFor)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fenceToWaitFor, m_fenceEvent), "Fence SetEventOnCompletion");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}
