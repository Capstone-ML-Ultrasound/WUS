#include <cstdio>       // Required for fprintf, stderr
#include <cstdlib>      // Required for exit
#include <cstring>      // Required for string operations
#ifdef _WIN32
  #include <winsock2.h>
  #include <winsock2.h>
#else
  #include <unistd.h>     // Required for gethostname
#endif

#include <csignal>
#include <vector>
#include <librdkafka/rdkafka.h>
#include <iostream>
#include "Utils.h"
#include "USFrameProtocol.h"

// Configuration constants
const int MIN_COMMIT_COUNT = 100;
const int FLUSH_EVERY_FRAMES = 50; // write CSV every N frames

volatile sig_atomic_t running = 1;

void signalHandler(int signum) {
    fprintf(stdout, "\n%% Shutdown signal received...\n");
    running = 0;
}

int main() {
    signal(SIGINT, signalHandler);

    char hostname[128];
    char errstr[512];
    int msg_count = 0; 
    Utils utils;
    std::vector<std::vector<unsigned char>> frameBuffer;

    // Windows requires Winsock initialization for network functions
    #ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "%% Failed to initialize Winsock\n");
        exit(1);
    }
    #endif

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

    const char* bootstrap_servers = std::getenv("BOOTSTRAP_SERVERS");
    if (!bootstrap_servers) {
        bootstrap_servers = "localhost:9092";
    }

    if (rd_kafka_conf_set(conf, "bootstrap.servers", bootstrap_servers,
                          errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        fprintf(stderr, "%% %s\n", errstr);
        exit(1);
    }

    // Set auto offset reset to latest (default) for real-time preference
    if (rd_kafka_conf_set(conf, "auto.offset.reset", "latest",
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

    // poll Loop 
    while (running) {
        // Poll for messages (timeout 1000ms)
        rd_kafka_message_t *rkmessage = rd_kafka_consumer_poll(rk, 1000);
        
        // Timeout: no message
        if (!rkmessage) {
            static int timeout_print_count = 0;
            if (++timeout_print_count % 5 == 0) { // Print every 5 timeouts (5 seconds)
                 fprintf(stdout, "%% Waiting for messages... (ensure producer is running)\r");
                 fflush(stdout);
            }
            continue; 
        }

        if (rkmessage->err) {
            fprintf(stderr, "%% Message error: %s\n", rd_kafka_message_errstr(rkmessage));
            rd_kafka_message_destroy(rkmessage);
            continue;
        }



        // Capture payload into frame buffer using new protocol
        USProtocol::DecodedFrame decoded;
        if (USProtocol::decodeEnvelope(rkmessage->payload, rkmessage->len, decoded)) {
            // Valid Frame
            std::vector<unsigned char> frame(decoded.payload, decoded.payload + decoded.payloadSize);
            frameBuffer.push_back(std::move(frame));
            
            // Periodically log metadata
            if (decoded.header->sequence_number % 100 == 0) {
                 std::cout << "Received Frame #" << decoded.header->sequence_number 
                           << " | Size: " << decoded.payloadSize 
                           << " | TS: " << decoded.header->timestamp_ns << std::endl;
            }

        } else {
            // Invalid frame (Legacy fallback or Corruption)
            // For now, let's treat it as legacy raw data if it doesn't match magic
            // OR log error and skip. Let's log error for safety.
            fprintf(stderr, "%% Invalid frame format: magic mismatch or size error\n");
            rd_kafka_message_destroy(rkmessage);
            continue;
        }

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
    
    #ifdef _WIN32
    WSACleanup();
    #endif

    return 0;
}
