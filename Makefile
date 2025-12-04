CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
TARGET = wheel-emulator
SOURCES = src/main.cpp src/config.cpp src/input/device_enumerator.cpp src/input/device_scanner.cpp src/input/input_manager.cpp \
	src/wheel_device.cpp src/logging/logger.cpp src/hid/hid_device.cpp
OBJECTS = $(SOURCES:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

.PHONY: all clean install
