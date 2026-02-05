// Windows Implementation using Raw Input
#include "device_scanner.h"
#include <iostream>
#include <vector>
#include <atomic>
#include "../logging/logger.h"
#include <windows.h>

extern std::atomic<bool> running;

namespace {
constexpr const char* kTag = "device_scanner";
}

static LRESULT CALLBACK RawInputWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

class WindowsInputBackend {
public:
    WindowsInputBackend() : hwnd(NULL), initialized(false) {}
    
    bool Initialize() {
        if (initialized) return true;
        const char* class_name = "WheelEmulatorInputClass";
        WNDCLASSEX wc = {};
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.lpfnWndProc = RawInputWindowProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = class_name;
        RegisterClassEx(&wc);

        // Create a message-only window
        hwnd = CreateWindowEx(0, class_name, "WheelEmulatorInput", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandle(NULL), NULL);
        if (!hwnd) {
            LOG_ERROR(kTag, "Failed to create message-only window for Raw Input");
            return false;
        }

        // Register for Raw Input
        RAWINPUTDEVICE Rid[2];
        
        // Keyboard
        Rid[0].usUsagePage = 0x01; 
        Rid[0].usUsage = 0x06; 
        Rid[0].dwFlags = RIDEV_INPUTSINK; // Receive input even when in background
        Rid[0].hwndTarget = hwnd;

        // Mouse
        Rid[1].usUsagePage = 0x01; 
        Rid[1].usUsage = 0x02; 
        Rid[1].dwFlags = RIDEV_INPUTSINK; // Receive input even when in background (for steering)
        Rid[1].hwndTarget = hwnd;

        if (RegisterRawInputDevices(Rid, 2, sizeof(Rid[0])) == FALSE) {
            LOG_ERROR(kTag, "RegisterRawInputDevices failed");
            return false;
        }
        
        initialized = true;
        return true;
    }

    void PumpMessages() {
        if (!initialized) return;
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    HWND GetHwnd() { return hwnd; }
    bool IsInitialized() const { return initialized; }

private:
    HWND hwnd;
    bool initialized;
};

// Singleton context for the window proc to access scanner
static DeviceScanner* g_scanner = nullptr;
static WindowsInputBackend* g_backend = nullptr;

// Mapping helper: VK code to Linux KEY_ code
static int MapVirtualKeyToLinux(UINT vk, UINT scancode, UINT flags) {
    switch (vk) {
        case VK_ESCAPE: return KEY_ESC;
        case '1': return KEY_1;
        case '2': return KEY_2;
        case '3': return KEY_3;
        case '4': return KEY_4;
        case '5': return KEY_5;
        case '6': return KEY_6;
        case '7': return KEY_7;
        case '8': return KEY_8;
        case '9': return KEY_9;
        case '0': return KEY_0;
        case VK_OEM_MINUS: return KEY_MINUS;
        case VK_OEM_PLUS: return KEY_EQUAL;
        case VK_BACK: return KEY_BACKSPACE;
        case VK_TAB: return KEY_TAB;
        case 'Q': return KEY_Q;
        case 'W': return KEY_W;
        case 'E': return KEY_E;
        case 'R': return KEY_R;
        case 'T': return KEY_T;
        case 'Y': return KEY_Y;
        case 'U': return KEY_U;
        case 'I': return KEY_I;
        case 'O': return KEY_O;
        case 'P': return KEY_P;
        case VK_OEM_4: return KEY_LEFTBRACE;
        case VK_OEM_6: return KEY_RIGHTBRACE;
        case VK_RETURN: return KEY_ENTER;
        case VK_CONTROL: return (flags & RI_KEY_E0) ? KEY_RIGHTCTRL : KEY_LEFTCTRL;
        case 'A': return KEY_A;
        case 'S': return KEY_S;
        case 'D': return KEY_D;
        case 'F': return KEY_F;
        case 'G': return KEY_G;
        case 'H': return KEY_H;
        case 'J': return KEY_J;
        case 'K': return KEY_K;
        case 'L': return KEY_L;
        case VK_OEM_1: return KEY_SEMICOLON;
        case VK_OEM_7: return KEY_APOSTROPHE;
        case VK_OEM_3: return KEY_GRAVE;
        case VK_SHIFT: return (scancode == 0x36) ? KEY_RIGHTSHIFT : KEY_LEFTSHIFT;
        case VK_OEM_5: return KEY_BACKSLASH;
        case 'Z': return KEY_Z;
        case 'X': return KEY_X;
        case 'C': return KEY_C;
        case 'V': return KEY_V;
        case 'B': return KEY_B;
        case 'N': return KEY_N;
        case 'M': return KEY_M;
        case VK_OEM_COMMA: return KEY_COMMA;
        case VK_OEM_PERIOD: return KEY_DOT;
        case VK_OEM_2: return KEY_SLASH;
        case VK_MULTIPLY: return KEY_KPASTERISK;
        case VK_MENU: return (flags & RI_KEY_E0) ? KEY_RIGHTALT : KEY_LEFTALT;
        case VK_SPACE: return KEY_SPACE;
        case VK_CAPITAL: return KEY_CAPSLOCK;
        case VK_F1: return KEY_F1;
        case VK_F2: return KEY_F2;
        case VK_F3: return KEY_F3;
        case VK_F4: return KEY_F4;
        case VK_F5: return KEY_F5;
        case VK_F6: return KEY_F6;
        case VK_F7: return KEY_F7;
        case VK_F8: return KEY_F8;
        case VK_F9: return KEY_F9;
        case VK_F10: return KEY_F10;
        case VK_F11: return KEY_F11;
        case VK_F12: return KEY_F12;
        case VK_UP: return KEY_UP;
        case VK_DOWN: return KEY_DOWN;
        case VK_LEFT: return KEY_LEFT;
        case VK_RIGHT: return KEY_RIGHT;
        case VK_LWIN: return KEY_LEFTMETA;
        case VK_RWIN: return KEY_RIGHTMETA;
        default: return KEY_RESERVED;
    }
}

// Raw Input Handler
void ProcessRawInput(HRAWINPUT hRawInput) {
    UINT dwSize;
    GetRawInputData(hRawInput, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));
    
    if (dwSize == 0) return;
    
    std::vector<BYTE> lpb(dwSize);
    if (GetRawInputData(hRawInput, RID_INPUT, lpb.data(), &dwSize, sizeof(RAWINPUTHEADER)) != dwSize)
         return;

    RAWINPUT* raw = (RAWINPUT*)lpb.data();

    if (!g_scanner) return;

    if (raw->header.dwType == RIM_TYPEKEYBOARD) {
        UINT vk = raw->data.keyboard.VKey;
        UINT scancode = raw->data.keyboard.MakeCode;
        UINT flags = raw->data.keyboard.Flags;
        bool is_pressed = !(flags & RI_KEY_BREAK);

        int linux_code = MapVirtualKeyToLinux(vk, scancode, flags);
        
        if (linux_code != KEY_RESERVED) {
            g_scanner->UpdateKeyState(linux_code, is_pressed);
        }
    }
    else if (raw->header.dwType == RIM_TYPEMOUSE) {
        int dx = raw->data.mouse.lLastX;
        
        if (dx != 0) {
            g_scanner->UpdateMouseState(dx);
        }
        
        // Mouse buttons
        USHORT btn_flags = raw->data.mouse.usButtonFlags;
        if (btn_flags & RI_MOUSE_LEFT_BUTTON_DOWN) g_scanner->UpdateKeyState(BTN_LEFT, true);
        if (btn_flags & RI_MOUSE_LEFT_BUTTON_UP) g_scanner->UpdateKeyState(BTN_LEFT, false);
        if (btn_flags & RI_MOUSE_RIGHT_BUTTON_DOWN) g_scanner->UpdateKeyState(BTN_RIGHT, true);
        if (btn_flags & RI_MOUSE_RIGHT_BUTTON_UP) g_scanner->UpdateKeyState(BTN_RIGHT, false);
        if (btn_flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) g_scanner->UpdateKeyState(BTN_MIDDLE, true);
        if (btn_flags & RI_MOUSE_MIDDLE_BUTTON_UP) g_scanner->UpdateKeyState(BTN_MIDDLE, false);
    }
}

LRESULT CALLBACK RawInputWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INPUT: 
            ProcessRawInput((HRAWINPUT)lParam);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// DeviceScanner Implementation

DeviceScanner::DeviceScanner() {
    g_scanner = this;
    g_backend = new WindowsInputBackend();
}

DeviceScanner::~DeviceScanner() {
    UnlockCursor();
    if (g_backend) {
        delete g_backend;
        g_backend = nullptr;
    }
    g_scanner = nullptr;
}

// On Windows we don't need explicit discovery as we use RIDEV_INPUTSINK
bool DeviceScanner::DiscoverKeyboard(const std::string& device_path) { return true; }
bool DeviceScanner::DiscoverMouse(const std::string& device_path) { return true; }

void DeviceScanner::Read(int& mouse_dx) {
    mouse_dx = 0;
    // Process Windows messages
    if (g_backend) {
        g_backend->PumpMessages();
    }
    
    // Consume accumulated mouse delta
    {
        std::lock_guard<std::mutex> lock(input_mutex);
        mouse_dx = accumulated_mouse_dx;
        accumulated_mouse_dx = 0;
    }

    // Re-apply cursor lock every frame — ClipCursor can be reset by Windows
    if (cursor_locked_) {
        RECT clip;
        clip.left   = saved_cursor_pos_.x;
        clip.top    = saved_cursor_pos_.y;
        clip.right  = saved_cursor_pos_.x + 1;
        clip.bottom = saved_cursor_pos_.y + 1;
        ClipCursor(&clip);
        SetCursorPos(saved_cursor_pos_.x, saved_cursor_pos_.y);
    }
}

void DeviceScanner::Read() {
    int dummy = 0;
    Read(dummy);
}

void DeviceScanner::UpdateKeyState(int linux_code, bool pressed) {
    std::lock_guard<std::mutex> lock(input_mutex);
    if (pressed) {
        active_keys.insert(linux_code);
    } else {
        active_keys.erase(linux_code);
    }
    NotifyInputChanged();
}

void DeviceScanner::UpdateMouseState(int dx) {
    std::lock_guard<std::mutex> lock(input_mutex);
    accumulated_mouse_dx += dx;
    NotifyInputChanged();
}

void DeviceScanner::NotifyInputChanged() {
    // CV not used — Windows message loop drives input via MsgWaitForMultipleObjects
}

bool DeviceScanner::WaitForEvents(int timeout_ms) {
    if (g_backend && !g_backend->IsInitialized()) {
        if (!g_backend->Initialize()) {
            LOG_ERROR(kTag, "Failed to initialize backend on reader thread");
            return false;
        }
    }

    if (g_backend) g_backend->PumpMessages();

    DWORD loop_timeout = (timeout_ms < 0) ? INFINITE : static_cast<DWORD>(timeout_ms);
    
    DWORD result = MsgWaitForMultipleObjects(0, NULL, FALSE, loop_timeout, QS_ALLINPUT);
    
    if (result == WAIT_OBJECT_0) {
        if (g_backend) g_backend->PumpMessages();
        return true;
    }
    
    return false; // Timeout
}

bool DeviceScanner::IsKeyPressed(int keycode) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(input_mutex));
    return active_keys.find(keycode) != active_keys.end();
}

bool DeviceScanner::Grab(bool enable) {
    if (enable) {
        LockCursor();
    } else {
        UnlockCursor();
    }
    return true;
}

void DeviceScanner::LockCursor() {
    if (cursor_locked_) return;

    GetCursorPos(&saved_cursor_pos_);

    RECT clip;
    clip.left   = saved_cursor_pos_.x;
    clip.top    = saved_cursor_pos_.y;
    clip.right  = saved_cursor_pos_.x + 1;
    clip.bottom = saved_cursor_pos_.y + 1;
    ClipCursor(&clip);

    while (ShowCursor(FALSE) >= 0) {}

    cursor_locked_ = true;
    LOG_INFO(kTag, "Cursor locked at (" << saved_cursor_pos_.x << ", " << saved_cursor_pos_.y << ")");
}

void DeviceScanner::UnlockCursor() {
    if (!cursor_locked_) return;

    ClipCursor(NULL);
    SetCursorPos(saved_cursor_pos_.x, saved_cursor_pos_.y);
    while (ShowCursor(TRUE) < 0) {}

    cursor_locked_ = false;
    LOG_INFO(kTag, "Cursor unlocked");
}

void DeviceScanner::ResyncKeyStates() { }

bool DeviceScanner::CheckToggle() {
    bool down = IsKeyPressed(KEY_LEFTCTRL) && IsKeyPressed(KEY_M);
    if (down && !toggle_latch_) {
        toggle_latch_ = true;
        return true;
    }
    if (!down) {
        toggle_latch_ = false;
    }
    return false;
}

bool DeviceScanner::HasGrabbedKeyboard() const { return true; }
bool DeviceScanner::HasGrabbedMouse() const { return true; }
bool DeviceScanner::AllRequiredGrabbed() const { return true; }
bool DeviceScanner::HasRequiredDevices() const { return true; }
