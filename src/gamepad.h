#ifndef GAMEPAD_H
#define GAMEPAD_H

#include <cstdint>
#include <map>
#include <string>

class Input; // Forward declaration

class GamepadDevice {
public:
    GamepadDevice();
    ~GamepadDevice();
    
    // Create virtual Xbox 360 controller
    bool Create();
    
    // Update gamepad state
    void UpdateSteering(int delta, int sensitivity);
    void UpdateThrottle(bool pressed);
    void UpdateBrake(bool pressed);
    void UpdateButtons(const Input& input);
    void UpdateDPad(const Input& input);
    
    // Send state to virtual device
    void SendState();
    void SendNeutral();

private:
    int fd;
    
    // State
    float mouse_position;  // Accumulated mouse position for steering
    float steering;  // Calculated steering value from mouse position
    float throttle;
    float brake;
    std::map<std::string, bool> buttons;
    int8_t dpad_x;
    int8_t dpad_y;
    
    void EmitEvent(uint16_t type, uint16_t code, int32_t value);
    int16_t ClampSteering(int16_t value);
};

#endif // GAMEPAD_H
