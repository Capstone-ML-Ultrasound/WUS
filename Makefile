# Compiler settings
CXX = g++
CXXFLAGS = -std=c++17 -Wall

# Platform detection
UNAME_S := $(shell uname -s)

# Platform-specific configuration
ifeq ($(UNAME_S),Darwin)
    # macOS (Homebrew)
    BOOST_ROOT = /opt/homebrew/opt/boost
    PORT_EXAMPLE = /dev/tty.usbserial-0001
    PLATFORM = macOS
    INSTALL_CMD = brew install boost
else ifeq ($(UNAME_S),Linux)
    # Linux
    BOOST_ROOT = /usr
    PORT_EXAMPLE = /dev/ttyUSB0
    PLATFORM = Linux
    INSTALL_CMD = sudo apt-get install libboost-all-dev
else
    # Windows (MinGW)
    BOOST_ROOT = C:/vcpkg/installed/x64-windows
    PORT_EXAMPLE = COM3
    PLATFORM = Windows
    INSTALL_CMD = vcpkg install boost-asio:x64-windows
endif

# Include and library paths
CXXFLAGS += -I$(BOOST_ROOT)/include

# Base linker flags (always include pthread and library path)
LDFLAGS = -L$(BOOST_ROOT)/lib -pthread

# On non-macOS platforms add boost_system library
ifeq ($(UNAME_S),Darwin)
    # macOS: do NOT link -lboost_system (header-only)
else
    LDFLAGS += -lboost_system
endif

# Directories
SRCDIR = src
INCDIR = include
BUILDDIR = build

# App name
APPNAME = test_serial

# Default target
all: check-deps $(APPNAME)

# Dependency check
.PHONY: check-deps
check-deps:
	@echo "=========================================="
	@echo "Checking dependencies for $(PLATFORM)..."
	@echo "=========================================="
	@echo ""
	@if [ ! -d "$(BOOST_ROOT)/include/boost" ]; then \
		echo "ERROR: Boost not found at $(BOOST_ROOT)"; \
		echo ""; \
		echo "Installation instructions:"; \
		echo "   $(INSTALL_CMD)"; \
		echo ""; \
		if [ "$(PLATFORM)" = "macOS" ]; then \
			echo "   If Homebrew is not installed:"; \
			echo "   /bin/bash -c \"\$$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\""; \
		elif [ "$(PLATFORM)" = "Windows" ]; then \
			echo "   If vcpkg is not installed:"; \
			echo "   git clone https://github.com/microsoft/vcpkg"; \
			echo "   cd vcpkg && bootstrap-vcpkg.bat"; \
			echo "   vcpkg integrate install"; \
		fi; \
		echo ""; \
		exit 1; \
	fi
	@echo "Boost found at $(BOOST_ROOT)"
	@echo ""
	@echo "Example serial port for $(PLATFORM): $(PORT_EXAMPLE)"
	@echo "   Update portName in test_serial.cpp if needed"
	@echo ""
	@echo "=========================================="


# Build test_serial
$(APPNAME): test_serial.cpp
	@echo "Building $(APPNAME)..."
	$(CXX) $(CXXFLAGS) -o $(APPNAME) test_serial.cpp $(LDFLAGS)
	@echo "Build successful!"
	@echo ""
	@echo "Run with: ./$(APPNAME)"
	@echo ""

# Clean
clean:
	@echo "Cleaning build artifacts..."
	rm -f $(APPNAME) $(APPNAME).exe
	@echo "Clean complete"

# Help
.PHONY: help
help:
	@echo "Available targets:"
	@echo "  make          - Check dependencies and build"
	@echo "  make clean    - Remove build artifacts"
	@echo "  make help     - Show this help message"
	@echo ""
	@echo "First time setup:"
	@echo "  1. Install Boost: $(INSTALL_CMD)"
	@echo "  2. Find your serial port:"
	@echo "     $(PLATFORM): $(PORT_EXAMPLE)"
	@echo "  3. Update portName in test_serial.cpp"
	@echo "  4. Run: make"
