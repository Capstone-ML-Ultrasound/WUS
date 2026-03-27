import argparse
import json
import os
import signal
import struct
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Tuple

import numpy as np

try:
    from confluent_kafka import Consumer, KafkaError, Producer
except ImportError:  # Allows CSV mode without Kafka dependencies.
    Consumer = None
    KafkaError = None
    Producer = None


US_MAGIC = b"US"
US_PROTOCOL_VERSION = 1
US_MESSAGE_TYPE_RAW = 1
US_FRAME_HEADER = struct.Struct("<2sBBqQIIB3sI")  # packed C++ USFrameHeader (36 bytes)


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
        raise ValueError(
            f"truncated frame: have={len(message)} expected>={expected_total}"
        )

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


class OnlineDepthCalibrator:
    def __init__(self, depth, eps=1e-6, freeze_after=None):
        self.depth = int(depth)
        self.eps = float(eps)
        self.freeze_after = None if freeze_after is None else int(freeze_after)

        self.n = 0
        self.mean = np.zeros(self.depth, dtype=np.float32)
        self.M2 = np.zeros(self.depth, dtype=np.float32)

    def update(self, frame: np.ndarray):
        if self.freeze_after is not None and self.n >= self.freeze_after:
            return

        x = np.asarray(frame, dtype=np.float32)
        if x.shape != (self.depth,):
            raise ValueError(f"Expected frame shape ({self.depth},), got {x.shape}")

        self.n += 1
        delta = x - self.mean
        self.mean += delta / self.n
        delta2 = x - self.mean
        self.M2 += delta * delta2

    def ready(self, min_frames=30):
        return self.n >= int(min_frames)

    def get_mean_std(self):
        if self.n < 2:
            std = np.ones(self.depth, dtype=np.float32)
        else:
            var = self.M2 / max(self.n - 1, 1)
            std = np.sqrt(np.maximum(var, self.eps)).astype(np.float32)
        return self.mean.astype(np.float32), std


class CalibrationController:
    SUPPORTED_MODES = {"fixed", "warmup_freeze", "continuous"}

    def __init__(
        self,
        depth: int,
        mode: str = "warmup_freeze",
        warmup_frames: int = 50,
        freeze_after: int = 50,
        fixed_mean: Optional[np.ndarray] = None,
        fixed_std: Optional[np.ndarray] = None,
        eps: float = 1e-6,
    ):
        self.depth = int(depth)
        self.mode = str(mode)
        self.warmup_frames = int(warmup_frames)
        self.freeze_after = int(freeze_after)
        self.eps = float(eps)

        if self.mode not in self.SUPPORTED_MODES:
            raise ValueError(
                f"Unsupported calibration mode '{self.mode}'. Supported: {sorted(self.SUPPORTED_MODES)}"
            )
        if self.warmup_frames < 0:
            raise ValueError("warmup_frames must be >= 0")
        if self.freeze_after < 0:
            raise ValueError("freeze_after must be >= 0")
        if self.mode == "warmup_freeze" and self.freeze_after < self.warmup_frames:
            raise ValueError(
                "freeze_after must be >= warmup_frames for warmup_freeze mode, "
                f"got freeze_after={self.freeze_after}, warmup_frames={self.warmup_frames}"
            )

        self._calibrator: Optional[OnlineDepthCalibrator] = None
        self._cached_mean: Optional[np.ndarray] = None
        self._cached_std: Optional[np.ndarray] = None

        if self.mode == "fixed":
            if fixed_mean is None or fixed_std is None:
                self._cached_mean = np.zeros(self.depth, dtype=np.float32)
                self._cached_std = np.ones(self.depth, dtype=np.float32)
            else:
                mean = np.asarray(fixed_mean, dtype=np.float32)
                std = np.asarray(fixed_std, dtype=np.float32)
                if mean.shape != (self.depth,) or std.shape != (self.depth,):
                    raise ValueError(
                        f"Fixed calibration shape mismatch. Expected ({self.depth},), got mean={mean.shape} std={std.shape}"
                    )
                self._cached_mean = mean
                self._cached_std = np.maximum(std, self.eps).astype(np.float32)
        else:
            freeze = self.freeze_after if self.mode == "warmup_freeze" else None
            self._calibrator = OnlineDepthCalibrator(
                depth=self.depth,
                eps=self.eps,
                freeze_after=freeze,
            )

    @property
    def calibration_n(self) -> int:
        if self._calibrator is None:
            return 0
        return int(self._calibrator.n)

    @property
    def is_frozen(self) -> bool:
        if self.mode == "fixed":
            return True
        if self.mode == "warmup_freeze":
            return self.calibration_n >= self.freeze_after
        return False

    def update(self, frame: np.ndarray):
        if self._calibrator is None:
            return
        self._calibrator.update(frame)
        if (
            self.mode == "warmup_freeze"
            and self.is_frozen
            and (self._cached_mean is None or self._cached_std is None)
        ):
            self._cached_mean, self._cached_std = self._calibrator.get_mean_std()

    def ready(self) -> bool:
        if self.mode == "fixed":
            return True
        return self.calibration_n >= self.warmup_frames

    def get_mean_std(self) -> Tuple[np.ndarray, np.ndarray]:
        if self._cached_mean is not None and self._cached_std is not None:
            return self._cached_mean, self._cached_std
        if self._calibrator is None:
            raise RuntimeError("Calibration is fixed but no fixed statistics are set.")
        return self._calibrator.get_mean_std()


class OnlineFeaturePipeline:
    def __init__(
        self,
        depth_mean,
        depth_std,
        scaler,
        pca,
        crop_first=100,
        crop_last=100,
        unstable_remove=20,
        gauss_sigma=2.0,
        win=10,
        step=5,
        b=3.0,
        include_mean=True,
        include_var=True,
        include_energy_sigmoid=True,
        eps=1e-6,
    ):
        self.depth_mean = None if depth_mean is None else np.asarray(depth_mean, dtype=np.float32)
        self.depth_std = None if depth_std is None else np.asarray(depth_std, dtype=np.float32)
        self.scaler = scaler
        self.pca = pca

        self.crop_first = int(crop_first)
        self.crop_last = int(crop_last)
        self.unstable_remove = int(unstable_remove)
        self.win = int(win)
        self.step = int(step)
        self.b = float(b)
        self.include_mean = bool(include_mean)
        self.include_var = bool(include_var)
        self.include_energy_sigmoid = bool(include_energy_sigmoid)
        self.eps = float(eps)

        self.kernel = self._gaussian_kernel1d(gauss_sigma).astype(np.float32)
        self.pad = len(self.kernel) // 2

        if self.depth_mean is not None:
            d0 = self.depth_mean.shape[0]
        elif self.depth_std is not None:
            d0 = self.depth_std.shape[0]
        else:
            raise ValueError("Need depth_mean/depth_std or set them later after construction.")

        left = self.crop_first + self.unstable_remove
        right = d0 - self.crop_last - self.unstable_remove
        if right <= left:
            raise ValueError("Cropping removes all depth bins.")

        self.depth_slice = slice(left, right)
        self.D_final = right - left

        if self.D_final < self.win:
            raise ValueError("Final depth smaller than sliding window.")

        self.starts = np.arange(0, self.D_final - self.win + 1, self.step, dtype=np.int32)

        self.n_features_per_window = (
            int(self.include_mean)
            + int(self.include_var)
            + int(self.include_energy_sigmoid)
        )
        if self.n_features_per_window == 0:
            raise ValueError("No features enabled.")

        self.feature_dim = len(self.starts) * self.n_features_per_window

    def set_calibration(self, depth_mean, depth_std):
        self.depth_mean = np.asarray(depth_mean, dtype=np.float32)
        self.depth_std = np.asarray(depth_std, dtype=np.float32)

    @staticmethod
    def _gaussian_kernel1d(sigma, radius=None):
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
    def _sigmoid(x):
        return 1.0 / (1.0 + np.exp(-x))

    def _normalize_per_depth(self, frame):
        if self.depth_mean is None or self.depth_std is None:
            raise RuntimeError("Calibration not set yet.")
        return (frame - self.depth_mean) / (self.depth_std + self.eps)

    def _crop_and_trim(self, frame):
        return frame[self.depth_slice]

    def _gaussian_filter_depth(self, frame):
        xpad = np.pad(frame, (self.pad, self.pad), mode="reflect")
        return np.convolve(xpad, self.kernel, mode="valid").astype(np.float32)

    def _extract_features(self, frame):
        # Vectorized sliding windows to avoid per-window Python overhead.
        windows = np.lib.stride_tricks.sliding_window_view(frame, self.win)[self.starts]
        parts = []

        if self.include_mean:
            means = windows.mean(axis=1, dtype=np.float32)
            parts.append(means)
        if self.include_var:
            variances = windows.var(axis=1, dtype=np.float32)
            parts.append(variances)
        if self.include_energy_sigmoid:
            energy = np.sqrt(np.sum(windows * windows, axis=1, dtype=np.float32), dtype=np.float32)
            energy_sigmoid = self._sigmoid(energy - self.b).astype(np.float32, copy=False)
            parts.append(energy_sigmoid)

        if not parts:
            raise ValueError("No features enabled.")

        feats = np.column_stack(parts).reshape(-1).astype(np.float32, copy=False)
        if feats.shape[0] != self.feature_dim:
            raise RuntimeError(
                f"Feature dim mismatch: expected={self.feature_dim}, got={feats.shape[0]}"
            )
        return feats

    def transform_frame_to_model_input(self, frame):
        x = np.asarray(frame, dtype=np.float32)
        x = self._normalize_per_depth(x)
        x = self._crop_and_trim(x)
        x = self._gaussian_filter_depth(x)
        feats = self._extract_features(x).reshape(1, -1)

        feats = self.scaler.transform(feats).astype(np.float32)
        if self.pca is not None:
            feats = self.pca.transform(feats).astype(np.float32)
        return feats

    def predict_frame(self, frame, model):
        x_model = self.transform_frame_to_model_input(frame)
        pred = model.predict(x_model)
        return float(pred[0])


def load_artifacts(model_dir: str):
    import joblib

    model_dir = Path(model_dir)

    cfg = json.loads((model_dir / "best_config.json").read_text(encoding="utf-8"))
    model = joblib.load(model_dir / "best_model.joblib")
    scaler = joblib.load(model_dir / "best_scaler.joblib")

    pca_path = model_dir / "best_pca.joblib"
    pca = joblib.load(pca_path) if pca_path.exists() else None

    return cfg, model, scaler, pca


def build_pipeline(model_dir: str, raw_depth: int):
    cfg, model, scaler, pca = load_artifacts(model_dir)

    dummy_mean = np.zeros(raw_depth, dtype=np.float32)
    dummy_std = np.ones(raw_depth, dtype=np.float32)

    pipeline = OnlineFeaturePipeline(
        depth_mean=dummy_mean,
        depth_std=dummy_std,
        scaler=scaler,
        pca=pca,
        crop_first=cfg["crop_first"],
        crop_last=cfg["crop_last"],
        unstable_remove=cfg["unstable_remove"],
        gauss_sigma=cfg["gauss_sigma"],
        win=cfg["win"],
        step=cfg["step"],
        b=cfg["b"],
        include_mean=cfg["include_mean"],
        include_var=cfg["include_var"],
        include_energy_sigmoid=cfg.get("include_energy_sigmoid", True),
    )

    return pipeline, model, cfg


def run_session_inference(
    model_dir,
    csv_path,
    calibration_mode="warmup_freeze",
    warmup_frames=50,
    freeze_after=50,
    out_dir=None,
):
    csv_path = Path(csv_path)
    out_dir = Path(out_dir) if out_dir is not None else csv_path.parent / f"inference_{csv_path.stem}"
    out_dir.mkdir(parents=True, exist_ok=True)

    # sniff depth cheaply before loading artifacts into the pipeline
    # load once to get the true matrix shape:
    # rows = depth bins, cols = frames
    data_preview = np.loadtxt(csv_path, delimiter=",", dtype=np.float32)
    if data_preview.ndim != 2:
        raise ValueError(f"Expected 2D CSV, got shape {data_preview.shape}")

    raw_depth = data_preview.shape[0]

    pipeline, model, cfg = build_pipeline(model_dir=model_dir, raw_depth=raw_depth)
    calibration = CalibrationController(
        depth=raw_depth,
        mode=calibration_mode,
        warmup_frames=warmup_frames,
        freeze_after=freeze_after,
    )

    print("loaded config:")
    print(json.dumps(cfg, indent=2))
    print(f"raw depth: {raw_depth}")
    print(f"feature dim before scaler/pca: {pipeline.feature_dim}")
    print(f"calibration_mode: {calibration_mode}")
    print(f"warmup_frames: {warmup_frames}")
    print(f"freeze_after: {freeze_after}")

    predictions = []
    infer_times_ms = []
    calib_counts = []
    frame_indices = []

    _, n_frames = data_preview.shape
    for frame_idx in range(n_frames):
        frame = data_preview[:, frame_idx].astype(np.float32, copy=False)
        calibration.update(frame)
        calib_counts.append(calibration.calibration_n)
        frame_indices.append(frame_idx)

        if not calibration.ready():
            predictions.append(np.nan)
            infer_times_ms.append(np.nan)
            if frame_idx % 100 == 0:
                print(f"frame {frame_idx:06d} | calibrating... n={calibration.calibration_n}")
            continue

        depth_mean, depth_std = calibration.get_mean_std()
        pipeline.set_calibration(depth_mean, depth_std)

        t0 = time.perf_counter()
        pred = pipeline.predict_frame(frame, model)
        t1 = time.perf_counter()

        dt_ms = (t1 - t0) * 1000.0
        predictions.append(pred)
        infer_times_ms.append(dt_ms)

        if frame_idx % 100 == 0:
            print(
                f"frame {frame_idx:06d} | pred={pred:.6f} | "
                f"infer_time_ms={dt_ms:.3f} | calib_n={calibration.calibration_n} | frozen={calibration.is_frozen}"
            )

    predictions = np.asarray(predictions, dtype=np.float32)
    infer_times_ms = np.asarray(infer_times_ms, dtype=np.float32)
    frame_indices = np.asarray(frame_indices, dtype=np.int32)
    calib_counts = np.asarray(calib_counts, dtype=np.int32)

    pred_csv = out_dir / "predictions.csv"
    with pred_csv.open("w", encoding="utf-8") as f:
        f.write("frame_idx,prediction,infer_time_ms,calib_n\n")
        for i, pred, dt, n in zip(frame_indices, predictions, infer_times_ms, calib_counts):
            pred_str = "" if np.isnan(pred) else f"{pred:.8f}"
            dt_str = "" if np.isnan(dt) else f"{dt:.6f}"
            f.write(f"{i},{pred_str},{dt_str},{n}\n")

    pred_png = out_dir / "predictions_over_time.png"
    try:
        import matplotlib.pyplot as plt

        plt.figure(figsize=(12, 5))
        plt.plot(frame_indices, predictions)
        plt.axvline(warmup_frames - 1, linestyle="--")
        plt.xlabel("Frame index")
        plt.ylabel("Prediction")
        plt.title("Predictions across time")
        plt.tight_layout()
        plt.savefig(pred_png, dpi=150)
        plt.close()
    except ImportError:
        print("matplotlib not installed; skipping prediction plot export")

    valid = ~np.isnan(predictions)
    if np.any(valid):
        print(
            "done | "
            f"frames={len(predictions)} | "
            f"valid_preds={int(valid.sum())} | "
            f"mean_pred={float(np.nanmean(predictions)):.6f} | "
            f"mean_infer_ms={float(np.nanmean(infer_times_ms)):.4f}"
        )
    else:
        print("done | no valid predictions produced")

    print(f"saved predictions csv: {pred_csv}")
    if pred_png.exists():
        print(f"saved prediction plot: {pred_png}")


def load_fixed_calibration(path: Optional[str], raw_depth: int) -> Tuple[Optional[np.ndarray], Optional[np.ndarray]]:
    if path is None or len(path.strip()) == 0:
        return None, None

    p = Path(path)
    if not p.exists():
        raise FileNotFoundError(f"FIXED_CALIBRATION_PATH does not exist: {p}")
    if p.suffix.lower() != ".npz":
        raise ValueError(
            f"Expected .npz file with 'mean' and 'std' arrays at {p}, got suffix '{p.suffix}'"
        )

    with np.load(p, allow_pickle=False) as obj:
        if "mean" not in obj.files or "std" not in obj.files:
            raise ValueError(f"Fixed calibration NPZ must contain 'mean' and 'std' arrays: {p}")
        mean = np.asarray(obj["mean"], dtype=np.float32)
        std = np.asarray(obj["std"], dtype=np.float32)

    if mean.shape != (raw_depth,) or std.shape != (raw_depth,):
        raise ValueError(
            f"Fixed calibration shape mismatch: expected ({raw_depth},), got mean={mean.shape}, std={std.shape}"
        )
    return mean, std


class LiveKafkaInferenceWorker:
    def __init__(
        self,
        model_dir: str,
        bootstrap_servers: str,
        topic_in: str = "ultrasound_raw_data",
        topic_out: str = "wrist_predictions",
        consumer_group_id: str = "wrist_inference_group",
        auto_offset_reset: str = "latest",
        calibration_mode: str = "warmup_freeze",
        warmup_frames: int = 50,
        freeze_after: int = 50,
        fixed_calibration_path: Optional[str] = None,
        poll_timeout_s: float = 0.5,
        log_every: int = 100,
        prediction_log_every: int = 0,
    ):
        if Consumer is None or Producer is None:
            raise ImportError(
                "confluent-kafka is required for Kafka mode. Install with: pip install confluent-kafka"
            )

        self.model_dir = str(model_dir)
        self.bootstrap_servers = str(bootstrap_servers)
        self.topic_in = str(topic_in)
        self.topic_out = str(topic_out)
        self.consumer_group_id = str(consumer_group_id)
        self.auto_offset_reset = str(auto_offset_reset)
        self.calibration_mode = str(calibration_mode)
        self.warmup_frames = int(warmup_frames)
        self.freeze_after = int(freeze_after)
        self.fixed_calibration_path = fixed_calibration_path
        self.poll_timeout_s = float(poll_timeout_s)
        self.log_every = max(1, int(log_every))
        self.prediction_log_every = max(0, int(prediction_log_every))

        self.pipeline = None
        self.model = None
        self.cfg = None
        self.calibration: Optional[CalibrationController] = None
        self.raw_depth = None
        self.running = True
        self.total_frames = 0
        self.total_errors = 0

    def stop(self):
        self.running = False

    def _init_model_for_depth(self, raw_depth: int):
        self.pipeline, self.model, self.cfg = build_pipeline(
            model_dir=self.model_dir, raw_depth=raw_depth
        )
        fixed_mean, fixed_std = load_fixed_calibration(self.fixed_calibration_path, raw_depth)
        self.calibration = CalibrationController(
            depth=raw_depth,
            mode=self.calibration_mode,
            warmup_frames=self.warmup_frames,
            freeze_after=self.freeze_after,
            fixed_mean=fixed_mean,
            fixed_std=fixed_std,
        )
        self.raw_depth = raw_depth
        print("loaded config:")
        print(json.dumps(self.cfg, indent=2))
        print(f"raw depth: {raw_depth}")
        print(f"feature dim before scaler/pca: {self.pipeline.feature_dim}")
        print(f"calibration_mode: {self.calibration_mode}")
        print(f"warmup_frames: {self.warmup_frames}")
        print(f"freeze_after: {self.freeze_after}")

    def _build_prediction_payload(
        self,
        frame: RawFrameEnvelope,
        pred: Optional[float],
        infer_ms: Optional[float],
    ) -> bytes:
        prediction_ts_ns = int(time.time_ns())
        ready = bool(pred is not None)
        payload = {
            "source_sequence": int(frame.sequence_number),
            "source_ts_ns": int(frame.timestamp_ns),
            "prediction_ts_ns": prediction_ts_ns,
            "label": "wrist_regression" if ready else "calibrating",
            "confidence": 1.0 if ready else 0.0,
            "prediction": None if pred is None else float(pred),
            "infer_time_ms": None if infer_ms is None else float(infer_ms),
            "ready": ready,
            "calibration_mode": self.calibration_mode,
            "calibration_n": 0 if self.calibration is None else int(self.calibration.calibration_n),
            "calibration_frozen": False if self.calibration is None else bool(self.calibration.is_frozen),
            "num_samples": int(frame.num_samples),
        }
        return json.dumps(payload, separators=(",", ":"), ensure_ascii=True).encode("utf-8")

    def _produce_prediction(self, producer: Producer, payload: bytes):
        while self.running:
            try:
                producer.produce(self.topic_out, value=payload)
                producer.poll(0)
                return
            except BufferError:
                producer.poll(0.05)

    def _log_prediction(self, frame: RawFrameEnvelope, pred: Optional[float], infer_ms: Optional[float]):
        next_frame_idx = self.total_frames + 1
        if (
            self.prediction_log_every <= 0
            or pred is None
            or self.calibration is None
            or next_frame_idx % self.prediction_log_every != 0
        ):
            return
        infer_str = "None" if infer_ms is None else f"{infer_ms:.3f}"
        print(
            "[WristInference][Prediction] "
            f"seq={frame.sequence_number} "
            f"pred={pred:.6f} "
            f"infer_ms={infer_str} "
            f"calib_n={self.calibration.calibration_n} "
            f"frozen={self.calibration.is_frozen}",
            flush=True,
        )

    def run(self):
        consumer = Consumer(
            {
                "bootstrap.servers": self.bootstrap_servers,
                "group.id": self.consumer_group_id,
                "auto.offset.reset": self.auto_offset_reset,
            }
        )
        producer = Producer({"bootstrap.servers": self.bootstrap_servers})

        consumer.subscribe([self.topic_in])
        print(f"[WristInference] Subscribed to {self.topic_in}")
        print(f"[WristInference] Publishing predictions to {self.topic_out}")
        if self.prediction_log_every > 0:
            print(f"[WristInference] Prediction logging enabled: every {self.prediction_log_every} frame(s)")

        try:
            while self.running:
                msg = consumer.poll(self.poll_timeout_s)
                if msg is None:
                    continue
                if msg.error():
                    if KafkaError is not None and msg.error().code() == KafkaError._PARTITION_EOF:
                        continue
                    self.total_errors += 1
                    print(f"[WristInference] Consumer error: {msg.error()}")
                    continue

                try:
                    envelope = decode_raw_frame_envelope(msg.value())
                except Exception as exc:
                    self.total_errors += 1
                    if self.total_errors % 50 == 1:
                        print(f"[WristInference] Dropping invalid frame: {exc}")
                    continue

                frame = envelope.samples
                if self.raw_depth is None:
                    self._init_model_for_depth(frame.shape[0])
                elif frame.shape[0] != self.raw_depth:
                    self.total_errors += 1
                    print(
                        "[WristInference] Depth changed unexpectedly. "
                        f"expected={self.raw_depth} got={frame.shape[0]} (dropping frame)"
                    )
                    continue

                assert self.pipeline is not None and self.model is not None and self.calibration is not None
                self.calibration.update(frame)

                pred = None
                infer_ms = None
                if self.calibration.ready():
                    mean, std = self.calibration.get_mean_std()
                    self.pipeline.set_calibration(mean, std)
                    t0 = time.perf_counter()
                    pred = self.pipeline.predict_frame(frame, self.model)
                    infer_ms = (time.perf_counter() - t0) * 1000.0

                payload = self._build_prediction_payload(envelope, pred, infer_ms)
                self._produce_prediction(producer, payload)
                self._log_prediction(envelope, pred, infer_ms)

                self.total_frames += 1
                if self.total_frames % self.log_every == 0:
                    source_age_ms = (time.time_ns() - int(envelope.timestamp_ns)) / 1_000_000.0
                    pred_str = "None" if pred is None else f"{pred:.6f}"
                    infer_str = "None" if infer_ms is None else f"{infer_ms:.3f}"
                    print(
                        "[WristInference] "
                        f"frames={self.total_frames} "
                        f"seq={envelope.sequence_number} "
                        f"pred={pred_str} "
                        f"infer_ms={infer_str} "
                        f"source_age_ms={source_age_ms:.1f} "
                        f"calib_n={self.calibration.calibration_n} "
                        f"frozen={self.calibration.is_frozen}"
                    )
        finally:
            consumer.close()
            producer.flush(5.0)
            print(
                "[WristInference] Done. "
                f"processed_frames={self.total_frames} "
                f"errors={self.total_errors}"
            )


def _default_model_dir() -> Path:
    repo_root = Path(__file__).resolve().parent.parent
    return repo_root / "inference" / "models" / "boosted_tree_regression_sessionnorm"


def _env_or_default(name: str, default: str) -> str:
    v = os.getenv(name)
    if v is None or len(v.strip()) == 0:
        return default
    return v


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


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Wrist regressor inference (CSV or Kafka mode)")
    parser.add_argument(
        "--mode",
        choices=["kafka", "csv"],
        default=_env_or_default("WRIST_INFERENCE_MODE", "kafka"),
    )
    parser.add_argument(
        "--model-dir",
        default=_env_or_default("MODEL_DIR", str(_default_model_dir())),
    )
    parser.add_argument("--csv-path", default=_env_or_default("CSV_PATH", ""))
    parser.add_argument("--out-dir", default=_env_or_default("OUT_DIR", ""))
    parser.add_argument(
        "--calibration-mode",
        choices=sorted(CalibrationController.SUPPORTED_MODES),
        default=_env_or_default("CALIBRATION_MODE", "warmup_freeze"),
    )
    parser.add_argument("--warmup-frames", type=int, default=_env_int("WARMUP_FRAMES", 50))
    parser.add_argument("--freeze-after", type=int, default=_env_int("FREEZE_AFTER", 50))
    parser.add_argument(
        "--fixed-calibration-path",
        default=_env_or_default("FIXED_CALIBRATION_PATH", ""),
    )
    parser.add_argument(
        "--bootstrap-servers",
        default=_env_or_default("BOOTSTRAP_SERVERS", "localhost:9092"),
    )
    parser.add_argument("--topic-in", default=_env_or_default("TOPIC_IN", "ultrasound_raw_data"))
    parser.add_argument("--topic-out", default=_env_or_default("TOPIC_OUT", "wrist_predictions"))
    parser.add_argument(
        "--consumer-group-id",
        default=_env_or_default("CONSUMER_GROUP_ID", "wrist_inference_group"),
    )
    parser.add_argument(
        "--auto-offset-reset",
        choices=["earliest", "latest"],
        default=_env_or_default("AUTO_OFFSET_RESET", "latest"),
    )
    parser.add_argument("--poll-timeout-s", type=float, default=_env_float("POLL_TIMEOUT_S", 0.5))
    parser.add_argument("--log-every", type=int, default=_env_int("LOG_EVERY", 100))
    parser.add_argument(
        "--prediction-log-every",
        type=int,
        default=_env_int("PREDICTION_LOG_EVERY", 0),
        help="Emit per-prediction logs every N frames (0 disables).",
    )
    return parser


def main():
    args = build_arg_parser().parse_args()
    fixed_calibration_path = args.fixed_calibration_path or None

    if args.mode == "csv":
        if not args.csv_path:
            raise ValueError("--csv-path (or CSV_PATH env) is required in csv mode")
        run_session_inference(
            model_dir=args.model_dir,
            csv_path=args.csv_path,
            calibration_mode=args.calibration_mode,
            warmup_frames=args.warmup_frames,
            freeze_after=args.freeze_after,
            out_dir=None if not args.out_dir else args.out_dir,
        )
        return

    worker = LiveKafkaInferenceWorker(
        model_dir=args.model_dir,
        bootstrap_servers=args.bootstrap_servers,
        topic_in=args.topic_in,
        topic_out=args.topic_out,
        consumer_group_id=args.consumer_group_id,
        auto_offset_reset=args.auto_offset_reset,
        calibration_mode=args.calibration_mode,
        warmup_frames=args.warmup_frames,
        freeze_after=args.freeze_after,
        fixed_calibration_path=fixed_calibration_path,
        poll_timeout_s=args.poll_timeout_s,
        log_every=args.log_every,
        prediction_log_every=args.prediction_log_every,
    )

    def _signal_handler(signum, _frame):
        print(f"[WristInference] Shutdown signal {signum} received...")
        worker.stop()

    signal.signal(signal.SIGINT, _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)
    worker.run()


if __name__ == "__main__":
    main()
