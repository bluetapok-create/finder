#pragma once

#include <Windows.h>
#include <iostream>

// rzctl.dll function types
typedef bool(__cdecl* RZInit)();
typedef void(__cdecl* RZMouseMove)(int x, int y, bool starting_point);
typedef void(__cdecl* RZMouseClick)(int up_down);

class MouseDriver {
public:
    MouseDriver() = default;
    ~MouseDriver() { close(); }

    bool open() {
        // Load rzctl.dll
        hModule_ = LoadLibraryA("rzctl.dll");
        if (!hModule_) {
            std::cerr << "Failed to load rzctl.dll. Make sure it's in the same folder.\n";
            std::cerr << "Download from: https://github.com/MarsQQ/rzctl/releases\n";
            return false;
        }

        // Get function pointers
        pInit = (RZInit)GetProcAddress(hModule_, "init");
        pMouseMove = (RZMouseMove)GetProcAddress(hModule_, "mouse_move");
        pMouseClick = (RZMouseClick)GetProcAddress(hModule_, "mouse_click");

        if (!pInit || !pMouseMove || !pMouseClick) {
            std::cerr << "Failed to get rzctl.dll functions.\n";
            FreeLibrary(hModule_);
            hModule_ = nullptr;
            return false;
        }

        // Initialize rzctl
        if (!pInit()) {
            std::cerr << "rzctl init() failed.\n";
            std::cerr << "Make sure Razer Synapse is running and you have a Razer device.\n";
            FreeLibrary(hModule_);
            hModule_ = nullptr;
            return false;
        }

        std::cout << "RZMouse connected successfully.\n";
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
    }

    bool move(int dx, int dy) {
        if (!connected_ || !pMouseMove) return false;
        // rzctl mouse_move with starting_point=true (as per working example)
        pMouseMove(dx, dy, true);
        return true;
    }

    bool click() {
        if (!connected_ || !pMouseClick) return false;
        pMouseClick(1);  // down
        Sleep(10);
        pMouseClick(0);  // up
        return true;
    }

    bool mouseDown() {
        if (!connected_ || !pMouseClick) return false;
        pMouseClick(1);
        return true;
    }

    bool mouseUp() {
        if (!connected_ || !pMouseClick) return false;
        pMouseClick(0);
        return true;
    }

    bool isConnected() const { return connected_; }

private:
    HMODULE hModule_ = nullptr;
    bool connected_ = false;

    RZInit pInit = nullptr;
    RZMouseMove pMouseMove = nullptr;
    RZMouseClick pMouseClick = nullptr;
};
