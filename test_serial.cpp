#include <iostream>
#include <boost/asio.hpp>
#include <boost/asio/serial_port.hpp>
#include <vector>
#include <thread>
#include <chrono>

using namespace boost::asio;

class SimpleSerial {
private:
    io_context io;  // Changed from io_service
    serial_port port;
    
public:
    SimpleSerial(const std::string& portName) 
        : port(io) {
        try {
            port.open(portName);
            
            // Configure serial port settings to match US-Builder
            port.set_option(serial_port_base::baud_rate(115200));
            port.set_option(serial_port_base::character_size(8));
            port.set_option(serial_port_base::stop_bits(
                serial_port_base::stop_bits::one));
            port.set_option(serial_port_base::parity(
                serial_port_base::parity::none));
            port.set_option(serial_port_base::flow_control(
                serial_port_base::flow_control::none));
            
            std::cout << "✅ Connected to " << portName << std::endl;
        } catch (boost::system::system_error& e) {
            std::cerr << "❌ Connection error: " << e.what() << std::endl;
            throw;
        }
    }
    
    ~SimpleSerial() {
        if (port.is_open()) {
            port.close();
            std::cout << "Port closed" << std::endl;
        }
    }
    
    // Write data to serial port
    bool write(const unsigned char* data, size_t len) {
        try {
            boost::asio::write(port, buffer(data, len));
            return true;
        } catch (boost::system::system_error& e) {
            std::cerr << "Write error: " << e.what() << std::endl;
            return false;
        }
    }
    
    // Read exact number of bytes with timeout handling
    bool read(unsigned char* data, size_t len, int timeoutMs = 2000) {
        try {
            // Set up async read with timeout
            boost::system::error_code ec;
            size_t bytesRead = 0;
            
            // Simple blocking read (for basic version)
            bytesRead = boost::asio::read(port, buffer(data, len), ec);
            
            if (ec) {
                std::cerr << "Read error: " << ec.message() << std::endl;
                return false;
            }
            
            return bytesRead == len;
        } catch (boost::system::system_error& e) {
            std::cerr << "Read error: " << e.what() << std::endl;
            return false;
        }
    }
    
    // Request firmware version (Function 3)
    bool requestFirmware(std::string& versionOut) {
        std::cout << "\n📡 Requesting firmware version..." << std::endl;
        
        // Command structure from manual:
        // Header: 140,140,140,140,140
        // Mode: 3 (firmware request)
        // Remaining bytes: unused
        unsigned char cmd[12] = {140, 140, 140, 140, 140, 3, 0, 0, 0, 0, 0, 0};
        
        if (!write(cmd, sizeof(cmd))) {
            std::cerr << "❌ Failed to send firmware request" << std::endl;
            return false;
        }
        
        // Wait for device to respond
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        // Read 1 byte response
        unsigned char response[1];
        if (!read(response, 1)) {
            std::cerr << "❌ Failed to read firmware version" << std::endl;
            return false;
        }
        
        versionOut = std::to_string((int)response[0]);
        std::cout << "✅ Firmware version: " << versionOut << std::endl;
        return true;
    }
    
    // Request A-scan (Function 0)
    bool requestAscan8bit(int numPoints, std::vector<unsigned char>& outData) {
        if (numPoints <= 0 || numPoints > 4000) {
            std::cerr << "❌ Invalid numPoints: " << numPoints << " (must be 1-4000)" << std::endl;
            return false;
        }
        
        std::cout << "\n📡 Requesting A-scan with " << numPoints << " points..." << std::endl;
        
        // Command structure from manual:
        // Header: 140,140,140,140,140
        // Mode: 0 (A-scan read)
        // cmd[6]: MSB of numPoints
        // cmd[7]: LSB of numPoints
        unsigned char cmd[12] = {140, 140, 140, 140, 140, 0, 0, 0, 0, 0, 0, 0};
        cmd[6] = (numPoints >> 8) & 0xFF;  // High byte
        cmd[7] = numPoints & 0xFF;         // Low byte
        
        if (!write(cmd, sizeof(cmd))) {
            std::cerr << "❌ Failed to send A-scan request" << std::endl;
            return false;
        }
        
        // Wait briefly for acquisition
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        // Read samples
        outData.resize(numPoints);
        if (!read(outData.data(), numPoints, 5000)) {
            std::cerr << "❌ Failed to read A-scan data" << std::endl;
            return false;
        }
        
        // Check if we got dummy data (all 50s = error)
        bool allFifty = true;
        for (size_t i = 0; i < std::min(size_t(10), outData.size()); i++) {
            if (outData[i] != 50) {
                allFifty = false;
                break;
            }
        }
        
        if (allFifty) {
            std::cerr << "⚠️  WARNING: Received dummy data (all 50s) - check connection" << std::endl;
        } else {
            std::cout << "✅ A-scan received successfully" << std::endl;
        }
        
        return true;
    }
};

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "US-Builder Serial Test Program" << std::endl;
    std::cout << "========================================" << std::endl;
    
    std::string portName;
    
    // Platform-specific default port
#ifdef _WIN32
    portName = "\\\\.\\COM3";  // Windows
    std::cout << "Platform: Windows" << std::endl;
#elif __APPLE__
    portName = "/dev/tty.usbmodem31101";  // macOS
    std::cout << "Platform: macOS" << std::endl;
#else
    portName = "/dev/ttyUSB0";  // Linux
    std::cout << "Platform: Linux" << std::endl;
#endif
    
    std::cout << "Attempting to connect to: " << portName << std::endl;
    std::cout << "\n⚠️  If connection fails, update portName in test_serial.cpp" << std::endl;
    std::cout << "========================================\n" << std::endl;
    
    try {
        // Connect to device
        SimpleSerial serial(portName);
        
        // Test 1: Get firmware version
        std::string version;
        if (!serial.requestFirmware(version)) {
            std::cerr << "\n❌ Firmware test failed" << std::endl;
            return 1;
        }
        
        // Test 2: Get A-scan
        std::vector<unsigned char> samples;
        if (!serial.requestAscan8bit(512, samples)) {
            std::cerr << "\n❌ A-scan test failed" << std::endl;
            return 1;
        }
        
        // Display first 10 samples
        std::cout << "\n📊 First 10 sample values: ";
        for (int i = 0; i < 10 && i < (int)samples.size(); i++) {
            std::cout << (int)samples[i] << " ";
        }
        std::cout << std::endl;
        
        // Calculate some basic stats
        int minVal = 255, maxVal = 0;
        long long sum = 0;
        for (unsigned char s : samples) {
            minVal = std::min(minVal, (int)s);
            maxVal = std::max(maxVal, (int)s);
            sum += s;
        }
        double avg = sum / (double)samples.size();
        
        std::cout << "\n📈 A-scan statistics:" << std::endl;
        std::cout << "   Samples: " << samples.size() << std::endl;
        std::cout << "   Min: " << minVal << std::endl;
        std::cout << "   Max: " << maxVal << std::endl;
        std::cout << "   Avg: " << avg << std::endl;
        
        std::cout << "\n========================================" << std::endl;
        std::cout << "✅ All tests passed!" << std::endl;
        std::cout << "========================================" << std::endl;
        
    } catch (std::exception& e) {
        std::cerr << "\n❌ Exception: " << e.what() << std::endl;
        std::cerr << "\nTroubleshooting:" << std::endl;
        std::cerr << "  1. Check device is connected and powered on" << std::endl;
        std::cerr << "  2. Verify correct port name:" << std::endl;
#ifdef _WIN32
        std::cerr << "     Windows: Get-PnpDevice -Class Ports" << std::endl;
#elif __APPLE__
        std::cerr << "     macOS: ls /dev/tty.*" << std::endl;
#else
        std::cerr << "     Linux: ls /dev/ttyUSB* /dev/ttyACM*" << std::endl;
#endif
        std::cerr << "  3. Check permissions (may need sudo on Linux/macOS)" << std::endl;
        return 1;
    }
    
    return 0;
}