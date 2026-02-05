@echo off
echo Attempting to build with g++...

where g++ >nul 2>nul
if %errorlevel% neq 0 (
    echo Error: g++ is not found in your PATH. 
    echo Please install MinGW-w64 and add it to your PATH.
    pause
    exit /b 1
)

if not exist "src\vjoy_sdk\inc\vjoyinterface.h" (
    echo Error: vJoy headers not found in src\vjoy_sdk\inc
    exit /b 1
)

echo Compiling Resources...
windres vjoy_dll.rc -o vjoy_dll.o
if %errorlevel% neq 0 (
    echo Resource compilation failed!
    exit /b 1
)

echo Compiling Code...
g++ -std=c++17 ^
    src/main.cpp ^
    src/config.cpp ^
    src/wheel_device.cpp ^
    src/hid/hid_device.cpp ^
    src/hid/vjoy_loader.cpp ^
    src/logging/logger.cpp ^
    src/input/device_scanner.cpp ^
    src/input/input_manager.cpp ^
    vjoy_dll.o ^
    -I src/vjoy_sdk/inc ^
    -lwinmm ^
    -o wheel-emulator.exe ^
    -static-libgcc -static-libstdc++ -lpthread

if %errorlevel% neq 0 (
    echo.
    echo Build FAILED!
    exit /b %errorlevel%
)

echo.
echo Build SUCCESS! Run wheel-emulator.exe.
echo (vJoyInterface.dll is embedded and will be extracted automatically)
del vjoy_dll.o

