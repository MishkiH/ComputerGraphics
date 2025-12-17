#pragma once
#include <windows.h>
#include <cstdint>
#include "D3D12Context.h"

class Window;
class Input;

class App
{
public:
    bool Initialize(HINSTANCE hInstance, int nCmdShow);
    int Run();

    LRESULT HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

private:
    void Update(float dt);
    void Render();

private:
    Window* m_window = nullptr;
    Input*  m_input = nullptr;
    D3D12Context* m_dx12 = nullptr;

    bool m_exitRequested = false;

    uint64_t m_prevTick = 0;
    double   m_secondsPerTick = 0.0;
    
};
