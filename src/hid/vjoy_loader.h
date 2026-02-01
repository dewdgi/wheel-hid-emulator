#ifndef VJOY_LOADER_H
#define VJOY_LOADER_H

#include <windows.h>
#include "../vjoy_sdk/inc/public.h"
#include "../vjoy_sdk/inc/vjoyinterface.h"

// Define function pointers types matching vJoyInterface.h
typedef BOOL (__cdecl *Func_vJoyEnabled)(void);
typedef enum VjdStat (__cdecl *Func_GetVJDStatus)(UINT rID);
typedef BOOL (__cdecl *Func_AcquireVJD)(UINT rID);
typedef VOID (__cdecl *Func_RelinquishVJD)(UINT rID);
typedef BOOL (__cdecl *Func_ResetVJD)(UINT rID);
typedef BOOL (__cdecl *Func_UpdateVJD)(UINT rID, PVOID pData);
typedef VOID (__cdecl *Func_FfbRegisterGenCB)(FfbGenCB cb, PVOID data);

// FFB Helper functions
typedef DWORD (__cdecl *Func_Ffb_h_Type)(const FFB_DATA * Packet, FFBPType *Type);
typedef DWORD (__cdecl *Func_Ffb_h_Eff_Constant)(const FFB_DATA * Packet, FFB_EFF_CONSTANT * ConstantEffect);
typedef DWORD (__cdecl *Func_Ffb_h_EffOp)(const FFB_DATA * Packet, FFB_EFF_OP* Operation);
typedef DWORD (__cdecl *Func_Ffb_h_DevCtrl)(const FFB_DATA * Packet, FFB_CTRL * Control);
typedef DWORD (__cdecl *Func_Ffb_h_Eff_Cond)(const FFB_DATA * Packet, FFB_EFF_COND* Condition);

struct VJoyAPI {
    Func_vJoyEnabled vJoyEnabled;
    Func_GetVJDStatus GetVJDStatus;
    Func_AcquireVJD AcquireVJD;
    Func_RelinquishVJD RelinquishVJD;
    Func_ResetVJD ResetVJD;
    Func_UpdateVJD UpdateVJD;
    Func_FfbRegisterGenCB FfbRegisterGenCB;
    
    Func_Ffb_h_Type Ffb_h_Type;
    Func_Ffb_h_Eff_Constant Ffb_h_Eff_Constant;
    Func_Ffb_h_EffOp Ffb_h_EffOp;
    Func_Ffb_h_DevCtrl Ffb_h_DevCtrl;
    Func_Ffb_h_Eff_Cond Ffb_h_Eff_Cond;

    bool IsLoaded() const { return hModule != nullptr; }
    HMODULE hModule = nullptr;
};

extern VJoyAPI vJoy;

// Loads the DLL from embedded resource or disk
bool LoadVJoyLibrary();
void FreeVJoyLibrary();

#endif
