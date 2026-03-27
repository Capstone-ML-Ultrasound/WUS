#include "USBuilder.h"
#include "Utils.h"
#include "USFrameProtocol.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <unistd.h>
#include <signal.h> // For Ctrl+C handling
#include <vector>
#include <getopt.h> // Added for argument parsing

#include <librdkafka/rdkafka.h>

// Global flag for clean shutdown
volatile sig_atomic_t running = 1;

void signalHandler(int signum) {
  std::cout << "\n\nShutdown signal received..." << std::endl;
  running = 0;
}

bool isValidDepth(int depth) {
  return depth > 0 && depth <= 4090;
}

bool isValidFreq(int freq) {
  return freq == 160 || freq == 80 || freq == 40 || freq == 20;
}

bool isValidCompression(int compression) {
  return compression >= 0 && compression <= 3;
}

bool isValidFilter(double filter_mhz) {
  return filter_mhz == -1.0 || filter_mhz == 1.25 || filter_mhz == 2.5 ||
         filter_mhz == 5.0 || filter_mhz == 10.0;
}

bool computeExpectedBytes(int depth, int compression, int& expectedBytes) {
  if (!isValidCompression(compression)) {
    std::cerr << "Invalid compression: " << compression << " (must be 0, 1, 2, or 3)" << std::endl;
    return false;
  }

  const int divisor = compression + 1;
  if (divisor <= 0) {
    std::cerr << "Invalid compression divisor: " << divisor << std::endl;
    return false;
  }

  if (depth % divisor != 0) {
    std::cerr << "Invalid depth/compression combo: depth=" << depth
              << " is not divisible by (compression+1)=" << divisor << std::endl;
    return false;
  }

  expectedBytes = depth / divisor;
  if (expectedBytes <= 0 || expectedBytes > 4096) {
    std::cerr << "Computed expected bytes out of range: " << expectedBytes << std::endl;
    return false;
  }

  return true;
}

int getNonNegativeEnvInt(const char* name, int fallback) {
  const char* raw = std::getenv(name);
  if (!raw || std::string(raw).empty()) return fallback;
  try {
    int value = std::stoi(raw);
    return value < 0 ? fallback : value;
  } catch (...) {
    return fallback;
  }
}

std::string getEnvOrDefault(const char* name, const std::string& fallback = "") {
  const char* raw = std::getenv(name);
  if (!raw) return fallback;
  const std::string value(raw);
  return value.empty() ? fallback : value;
}

#if defined(__APPLE__)
std::string detectMacSerialPort() {
  namespace fs = std::filesystem;
  const fs::path devPath("/dev");
  const std::vector<std::string> prefixes = {
      "tty.usbmodem",
      "cu.usbmodem",
      "tty.usbserial",
      "cu.usbserial",
  };

  std::error_code ec;
  if (!fs::exists(devPath, ec)) {
    return "";
  }

  std::vector<std::string> matches;
  for (const auto& entry : fs::directory_iterator(devPath, ec)) {
    if (ec || !entry.is_character_file(ec)) continue;
    const std::string name = entry.path().filename().string();
    for (const auto& prefix : prefixes) {
      if (name.rfind(prefix, 0) == 0) {
        matches.push_back(entry.path().string());
        break;
      }
    }
  }

  if (matches.empty()) return "";
  std::sort(matches.begin(), matches.end());
  return matches.front();
}
#endif

std::string getDefaultPort() {
  const std::string envPort = getEnvOrDefault("US_SERIAL_PORT");
  if (!envPort.empty()) {
    return envPort;
  }

#ifdef _WIN32
  return "\\\\.\\COM4";
#elif __APPLE__
  const std::string detected = detectMacSerialPort();
  if (!detected.empty()) {
    return detected;
  }
  return "/dev/tty.usbmodem11101";
#else
  if (std::filesystem::exists("/dev/ttyUSB0")) return "/dev/ttyUSB0";
  if (std::filesystem::exists("/dev/ttyACM0")) return "/dev/ttyACM0";
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

bool stream_continuous(USBuilder &dev, int depth, int freq, double filter_mhz, int compression, rd_kafka_t *rk, rd_kafka_topic_t *rkt) {

  // 1. Setup Hardware

  int expectedBytes = 0;
  if (!computeExpectedBytes(depth, compression, expectedBytes)) {
    return false;
  }

  // ONLY Can change filter IFF freq=80
  if (freq == 80) {
      if (!dev.func14_setFilter(filter_mhz)){return false;}
  }

  if (!dev.func24_setSamplingFreq(freq)){return false;}

  if (freq==80 && filter_mhz == -1) {
      if (!dev.func3_setCompression(compression)){return false;}
  }

  if (!dev.func4_setAutoSample(expectedBytes)) {return false;}    ///****************** dpeth


  // 2. Setup for streaming
  int frameCount = 0;
  uint64_t sequence = 0; // Monotonic sequence counter
  uint32_t deviceId = 1; // Assuming device 1 for now
  const int logEvery = std::max(1, getNonNegativeEnvInt("STREAM_LOG_EVERY", 100));
  auto overallStart = std::chrono::high_resolution_clock::now();
  std::vector<unsigned char> samples(expectedBytes);
  // std::vector<unsigned char> samples;
  // samples.reserve(depth);
  //int expectedBytes = depth / (compression + 1); //for the compression thing

  // 3. Stream
  while (running) {
    // Acquire data from hardware
    if (!dev.requestAscan8bit(expectedBytes, samples)) {
        std::cerr << "Frame " << frameCount << " failed!" << std::endl;
        continue;
    }
    frameCount++;

    // Encode data with our binary envelope
    std::vector<uint8_t> encodedMessage = USProtocol::encodeEnvelope(sequence++, deviceId, samples);

    // Send to Kafka
    if (rd_kafka_produce(
            rkt,                          // topic handle
            RD_KAFKA_PARTITION_UA,        // choose partition (auto)
            RD_KAFKA_MSG_F_COPY,          // copies payload (encodedMessage is local stack)
            (void*)encodedMessage.data(), // pointer to our ENCODED envelope
            encodedMessage.size(),        // total size (Header + Payload)
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

    // 4. Calculate and display FPS
    if (frameCount % logEvery == 0) {
        auto now = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = now - overallStart;
        double fps = frameCount / elapsed.count();
        std::cout << "Frame " << frameCount << " | FPS: " << std::fixed << fps << std::endl;
    }
  }

  // 5. Streaming stats
  auto overallEnd = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> totalTime = overallEnd - overallStart;

  std::cout << "\nTotal frames: " << frameCount << std::endl;
  std::cout << "Average FPS: " << (frameCount / totalTime.count()) << std::endl;

  // 6. Flush pending Kafka messages
  rd_kafka_flush(rk, 10 * 1000);

  return true;
}


void burst(USBuilder &dev, int depth, int freq, double filter_mhz, int compression, int numFrames, rd_kafka_t *rk, rd_kafka_topic_t *rkt) {

  std::cout << "\n--- Acquiring burst data ---" << std::endl;
  std::vector<std::vector<unsigned char>> burstData;

  // 1. Setup Hardware
  int expectedBytes = 0;
  if (!computeExpectedBytes(depth, compression, expectedBytes)) {
    return;
  }

  // ONLY Can change filter IFF freq=80
  if (freq == 80) {
    if (!dev.func14_setFilter(filter_mhz)){return;}
  }
  if (!dev.func24_setSamplingFreq(freq)){return;}
  if (freq==80 && filter_mhz == -1) {
    if (!dev.func3_setCompression(compression)){return;}
  }
  if (!dev.func4_setAutoSample(expectedBytes)) {return;}    ///****************** dpeth


  auto start = std::chrono::high_resolution_clock::now();
  std::cout << "\n--- Acquiring burst "<< numFrames << " Samples ---" << std::endl;


  if (dev.requestAscan8bitBurst(expectedBytes, numFrames, burstData)) {
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration_ms = end - start;

    std::cout << "Burst acquisition complete" << std::endl;
    std::cout << "   Frames: " << burstData.size() << std::endl;
    std::cout << "   Samples per frame: " << burstData[0].size() << std::endl;
    std::cout << "   Duration: " << duration_ms.count() << " ms" << std::endl;
    std::cout << "   Frame rate: "
              << (burstData.size() * 1000.0 / duration_ms.count()) << " fps"
              << std::endl;

    // TODO -- PASS TO KAFKA AND SAVE

  } else {
    std::cerr << "Burst acquisition failed" << std::endl;
    dev.disconnect();
  }
}

int main(int argc, char* argv[]) {
  std::cout << "US-Builder Data Acquisition (Kafka Streamer)" << std::endl;

  // 1. Default Values
  int depth = 4090;
  int freq = 80;
  double filter_mhz = -1;  // no filter
  int compression = 1;    // no compression
  std::string portName = getDefaultPort();

  // 2. Parse Command Line Arguments
  static struct option long_options[] = {
      {"depth", required_argument, 0, 'd'},
      {"freq",  required_argument, 0, 'f'},
      {"help",  no_argument, 0, 'h'},
      {"filter", required_argument, 0, 'm'},
      {"compression", required_argument, 0, 'c'},
      {"port", required_argument, 0, 'p'},
      {0, 0, 0, 0}
  };

  int opt;
  int option_index = 0;
  while ((opt = getopt_long(argc, argv, "d:f:m:c:p:h", long_options, &option_index)) != -1) {
      switch (opt) {
          case 'd':
              depth = std::stoi(optarg);
              break;
          case 'f':
              freq = std::stoi(optarg);
              break;
          case 'm':
              filter_mhz = std::stod(optarg); // using stod for doubles (e.g. 1.25)
              break;
          case 'c':
              compression = std::stoi(optarg);
              break;
          case 'p':
              portName = optarg;
              break;
          case 'h':
              std::cout << "Usage: " << argv[0] << " [--depth <n>] [--freq <n>]\n"
                        << "  --depth, -d        Number of samples to capture (max 4090)\n"
                        << "  --freq,  -f        Sampling frequency in MHz (160, 80, 40, 20)\n"
                        << "  --filter, -m       Filter (1.25, 2.5, 5, 10, -1)\n"
                        << "  --compression, -c  Compression factor (0, 1, 2, 3)\n"
                        << "  --port, -p         Serial port (or set US_SERIAL_PORT)\n"
                        << "  --help,  -h        Display this help message" << std::endl;
              return 0;
          default:
              return 1;
      }
  }

  if (!isValidDepth(depth)) {
    std::cerr << "Invalid depth: " << depth << " (must be 1-4090)" << std::endl;
    return 1;
  }
  if (!isValidFreq(freq)) {
    std::cerr << "Invalid frequency: " << freq << " (must be 160, 80, 40, or 20 MHz)" << std::endl;
    return 1;
  }
  if (!isValidFilter(filter_mhz)) {
    std::cerr << "Invalid filter: " << filter_mhz << " (must be -1, 1.25, 2.5, 5, or 10 MHz)" << std::endl;
    return 1;
  }
  if (!isValidCompression(compression)) {
    std::cerr << "Invalid compression: " << compression << " (must be 0, 1, 2, or 3)" << std::endl;
    return 1;
  }
  int expectedBytes = 0;
  if (!computeExpectedBytes(depth, compression, expectedBytes)) {
    return 1;
  }

  // Set up Ctrl+C handler
  signal(SIGINT, signalHandler);

  // Platform-specific port selection (or CLI/env override)
  std::cout << "Using port: " << portName << std::endl;
  std::cout << "Configuration: Depth=" << depth
            << ", Freq=" << freq
            << ", Filter=" << filter_mhz
            << ", Comp=" << compression
            << ", ExpectedBytes=" << expectedBytes << "\n" << std::endl;

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
  std::string hostnameStr = "localhost";

  // create Kafka configuration object (global level configuration)
  rd_kafka_conf_t* conf = rd_kafka_conf_new();

  // Identifies machine hostname
  if (gethostname(hostname, sizeof(hostname))) {
    fprintf(stderr, "%% Failed to lookup hostname, defaulting to localhost\n");
  } else {
    hostnameStr = hostname;
  }

  // sets hostname retrieved above as client.id
  if (rd_kafka_conf_set(conf, "client.id", hostnameStr.c_str(),
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

  if (!stream_continuous(dev, depth, freq, filter_mhz, compression, rk, rkt)) {
    std::cerr << "Streaming setup failed." << std::endl;
    dev.disconnect();
    rd_kafka_flush(rk, 5000);
    rd_kafka_topic_destroy(rkt);
    rd_kafka_destroy(rk);
    return 1;
  }

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
