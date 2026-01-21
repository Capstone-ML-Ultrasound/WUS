#pragma once

#include <cstdint>
#include <vector>
#include <cstring>
#include <chrono>

// Magic bytes to identify our protocol "US"
const char US_MAGIC[2] = {'U', 'S'};
const uint8_t US_PROTOCOL_VERSION = 1;

#pragma pack(push, 1)
struct USFrameHeader {
    char magic[2];              // "US"
    uint8_t version;            // 1
    uint8_t message_type;       // 1=A-Scan, 2=Processed A-Scan
    int64_t timestamp_ns;       // Unix timestamp in nanoseconds (Capture Time)
    uint64_t sequence_number;   // Monotonic counter
    uint32_t device_id;         // Identifier for the source device
    uint32_t num_samples;       // e.g. 512
    uint8_t sample_format;      // 8 = 8-bit, 16 = 16-bit
    uint8_t reserved[3];        // Padding for alignment
    uint32_t payload_length;    // Size of the following payload in bytes
};

struct USProcessedFrameHeader {
    USFrameHeader base;         // Inherit base fields (Identity)
    int64_t processing_ts_ns;   // When processing finished
    uint16_t window_size;       // Temporal aperture size
    float sigma_depth;          // Smoothing param 1
    float sigma_time;           // Smoothing param 2
    uint8_t reserved_proc[6];   // Padding to reach 24 bytes extension
};
#pragma pack(pop)

// Ensure struct sizes
static_assert(sizeof(USFrameHeader) == 36, "USFrameHeader size mismatch");
static_assert(sizeof(USProcessedFrameHeader) == 60, "USProcessedFrameHeader size mismatch");


namespace USProtocol {

    inline std::vector<uint8_t> encodeEnvelope(
        uint64_t seq,
        uint32_t deviceId,
        const std::vector<unsigned char>& samples
    ) {
        USFrameHeader header;
        std::memcpy(header.magic, US_MAGIC, 2);
        header.version = US_PROTOCOL_VERSION;
        header.message_type = 1; // A-Scan
        
        auto now = std::chrono::system_clock::now();
        header.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
            
        header.sequence_number = seq;
        header.device_id = deviceId;
        header.num_samples = static_cast<uint32_t>(samples.size());
        header.sample_format = 8; // 8-bit
        std::memset(header.reserved, 0, 3);
        header.payload_length = static_cast<uint32_t>(samples.size());

        // Allocate target buffer
        std::vector<uint8_t> buffer;
        buffer.reserve(sizeof(USFrameHeader) + samples.size());

        // Copy Header
        const uint8_t* headerBytes = reinterpret_cast<const uint8_t*>(&header);
        buffer.insert(buffer.end(), headerBytes, headerBytes + sizeof(USFrameHeader));

        // Copy Payload
        buffer.insert(buffer.end(), samples.begin(), samples.end());

        return buffer;
    }

    struct DecodedFrame {
        const USFrameHeader* header;
        const uint8_t* payload;
        size_t payloadSize;
    };

    inline bool decodeEnvelope(const void* data, size_t size, DecodedFrame& out) {
        if (size < sizeof(USFrameHeader)) return false;

        const USFrameHeader* h = reinterpret_cast<const USFrameHeader*>(data);

        // Validate Magic
        if (h->magic[0] != 'U' || h->magic[1] != 'S') return false;
        
        // Validate Version (Simple check for now)
        if (h->version != US_PROTOCOL_VERSION) return false;

        // Validate Size integrity
        if (size < sizeof(USFrameHeader) + h->payload_length) return false;

        out.header = h;
        out.payload = reinterpret_cast<const uint8_t*>(data) + sizeof(USFrameHeader);
        out.payloadSize = h->payload_length;

        return true;
    }
}
