#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <librdkafka/rdkafka.h>
#include <sstream>
#include <string>
#include <vector>

#include "USFrameProtocol.h"

const char* TOPIC_IN = "ultrasound.clean";
const int BURST_SIZE = 1000;
const int POLL_TIMEOUT_MS = 500;

volatile sig_atomic_t running = 1;

void signalHandler(int /*signum*/) {
    std::cout << "\n[ProcessSink] Shutdown signal received..." << std::endl;
    running = 0;
}

int get_nonnegative_env_int(const char* name, int fallback) {
    const char* raw = std::getenv(name);
    if (!raw || std::string(raw).empty()) return fallback;
    try {
        long long parsed = std::stoll(raw);
        if (parsed < 0 || parsed > std::numeric_limits<int>::max()) return fallback;
        return static_cast<int>(parsed);
    } catch (...) {
        return fallback;
    }
}

bool is_truthy_env(const char* value) {
    if (!value) return false;
    std::string v(value);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

bool upload_file_to_gcs(const std::string& local_file) {
    const char* bucket = std::getenv("PROCESSED_GCS_BUCKET");
    if (!bucket || std::string(bucket).empty()) return false;

    std::string prefix = "ultrasound/processed";
    if (const char* env_prefix = std::getenv("PROCESSED_GCS_PREFIX")) {
        if (std::string(env_prefix).size() > 0) prefix = env_prefix;
    }
    if (!prefix.empty() && prefix.back() == '/') prefix.pop_back();

    const std::string filename = std::filesystem::path(local_file).filename().string();
    const std::string gs_uri = "gs://" + std::string(bucket) + "/" + prefix + "/" + filename;

    std::string cmd = "gsutil cp \"" + local_file + "\" \"" + gs_uri + "\"";
    const char* project = std::getenv("GCP_PROJECT_ID");
    if (project && std::string(project).size() > 0) {
        cmd = "gsutil -u \"" + std::string(project) + "\" cp \"" + local_file + "\" \"" + gs_uri + "\"";
    }

    std::cout << "[ProcessSink] Uploading burst to " << gs_uri << "..." << std::endl;
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::cerr << "[ProcessSink] GCS upload failed (exit code " << rc << "). Keeping local file." << std::endl;
        return false;
    }

    std::cout << "[ProcessSink] GCS upload complete." << std::endl;
    return true;
}

rd_kafka_t* create_consumer(const std::string& brokers, const std::string& group_id) {
    char errstr[512];
    rd_kafka_conf_t* conf = rd_kafka_conf_new();

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

    rd_kafka_t* rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
    if (!rk) {
        std::cerr << "[Error] Failed to create consumer: " << errstr << std::endl;
        rd_kafka_conf_destroy(conf);
        return nullptr;
    }
    return rk;
}

void write_burst_csv(const std::vector<std::vector<uint8_t>>& buffer) {
    std::string output_dir = "data/processed_verify";
    if (!std::filesystem::exists(output_dir)) {
        std::filesystem::create_directory(output_dir);
    }

    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << output_dir << "/us_processed_burst_" << std::put_time(std::localtime(&in_time_t), "%Y%m%d_%H%M%S") << ".csv";
    std::string filename = ss.str();

    std::ofstream csv_file(filename);
    if (!csv_file.is_open()) {
        std::cerr << "[Error] Could not open " << filename << " for writing." << std::endl;
        return;
    }

    if (buffer.empty()) return;
    int frames = static_cast<int>(buffer.size());
    int samples = static_cast<int>(buffer[0].size());

    std::cout << "[ProcessSink] Writing burst to " << filename << " (" << samples << "x" << frames << ")..." << std::endl;
    for (int d = 0; d < samples; ++d) {
        for (int f = 0; f < frames; ++f) {
            if (d < static_cast<int>(buffer[f].size())) {
                csv_file << static_cast<int>(buffer[f][d]);
            } else {
                csv_file << "0";
            }
            if (f < frames - 1) csv_file << ",";
        }
        csv_file << "\n";
    }
    csv_file.close();
    std::cout << "[ProcessSink] Burst written." << std::endl;

    const bool uploaded = upload_file_to_gcs(filename);
    const bool keep_local = is_truthy_env(std::getenv("PROCESSED_GCS_KEEP_LOCAL"));
    if (uploaded && !keep_local) {
        std::error_code ec;
        std::filesystem::remove(filename, ec);
        if (ec) {
            std::cerr << "[ProcessSink] Uploaded but failed to remove local file: " << ec.message() << std::endl;
        } else {
            std::cout << "[ProcessSink] Removed local file after successful upload." << std::endl;
        }
    }
}

void consume_and_process(rd_kafka_t* consumer) {
    rd_kafka_topic_partition_list_t* subscription = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_list_add(subscription, TOPIC_IN, RD_KAFKA_PARTITION_UA);
    rd_kafka_subscribe(consumer, subscription);
    rd_kafka_topic_partition_list_destroy(subscription);

    std::cout << "[ProcessSink] Subscribed to " << TOPIC_IN << ". Buffering " << BURST_SIZE << " frames per burst." << std::endl;

    std::vector<std::vector<uint8_t>> frame_buffer;
    frame_buffer.reserve(BURST_SIZE);
    int total_frames = 0;
    const int idle_flush_ms = get_nonnegative_env_int("PROCESS_SINK_IDLE_FLUSH_MS", 0);
    int idle_elapsed_ms = 0;

    while (running) {
        rd_kafka_message_t* rkmessage = rd_kafka_consumer_poll(consumer, POLL_TIMEOUT_MS);

        if (!rkmessage) {
            idle_elapsed_ms += POLL_TIMEOUT_MS;
            if (idle_flush_ms > 0 && !frame_buffer.empty() && idle_elapsed_ms >= idle_flush_ms) {
                std::cout << "[ProcessSink] Idle timeout reached. Flushing partial burst of "
                          << frame_buffer.size() << " frame(s)." << std::endl;
                write_burst_csv(frame_buffer);
                frame_buffer.clear();
                frame_buffer.reserve(BURST_SIZE);
                idle_elapsed_ms = 0;
            }
            continue;
        }

        idle_elapsed_ms = 0;
        if (rkmessage->err) {
            if (rkmessage->err != RD_KAFKA_RESP_ERR__PARTITION_EOF) {
                std::cerr << "[Error] " << rd_kafka_message_errstr(rkmessage) << std::endl;
            }
            rd_kafka_message_destroy(rkmessage);
            continue;
        }

        if (rkmessage->len < sizeof(USProcessedFrameHeader)) {
            rd_kafka_message_destroy(rkmessage);
            continue;
        }

        const USFrameHeader* base_h = reinterpret_cast<const USFrameHeader*>(rkmessage->payload);
        if (std::strncmp(base_h->magic, US_MAGIC, 2) != 0 || base_h->message_type != 2) {
            rd_kafka_message_destroy(rkmessage);
            continue;
        }

        const USProcessedFrameHeader* proc_h = reinterpret_cast<const USProcessedFrameHeader*>(rkmessage->payload);
        const uint8_t* payload_ptr = reinterpret_cast<const uint8_t*>(rkmessage->payload) + sizeof(USProcessedFrameHeader);
        size_t payload_size = proc_h->base.payload_length;
        const size_t expected_len = sizeof(USProcessedFrameHeader) + payload_size;
        if (rkmessage->len < expected_len) {
            rd_kafka_message_destroy(rkmessage);
            continue;
        }

        std::vector<uint8_t> frame(payload_ptr, payload_ptr + payload_size);
        frame_buffer.push_back(std::move(frame));

        if (frame_buffer.size() >= BURST_SIZE) {
            write_burst_csv(frame_buffer);
            frame_buffer.clear();
            frame_buffer.reserve(BURST_SIZE);
        }

        if (++total_frames % 100 == 0) {
            std::cout << "[ProcessSink] Buffered " << frame_buffer.size() << "/" << BURST_SIZE
                      << " frames (Total: " << total_frames << ")" << std::endl;
        }
        rd_kafka_message_destroy(rkmessage);
    }

    if (!frame_buffer.empty()) {
        std::cout << "[ProcessSink] Flushing final partial burst of "
                  << frame_buffer.size() << " frame(s) before shutdown." << std::endl;
        write_burst_csv(frame_buffer);
    }
}

int main() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::string brokers = "localhost:9092";
    if (const char* env = std::getenv("BOOTSTRAP_SERVERS")) brokers = env;

    rd_kafka_t* consumer = create_consumer(brokers, "process_sink_group");
    if (!consumer) return 1;

    consume_and_process(consumer);

    rd_kafka_consumer_close(consumer);
    rd_kafka_destroy(consumer);

    std::cout << "\n[ProcessSink] Done." << std::endl;
    return 0;
}
