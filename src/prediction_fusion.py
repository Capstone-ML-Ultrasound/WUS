import argparse
import json
import os
import signal
import time
from dataclasses import dataclass, field
from typing import Dict, Optional

try:
    from confluent_kafka import Consumer, KafkaError, Producer
except ImportError:  # pragma: no cover
    Consumer = None
    KafkaError = None
    Producer = None


def _env_or_default(name: str, default: str) -> str:
    value = os.getenv(name)
    if value is None or len(value.strip()) == 0:
        return default
    return value


def _env_int(name: str, default: int) -> int:
    try:
        return int(_env_or_default(name, str(default)))
    except ValueError:
        return int(default)


def _env_float(name: str, default: float) -> float:
    try:
        return float(_env_or_default(name, str(default)))
    except ValueError:
        return float(default)


def _to_optional_float(value) -> Optional[float]:
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _to_optional_int(value) -> Optional[int]:
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _to_optional_seq(value) -> Optional[int]:
    out = _to_optional_int(value)
    if out is None or out < 0:
        return None
    return out


@dataclass
class PendingPair:
    first_seen_ns: int
    wrist: Optional[dict] = None
    hand: Optional[dict] = None


class PredictionFusionWorker:
    def __init__(
        self,
        bootstrap_servers: str,
        wrist_topic_in: str = "wrist_predictions",
        hand_topic_in: str = "hand_state_predictions",
        topic_out: str = "model_predictions",
        consumer_group_id: str = "prediction_fusion_group",
        auto_offset_reset: str = "latest",
        poll_timeout_s: float = 0.5,
        max_sync_lag_ms: int = 1000,
        max_pending: int = 5000,
        log_every: int = 100,
        prediction_log_every: int = 0,
    ):
        if Consumer is None or Producer is None:
            raise ImportError(
                "confluent-kafka is required for fusion. Install with: pip install confluent-kafka"
            )

        self.bootstrap_servers = str(bootstrap_servers)
        self.wrist_topic_in = str(wrist_topic_in)
        self.hand_topic_in = str(hand_topic_in)
        self.topic_out = str(topic_out)
        self.consumer_group_id = str(consumer_group_id)
        self.auto_offset_reset = str(auto_offset_reset)
        self.poll_timeout_s = float(poll_timeout_s)
        self.max_sync_lag_ms = max(10, int(max_sync_lag_ms))
        self.max_pending = max(100, int(max_pending))
        self.log_every = max(1, int(log_every))
        self.prediction_log_every = max(0, int(prediction_log_every))

        self.pending: Dict[int, PendingPair] = {}
        self.running = True
        self.total_seen = 0
        self.total_emitted = 0
        self.total_errors = 0
        self.total_expired = 0

    def stop(self) -> None:
        self.running = False

    def _maybe_parse_payload(self, payload: bytes) -> Optional[dict]:
        try:
            obj = json.loads(payload.decode("utf-8"))
        except Exception:
            return None
        return obj if isinstance(obj, dict) else None

    def _build_merged_payload(self, wrist: dict, hand: dict) -> bytes:
        source_sequence = _to_optional_seq(wrist.get("source_sequence"))
        if source_sequence is None:
            source_sequence = _to_optional_seq(hand.get("source_sequence"))
        if source_sequence is None:
            raise ValueError("Cannot merge payloads without source_sequence")

        source_ts_ns = _to_optional_int(wrist.get("source_ts_ns"))
        if source_ts_ns is None:
            source_ts_ns = _to_optional_int(hand.get("source_ts_ns"))

        flexion = _to_optional_float(wrist.get("flexion"))
        if flexion is None:
            flexion = _to_optional_float(wrist.get("prediction"))
        if flexion is None:
            flexion = 0.0

        hand_state = _to_optional_float(hand.get("hand_state"))
        if hand_state is None:
            hand_prob = _to_optional_float(hand.get("binary_probability"))
            if hand_prob is None:
                hand_prob = _to_optional_float(hand.get("prediction"))
            hand_state = 1.0 if (hand_prob is not None and hand_prob >= 0.5) else 0.0

        up_down = _to_optional_float(wrist.get("up_down"))
        if up_down is None:
            up_down = _to_optional_float(hand.get("up_down"))
        if up_down is None:
            up_down = 0.0

        wrist_ready = bool(wrist.get("ready", False))
        hand_ready = bool(hand.get("ready", False))
        ready = bool(wrist_ready and hand_ready)

        wrist_pred = _to_optional_float(wrist.get("prediction"))
        hand_prob = _to_optional_float(hand.get("binary_probability"))
        if hand_prob is None:
            hand_prob = _to_optional_float(hand.get("prediction"))

        wrist_ms = _to_optional_float(wrist.get("infer_time_ms"))
        hand_ms = _to_optional_float(hand.get("infer_time_ms"))
        infer_total_ms = None
        if wrist_ms is not None or hand_ms is not None:
            infer_total_ms = float((wrist_ms or 0.0) + (hand_ms or 0.0))

        wrist_conf = _to_optional_float(wrist.get("confidence"))
        hand_conf = _to_optional_float(hand.get("confidence"))
        if wrist_conf is None:
            wrist_conf = 1.0 if wrist_ready else 0.0
        if hand_conf is None:
            hand_conf = 1.0 if hand_ready else 0.0
        confidence = float(max(0.0, min(1.0, min(wrist_conf, hand_conf))))

        payload = {
            "source_sequence": int(source_sequence),
            "source_ts_ns": None if source_ts_ns is None else int(source_ts_ns),
            "prediction_ts_ns": int(time.time_ns()),
            "label": "combined_prediction" if ready else "calibrating",
            "confidence": confidence,
            "ready": ready,
            "hand_state": float(hand_state),
            "flexion": float(flexion),
            "up_down": float(up_down),
            "prediction": None if wrist_pred is None else float(wrist_pred),
            "hand_probability": None if hand_prob is None else float(hand_prob),
            "infer_time_ms": infer_total_ms,
            "infer_time_ms_wrist": wrist_ms,
            "infer_time_ms_hand": hand_ms,
            "wrist_ready": wrist_ready,
            "hand_ready": hand_ready,
            "calibration_mode": wrist.get("calibration_mode"),
            "calibration_n": wrist.get("calibration_n"),
            "calibration_frozen": wrist.get("calibration_frozen"),
            "num_samples": wrist.get("num_samples", hand.get("num_samples")),
            "model_name": "fused_wrist_and_hand_state",
        }
        return json.dumps(payload, separators=(",", ":"), ensure_ascii=True).encode("utf-8")

    def _log_merged_prediction(self, merged_payload: bytes) -> None:
        if self.prediction_log_every <= 0:
            return
        try:
            obj = json.loads(merged_payload.decode("utf-8"))
        except Exception:
            return
        seq = _to_optional_seq(obj.get("source_sequence"))
        hand_prob = _to_optional_float(obj.get("hand_probability"))
        hand_state = _to_optional_float(obj.get("hand_state"))
        flexion = _to_optional_float(obj.get("flexion"))
        ready = bool(obj.get("ready", False))
        if hand_prob is None:
            hand_prob_str = "n/a"
        else:
            hand_prob_str = f"{hand_prob:.6f}"
        if hand_state is None:
            hand_state_str = "n/a"
        else:
            hand_state_str = f"{hand_state:.3f}"
        if flexion is None:
            flexion_str = "n/a"
        else:
            flexion_str = f"{flexion:.3f}"
        print(
            "[PredictionFusion][Merged] "
            f"seq={seq} ready={int(ready)} "
            f"hand_prob={hand_prob_str} hand_state={hand_state_str} "
            f"flexion={flexion_str}",
            flush=True,
        )

    def _produce(self, producer: Producer, payload: bytes) -> None:
        while self.running:
            try:
                producer.produce(self.topic_out, value=payload)
                producer.poll(0)
                return
            except BufferError:
                producer.poll(0.05)

    def _expire_old_pending(self) -> None:
        if not self.pending:
            return
        now_ns = time.time_ns()
        ttl_ns = int(self.max_sync_lag_ms) * 1_000_000
        expired_keys = []
        for sequence, item in self.pending.items():
            if now_ns - item.first_seen_ns > ttl_ns:
                expired_keys.append(sequence)
        for sequence in expired_keys:
            self.pending.pop(sequence, None)
        self.total_expired += len(expired_keys)

        if len(self.pending) > self.max_pending:
            ordered = sorted(self.pending.items(), key=lambda kv: kv[1].first_seen_ns)
            to_drop = len(self.pending) - self.max_pending
            for sequence, _ in ordered[:to_drop]:
                self.pending.pop(sequence, None)
            self.total_expired += to_drop

    def run(self) -> None:
        consumer = Consumer(
            {
                "bootstrap.servers": self.bootstrap_servers,
                "group.id": self.consumer_group_id,
                "auto.offset.reset": self.auto_offset_reset,
            }
        )
        producer = Producer({"bootstrap.servers": self.bootstrap_servers})
        consumer.subscribe([self.wrist_topic_in, self.hand_topic_in])

        print(
            f"[PredictionFusion] Subscribed to {self.wrist_topic_in} and {self.hand_topic_in}",
            flush=True,
        )
        print(f"[PredictionFusion] Publishing fused predictions to {self.topic_out}", flush=True)

        try:
            while self.running:
                msg = consumer.poll(self.poll_timeout_s)
                if msg is None:
                    self._expire_old_pending()
                    continue

                if msg.error():
                    if KafkaError is not None and msg.error().code() == KafkaError._PARTITION_EOF:
                        continue
                    self.total_errors += 1
                    print(f"[PredictionFusion] Consumer error: {msg.error()}", flush=True)
                    continue

                topic = msg.topic()
                obj = self._maybe_parse_payload(msg.value())
                if obj is None:
                    self.total_errors += 1
                    if self.total_errors % 25 == 1:
                        preview = msg.value()[:200]
                        print(f"[PredictionFusion] Could not parse payload from {topic}: {preview!r}", flush=True)
                    continue

                sequence = _to_optional_seq(obj.get("source_sequence"))
                if sequence is None:
                    self.total_errors += 1
                    if self.total_errors % 25 == 1:
                        print(f"[PredictionFusion] Missing source_sequence in payload from {topic}", flush=True)
                    continue

                item = self.pending.get(sequence)
                if item is None:
                    item = PendingPair(first_seen_ns=time.time_ns())
                    self.pending[sequence] = item

                if topic == self.wrist_topic_in:
                    item.wrist = obj
                elif topic == self.hand_topic_in:
                    item.hand = obj
                else:
                    continue

                self.total_seen += 1
                if item.wrist is not None and item.hand is not None:
                    try:
                        merged = self._build_merged_payload(item.wrist, item.hand)
                        self._produce(producer, merged)
                        self.total_emitted += 1
                        if (
                            self.prediction_log_every > 0
                            and self.total_emitted % self.prediction_log_every == 0
                        ):
                            self._log_merged_prediction(merged)
                    except Exception as exc:
                        self.total_errors += 1
                        if self.total_errors % 25 == 1:
                            print(f"[PredictionFusion] Merge error for seq={sequence}: {exc}", flush=True)
                    finally:
                        self.pending.pop(sequence, None)

                if self.total_seen % 50 == 0:
                    self._expire_old_pending()
                if self.total_emitted > 0 and self.total_emitted % self.log_every == 0:
                    print(
                        "[PredictionFusion] "
                        f"seen={self.total_seen} "
                        f"emitted={self.total_emitted} "
                        f"pending={len(self.pending)} "
                        f"expired={self.total_expired} "
                        f"errors={self.total_errors}",
                        flush=True,
                    )
        finally:
            consumer.close()
            producer.flush(5.0)
            print(
                "[PredictionFusion] Done. "
                f"seen={self.total_seen} "
                f"emitted={self.total_emitted} "
                f"expired={self.total_expired} "
                f"errors={self.total_errors}",
                flush=True,
            )


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Synchronize wrist + hand predictions for visualizer")
    parser.add_argument(
        "--bootstrap-servers",
        default=_env_or_default("BOOTSTRAP_SERVERS", "localhost:9092"),
    )
    parser.add_argument(
        "--wrist-topic-in",
        default=_env_or_default("WRIST_TOPIC_IN", "wrist_predictions"),
    )
    parser.add_argument(
        "--hand-topic-in",
        default=_env_or_default("HAND_TOPIC_IN", "hand_state_predictions"),
    )
    parser.add_argument("--topic-out", default=_env_or_default("TOPIC_OUT", "model_predictions"))
    parser.add_argument(
        "--consumer-group-id",
        default=_env_or_default("CONSUMER_GROUP_ID", "prediction_fusion_group"),
    )
    parser.add_argument(
        "--auto-offset-reset",
        choices=["earliest", "latest"],
        default=_env_or_default("AUTO_OFFSET_RESET", "latest"),
    )
    parser.add_argument("--poll-timeout-s", type=float, default=_env_float("POLL_TIMEOUT_S", 0.5))
    parser.add_argument("--max-sync-lag-ms", type=int, default=_env_int("MAX_SYNC_LAG_MS", 1000))
    parser.add_argument("--max-pending", type=int, default=_env_int("MAX_PENDING", 5000))
    parser.add_argument("--log-every", type=int, default=_env_int("LOG_EVERY", 100))
    parser.add_argument(
        "--prediction-log-every",
        type=int,
        default=_env_int("PREDICTION_LOG_EVERY", 0),
        help="Emit merged prediction logs every N messages (0 disables).",
    )
    return parser


def main() -> None:
    args = build_arg_parser().parse_args()
    worker = PredictionFusionWorker(
        bootstrap_servers=args.bootstrap_servers,
        wrist_topic_in=args.wrist_topic_in,
        hand_topic_in=args.hand_topic_in,
        topic_out=args.topic_out,
        consumer_group_id=args.consumer_group_id,
        auto_offset_reset=args.auto_offset_reset,
        poll_timeout_s=args.poll_timeout_s,
        max_sync_lag_ms=args.max_sync_lag_ms,
        max_pending=args.max_pending,
        log_every=args.log_every,
        prediction_log_every=args.prediction_log_every,
    )

    def _signal_handler(signum, _frame):
        print(f"[PredictionFusion] Shutdown signal {signum} received...", flush=True)
        worker.stop()

    signal.signal(signal.SIGINT, _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)
    worker.run()


if __name__ == "__main__":
    main()
