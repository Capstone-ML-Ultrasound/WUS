#include "Utils.h"

#include <sstream>       // stringstream
#include <iostream>      // cout, cerr
#include <fstream>       // ofstream
#include <chrono>        // system_clock
#include <ctime>         // localtime
#include <iomanip>       // put_time
#include <filesystem>    // create/check directories
Utils::Utils() {}

Utils::~Utils() {}

bool Utils::writeCSV(std::vector<unsigned char>& samples){
    // Obtain and format the current timestamp
    auto now = std::chrono::system_clock::now();
    std::time_t currentTime = std::chrono::system_clock::to_time_t(now);
    std::tm* localTime = std::localtime(&currentTime);

    std::stringstream ss;
    ss << std::put_time(localTime, "%Y-%m-%d_%H-%M-%S");
    std::string timestamp = ss.str();

    // Create or ensure data directory exists
    std::filesystem::path cwd = std::filesystem::current_path();
    std::filesystem::path data_dir = cwd / "data";

    // Create the folder if it doesn't exist
    if (!std::filesystem::exists(data_dir)) {
        try {
            std::filesystem::create_directory(data_dir);
            std::cout << "Created directory: " << data_dir << std::endl;
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Error creating directory: " << e.what() << std::endl;
            return false;
        }
    }

    // Build full file path
    std::string csv_name = "sample_" + timestamp + ".csv";
    std::filesystem::path csv_location = data_dir / csv_name;

    std::cout << "FILE NAME: " << csv_name << " Located: " << csv_location << std::endl;

    // Open file for writing
    std::ofstream outputFile(csv_location, std::ios_base::app);
    if (!outputFile.is_open()) {
        std::cerr << "Error opening file: " << csv_location << std::endl;
        perror("Reason");
        return false;
    }

    // Write data
    for (unsigned char s : samples) {
        outputFile << (int)s << std::endl;
    }

    outputFile.close();
    std::cout << "Data saved to " << csv_location << std::endl;
    return true;
}

/**
 * Save burst data (multiple frames) into a CSV file
 * Format: columns = frames, rows = samples
 */
bool Utils::writeBurstCSV(const std::vector<std::vector<unsigned char>>& burstData) {
    if (burstData.empty()) {
        std::cerr << "writeBurstCSV: burstData is empty.\n";
        return false;
    }

    // Determine max number of samples across frames
    size_t numFrames = burstData.size();
    size_t maxSamples = 0;
    for (const auto& f : burstData) {
        maxSamples = std::max(maxSamples, f.size());
    }
    if (maxSamples == 0) {
        std::cerr << "writeBurstCSV: all frames are empty.\n";
        return false;
    }

    // Timestamped filename under ./data/
    auto now = std::chrono::system_clock::now();
    std::time_t currentTime = std::chrono::system_clock::to_time_t(now);
    std::tm* localTime = std::localtime(&currentTime);

    std::stringstream ss;
    ss << std::put_time(localTime, "%Y-%m-%d_%H-%M-%S");
    std::string timestamp = ss.str();

    std::filesystem::path data_dir = std::filesystem::current_path() / "data";
    if (!std::filesystem::exists(data_dir)) {
        try {
            std::filesystem::create_directory(data_dir);
            std::cout << "Created directory: " << data_dir << '\n';
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Error creating directory: " << e.what() << '\n';
            return false;
        }
    }

    std::string csv_name = "burst_" + timestamp + ".csv";
    std::filesystem::path csv_location = data_dir / csv_name;
    std::cout << "FILE NAME: " << csv_name << " Located: " << csv_location << '\n';

    // Open NEW file (truncate) and write
    std::ofstream out(csv_location, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "Error opening file: " << csv_location << '\n';
        perror("Reason");
        return false;
    }

    // Header: frame_0,frame_1,...,frame_{N-1}
    for (size_t c = 0; c < numFrames; ++c) {
        out << "frame_" << c;
        if (c + 1 < numFrames) out << ',';
    }
    out << '\n';

    // Rows: sample index 0..maxSamples-1
    for (size_t r = 0; r < maxSamples; ++r) {
        for (size_t c = 0; c < numFrames; ++c) {
            int val = (r < burstData[c].size()) ? static_cast<int>(burstData[c][r]) : 0; // pad if ragged
            out << val;
            if (c + 1 < numFrames) out << ',';
        }
        out << '\n';
    }

    out.close();
    std::cout << "Data saved to " << csv_location << '\n';
    return true;
}
