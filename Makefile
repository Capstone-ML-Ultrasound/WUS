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
    ARMA_INSTALL_CMD = brew install armadillo
    LDFLAGS = -L$(BOOST_ROOT)/lib -pthread
else ifeq ($(findstring MINGW,$(UNAME_S)),MINGW)
    # Windows (MinGW/MSYS2)
    BOOST_ROOT = /mingw64
    RDKAFKA_ROOT = /mingw64
    PORT_EXAMPLE = COM3
    PLATFORM = Windows
    INSTALL_CMD = pacman -S --noconfirm mingw-w64-x86_64-boost
    RDKAFKA_INSTALL_CMD = pacman -S --noconfirm mingw-w64-x86_64-librdkafka
    ARMA_INSTALL_CMD = pacman -S --noconfirm mingw-w64-x86_64-armadillo
    LDFLAGS = -L/mingw64/lib -lws2_32 -lwsock32 -pthread
else
    # Fallback Windows (vcpkg)
    BOOST_ROOT = C:/vcpkg/installed/x64-windows
    RDKAFKA_ROOT = C:/vcpkg/installed/x64-windows
    PORT_EXAMPLE = COM3
    PLATFORM = Windows
    INSTALL_CMD = vcpkg install boost-asio:x64-windows
    RDKAFKA_INSTALL_CMD = vcpkg install librdkafka:x64-windows
    ARMA_INSTALL_CMD = echo "Please install armadillo manually via vcpkg"
    LDFLAGS = -LC:/vcpkg/installed/x64-windows/lib -lboost_system-mt -lws2_32 -lwsock32 -pthread
endif

# Include paths
CXXFLAGS += -I$(BOOST_ROOT)/include

# Kafka (prefer pkg-config if available)
KAFKA_CFLAGS := $(shell command -v pkg-config >/dev/null 2>&1 && pkg-config --cflags rdkafka)
KAFKA_LDFLAGS := $(shell command -v pkg-config >/dev/null 2>&1 && pkg-config --libs rdkafka)

# Armadillo (prefer pkg-config if available)
ARMA_CFLAGS := $(shell command -v pkg-config >/dev/null 2>&1 && pkg-config --cflags armadillo)
ARMA_LDFLAGS := $(shell command -v pkg-config >/dev/null 2>&1 && pkg-config --libs armadillo)

CXXFLAGS += $(KAFKA_CFLAGS) $(ARMA_CFLAGS)
LDFLAGS += $(KAFKA_LDFLAGS) $(ARMA_LDFLAGS)

# Fallback when pkg-config is unavailable
ifeq ($(strip $(KAFKA_LDFLAGS)),)
    LDFLAGS += -lrdkafka
endif
ifeq ($(strip $(ARMA_LDFLAGS)),)
    LDFLAGS += -larmadillo
endif

# Directories
SRCDIR = src
INCDIR = include
BUILDDIR = build

# App names
APPNAME = us_acq
PREPROCESS_APP = preprocess
OUTPUT_APP = output
PREDICTION_CONSUMER_APP = prediction_consumer
PROCESSED_SINK_APP = processed_sink
REPLAY_RAW_APP = replay_raw
REPLAY_PROCESSED_APP = replay_processed

# Sources and objects
SRC = $(wildcard $(SRCDIR)/*.cpp)
# APP_SRC excludes service-specific files
APP_SRC = $(filter-out $(SRCDIR)/preprocess.cpp $(SRCDIR)/ml_model.cpp $(SRCDIR)/raw_sink.cpp $(SRCDIR)/prediction_consumer.cpp $(SRCDIR)/processed_sink.cpp $(SRCDIR)/replay_raw.cpp $(SRCDIR)/replay_processed.cpp, $(SRC))
APP_OBJ = $(patsubst $(SRCDIR)/%.cpp, $(BUILDDIR)/%.o, $(APP_SRC))

PREPROCESS_SRC = $(SRCDIR)/preprocess.cpp
PREPROCESS_OBJ = $(BUILDDIR)/preprocess.o

OUTPUT_SRC = $(SRCDIR)/ml_model.cpp
OUTPUT_OBJ = $(BUILDDIR)/output.o

RAW_SINK_SRC = $(SRCDIR)/raw_sink.cpp
RAW_SINK_OBJ = $(BUILDDIR)/raw_sink.o

PREDICTION_CONSUMER_SRC = $(SRCDIR)/prediction_consumer.cpp
PREDICTION_CONSUMER_OBJ = $(BUILDDIR)/prediction_consumer.o

PROCESSED_SINK_SRC = $(SRCDIR)/processed_sink.cpp
PROCESSED_SINK_OBJ = $(BUILDDIR)/processed_sink.o

REPLAY_RAW_SRC = $(SRCDIR)/replay_raw.cpp
REPLAY_RAW_OBJ = $(BUILDDIR)/replay_raw.o

REPLAY_PROCESSED_SRC = $(SRCDIR)/replay_processed.cpp
REPLAY_PROCESSED_OBJ = $(BUILDDIR)/replay_processed.o

COMMON_OBJ = $(BUILDDIR)/Utils.o

# Default target
all: check-deps $(APPNAME) $(PREPROCESS_APP) $(OUTPUT_APP) raw_sink $(PREDICTION_CONSUMER_APP) $(PROCESSED_SINK_APP) $(REPLAY_RAW_APP) $(REPLAY_PROCESSED_APP)

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
	@if [ ! -f "$(RDKAFKA_ROOT)/include/armadillo" ]; then \
		echo "📦 Armadillo not found. Installing..."; \
		$(ARMA_INSTALL_CMD); \
	fi
	@echo "✅ Armadillo found"
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

$(PREPROCESS_APP): $(PREPROCESS_OBJ) $(COMMON_OBJ)
	@echo "Linking $(PREPROCESS_APP)..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✅ Preprocess build successful!"
	@echo ""
	@echo "Run with: ./$(PREPROCESS_APP)"
	@echo ""

$(OUTPUT_APP): $(OUTPUT_OBJ) $(COMMON_OBJ)
	@echo "Linking $(OUTPUT_APP)..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✅ Output build successful!"
	@echo ""
	@echo "Run with: ./$(OUTPUT_APP)"
	@echo ""

raw_sink: $(RAW_SINK_OBJ) $(COMMON_OBJ)
	@echo "Linking raw_sink..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✅ Raw sink build successful!"
	@echo ""
	@echo "Run with: ./raw_sink"
	@echo ""

$(PREDICTION_CONSUMER_APP): $(PREDICTION_CONSUMER_OBJ)
	@echo "Linking $(PREDICTION_CONSUMER_APP)..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✅ Prediction consumer build successful!"
	@echo ""
	@echo "Run with: ./$(PREDICTION_CONSUMER_APP)"
	@echo ""

$(PROCESSED_SINK_APP): $(PROCESSED_SINK_OBJ) $(COMMON_OBJ)
	@echo "Linking $(PROCESSED_SINK_APP)..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✅ Process sink build successful!"
	@echo ""
	@echo "Run with: ./$(PROCESSED_SINK_APP)"
	@echo ""

$(REPLAY_RAW_APP): $(REPLAY_RAW_OBJ)
	@echo "Linking $(REPLAY_RAW_APP)..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✅ Replay raw build successful!"
	@echo ""
	@echo "Run with: ./$(REPLAY_RAW_APP)"
	@echo ""

$(REPLAY_PROCESSED_APP): $(REPLAY_PROCESSED_OBJ)
	@echo "Linking $(REPLAY_PROCESSED_APP)..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✅ Replay processed build successful!"
	@echo ""
	@echo "Run with: ./$(REPLAY_PROCESSED_APP)"
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
	rm -rf $(BUILDDIR) $(APPNAME) $(APPNAME).exe $(PREPROCESS_APP) $(PREPROCESS_APP).exe $(OUTPUT_APP) $(OUTPUT_APP).exe $(PREDICTION_CONSUMER_APP) $(PREDICTION_CONSUMER_APP).exe $(PROCESSED_SINK_APP) $(PROCESSED_SINK_APP).exe $(REPLAY_RAW_APP) $(REPLAY_RAW_APP).exe $(REPLAY_PROCESSED_APP) $(REPLAY_PROCESSED_APP).exe
	@echo "✅ Clean complete"

# Help
.PHONY: help
help:
	@echo "Available targets:"
	@echo "  make               - Check dependencies and build producer+preprocess+output"
	@echo "  make us_acq        - Build only the producer"
	@echo "  make preprocess    - Build only the preprocess service"
	@echo "  make output        - Build only the output service"
	@echo "  make $(PROCESSED_SINK_APP) - Build only the processed-frame sink"
	@echo "  make $(PREDICTION_CONSUMER_APP) - Build only the model prediction consumer"
	@echo "  make $(REPLAY_RAW_APP) - Build only the raw replay source"
	@echo "  make $(REPLAY_PROCESSED_APP) - Build only the processed replay source"
	@echo "  make clean         - Remove build artifacts"
	@echo "  make help          - Show this help message"
	@echo ""
	@echo "Current platform: $(PLATFORM)"
	@echo "Port example: $(PORT_EXAMPLE)"


