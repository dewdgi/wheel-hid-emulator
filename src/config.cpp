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
    
    // Set default button mappings
    button_map["KEY_Q"] = BTN_A;
    button_map["KEY_E"] = BTN_B;
    button_map["KEY_F"] = BTN_X;
    button_map["KEY_G"] = BTN_Y;
    button_map["KEY_H"] = BTN_TL;
    
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
            // Map key name to button code
            int button_code = -1;
            if (value == "BTN_A") button_code = BTN_A;
            else if (value == "BTN_B") button_code = BTN_B;
            else if (value == "BTN_X") button_code = BTN_X;
            else if (value == "BTN_Y") button_code = BTN_Y;
            else if (value == "BTN_TL") button_code = BTN_TL;
            else if (value == "BTN_TR") button_code = BTN_TR;
            else if (value == "BTN_SELECT") button_code = BTN_SELECT;
            else if (value == "BTN_START") button_code = BTN_START;
            else if (value == "BTN_THUMBL") button_code = BTN_THUMBL;
            else if (value == "BTN_THUMBR") button_code = BTN_THUMBR;
            else if (value == "BTN_MODE") button_code = BTN_MODE;
            
            if (button_code != -1) {
                button_map[key] = button_code;
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
    
    file << "[button_mapping]\n";
    file << "# Xbox 360 Controller Button Mapping\n";
    file << "# Available buttons: BTN_A, BTN_B, BTN_X, BTN_Y\n";
    file << "#                   BTN_TL (L1), BTN_TR (R1)\n";
    file << "#                   BTN_SELECT (Back), BTN_START\n";
    file << "#                   BTN_THUMBL (L3), BTN_THUMBR (R3)\n";
    file << "#                   BTN_MODE (Xbox button)\n\n";
    file << "KEY_Q=BTN_A\n";
    file << "KEY_E=BTN_B\n";
    file << "KEY_F=BTN_X\n";
    file << "KEY_G=BTN_Y\n";
    file << "KEY_H=BTN_TL\n";
    file << "# KEY_R=BTN_TR\n";
    file << "# KEY_TAB=BTN_SELECT\n";
    file << "# KEY_ENTER=BTN_START\n";
    file << "# KEY_LEFTSHIFT=BTN_THUMBL\n";
    file << "# KEY_LEFTCTRL=BTN_THUMBR\n";
    file << "# KEY_ESC=BTN_MODE\n";
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
