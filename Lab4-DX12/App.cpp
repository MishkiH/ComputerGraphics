#include "App.h"
#include "Window.h"
#include "Input.h"
#include <windows.h>
#include <windowsx.h>
#include <stdexcept>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

static uint64_t GetQpc()
{
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

static double GetQpf()
{
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
}

bool App::Initialize(HINSTANCE hInstance, int nCmdShow)
{
    m_window = new Window();
    m_input  = new Input();
    m_input->Reset();

    if (!m_window->Create(this, hInstance, nCmdShow, 1280, 720, L"DX12 Lab Part 2 (Clear)"))
        return false;

    m_secondsPerTick = 1.0 / GetQpf();
    m_prevTick = GetQpc();

    m_dx12 = new D3D12Context();

    RECT rc{};
    GetClientRect(m_window->GetHwnd(), &rc);
    uint32_t w = (uint32_t)(rc.right - rc.left);
    uint32_t h = (uint32_t)(rc.bottom - rc.top);

    try
    {
        if (!m_dx12->Initialize(m_window->GetHwnd(), w, h))
            return false;
    }
    catch (const std::exception& e)
    {
        MessageBoxA(nullptr, e.what(), "DX12 init failed", MB_OK | MB_ICONERROR);
        return false;
    }

    return true;
}

void App::Render() { if (m_dx12) m_dx12->Draw(); }

int App::Run()
{
    MSG msg{};
    while (!m_exitRequested)
    {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                m_exitRequested = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        const uint64_t now = GetQpc();
        const double dt = (now - m_prevTick) * m_secondsPerTick;
        m_prevTick = now;

        Update(static_cast<float>(dt));
        Render();
    }

    return 0;
}

void App::Update(float /*dt*/)
{
    if (m_input && m_input->IsKeyDown(VK_ESCAPE))
        m_exitRequested = true;
}

LRESULT App::HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_CLOSE:
    case WM_DESTROY:
        m_exitRequested = true;
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        if (m_input) m_input->OnKeyDown(static_cast<uint32_t>(wparam));
        return 0;

    case WM_KEYUP:
        if (m_input) m_input->OnKeyUp(static_cast<uint32_t>(wparam));
        return 0;

    case WM_MOUSEMOVE:
        if (m_input) m_input->OnMouseMove(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;

    case WM_SIZE:
    {
        uint32_t w = (uint32_t)LOWORD(lparam);
        uint32_t h = (uint32_t)HIWORD(lparam);

        if (w == 0 || h == 0) return 0;

        if (m_dx12) m_dx12->OnResize(w, h);
        return 0;
    }

    default:
        return DefWindowProc(hwnd, msg, wparam, lparam);
    }
}
