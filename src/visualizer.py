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
        "confluent-kafka is required for visualizer. Install with: pip install confluent-kafka"
    ) from exc


TOPIC_IN_DEFAULT = "model_predictions"
GROUP_ID_DEFAULT = "visualizer_group_local"

FLEXION_MIN = -80.0
FLEXION_MAX = 50.0
MAX_SPEED = 12.0
DEADZONE_DEG = 3.0
SMOOTHING_ALPHA = 0.8

WINDOW_NAME = "Ultrasound Etch-a-Sketch"

running = True


@dataclass
class ModelPrediction:
    hand_state: float
    flexion: float
    up_down: float
    ready: bool
    source_sequence: Optional[int] = None
    source_ts_ns: Optional[int] = None
    prediction_ts_ns: Optional[int] = None


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
    v = raw.strip().lower()
    return v in {"1", "true", "yes", "y", "on"}


def _signal_handler(signum, _frame):
    global running
    log(f"[Visualizer] Shutdown signal {signum} received...")
    running = False


def clamp_flexion(x: float) -> float:
    return max(FLEXION_MIN, min(FLEXION_MAX, float(x)))


def flexion_to_speed(deg: float) -> float:
    d = float(deg)
    if abs(d) < DEADZONE_DEG:
        return 0.0
    if d < 0.0:
        norm = d / (-FLEXION_MIN)
    else:
        norm = d / FLEXION_MAX
    norm = max(-1.0, min(norm, 1.0))
    return norm * MAX_SPEED


def _to_optional_int(value) -> Optional[int]:
    if value is None:
        return None
    try:
        return int(value)
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


def decode_prediction(payload: bytes) -> Optional[ModelPrediction]:
    try:
        obj = json.loads(payload.decode("utf-8"))
    except Exception:
        return None

    prediction = obj.get("prediction", None)
    flexion = obj.get("flexion", prediction)
    ready = bool(obj.get("ready", False))

    if flexion is None and obj.get("ready", None) is None:
        return None

    try:
        flexion_value = clamp_flexion(0.0 if flexion is None else float(flexion))
    except (TypeError, ValueError):
        return None

    hand_state_raw = obj.get("hand_state", 1.0 if ready else 0.0)
    up_down_raw = obj.get("up_down", 0.0)

    try:
        hand_state = float(hand_state_raw)
        up_down = float(up_down_raw)
    except (TypeError, ValueError):
        hand_state = 1.0 if ready else 0.0
        up_down = 0.0

    return ModelPrediction(
        hand_state=hand_state,
        flexion=flexion_value,
        up_down=up_down,
        ready=ready,
        source_sequence=_to_optional_int(obj.get("source_sequence")),
        source_ts_ns=_to_optional_int(obj.get("source_ts_ns")),
        prediction_ts_ns=_to_optional_int(obj.get("prediction_ts_ns")),
    )


class GuiRenderer:
    def __init__(self):
        import cv2  # local import so headless mode does not require opencv
        import numpy as np

        self.cv2 = cv2
        self.np = np

        self.width = 1000
        self.height = 700
        self.header_height = 90
        self.canvas = np.full((self.height, self.width, 3), 255, dtype=np.uint8)
        self.pos_x = self.width / 2.0
        self.pos_y = self.header_height + (self.height - self.header_height) / 2.0

        cv2.namedWindow(WINDOW_NAME, cv2.WINDOW_AUTOSIZE)

    def render(
        self,
        pred: ModelPrediction,
        smoothed_dx: float,
        frame_idx: int,
        e2e_ms: Optional[float],
        model_ms: Optional[float],
        post_model_ms: Optional[float],
    ) -> bool:
        cv2 = self.cv2

        prev_x, prev_y = self.pos_x, self.pos_y
        self.pos_x += float(smoothed_dx)
        self.pos_y += float(pred.up_down)

        self.pos_x = max(0.0, min(self.pos_x, float(self.width - 1)))
        self.pos_y = max(float(self.header_height), min(self.pos_y, float(self.height - 1)))

        pen_down = pred.hand_state > 0.5
        if pen_down:
            cv2.line(
                self.canvas,
                (int(prev_x), int(prev_y)),
                (int(self.pos_x), int(self.pos_y)),
                (0, 0, 200),
                3,
                cv2.LINE_AA,
            )

        display = self.canvas.copy()

        cursor_color = (0, 0, 255) if pen_down else (80, 80, 255)
        cv2.circle(display, (int(self.pos_x), int(self.pos_y)), 6, cursor_color, -1)

        cv2.rectangle(display, (0, 0), (self.width, self.header_height), (30, 30, 30), -1)
        cv2.line(
            display,
            (0, self.header_height),
            (self.width, self.header_height),
            (90, 90, 90),
            2,
        )

        state_str = "STATE: PEN DOWN" if pen_down else "STATE: HOVERING"
        state_color = (80, 80, 255) if pen_down else (200, 200, 200)
        cv2.putText(
            display,
            state_str,
            (20, 38),
            cv2.FONT_HERSHEY_DUPLEX,
            0.75,
            state_color,
            2,
        )

        bar_centre = self.width // 2
        norm_flex = (pred.flexion - FLEXION_MIN) / (FLEXION_MAX - FLEXION_MIN)
        norm_flex = max(0.0, min(norm_flex, 1.0))
        bar_x = int(norm_flex * (self.width - 40)) + 20

        cv2.line(
            display,
            (bar_centre, self.header_height - 18),
            (bar_centre, self.header_height - 6),
            (60, 60, 60),
            2,
        )
        bar_color = (255, 120, 0) if pred.flexion < 0 else (0, 220, 100)
        cv2.circle(display, (bar_x, self.header_height - 12), 6, bar_color, -1)

        info = (
            f"VEC[{pred.hand_state:.1f} | flex={pred.flexion:.1f} | ud={pred.up_down:.1f}] "
            f"dx={smoothed_dx:.2f} frame={frame_idx}"
        )
        cv2.putText(
            display,
            info,
            (20, 68),
            cv2.FONT_HERSHEY_PLAIN,
            0.95,
            (0, 220, 220),
            1,
        )

        latency_info = (
            f"LAT e2e={_fmt_ms(e2e_ms)}ms "
            f"model={_fmt_ms(model_ms)}ms "
            f"gui={_fmt_ms(post_model_ms)}ms"
        )
        cv2.putText(
            display,
            latency_info,
            (20, 84),
            cv2.FONT_HERSHEY_PLAIN,
            0.95,
            (120, 240, 120),
            1,
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
    log_every = max(1, _env_int("VISUALIZER_LOG_EVERY", 10))
    request_gui = _env_bool("VISUALIZER_GUI", False)

    consumer = Consumer(
        {
            "bootstrap.servers": brokers,
            "group.id": group_id,
            "auto.offset.reset": "latest",
        }
    )
    consumer.subscribe([topic_in])

    log(f"[Visualizer] Subscribed to {topic_in} (group={group_id})")

    gui = None
    if request_gui:
        try:
            gui = GuiRenderer()
            log("[Visualizer] GUI mode enabled.")
        except Exception as exc:
            log(
                "[Visualizer] GUI requested but unavailable. "
                "Install opencv-python and run on a desktop session."
            )
            log(f"[Visualizer] GUI init error: {exc}")
            gui = None

    if gui is None:
        log("[Visualizer] Headless mode enabled.")

    seen = 0
    parse_failures = 0
    smoothed_dx = 0.0

    try:
        while running:
            msg = consumer.poll(0.5)
            if msg is None:
                continue

            if msg.error():
                if msg.error().code() == KafkaError._PARTITION_EOF:
                    continue
                log(f"[Visualizer] Consumer error: {msg.error()}")
                continue

            pred = decode_prediction(msg.value())
            if pred is None:
                parse_failures += 1
                if parse_failures % 25 == 1:
                    preview = msg.value()[:200]
                    log(f"[Visualizer] Could not parse payload: {preview!r}")
                continue

            seen += 1
            raw_dx = flexion_to_speed(pred.flexion)
            smoothed_dx = (SMOOTHING_ALPHA * smoothed_dx) + ((1.0 - SMOOTHING_ALPHA) * raw_dx)
            now_ns = time.time_ns()
            e2e_ms = _latency_ms(now_ns, pred.source_ts_ns)
            model_ms = _latency_ms(pred.prediction_ts_ns, pred.source_ts_ns)
            post_model_ms = _latency_ms(now_ns, pred.prediction_ts_ns)

            if gui is not None:
                if not gui.render(pred, smoothed_dx, seen, e2e_ms, model_ms, post_model_ms):
                    break
            elif seen % log_every == 0:
                log(
                    "[Visualizer] "
                    f"frame={seen} ready={int(pred.ready)} "
                    f"seq={pred.source_sequence} "
                    f"vec=[{pred.hand_state:.1f},{pred.flexion:.1f},{pred.up_down:.1f}] "
                    f"raw_dx={raw_dx:.2f} smooth_dx={smoothed_dx:.2f} "
                    f"lat_e2e_ms={_fmt_ms(e2e_ms)} "
                    f"lat_model_ms={_fmt_ms(model_ms)} "
                    f"lat_gui_ms={_fmt_ms(post_model_ms)}"
                )
    finally:
        consumer.close()
        if gui is not None:
            gui.close()
        log("[Visualizer] Done.")


if __name__ == "__main__":
    sys.exit(main() or 0)
