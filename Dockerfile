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
    libarmadillo-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy source code
COPY . .

# Build executables
RUN mkdir -p build && \
    g++ -std=c++17 -Wall -Iinclude -o us_acq src/main.cpp src/USBuilder.cpp src/Utils.cpp -lrdkafka -lpthread && \
    g++ -std=c++17 -Wall -Iinclude -o preprocess src/preprocess.cpp src/Utils.cpp -lrdkafka -larmadillo -lpthread && \
    g++ -std=c++17 -Wall -Iinclude -o output src/output.cpp src/Utils.cpp -lrdkafka -lpthread

# Runtime Stage
FROM ubuntu:22.04

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    librdkafka1 \
    libarmadillo10 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy executables from builder
COPY --from=builder /app/us_acq .
COPY --from=builder /app/preprocess .
COPY --from=builder /app/output .

# Create data directory for CSV output
RUN mkdir -p data

# Default command (can be overridden by docker-compose)
CMD ["./preprocess"]
