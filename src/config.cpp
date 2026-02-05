#include "config.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <sys/stat.h>

bool Config::Load() {
    const char* system_config = "./wheel-emulator.conf";

    if (LoadFromFile(system_config)) {
        std::cout << "Loaded config from: " << system_config << std::endl;
        return true;
    }
    
    std::cout << "No config found, generating default at " << system_config << std::endl;
    SaveDefault(system_config);
    std::cout << "Default config saved." << std::endl;
    
    sensitivity = 50;
    ffb_gain = 0.3f;
    
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
        
        if (section == "sensitivity") {
            if (key == "sensitivity") {
                int val = std::stoi(value);
                if (val < 1) val = 1;
                if (val > 100) val = 100;
                sensitivity = val;
            }
        } else if (section == "ffb") {
            if (key == "gain") {
                float val = std::stof(value);
                if (val < 0.1f) val = 0.1f;
                if (val > 4.0f) val = 4.0f;
                ffb_gain = val;
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
    
    file << "# Wheel Emulator Configuration (Windows / vJoy)\n\n";
    
    file << "[sensitivity]\n";
    file << "sensitivity=50\n\n";

    file << "[ffb]\n";
    file << "# Overall force feedback strength multiplier (0.1 - 4.0)\n";
    file << "gain=0.3\n\n";
    
    file << "# === CONTROLS (Hardcoded) ===\n";
    file << "# Steering: Mouse horizontal movement (sensitivity adjustable above)\n";
    file << "# Throttle: Hold W (analog ramping 0-100%)\n";
    file << "# Brake: Hold S (analog ramping 0-100%)\n";
    file << "# Clutch: Hold A (analog ramping 0-100%)\n";
    file << "# D-Pad: Arrow keys (UP/DOWN/LEFT/RIGHT)\n";
    file << "# Toggle Emulation: Ctrl+M\n";
    file << "#\n";
    file << "# === BUTTON MAPPINGS (Hardcoded) ===\n";
    file << "# Button 1:  Q              Button 2:  E\n";
    file << "# Button 3:  F              Button 4:  G\n";
    file << "# Button 5:  H              Button 6:  R\n";
    file << "# Button 7:  T              Button 8:  Y\n";
    file << "# Button 9:  U              Button 10: I\n";
    file << "# Button 11: O              Button 12: P\n";
    file << "# Button 13: 1              Button 14: 2\n";
    file << "# Button 15: 3              Button 16: 4\n";
    file << "# Button 17: 5              Button 18: 6\n";
    file << "# Button 19: 7              Button 20: 8\n";
    file << "# Button 21: 9              Button 22: 0\n";
    file << "# Button 23: Left Shift     Button 24: Space\n";
    file << "# Button 25: Tab            Button 26: Enter\n";
    file << "#\n";
    file << "# NOTE: Real G29 has INVERTED pedals (32767=rest, -32768=pressed).\n";
    file << "#       Enable 'Invert Pedals' option in your game settings if needed.\n";
}
