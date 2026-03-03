#include <iostream>
#include <vector>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <librdkafka/rdkafka.h>

#include "USFrameProtocol.h"

// =================================================================================================
// Configuration
// =================================================================================================

const char* TOPIC_IN_DEFAULT  = "ultrasound.clean";
const char* TOPIC_OUT = "model.predictions";

volatile sig_atomic_t running = 1;

void signalHandler(int signum) {
    std::cout << "\n[Output] Shutdown signal received..." << std::endl;
    running = 0;
}

struct ModelPrediction {
    uint64_t source_sequence = 0;
    uint64_t source_ts_ns = 0;
    uint64_t prediction_ts_ns = 0;
    std::string label;
    float confidence = 0.0f;
};

// =================================================================================================
// Kafka Helpers
// =================================================================================================

rd_kafka_t* create_consumer(const std::string& brokers, const std::string& group_id) {
    char errstr[512];
    rd_kafka_conf_t* conf = rd_kafka_conf_new();

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

    rd_kafka_t* rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
    if (!rk) {
        std::cerr << "[Consumer] Failed to create: " << errstr << std::endl;
        rd_kafka_conf_destroy(conf);
        return nullptr;
    }
    return rk;
}

rd_kafka_t* create_producer(const std::string& brokers) {
    char errstr[512];
    rd_kafka_conf_t* conf = rd_kafka_conf_new();

    if (rd_kafka_conf_set(conf, "bootstrap.servers", brokers.c_str(), errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        std::cerr << "[Producer] Config error: " << errstr << std::endl;
        return nullptr;
    }

    rd_kafka_t* rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
    if (!rk) {
        std::cerr << "[Producer] Failed to create: " << errstr << std::endl;
        rd_kafka_conf_destroy(conf);
        return nullptr;
    }
    return rk;
}

// =================================================================================================
// Placeholder Inference + Publish
// =================================================================================================

ModelPrediction run_model_placeholder(const USProcessedFrameHeader* header, const uint8_t* payload, size_t payload_size) {
    ModelPrediction pred;

    pred.source_sequence = header->base.sequence_number;
    pred.source_ts_ns = header->base.timestamp_ns;
    pred.prediction_ts_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());

    // Placeholder model behavior:
    // Use average intensity to produce a binary hand-state prediction.
    double sum = 0.0;
    for (size_t i = 0; i < payload_size; ++i) {
        sum += static_cast<double>(payload[i]);
    }

    const double mean_intensity = payload_size > 0 ? sum / static_cast<double>(payload_size) : 0.0;
    const double normalized_distance = std::fabs(mean_intensity - 127.5) / 127.5;

    pred.label = (mean_intensity >= 127.5) ? "open_hand" : "closed_hand";
    pred.confidence = static_cast<float>(std::min(1.0, normalized_distance));

    return pred;
}

std::string serialize_prediction_json(const ModelPrediction& pred) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(3);
    ss << "{"
       << "\"source_sequence\":" << pred.source_sequence << ","
       << "\"source_ts_ns\":" << pred.source_ts_ns << ","
       << "\"prediction_ts_ns\":" << pred.prediction_ts_ns << ","
       << "\"label\":\"" << pred.label << "\","
       << "\"confidence\":" << pred.confidence
       << "}";
    return ss.str();
}

void publish_prediction(rd_kafka_t* producer, rd_kafka_topic_t* producer_topic, const ModelPrediction& pred) {
    const std::string message = serialize_prediction_json(pred);

    if (rd_kafka_produce(
        producer_topic,
        RD_KAFKA_PARTITION_UA,
        RD_KAFKA_MSG_F_COPY,
        const_cast<char*>(message.data()),
        message.size(),
        NULL,
        0,
        NULL) != 0) {
        std::cerr << "[Output] Failed to publish prediction: " << rd_kafka_err2str(rd_kafka_last_error()) << std::endl;
    } else {
        rd_kafka_poll(producer, 0);
    }
}

// =================================================================================================
// Main Consumer Loop
// =================================================================================================

void consume_and_predict(
    rd_kafka_t* consumer,
    rd_kafka_t* producer,
    rd_kafka_topic_t* producer_topic,
    const std::string& topic_in) {
    rd_kafka_topic_partition_list_t* subscription = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_list_add(subscription, topic_in.c_str(), RD_KAFKA_PARTITION_UA);
    rd_kafka_subscribe(consumer, subscription);
    rd_kafka_topic_partition_list_destroy(subscription);

    std::cout << "[Output] Subscribed to " << topic_in << std::endl;
    std::cout << "[Output] Publishing predictions to " << TOPIC_OUT << std::endl;

    int total_predictions = 0;

    while (running) {
        rd_kafka_message_t* rkmessage = rd_kafka_consumer_poll(consumer, 500);

        if (!rkmessage) continue;
        if (rkmessage->err) {
            if (rkmessage->err != RD_KAFKA_RESP_ERR__PARTITION_EOF) {
                std::cerr << "[Output] Consumer error: " << rd_kafka_message_errstr(rkmessage) << std::endl;
            }
            rd_kafka_message_destroy(rkmessage);
            continue;
        }

        if (rkmessage->len < sizeof(USFrameHeader)) {
            rd_kafka_message_destroy(rkmessage);
            continue;
        }

        const USFrameHeader* base_h = reinterpret_cast<const USFrameHeader*>(rkmessage->payload);
        if (std::strncmp(base_h->magic, US_MAGIC, 2) != 0) {
            rd_kafka_message_destroy(rkmessage);
            continue;
        }

        if (base_h->message_type != 2 || rkmessage->len < sizeof(USProcessedFrameHeader)) {
            rd_kafka_message_destroy(rkmessage);
            continue;
        }

        const USProcessedFrameHeader* proc_h = reinterpret_cast<const USProcessedFrameHeader*>(rkmessage->payload);
        const uint8_t* payload_ptr = reinterpret_cast<const uint8_t*>(rkmessage->payload) + sizeof(USProcessedFrameHeader);
        const size_t payload_size = base_h->payload_length;

        const size_t expected_len = sizeof(USProcessedFrameHeader) + payload_size;
        if (rkmessage->len < expected_len) {
            rd_kafka_message_destroy(rkmessage);
            continue;
        }

        const ModelPrediction pred = run_model_placeholder(proc_h, payload_ptr, payload_size);
        publish_prediction(producer, producer_topic, pred);

        ++total_predictions;
        if (total_predictions % 50 == 0) {
            std::cout << "[Output] Published " << total_predictions
                      << " predictions. Last label=" << pred.label
                      << " confidence=" << pred.confidence << std::endl;
        }

        rd_kafka_message_destroy(rkmessage);
    }
}

int main() {
    signal(SIGINT, signalHandler);

    std::string brokers = "localhost:9092";
    if (const char* env = std::getenv("BOOTSTRAP_SERVERS")) brokers = env;
    std::string topic_in = TOPIC_IN_DEFAULT;
    if (const char* env = std::getenv("TOPIC_IN")) {
        if (std::strlen(env) > 0) topic_in = env;
    }

    rd_kafka_t* consumer = create_consumer(brokers, "output_model_group");
    if (!consumer) return 1;

    rd_kafka_t* producer = create_producer(brokers);
    if (!producer) {
        rd_kafka_consumer_close(consumer);
        rd_kafka_destroy(consumer);
        return 1;
    }

    rd_kafka_topic_t* producer_topic = rd_kafka_topic_new(producer, TOPIC_OUT, NULL);
    if (!producer_topic) {
        std::cerr << "[Output] Failed to create producer topic handle for " << TOPIC_OUT << std::endl;
        rd_kafka_consumer_close(consumer);
        rd_kafka_destroy(consumer);
        rd_kafka_destroy(producer);
        return 1;
    }

    consume_and_predict(consumer, producer, producer_topic, topic_in);

    rd_kafka_topic_destroy(producer_topic);
    rd_kafka_flush(producer, 5000);
    rd_kafka_destroy(producer);

    rd_kafka_consumer_close(consumer);
    rd_kafka_destroy(consumer);

    std::cout << "\n[Output] Done." << std::endl;
    return 0;
}


