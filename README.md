# Wireless Ultrasound System (WUS)

This project implements a streaming ultrasound pipeline over Kafka, including live ingestion, Python wrist-regression inference, archival sinks, and replay services.

## Architecture

Core topics:
- `ultrasound_raw_data`: live raw frames from hardware.
- `wrist_predictions`: regression output from `src/wrist_inference.py`.
- `hand_state_predictions`: binary open/close output from `src/inference.py`.
- `model_predictions`: fused vector output from `src/prediction_fusion.py` for the visualizer.
- `ultrasound_raw_data_replay`: replayed raw frames for offline validation.

Main components:
1. `us_acq` (host process): acquires from hardware and publishes raw frames.
2. `wrist-inference-*`: Python Kafka worker (`src/wrist_inference.py`) that consumes raw frames and publishes `wrist_predictions` (flexion/regression).
3. `binary-inference-*`: Python Kafka worker (`src/inference.py`) that consumes raw frames and publishes `hand_state_predictions` (open/close probability and state).
4. `prediction-fusion`: Python Kafka worker (`src/prediction_fusion.py`) that synchronizes wrist + hand predictions by `source_sequence` and publishes fused `model_predictions`.
5. `visualizer`: Python consumer (`src/visualizer.py`) that consumes `model_predictions` and renders an Etch-a-Sketch-style cursor. It runs headless in Docker by default; GUI mode is available on host with OpenCV.
6. `raw_sink`: consumes `ultrasound_raw_data`, writes burst CSVs, optionally uploads to GCS.
7. `replay_raw`: reads archived raw CSVs from GCS and republishes to replay raw topic.

## Docker Compose Profiles

`kafka` always starts when you run `docker compose up ...`. Other services are controlled by profiles.

| Profile | Services enabled |
| --- | --- |
| `live` | `wrist-inference-live`, `binary-inference-live`, `prediction-fusion`, `visualizer` |
| `testing` | `raw_sink` |
| `data-gathering` | `raw_sink` |
| `replay-raw` | `replay_raw_service`, `wrist-inference-replay-raw`, `binary-inference-replay-raw`, `prediction-fusion`, `visualizer` |

## Docker Quick Start

Use one of these common launch patterns:

```bash
# Live pipeline (no archival sinks)
docker compose --profile live up --build

# Live pipeline + raw archival sink
docker compose --profile live --profile testing up --build

# Data gathering without model inference or prediction consumption
docker compose --profile data-gathering up --build

# Replay archived raw data through wrist inference
docker compose --profile replay-raw up --build
```

To inspect exactly what will run for your selected profiles:

```bash
docker compose --profile live --profile testing config
```

## Environment Variables (GCS and Replay)

You can export these in your shell or place them in a `.env` file next to `docker-compose.yml`.

Raw archive sink (`raw_sink`):
- `RAW_GCS_BUCKET` (default: `raw-capstone-bucket`)
- `RAW_GCS_PREFIX` (default: `ultrasound/raw`)
- `RAW_GCS_KEEP_LOCAL` (default: `false`)

Python wrist inference (`wrist-inference-live`, `wrist-inference-replay-raw`):
- `MODEL_DIR` (default in compose: `/app/ml_infra/wrist_regressor/models/boosted_tree_regression_sessionnorm`)
- `CALIBRATION_MODE` (`warmup_freeze` by default; optional: `fixed`, `continuous`)
- `WARMUP_FRAMES` (default: `50`)
- `FREEZE_AFTER` (default: `50`)
- `FIXED_CALIBRATION_PATH` (optional `.npz` with `mean` and `std` arrays for `fixed` mode)
- `TOPIC_OUT` (compose default: `wrist_predictions`)

Binary hand-state inference (`binary-inference-live`, `binary-inference-replay-raw`):
- `MODEL_JSON_PATH` (default in compose: `/app/ml_infra/binary_classifier/tree_serialization/xgb_binary_open_close_hand_model.json`)
- `MODEL_LIB_PATH` (optional TL2cgen shared library path; JSON backend is preferred in compose)
- `INPUT_DIM` (default: `200`)
- `NORMALIZE_FRAME` (default: `false`)
- `HAND_STATE_THRESHOLD` (default: `0.5`)
- `TOPIC_OUT` (compose default: `hand_state_predictions`)

Prediction fusion (`prediction-fusion`):
- `WRIST_TOPIC_IN` (default: `wrist_predictions`)
- `HAND_TOPIC_IN` (default: `hand_state_predictions`)
- `TOPIC_OUT` (default: `model_predictions`)
- `FUSION_MAX_SYNC_LAG_MS` (default: `1000`)

Visualizer (`visualizer`):
- `VISUALIZER_CONSUMER_GROUP_ID` (maps to container `CONSUMER_GROUP_ID`; default: `visualizer_group_docker`)
- `VISUALIZER_GUI` (default: `false`; set `true` to request OpenCV window mode)
- `VISUALIZER_LOG_EVERY` (default: `50`)
- `TOPIC_IN` (default: `model_predictions`)

Visualizer connection defaults:
- `BOOTSTRAP_SERVERS` default is `localhost:9092` for host runs.
- In Docker Compose, visualizer uses `BOOTSTRAP_SERVERS=kafka:29092`.
- Host `src/visualizer.py` default group id is `visualizer_group_local` to avoid clashing with Docker's default consumer group.

Replay raw (`replay_raw_service`):
- `REPLAY_RAW_GCS_PREFIX` (default: `ultrasound/raw`)
- `REPLAY_RAW_FILE_PATTERN` (optional filter)
- `REPLAY_RAW_LOOP` (default: `false`)
- `REPLAY_RAW_FRAME_INTERVAL_MS` (default: `20`)

Shared cloud auth:
- `GCP_PROJECT_ID`
- `GOOGLE_APPLICATION_CREDENTIALS` (container path; repo mounts `./secrets:/app/secrets:ro`)

## Producer (Host Machine)

The producer must run on the host to access USB/serial hardware.

```bash
make us_acq
./us_acq.exe
```

## Local Build (No Docker)

Build all binaries:

```bash
make
```

Build individual binaries:

```bash
make us_acq
make replay_raw
make raw_sink
```

Run GUI visualizer on host (recommended easiest GUI path):

```bash
# In one terminal
docker compose --profile live up --build

# In another terminal (host)
python -m pip install opencv-python confluent-kafka
VISUALIZER_GUI=true BOOTSTRAP_SERVERS=localhost:9092 python src/visualizer.py
```

PowerShell equivalent:

```powershell
python -m pip install opencv-python confluent-kafka
$env:VISUALIZER_GUI="true"
$env:BOOTSTRAP_SERVERS="localhost:9092"
python src/visualizer.py
```

Always run live GUI visualizer on host (PowerShell):

```powershell
# Terminal 1: run Kafka + topic init + inference + fusion (without docker visualizer)
docker compose up --build kafka kafka-init wrist-inference-live binary-inference-live prediction-fusion

# Terminal 2: run host visualizer in GUI mode
$env:VISUALIZER_GUI="true"
$env:BOOTSTRAP_SERVERS="localhost:9092"
$env:CONSUMER_GROUP_ID="visualizer_gui_host"
python src/visualizer.py
```

## Visualizer Details

`src/visualizer.py` accepts prediction payloads in either of these forms:
- Fused payload from `src/prediction_fusion.py` (recommended): includes `hand_state`, `flexion`, `up_down`, `ready`, and latency metadata.
- Optional direct payloads with `prediction`/`ready` or `hand_state`/`flexion`/`up_down`.

Mapping and behavior:
- `flexion` drives horizontal velocity after clamping to `[-80, +50]` degrees.
- A deadzone (`±3` degrees) suppresses jitter near neutral.
- Velocity is smoothed with an exponential filter (`alpha=0.8`).
- `hand_state > 0.5` means pen down; otherwise cursor hovers.
- `up_down` shifts vertical position (defaults to `0` when omitted).

GUI controls:
- Press `Esc` or `q` to close the window.

Headless logs:
- In non-GUI mode, visualizer prints one status line every `VISUALIZER_LOG_EVERY` valid frames.

For the offline C++ playground visualizer and CSV replay tooling, see:
- `ui/ui_playground-new_UI_1/README.md`
