#include "vjoy_loader.h"
#include <iostream>
#include <fstream>
#include <vector>

VJoyAPI vJoy = {0};

bool LoadVJoyLibrary() {
    if (vJoy.hModule) return true;

    // 1. Try to load from current directory first
    vJoy.hModule = LoadLibraryA("vJoyInterface.dll");

    // 2. If not found, try to extract from resource
    if (!vJoy.hModule) {
        HRSRC hRes = FindResource(NULL, "VJOY_DLL", RT_RCDATA);
        if (hRes) {
            HGLOBAL hData = LoadResource(NULL, hRes);
            DWORD size = SizeofResource(NULL, hRes);
            void* data = LockResource(hData);

            if (data && size > 0) {
                char tempPath[MAX_PATH];
                GetTempPathA(MAX_PATH, tempPath);
                std::string dllPath = std::string(tempPath) + "vJoyInterface_embedded.dll";

                std::ofstream file(dllPath, std::ios::binary);
                if (file.write((char*)data, size)) {
                    file.close();
                    vJoy.hModule = LoadLibraryA(dllPath.c_str());
                }
            }
        }
    }

    if (!vJoy.hModule) {
        std::cerr << "Failed to load vJoyInterface.dll" << std::endl;
        return false;
    }

    // Load function pointers
    vJoy.vJoyEnabled = (Func_vJoyEnabled)GetProcAddress(vJoy.hModule, "vJoyEnabled");
    vJoy.GetVJDStatus = (Func_GetVJDStatus)GetProcAddress(vJoy.hModule, "GetVJDStatus");
    vJoy.AcquireVJD = (Func_AcquireVJD)GetProcAddress(vJoy.hModule, "AcquireVJD");
    vJoy.RelinquishVJD = (Func_RelinquishVJD)GetProcAddress(vJoy.hModule, "RelinquishVJD");
    vJoy.ResetVJD = (Func_ResetVJD)GetProcAddress(vJoy.hModule, "ResetVJD");
    vJoy.UpdateVJD = (Func_UpdateVJD)GetProcAddress(vJoy.hModule, "UpdateVJD");
    vJoy.FfbRegisterGenCB = (Func_FfbRegisterGenCB)GetProcAddress(vJoy.hModule, "FfbRegisterGenCB");
    
    vJoy.Ffb_h_Type = (Func_Ffb_h_Type)GetProcAddress(vJoy.hModule, "Ffb_h_Type");
    vJoy.Ffb_h_Eff_Constant = (Func_Ffb_h_Eff_Constant)GetProcAddress(vJoy.hModule, "Ffb_h_Eff_Constant");
    vJoy.Ffb_h_EffOp = (Func_Ffb_h_EffOp)GetProcAddress(vJoy.hModule, "Ffb_h_EffOp");
    vJoy.Ffb_h_DevCtrl = (Func_Ffb_h_DevCtrl)GetProcAddress(vJoy.hModule, "Ffb_h_DevCtrl");
    vJoy.Ffb_h_Eff_Cond = (Func_Ffb_h_Eff_Cond)GetProcAddress(vJoy.hModule, "Ffb_h_Eff_Cond");

    // Check critical functions
    if (!vJoy.vJoyEnabled || !vJoy.AcquireVJD || !vJoy.UpdateVJD) {
        std::cerr << "Failed to load functions from vJoyInterface.dll" << std::endl;
        FreeLibrary(vJoy.hModule);
        vJoy.hModule = nullptr;
        return false;
    }

    return true;
}

void FreeVJoyLibrary() {
    if (vJoy.hModule) {
        FreeLibrary(vJoy.hModule);
        vJoy.hModule = nullptr;
    }
}
