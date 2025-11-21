# Compiler settings
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Iinclude

# Platform detection
UNAME_S := $(shell uname -s)

# Platform-specific configuration
ifeq ($(UNAME_S),Darwin)
    # macOS (Homebrew)
    BOOST_ROOT = /opt/homebrew/opt/boost
    PORT_EXAMPLE = /dev/tty.usbmodem31101
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
    PORT_EXAMPLE = COM4
    PLATFORM = Windows
    INSTALL_CMD = vcpkg install boost-asio:x64-windows
endif

# Include and library paths
CXXFLAGS += -I$(BOOST_ROOT)/include

# Base linker flags
LDFLAGS = -L$(BOOST_ROOT)/lib -pthread

# On non-macOS platforms, add boost_system library
ifeq ($(UNAME_S),Darwin)
    # macOS: do NOT link -lboost_system
else
    LDFLAGS += -lboost_system
endif

# Directories
SRCDIR = src
INCDIR = include
BUILDDIR = build

# App name
APPNAME = us_acq

# Sources and objects
SRC = $(wildcard $(SRCDIR)/*.cpp)
OBJ = $(patsubst $(SRCDIR)/%.cpp, $(BUILDDIR)/%.o, $(SRC))

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
		exit 1; \
	fi
	@echo "Boost found at $(BOOST_ROOT)"
	@echo ""
	@echo "Example serial port for $(PLATFORM): $(PORT_EXAMPLE)"
	@echo "   Update portName in main.cpp if needed"
	@echo ""
	@echo "=========================================="

# Link step
$(APPNAME): $(OBJ)
	@echo "Linking $(APPNAME)..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build successful!"
	@echo ""
	@echo "Run with: ./$(APPNAME)"
	@echo ""

# Compile step (make sure build dir exists)
$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Create build directory if missing
$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Clean
clean:
	@echo "Cleaning build artifacts..."
	rm -rf $(BUILDDIR) $(APPNAME) $(APPNAME).exe
	@echo "Clean complete"

# Help
.PHONY: help
help:
	@echo "Available targets:"
	@echo "  make          - Check dependencies and build"
	@echo "  make clean    - Remove build artifacts"
	@echo "  make help     - Show this help message"
	@echo ""
	@echo "Current platform: $(PLATFORM)"
	@echo "Port example: $(PORT_EXAMPLE)"
