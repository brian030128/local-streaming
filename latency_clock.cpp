// Large running millisecond clock for measuring display-to-display latency.
// Usage: latency_clock.exe [seconds]   (default 60; Esc quits)
#include <windows.h>
#include <timeapi.h>

#include <cstdio>
#include <cstdlib>

static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);
    const double seconds = argc > 1 ? atof(argv[1]) : 60.0;

    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LatencyClock";
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"Latency clock", WS_POPUP | WS_VISIBLE,
                                0, 0, 960, 300, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;
    HFONT font = CreateFontW(280, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, L"Consolas");

    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    MSG msg;
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            DispatchMessageW(&msg);
        }
        QueryPerformanceCounter(&now);
        const double t = static_cast<double>(now.QuadPart - start.QuadPart) / freq.QuadPart;
        if (t >= seconds) break;
        if (!running) break;

        wchar_t text[16];
        const int ms = static_cast<int>(t * 1000) % 100000;
        swprintf(text, 16, L"%02d.%03d", ms / 1000, ms % 1000);
        HDC dc = GetDC(hwnd);
        HGDIOBJ old = SelectObject(dc, font);
        SetBkColor(dc, RGB(0, 0, 0));
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutW(dc, 10, 5, text, 6);
        SelectObject(dc, old);
        ReleaseDC(hwnd, dc);
        Sleep(1);
    }
    timeEndPeriod(1);
    return 0;
}
