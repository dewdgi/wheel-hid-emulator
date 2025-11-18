#include "config.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdlib>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

bool Config::Load() {
    // Only use system config at /etc/wheel-emulator.conf
    const char* system_config = "/etc/wheel-emulator.conf";
    if (LoadFromFile(system_config)) {
        std::cout << "Loaded config from: " << system_config << std::endl;
        return true;
    }
    
    // Generate default config in /etc
    std::cout << "No config found, generating default at " << system_config << std::endl;
    SaveDefault(system_config);
    std::cout << "Default config saved. Please edit " << system_config << " and run --detect to configure devices." << std::endl;
    
    // Set default values
    sensitivity = 50;
    
    // Set default button mappings (for reference - hardcoded in gamepad.cpp)
    button_map["KEY_Q"] = BTN_TRIGGER;
    button_map["KEY_E"] = BTN_THUMB;
    button_map["KEY_F"] = BTN_THUMB2;
    button_map["KEY_G"] = BTN_TOP;
    button_map["KEY_H"] = BTN_TOP2;
    button_map["KEY_R"] = BTN_PINKIE;
    button_map["KEY_T"] = BTN_BASE;
    button_map["KEY_Y"] = BTN_BASE2;
    button_map["KEY_U"] = BTN_BASE3;
    button_map["KEY_I"] = BTN_BASE4;
    button_map["KEY_O"] = BTN_BASE5;
    button_map["KEY_P"] = BTN_BASE6;
    button_map["KEY_1"] = BTN_DEAD;
    button_map["KEY_2"] = BTN_TRIGGER_HAPPY1;
    button_map["KEY_3"] = BTN_TRIGGER_HAPPY2;
    button_map["KEY_4"] = BTN_TRIGGER_HAPPY3;
    button_map["KEY_5"] = BTN_TRIGGER_HAPPY4;
    button_map["KEY_6"] = BTN_TRIGGER_HAPPY5;
    button_map["KEY_7"] = BTN_TRIGGER_HAPPY6;
    button_map["KEY_8"] = BTN_TRIGGER_HAPPY7;
    button_map["KEY_9"] = BTN_TRIGGER_HAPPY8;
    button_map["KEY_0"] = BTN_TRIGGER_HAPPY9;
    button_map["KEY_LEFTSHIFT"] = BTN_TRIGGER_HAPPY10;
    button_map["KEY_SPACE"] = BTN_TRIGGER_HAPPY11;
    button_map["KEY_TAB"] = BTN_TRIGGER_HAPPY12;
    
    return true;
}

bool Config::LoadFromFile(const char* path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    ParseINI(buffer.str());
    
    return true;
}

void Config::ParseINI(const std::string& content) {
    std::istringstream stream(content);
    std::string line;
    std::string section;
    
    while (std::getline(stream, line)) {
        // Remove whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        
        // Check for section
        if (line[0] == '[' && line[line.length() - 1] == ']') {
            section = line.substr(1, line.length() - 2);
            continue;
        }
        
        // Parse key=value
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }
        
        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);
        
        // Trim key and value
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        
        if (section == "devices") {
            if (key == "keyboard") {
                keyboard_device = value;
            } else if (key == "mouse") {
                mouse_device = value;
            }
        } else if (section == "sensitivity") {
            if (key == "sensitivity") {
                int val = std::stoi(value);
                // Clamp to valid range
                if (val < 1) val = 1;
                if (val > 100) val = 100;
                sensitivity = val;
            }
        } else if (section == "button_mapping") {
            // Map button code to key name (format: BUTTON=KEY)
            int button_code = -1;
            if (key == "BTN_TRIGGER") button_code = BTN_TRIGGER;
            else if (key == "BTN_THUMB") button_code = BTN_THUMB;
            else if (key == "BTN_THUMB2") button_code = BTN_THUMB2;
            else if (key == "BTN_TOP") button_code = BTN_TOP;
            else if (key == "BTN_TOP2") button_code = BTN_TOP2;
            else if (key == "BTN_PINKIE") button_code = BTN_PINKIE;
            else if (key == "BTN_BASE") button_code = BTN_BASE;
            else if (key == "BTN_BASE2") button_code = BTN_BASE2;
            else if (key == "BTN_BASE3") button_code = BTN_BASE3;
            else if (key == "BTN_BASE4") button_code = BTN_BASE4;
            else if (key == "BTN_BASE5") button_code = BTN_BASE5;
            else if (key == "BTN_BASE6") button_code = BTN_BASE6;
            else if (key == "BTN_DEAD") button_code = BTN_DEAD;
            else if (key == "BTN_TRIGGER_HAPPY1") button_code = BTN_TRIGGER_HAPPY1;
            else if (key == "BTN_TRIGGER_HAPPY2") button_code = BTN_TRIGGER_HAPPY2;
            else if (key == "BTN_TRIGGER_HAPPY3") button_code = BTN_TRIGGER_HAPPY3;
            else if (key == "BTN_TRIGGER_HAPPY4") button_code = BTN_TRIGGER_HAPPY4;
            else if (key == "BTN_TRIGGER_HAPPY5") button_code = BTN_TRIGGER_HAPPY5;
            else if (key == "BTN_TRIGGER_HAPPY6") button_code = BTN_TRIGGER_HAPPY6;
            else if (key == "BTN_TRIGGER_HAPPY7") button_code = BTN_TRIGGER_HAPPY7;
            else if (key == "BTN_TRIGGER_HAPPY8") button_code = BTN_TRIGGER_HAPPY8;
            else if (key == "BTN_TRIGGER_HAPPY9") button_code = BTN_TRIGGER_HAPPY9;
            else if (key == "BTN_TRIGGER_HAPPY10") button_code = BTN_TRIGGER_HAPPY10;
            else if (key == "BTN_TRIGGER_HAPPY11") button_code = BTN_TRIGGER_HAPPY11;
            else if (key == "BTN_TRIGGER_HAPPY12") button_code = BTN_TRIGGER_HAPPY12;
            
            if (button_code != -1) {
                button_map[value] = button_code;
            }
        }
    }
}

void Config::SaveDefault(const char* path) {
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "Failed to create config file: " << path << std::endl;
        return;
    }
    
    file << "# Wheel Emulator Configuration\n";
    file << "# Run with --detect flag to identify your devices\n\n";
    
    file << "[devices]\n";
    file << "# Specify exact device paths (use --detect to find them)\n";
    file << "# Leave empty for auto-detection\n";
    file << "keyboard=\n";
    file << "mouse=\n\n";
    
    file << "[sensitivity]\n";
    file << "sensitivity=50\n\n";
    
    file << "[controls]\n";
    file << "# Logitech G29 Racing Wheel Controls\n";
    file << "# Format: CONTROL=KEYBOARD_KEY or MOUSE_BUTTON\n\n";
    file << "# Primary Controls (Hardcoded)\n";
    file << "# Steering: Mouse horizontal movement\n";
    file << "# Throttle: Hold KEY_W to increase (0-100%)\n";
    file << "# Brake: Hold KEY_S to increase (0-100%)\n";
    file << "# D-Pad: Arrow keys (KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT)\n\n";
    
    file << "[button_mapping]\n";
    file << "# Logitech G29 Racing Wheel - Button Mappings (HARDCODED - for reference only)\n";
    file << "# Format: EMULATED_BUTTON=KEYBOARD_KEY\n";
    file << "# Note: These mappings are currently hardcoded in the source code.\n";
    file << "#       Editing this section will NOT change the actual mappings.\n";
    file << "#       This is for documentation purposes.\n\n";
    
    file << "# === CURRENT BUTTON MAPPINGS ===\n";
    file << "# Button 1:  BTN_TRIGGER        = KEY_Q\n";
    file << "# Button 2:  BTN_THUMB          = KEY_E\n";
    file << "# Button 3:  BTN_THUMB2         = KEY_F\n";
    file << "# Button 4:  BTN_TOP            = KEY_G\n";
    file << "# Button 5:  BTN_TOP2           = KEY_H\n";
    file << "# Button 6:  BTN_PINKIE         = KEY_R\n";
    file << "# Button 7:  BTN_BASE           = KEY_T\n";
    file << "# Button 8:  BTN_BASE2          = KEY_Y\n";
    file << "# Button 9:  BTN_BASE3          = KEY_U\n";
    file << "# Button 10: BTN_BASE4          = KEY_I\n";
    file << "# Button 11: BTN_BASE5          = KEY_O\n";
    file << "# Button 12: BTN_BASE6          = KEY_P\n";
    file << "# Button 13: BTN_DEAD           = KEY_1\n";
    file << "# Button 14: BTN_TRIGGER_HAPPY1 = KEY_2\n";
    file << "# Button 15: BTN_TRIGGER_HAPPY2 = KEY_3\n";
    file << "# Button 16: BTN_TRIGGER_HAPPY3 = KEY_4\n";
    file << "# Button 17: BTN_TRIGGER_HAPPY4 = KEY_5\n";
    file << "# Button 18: BTN_TRIGGER_HAPPY5 = KEY_6\n";
    file << "# Button 19: BTN_TRIGGER_HAPPY6 = KEY_7\n";
    file << "# Button 20: BTN_TRIGGER_HAPPY7 = KEY_8\n";
    file << "# Button 21: BTN_TRIGGER_HAPPY8 = KEY_9\n";
    file << "# Button 22: BTN_TRIGGER_HAPPY9 = KEY_0\n";
    file << "# Button 23: BTN_TRIGGER_HAPPY10 = KEY_LEFTSHIFT\n";
    file << "# Button 24: BTN_TRIGGER_HAPPY11 = KEY_SPACE\n";
    file << "# Button 25: BTN_TRIGGER_HAPPY12 = KEY_TAB\n\n";
    
    file << "# === AXES (Read-only, automatically handled) ===\n";
    file << "# ABS_X: Steering wheel (-32768 to 32767, mouse horizontal)\n";
    file << "# ABS_Y: Unused (always 32767, matches real G29)\n";
    file << "# ABS_Z: Brake pedal (32767 at rest, -32768 when fully pressed, KEY_S)\n";
    file << "# ABS_RZ: Throttle pedal (32767 at rest, -32768 when fully pressed, KEY_W)\n";
    file << "# ABS_HAT0X: D-Pad horizontal (-1, 0, 1) - Arrow LEFT/RIGHT\n";
    file << "# ABS_HAT0Y: D-Pad vertical (-1, 0, 1) - Arrow UP/DOWN\n\n";
    
    file << "# === PRIMARY CONTROLS (Hardcoded) ===\n";
    file << "# Steering: Mouse horizontal movement (sensitivity adjustable above)\n";
    file << "# Throttle: Hold KEY_W (analog ramping 0-100%)\n";
    file << "# Brake: Hold KEY_S (analog ramping 0-100%)\n";
    file << "# D-Pad: Arrow keys (UP/DOWN/LEFT/RIGHT)\n";
    file << "# Toggle Emulation: CTRL+M (enable/disable input grabbing)\n";
}

bool Config::UpdateDevices(const std::string& kbd_path, const std::string& mouse_path) {
    const char* config_path = "/etc/wheel-emulator.conf";
    
    // Read existing config
    std::ifstream infile(config_path);
    if (!infile.is_open()) {
        std::cerr << "Failed to open config for updating: " << config_path << std::endl;
        return false;
    }
    
    std::vector<std::string> lines;
    std::string line;
    std::string current_section;
    bool updated_keyboard = false;
    bool updated_mouse = false;
    
    while (std::getline(infile, line)) {
        // Track current section
        if (!line.empty() && line[0] == '[') {
            current_section = line;
            lines.push_back(line);
            continue;
        }
        
        // Update device lines in [devices] section
        if (current_section.find("[devices]") != std::string::npos) {
            if (line.find("keyboard=") != std::string::npos) {
                lines.push_back("keyboard=" + kbd_path);
                updated_keyboard = true;
                continue;
            } else if (line.find("mouse=") != std::string::npos) {
                lines.push_back("mouse=" + mouse_path);
                updated_mouse = true;
                continue;
            }
        }
        
        lines.push_back(line);
    }
    infile.close();
    
    // Write updated config
    std::ofstream outfile(config_path);
    if (!outfile.is_open()) {
        std::cerr << "Failed to write updated config: " << config_path << std::endl;
        return false;
    }
    
    for (const auto& l : lines) {
        outfile << l << "\n";
    }
    outfile.close();
    
    if (updated_keyboard && updated_mouse) {
        std::cout << "\nConfig updated successfully at " << config_path << std::endl;
        return true;
    } else {
        std::cerr << "Warning: Could not find device entries in config" << std::endl;
        return false;
    }
}
