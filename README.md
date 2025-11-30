# WUS

## Quick Start with Docker Kafka
1. Install Docker (with Compose).
2. Start Kafka (single-node KRaft, auto-create topics enabled):
   ```
   docker compose up -d
   ```
   Kafka listens on `localhost:9092`. Data persisted under `./kafka_data`.
3. Build:
   ```
   make
   ```
4. Run consumers/producers in separate shells from the repo root:
   ```
   ./consumer    # writes burst CSVs to ./data every 50 frames
   ./us_acq      # produces ultrasound frames to Kafka topic ultrasound_raw_data
   ```

## Useful Commands
- Stop Kafka: `docker compose down`
- View Kafka logs: `docker compose logs -f kafka`
- Clean build artifacts: `make clean`
