# Compiler settings
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Iinclude

# Platform detection
UNAME_S := $(shell uname -s)

# Platform-specific configuration
ifeq ($(UNAME_S),Darwin)
    # macOS (Homebrew)
    BOOST_ROOT = /opt/homebrew/opt/boost
    RDKAFKA_ROOT = /opt/homebrew/opt/librdkafka
    PORT_EXAMPLE = /dev/tty.usbmodem31101
    PLATFORM = macOS
    INSTALL_CMD = brew install boost
    RDKAFKA_INSTALL_CMD = brew install librdkafka
    LDFLAGS = -L$(BOOST_ROOT)/lib -pthread
else ifeq ($(findstring MINGW,$(UNAME_S)),MINGW)
    # Windows (MinGW/MSYS2)
    BOOST_ROOT = /mingw64
    RDKAFKA_ROOT = /mingw64
    PORT_EXAMPLE = COM3
    PLATFORM = Windows
    INSTALL_CMD = pacman -S --noconfirm mingw-w64-x86_64-boost
    RDKAFKA_INSTALL_CMD = pacman -S --noconfirm mingw-w64-x86_64-librdkafka
    LDFLAGS = -L/mingw64/lib -lws2_32 -lwsock32 -pthread
else
    # Fallback Windows (vcpkg)
    BOOST_ROOT = C:/vcpkg/installed/x64-windows
    RDKAFKA_ROOT = C:/vcpkg/installed/x64-windows
    PORT_EXAMPLE = COM3
    PLATFORM = Windows
    INSTALL_CMD = vcpkg install boost-asio:x64-windows
    RDKAFKA_INSTALL_CMD = vcpkg install librdkafka:x64-windows
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
REPLAY_RAW_APP = replay_raw

# Sources and objects
SRC = $(wildcard $(SRCDIR)/*.cpp)
# APP_SRC excludes service-specific files
APP_SRC = $(filter-out $(SRCDIR)/raw_sink.cpp $(SRCDIR)/replay_raw.cpp, $(SRC))
APP_OBJ = $(patsubst $(SRCDIR)/%.cpp, $(BUILDDIR)/%.o, $(APP_SRC))

RAW_SINK_SRC = $(SRCDIR)/raw_sink.cpp
RAW_SINK_OBJ = $(BUILDDIR)/raw_sink.o

REPLAY_RAW_SRC = $(SRCDIR)/replay_raw.cpp
REPLAY_RAW_OBJ = $(BUILDDIR)/replay_raw.o

COMMON_OBJ = $(BUILDDIR)/Utils.o

# Default target (active runtime path)
all: check-deps $(APPNAME) raw_sink $(REPLAY_RAW_APP)

# Dependency check (auto-install if missing)
.PHONY: check-deps
check-deps:
	@echo "=========================================="
	@echo "Checking dependencies for $(PLATFORM)..."
	@echo "=========================================="
	@echo ""
	@if [ ! -d "$(BOOST_ROOT)/include/boost" ]; then \
		echo "📦 Boost not found. Installing..."; \
		$(INSTALL_CMD); \
	fi
	@echo "✅ Boost found at $(BOOST_ROOT)"
	@if [ ! -f "$(RDKAFKA_ROOT)/include/librdkafka/rdkafka.h" ]; then \
		echo "📦 librdkafka not found. Installing..."; \
		$(RDKAFKA_INSTALL_CMD); \
	fi
	@echo "✅ librdkafka found at $(RDKAFKA_ROOT)"
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

raw_sink: $(RAW_SINK_OBJ) $(COMMON_OBJ)
	@echo "Linking raw_sink..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✅ Raw sink build successful!"
	@echo ""
	@echo "Run with: ./raw_sink"
	@echo ""

$(REPLAY_RAW_APP): $(REPLAY_RAW_OBJ)
	@echo "Linking $(REPLAY_RAW_APP)..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✅ Replay raw build successful!"
	@echo ""
	@echo "Run with: ./$(REPLAY_RAW_APP)"
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
	rm -rf $(BUILDDIR) $(APPNAME) $(APPNAME).exe $(REPLAY_RAW_APP) $(REPLAY_RAW_APP).exe
	@echo "✅ Clean complete"

# Help
.PHONY: help
help:
	@echo "Available targets:"
	@echo "  make               - Build active runtime binaries (us_acq + raw_sink + replay_raw)"
	@echo "  make us_acq        - Build only the producer"
	@echo "  make $(REPLAY_RAW_APP) - Build only the raw replay source"
	@echo "  make raw_sink      - Build only the raw-frame sink"
	@echo "  make clean         - Remove build artifacts"
	@echo "  make help          - Show this help message"
	@echo ""
	@echo "Current platform: $(PLATFORM)"
	@echo "Port example: $(PORT_EXAMPLE)"


