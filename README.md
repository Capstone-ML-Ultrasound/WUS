# Wireless Ultrasound System (WUS)

This project streams ultrasound A-scan frames from a custom hardware device to Kafka, where a Consumer services reads and persists the data to CSV.

## 🐳 Docker Quick Start (Recommended for Consumer)
You can run the entire backend (Kafka + Consumer) in Docker. This avoids installing `librdkafka` and `Boost` manually.

1.  **Start Kafka & Consumer**:
    ```bash
    docker-compose up --build
    ```
    *   **Kafka** listens on `localhost:9092` (Host) and `kafka:29092` (Internal).
    *   **Consumer** starts automatically inside Docker and waits for data.
    *   **Data** is saved to the `./data` folder on your host machine.

2.  **Run Producer (Host Machine)**:
    *   *Note: The producer must run on the host to access the USB hardware (COM port).*
    *   **Compile**:
        ```bash
        make
        # OR 
        g++ src/main.cpp src/USBuilder.cpp src/Utils.cpp -o us_acq.exe -Iinclude -lrdkafka -lws2_32
        ```
    *   **Run**:
        ```bash
        ./us_acq.exe
        ```

---

## 🛠 Manual Setup (Windows/MinGW)

If you prefer to run everything locally without Docker:

1.  **Prerequisites**:
    *   **librdkafka** (Kafka C library):
        ```bash
        # MSYS2/MinGW
        pacman -S mingw-w64-x86_64-librdkafka
        
        # macOS (Homebrew)
        brew install librdkafka
        ```
    *   **Boost** (for serial communication):
        ```bash
        # MSYS2/MinGW
        pacman -S mingw-w64-x86_64-boost
        
        # macOS (Homebrew)
        brew install boost
        ```
    *   `ws2_32` (Winsock) for Windows - included with MinGW.

2.  **Start Background Services**:
    *   Ensure a Kafka broker is running on `localhost:9092`.

3.  **Compile & Run**:
    ```bash
    # Build
    make
    
    # Run Consumer (Terminal 1)
    ./consumer.exe
    
    # Run Producer (Terminal 2)
    ./us_acq.exe
    ```

## 📚 Architecture
*   **Producer**: C++ app. connects to hardware via USB/Serial. Encodes frames with `USFrameProtocol` and pushes to Kafka (`ultrasound_raw_data`).
*   **Kafka**: Message broker.
*   **Consumer**: C++ app. Decodes frames, logs metadata, and batches data into CSV files in `data/`.
