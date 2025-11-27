#ifndef USBUILDER_H
#define USBUILDER_H

#include <string>
#include <vector>
#include <memory>
#include <boost/asio.hpp> 

class USBuilder {
public: 
    USBuilder(const std::string& portName);
    ~USBuilder();

    bool connect();
    void disconnect();

    bool requestFirmware(std::string& versionOut);
    bool requestAscan8bit(int numPoints, std::vector<unsigned char>& outData);
    bool requestAscan8bitBurst(int numPoints, int numFrames, 
                               std::vector<std::vector<unsigned char>>& outData);

    bool programSPIFunc2();
    bool programSPIFunc4(int numPoints);

    
private:
    std::string m_portName;
    std::unique_ptr<boost::asio::io_context> m_io;
    std::unique_ptr<boost::asio::serial_port> m_port;

    bool writeAll(const unsigned char* buf, size_t len);
    bool readExact(unsigned char* buf, size_t len, int timeoutMs = 2000);
};

#endif
