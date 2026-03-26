#include "USBuilder.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <thread>
#include <boost/asio.hpp>
#include <boost/asio/serial_port.hpp>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace boost::asio;

// Constructor
USBuilder::USBuilder(const std::string& portName)
    : m_portName(portName),
      m_io(std::make_unique<io_context>()),
      m_port(std::make_unique<serial_port>(*m_io)) {
}

// Destructor
USBuilder::~USBuilder() {
    disconnect();
}

/**
 * Opens the serial port and configures it for communication with US-Builder.
 */
bool USBuilder::connect() {
    std::cout << "Connecting to US-Builder on " << m_portName << "..." << std::endl;

    try {
        m_port->open(m_portName);

        // Configure serial port settings to match US-Builder firmware
        m_port->set_option(serial_port_base::baud_rate(115200));
        m_port->set_option(serial_port_base::character_size(8));
        m_port->set_option(serial_port_base::stop_bits(serial_port_base::stop_bits::one));
        m_port->set_option(serial_port_base::parity(serial_port_base::parity::none));
        m_port->set_option(serial_port_base::flow_control(serial_port_base::flow_control::none));

        std::cout << "Connected to US-Builder successfully" << std::endl;
        return true;

    } catch (boost::system::system_error& e) {
        std::cerr << "Failed to open port " << m_portName << ": " << e.what() << std::endl;
        return false;
    }
}

/**
 * Disconnect from serial port for cleanup
 */
void USBuilder::disconnect() {
    if (m_port && m_port->is_open()) {
        m_port->close();
        std::cout << "Disconnected from US-Builder successfully" << std::endl;
    }
}

/**
 * Write an entire buffer to the device.
 */
bool USBuilder::writeAll(const unsigned char* buf, size_t len) {
    try {
        boost::asio::write(*m_port, buffer(buf, len));
        return true;
    } catch (boost::system::system_error& e) {
        std::cerr << "Write error: " << e.what() << std::endl;
        return false;
    }
}

/**
 * Read exactly 'len' bytes from the device.
 * Loops until all data arrives or timeout hits.
 */
bool USBuilder::readExact(unsigned char* buf, size_t len) {
    try {
#ifdef _WIN32
        // Windows fallback: keep blocking read behavior when non-blocking fd control
        // is unavailable through this build of Boost.Asio.
        boost::system::error_code ec;
        size_t bytesRead = boost::asio::read(*m_port, buffer(buf, len), ec);
        if (ec) {
            std::cerr << "Read error: " << ec.message() << std::endl;
            return false;
        }
        return bytesRead == len;
#else
        const int fd = m_port->native_handle();
        const int originalFlags = fcntl(fd, F_GETFL, 0);
        if (originalFlags == -1) {
            std::cerr << "Failed to get serial port flags" << std::endl;
            return false;
        }
        if (fcntl(fd, F_SETFL, originalFlags | O_NONBLOCK) == -1) {
            std::cerr << "Failed to set serial port to non-blocking mode" << std::endl;
            return false;
        }

        boost::system::error_code ec;

        size_t totalRead = 0;
        constexpr int kReadTimeoutMs = 2000;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kReadTimeoutMs);

        while (totalRead < len) {
            const size_t readNow = m_port->read_some(buffer(buf + totalRead, len - totalRead), ec);
            if (!ec) {
                totalRead += readNow;
                continue;
            }

            if (ec == boost::asio::error::would_block ||
                ec == boost::asio::error::try_again) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    std::cerr << "Read timeout after " << kReadTimeoutMs
                              << " ms (received " << totalRead << "/" << len << " bytes)" << std::endl;
                    fcntl(fd, F_SETFL, originalFlags);
                    return false;
                }
                ec.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            std::cerr << "Read error: " << ec.message() << std::endl;
            fcntl(fd, F_SETFL, originalFlags);
            return false;
        }

        if (fcntl(fd, F_SETFL, originalFlags) == -1) {
            std::cerr << "Failed to restore serial port flags" << std::endl;
            return false;
        }

        return true;
#endif

    } catch (boost::system::system_error& e) {
        std::cerr << "Read error: " << e.what() << std::endl;
        return false;
    }
}

/**
 * Send the firmware request command and read back response.
 */
bool USBuilder::requestFirmware(std::string& versionOut) {
    /**
     * Command structure based on manual
     * cmd[0..4] == header
     * cmd[5] == mode - in this case 3, which requests firmware version (handshake)
     * cmd[6..11] == 0 - not needed for firmware req
     */
    unsigned char cmd[12] = {140, 140, 140, 140, 140, 3, 0, 0, 0, 0, 0, 0};

    if (!writeAll(cmd, sizeof(cmd))) {
        std::cerr << "Failed in writing cmd to buffer" << std::endl;
        return false;
    }

    // Give device time to respond
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    unsigned char buf[1];
    if (!readExact(buf, 1)) {
        std::cerr << "Failed in reading from buffer" << std::endl;
        return false;
    }

    versionOut = std::to_string((int)buf[0]); // convert numeric FW version to string
    return true;
}

/**
 * Request an A-scan (ultrasound intensity vs. depth).
 * @param numPoints Number of samples requested (e.g. 512)
 * @param outData Will be filled with samples from device
 *
 * Note -- if the request is bad -- it will give all 50's back to you (dummy data)
 */
bool USBuilder::requestAscan8bit(int numPoints, std::vector<unsigned char>& outData) {
    if (numPoints <= 0 || numPoints > 4096) {
        std::cerr << "Invalid numPoints: " << numPoints << " (must be 1-4096)" << std::endl;
        return false;
    }

    /**
     * Command structure based on manual
     * cmd[0..4] == header
     * cmd[5] == mode -- in this case 0, which requests A-scan read (must specify number of points cmd[6/7])
     * cmd[6] == most significant bit (high byte)
     * cmd[7] == least significant bit (low byte)
     * Big-endian -> 16 bit split 1 byte MSBs // 1 byte LSbs
     */
    unsigned char cmd[12] = {140, 140, 140, 140, 140, 0, 0, 0, 0, 0, 0, 0};

    cmd[6] = (numPoints >> 8) & 0xFF; // high byte
    cmd[7] = numPoints & 0xFF;        // low byte

    if (!writeAll(cmd, sizeof(cmd))) {
        return false;
    }

    // Read samples back
    outData.resize(numPoints);
    return readExact(outData.data(), numPoints); // .data() returns ptr to underlying vector
}

/**
 * Request burst of numFrames A-scan (ultrasound intensity vs. depth).
 * @param numPoints Number of samples requested (e.g. 512)
 * @param numFrames Number of scans requested
 * @param outData Will be filled with samples from device
 *
 * Note -- if the request is bad -- it will give all 50's back to you (dummy data)
 */
bool USBuilder::requestAscan8bitBurst(int numPoints, int numFrames,
                                      std::vector<std::vector<unsigned char>>& outBurstData) {

    if (numPoints <= 0 || numPoints > 4096) {
        std::cerr << "Invalid numPoints: " << numPoints << " (must be 1-4096)" << std::endl;
        return false;
    }

    unsigned char cmd[12] = {140, 140, 140, 140, 140, 0, 0, 0, 0, 0, 0, 0};

    // Num points max== 4096 (bitwise ~12bits, needs to set the correct bits)
    cmd[6] = (numPoints >> 8) & 0xFF;
    cmd[7] = numPoints & 0xFF;

    outBurstData.resize(numFrames);              // preallocate top most vec
    for (auto& frame : outBurstData) frame.resize(numPoints); // preallocate inner vecs

    //Loop for the # of Frames we want and send Scan Request 
    for (int i = 0; i < numFrames; ++i) {
        if (!writeAll(cmd, sizeof(cmd))) {
            std::cerr << "Failed to write command for frame " << i << std::endl;
            return false;
        }

        if (!readExact(outBurstData[i].data(), numPoints)) {
            std::cerr << "Failed to read frame " << i << std::endl;
            return false;
        }
    }

    return true;
}

bool USBuilder::programSPIFunc2(){

    unsigned char cmd[12] = {140, 140, 140, 140, 140, 0, 0, 0, 0, 0, 0, 0};

    cmd[5] = 1;     // Write function 
    cmd[6] = 1;     // # of params to be sent
    cmd[7] = 2;     // Fct.2 = Sampling Request

    if (!writeAll(cmd, sizeof(cmd))) {
        std::cerr << "Failed to send trigger command" << std::endl;
        return false;
    }
    
    std::cout << "Acquisition triggered (Function 2)" << std::endl;
    return true;
}

bool USBuilder::programSPIFunc4(int numpoints){

    unsigned char cmd[12] = {140, 140, 140, 140, 140, 0, 0, 0, 0, 0, 0, 0};

    cmd[5] = 1;                       // Write mode
    cmd[6] = 3;                       // 3 parameters (func, lsb, msb)
    cmd[7] = 4;                       // Function 4 (Auto Sampling Request)
    cmd[8] = (numpoints >> 8) & 0xFF;  // high byte
    cmd[9] = numpoints & 0xFF;         // low byte

    // cmd[5] = 1;                        // Write mode
    // cmd[6] = 3;                        // 3 parameters
    // cmd[7] = numpoints & 0xFF;         // LSB first
    // cmd[8] = (numpoints >> 8) & 0xFF;  // MSB second
    // cmd[9] = 4;                        // Function number last

    if (!writeAll(cmd, sizeof(cmd))) {
        std::cerr << "Failed to send Function 4 auto-sampling command" << std::endl;
        return false;
    }

    std::cout << "Auto-sampling enabled: will trigger after " 
              << numpoints << " samples read (Function 4)" << std::endl;

    return true;

}



bool USBuilder::func4_setAutoSample(int numpoints) {

    unsigned char cmd[12] = {140, 140, 140, 140, 140, 0, 0, 0, 0, 0, 0, 0};
    cmd[5] = 1;                                                     // Write mode
    cmd[6] = 3;                                                     // 3 parameters (func, lsb, msb)
    cmd[7] = static_cast<unsigned char>((numpoints >> 8) & 0xFF);   // MSB
    cmd[8] = static_cast<unsigned char>(numpoints & 0xFF);          // LSB
    cmd[9] = 4;                                                     // Function 4 (Auto Sampling Request)

    if (!writeAll(cmd, sizeof(cmd))) {
        std::cerr << "Failed to send Function 4 auto-sampling command" << std::endl;
        return false;
    }

    std::cout << "Auto-sampling enabled: will trigger after "
              << numpoints << " samples read (Function 4)" << std::endl;

    return true;
}

bool USBuilder::func24_setSamplingFreq(int freq) {
    int f;
    if (freq == 160) {
        f = 0;
    } else if (freq == 80) {
        f = 1;
    } else if (freq == 40) {
        f = 2;
    } else if (freq == 20) {
        f = 3;
    } else {
        std::cerr << "NOT VALID FREQ" << std::endl;
        return false;
    }

    unsigned char cmd[12] = {140, 140, 140, 140, 140, 0, 0, 0, 0, 0, 0, 0};

    cmd[5] = 1;          // Write mode
    cmd[6] = 2;          // 3 parameters (func, lsb, msb)
    cmd[7] = f;          // LSB
    cmd[8] = 24;         // func 24

    if (!writeAll(cmd, sizeof(cmd))) {
        std::cerr << "Failed to send Function 24 (SamplingFreq) command" << std::endl;
        return false;
    }
    return true;
}

bool USBuilder::func14_setFilter(double mhz) {
    int filter;

    if (mhz == 1.25) {
        filter = 0;
    } else if (mhz == 2.5) {
        filter = 1;
    } else if (mhz == 5) {
        filter = 2;
    } else if (mhz == 10) {
        filter = 3;
    } else if (mhz == -1){
        filter = 4;
    }else{
        std::cerr << "NOT VALID Filter" << std::endl;
        return false;
    }

    unsigned char cmd[12] = {140, 140, 140, 140, 140, 0, 0, 0, 0, 0, 0, 0};

    cmd[5] = 1;          // Write mode
    cmd[6] = 2;          // # params
    cmd[7] = filter;
    cmd[8] = 14;        // Func 14

    if (!writeAll(cmd, sizeof(cmd))) {
        std::cerr << "Failed to send Function 14 (Filter) command" << std::endl;
        return false;
    }
    return true;
}

bool USBuilder::func3_setCompression(int factor) {

    unsigned char cmd[12] = {140, 140, 140, 140, 140, 0, 0, 0, 0, 0, 0, 0};

    cmd[5] = 1;          // Write mode
    cmd[6] = 2;          // # params
    cmd[7] = factor;
    cmd[8] = 3;          // Func 3

    if (!writeAll(cmd, sizeof(cmd))) {
        std::cerr << "Failed to send Function 3 (Compression) command" << std::endl;
        return false;
    }
    return true;
}
