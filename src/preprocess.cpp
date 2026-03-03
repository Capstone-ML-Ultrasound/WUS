#include <iostream>
#include <vector>
#include <deque>
#include <cmath>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <armadillo>
#include <librdkafka/rdkafka.h>

#include "USFrameProtocol.h"

// =================================================================================================
// Configuration Constants
// =================================================================================================

// Kafka
const char* TOPIC_IN_DEFAULT  = "ultrasound_raw_data";
const char* TOPIC_OUT = "ultrasound_clean";

// Signal Processing
const double TGC_ALPHA   = 0.000004;
const double SIGMA_DEPTH = 2.0; // 1D Blur (Vertical)
const double SIGMA_TIME  = 5.0; // 1D Blur (Horizontal/Time)

// Sliding Window
const int KERNEL_RADIUS_TIME = static_cast<int>(std::ceil(3.0 * SIGMA_TIME));
const int WINDOW_SIZE = 2 * KERNEL_RADIUS_TIME + 1; // e.g. 31 frames if Sigma=5

// =================================================================================================
// Globals & Signals
// =================================================================================================

volatile sig_atomic_t running = 1;

void signalHandler(int signum) {
    std::cout << "\n[Preprocess] Shutdown signal received..." << std::endl;
    running = 0;
}

// =================================================================================================
// Kafka Helper Functions
// =================================================================================================

/**
 * Creates and configures a Kafka Consumer.
 */
rd_kafka_t* create_consumer(const std::string& brokers, const std::string& group_id) {
    char errstr[512];
    rd_kafka_conf_t *conf = rd_kafka_conf_new();

    if (rd_kafka_conf_set(conf, "bootstrap.servers", brokers.c_str(), errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        std::cerr << "[Consumer] Config error: " << errstr << std::endl;
        return nullptr;
    }
    if (rd_kafka_conf_set(conf, "group.id", group_id.c_str(), errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        std::cerr << "[Consumer] Config error: " << errstr << std::endl;
        return nullptr;
    }
    if (rd_kafka_conf_set(conf, "auto.offset.reset", "latest", errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        std::cerr << "[Consumer] Config error: " << errstr << std::endl;
        return nullptr;
    }

    rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
    if (!rk) {
        std::cerr << "[Consumer] Failed to create: " << errstr << std::endl;
        // conf is consumed by new, no need to free if it fails? Check docs. 
        // actually rd_kafka_new takes ownership of conf ONLY on success usually, but for safety:
        // if rk is null, conf persists.
        rd_kafka_conf_destroy(conf); 
        return nullptr;
    }
    return rk;
}

/**
 * Creates and configures a Kafka Producer.
 */
rd_kafka_t* create_producer(const std::string& brokers) {
    char errstr[512];
    rd_kafka_conf_t *conf = rd_kafka_conf_new();

    if (rd_kafka_conf_set(conf, "bootstrap.servers", brokers.c_str(), errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        std::cerr << "[Producer] Config error: " << errstr << std::endl;
        return nullptr;
    }

    rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
    if (!rk) {
        std::cerr << "[Producer] Failed to create: " << errstr << std::endl;
        rd_kafka_conf_destroy(conf);
        return nullptr;
    }
    return rk;
}

// =================================================================================================
// Signal Processing Helpers
// =================================================================================================

/**
 * Generate a 1D Gaussian Kernel.
 */
arma::vec generate_gaussian_kernel(double sigma) {
    int radius = std::ceil(3.0 * sigma);
    int size = 2 * radius + 1;
    arma::vec kernel(size);
    double sum = 0.0;
    
    for (int i = 0; i < size; ++i) {
        double x = i - radius;
        double val = std::exp(-(x*x) / (2 * sigma * sigma));
        kernel(i) = val;
        sum += val;
    }
    kernel /= sum; // Normalize
    return kernel;
}

// =================================================================================================
// Processing Loop
// =================================================================================================

void process_stream(
    rd_kafka_t* consumer,
    rd_kafka_t* producer,
    rd_kafka_topic_t* producer_topic,
    const std::string& topic_in) {
    
    // 1. Subscribe
    rd_kafka_topic_partition_list_t *subscription = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_list_add(subscription, topic_in.c_str(), RD_KAFKA_PARTITION_UA);
    rd_kafka_subscribe(consumer, subscription);
    rd_kafka_topic_partition_list_destroy(subscription);

    std::cout << "[Preprocess] Subscribed to " << topic_in << std::endl;
    std::cout << "[Preprocess] Waiting for frames to fill buffer (" << WINDOW_SIZE << " frames)..." << std::endl;

    // 2. Pre-compute Kernels and Curves
    // We assume 512 samples for now, but will resize TGC if needed
    arma::vec tgc_curve; 
    bool tgc_initialized = false;
    
    // Depth Blur Kernel (Sigma = 2.0)
    arma::vec depth_kernel = generate_gaussian_kernel(SIGMA_DEPTH);

    // Time Blur Kernel (Sigma = 5.0) for the sliding window
    arma::vec time_kernel = generate_gaussian_kernel(SIGMA_TIME);

    // 3. Sliding Window Buffer
    // We store fully pre-processed (TGC, Env, Log, DepthBlur) frames here.
    // The "Time Blur" is the only thing left to do.
    std::deque<arma::vec> window;
    std::deque<USFrameHeader> header_window;

    int msg_count = 0;

    while (running) {
        rd_kafka_message_t *rkmessage = rd_kafka_consumer_poll(consumer, 200);

        if (!rkmessage) continue; // Timeout
        if (rkmessage->err) {
            std::cerr << "[Consumer] Error: " << rd_kafka_message_errstr(rkmessage) << std::endl;
            rd_kafka_message_destroy(rkmessage);
            continue;
        }
        // std::cout << "[Preprocess] Consumed message, len=" << rkmessage->len << std::endl;

        // --- Step A: Decode ---
        USProtocol::DecodedFrame decoded;
        if (!USProtocol::decodeEnvelope(rkmessage->payload, rkmessage->len, decoded)) {
             std::cerr << "[Preprocess] Invalid Frame received" << std::endl;
             rd_kafka_message_destroy(rkmessage);
             continue;
        }

        // Convert Payload to Armadillo Vector
        int n_samples = decoded.payloadSize;
        arma::vec frame_signal(n_samples);
        for(int i=0; i<n_samples; ++i) {
            frame_signal(i) = static_cast<double>(decoded.payload[i]);
        }

        // --- Step B: Per-Frame Processing (Depth Axis) ---
        
        // B.1 Lazy Initialize TGC
        if (!tgc_initialized || tgc_curve.n_elem != n_samples) {
            tgc_curve.set_size(n_samples);
            for(int i=0; i<n_samples; ++i) tgc_curve(i) = std::exp(TGC_ALPHA * i);
            tgc_initialized = true;
        }

        // B.2 TGC
        frame_signal = frame_signal % tgc_curve;

        // B.3 DC Removal
        frame_signal = frame_signal - arma::mean(frame_signal);

        // B.4 Envelope (Hilbert)
        // Note: Armadillo fft requires complex numbers
        arma::cx_vec fft_out = arma::fft(frame_signal);
        // Zero out negative frequencies (analytic signal)
        if (n_samples > 0) {
            // fft_out(0) remains
            if (n_samples/2 > 1) fft_out.subvec(1, n_samples/2 - 1) *= 2.0;
            if (n_samples/2 + 1 < n_samples) fft_out.subvec(n_samples/2 + 1, n_samples - 1).zeros();
        }
        arma::vec envelope = arma::abs(arma::ifft(fft_out));

        // B.5 Log Scale
        arma::vec log_signal = arma::log10(envelope + 1e-6);

        // B.6 Gaussian Blur (Depth / Vertical) 
        // "same" convolution mode keeps size equal
        arma::vec depth_blurred = arma::conv(log_signal, depth_kernel, "same");


        // --- Step C: Buffer Management ---
        window.push_back(depth_blurred);
        header_window.push_back(*decoded.header);

        if (window.size() > WINDOW_SIZE) {
            window.pop_front();
            header_window.pop_front();
        }

        // --- Step D: Time Blur (Horizontal) & Produce ---
        // Only produce if buffer is full (latency period over)
        if (window.size() == WINDOW_SIZE) {
            
            // Weighted sum across time
            arma::vec temporal_result(n_samples, arma::fill::zeros);
            
            // Iterate window
            // time_kernel index 0 corresponds to window[0] .. and so on
            // This applies the Gaussian weights across the deque
            for (size_t i = 0; i < window.size(); ++i) {
                temporal_result += window[i] * time_kernel(i);
            }

            // --- Step E: Send to Kafka ---
            // Middle frame header (Identity Source)
            const USFrameHeader& mid_header = header_window[KERNEL_RADIUS_TIME];
            
            // Construct Processed Header
            USProcessedFrameHeader out_header;
            out_header.base = mid_header;
            out_header.base.message_type = 2; // PROCESSED/BLURRED
            out_header.base.payload_length = static_cast<uint32_t>(n_samples);
            
            // Provenance
            auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            out_header.processing_ts_ns = now_ns;
            out_header.window_size = static_cast<uint16_t>(WINDOW_SIZE);
            out_header.sigma_depth = static_cast<float>(SIGMA_DEPTH);
            out_header.sigma_time = static_cast<float>(SIGMA_TIME);
            std::memset(out_header.reserved_proc, 0, sizeof(out_header.reserved_proc));

            // Normalize Payload
            double min_val = temporal_result.min();
            double max_val = temporal_result.max();
            if (max_val - min_val < 0.001) max_val = min_val + 1.0;

            std::vector<unsigned char> payload_bytes(n_samples);
            for(int i=0; i<n_samples; ++i) {
                double val = (temporal_result(i) - min_val) / (max_val - min_val) * 255.0;
                if (val < 0) val = 0;
                if (val > 255) val = 255;
                payload_bytes[i] = static_cast<uint8_t>(val);
            }

            // Serialize (Header + Payload)
            std::vector<uint8_t> msg;
            msg.reserve(sizeof(USProcessedFrameHeader) + payload_bytes.size());
            
            const uint8_t* h_ptr = reinterpret_cast<const uint8_t*>(&out_header);
            msg.insert(msg.end(), h_ptr, h_ptr + sizeof(USProcessedFrameHeader));
            msg.insert(msg.end(), payload_bytes.begin(), payload_bytes.end());

            // Produce
            if (rd_kafka_produce(
                producer_topic, RD_KAFKA_PARTITION_UA,
                RD_KAFKA_MSG_F_COPY,
                msg.data(), msg.size(),
                NULL, 0, NULL) != 0) {
                 std::cerr << "[Producer] Failed to send detected frame" << std::endl;
            } else {
                 rd_kafka_poll(producer, 0); // Trigger callbacks
            }

        if (++msg_count % 10 == 0) {
                 std::cout << "[Preprocess] Processed " << mid_header.sequence_number << " frames..." << std::endl;
            }
        }

        rd_kafka_message_destroy(rkmessage);
    }
}

// =================================================================================================
// Main
// =================================================================================================

int main(int argc, char** argv) {
    signal(SIGINT, signalHandler);

    std::string brokers = "localhost:9092";
    if (const char* env_brokers = std::getenv("BOOTSTRAP_SERVERS")) {
        brokers = env_brokers;
    }
    std::string topic_in = TOPIC_IN_DEFAULT;
    if (const char* env_topic_in = std::getenv("TOPIC_IN")) {
        if (std::strlen(env_topic_in) > 0) topic_in = env_topic_in;
    }

    // 1. Setup Consumer
    std::cout << "[Preprocess] Connecting to Kafka at " << brokers << "..." << std::endl;
    rd_kafka_t* consumer = create_consumer(brokers, "preprocess_group_01");
    if (!consumer) return 1;

    // 2. Setup Producer
    rd_kafka_t* producer = create_producer(brokers);
    if (!producer) {
        rd_kafka_consumer_close(consumer);
        rd_kafka_destroy(consumer);
        return 1;
    }
    
    // 3. Create Producer Topic Handle
    rd_kafka_topic_t* producer_topic = rd_kafka_topic_new(producer, TOPIC_OUT, NULL);

    // 4. Run Process Loop
    process_stream(consumer, producer, producer_topic, topic_in);

    // 5. Cleanup
    std::cout << "\n[Preprocess] Cleaning up..." << std::endl;
    rd_kafka_topic_destroy(producer_topic);
    
    rd_kafka_flush(producer, 5000);
    rd_kafka_destroy(producer);

    rd_kafka_consumer_close(consumer);
    rd_kafka_destroy(consumer);

    // Armadillo cleanup not strictly needed (RAII)
    return 0;
}
