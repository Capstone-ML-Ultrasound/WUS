#include <cstdio>       // Required for fprintf, stderr
#include <cstdlib>      // Required for exit
#include <cstring>      // Required for string operations
#include <unistd.h>     // Required for gethostname
#include <vector>
#include <librdkafka/rdkafka.h>
#include "Utils.h"

// Configuration constants
const int MIN_COMMIT_COUNT = 100;
const int FLUSH_EVERY_FRAMES = 50; // write CSV every N frames

int main() {
    char hostname[128];
    char errstr[512];
    int msg_count = 0; 
    Utils utils;
    std::vector<std::vector<unsigned char>> frameBuffer;

    // create configuration object
    rd_kafka_conf_t *conf = rd_kafka_conf_new();

    if (gethostname(hostname, sizeof(hostname)) != 0) {
        fprintf(stderr, "%% Failed to lookup hostname, defaulting to localhost\n");
        std::strncpy(hostname, "localhost", sizeof(hostname) - 1);
        hostname[sizeof(hostname) - 1] = '\0';
    }

    // 2. Set configuration properties
    if (rd_kafka_conf_set(conf, "client.id", hostname,
                          errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        fprintf(stderr, "%% %s\n", errstr);
        exit(1);
    }

    if (rd_kafka_conf_set(conf, "group.id", "foo",
                          errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        fprintf(stderr, "%% %s\n", errstr);
        exit(1);
    }

    if (rd_kafka_conf_set(conf, "bootstrap.servers", "localhost:9092",
                          errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        fprintf(stderr, "%% %s\n", errstr);
        exit(1);
    }

    // Allow consumer to auto-create topic if it doesn't exist yet (relies on broker auto.create.topics.enable=true)
    if (rd_kafka_conf_set(conf, "allow.auto.create.topics", "true",
                          errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        fprintf(stderr, "%% %s\n", errstr);
        exit(1);
    }

    // create Kafka consumer handle
    rd_kafka_t *rk;
    if (!(rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf,
                            errstr, sizeof(errstr)))) {
        fprintf(stderr, "%% Failed to create new consumer: %s\n", errstr);
        exit(1);
    }

    // subscribe to topics
    rd_kafka_topic_partition_list_t *subscription = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_list_add(subscription, "ultrasound_raw_data", RD_KAFKA_PARTITION_UA);

    rd_kafka_resp_err_t err = rd_kafka_subscribe(rk, subscription);
    if (err) {
        fprintf(stderr, "%% Failed to subscribe to %d topics: %s\n",
                subscription->cnt, rd_kafka_err2str(err));
        rd_kafka_topic_partition_list_destroy(subscription);
        rd_kafka_destroy(rk);
        exit(1);
    }
    
    // destroy the list object now, the subscription is stored in 'rk'
    rd_kafka_topic_partition_list_destroy(subscription);

    bool running = true;

    // poll Loop 
    while (running) {
        // Poll for messages (timeout 1000ms)
        rd_kafka_message_t *rkmessage = rd_kafka_consumer_poll(rk, 1000);
        
        // Timeout: no message
        if (!rkmessage)
            continue; 

        if (rkmessage->err) {
            fprintf(stderr, "%% Message error: %s\n", rd_kafka_message_errstr(rkmessage));
            rd_kafka_message_destroy(rkmessage);
            continue;
        }

        // Capture payload into frame buffer
        const unsigned char* payload = reinterpret_cast<const unsigned char*>(rkmessage->payload);
        std::vector<unsigned char> frame(payload, payload + rkmessage->len);
        frameBuffer.push_back(std::move(frame));

        // Periodically flush frames to CSV
        if (frameBuffer.size() >= FLUSH_EVERY_FRAMES) {
            if (utils.writeBurstCSV(frameBuffer)) {
                fprintf(stdout, "%% Wrote %zu frames to CSV\n", frameBuffer.size());
                frameBuffer.clear();
            } else {
                fprintf(stderr, "%% Failed to write CSV\n");
            }
        }

        rd_kafka_message_destroy(rkmessage);

        // Commit offsets periodically
        if ((++msg_count % MIN_COMMIT_COUNT) == 0) {
            rd_kafka_resp_err_t commit_err = rd_kafka_commit(rk, NULL, 0); // Sync commit
            if (commit_err) {
                fprintf(stderr, "%% Commit failed: %s\n", rd_kafka_err2str(commit_err));
                // Application-specific rollback logic would go here
            }
        }
    }

    // cleanup
    rd_kafka_consumer_close(rk);
    rd_kafka_destroy(rk);
    
    return 0;
}
