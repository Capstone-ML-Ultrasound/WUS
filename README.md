# Wireless Ultrasound System (WUS)

This project implements a streaming ultrasound pipeline over Kafka, including live ingestion, preprocessing, ML-style prediction publishing, archival sinks, and replay services.

## Architecture

Core topics:
- `ultrasound_raw_data`: live raw frames from hardware.
- `ultrasound.clean`: preprocessed frames.
- `model.predictions`: prediction messages from `output`.

Main components:
1. `us_acq` (host process): acquires from hardware and publishes raw frames.
2. `preprocess`: consumes raw frames and publishes cleaned frames.
3. `output`: consumes cleaned frames and publishes `model.predictions`.
4. `prediction_consumer`: consumes and logs prediction events.
5. `raw_sink`: consumes `ultrasound_raw_data`, writes burst CSVs, optionally uploads to GCS.
6. `process_sink`: consumes `ultrasound.clean`, writes burst CSVs, optionally uploads to GCS.
7. `replay_raw`: reads archived raw CSVs from GCS and republishes to replay raw topic.
8. `replay_processed`: reads archived processed CSVs from GCS and republishes to replay processed topic.

## Docker Compose Profiles

`kafka` always starts when you run `docker compose up ...`. Other services are controlled by profiles.

| Profile | Services enabled |
| --- | --- |
| `live` | `preprocess-live`, `output-live`, `prediction_consumer` |
| `testing` | `raw_sink`, `process_sink` |
| `replay-raw` | `replay_raw_service`, `preprocess-replay`, `output-live`, `prediction_consumer` |
| `replay-preprocess` | `replay_processed_service`, `output-replay-preprocess`, `prediction_consumer` |

## Docker Quick Start

Use one of these common launch patterns:

```bash
# Live pipeline (no archival sinks)
docker compose --profile live up --build

# Live pipeline + raw/processed archival sinks
docker compose --profile live --profile testing up --build

# Replay archived raw data through preprocess + output
docker compose --profile replay-raw up --build

# Replay archived processed data directly into output
docker compose --profile replay-preprocess up --build
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

Processed archive sink (`process_sink`):
- `PROCESSED_GCS_BUCKET` (default: `processed-bucket-capstone`)
- `PROCESSED_GCS_PREFIX` (default: `ultrasound/processed`)
- `PROCESSED_GCS_KEEP_LOCAL` (default: `false`)
- `PROCESS_SINK_IDLE_FLUSH_MS` (default: `5000` in compose; set `0` to disable)

Replay raw (`replay_raw_service`):
- `REPLAY_RAW_GCS_PREFIX` (default: `ultrasound/raw`)
- `REPLAY_RAW_FILE_PATTERN` (optional filter)
- `REPLAY_RAW_LOOP` (default: `false`)
- `REPLAY_RAW_FRAME_INTERVAL_MS` (default: `20`)

Replay processed (`replay_processed_service`):
- `REPLAY_PROCESSED_GCS_PREFIX` (default: `ultrasound/processed`)
- `REPLAY_PROCESSED_FILE_PATTERN` (optional filter)
- `REPLAY_PROCESSED_LOOP` (default: `false`)
- `REPLAY_PROCESSED_FRAME_INTERVAL_MS` (default: `20`)

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
make preprocess
make output
make prediction_consumer
make process_sink
make replay_raw
make replay_processed
```
