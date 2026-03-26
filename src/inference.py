import argparse
import json
import os
import platform
import signal
import struct
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Optional

import numpy as np

try:
    from confluent_kafka import Consumer, KafkaError, Producer
except ImportError:  # pragma: no cover
    Consumer = None
    KafkaError = None
    Producer = None

try:
    import xgboost as xgb
except ImportError:  # pragma: no cover
    xgb = None

try:
    import tl2cgen
except ImportError:  # pragma: no cover
    tl2cgen = None

try:
    import joblib
except ImportError:  # pragma: no cover
    joblib = None


US_MAGIC = b"US"
US_PROTOCOL_VERSION = 1
US_MESSAGE_TYPE_RAW = 1
US_FRAME_HEADER = struct.Struct("<2sBBqQIIB3sI")


@dataclass(frozen=True)
class RawFrameEnvelope:
    timestamp_ns: int
    sequence_number: int
    device_id: int
    num_samples: int
    sample_format: int
    payload_length: int
    samples: np.ndarray


def decode_raw_frame_envelope(message: bytes) -> RawFrameEnvelope:
    if len(message) < US_FRAME_HEADER.size:
        raise ValueError(f"message too small ({len(message)} bytes)")

    (
        magic,
        version,
        message_type,
        timestamp_ns,
        sequence_number,
        device_id,
        num_samples,
        sample_format,
        _reserved,
        payload_length,
    ) = US_FRAME_HEADER.unpack_from(message, 0)

    if magic != US_MAGIC:
        raise ValueError("invalid US magic")
    if version != US_PROTOCOL_VERSION:
        raise ValueError(f"unsupported protocol version {version}")
    if message_type != US_MESSAGE_TYPE_RAW:
        raise ValueError(f"unexpected message_type {message_type} (expected raw=1)")

    expected_total = US_FRAME_HEADER.size + payload_length
    if len(message) < expected_total:
        raise ValueError(f"truncated frame: have={len(message)} expected>={expected_total}")

    payload = message[US_FRAME_HEADER.size:expected_total]

    if sample_format == 8:
        expected_payload = num_samples
        if payload_length != expected_payload:
            raise ValueError(
                f"payload_length mismatch for 8-bit: payload_length={payload_length}, num_samples={num_samples}"
            )
        samples = np.frombuffer(payload, dtype=np.uint8, count=num_samples)
    elif sample_format == 16:
        expected_payload = num_samples * 2
        if payload_length != expected_payload:
            raise ValueError(
                f"payload_length mismatch for 16-bit: payload_length={payload_length}, num_samples={num_samples}"
            )
        samples = np.frombuffer(payload, dtype="<u2", count=num_samples)
    else:
        raise ValueError(f"unsupported sample_format={sample_format}")

    return RawFrameEnvelope(
        timestamp_ns=int(timestamp_ns),
        sequence_number=int(sequence_number),
        device_id=int(device_id),
        num_samples=int(num_samples),
        sample_format=int(sample_format),
        payload_length=int(payload_length),
        samples=samples.astype(np.float32, copy=False),
    )


def _default_model_json_path() -> Path:
    repo_root = Path(__file__).resolve().parent.parent
    return (
        repo_root
        / "inference"
        / "models"
        / "binary_classifier"
        / "xgb_binary_open_close_hand_model.json"
    )


def _default_model_lib_path() -> Path:
    repo_root = Path(__file__).resolve().parent.parent
    ext = ".dll"
    system = platform.system().lower()
    if "linux" in system:
        ext = ".so"
    elif "darwin" in system:
        ext = ".dylib"
    return (
        repo_root
        / "inference"
        / "models"
        / "binary_classifier"
        / f"xgb_binary_open_close_hand_model{ext}"
    )


def _default_model_bundle_path() -> Path:
    repo_root = Path(__file__).resolve().parent.parent
    return (
        repo_root
        / "inference"
        / "models"
        / "binary_classifier"
        / "binary_open_close_pipeline.joblib"
    )


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


def _env_bool(name: str, default: bool) -> bool:
    value = os.getenv(name)
    if value is None or len(value.strip()) == 0:
        return default
    return value.strip().lower() in {"1", "true", "yes", "y", "on"}


def _to_optional_float(value) -> Optional[float]:
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


ANSI_RESET = "\033[0m"
ANSI_GREEN = "\033[32m"
ANSI_RED = "\033[31m"


def _format_hand_state_colored(hand_state: int) -> str:
    if int(hand_state) > 0:
        return f"{ANSI_RED}closed{ANSI_RESET}"
    return f"{ANSI_GREEN}open{ANSI_RESET}"


class OnlineBinaryFeaturePipeline:
    def __init__(self, scaler, pca, cfg: Dict[str, Any]):
        self.scaler = scaler
        self.pca = pca

        self.crop_first = int(cfg.get("crop_first", 100))
        self.crop_last = int(cfg.get("crop_last", 100))
        self.unstable_remove = int(cfg.get("unstable_remove", 20))
        self.win = int(cfg.get("win", 10))
        self.step = int(cfg.get("step", 5))
        self.b_thresh = float(cfg.get("b_thresh", cfg.get("b", 3.0)))

        sigma = float(cfg.get("gauss_sigma", 2.0))
        self.kernel = self._gaussian_kernel1d(sigma).astype(np.float32)
        self.pad = len(self.kernel) // 2

        self.depth_slice = None
        self.starts = None
        self.feature_dim = None

    def init_for_depth(self, raw_depth: int) -> None:
        left = self.crop_first + self.unstable_remove
        right = int(raw_depth) - self.crop_last - self.unstable_remove
        if right <= left:
            raise ValueError("Cropping removes all depth bins.")

        depth_final = right - left
        if depth_final < self.win:
            raise ValueError("Final depth smaller than sliding window.")

        self.depth_slice = slice(left, right)
        self.starts = np.arange(0, depth_final - self.win + 1, self.step, dtype=np.int32)
        self.feature_dim = len(self.starts) * 3

    @staticmethod
    def _gaussian_kernel1d(sigma: float, radius: Optional[int] = None) -> np.ndarray:
        sigma = float(sigma)
        if sigma <= 0:
            raise ValueError("sigma must be > 0")
        if radius is None:
            radius = int(np.ceil(3 * sigma))
        x = np.arange(-radius, radius + 1, dtype=np.float32)
        k = np.exp(-(x * x) / (2.0 * sigma * sigma)).astype(np.float32)
        k /= (k.sum() + 1e-12)
        return k

    @staticmethod
    def _sigmoid(x: np.ndarray) -> np.ndarray:
        return 1.0 / (1.0 + np.exp(-x))

    def transform(self, frame: np.ndarray) -> np.ndarray:
        x = np.asarray(frame, dtype=np.float32).reshape(-1)
        if self.depth_slice is None or self.starts is None:
            self.init_for_depth(x.size)

        assert self.depth_slice is not None
        assert self.starts is not None
        assert self.feature_dim is not None

        x = x[self.depth_slice]
        xpad = np.pad(x, (self.pad, self.pad), mode="reflect")
        x = np.convolve(xpad, self.kernel, mode="valid").astype(np.float32)

        feats = np.empty(self.feature_dim, dtype=np.float32)
        j = 0
        for s in self.starts:
            w = x[s:s + self.win]
            m = w.mean(dtype=np.float32)
            v = ((w - m) ** 2).mean(dtype=np.float32)
            energy = np.sqrt((w * w).sum(dtype=np.float32), dtype=np.float32)
            es = self._sigmoid(energy - self.b_thresh)
            feats[j] = m
            feats[j + 1] = v
            feats[j + 2] = es
            j += 3

        feats = self.scaler.transform(feats.reshape(1, -1)).astype(np.float32)
        if self.pca is not None:
            feats = self.pca.transform(feats).astype(np.float32)
        return feats


class BinaryInferenceEngine:
    def __init__(
        self,
        model_json_path: Optional[str],
        model_lib_path: Optional[str],
        model_bundle_path: Optional[str],
        input_dim: int,
        normalize_frame: bool = False,
        backend_preference: str = "bundle_first",
    ):
        self.model_json_path = Path(model_json_path).expanduser() if model_json_path else None
        self.model_lib_path = Path(model_lib_path).expanduser() if model_lib_path else None
        self.model_bundle_path = Path(model_bundle_path).expanduser() if model_bundle_path else None
        self.input_dim = max(1, int(input_dim))
        self.normalize_frame = bool(normalize_frame)
        pref = str(backend_preference).strip().lower()
        self.backend_preference = pref if pref in {"bundle_first", "legacy_first"} else "bundle_first"

        self._backend = None
        self._backend_name = ""
        self._bundle_model = None
        self._bundle_pipeline = None
        self._bundle_closed_class = 0
        self._load_backend()

    @property
    def backend_name(self) -> str:
        return self._backend_name

    def _load_backend(self) -> None:
        if self.backend_preference == "legacy_first":
            load_order = ("json", "lib", "bundle")
        else:
            load_order = ("bundle", "json", "lib")

        for backend in load_order:
            if backend == "json" and self.model_json_path is not None and self.model_json_path.exists():
                if xgb is None:
                    raise ImportError(
                        "xgboost is required for JSON model inference. Install with: pip install xgboost"
                    )
                booster = xgb.Booster()
                booster.load_model(str(self.model_json_path))
                self._backend = booster
                self._backend_name = "xgboost_json"
                return

            if backend == "lib" and self.model_lib_path is not None and self.model_lib_path.exists():
                if tl2cgen is None:
                    raise ImportError(
                        "tl2cgen is required for shared-library inference. Install with: pip install tl2cgen"
                    )
                self._backend = tl2cgen.Predictor(str(self.model_lib_path), nthread=1)
                self._backend_name = "tl2cgen_lib"
                return

            if backend == "bundle" and self.model_bundle_path is not None and self.model_bundle_path.exists():
                if joblib is None:
                    raise ImportError(
                        "joblib is required for pipeline-bundle inference. Install with: pip install joblib"
                    )
                bundle = joblib.load(self.model_bundle_path)
                if not isinstance(bundle, dict):
                    raise ValueError(
                        "MODEL_BUNDLE_PATH must point to a joblib dict bundle with keys: "
                        "'model', 'scaler', 'pca', and 'preprocess_config'."
                    )
                model = bundle.get("model")
                scaler = bundle.get("scaler")
                pca = bundle.get("pca")
                cfg = bundle.get("preprocess_config")
                if model is None or scaler is None or cfg is None:
                    raise ValueError(
                        "Invalid model bundle. Expected keys: 'model', 'scaler', and 'preprocess_config'."
                    )
                self._bundle_model = model
                self._bundle_pipeline = OnlineBinaryFeaturePipeline(scaler=scaler, pca=pca, cfg=cfg)
                class_mapping = bundle.get("class_mapping", {0: "closed", 1: "open"})
                self._bundle_closed_class = self._resolve_closed_class(class_mapping)
                self._backend = bundle
                self._backend_name = "joblib_bundle"
                return

        raise FileNotFoundError(
            "No binary-classifier model artifact found. "
            "Checked model_json_path="
            f"{self.model_json_path}, model_lib_path={self.model_lib_path}, "
            f"and model_bundle_path={self.model_bundle_path}"
        )

    @staticmethod
    def _resolve_closed_class(class_mapping: Any) -> Any:
        if isinstance(class_mapping, dict):
            for key, value in class_mapping.items():
                if str(value).strip().lower() == "closed":
                    return key
        return 0

    def _frame_to_features(self, frame: np.ndarray) -> np.ndarray:
        x = np.asarray(frame, dtype=np.float32).reshape(-1)
        if x.size == 0:
            return np.zeros((1, self.input_dim), dtype=np.float32)

        if self.normalize_frame:
            mu = float(x.mean(dtype=np.float32))
            sigma = float(x.std(dtype=np.float32))
            if sigma < 1e-6:
                sigma = 1.0
            x = (x - mu) / sigma

        if x.size != self.input_dim:
            src_idx = np.arange(x.size, dtype=np.float32)
            dst_idx = np.linspace(0.0, float(x.size - 1), self.input_dim, dtype=np.float32)
            x = np.interp(dst_idx, src_idx, x).astype(np.float32)

        return x.reshape(1, -1).astype(np.float32, copy=False)

    def predict_probability(self, frame: np.ndarray) -> float:
        features = self._frame_to_features(frame)

        if self._backend_name == "xgboost_json":
            preds = self._backend.predict(xgb.DMatrix(features))
        elif self._backend_name == "tl2cgen_lib":
            preds = self._backend.predict(tl2cgen.DMatrix(features))
        elif self._backend_name == "joblib_bundle":
            assert self._bundle_model is not None and self._bundle_pipeline is not None
            bundle_features = self._bundle_pipeline.transform(frame)
            probs = np.asarray(self._bundle_model.predict_proba(bundle_features), dtype=np.float32).reshape(-1)
            classes = np.asarray(getattr(self._bundle_model, "classes_", []))
            if classes.size == probs.size and classes.size > 0:
                target = str(self._bundle_closed_class)
                idx = None
                for i, cls in enumerate(classes):
                    if str(cls) == target:
                        idx = i
                        break
                if idx is None:
                    idx = 0
                return float(probs[idx])
            idx = int(self._bundle_closed_class) if str(self._bundle_closed_class).isdigit() else 0
            idx = min(max(idx, 0), max(0, probs.size - 1))
            return float(probs[idx])
        else:
            raise RuntimeError("Inference backend is not initialized.")

        return float(np.asarray(preds).reshape(-1)[0])


class LiveKafkaBinaryInferenceWorker:
    def __init__(
        self,
        bootstrap_servers: str,
        topic_in: str = "ultrasound_raw_data",
        topic_out: str = "hand_state_predictions",
        consumer_group_id: str = "binary_inference_group",
        auto_offset_reset: str = "latest",
        model_json_path: Optional[str] = None,
        model_lib_path: Optional[str] = None,
        model_bundle_path: Optional[str] = None,
        input_dim: int = 200,
        normalize_frame: bool = False,
        threshold: float = 0.5,
        probability_positive_class: str = "closed",
        backend_preference: str = "bundle_first",
        poll_timeout_s: float = 0.5,
        log_every: int = 100,
        prediction_log_every: int = 50,
    ):
        if Consumer is None or Producer is None:
            raise ImportError(
                "confluent-kafka is required. Install with: pip install confluent-kafka"
            )

        self.bootstrap_servers = str(bootstrap_servers)
        self.topic_in = str(topic_in)
        self.topic_out = str(topic_out)
        self.consumer_group_id = str(consumer_group_id)
        self.auto_offset_reset = str(auto_offset_reset)
        self.threshold = float(threshold)
        cls = str(probability_positive_class).strip().lower()
        self.probability_positive_class = cls if cls in {"closed", "open"} else "closed"
        self.poll_timeout_s = float(poll_timeout_s)
        self.log_every = max(1, int(log_every))
        self.prediction_log_every = max(0, int(prediction_log_every))

        self.engine = BinaryInferenceEngine(
            model_json_path=model_json_path,
            model_lib_path=model_lib_path,
            model_bundle_path=model_bundle_path,
            input_dim=input_dim,
            normalize_frame=normalize_frame,
            backend_preference=backend_preference,
        )

        self.running = True
        self.total_frames = 0
        self.total_errors = 0

    def stop(self) -> None:
        self.running = False

    def _build_prediction_payload(
        self,
        frame: RawFrameEnvelope,
        model_probability: float,
        infer_ms: float,
    ) -> bytes:
        model_probability = max(0.0, min(1.0, float(model_probability)))
        closed_probability = (
            model_probability
            if self.probability_positive_class == "closed"
            else (1.0 - model_probability)
        )
        prediction_ts_ns = int(time.time_ns())
        hand_state = 1.0 if closed_probability >= self.threshold else 0.0
        confidence = max(0.0, min(1.0, 2.0 * abs(closed_probability - 0.5)))
        payload = {
            "source_sequence": int(frame.sequence_number),
            "source_ts_ns": int(frame.timestamp_ns),
            "prediction_ts_ns": prediction_ts_ns,
            "label": "hand_closed" if hand_state > 0.5 else "hand_open",
            "confidence": float(confidence),
            "prediction": float(closed_probability),
            "binary_probability": float(closed_probability),
            "model_probability_raw": float(model_probability),
            "probability_positive_class": self.probability_positive_class,
            "hand_state": float(hand_state),
            "ready": True,
            "infer_time_ms": float(infer_ms),
            "num_samples": int(frame.num_samples),
            "model_name": "binary_open_close_classifier",
            "backend": self.engine.backend_name,
        }
        return json.dumps(payload, separators=(",", ":"), ensure_ascii=True).encode("utf-8")

    def _produce_prediction(self, producer: Producer, payload: bytes) -> None:
        while self.running:
            try:
                producer.produce(self.topic_out, value=payload)
                producer.poll(0)
                return
            except BufferError:
                producer.poll(0.05)

    def run(self) -> None:
        consumer = Consumer(
            {
                "bootstrap.servers": self.bootstrap_servers,
                "group.id": self.consumer_group_id,
                "auto.offset.reset": self.auto_offset_reset,
            }
        )
        producer = Producer({"bootstrap.servers": self.bootstrap_servers})
        consumer.subscribe([self.topic_in])

        print(f"[BinaryInference] Subscribed to {self.topic_in}", flush=True)
        print(f"[BinaryInference] Publishing predictions to {self.topic_out}", flush=True)
        print(f"[BinaryInference] Backend: {self.engine.backend_name}", flush=True)
        print(f"[BinaryInference] Backend preference: {self.engine.backend_preference}", flush=True)
        print(f"[BinaryInference] Normalize frame: {int(self.engine.normalize_frame)}", flush=True)
        print(f"[BinaryInference] Threshold: {self.threshold:.3f}", flush=True)
        print(
            "[BinaryInference] Probability positive class: "
            f"{self.probability_positive_class}",
            flush=True,
        )
        if self.engine.backend_name != "joblib_bundle":
            print(
                "[BinaryInference][Warn] Using legacy backend without bundle preprocessing. "
                "If model was trained with scaler/PCA feature pipeline, predictions may degrade.",
                flush=True,
            )
        if self.prediction_log_every > 0:
            print(
                "[BinaryInference] Prediction logging enabled: "
                f"every {self.prediction_log_every} frame(s)",
                flush=True,
            )

        try:
            while self.running:
                msg = consumer.poll(self.poll_timeout_s)
                if msg is None:
                    continue

                if msg.error():
                    if KafkaError is not None and msg.error().code() == KafkaError._PARTITION_EOF:
                        continue
                    self.total_errors += 1
                    print(f"[BinaryInference] Consumer error: {msg.error()}", flush=True)
                    continue

                try:
                    envelope = decode_raw_frame_envelope(msg.value())
                    t0 = time.perf_counter()
                    model_probability = self.engine.predict_probability(envelope.samples)
                    infer_ms = (time.perf_counter() - t0) * 1000.0
                    payload = self._build_prediction_payload(envelope, model_probability, infer_ms)
                    self._produce_prediction(producer, payload)
                except Exception as exc:
                    self.total_errors += 1
                    if self.total_errors % 25 == 1:
                        print(f"[BinaryInference] Dropping frame due to error: {exc}", flush=True)
                    continue

                self.total_frames += 1
                if self.prediction_log_every > 0 and self.total_frames % self.prediction_log_every == 0:
                    closed_probability = (
                        model_probability
                        if self.probability_positive_class == "closed"
                        else (1.0 - model_probability)
                    )
                    hand_state = 1 if closed_probability >= self.threshold else 0
                    state_text = "closed" if hand_state > 0 else "open"
                    line_color = ANSI_RED if hand_state > 0 else ANSI_GREEN
                    line = (
                        "[BinaryInference][Prediction] "
                        f"seq={envelope.sequence_number} "
                        f"closed_prob={closed_probability:.6f} "
                        f"raw_prob={model_probability:.6f} "
                        f"hand_state={hand_state}({state_text}) "
                        f"infer_ms={infer_ms:.3f}"
                    )
                    print(f"{line_color}{line}{ANSI_RESET}", flush=True)
                elif self.total_frames % self.log_every == 0:
                    print(
                        "[BinaryInference] "
                        f"frames={self.total_frames} "
                        f"last_raw_prob={model_probability:.6f} "
                        f"errors={self.total_errors}",
                        flush=True,
                    )
        finally:
            consumer.close()
            producer.flush(5.0)
            print(
                "[BinaryInference] Done. "
                f"processed_frames={self.total_frames} "
                f"errors={self.total_errors}",
                flush=True,
            )


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Binary hand-state inference from raw Kafka frames")
    parser.add_argument(
        "--bootstrap-servers",
        default=_env_or_default("BOOTSTRAP_SERVERS", "localhost:9092"),
    )
    parser.add_argument("--topic-in", default=_env_or_default("TOPIC_IN", "ultrasound_raw_data"))
    parser.add_argument("--topic-out", default=_env_or_default("TOPIC_OUT", "hand_state_predictions"))
    parser.add_argument(
        "--consumer-group-id",
        default=_env_or_default("CONSUMER_GROUP_ID", "binary_inference_group"),
    )
    parser.add_argument(
        "--auto-offset-reset",
        choices=["earliest", "latest"],
        default=_env_or_default("AUTO_OFFSET_RESET", "latest"),
    )
    parser.add_argument(
        "--model-json-path",
        default=_env_or_default("MODEL_JSON_PATH", str(_default_model_json_path())),
    )
    parser.add_argument(
        "--model-lib-path",
        default=_env_or_default("MODEL_LIB_PATH", str(_default_model_lib_path())),
    )
    parser.add_argument(
        "--model-bundle-path",
        default=_env_or_default("MODEL_BUNDLE_PATH", str(_default_model_bundle_path())),
        help="Path to sklearn/joblib binary bundle (preferred when JSON/SO artifacts are unavailable).",
    )
    parser.add_argument(
        "--backend-preference",
        choices=["bundle_first", "legacy_first"],
        default=_env_or_default("BACKEND_PREFERENCE", "bundle_first"),
        help="Backend load order. 'bundle_first' avoids preprocessing mismatch when bundle exists.",
    )
    parser.add_argument("--input-dim", type=int, default=_env_int("INPUT_DIM", 200))
    parser.add_argument(
        "--normalize-frame",
        action=argparse.BooleanOptionalAction,
        default=_env_bool("NORMALIZE_FRAME", False),
    )
    parser.add_argument("--threshold", type=float, default=_env_float("THRESHOLD", 0.5))
    parser.add_argument(
        "--probability-positive-class",
        choices=["closed", "open"],
        default=_env_or_default("PROBABILITY_POSITIVE_CLASS", "closed"),
        help="Interpretation of model output probability before thresholding hand_state.",
    )
    parser.add_argument("--poll-timeout-s", type=float, default=_env_float("POLL_TIMEOUT_S", 0.5))
    parser.add_argument("--log-every", type=int, default=_env_int("LOG_EVERY", 100))
    parser.add_argument(
        "--prediction-log-every",
        type=int,
        default=_env_int("PREDICTION_LOG_EVERY", 50),
        help="Emit per-prediction logs every N frames (0 disables).",
    )
    return parser


def main() -> None:
    args = build_arg_parser().parse_args()
    worker = LiveKafkaBinaryInferenceWorker(
        bootstrap_servers=args.bootstrap_servers,
        topic_in=args.topic_in,
        topic_out=args.topic_out,
        consumer_group_id=args.consumer_group_id,
        auto_offset_reset=args.auto_offset_reset,
        model_json_path=args.model_json_path,
        model_lib_path=args.model_lib_path,
        model_bundle_path=args.model_bundle_path,
        backend_preference=args.backend_preference,
        input_dim=args.input_dim,
        normalize_frame=args.normalize_frame,
        threshold=args.threshold,
        probability_positive_class=args.probability_positive_class,
        poll_timeout_s=args.poll_timeout_s,
        log_every=args.log_every,
        prediction_log_every=args.prediction_log_every,
    )

    def _signal_handler(signum, _frame):
        print(f"[BinaryInference] Shutdown signal {signum} received...", flush=True)
        worker.stop()

    signal.signal(signal.SIGINT, _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)
    worker.run()


if __name__ == "__main__":
    main()
