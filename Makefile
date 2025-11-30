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
    LDFLAGS = -L$(BOOST_ROOT)/lib -pthread
else ifeq ($(findstring MINGW,$(UNAME_S)),MINGW)
    # Windows (MinGW/MSYS2)
    BOOST_ROOT = /mingw64
    PORT_EXAMPLE = COM3
    PLATFORM = Windows
    INSTALL_CMD = pacman -S mingw-w64-x86_64-boost
    LDFLAGS = -L/mingw64/lib -lws2_32 -lwsock32 -pthread
else
    # Fallback Windows (vcpkg)
    BOOST_ROOT = C:/vcpkg/installed/x64-windows
    PORT_EXAMPLE = COM3
    PLATFORM = Windows
    INSTALL_CMD = vcpkg install boost-asio:x64-windows
    LDFLAGS = -LC:/vcpkg/installed/x64-windows/lib -lboost_system-mt -lws2_32 -lwsock32 -pthread
endif

# Include paths
CXXFLAGS += -I$(BOOST_ROOT)/include

# Kafka (prefer pkg-config if available)
KAFKA_CFLAGS := $(shell command -v pkg-config >/dev/null 2>&1 && pkg-config --cflags rdkafka)
KAFKA_LDFLAGS := $(shell command -v pkg-config >/dev/null 2>&1 && pkg-config --libs rdkafka)

CXXFLAGS += $(KAFKA_CFLAGS)
LDFLAGS += $(KAFKA_LDFLAGS)

# Fallback when pkg-config is unavailable
ifeq ($(strip $(KAFKA_LDFLAGS)),)
    LDFLAGS += -lrdkafka
endif

# Directories
SRCDIR = src
INCDIR = include
BUILDDIR = build

# App names
APPNAME = us_acq
CONSUMER_APP = consumer

# Sources and objects
SRC = $(wildcard $(SRCDIR)/*.cpp)
APP_SRC = $(filter-out $(SRCDIR)/consumer.cpp, $(SRC))
APP_OBJ = $(patsubst $(SRCDIR)/%.cpp, $(BUILDDIR)/%.o, $(APP_SRC))
CONSUMER_SRC = $(SRCDIR)/consumer.cpp
CONSUMER_OBJ = $(BUILDDIR)/consumer.o
COMMON_OBJ = $(BUILDDIR)/Utils.o

# Default target
all: check-deps $(APPNAME) $(CONSUMER_APP)

# Dependency check
.PHONY: check-deps
check-deps:
	@echo "=========================================="
	@echo "Checking dependencies for $(PLATFORM)..."
	@echo "=========================================="
	@echo ""
	@if [ ! -d "$(BOOST_ROOT)/include/boost" ]; then \
		echo "❌ ERROR: Boost not found at $(BOOST_ROOT)"; \
		echo ""; \
		echo "📦 Installation instructions:"; \
		echo "   $(INSTALL_CMD)"; \
		echo ""; \
		exit 1; \
	fi
	@echo "✅ Boost found at $(BOOST_ROOT)"
	@echo ""
	@echo "📍 Example serial port for $(PLATFORM): $(PORT_EXAMPLE)"
	@echo "   Update portName in main.cpp if needed"
	@echo ""
	@echo "=========================================="

# Link step
$(APPNAME): $(APP_OBJ)
	@echo "Linking $(APPNAME)..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✅ Build successful!"
	@echo ""
	@echo "Run with: ./$(APPNAME)"
	@echo ""

$(CONSUMER_APP): $(CONSUMER_OBJ) $(COMMON_OBJ)
	@echo "Linking $(CONSUMER_APP)..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✅ Consumer build successful!"
	@echo ""
	@echo "Run with: ./$(CONSUMER_APP)"
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
	rm -rf $(BUILDDIR) $(APPNAME) $(APPNAME).exe $(CONSUMER_APP) $(CONSUMER_APP).exe
	@echo "✅ Clean complete"

# Help
.PHONY: help
help:
	@echo "Available targets:"
	@echo "  make               - Check dependencies and build producer+consumer"
	@echo "  make us_acq        - Build only the producer"
	@echo "  make consumer      - Build only the consumer"
	@echo "  make clean         - Remove build artifacts"
	@echo "  make help          - Show this help message"
	@echo ""
	@echo "Current platform: $(PLATFORM)"
	@echo "Port example: $(PORT_EXAMPLE)"
