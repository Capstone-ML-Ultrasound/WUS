#include "USBuilder.h"
#include "Utils.h"

#include <chrono>
#include <iostream>
#include <signal.h> // For Ctrl+C handling
#include <vector>
#include <thread>

#include <librdkafka/rdkafka.h>


// Global flag for clean shutdown
volatile sig_atomic_t running = 1;

void signalHandler(int signum) {
  std::cout << "\n\nShutdown signal received..." << std::endl;
  running = 0;
}

std::string getDefaultPort() {
#ifdef _WIN32
  return "\\\\.\\COM4";
#elif __APPLE__
  return "/dev/tty.usbmodem1101"; // (TO CHECK) ls /dev | grep tty.usb
#else
  return "/dev/ttyUSB0";
#endif
}

void test_firmware(USBuilder &dev) {
  // Step 1: Get firmware version
  std::string version;
  if (dev.requestFirmware(version)) {
    std::cout << "Firmware version: " << version << std::endl;
  } else {
    std::cerr << "Firmware request failed" << std::endl;
  }
}

std::vector<unsigned char> acquire_single_Ascan(USBuilder &dev) {
  // Step 2: Acquire single A-scan (optional test)
  std::cout << "\n--- Single A-scan ---" << std::endl;
  std::vector<unsigned char> samples;
  if (!dev.requestAscan8bit(512, samples)) {
    std::cerr << "Single A-scan failed" << std::endl;
    running = false;
  }
  return samples;
}

/**
 *  IDK WHY THIS IS THE SAME
 *
 *  - this is what is expected (set func4 -> manuual trigger -> read)
 *  - but this gives the same FPS as, where we jsut send a read command?????
 *
 *  is by default Func4 active? does it work?
 */
void func4_set_burst(USBuilder &dev, Utils &utils) {

  std::cout << "\n--- Acquiring burst data ---" << std::endl;
  std::vector<std::vector<unsigned char>> burstData;

  // 1. Prog to Automatic Sampling request
  std::cout << "[Start] -- Programming func 4" << std::endl;
  dev.programSPIFunc4(4000);
  std::cout << "[Done] -- Programming func 4" << std::endl;

  // 2. Trigger FIRST acquisition manually (Function 2)
  std::cout << "[Start] -- Trigger FIRST acquisition manually -- Func 2"
            << std::endl;
  dev.programSPIFunc2();
  std::cout << "[Done] -- Trigger FIRST acquisition manually -- Func 2"
            << std::endl;

  auto start = std::chrono::high_resolution_clock::now();

  std::cout << "\n--- Acquiring burst 1000 Samples ---" << std::endl;
  if (dev.requestAscan8bitBurst(4000, 1000, burstData)) {
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration_ms = end - start;

    std::cout << "Burst acquisition complete" << std::endl;
    std::cout << "   Frames: " << burstData.size() << std::endl;
    std::cout << "   Samples per frame: " << burstData[0].size() << std::endl;
    std::cout << "   Duration: " << duration_ms.count() << " ms" << std::endl;
    std::cout << "   Frame rate: "
              << (burstData.size() * 1000.0 / duration_ms.count()) << " fps"
              << std::endl;

    // Save to CSV
    utils.writeBurstCSV(burstData);

  } else {
    std::cerr << "Burst acquisition failed" << std::endl;
    dev.disconnect();
  }
}

void stream_continuous(USBuilder &dev, int numSamples) {

  int frameCount = 0;
  auto overallStart = std::chrono::high_resolution_clock::now();

  std::vector<unsigned char> samples;
  samples.reserve(numSamples); // Pre-allocate to avoid reallocation

  while (running) {
    auto frameStart = std::chrono::high_resolution_clock::now();

    // Acquire single frame
    if (!dev.requestAscan8bit(numSamples, samples)) {
      std::cerr << "Frame " << frameCount << " failed!" << std::endl;
      continue; // Try again instead of exiting
    }

    frameCount++;

    // Display statistics every 10 frames
    if (frameCount % 10 == 0) {
      auto now = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> elapsed = now - overallStart;
      double fps = frameCount / elapsed.count();

      // Calculate some basic stats from the frame
      unsigned char minVal = 255, maxVal = 0;
      long sum = 0;
      for (unsigned char val : samples) {
        sum += val;
        if (val < minVal)
          minVal = val;
        if (val > maxVal)
          maxVal = val;
      }
      double avgVal = sum / (double)samples.size();

      std::cout << "Frame " << frameCount << " | FPS: " << std::fixed << fps
                << " | Min: " << (int)minVal << " | Max: " << (int)maxVal
                << " | Avg: " << std::fixed << avgVal
                << " | First sample: " << (int)samples[0] << std::endl;
    }
  }

  auto overallEnd = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> totalTime = overallEnd - overallStart;

  std::cout << "\n========================================" << std::endl;
  std::cout << "STREAMING STOPPED" << std::endl;
  std::cout << "Total frames: " << frameCount << std::endl;
  std::cout << "Total time: " << std::fixed << totalTime.count() << " seconds"
            << std::endl;
  std::cout << "Average FPS: " << std::fixed << (frameCount / totalTime.count())
            << std::endl;
  std::cout << "========================================\n" << std::endl;
}

void stream_with_func4(USBuilder &dev, int numSamples, rd_kafka_t *rk, rd_kafka_topic_t *rkt) {
  std::cout << "\n========================================" << std::endl;
  std::cout << "STREAMING MODE (Function 4 Auto-Sampling)" << std::endl;
  std::cout << "Press Ctrl+C to stop" << std::endl;
  std::cout << "========================================\n" << std::endl;

  // Enable auto-sampling
  if (!dev.programSPIFunc4(numSamples)) {
    std::cerr << "Failed to enable auto-sampling" << std::endl;
    return;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Trigger first acquisition
  if (!dev.programSPIFunc2()) {
    std::cerr << "Failed to trigger first acquisition" << std::endl;
    return;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  int frameCount = 0;
  auto overallStart = std::chrono::high_resolution_clock::now();

  std::vector<unsigned char> samples(numSamples);

  while (running) {
    // Just read - hardware auto-triggers!
    if (!dev.requestAscan8bit(numSamples, samples)) {
      std::cerr << "Frame " << frameCount << " failed!" << std::endl;
      continue;
    }

    frameCount++;

    // Send data to Kafka
    if (rd_kafka_produce(
            rkt,                          // topic handle
            RD_KAFKA_PARTITION_UA,        // choose partition (auto)
            RD_KAFKA_MSG_F_COPY,          // librdkafka copies payload internally
            (void*)samples.data(),        // pointer to your ultrasound frame
            samples.size(),               // number of bytes in the frame
            nullptr,                      // optional key (we don't use)
            0,                            // key length
            nullptr) == -1)
    {
        std::cerr << "Kafka produce failed: "
                  << rd_kafka_err2str(rd_kafka_last_error())
                  << std::endl;
    }

    // Poll to handle delivery reports
    rd_kafka_poll(rk, 0);

    if (frameCount % 10 == 0) {
      auto now = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> elapsed = now - overallStart;
      double fps = frameCount / elapsed.count();

      unsigned char maxVal = 0;
      for (unsigned char val : samples) {
        if (val > maxVal)
          maxVal = val;
      }

      std::cout << "Frame " << frameCount << " | FPS: " << std::fixed << fps
                << " | Peak: " << (int)maxVal << std::endl;
    }
  }

  auto overallEnd = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> totalTime = overallEnd - overallStart;

  std::cout << "\nTotal frames: " << frameCount << std::endl;
  std::cout << "Average FPS: " << (frameCount / totalTime.count()) << std::endl;
  
  // Flush pending Kafka messages
  rd_kafka_flush(rk, 10 * 1000);

}

int main() {
  std::cout << "========================================" << std::endl;
  std::cout << "US-Builder Data Acquisition" << std::endl;
  std::cout << "========================================\n" << std::endl;

  // Set up Ctrl+C handler
  signal(SIGINT, signalHandler);

  // Platform-specific port selection
  std::string portName = getDefaultPort();
  std::cout << "Using port: " << portName << "\n" << std::endl;

  // Instantiations
  USBuilder dev(portName);
  Utils utils;

  // Connect to device
  if (!dev.connect()) {
    std::cerr << "Failed to connect to device" << std::endl;
    return 1;
  }

  // Create Kafka configuration
  char hostname[128];
  char errstr[512];

  // create Kafka configuration object (global level configuration)
  rd_kafka_conf_t* conf = rd_kafka_conf_new();

  // Identifies machine hostname 
  if (gethostname(hostname, sizeof(hostname))) {
    fprintf(stderr, "%% Failed to lookup hostname\n");
    exit(1);
  }

  // sets hostname retrieved above as client.id
  if (rd_kafka_conf_set(conf, "client.id", hostname,
                      errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
    fprintf(stderr, "%% %s\n", errstr);
    exit(1);
  }

  // sets bootstrap servers
  if (rd_kafka_conf_set(conf, "bootstrap.servers", "localhost:9092",
                      errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
    fprintf(stderr, "%% %s\n", errstr);
    exit(1);
  }

  // create topic configuration object (topic level configuration)
  rd_kafka_topic_conf_t *topic_conf = rd_kafka_topic_conf_new();

  // sets acknowledgements for produced messages (acks)
  if (rd_kafka_topic_conf_set(topic_conf, "acks", "all",
                      errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
    fprintf(stderr, "%% %s\n", errstr);
    exit(1);
  }

  // Create Kafka producer handle 
  rd_kafka_t *rk;
  if (!(rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf,
                          errstr, sizeof(errstr)))) {
    fprintf(stderr, "%% Failed to create new producer: %s\n", errstr);
    exit(1);
  }

  // create topic handle 
  const char* topic_name = "ultrasound_raw_data";
  rd_kafka_topic_t *rkt = rd_kafka_topic_new(rk, topic_name, topic_conf);
  if (!rkt) {
    fprintf(stderr, "%% Failed to create topic handle: %s\n",
            rd_kafka_err2str(rd_kafka_last_error()));
    exit(1);
  }
  
  // stream_continuous(dev, 512);
  stream_with_func4(dev, 512, rk, rkt);

  // Disconnect before exiting
  dev.disconnect();

  // cleanup Kafka resources
  rd_kafka_flush(rk, 5000);
  rd_kafka_topic_destroy(rkt);
  rd_kafka_destroy(rk);

  std::cout << "\n========================================" << std::endl;
  std::cout << " Program completed successfully" << std::endl;
  std::cout << "========================================" << std::endl;

  return 0;
}