#include "D3D12Context.h"
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <array>
#include <cmath>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <wincodec.h>
#include <objbase.h>
#include <DirectXMath.h>
using namespace DirectX;

using Microsoft::WRL::ComPtr;

static std::wstring ToWStringAscii(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

static std::string Dirname(const std::string& path)
{
    size_t p = path.find_last_of("/\\");
    return (p == std::string::npos) ? std::string() : path.substr(0, p + 1);
}

static std::string JoinPath(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + "/" + b;
}

struct WicImage
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;
};

static bool LoadImageWIC(const std::wstring& filePath, WicImage& out)
{
    static bool s_comInitTried = false;
    if (!s_comInitTried)
    {
        s_comInitTried = true;
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        (void)hrCo;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return false;

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0) return false;

    ComPtr<IWICFormatConverter> conv;
    hr = factory->CreateFormatConverter(&conv);
    if (FAILED(hr)) return false;

    hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                          WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return false;

    out.width = (uint32_t)w;
    out.height = (uint32_t)h;
    out.bgra.resize((size_t)w * (size_t)h * 4);

    const UINT stride = w * 4;
    hr = conv->CopyPixels(nullptr, stride, (UINT)out.bgra.size(), out.bgra.data());
    return SUCCEEDED(hr);
}

static D3D12_RESOURCE_DESC Tex2DDesc(uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Alignment = 0;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    return d;
}

struct ObjKey
{
    int p = -1;
    int t = -1;
    int n = -1;
    bool operator==(const ObjKey& o) const { return p==o.p && t==o.t && n==o.n; }
};
struct ObjKeyHash
{
    size_t operator()(const ObjKey& k) const noexcept
    {
        return (size_t)k.p * 73856093u ^ (size_t)k.t * 19349663u ^ (size_t)k.n * 83492791u;
    }
};

static int FixObjIndex(int idx, int size)
{
    if (idx > 0) return idx - 1;
    if (idx < 0) return size + idx;
    return -1;
}

static void ParseFaceToken(const std::string& tok, int& p, int& t, int& n)
{
    p = t = n = 0;
    size_t s1 = tok.find('/');
    if (s1 == std::string::npos)
    {
        p = std::stoi(tok);
        return;
    }

    if (s1 > 0) p = std::stoi(tok.substr(0, s1));

    size_t s2 = tok.find('/', s1 + 1);
    if (s2 == std::string::npos)
    {
        // v/vt
        if (s1 + 1 < tok.size()) t = std::stoi(tok.substr(s1 + 1));
        return;
    }

    // v//vn or v/vt/vn
    if (s2 > s1 + 1) t = std::stoi(tok.substr(s1 + 1, s2 - (s1 + 1)));
    if (s2 + 1 < tok.size()) n = std::stoi(tok.substr(s2 + 1));
}

static std::unordered_map<std::string, std::string> LoadMtlMapKd(const std::string& mtlPath)
{
    std::unordered_map<std::string, std::string> out;
    std::ifstream f(mtlPath);
    if (!f.is_open()) return out;

    std::string baseDir = Dirname(mtlPath);

    std::string line;
    std::string cur;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;
        if (cmd == "newmtl")
        {
            ss >> cur;
        }
        else if (cmd == "map_Kd" && !cur.empty())
        {
            std::string tok, last;
            while (ss >> tok) last = tok;
            if (!last.empty())
                out[cur] = JoinPath(baseDir, last);
        }
    }
    return out;
}

struct ObjLoaded
{
    std::vector<D3D12Context::Vertex> vertices;
    std::vector<uint32_t> indices;

    struct Group { uint32_t start = 0; uint32_t count = 0; std::string mtl; };
    std::vector<Group> groups;

    std::unordered_map<std::string, std::string> mtlToDiffuse;
};

static bool LoadObjWithGroups(const std::string& objPath, ObjLoaded& out)
{
    std::ifstream file(objPath);
    if (!file.is_open()) return false;

    std::string baseDir = Dirname(objPath);

    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<DirectX::XMFLOAT3> normals;
    std::vector<DirectX::XMFLOAT2> texcoords;

    positions.reserve(200000);
    normals.reserve(200000);
    texcoords.reserve(200000);

    std::unordered_map<ObjKey, uint32_t, ObjKeyHash> uniqueMap;

    std::string mtlLib;
    std::string curMtl;

    auto beginGroupIfNeeded = [&]()
    {
        if (out.groups.empty())
        {
            ObjLoaded::Group g{};
            g.start = (uint32_t)out.indices.size();
            g.mtl = curMtl;
            out.groups.push_back(g);
        }
        else
        {}
    };

    auto switchMaterial = [&](const std::string& newMtl)
    {
        if (out.groups.empty())
        {
            curMtl = newMtl;
            ObjLoaded::Group g{};
            g.start = (uint32_t)out.indices.size();
            g.mtl = curMtl;
            out.groups.push_back(g);
            return;
        }

        if (curMtl == newMtl) return;

        out.groups.back().count = (uint32_t)out.indices.size() - out.groups.back().start;

        curMtl = newMtl;
        ObjLoaded::Group g{};
        g.start = (uint32_t)out.indices.size();
        g.mtl = curMtl;
        out.groups.push_back(g);
    };

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#') continue;

        // mtllib
        if (line.rfind("mtllib ", 0) == 0)
        {
            std::istringstream ss(line);
            std::string cmd, name;
            ss >> cmd >> name;
            mtlLib = JoinPath(baseDir, name);
            continue;
        }

        // usemtl
        if (line.rfind("usemtl ", 0) == 0)
        {
            std::istringstream ss(line);
            std::string cmd, name;
            ss >> cmd >> name;
            switchMaterial(name);
            continue;
        }

        // v
        if (line.size() > 2 && line[0]=='v' && line[1]==' ')
        {
            std::istringstream ss(line);
            char v; float x,y,z;
            ss >> v >> x >> y >> z;
            positions.push_back({x,y,z});
            continue;
        }

        // vn
        if (line.size() > 3 && line[0]=='v' && line[1]=='n' && line[2]==' ')
        {
            std::istringstream ss(line);
            std::string vn; float x,y,z;
            ss >> vn >> x >> y >> z;
            normals.push_back({x,y,z});
            continue;
        }

        // vt
        if (line.size() > 3 && line[0]=='v' && line[1]=='t' && line[2]==' ')
        {
            std::istringstream ss(line);
            std::string vt; float u,v;
            ss >> vt >> u >> v;
            texcoords.push_back({u, 1.0f - v});
            continue;
        }

        if (line.size() > 2 && line[0]=='f' && line[1]==' ')
        {
            beginGroupIfNeeded();

            std::istringstream ss(line);
            char fch; ss >> fch;

            std::vector<uint32_t> face;
            face.reserve(8);

            std::string tok;
            while (ss >> tok)
            {
                int pRaw=0,tRaw=0,nRaw=0;
                ParseFaceToken(tok, pRaw, tRaw, nRaw);

                int p = FixObjIndex(pRaw, (int)positions.size());
                int t = FixObjIndex(tRaw, (int)texcoords.size());
                int n = FixObjIndex(nRaw, (int)normals.size());

                if (p < 0) continue;

                ObjKey key{p,t,n};
                auto it = uniqueMap.find(key);
                if (it == uniqueMap.end())
                {
                    D3D12Context::Vertex v{};
                    v.Pos = positions[p];
                    v.Normal = (n>=0) ? normals[n] : DirectX::XMFLOAT3(0,1,0);
                    v.TexC = (t>=0) ? texcoords[t] : DirectX::XMFLOAT2(0,0);

                    uint32_t idx = (uint32_t)out.vertices.size();
                    out.vertices.push_back(v);
                    uniqueMap.emplace(key, idx);
                    face.push_back(idx);
                }
                else face.push_back(it->second);
            }

            if (face.size() >= 3)
            {
                for (size_t i=1; i+1<face.size(); ++i)
                {
                    out.indices.push_back(face[0]);
                    out.indices.push_back(face[i]);
                    out.indices.push_back(face[i+1]);
                }
            }
        }
    }

    if (!out.groups.empty())
        out.groups.back().count = (uint32_t)out.indices.size() - out.groups.back().start;
    if (!mtlLib.empty())
        out.mtlToDiffuse = LoadMtlMapKd(mtlLib);

    return !out.vertices.empty() && !out.indices.empty();
}


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
    m_cbvSrvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CreateDescriptorHeaps();
    CreateRtvForBackBuffers();
    CreateDepthStencil();

    m_viewport = { 0.f, 0.f, (float)m_width, (float)m_height, 0.f, 1.f };
    m_scissorRect = { 0, 0, (LONG)m_width, (LONG)m_height };

    XMStoreFloat4x4(&m_world, XMMatrixScaling(0.01f, 0.01f, 0.01f));

    XMVECTOR eye = XMVectorSet(m_eyePos.x, m_eyePos.y, m_eyePos.z, 1.f);
    XMVECTOR target = XMVectorZero();
    XMVECTOR up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
    XMStoreFloat4x4(&m_view, XMMatrixLookAtLH(eye, target, up));

    float aspect = (m_height > 0) ? ((float)m_width / (float)m_height) : 1.f;
    XMStoreFloat4x4(&m_proj, XMMatrixPerspectiveFovLH(0.25f * XM_PI, aspect, 1.0f, 1000.0f));

    BuildShaders();
    BuildGeometry();
    BuildConstantBuffer();
    BuildRootSignature();
    BuildPSO();

    m_initialized = true;
    return true;
}

void D3D12Context::Shutdown()
{
    if (m_cmdQueue) FlushCommandQueue();

    if (m_objectCB && m_mappedObjectCB)
    {
        m_objectCB->Unmap(0, nullptr);
        m_mappedObjectCB = nullptr;
    }

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
        0,D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmdAlloc.Get(),
        nullptr, IID_PPV_ARGS(&m_cmdList)), "CreateCommandList");

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

    ThrowIfFailed(m_factory->CreateSwapChain(m_cmdQueue.Get(), &sd,
        m_swapChain.GetAddressOf()), "CreateSwapChain");

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
        m_depthStencilBuffer.Get(), nullptr,
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
        kSwapChainBufferCount, m_width, m_height,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0), "SwapChain ResizeBuffers");

    m_currBackBuffer = 0;

    CreateRtvForBackBuffers();
    CreateDepthStencil();

    m_viewport = { 0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f };
    m_scissorRect = { 0, 0, (LONG)m_width, (LONG)m_height };

    float aspect = (m_height > 0) ? ((float)m_width / (float)m_height) : 1.0f;
    XMStoreFloat4x4(&m_proj, XMMatrixPerspectiveFovLH(0.25f * XM_PI, aspect, 1.0f, 1000.0f));
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
    if (!m_initialized) return;

    UpdateConstantBuffer();

    ThrowIfFailed(m_cmdAlloc->Reset(), "CmdAlloc Reset");
    ThrowIfFailed(m_cmdList->Reset(m_cmdAlloc.Get(), m_pso.Get()), "CmdList Reset");

    m_cmdList->RSSetViewports(1, &m_viewport);
    m_cmdList->RSSetScissorRects(1, &m_scissorRect);

    m_cmdList->SetGraphicsRootSignature(m_rootSignature.Get());

    D3D12_RESOURCE_BARRIER toRT{};
    toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRT.Transition.pResource = CurrentBackBuffer();
    toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &toRT);

    auto rtv = CurrentBackBufferRTV();
    auto dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_cmdList->OMSetRenderTargets(1, &rtv, TRUE, &dsv);

    const float clearColor[4] = { 0.10f, 0.10f, 0.35f, 1.0f };
    m_cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    ID3D12DescriptorHeap* heaps[] = { m_cbvHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, heaps);

    auto base = m_cbvHeap->GetGPUDescriptorHandleForHeapStart();
    m_cmdList->SetGraphicsRootDescriptorTable(0, base);

    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmdList->IASetVertexBuffers(0, 1, &m_vbv);
    m_cmdList->IASetIndexBuffer(&m_ibv);

    for (const auto& di : m_drawItems)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE srv = base;
        srv.ptr += (UINT64)di.TextureSrvIndex * (UINT64)m_cbvSrvUavDescriptorSize;
        m_cmdList->SetGraphicsRootDescriptorTable(1, srv);

        m_cmdList->DrawIndexedInstanced(di.IndexCount, 1, di.StartIndexLocation, 0, 0);
    }

    D3D12_RESOURCE_BARRIER toPresent = toRT;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_cmdList->ResourceBarrier(1, &toPresent);

    ThrowIfFailed(m_cmdList->Close(), "CmdList Close");

    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);

    ThrowIfFailed(m_swapChain->Present(1, 0), "SwapChain Present");
    m_currBackBuffer = (m_currBackBuffer + 1) % kSwapChainBufferCount;

    FlushCommandQueue();
}


void D3D12Context::SetCamera(const DirectX::XMFLOAT3& eyePos, float yaw, float pitch)
{
    using namespace DirectX;

    m_eyePos = eyePos;

    const float cy = cosf(yaw);
    const float sy = sinf(yaw);
    const float cp = cosf(pitch);
    const float sp = sinf(pitch);

    XMVECTOR forward = XMVector3Normalize(XMVectorSet(sy*cp, sp, cy*cp, 0.0f));
    XMVECTOR eye = XMVectorSet(eyePos.x, eyePos.y, eyePos.z, 1.0f);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMStoreFloat4x4(&m_view, XMMatrixLookToLH(eye, forward, up));
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

static uint32_t AlignConstantBufferByteSize(uint32_t byteSize)
{
    return (byteSize + 255u) & ~255u;
}

static D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;
    p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

static D3D12_RESOURCE_DESC BufferDesc(UINT64 byteSize)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = byteSize;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

bool D3D12Context::BuildShaders()
{
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errors;

    HRESULT hr = D3DCompileFromFile(
        L"../../Shaders.hlsl",
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain",
        "vs_5_0",
        flags,
        0,
        &m_vsBytecode,
        &errors
    );
    if (FAILED(hr))
    {
        if (errors) throw std::runtime_error((const char*)errors->GetBufferPointer());
        ThrowIfFailed(hr, "D3DCompileFromFile VS failed");
    }

    errors.Reset();
    hr = D3DCompileFromFile(
        L"../../Shaders.hlsl",
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain",
        "ps_5_0",
        flags,
        0,
        &m_psBytecode,
        &errors
    );
    if (FAILED(hr))
    {
        if (errors) throw std::runtime_error((const char*)errors->GetBufferPointer());
        ThrowIfFailed(hr, "D3DCompileFromFile PS failed");
    }

    m_inputLayout[0] = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    m_inputLayout[1] = { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
                         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    m_inputLayout[2] = { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
                         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };

    return true;
}

bool D3D12Context::BuildGeometry()
{
    const std::string objPath = "../../sponza.obj";
    ObjLoaded model{};
    if (!LoadObjWithGroups(objPath, model))
        throw std::runtime_error("Failed to load OBJ (or empty mesh): " + objPath);

    m_indexCount = (uint32_t)model.indices.size();

    std::unordered_map<std::string, uint32_t> pathToKey;
    std::vector<std::string> uniquePaths;

    auto getOrAddKey = [&](const std::string& path) -> uint32_t
    {
        if (path.empty()) return 0; //белый
        auto it = pathToKey.find(path);
        if (it != pathToKey.end()) return it->second;
        uint32_t key = 1u + (uint32_t)uniquePaths.size();
        pathToKey.emplace(path, key);
        uniquePaths.push_back(path);
        return key;
    };

    std::vector<uint32_t> groupKey;
    groupKey.reserve(model.groups.size());

    for (const auto& g : model.groups)
    {
        std::string diffuse;
        if (!g.mtl.empty())
        {
            auto it = model.mtlToDiffuse.find(g.mtl);
            if (it != model.mtlToDiffuse.end()) diffuse = it->second;
        }
        groupKey.push_back(getOrAddKey(diffuse));
    }

    const UINT64 vbByteSize = (UINT64)model.vertices.size() * sizeof(Vertex);
    const UINT64 ibByteSize = (UINT64)model.indices.size() * sizeof(uint32_t);

    D3D12_HEAP_PROPERTIES defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES uploadHeap  = HeapProps(D3D12_HEAP_TYPE_UPLOAD);

    D3D12_RESOURCE_DESC vbDesc = BufferDesc(vbByteSize);
    D3D12_RESOURCE_DESC ibDesc = BufferDesc(ibByteSize);

    ThrowIfFailed(m_device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_vertexBufferGPU)), "Create VB (default)");

    ThrowIfFailed(m_device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_indexBufferGPU)), "Create IB (default)");

    ComPtr<ID3D12Resource> vbUpload;
    ComPtr<ID3D12Resource> ibUpload;

    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&vbUpload)), "Create VB (upload)");

    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&ibUpload)), "Create IB (upload)");

    {
        void* mapped = nullptr;
        D3D12_RANGE rr{ 0,0 };
        ThrowIfFailed(vbUpload->Map(0, &rr, &mapped), "Map VB upload");
        std::memcpy(mapped, model.vertices.data(), (size_t)vbByteSize);
        vbUpload->Unmap(0, nullptr);
    }
    {
        void* mapped = nullptr;
        D3D12_RANGE rr{ 0,0 };
        ThrowIfFailed(ibUpload->Map(0, &rr, &mapped), "Map IB upload");
        std::memcpy(mapped, model.indices.data(), (size_t)ibByteSize);
        ibUpload->Unmap(0, nullptr);
    }

    m_textures.clear();
    m_textures.reserve(1 + uniquePaths.size());

    {
        D3D12_RESOURCE_DESC td = Tex2DDesc(1, 1, DXGI_FORMAT_B8G8R8A8_UNORM);
        ComPtr<ID3D12Resource> tex;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&tex)), "Create white tex");
        m_textures.push_back(tex);
    }

    std::vector<bool> loadedOk;
    loadedOk.reserve(uniquePaths.size());

    for (const auto& p : uniquePaths)
    {
        WicImage img{};
        bool ok = LoadImageWIC(ToWStringAscii(p), img);
        loadedOk.push_back(ok);

        if (!ok)
        {
            m_textures.push_back(m_textures[0]);
            continue;
        }

        D3D12_RESOURCE_DESC td = Tex2DDesc(img.width, img.height, DXGI_FORMAT_B8G8R8A8_UNORM);
        ComPtr<ID3D12Resource> tex;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&tex)), "Create texture default");
        m_textures.push_back(tex);
    }

    ThrowIfFailed(m_cmdAlloc->Reset(), "CmdAlloc Reset (BuildGeometry)");
    ThrowIfFailed(m_cmdList->Reset(m_cmdAlloc.Get(), nullptr), "CmdList Reset (BuildGeometry)");

    m_cmdList->CopyBufferRegion(m_vertexBufferGPU.Get(), 0, vbUpload.Get(), 0, vbByteSize);
    m_cmdList->CopyBufferRegion(m_indexBufferGPU.Get(), 0, ibUpload.Get(), 0, ibByteSize);

    {
        D3D12_RESOURCE_BARRIER b[2]{};
        b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition.pResource = m_vertexBufferGPU.Get();
        b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[1].Transition.pResource = m_indexBufferGPU.Get();
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_INDEX_BUFFER;
        b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        m_cmdList->ResourceBarrier(2, b);
    }

    std::vector<ComPtr<ID3D12Resource>> texUploads;
    texUploads.reserve(m_textures.size());

    {
        const uint8_t white[4] = { 255,255,255,255 };
        D3D12_RESOURCE_DESC td = Tex2DDesc(1, 1, DXGI_FORMAT_B8G8R8A8_UNORM);

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT numRows=0; UINT64 rowBytes=0; UINT64 totalBytes=0;
        m_device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &numRows, &rowBytes, &totalBytes);

        D3D12_RESOURCE_DESC upDesc = BufferDesc(totalBytes);
        ComPtr<ID3D12Resource> upload;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &upDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&upload)), "Create white upload");

        void* mapped=nullptr; D3D12_RANGE rr{0,0};
        ThrowIfFailed(upload->Map(0, &rr, &mapped), "Map white upload");
        std::memcpy(mapped, white, 4);
        upload->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = m_textures[0].Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;

        m_cmdList->CopyTextureRegion(&dst, 0,0,0, &src, nullptr);

        D3D12_RESOURCE_BARRIER tb{};
        tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tb.Transition.pResource = m_textures[0].Get();
        tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_cmdList->ResourceBarrier(1, &tb);

        texUploads.push_back(upload);
    }

    // аплоад реальной текстуры (почти работает (нет))
    for (size_t i = 0; i < uniquePaths.size(); ++i)
    {
        if (!loadedOk[i]) continue;

        WicImage img{};
        if (!LoadImageWIC(ToWStringAscii(uniquePaths[i]), img)) continue;

        D3D12_RESOURCE_DESC td = Tex2DDesc(img.width, img.height, DXGI_FORMAT_B8G8R8A8_UNORM);

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT numRows=0; UINT64 rowBytes=0; UINT64 totalBytes=0;
        m_device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &numRows, &rowBytes, &totalBytes);

        D3D12_RESOURCE_DESC upDesc = BufferDesc(totalBytes);
        ComPtr<ID3D12Resource> upload;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &upDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&upload)), "Create tex upload");

        void* mapped=nullptr; D3D12_RANGE rr{0,0};
        ThrowIfFailed(upload->Map(0, &rr, &mapped), "Map tex upload");

        uint8_t* dst = (uint8_t*)mapped;
        const uint8_t* srcPx = img.bgra.data();
        const uint32_t srcRowPitch = img.width * 4;
        const uint32_t dstRowPitch = fp.Footprint.RowPitch;

        for (uint32_t y = 0; y < img.height; ++y)
            std::memcpy(dst + (size_t)y * dstRowPitch, srcPx + (size_t)y * srcRowPitch, srcRowPitch);

        upload->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = m_textures[i + 1].Get();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = upload.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = fp;

        m_cmdList->CopyTextureRegion(&dstLoc, 0,0,0, &srcLoc, nullptr);

        D3D12_RESOURCE_BARRIER tb{};
        tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tb.Transition.pResource = m_textures[i + 1].Get();
        tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_cmdList->ResourceBarrier(1, &tb);

        texUploads.push_back(upload);
    }

    ThrowIfFailed(m_cmdList->Close(), "CmdList Close (BuildGeometry)");
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);
    FlushCommandQueue();

    // Views
    m_vbv.BufferLocation = m_vertexBufferGPU->GetGPUVirtualAddress();
    m_vbv.StrideInBytes = sizeof(Vertex);
    m_vbv.SizeInBytes = (UINT)vbByteSize;

    m_ibv.BufferLocation = m_indexBufferGPU->GetGPUVirtualAddress();
    m_ibv.SizeInBytes = (UINT)ibByteSize;
    m_ibv.Format = DXGI_FORMAT_R32_UINT;

    m_drawItems.clear();
    m_drawItems.reserve(model.groups.size());

    for (size_t gi = 0; gi < model.groups.size(); ++gi)
    {
        const auto& g = model.groups[gi];

        DrawItem di{};
        di.StartIndexLocation = g.start;
        di.IndexCount = g.count;

        uint32_t key = groupKey[gi];
        uint32_t texResIndex = 0;
        if (key > 0)
        {
            if (key < (uint32_t)m_textures.size())
                texResIndex = key;
        }

        di.TextureSrvIndex = 1 + texResIndex;
        m_drawItems.push_back(di);
    }

    return true;
}



bool D3D12Context::BuildConstantBuffer()
{
    m_objectCBByteSize = AlignConstantBufferByteSize(sizeof(ObjectConstants));

    D3D12_HEAP_PROPERTIES uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC cbDesc = BufferDesc((UINT64)m_objectCBByteSize);

    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE,
        &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_objectCB)), "Create Object Constant Buffer");

    D3D12_RANGE readRange{ 0,0 };
    ThrowIfFailed(m_objectCB->Map(0, &readRange, reinterpret_cast<void**>(&m_mappedObjectCB)),
                  "Map Object Constant Buffer");

    if (m_textures.empty())
        throw std::runtime_error("No textures created (expected at least 1 white texture).");

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 1 + (UINT)m_textures.size();
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_cbvHeap)), "Create CBV/SRV heap");

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
    cbvDesc.BufferLocation = m_objectCB->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = m_objectCBByteSize;
    m_device->CreateConstantBufferView(&cbvDesc, m_cbvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_CPU_DESCRIPTOR_HANDLE h = m_cbvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)m_cbvSrvUavDescriptorSize;

    for (size_t i = 0; i < m_textures.size(); ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.ResourceMinLODClamp = 0.0f;

        m_device->CreateShaderResourceView(m_textures[i].Get(), &srv, h);
        h.ptr += (UINT64)m_cbvSrvUavDescriptorSize;
    }

    UpdateConstantBuffer();
    return true;
}


bool D3D12Context::BuildRootSignature()
{
    D3D12_DESCRIPTOR_RANGE cbvRange{};
    cbvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    cbvRange.NumDescriptors = 1;
    cbvRange.BaseShaderRegister = 0;
    cbvRange.RegisterSpace = 0;
    cbvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2]{};

    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &cbvRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.MipLODBias = 0.0f;
    samp.MaxAnisotropy = 1;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    samp.MinLOD = 0.0f;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0; // s0
    samp.RegisterSpace = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &samp;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(hr))
    {
        if (errors) throw std::runtime_error((const char*)errors->GetBufferPointer());
        ThrowIfFailed(hr, "D3D12SerializeRootSignature failed");
    }

    ThrowIfFailed(m_device->CreateRootSignature(0,
        serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)), "CreateRootSignature");

    return true;
}


bool D3D12Context::BuildPSO()
{
    D3D12_RASTERIZER_DESC rast{};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_NONE;
    rast.FrontCounterClockwise = TRUE;
    rast.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rast.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rast.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rast.DepthClipEnable = TRUE;

    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    ds.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { m_vsBytecode->GetBufferPointer(), m_vsBytecode->GetBufferSize() };
    psoDesc.PS = { m_psBytecode->GetBufferPointer(), m_psBytecode->GetBufferSize() };
    psoDesc.BlendState = blend;
    psoDesc.RasterizerState = rast;
    psoDesc.DepthStencilState = ds;
    psoDesc.SampleMask = UINT_MAX;

    psoDesc.InputLayout = { m_inputLayout, 3 };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)), "CreateGraphicsPipelineState");
    return true;
}

void D3D12Context::UpdateConstantBuffer()
{
    if (!m_mappedObjectCB) return;

    ObjectConstants cb{};

    XMMATRIX world = XMLoadFloat4x4(&m_world);
    XMMATRIX view = XMLoadFloat4x4(&m_view);
    XMMATRIX proj = XMLoadFloat4x4(&m_proj);
    XMMATRIX wvp = world*view*proj;

    XMStoreFloat4x4(&cb.World, XMMatrixTranspose(world));
    XMStoreFloat4x4(&cb.WorldViewProj, XMMatrixTranspose(wvp));

    cb.EyePosW = m_eyePos;
    cb.LightDirW = m_lightDir;
    DirectX::XMVECTOR L = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&m_lightDir));
    DirectX::XMStoreFloat3(&cb.LightDirW, L);

    cb.Ambient = DirectX::XMFLOAT4(0.08f, 0.08f, 0.08f, 1.0f);
    cb.Diffuse = DirectX::XMFLOAT4(0.90f, 0.90f, 0.90f, 1.0f);
    cb.Specular = DirectX::XMFLOAT4(0.90f, 0.90f, 0.90f, 1.0f);
    cb.SpecPower = 64.0f;

    std::memcpy(m_mappedObjectCB, &cb, sizeof(cb));
}