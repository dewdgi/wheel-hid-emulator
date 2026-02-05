#ifndef CONFIG_H
#define CONFIG_H

#include <string>

class Config {
public:
    int sensitivity = 50;
    float ffb_gain = 0.3f;
    
    // Load configuration from default locations
    // Returns true if successful, false otherwise
    bool Load();
    
    // Save default configuration to specified path
    void SaveDefault(const char* path);
    
private:
    bool LoadFromFile(const char* path);
    void ParseINI(const std::string& content);
};

#endif // CONFIG_H
