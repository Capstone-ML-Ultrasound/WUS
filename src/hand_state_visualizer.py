import json
import os
import signal
import sys
import time
from dataclasses import dataclass
from typing import Optional

try:
    from confluent_kafka import Consumer, KafkaError
except ImportError as exc:  # pragma: no cover
    raise ImportError(
        "confluent-kafka is required for hand-state visualizer. Install with: pip install confluent-kafka"
    ) from exc


TOPIC_IN_DEFAULT = "hand_state_predictions"
GROUP_ID_DEFAULT = "hand_state_visualizer_group_local"
WINDOW_NAME = "Hand State Visualizer"

running = True


@dataclass
class HandStatePrediction:
    closed_probability: float
    hand_state: float
    ready: bool
    label: Optional[str] = None
    source_sequence: Optional[int] = None
    source_ts_ns: Optional[int] = None
    prediction_ts_ns: Optional[int] = None
    infer_time_ms: Optional[float] = None


def log(message: str) -> None:
    print(message, flush=True)


def _env_or_default(name: str, default: str) -> str:
    raw = os.getenv(name)
    if raw is None or len(raw.strip()) == 0:
        return default
    return raw


def _env_int(name: str, default: int) -> int:
    try:
        return int(_env_or_default(name, str(default)))
    except ValueError:
        return int(default)


def _env_bool(name: str, default: bool) -> bool:
    raw = os.getenv(name)
    if raw is None or len(raw.strip()) == 0:
        return default
    return raw.strip().lower() in {"1", "true", "yes", "y", "on"}


def _to_optional_int(value) -> Optional[int]:
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _to_optional_float(value) -> Optional[float]:
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _latency_ms(newer_ns: Optional[int], older_ns: Optional[int]) -> Optional[float]:
    if newer_ns is None or older_ns is None:
        return None
    delta = int(newer_ns) - int(older_ns)
    if delta < 0:
        return None
    return float(delta) / 1_000_000.0


def _fmt_ms(value: Optional[float]) -> str:
    if value is None:
        return "n/a"
    return f"{value:.1f}"


def _clamp01(value: float) -> float:
    return max(0.0, min(1.0, float(value)))


def _signal_handler(signum, _frame):
    global running
    log(f"[HandStateVisualizer] Shutdown signal {signum} received...")
    running = False


def decode_prediction(payload: bytes) -> Optional[HandStatePrediction]:
    try:
        obj = json.loads(payload.decode("utf-8"))
    except Exception:
        return None

    label_raw = obj.get("label")
    label = str(label_raw) if label_raw is not None else None

    closed_probability = _to_optional_float(
        obj.get("binary_probability", obj.get("prediction"))
    )
    hand_state = _to_optional_float(obj.get("hand_state"))
    ready = bool(obj.get("ready", False))

    if closed_probability is None and hand_state is None and label is None:
        return None

    if hand_state is None:
        hand_state = 1.0 if (closed_probability or 0.0) >= 0.5 else 0.0
    if closed_probability is None:
        closed_probability = 1.0 if hand_state > 0.5 else 0.0

    return HandStatePrediction(
        closed_probability=_clamp01(closed_probability),
        hand_state=float(hand_state),
        ready=ready,
        label=label,
        source_sequence=_to_optional_int(obj.get("source_sequence")),
        source_ts_ns=_to_optional_int(obj.get("source_ts_ns")),
        prediction_ts_ns=_to_optional_int(obj.get("prediction_ts_ns")),
        infer_time_ms=_to_optional_float(obj.get("infer_time_ms")),
    )


class HandStateGuiRenderer:
    def __init__(self, closed_when_high: bool = True, fullscreen: bool = True):
        import cv2  # local import so headless mode does not require opencv
        import numpy as np

        self.cv2 = cv2
        self.np = np
        self.closed_when_high = bool(closed_when_high)

        self.width = 1280
        self.height = 720
        self.top_bar_height = 120

        cv2.namedWindow(WINDOW_NAME, cv2.WINDOW_NORMAL)
        if fullscreen:
            cv2.setWindowProperty(WINDOW_NAME, cv2.WND_PROP_FULLSCREEN, cv2.WINDOW_FULLSCREEN)

    def _is_closed(self, pred: HandStatePrediction) -> bool:
        if pred.label is not None:
            label = pred.label.strip().lower()
            if "hand_closed" in label or label == "closed":
                return True
            if "hand_open" in label or label == "open":
                return False
        if self.closed_when_high:
            return pred.hand_state > 0.5
        return pred.hand_state <= 0.5

    def render(
        self,
        pred: HandStatePrediction,
        e2e_ms: Optional[float],
        model_path_ms: Optional[float],
        post_model_ms: Optional[float],
        frame_idx: int,
    ) -> bool:
        cv2 = self.cv2
        np = self.np

        closed = self._is_closed(pred)
        state_text = "CLOSED" if closed else "OPEN"

        bg_color = (0, 0, 230) if closed else (0, 180, 0)
        display = np.full((self.height, self.width, 3), bg_color, dtype=np.uint8)

        cv2.rectangle(display, (0, 0), (self.width, self.top_bar_height), (0, 0, 0), -1)

        pred_text = (
            f"PRED: {state_text}  closed_prob={pred.closed_probability:.3f}  "
            f"hand_state={pred.hand_state:.2f}  ready={int(pred.ready)}"
        )
        lat_text = (
            f"LAT(ms): e2e={_fmt_ms(e2e_ms)}  model_path={_fmt_ms(model_path_ms)}  "
            f"infer={_fmt_ms(pred.infer_time_ms)}  post_model={_fmt_ms(post_model_ms)}"
        )
        seq_text = f"SEQ: {pred.source_sequence}  frame={frame_idx}"

        cv2.putText(
            display,
            pred_text,
            (20, 34),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.75,
            (255, 255, 255),
            2,
            cv2.LINE_AA,
        )
        cv2.putText(
            display,
            lat_text,
            (20, 70),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.75,
            (255, 255, 255),
            2,
            cv2.LINE_AA,
        )
        cv2.putText(
            display,
            seq_text,
            (20, 106),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.75,
            (255, 255, 255),
            2,
            cv2.LINE_AA,
        )

        font = cv2.FONT_HERSHEY_DUPLEX
        font_scale = 5.2
        thickness = 10
        (text_w, text_h), baseline = cv2.getTextSize(state_text, font, font_scale, thickness)
        text_x = max(0, (self.width - text_w) // 2)
        text_y = self.top_bar_height + ((self.height - self.top_bar_height) + text_h) // 2
        text_y = min(self.height - baseline - 20, text_y)

        cv2.putText(
            display,
            state_text,
            (text_x, text_y),
            font,
            font_scale,
            (255, 255, 255),
            thickness,
            cv2.LINE_AA,
        )

        cv2.imshow(WINDOW_NAME, display)
        key = cv2.waitKey(1) & 0xFF
        return key not in (27, ord("q"), ord("Q"))

    def close(self):
        self.cv2.destroyAllWindows()


def main():
    signal.signal(signal.SIGINT, _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)

    brokers = _env_or_default("BOOTSTRAP_SERVERS", "localhost:9092")
    topic_in = _env_or_default("TOPIC_IN", TOPIC_IN_DEFAULT)
    group_id = _env_or_default("CONSUMER_GROUP_ID", GROUP_ID_DEFAULT)
    log_every = max(1, _env_int("HAND_STATE_VISUALIZER_LOG_EVERY", 20))
    request_gui = _env_bool("HAND_STATE_VISUALIZER_GUI", False)
    fullscreen = _env_bool("HAND_STATE_VISUALIZER_FULLSCREEN", True)
    hand_closed_when_high = _env_bool("VISUALIZER_HAND_CLOSED_WHEN_HIGH", True)

    consumer = Consumer(
        {
            "bootstrap.servers": brokers,
            "group.id": group_id,
            "auto.offset.reset": "latest",
        }
    )
    consumer.subscribe([topic_in])
    log(f"[HandStateVisualizer] Subscribed to {topic_in} (group={group_id})")

    gui = None
    if request_gui:
        try:
            gui = HandStateGuiRenderer(
                closed_when_high=hand_closed_when_high,
                fullscreen=fullscreen,
            )
            log("[HandStateVisualizer] GUI mode enabled.")
        except Exception as exc:
            log(
                "[HandStateVisualizer] GUI requested but unavailable. "
                "Install opencv-python and run on a desktop session."
            )
            log(f"[HandStateVisualizer] GUI init error: {exc}")
            gui = None

    if gui is None:
        log("[HandStateVisualizer] Headless mode enabled.")

    seen = 0
    parse_failures = 0

    try:
        while running:
            msg = consumer.poll(0.5)
            if msg is None:
                continue

            if msg.error():
                if msg.error().code() == KafkaError._PARTITION_EOF:
                    continue
                log(f"[HandStateVisualizer] Consumer error: {msg.error()}")
                continue

            pred = decode_prediction(msg.value())
            if pred is None:
                parse_failures += 1
                if parse_failures % 25 == 1:
                    preview = msg.value()[:200]
                    log(f"[HandStateVisualizer] Could not parse payload: {preview!r}")
                continue

            seen += 1
            now_ns = time.time_ns()
            e2e_ms = _latency_ms(now_ns, pred.source_ts_ns)
            model_path_ms = _latency_ms(pred.prediction_ts_ns, pred.source_ts_ns)
            post_model_ms = _latency_ms(now_ns, pred.prediction_ts_ns)

            if gui is not None:
                if not gui.render(pred, e2e_ms, model_path_ms, post_model_ms, seen):
                    break
            elif seen % log_every == 0:
                state = "CLOSED" if pred.hand_state > 0.5 else "OPEN"
                log(
                    "[HandStateVisualizer] "
                    f"frame={seen} state={state} ready={int(pred.ready)} "
                    f"closed_prob={pred.closed_probability:.3f} "
                    f"lat_e2e_ms={_fmt_ms(e2e_ms)} "
                    f"lat_model_path_ms={_fmt_ms(model_path_ms)} "
                    f"infer_ms={_fmt_ms(pred.infer_time_ms)} "
                    f"lat_post_model_ms={_fmt_ms(post_model_ms)}"
                )
    finally:
        consumer.close()
        if gui is not None:
            gui.close()
        log("[HandStateVisualizer] Done.")


if __name__ == "__main__":
    sys.exit(main() or 0)
