#include <iostream>
#include <fstream>
#include <vector>
#include <csignal>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <limits>
#include <librdkafka/rdkafka.h>

#include "USFrameProtocol.h"

#include <filesystem>

// =================================================================================================
// Configuration
// =================================================================================================

const char* TOPIC_IN = "ultrasound_raw_data";
const int BURST_SIZE = 1000; // Frames per CSV file
const int POLL_TIMEOUT_MS = 500;

volatile sig_atomic_t running = 1;

void signalHandler(int signum) {
    std::cout << "\n[RawSink] Shutdown signal received..." << std::endl;
    running = 0;
}

bool is_truthy_env(const char* value) {
    if (!value) return false;
    std::string v(value);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return v == "1" || v == "true" || v == "yes" || v == "on";
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

bool ensure_output_dir(const std::string& output_dir) {
    std::error_code ec;
    if (std::filesystem::exists(output_dir, ec)) return true;
    if (ec) {
        std::cerr << "[RawSink] Failed to inspect output directory " << output_dir
                  << ": " << ec.message() << std::endl;
        return false;
    }

    if (std::filesystem::create_directories(output_dir, ec)) return true;
    if (ec) {
        std::cerr << "[RawSink] Failed to create output directory " << output_dir
                  << ": " << ec.message() << std::endl;
        return false;
    }
    return true;
}

bool try_parse_session_part_filename(
    const std::filesystem::path& path,
    int& session_number,
    int& part_number) {
    if (path.extension() != ".csv") return false;

    const std::string stem = path.stem().string();
    const std::string session_prefix = "session_";
    const std::string part_marker = "_part_";

    if (stem.rfind(session_prefix, 0) != 0) return false;

    const size_t part_pos = stem.find(part_marker, session_prefix.size());
    if (part_pos == std::string::npos) return false;

    const std::string session_text = stem.substr(session_prefix.size(), part_pos - session_prefix.size());
    const std::string suffix = stem.substr(part_pos + part_marker.size());
    const size_t frames_pos = suffix.find('_');
    const std::string part_text = suffix.substr(0, frames_pos);
    if (session_text.empty() || part_text.empty()) return false;

    const auto is_number = [](const std::string& value) {
        return std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        });
    };

    if (!is_number(session_text) || !is_number(part_text)) return false;

    session_number = std::stoi(session_text);
    part_number = std::stoi(part_text);
    return true;
}

int get_max_local_session_number(const std::string& output_dir) {
    if (!ensure_output_dir(output_dir)) return 0;

    int max_session_number = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(output_dir, ec)) {
        if (ec) {
            std::cerr << "[RawSink] Failed to scan output directory " << output_dir
                      << ": " << ec.message() << std::endl;
            return max_session_number;
        }
        if (!entry.is_regular_file()) continue;

        int session_number = 0;
        int part_number = 0;
        if (try_parse_session_part_filename(entry.path(), session_number, part_number)) {
            max_session_number = std::max(max_session_number, session_number);
        }
    }

    return max_session_number;
}

int get_max_gcs_session_number() {
    const char* bucket = std::getenv("RAW_GCS_BUCKET");
    if (!bucket || std::string(bucket).empty()) return 0;

    std::string prefix = "ultrasound/raw";
    if (const char* env_prefix = std::getenv("RAW_GCS_PREFIX")) {
        if (std::string(env_prefix).size() > 0) prefix = env_prefix;
    }
    if (!prefix.empty() && prefix.back() == '/') prefix.pop_back();

    const std::string gs_uri = "gs://" + std::string(bucket) + "/" + prefix + "/session_*_part_*.csv";
    std::string cmd = "gsutil ls \"" + gs_uri + "\" 2>/dev/null";
    if (const char* project = std::getenv("GCP_PROJECT_ID")) {
        if (std::string(project).size() > 0) {
            cmd = "gsutil -u \"" + std::string(project) + "\" ls \"" + gs_uri + "\" 2>/dev/null";
        }
    }

#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) {
        std::cerr << "[RawSink] Failed to inspect GCS for prior session numbers." << std::endl;
        return 0;
    }

    int max_session_number = 0;
    char buffer[1024];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line(buffer);
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());
        if (line.empty()) continue;

        int session_number = 0;
        int part_number = 0;
        if (try_parse_session_part_filename(std::filesystem::path(line), session_number, part_number)) {
            max_session_number = std::max(max_session_number, session_number);
        }
    }

#ifdef _WIN32
    const int rc = _pclose(pipe);
#else
    const int rc = pclose(pipe);
#endif
    if (rc != 0 && max_session_number == 0) {
        std::cerr << "[RawSink] GCS session scan did not return any prior session files." << std::endl;
    }

    return max_session_number;
}

int get_next_session_number(const std::string& output_dir) {
    const int max_local_session_number = get_max_local_session_number(output_dir);
    const int max_gcs_session_number = get_max_gcs_session_number();
    const int max_session_number = std::max(max_local_session_number, max_gcs_session_number);
    if (max_gcs_session_number > 0) {
        std::cout << "[RawSink] Highest existing session found in GCS: session_"
                  << max_gcs_session_number << "." << std::endl;
    }
    if (max_local_session_number > 0) {
        std::cout << "[RawSink] Highest existing local session: session_"
                  << max_local_session_number << "." << std::endl;
    }
    return max_session_number + 1;
}

bool upload_file_to_gcs(const std::string& local_file) {
    const char* bucket = std::getenv("RAW_GCS_BUCKET");
    if (!bucket || std::string(bucket).empty()) return false;

    std::string prefix = "ultrasound/raw";
    if (const char* env_prefix = std::getenv("RAW_GCS_PREFIX")) {
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

    std::cout << "[RawSink] Uploading burst to " << gs_uri << "..." << std::endl;
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::cerr << "[RawSink] GCS upload failed (exit code " << rc << "). Keeping local file." << std::endl;
        return false;
    }

    std::cout << "[RawSink] GCS upload complete." << std::endl;
    return true;
}

// =================================================================================================
// Kafka Helper
// =================================================================================================

rd_kafka_t* create_consumer(const std::string& brokers, const std::string& group_id) {
    char errstr[512];
    rd_kafka_conf_t *conf = rd_kafka_conf_new();

    if (rd_kafka_conf_set(conf, "bootstrap.servers", brokers.c_str(), errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        std::cerr << "[Config] " << errstr << std::endl;
        rd_kafka_conf_destroy(conf);
        return nullptr;
    }
    if (rd_kafka_conf_set(conf, "group.id", group_id.c_str(), errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        std::cerr << "[Config] " << errstr << std::endl;
        rd_kafka_conf_destroy(conf);
        return nullptr;
    }
    if (rd_kafka_conf_set(conf, "auto.offset.reset", "latest", errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        std::cerr << "[Config] " << errstr << std::endl;
        rd_kafka_conf_destroy(conf);
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

void write_burst_csv(
    const std::vector<std::vector<uint8_t>>& buffer,
    const std::vector<int64_t>& timestamps,
    int session_number,
    int part_number) {
    const std::string output_dir = "data/verify";
    if (!ensure_output_dir(output_dir)) return;

    if (buffer.empty()) return;

    const int frames = static_cast<int>(buffer.size());
    std::stringstream ss;
    ss << output_dir << "/session_" << session_number << "_part_" << part_number
       << "_" << frames << ".csv";
    const std::string filename = ss.str();

    std::ofstream csv_file(filename);
    if (!csv_file.is_open()) {
        std::cerr << "[Error] Could not open " << filename << " for writing." << std::endl;
        return;
    }

    // Transpose write: Rows = Depth (height), Cols = Time (width/buffer size)
    // buffer[frame_idx][depth_idx]
    
    // Validate dimensions
    int samples = buffer[0].size(); 

    std::cout << "[RawSink] Writing burst to " << filename << " (" << samples << "x" << frames << ")..." << std::endl;

    // Write timestamps as the first row
    for (int f = 0; f < frames; ++f) {
        if (f < static_cast<int>(timestamps.size())) {
            csv_file << timestamps[f];
        } else {
            csv_file << "0";
        }
        if (f < frames - 1) csv_file << ",";
    }
    csv_file << "\n";

    for (int d = 0; d < samples; ++d) {
        for (int f = 0; f < frames; ++f) {
            // Safety check
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
    std::cout << "[RawSink] Burst written." << std::endl;

    // Optional Kappa archive path: upload immutable raw burst to object storage.
    const bool uploaded = upload_file_to_gcs(filename);
    const bool keep_local = is_truthy_env(std::getenv("RAW_GCS_KEEP_LOCAL"));
    if (uploaded && !keep_local) {
        std::error_code ec;
        std::filesystem::remove(filename, ec);
        if (ec) {
            std::cerr << "[RawSink] Uploaded but failed to remove local file: " << ec.message() << std::endl;
        } else {
            std::cout << "[RawSink] Removed local file after successful upload." << std::endl;
        }
    }
}

// =================================================================================================
// Main Consumer Loop
// =================================================================================================

void consume_and_process(rd_kafka_t* consumer) {
    rd_kafka_topic_partition_list_t *subscription = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_list_add(subscription, TOPIC_IN, RD_KAFKA_PARTITION_UA);
    const rd_kafka_resp_err_t subscribe_err = rd_kafka_subscribe(consumer, subscription);
    rd_kafka_topic_partition_list_destroy(subscription);
    if (subscribe_err != RD_KAFKA_RESP_ERR_NO_ERROR) {
        std::cerr << "[RawSink] Subscription failed: " << rd_kafka_err2str(subscribe_err) << std::endl;
        return;
    }

    const std::string output_dir = "data/verify";
    int next_session_number = get_next_session_number(output_dir);
    int session_number = 0;
    int part_number = 1;
    int total_frames = 0;
    int idle_elapsed_ms = 0;
    const int idle_flush_ms = get_nonnegative_env_int("RAW_SINK_IDLE_FLUSH_MS", 2000);
    bool session_active = false;
    bool has_last_sequence = false;
    uint64_t last_sequence_number = 0;

    std::cout << "[RawSink] Subscribed to " << TOPIC_IN << ". Buffering " << BURST_SIZE << " frames per burst." << std::endl;
    std::cout << "[RawSink] Waiting for acquisition data. A new session starts after "
              << idle_flush_ms << " ms of inactivity or a producer restart." << std::endl;

    std::vector<std::vector<uint8_t>> frame_buffer;
    frame_buffer.reserve(BURST_SIZE);
    
    std::vector<int64_t> timestamp_buffer;
    timestamp_buffer.reserve(BURST_SIZE);

    const auto flush_current_session = [&](bool session_complete) {
        if (!frame_buffer.empty()) {
            std::cout << "[RawSink] Finalizing session_" << session_number
                      << " part_" << part_number << " with " << frame_buffer.size()
                      << " frames." << std::endl;
            write_burst_csv(frame_buffer, timestamp_buffer, session_number, part_number);
            ++part_number;
            frame_buffer.clear();
            frame_buffer.reserve(BURST_SIZE);
            timestamp_buffer.clear();
            timestamp_buffer.reserve(BURST_SIZE);
        }

        if (session_complete && session_active) {
            std::cout << "[RawSink] Session_" << session_number << " complete." << std::endl;
            session_active = false;
            has_last_sequence = false;
            total_frames = 0;
        }
    };

    while (running) {
        rd_kafka_message_t *rkmessage = rd_kafka_consumer_poll(consumer, POLL_TIMEOUT_MS);

        if (!rkmessage) {
            idle_elapsed_ms += POLL_TIMEOUT_MS;
            if (session_active && idle_flush_ms > 0 && idle_elapsed_ms >= idle_flush_ms) {
                flush_current_session(true);
            }
            continue;
        }
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

        const int gap_before_message_ms = idle_elapsed_ms;
        idle_elapsed_ms = 0;

        // Extract Payload
        const uint8_t* payload_ptr = nullptr;
        size_t payload_size = 0;
        const size_t header_size = sizeof(USFrameHeader);

        // Check for Raw A-Scan (Type 1)
        if (base_h->message_type == 1) { 
            if (base_h->sample_format != 8) {
                rd_kafka_message_destroy(rkmessage);
                continue;
            }
            if (base_h->payload_length != base_h->num_samples) {
                rd_kafka_message_destroy(rkmessage);
                continue;
            }
            if (rkmessage->len < header_size + static_cast<size_t>(base_h->payload_length)) {
                rd_kafka_message_destroy(rkmessage);
                continue;
            }

            payload_ptr = reinterpret_cast<const uint8_t*>(rkmessage->payload) + header_size;
            payload_size = static_cast<size_t>(base_h->payload_length);
        } else {
             // Ignore other types
             rd_kafka_message_destroy(rkmessage);
             continue;
        }

        const bool sequence_reset = has_last_sequence && base_h->sequence_number < last_sequence_number;
        const bool idle_session_restart = session_active && idle_flush_ms > 0 && gap_before_message_ms >= idle_flush_ms;

        if (!session_active || idle_session_restart || sequence_reset) {
            if (session_active) {
                if (sequence_reset) {
                    std::cout << "[RawSink] Detected acquisition restart via sequence reset ("
                              << last_sequence_number << " -> " << base_h->sequence_number << ")." << std::endl;
                }
                flush_current_session(true);
            }

            session_number = next_session_number++;
            part_number = 1;
            session_active = true;
            total_frames = 0;
            std::cout << "[RawSink] Starting session_" << session_number << "." << std::endl;
        }

        // Copy frame to vector
        std::vector<uint8_t> frame(payload_ptr, payload_ptr + payload_size);
        frame_buffer.push_back(std::move(frame));
        timestamp_buffer.push_back(base_h->timestamp_ns);
        last_sequence_number = base_h->sequence_number;
        has_last_sequence = true;

        // Check Buffer
        if (frame_buffer.size() >= BURST_SIZE) {
            write_burst_csv(frame_buffer, timestamp_buffer, session_number, part_number);
            ++part_number;
            frame_buffer.clear();
            frame_buffer.reserve(BURST_SIZE);
            timestamp_buffer.clear();
            timestamp_buffer.reserve(BURST_SIZE);
        }

        if (++total_frames % 100 == 0) {
            std::cout << "[RawSink] Buffered " << frame_buffer.size() << "/" << BURST_SIZE << " frames (Total: " << total_frames << ")" << std::endl;
        }

        rd_kafka_message_destroy(rkmessage);
    }

    flush_current_session(false);
}

int main() {
    signal(SIGINT, signalHandler);

    std::string brokers = "localhost:9092";
    if (const char* env = std::getenv("BOOTSTRAP_SERVERS")) brokers = env;

    rd_kafka_t* consumer = create_consumer(brokers, "raw_sink_group");
    if (!consumer) return 1;

    consume_and_process(consumer);

    rd_kafka_consumer_close(consumer);
    rd_kafka_destroy(consumer);

    std::cout << "\n[RawSink] Done." << std::endl;
    return 0;
}
