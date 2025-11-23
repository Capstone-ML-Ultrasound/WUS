#include "USBuilder.h"
#include "Utils.h"

#include <iostream>
#include <chrono>
#include <vector>


void test_firmware(USBuilder &dev){
    // Step 1: Get firmware version
    std::string version;
    if (dev.requestFirmware(version)) {
        std::cout << "Firmware version: " << version << std::endl;
    } else {
        std::cerr << "Firmware request failed" << std::endl;
    }
}

void acquire_single_Ascan(USBuilder &dev, Utils utils){
    // Step 2: Acquire single A-scan (optional test)
    std::cout << "\n--- Single A-scan ---" << std::endl;
    std::vector<unsigned char> samples;
    if (dev.requestAscan8bit(512, samples)) {
        utils.writeCSV(samples);
        std::cout << "Single A-scan saved" << std::endl;
    } else {
        std::cerr << "Single A-scan failed" << std::endl;
    }
    
}

void acquire_burst_Ascan(USBuilder &dev, Utils &utils){
    // Step 3: Acquire burst A-scan
    std::cout << "\n--- Acquiring burst data ---" << std::endl;
    std::vector<std::vector<unsigned char>> burstData;

    auto start = std::chrono::high_resolution_clock::now();

    if (dev.requestAscan8bitBurst(4000, 1000, burstData)) {
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> duration_ms = end - start;

        std::cout << "Burst acquisition complete" << std::endl;
        std::cout << "   Frames: " << burstData.size() << std::endl;
        std::cout << "   Samples per frame: " << burstData[0].size() << std::endl;
        std::cout << "   Duration: " << duration_ms.count() << " ms" << std::endl;
        std::cout << "   Frame rate: " << (burstData.size() * 1000.0 / duration_ms.count()) << " fps" << std::endl;

        // Save to CSV
        utils.writeBurstCSV(burstData);

    } else {
        std::cerr << "Burst acquisition failed" << std::endl;
        dev.disconnect();
    
    }
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "US-Builder Data Acquisition" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // Platform-specific port selection
    std::string portName;

#ifdef _WIN32
    portName = "\\\\.\\COM4";  // Windows
    std::cout << "Platform: Windows" << std::endl;
#elif __APPLE__
    portName = "/dev/tty.usbmodem1101";  // macOS (update as needed)
    std::cout << "Platform: macOS" << std::endl;
#else
    portName = "/dev/ttyUSB0";  // Linux
    std::cout << "Platform: Linux" << std::endl;
#endif

    std::cout << "Using port: " << portName << "\n" << std::endl;

    // Instantiations 
    USBuilder dev(portName);
    Utils utils;

    // Connect to device
    if (!dev.connect()) {
        std::cerr << "Failed to connect to device" << std::endl;
        return 1;
    }

    
    // Step 3: Acquire burst A-scan
    acquire_burst_Ascan(dev, utils);

    
    // Disconnect before exiting
    dev.disconnect();

    std::cout << "\n========================================" << std::endl;
    std::cout << " Program completed successfully" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}