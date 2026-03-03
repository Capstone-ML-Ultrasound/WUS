#include <iostream>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <librdkafka/rdkafka.h>

const char* TOPIC_IN = "model_predictions";

volatile sig_atomic_t running = 1;

void signalHandler(int signum) {
    std::cout << "\n[PredictionConsumer] Shutdown signal received..." << std::endl;
    running = 0;
}

rd_kafka_t* create_consumer(const std::string& brokers, const std::string& group_id) {
    char errstr[512];
    rd_kafka_conf_t* conf = rd_kafka_conf_new();

    if (rd_kafka_conf_set(conf, "bootstrap.servers", brokers.c_str(), errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        std::cerr << "[PredictionConsumer] Config error: " << errstr << std::endl;
        return nullptr;
    }
    if (rd_kafka_conf_set(conf, "group.id", group_id.c_str(), errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        std::cerr << "[PredictionConsumer] Config error: " << errstr << std::endl;
        return nullptr;
    }
    if (rd_kafka_conf_set(conf, "auto.offset.reset", "latest", errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        std::cerr << "[PredictionConsumer] Config error: " << errstr << std::endl;
        return nullptr;
    }

    rd_kafka_t* rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
    if (!rk) {
        std::cerr << "[PredictionConsumer] Failed to create consumer: " << errstr << std::endl;
        rd_kafka_conf_destroy(conf);
        return nullptr;
    }
    return rk;
}

void consume_predictions(rd_kafka_t* consumer) {
    rd_kafka_topic_partition_list_t* subscription = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_list_add(subscription, TOPIC_IN, RD_KAFKA_PARTITION_UA);
    rd_kafka_subscribe(consumer, subscription);
    rd_kafka_topic_partition_list_destroy(subscription);

    std::cout << "[PredictionConsumer] Subscribed to " << TOPIC_IN << std::endl;

    int seen = 0;
    while (running) {
        rd_kafka_message_t* rkmessage = rd_kafka_consumer_poll(consumer, 500);

        if (!rkmessage) continue;
        if (rkmessage->err) {
            if (rkmessage->err != RD_KAFKA_RESP_ERR__PARTITION_EOF) {
                std::cerr << "[PredictionConsumer] Error: " << rd_kafka_message_errstr(rkmessage) << std::endl;
            }
            rd_kafka_message_destroy(rkmessage);
            continue;
        }

        std::string payload(static_cast<const char*>(rkmessage->payload), rkmessage->len);

        // Placeholder integration point:
        // Parse payload and drive Unity hand pose/animation parameters here.
        if (++seen % 25 == 0) {
            std::cout << "[PredictionConsumer] Received " << seen << " predictions. Last payload=" << payload << std::endl;
        }

        rd_kafka_message_destroy(rkmessage);
    }
}

int main() {
    signal(SIGINT, signalHandler);

    std::string brokers = "localhost:9092";
    if (const char* env = std::getenv("BOOTSTRAP_SERVERS")) brokers = env;

    rd_kafka_t* consumer = create_consumer(brokers, "model_prediction_consumer_group");
    if (!consumer) return 1;

    consume_predictions(consumer);

    rd_kafka_consumer_close(consumer);
    rd_kafka_destroy(consumer);

    std::cout << "\n[PredictionConsumer] Done." << std::endl;
    return 0;
}


