# Wireless Ultrasound System (WUS)

This project implements a streaming data pipeline for ultrasound A-scan frames. It captures data from hardware, processes it in real-time (TGC, Envelope Detection, Log Compression, Gaussian Blur), and persists the results.

## � Architecture

The pipeline consists of four main components connected via **Apache Kafka**:

1.  **Producer (`us_acq`)**:
    *   Connects to the ultrasound hardware via USB/Serial.
    *   Acquires raw A-scans via SPI Function 4 (Auto-sampling).
    *   Encodes raw data with `USFrameProtocol` (V1).
    *   Publishes to Kafka topic: `ultrasound_raw_data`.

2.  **Preprocess Service (`preprocess`)**:
    *   Consumes raw frames.
    *   **Signal Processing**:
        *   Time Gain Compensation (TGC).
        *   DC Removal.
        *   Envelope Detection (Hilbert Transform via Armadillo).
        *   Log Compression.
        *   **Separable Gaussian Blur**: Using a sliding window buffer (~30 frames) to apply smoothing in both Depth (1D) and Time (Weighted Average).
    *   Publishes processed frames to Kafka topic: `ultrasound.clean`.
    *   Uses `USProcessedFrameHeader` (V2 Extension) to track provenance.

3.  **Output Service (`output`)**:
    *   Consumes processed frames from `ultrasound.clean`.
    *   Writes frames to CSV file (`ultrasound_output.csv`).

4.  **Raw Sink (`raw_sink`)**:
    *   Consumes immutable raw frames from `ultrasound_raw_data`.
    *   Writes burst CSV files and uploads them to Google Cloud Storage for replayability.
    *   Optional local retention controlled by `RAW_GCS_KEEP_LOCAL`.

5.  **Kafka**: Message broker handling the stream.

---

## 🐳 Docker Quick Start (Recommended)

You can run the processing and output backend in Docker.

1.  **Start Services**:
    ```bash
    docker-compose up --build
    ```
    *   Starts **Kafka**, **Preprocess**, **Output**, and **RawSink** containers.
    *   **Data** is saved to the `./data` folder on your host machine.

2.  **Enable object-store archive for raw replay (Google Cloud Storage)**:
    Set these environment variables before `docker-compose up`:
    ```bash
    export RAW_GCS_BUCKET=my-ultrasound-raw-archive
    export RAW_GCS_PREFIX=ultrasound/raw
    export RAW_GCS_KEEP_LOCAL=false
    export GCP_PROJECT_ID=my-gcp-project

    # Path inside the container (mounted from ./secrets by docker-compose)
    export GOOGLE_APPLICATION_CREDENTIALS=/app/secrets/gcp-sa.json
    ```
    If `RAW_GCS_BUCKET` is empty, `raw_sink` keeps writing local CSV only.

3.  **Run Producer (Host Machine)**:
    *   *Note: The producer must run on the host to access the USB hardware (COM port).*
    *   **Compile**:
        ```bash
        make us_acq
        ```
    *   **Run**:
        ```bash
        ./us_acq.exe
        ```

---

## 🛠 Manual Setup (Windows/MinGW)

If you prefer to run everything locally without Docker:

### 1. Prerequisites
*   **librdkafka** (Kafka C library):
    ```bash
    pacman -S mingw-w64-x86_64-librdkafka
    ```
*   **Armadillo** (Linear Algebra & Signal Processing):
    ```bash
    pacman -S mingw-w64-x86_64-armadillo
    ```
*   **Boost** (for serial communication):
    ```bash
    pacman -S mingw-w64-x86_64-boost
    ```
*   **Compiler**: GCC/G++ (MinGW64).

### 2. Compile
Use the provided `Makefile` (defaults to building all services):
```bash
# Build everything
make

# Build individual services
make us_acq       # Producer
make preprocess   # Signal Processing
make output       # CSV Writer
```

### 3. Run
Open separate terminals for each service:

1.  **Kafka**: Ensure Kafka is running on `localhost:9092`.
2.  **Preprocess**:
    ```bash
    ./preprocess.exe
    ```
3.  **Output**:
    ```bash
    ./output.exe
    ```
4.  **Producer**:
    ```bash
    ./us_acq.exe
    ```

---

## � Protocol & Traceability
The system maintains strict record identity:
*   **Identity**: Generally, `Output Frame N` corresponds to `Input Frame N`. In the sliding window blur, `Output Frame N` is the smoothed version of `Input Frame N` (the center of the window).
*   **Headers**: 
    *   Input: `USFrameHeader` (Capture TS, Sequence ID).
    *   Output: `USProcessedFrameHeader` (Inherits Capture TS/Seq, adds Processing TS, Window Size, Sigma parameters).
