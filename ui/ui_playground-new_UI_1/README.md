# UI Visualizer Playground

This folder contains an offline OpenCV visualizer used to test the Etch-a-Sketch cursor mapping without Kafka.

## What Is Here

- `cv_etch_a_sketch.cpp`: replays a CSV stream and renders cursor + drawing state.
- `convert_predictions.py`: converts `frame_idx,pred` CSV into the 3-column vector format.
- `vector_simulation_3col.csv`: example input in the new format.

## Input Format

`cv_etch_a_sketch.cpp` expects rows with 3 float columns:

```text
hand_state,flexion,up_down
```

- `hand_state`: `0` = hover, `1` = pen down (draw).
- `flexion`: wrist angle in degrees (expected approx `-80` to `+50`).
  - negative -> move left
  - positive -> move right
- `up_down`: vertical delta per frame (currently often `0`).

The loader skips invalid rows, so a CSV header is allowed.

## Build And Run

From `ui/ui_playground-new_UI_1`:

```bash
cmake -S . -B build
cmake --build build --config Release
```

Run from the `build` directory so the default CSV path (`../vector_simulation_3col.csv`) resolves:

```bash
# Linux/macOS
cd build && ./temp

# Windows (PowerShell)
cd build; .\Release\temp.exe
```

If your generator outputs `temp.exe` directly in `build`, run `.\temp.exe`.

## Converting Model Output CSV

If you have model predictions shaped like `frame_idx,pred`, convert them:

```bash
python convert_predictions.py --input session_010_full_predictions.csv --output vector_simulation_3col.csv
```

## Notes

- Target playback speed is `25 FPS` (`FPS_TARGET` in `cv_etch_a_sketch.cpp`).
- Motion uses deadzone + low-pass smoothing to reduce jitter.
- Press `Esc` to exit.
- For the live Kafka visualizer (`src/visualizer.py`), see the repo root `README.md`.
