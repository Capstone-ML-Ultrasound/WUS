#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <thread>
#include <chrono>
#include <cmath>
#include <opencv2/opencv.hpp>

// ─── NEW 1×3 Vector format ────────────────────────────────────────────────────
// vec[0]  hand_state : 0 = open/hovering,  1 = closed/pen-down
// vec[1]  flexion    : degrees, –80 (hard left) … 0 (neutral) … +50 (hard right)
// vec[2]  up_down    : reserved / zero for now
struct ModelPrediction {
    float handState;   // 0 or 1
    float flexion;     // signed degrees of wrist rotation
    float upDown;      // reserved
};

// ─── Timing constants ────────────────────────────────────────────────────────
// Model runs at ~25 FPS  →  40 ms per frame.
// Adjust FPS_TARGET to 12 or 24 if you want to simulate those speeds.
static constexpr int FPS_TARGET  = 25;         // frames per second
static constexpr int FRAME_MS    = 1000 / FPS_TARGET;  // ms between frames

// Movement scaling
// Flexion range: –80 … +50 degrees.
// We normalise into [–1, +1] then multiply by maxSpeed pixels/frame.
static constexpr float FLEXION_MIN  = -80.0f;
static constexpr float FLEXION_MAX  =  50.0f;
static constexpr float MAX_SPEED    = 12.0f;   // pixels per frame at full deflection
static constexpr float DEADZONE_DEG =  3.0f;   // ignore jitter below ±3°

// CSV reader
std::vector<ModelPrediction> loadPredictions(const std::string& filename) {
    std::vector<ModelPrediction> preds;
    std::ifstream file(filename);
    if (!file.is_open()) return preds;

    std::string line, val;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::vector<float> vec;
        while (std::getline(ss, val, ',')) {
            try { vec.push_back(std::stof(val)); }
            catch (...) { vec.clear(); break; }   // skip header / bad rows
        }
        if (vec.size() == 3) {
            preds.push_back({ vec[0], vec[1], vec[2] });
        }
    }
    return preds;
}

// Normalise flexion to a signed speed
// Returns a value in (–MAX_SPEED, +MAX_SPEED).
// Dead-zone applied before normalisation.
float flexionToSpeed(float deg) {
    if (std::abs(deg) < DEADZONE_DEG) return 0.0f;

    // Normalise separately for the left and right halves
    float norm;
    if (deg < 0.0f) {
        norm = deg / (-FLEXION_MIN);   // –1 at hard left
    } else {
        norm = deg / FLEXION_MAX;      // +1 at hard right
    }
    norm = std::max(-1.0f, std::min(norm, 1.0f));
    return norm * MAX_SPEED;           // positive → right, negative → left
}

int main() {
    const std::string csvPath = "../vector_simulation_3col.csv";

    std::vector<ModelPrediction> liveStream = loadPredictions(csvPath);
    if (liveStream.empty()) {
        std::cerr << "ERROR: Could not load '" << csvPath << "' (missing or empty)." << std::endl;
        return -1;
    }
    std::cout << "Loaded " << liveStream.size() << " frames from '" << csvPath << "'\n";
    std::cout << "Playback: " << FPS_TARGET << " FPS  (" << FRAME_MS << " ms/frame)\n";

    // ── Canvas setup ──────────────────────────────────────────────────────────
    int width        = 1000;
    int height       = 700;
    int headerHeight =  90;

    cv::Mat canvas(height, width, CV_8UC3, cv::Scalar(255, 255, 255));
    cv::Point2f pos(width / 2.0f,
                    headerHeight + (height - headerHeight) / 2.0f);

    cv::namedWindow("Ultrasound Etch-a-Sketch", cv::WINDOW_AUTOSIZE);

    // LOW-PASS FILTER MEMORY
    // This variable stores the momentum from the previous frame.
    // We declare it outside the loop so it persists across frames.
    float smoothed_dx = 0.0f;

    //  Frame loop
    for (size_t i = 0; i < liveStream.size(); ++i) {
        const auto& pred = liveStream[i];
        auto frameStart  = std::chrono::steady_clock::now();

        cv::Point2f prevPos = pos;

        // 1. MOVEMENT MATH
        float raw_dx = flexionToSpeed(pred.flexion);

        // --- LOW-PASS FILTER APPLIED HERE ---
        // Alpha controls the "weight".
        // 0.8f means 80% old momentum, 20% new sensor input.
        // Change to 0.5f for faster response, or 0.9f for heavier smoothing.
        float alpha = 0.8f;
        smoothed_dx = (alpha * smoothed_dx) + ((1.0f - alpha) * raw_dx);

        // dy is zero until up_down model is integrated
        float dy = 0.0f;

        pos.x += smoothed_dx; // Use the smoothed velocity
        pos.y += dy;

        // Clamp to canvas bounds
        pos.x = std::max(0.0f, std::min(pos.x, (float)(width  - 1)));
        pos.y = std::max((float)headerHeight,
                         std::min(pos.y, (float)(height - 1)));

        // 2. STATE LOGIC
        bool penDown = (pred.handState > 0.5f);

        if (penDown) {
            cv::line(canvas, prevPos, pos,
                     cv::Scalar(0, 0, 200), 3, cv::LINE_AA);
        }

        // 3. RENDER UI OVERLAY
        cv::Mat display = canvas.clone();

        // Cursor dot
        cv::Scalar cursorColor = penDown
            ? cv::Scalar(0, 0, 255)
            : cv::Scalar(80, 80, 255);
        cv::circle(display, pos, 6, cursorColor, -1);

        // Header bar
        cv::rectangle(display, cv::Rect(0, 0, width, headerHeight),
                      cv::Scalar(30, 30, 30), -1);
        cv::line(display,
                 cv::Point(0, headerHeight),
                 cv::Point(width, headerHeight),
                 cv::Scalar(90, 90, 90), 2);

        // State string
        std::string stateStr = penDown ? "STATE: PEN DOWN" : "STATE: HOVERING";
        cv::Scalar stateColor = penDown
            ? cv::Scalar(80, 80, 255)
            : cv::Scalar(200, 200, 200);
        cv::putText(display, stateStr,
                    cv::Point(20, 38),
                    cv::FONT_HERSHEY_DUPLEX, 0.75, stateColor, 2);

        // Flexion bar (visual gauge)
        // Maps –80…+50 onto the header width
        int barCentre = width / 2;
        float normFlex = (pred.flexion - FLEXION_MIN) /
                         (FLEXION_MAX - FLEXION_MIN);
        normFlex = std::max(0.0f, std::min(normFlex, 1.0f));
        int barX = (int)(normFlex * (width - 40)) + 20;
        cv::line(display,
                 cv::Point(barCentre, headerHeight - 18),
                 cv::Point(barCentre, headerHeight - 6),
                 cv::Scalar(60, 60, 60), 2);
        cv::circle(display, cv::Point(barX, headerHeight - 12),
                   6,
                   pred.flexion < 0 ? cv::Scalar(255, 120, 0)   // left → orange
                                    : cv::Scalar(0,   220, 100), // right → green
                   -1);

        // Numeric debug
        char info[160];
        snprintf(info, sizeof(info),
                 "VEC[%.1f | flex=%.1f | ud=%.1f]   dx=%.1f   frame=%zu / %zu   %d fps",
                 pred.handState, pred.flexion, pred.upDown,
                 smoothed_dx, i + 1, liveStream.size(), FPS_TARGET); // Updated to show smoothed_dx
        cv::putText(display, info,
                    cv::Point(20, 68),
                    cv::FONT_HERSHEY_PLAIN, 0.95,
                    cv::Scalar(0, 220, 220), 1);

        cv::imshow("Ultrasound Etch-a-Sketch", display);

        // 4. TIMING — sleep to hit target FPS
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - frameStart).count();
        int sleepMs = FRAME_MS - (int)elapsed;
        if (sleepMs > 0) {
            if (cv::waitKey(sleepMs) == 27) goto done;  // ESC to quit
        } else {
            if (cv::waitKey(1) == 27) goto done;
        }
    }

done:
    cv::destroyAllWindows();
    return 0;
}