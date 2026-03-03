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
    g++ -std=c++17 -Wall -Iinclude -o output src/ml_model.cpp src/Utils.cpp -lrdkafka -lpthread && \
    g++ -std=c++17 -Wall -Iinclude -o raw_sink src/raw_sink.cpp -lrdkafka -lpthread && \
    g++ -std=c++17 -Wall -Iinclude -o processed_sink src/processed_sink.cpp src/Utils.cpp -lrdkafka -lpthread && \
    g++ -std=c++17 -Wall -Iinclude -o replay_raw src/replay_raw.cpp -lrdkafka -lpthread && \
    g++ -std=c++17 -Wall -Iinclude -o replay_processed src/replay_processed.cpp -lrdkafka -lpthread && \
    g++ -std=c++17 -Wall -Iinclude -o prediction_consumer src/prediction_consumer.cpp -lrdkafka -lpthread

# Runtime Stage
FROM ubuntu:22.04

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    librdkafka1 \
    libarmadillo10 \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

# Install google-cloud-cli to provide gsutil and gcloud commands
RUN apt-get update && apt-get install -y apt-transport-https ca-certificates gnupg curl && \
    curl https://packages.cloud.google.com/apt/doc/apt-key.gpg | gpg --dearmor -o /usr/share/keyrings/cloud.google.gpg && \
    echo "deb [signed-by=/usr/share/keyrings/cloud.google.gpg] https://packages.cloud.google.com/apt cloud-sdk main" | tee -a /etc/apt/sources.list.d/google-cloud-sdk.list && \
    apt-get update && apt-get install -y google-cloud-cli && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy executables from builder
COPY --from=builder /app/us_acq .
COPY --from=builder /app/preprocess .
COPY --from=builder /app/output .
COPY --from=builder /app/raw_sink .
COPY --from=builder /app/processed_sink .
COPY --from=builder /app/replay_raw .
COPY --from=builder /app/replay_processed .
COPY --from=builder /app/prediction_consumer .

# Create data directory for CSV output
RUN mkdir -p data

# Default command (can be overridden by docker-compose)
CMD ["./preprocess"]


