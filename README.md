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

4.  **Kafka**: Message broker handling the stream.

---

## 🐳 Docker Quick Start (Recommended)

You can run the processing and output backend in Docker.

1.  **Start Services**:
    ```bash
    docker-compose up --build
    ```
    *   Starts **Kafka**, **Preprocess**, and **Output** containers.
    *   **Data** is saved to the `./data` folder on your host machine.

2.  **Run Producer (Host Machine)**:
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
