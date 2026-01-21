#include <iostream>
#include <fstream>
#include <vector>
#include <csignal>
#include <cstring>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <librdkafka/rdkafka.h>

#include "USFrameProtocol.h"

// =================================================================================================
// Configuration
// =================================================================================================

const char* TOPIC_IN = "ultrasound.clean";
const int BURST_SIZE = 500; // Frames per CSV file

volatile sig_atomic_t running = 1;

void signalHandler(int signum) {
    std::cout << "\n[Output] Shutdown signal received..." << std::endl;
    running = 0;
}

// =================================================================================================
// Kafka Helper
// =================================================================================================

rd_kafka_t* create_consumer(const std::string& brokers, const std::string& group_id) {
    char errstr[512];
    rd_kafka_conf_t *conf = rd_kafka_conf_new();

    if (rd_kafka_conf_set(conf, "bootstrap.servers", brokers.c_str(), errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        std::cerr << "[Config] " << errstr << std::endl;
        return nullptr;
    }
    if (rd_kafka_conf_set(conf, "group.id", group_id.c_str(), errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        std::cerr << "[Config] " << errstr << std::endl;
        return nullptr;
    }
    if (rd_kafka_conf_set(conf, "auto.offset.reset", "latest", errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        std::cerr << "[Config] " << errstr << std::endl;
        return nullptr;
    }

    rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
    if (!rk) {
        std::cerr << "[Error] Failed to create consumer: " << errstr << std::endl;
        rd_kafka_conf_destroy(conf);
        return nullptr;
    }
    return rk;
}

// =================================================================================================
// Writer
// =================================================================================================

void write_burst_csv(const std::vector<std::vector<uint8_t>>& buffer, int width, int height) {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    
    std::stringstream ss;
    ss << "data/us_burst_" << std::put_time(std::localtime(&in_time_t), "%Y%m%d_%H%M%S") << ".csv";
    std::string filename = ss.str();

    std::ofstream csv_file(filename);
    if (!csv_file.is_open()) {
        std::cerr << "[Error] Could not open " << filename << " for writing." << std::endl;
        return;
    }

    // Transpose write: Rows = Depth (height), Cols = Time (width/buffer size)
    // buffer[frame_idx][depth_idx]
    // We want to write:
    // depth_0_frame_0, depth_0_frame_1, ...
    // depth_1_frame_0, depth_1_frame_1, ...
    
    // Validate dimensions
    if (buffer.empty()) return;
    int frames = buffer.size();
    int samples = buffer[0].size(); 

    std::cout << "[Output] Writing burst to " << filename << " (" << samples << "x" << frames << ")..." << std::endl;

    for (int d = 0; d < samples; ++d) {
        for (int f = 0; f < frames; ++f) {
            // Safety check in case frames have different sizes (shouldn't happen with fixed protocol)
            if (d < buffer[f].size()) {
                csv_file << static_cast<int>(buffer[f][d]);
            } else {
                csv_file << "0";
            }
            if (f < frames - 1) csv_file << ",";
        }
        csv_file << "\n";
    }
    
    csv_file.close();
    std::cout << "[Output] Burst written." << std::endl;
}

// =================================================================================================
// Main Consumer Loop
// =================================================================================================

void consume_and_process(rd_kafka_t* consumer) {
    rd_kafka_topic_partition_list_t *subscription = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_list_add(subscription, TOPIC_IN, RD_KAFKA_PARTITION_UA);
    rd_kafka_subscribe(consumer, subscription);
    rd_kafka_topic_partition_list_destroy(subscription);

    std::cout << "[Output] Subscribed to " << TOPIC_IN << ". Buffering " << BURST_SIZE << " frames per burst." << std::endl;

    std::vector<std::vector<uint8_t>> frame_buffer;
    frame_buffer.reserve(BURST_SIZE);

    int total_frames = 0;

    while (running) {
        rd_kafka_message_t *rkmessage = rd_kafka_consumer_poll(consumer, 500);

        if (!rkmessage) continue;
        if (rkmessage->err) {
            if (rkmessage->err != RD_KAFKA_RESP_ERR__PARTITION_EOF) {
                std::cerr << "[Error] " << rd_kafka_message_errstr(rkmessage) << std::endl;
            }
            rd_kafka_message_destroy(rkmessage);
            continue;
        }

        // Decode Header
        if (rkmessage->len < sizeof(USFrameHeader)) {
            rd_kafka_message_destroy(rkmessage);
            continue;
        }

        const USFrameHeader* base_h = reinterpret_cast<const USFrameHeader*>(rkmessage->payload);
        
        // Basic Validation
        if (std::strncmp(base_h->magic, US_MAGIC, 2) != 0) {
             rd_kafka_message_destroy(rkmessage);
             continue;
        }

        // Extract Payload
        const uint8_t* payload_ptr = nullptr;
        size_t payload_size = 0;

        if (base_h->message_type == 2) { // Processed
            if (rkmessage->len < sizeof(USProcessedFrameHeader)) {
                rd_kafka_message_destroy(rkmessage);
                continue;
            }
            // const USProcessedFrameHeader* proc_h = reinterpret_cast<const USProcessedFrameHeader*>(rkmessage->payload);
            payload_ptr = reinterpret_cast<const uint8_t*>(rkmessage->payload) + sizeof(USProcessedFrameHeader);
            payload_size = base_h->payload_length;
        } else {
             // Fallback or ignore
             // If receiving raw, we might want to handle it too, but for now skip
             rd_kafka_message_destroy(rkmessage);
             continue;
        }

        // Copy frame to vector
        std::vector<uint8_t> frame(payload_ptr, payload_ptr + payload_size);
        frame_buffer.push_back(std::move(frame));

        // Check Buffer
        if (frame_buffer.size() >= BURST_SIZE) {
            write_burst_csv(frame_buffer, BURST_SIZE, frame_buffer[0].size());
            frame_buffer.clear();
            frame_buffer.reserve(BURST_SIZE);
        }

        if (++total_frames % 100 == 0) {
            std::cout << "[Output] Buffered " << frame_buffer.size() << "/" << BURST_SIZE << " frames (Total: " << total_frames << ")" << std::endl;
        }

        rd_kafka_message_destroy(rkmessage);
    }
}

int main() {
    signal(SIGINT, signalHandler);

    std::string brokers = "localhost:9092";
    if (const char* env = std::getenv("BOOTSTRAP_SERVERS")) brokers = env;

    rd_kafka_t* consumer = create_consumer(brokers, "output_csv_group");
    if (!consumer) return 1;

    consume_and_process(consumer);

    rd_kafka_consumer_close(consumer);
    rd_kafka_destroy(consumer);

    std::cout << "\n[Output] Done." << std::endl;
    return 0;
}
