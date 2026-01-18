# Build Stage
FROM ubuntu:22.04 AS builder

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Update and install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libboost-all-dev \
    librdkafka-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy source code and Makefile
COPY . .

# Build consumer (and producer, though producer usually runs on host)
# Note: We manually invoke g++ commands similar to your Makefile or use make if generic
# Since your Makefile might be Windows specific (linking -lws2_32), we'll compile manually here for Linux
RUN mkdir -p build && \
    g++ -std=c++17 -Wall -Iinclude -o consumer src/consumer.cpp src/Utils.cpp -lrdkafka -lpthread && \
    g++ -std=c++17 -Wall -Iinclude -o us_acq src/main.cpp src/USBuilder.cpp src/Utils.cpp -lrdkafka -lpthread

# Runtime Stage
FROM ubuntu:22.04

# Install runtime dependencies (check shared libs for rdkafka)
RUN apt-get update && apt-get install -y \
    librdkafka1 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy executables from builder
COPY --from=builder /app/consumer .
COPY --from=builder /app/us_acq .

# Create data directory for CSV output
RUN mkdir -p data

# Default command runs the consumer
CMD ["./consumer"]
