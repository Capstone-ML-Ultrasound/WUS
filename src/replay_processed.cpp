#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <librdkafka/rdkafka.h>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "USFrameProtocol.h"

namespace {

const char* DEFAULT_TOPIC_OUT = "ultrasound_clean_replay";
const char* DEFAULT_GCS_PREFIX = "ultrasound/processed";
const int DEFAULT_FRAME_INTERVAL_MS = 20;
const uint64_t DEFAULT_DEVICE_ID = 9002;
const uint16_t DEFAULT_WINDOW_SIZE = 31;
const float DEFAULT_SIGMA_DEPTH = 2.0f;
const float DEFAULT_SIGMA_TIME = 5.0f;

volatile sig_atomic_t running = 1;

void signalHandler(int /*signum*/) {
    std::cout << "\n[ReplayProcessed] Shutdown signal received..." << std::endl;
    running = 0;
}

std::string get_env_or_default(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    std::string v(value);
    return v.empty() ? fallback : v;
}

bool is_truthy_env(const char* name) {
    const char* value = std::getenv(name);
    if (!value) return false;
    std::string v(value);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

int get_env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

uint64_t get_env_u64(const char* name, uint64_t fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    try {
        return static_cast<uint64_t>(std::stoull(value));
    } catch (...) {
        return fallback;
    }
}

float get_env_float(const char* name, float fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    try {
        return std::stof(value);
    } catch (...) {
        return fallback;
    }
}

std::string trim_copy(const std::string& input) {
    const char* ws = " \t\r\n";
    const size_t first = input.find_first_not_of(ws);
    if (first == std::string::npos) return "";
    const size_t last = input.find_last_not_of(ws);
    return input.substr(first, last - first + 1);
}

std::string basename_from_uri(const std::string& uri) {
    const size_t slash = uri.find_last_of('/');
    if (slash == std::string::npos) return uri;
    return uri.substr(slash + 1);
}

std::string wildcard_to_regex(const std::string& pattern) {
    std::string out = "^";
    for (char c : pattern) {
        switch (c) {
            case '*': out += ".*"; break;
            case '?': out += "."; break;
            case '.':
            case '+':
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
            case '^':
            case '$':
            case '|':
            case '\\':
                out += '\\';
                out += c;
                break;
            default:
                out += c;
                break;
        }
    }
    out += "$";
    return out;
}

bool pattern_matches(const std::string& candidate, const std::string& pattern) {
    if (pattern.find('*') != std::string::npos || pattern.find('?') != std::string::npos) {
        return std::regex_match(candidate, std::regex(wildcard_to_regex(pattern)));
    }
    return candidate.find(pattern) != std::string::npos;
}

struct GcsObject {
    std::string updated_at;
    std::string uri;
};

bool run_command_capture(const std::string& command, std::vector<std::string>& lines, int& exit_code) {
#if defined(_WIN32)
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) {
        exit_code = -1;
        return false;
    }

    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        lines.emplace_back(buffer);
    }

#if defined(_WIN32)
    exit_code = _pclose(pipe);
#else
    exit_code = pclose(pipe);
#endif
    return true;
}

std::vector<GcsObject> list_gcs_csv_objects(
    const std::string& bucket,
    const std::string& prefix,
    const std::string& project_id) {
    std::vector<GcsObject> objects;

    std::string cleaned_prefix = prefix;
    while (!cleaned_prefix.empty() && cleaned_prefix.back() == '/') cleaned_prefix.pop_back();
    const std::string pattern = "gs://" + bucket + "/" + cleaned_prefix + "/*.csv";

    std::string cmd;
    if (!project_id.empty()) {
        cmd = "gsutil -u \"" + project_id + "\" ls -l \"" + pattern + "\" 2>&1";
    } else {
        cmd = "gsutil ls -l \"" + pattern + "\" 2>&1";
    }

    std::vector<std::string> output_lines;
    int rc = 0;
    if (!run_command_capture(cmd, output_lines, rc)) {
        std::cerr << "[ReplayProcessed] Failed to execute gsutil ls command." << std::endl;
        return objects;
    }

    for (const std::string& raw_line : output_lines) {
        const std::string line = trim_copy(raw_line);
        if (line.empty() || line.rfind("TOTAL:", 0) == 0) continue;

        std::istringstream iss(line);
        std::string size_token;
        std::string date_token;
        std::string uri_token;
        if (!(iss >> size_token >> date_token >> uri_token)) continue;
        if (uri_token.rfind("gs://", 0) != 0) continue;
        if (uri_token.size() < 4 || uri_token.substr(uri_token.size() - 4) != ".csv") continue;

        objects.push_back({date_token, uri_token});
    }

    std::sort(objects.begin(), objects.end(), [](const GcsObject& a, const GcsObject& b) {
        if (a.updated_at == b.updated_at) return a.uri < b.uri;
        return a.updated_at < b.updated_at;
    });

    if (objects.empty()) {
        std::cerr << "[ReplayProcessed] No CSV files found under " << pattern << std::endl;
    }
    if (rc != 0 && objects.empty()) {
        std::cerr << "[ReplayProcessed] gsutil ls returned exit code " << rc << std::endl;
    }

    return objects;
}

std::vector<GcsObject> select_objects(
    const std::vector<GcsObject>& objects,
    const std::string& file_pattern) {
    if (objects.empty()) return {};

    if (file_pattern.empty()) {
        return {objects.back()};
    }

    if (file_pattern.rfind("gs://", 0) == 0) {
        for (const auto& obj : objects) {
            if (obj.uri == file_pattern) return {obj};
        }
        return {};
    }

    std::vector<GcsObject> selected;
    for (const auto& obj : objects) {
        const std::string name = basename_from_uri(obj.uri);
        if (pattern_matches(name, file_pattern) || pattern_matches(obj.uri, file_pattern)) {
            selected.push_back(obj);
        }
    }
    return selected;
}

bool copy_gcs_to_local(const std::string& uri, const std::string& local_path, const std::string& project_id) {
    std::string cmd;
    if (!project_id.empty()) {
        cmd = "gsutil -u \"" + project_id + "\" cp \"" + uri + "\" \"" + local_path + "\"";
    } else {
        cmd = "gsutil cp \"" + uri + "\" \"" + local_path + "\"";
    }

    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::cerr << "[ReplayProcessed] Failed to download " << uri << " (exit code " << rc << ")" << std::endl;
        return false;
    }
    return true;
}

bool parse_cell_to_u8(const std::string& token, uint8_t& value_out) {
    try {
        const int value = std::stoi(trim_copy(token));
        if (value < 0) value_out = 0;
        else if (value > 255) value_out = 255;
        else value_out = static_cast<uint8_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<std::vector<uint8_t>> read_csv_rows(const std::string& csv_path) {
    std::ifstream input(csv_path);
    if (!input.is_open()) {
        std::cerr << "[ReplayProcessed] Failed to open " << csv_path << std::endl;
        return {};
    }

    std::vector<std::vector<uint8_t>> rows;
    std::string line;
    int line_no = 0;
    while (std::getline(input, line)) {
        ++line_no;
        if (trim_copy(line).empty()) continue;

        std::vector<uint8_t> row;
        std::stringstream ss(line);
        std::string token;
        bool all_cells_valid = true;
        while (std::getline(ss, token, ',')) {
            uint8_t parsed = 0;
            if (!parse_cell_to_u8(token, parsed)) {
                all_cells_valid = false;
                break;
            }
            row.push_back(parsed);
        }

        if (!all_cells_valid) {
            if (line_no == 1) {
                continue;  // Skip optional header row
            }
            std::cerr << "[ReplayProcessed] Non-numeric CSV data at line " << line_no
                      << " in " << csv_path << std::endl;
            return {};
        }
        if (!row.empty()) rows.push_back(std::move(row));
    }

    return rows;
}

std::vector<std::vector<uint8_t>> rows_to_frames(const std::vector<std::vector<uint8_t>>& rows) {
    if (rows.empty()) return {};
    size_t frame_count = 0;
    for (const auto& row : rows) frame_count = std::max(frame_count, row.size());
    if (frame_count == 0) return {};

    const size_t sample_count = rows.size();
    std::vector<std::vector<uint8_t>> frames(frame_count, std::vector<uint8_t>(sample_count, 0));
    for (size_t sample_idx = 0; sample_idx < rows.size(); ++sample_idx) {
        for (size_t frame_idx = 0; frame_idx < rows[sample_idx].size(); ++frame_idx) {
            frames[frame_idx][sample_idx] = rows[sample_idx][frame_idx];
        }
    }
    return frames;
}

rd_kafka_t* create_producer(const std::string& brokers) {
    char errstr[512];
    rd_kafka_conf_t* conf = rd_kafka_conf_new();

    if (rd_kafka_conf_set(conf, "bootstrap.servers", brokers.c_str(), errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        std::cerr << "[ReplayProcessed] Producer config error: " << errstr << std::endl;
        return nullptr;
    }

    rd_kafka_t* rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
    if (!rk) {
        std::cerr << "[ReplayProcessed] Failed to create producer: " << errstr << std::endl;
        rd_kafka_conf_destroy(conf);
        return nullptr;
    }
    return rk;
}

bool publish_frame(
    rd_kafka_topic_t* topic,
    const std::vector<uint8_t>& frame,
    uint64_t sequence_number,
    uint64_t device_id,
    int64_t timestamp_ns,
    uint16_t window_size,
    float sigma_depth,
    float sigma_time) {
    USProcessedFrameHeader header{};
    std::memcpy(header.base.magic, US_MAGIC, 2);
    header.base.version = US_PROTOCOL_VERSION;
    header.base.message_type = 2;
    header.base.timestamp_ns = timestamp_ns;
    header.base.sequence_number = sequence_number;
    header.base.device_id = static_cast<uint32_t>(device_id);
    header.base.num_samples = static_cast<uint32_t>(frame.size());
    header.base.sample_format = 8;
    std::memset(header.base.reserved, 0, sizeof(header.base.reserved));
    header.base.payload_length = static_cast<uint32_t>(frame.size());

    header.processing_ts_ns = timestamp_ns;
    header.window_size = window_size;
    header.sigma_depth = sigma_depth;
    header.sigma_time = sigma_time;
    std::memset(header.reserved_proc, 0, sizeof(header.reserved_proc));

    std::vector<uint8_t> message;
    message.reserve(sizeof(USProcessedFrameHeader) + frame.size());
    const uint8_t* h_ptr = reinterpret_cast<const uint8_t*>(&header);
    message.insert(message.end(), h_ptr, h_ptr + sizeof(USProcessedFrameHeader));
    message.insert(message.end(), frame.begin(), frame.end());

    if (rd_kafka_produce(
            topic,
            RD_KAFKA_PARTITION_UA,
            RD_KAFKA_MSG_F_COPY,
            message.data(),
            message.size(),
            nullptr,
            0,
            nullptr) != 0) {
        std::cerr << "[ReplayProcessed] Failed to publish frame: " << rd_kafka_err2str(rd_kafka_last_error()) << std::endl;
        return false;
    }
    return true;
}

}  // namespace

int main() {
    signal(SIGINT, signalHandler);

    const std::string brokers = get_env_or_default("BOOTSTRAP_SERVERS", "localhost:9092");
    const std::string bucket = get_env_or_default("REPLAY_GCS_BUCKET", get_env_or_default("PROCESSED_GCS_BUCKET", ""));
    const std::string prefix = get_env_or_default("REPLAY_GCS_PREFIX", DEFAULT_GCS_PREFIX);
    const std::string file_pattern = get_env_or_default("REPLAY_FILE_PATTERN", "");
    const std::string topic_out = get_env_or_default("REPLAY_TOPIC_OUT", DEFAULT_TOPIC_OUT);
    const std::string local_dir = get_env_or_default("REPLAY_LOCAL_DIR", "/tmp/replay_processed");
    const std::string project_id = get_env_or_default("GCP_PROJECT_ID", "");
    const bool replay_loop = is_truthy_env("REPLAY_LOOP");
    const int frame_interval_ms = std::max(0, get_env_int("REPLAY_FRAME_INTERVAL_MS", DEFAULT_FRAME_INTERVAL_MS));
    uint64_t sequence = get_env_u64("REPLAY_SEQUENCE_START", 1);
    const uint64_t device_id = get_env_u64("REPLAY_DEVICE_ID", DEFAULT_DEVICE_ID);
    const uint16_t window_size = static_cast<uint16_t>(std::max(1, get_env_int("REPLAY_WINDOW_SIZE", DEFAULT_WINDOW_SIZE)));
    const float sigma_depth = get_env_float("REPLAY_SIGMA_DEPTH", DEFAULT_SIGMA_DEPTH);
    const float sigma_time = get_env_float("REPLAY_SIGMA_TIME", DEFAULT_SIGMA_TIME);

    if (bucket.empty()) {
        std::cerr << "[ReplayProcessed] Missing REPLAY_GCS_BUCKET (or PROCESSED_GCS_BUCKET fallback)." << std::endl;
        return 1;
    }

    std::filesystem::create_directories(local_dir);

    std::cout << "[ReplayProcessed] Brokers: " << brokers << std::endl;
    std::cout << "[ReplayProcessed] GCS source: gs://" << bucket << "/" << prefix << std::endl;
    std::cout << "[ReplayProcessed] Topic out: " << topic_out << std::endl;

    const auto objects = list_gcs_csv_objects(bucket, prefix, project_id);
    const auto selected = select_objects(objects, file_pattern);
    if (selected.empty()) {
        std::cerr << "[ReplayProcessed] No replay files selected." << std::endl;
        return 1;
    }

    if (file_pattern.empty()) {
        std::cout << "[ReplayProcessed] Default mode: replaying most recent file " << selected.front().uri << std::endl;
    } else {
        std::cout << "[ReplayProcessed] Selected " << selected.size() << " file(s) matching pattern '" << file_pattern << "'" << std::endl;
    }

    rd_kafka_t* producer = create_producer(brokers);
    if (!producer) return 1;

    rd_kafka_topic_t* topic = rd_kafka_topic_new(producer, topic_out.c_str(), nullptr);
    if (!topic) {
        std::cerr << "[ReplayProcessed] Failed to create topic handle for " << topic_out << std::endl;
        rd_kafka_destroy(producer);
        return 1;
    }

    auto now = std::chrono::system_clock::now().time_since_epoch();
    int64_t next_ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

    do {
        for (const auto& object : selected) {
            if (!running) break;

            const std::string local_file = local_dir + "/" + basename_from_uri(object.uri);
            std::cout << "[ReplayProcessed] Downloading " << object.uri << std::endl;
            if (!copy_gcs_to_local(object.uri, local_file, project_id)) continue;

            const auto rows = read_csv_rows(local_file);
            const auto frames = rows_to_frames(rows);
            if (frames.empty()) {
                std::cerr << "[ReplayProcessed] No frames decoded from " << local_file << std::endl;
                std::error_code rm_ec;
                std::filesystem::remove(local_file, rm_ec);
                continue;
            }

            std::cout << "[ReplayProcessed] Publishing " << frames.size()
                      << " frame(s), samples/frame=" << frames.front().size() << std::endl;

            int published = 0;
            for (const auto& frame : frames) {
                if (!running) break;
                if (publish_frame(topic, frame, sequence++, device_id, next_ts_ns, window_size, sigma_depth, sigma_time)) {
                    ++published;
                    rd_kafka_poll(producer, 0);
                }
                next_ts_ns += static_cast<int64_t>(frame_interval_ms) * 1000000LL;
                if (frame_interval_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(frame_interval_ms));
                }
            }

            std::cout << "[ReplayProcessed] Published " << published << " frame(s) from " << object.uri << std::endl;

            std::error_code rm_ec;
            std::filesystem::remove(local_file, rm_ec);
            if (rm_ec) {
                std::cerr << "[ReplayProcessed] Failed to remove temporary file " << local_file
                          << ": " << rm_ec.message() << std::endl;
            }
        }
    } while (running && replay_loop);

    rd_kafka_flush(producer, 10000);
    rd_kafka_topic_destroy(topic);
    rd_kafka_destroy(producer);

    std::cout << "[ReplayProcessed] Done." << std::endl;
    return 0;
}
