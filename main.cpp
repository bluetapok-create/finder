#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <filesystem>
#include <string>
#include <chrono>
#include <random>
#include <cstdio>

#include "rzctl_embedded.hpp"

// ============================================================================
// Embedded DLL extraction
// ============================================================================

// rzctl.dll function types
typedef bool(__cdecl* RZInit)();
typedef void(__cdecl* RZMouseMove)(int x, int y, bool starting_point);
typedef void(__cdecl* RZMouseClick)(int up_down);

class EmbeddedMouseDriver {
public:
    EmbeddedMouseDriver() = default;
    ~EmbeddedMouseDriver() { close(); }

    bool open() {
        // Generate random temp path for DLL
        wchar_t tempPath[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        
        // Use a random-ish name
        DWORD tick = GetTickCount();
        dllPath_ = std::wstring(tempPath) + L"sys" + std::to_wstring(tick) + L".tmp";

        // Write embedded DLL to temp file
        HANDLE hFile = CreateFileW(
            dllPath_.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY,
            nullptr
        );

        if (hFile == INVALID_HANDLE_VALUE) {
            return false;
        }

        DWORD written;
        WriteFile(hFile, rzctl_dll_data, static_cast<DWORD>(rzctl_dll_size), &written, nullptr);
        CloseHandle(hFile);

        if (written != rzctl_dll_size) {
            DeleteFileW(dllPath_.c_str());
            return false;
        }

        // Load the DLL
        hModule_ = LoadLibraryW(dllPath_.c_str());
        if (!hModule_) {
            DeleteFileW(dllPath_.c_str());
            return false;
        }

        // Get function pointers
        pInit = (RZInit)GetProcAddress(hModule_, "init");
        pMouseMove = (RZMouseMove)GetProcAddress(hModule_, "mouse_move");
        pMouseClick = (RZMouseClick)GetProcAddress(hModule_, "mouse_click");

        if (!pInit || !pMouseMove || !pMouseClick) {
            FreeLibrary(hModule_);
            hModule_ = nullptr;
            DeleteFileW(dllPath_.c_str());
            return false;
        }

        // Initialize rzctl
        if (!pInit()) {
            FreeLibrary(hModule_);
            hModule_ = nullptr;
            DeleteFileW(dllPath_.c_str());
            return false;
        }

        connected_ = true;
        return true;
    }

    void close() {
        if (hModule_) {
            FreeLibrary(hModule_);
            hModule_ = nullptr;
        }
        connected_ = false;
        pInit = nullptr;
        pMouseMove = nullptr;
        pMouseClick = nullptr;

        // Delete temp DLL file
        if (!dllPath_.empty()) {
            DeleteFileW(dllPath_.c_str());
            dllPath_.clear();
        }
    }

    bool mouseDown() {
        if (!connected_ || !pMouseClick) return false;
        pMouseClick(1); // 0 for down
        return true;
    }

    bool mouseUp() {
        if (!connected_ || !pMouseClick) return false;
        pMouseClick(2); // 1 for up
        return true;
    }

    bool click() {
        if (!connected_ || !pMouseClick) return false;
        mouseDown();
        Sleep(50);
        mouseUp();
        Sleep(50);  // extra delay after up to ensure it registers
        return true;
    }

    bool isConnected() const { return connected_; }

private:
    HMODULE hModule_ = nullptr;
    bool connected_ = false;
    std::wstring dllPath_;

    RZInit pInit = nullptr;
    RZMouseMove pMouseMove = nullptr;
    RZMouseClick pMouseClick = nullptr;
};

// Global state
std::atomic<bool> g_running{ true };
std::atomic<bool> g_colorCheckEnabled{ true };  // Toggle state for color checking
EmbeddedMouseDriver g_mouse;

// ============================================================================
// Status Overlay (WDA_EXCLUDEFROMCAPTURE)
// ============================================================================

HWND g_overlayHwnd = nullptr;
std::atomic<bool> g_overlayRunning{ true };

// Window procedure for the overlay
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND: {
            // Fill with colorkey color (black) to make it transparent
            HDC hdc = (HDC)wParam;
            RECT rect;
            GetClientRect(hwnd, &rect);
            HBRUSH hBrush = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hdc, &rect, hBrush);
            DeleteObject(hBrush);
            return 1;  // We handled it
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            // Set up transparent background
            SetBkMode(hdc, TRANSPARENT);
            
            // Create font
            HFONT hFont = CreateFontW(
                24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas"
            );
            HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
            
            // Draw status text
            const wchar_t* statusText = g_colorCheckEnabled ? L"[ON]" : L"[OFF]";
            COLORREF textColor = g_colorCheckEnabled ? RGB(0, 255, 0) : RGB(255, 0, 0);
            SetTextColor(hdc, textColor);
            
            RECT rect = { 5, 5, 100, 35 };
            DrawTextW(hdc, statusText, -1, &rect, DT_LEFT | DT_TOP | DT_NOCLIP);
            
            SelectObject(hdc, hOldFont);
            DeleteObject(hFont);
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// Create and run the overlay window
void overlayThread() {
    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"CheckerStatusOverlay";
    
    if (!RegisterClassExW(&wc)) {
        return;
    }
    
    // Create layered window
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"CheckerStatusOverlay",
        L"",
        WS_POPUP,
        10, 10,  // Top-left position
        100, 40, // Size
        nullptr, nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );
    
    if (!hwnd) {
        UnregisterClassW(L"CheckerStatusOverlay", GetModuleHandleW(nullptr));
        return;
    }
    
    g_overlayHwnd = hwnd;
    
    // Set layered window attributes for transparency (black = transparent)
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 255, LWA_COLORKEY);
    
    // Apply WDA_EXCLUDEFROMCAPTURE to make overlay not capturable
    typedef BOOL(WINAPI* SetWindowDisplayAffinityFunc)(HWND, DWORD);
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        SetWindowDisplayAffinityFunc pSetWindowDisplayAffinity = 
            (SetWindowDisplayAffinityFunc)GetProcAddress(hUser32, "SetWindowDisplayAffinity");
        if (pSetWindowDisplayAffinity) {
            // WDA_EXCLUDEFROMCAPTURE = 0x11
            pSetWindowDisplayAffinity(hwnd, 0x00000011);
        }
    }
    
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    
    // Timer for 1 second update rate
    SetTimer(hwnd, 1, 1000, nullptr);
    
    // Message loop with proper handling
    MSG msg;
    BOOL bRet;
    while ((bRet = GetMessage(&msg, nullptr, 0, 0)) != 0) {
        if (bRet == -1) {
            // Error - exit loop
            break;
        }
        if (!g_overlayRunning) {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    // Cleanup
    if (IsWindow(hwnd)) {
        KillTimer(hwnd, 1);
        DestroyWindow(hwnd);
    }
    g_overlayHwnd = nullptr;
    UnregisterClassW(L"CheckerStatusOverlay", GetModuleHandleW(nullptr));
}

// ============================================================================
// Stealth utilities
// ============================================================================

// Modify file timestamps (created and last access) to offset days from now
bool modifyFileTimestamps(const std::wstring& filePath, int daysOffset) {
    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    // Get current time
    FILETIME ftNow;
    GetSystemTimeAsFileTime(&ftNow);

    // Convert to ULARGE_INTEGER for arithmetic
    ULARGE_INTEGER uli;
    uli.LowPart = ftNow.dwLowDateTime;
    uli.HighPart = ftNow.dwHighDateTime;

    // Calculate offset (100-nanosecond intervals: 10,000,000 per second * 86400 seconds per day)
    const ULONGLONG intervalsPerDay = 10000000ULL * 60ULL * 60ULL * 24ULL;
    // daysOffset is negative for past dates, positive for future
    if (daysOffset < 0) {
        uli.QuadPart -= static_cast<ULONGLONG>(-daysOffset) * intervalsPerDay;
    } else {
        uli.QuadPart += static_cast<ULONGLONG>(daysOffset) * intervalsPerDay;
    }

    FILETIME ftModified;
    ftModified.dwLowDateTime = uli.LowPart;
    ftModified.dwHighDateTime = uli.HighPart;

    // Set creation time, last access time, and last write time
    BOOL result = SetFileTime(hFile, &ftModified, &ftModified, &ftModified);
    CloseHandle(hFile);

    return result != FALSE;
}

// Get path to current executable
std::wstring getExecutablePath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::wstring(path);
}

// Clear prefetch entry for the executable
void clearPrefetchEntry() {
    // Get executable name without path
    std::wstring exePath = getExecutablePath();
    std::wstring exeName = exePath.substr(exePath.find_last_of(L"\\") + 1);
    
    // Remove .exe extension and convert to uppercase
    if (exeName.size() > 4) {
        exeName = exeName.substr(0, exeName.size() - 4);
    }
    for (auto& c : exeName) {
        c = towupper(c);
    }

    // Prefetch directory
    wchar_t winDir[MAX_PATH];
    GetWindowsDirectoryW(winDir, MAX_PATH);
    std::wstring prefetchDir = std::wstring(winDir) + L"\\Prefetch";

    // Find and delete matching prefetch files
    WIN32_FIND_DATAW findData;
    std::wstring searchPattern = prefetchDir + L"\\" + exeName + L"*.pf";
    
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::wstring fullPath = prefetchDir + L"\\" + findData.cFileName;
            DeleteFileW(fullPath.c_str());
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }
}

// ============================================================================
// DXGI Screen Capture (no loading cursor, hardware accelerated)
// ============================================================================

#include <d3d11.h>
#include <dxgi1_2.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

class DXGIScreenCapture {
public:
    DXGIScreenCapture() = default;
    ~DXGIScreenCapture() { cleanup(); }

    bool init() {
        // Create D3D11 device
        D3D_FEATURE_LEVEL featureLevel;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION,
            &device_, &featureLevel, &context_
        );
        if (FAILED(hr)) return false;

        // Get DXGI device
        IDXGIDevice* dxgiDevice = nullptr;
        hr = device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
        if (FAILED(hr)) return false;

        // Get adapter
        IDXGIAdapter* adapter = nullptr;
        hr = dxgiDevice->GetAdapter(&adapter);
        dxgiDevice->Release();
        if (FAILED(hr)) return false;

        // Get output (monitor)
        IDXGIOutput* output = nullptr;
        hr = adapter->EnumOutputs(0, &output);
        adapter->Release();
        if (FAILED(hr)) return false;

        // Get output1 for duplication
        IDXGIOutput1* output1 = nullptr;
        hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        output->Release();
        if (FAILED(hr)) return false;

        // Create desktop duplication
        hr = output1->DuplicateOutput(device_, &duplication_);
        output1->Release();
        if (FAILED(hr)) return false;

        // Get screen dimensions
        DXGI_OUTDUPL_DESC desc;
        duplication_->GetDesc(&desc);
        screenWidth_ = desc.ModeDesc.Width;
        screenHeight_ = desc.ModeDesc.Height;

        initialized_ = true;
        return true;
    }

    void cleanup() {
        if (duplication_) { duplication_->Release(); duplication_ = nullptr; }
        if (context_) { context_->Release(); context_ = nullptr; }
        if (device_) { device_->Release(); device_ = nullptr; }
        initialized_ = false;
    }

    // Check if exact color RGB(255, 69, 69) exists in 2x2 center area
    bool scanCenterForColor() {
        if (!initialized_) return false;

        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        IDXGIResource* resource = nullptr;
        
        // Try to acquire frame (timeout 6ms ~= 165fps)
        HRESULT hr = duplication_->AcquireNextFrame(6, &frameInfo, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            return false;  // No new frame, don't trigger (prevents stale clicks)
        }
        if (FAILED(hr)) {
            // Reinitialize on error
            cleanup();
            init();
            return false;
        }

        // Get texture from resource
        ID3D11Texture2D* texture = nullptr;
        hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture);
        resource->Release();
        if (FAILED(hr)) {
            duplication_->ReleaseFrame();
            return false;
        }

        // Create staging texture for CPU access (if not created)
        if (!stagingTexture_) {
            D3D11_TEXTURE2D_DESC desc;
            texture->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            device_->CreateTexture2D(&desc, nullptr, &stagingTexture_);
        }

        // Copy to staging
        context_->CopyResource(stagingTexture_, texture);
        texture->Release();

        // Map for CPU read
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = context_->Map(stagingTexture_, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            duplication_->ReleaseFrame();
            return false;
        }

        // Check 2x2 pixels at center
        int centerX = screenWidth_ / 2;
        int centerY = screenHeight_ / 2;
        bool found = false;

        BYTE* data = static_cast<BYTE*>(mapped.pData);
        for (int dy = -1; dy <= 0 && !found; dy++) {
            for (int dx = -1; dx <= 0 && !found; dx++) {
                int x = centerX + dx;
                int y = centerY + dy;
                
                // BGRA format, 4 bytes per pixel
                BYTE* pixel = data + y * mapped.RowPitch + x * 4;
                BYTE b = pixel[0];
                BYTE g = pixel[1];
                BYTE r = pixel[2];
                
                if (r == 255 && g == 69 && b == 69) {
                    found = true;
                }
            }
        }

        context_->Unmap(stagingTexture_, 0);
        duplication_->ReleaseFrame();
        
        lastResult_ = found;
        return found;
    }

    bool isInitialized() const { return initialized_; }

private:
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGIOutputDuplication* duplication_ = nullptr;
    ID3D11Texture2D* stagingTexture_ = nullptr;
    
    int screenWidth_ = 0;
    int screenHeight_ = 0;
    bool initialized_ = false;
    bool lastResult_ = false;
};

// Global DXGI capture
DXGIScreenCapture g_capture;

// ============================================================================
// Main scanning loop
// ============================================================================

void scannerLoop() {
    bool shiftF3WasPressed = false;  // Debounce for toggle hotkey
    
    while (g_running) {
        // Check for Shift+F11 hotkey to exit
        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_F11) & 0x8000)) {
            g_running = false;
            g_overlayRunning = false;
            if (g_overlayHwnd) {
                PostMessage(g_overlayHwnd, WM_QUIT, 0, 0);
            }
            break;
        }

        // Check for Shift+F3 hotkey to toggle color checking
        bool shiftF3Pressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_F3) & 0x8000);
        if (shiftF3Pressed && !shiftF3WasPressed) {
            g_colorCheckEnabled = !g_colorCheckEnabled;
            // Force overlay repaint
            if (g_overlayHwnd) {
                InvalidateRect(g_overlayHwnd, nullptr, TRUE);
            }
        }
        shiftF3WasPressed = shiftF3Pressed;

        // Debug hotkey "5" - manually trigger click
        if (GetAsyncKeyState('5') & 0x8000) {
            if (g_mouse.isConnected()) {
                g_mouse.click();
                Sleep(300);  // cooldown
            }
        }

        // Only scan for color if enabled
        if (g_colorCheckEnabled && g_capture.scanCenterForColor()) {
            if (g_mouse.isConnected()) {
                g_mouse.click();
                Sleep(300);  // 0.3 sec cooldown after click
            }
        }
        
        // Small delay - DXGI is fast, no need for long waits
        Sleep(1);
    }
}

// Console handler for graceful shutdown
BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT || 
        signal == CTRL_BREAK_EVENT || signal == CTRL_LOGOFF_EVENT || 
        signal == CTRL_SHUTDOWN_EVENT) {
        g_running = false;
        g_overlayRunning = false;
        return TRUE;
    }
    return FALSE;
}

// ============================================================================
// Entry point - WinMain for no console window
// ============================================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, 
                   LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    // Check environment variable for mode=dev
    bool devMode = false;

    wchar_t envBuf[64];
    if (GetEnvironmentVariableW(L"mode", envBuf, 64) > 0) {
        if (wcscmp(envBuf, L"dev") == 0) {
            devMode = true;
        }
    }

    if (!devMode) {
        // Console mode: prompt for file and check existence
        AllocConsole();
        FILE* fp;
        freopen_s(&fp, "CONIN$", "r", stdin);
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);

        while (true) {
            std::cout << "Enter path/filename: ";
            std::string inputPath;
            std::getline(std::cin, inputPath);

            if (inputPath.empty()) {
                continue;
            }

            // Remove quotes if present
            if (inputPath.size() >= 2 && inputPath.front() == '"' && inputPath.back() == '"') {
                inputPath = inputPath.substr(1, inputPath.size() - 2);
            }

            if (std::filesystem::exists(inputPath)) {
                std::cout << "exists" << std::endl;
            } else {
                std::cout << "missing" << std::endl;
            }
        }
        
        fclose(fp);
        FreeConsole();
        return 0;
    }

    // Main App Mode (devMode == true)

    // Modify own timestamps on launch (-4 to -7 days)
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distr(-7, -4);
    int randomDays = distr(gen);

    std::wstring exePath = getExecutablePath();
    modifyFileTimestamps(exePath, randomDays);

    // Initialize mouse driver
    if (!g_mouse.open()) {
        return 1;
    }

    // Initialize DXGI screen capture
    if (!g_capture.init()) {
        g_mouse.close();
        return 1;
    }

    // Start overlay thread
    std::thread overlayThreadObj(overlayThread);

    // Run scanner loop
    scannerLoop();

    // Signal overlay to stop and wait for it
    g_overlayRunning = false;
    if (g_overlayHwnd) {
        PostMessage(g_overlayHwnd, WM_QUIT, 0, 0);
    }
    if (overlayThreadObj.joinable()) {
        overlayThreadObj.join();
    }

    // Cleanup on exit
    g_capture.cleanup();
    g_mouse.close();
    
    // Clear prefetch entry
    clearPrefetchEntry();

    return 0;
}
