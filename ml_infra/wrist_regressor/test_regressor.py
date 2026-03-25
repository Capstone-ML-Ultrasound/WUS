from pathlib import Path
import argparse
import json
import joblib
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt


class Transforms:
    @staticmethod
    def normalize_per_session(X_time_depth, mode="per_depth", eps=1e-6):
        X = X_time_depth.astype(np.float32, copy=False)
        if mode == "none":
            return X
        if mode == "per_depth":
            mean = X.mean(axis=0, keepdims=True)
            std = X.std(axis=0, keepdims=True)
            return (X - mean) / (std + eps)
        if mode == "per_frame":
            mean = X.mean(axis=1, keepdims=True)
            std = X.std(axis=1, keepdims=True)
            return (X - mean) / (std + eps)
        if mode == "global":
            mean = X.mean()
            std = X.std()
            return (X - mean) / (std + eps)
        raise ValueError(f"Unknown session_norm mode: {mode}")

    @staticmethod
    def crop_depth_middle(X_time_depth, crop_first=100, crop_last=100):
        if crop_first < 0 or crop_last < 0:
            raise ValueError("crop_first and crop_last must be >= 0")
        if crop_last == 0:
            X_out = X_time_depth[:, crop_first:]
        else:
            X_out = X_time_depth[:, crop_first:-crop_last]
        if X_out.shape[1] <= 0:
            raise ValueError("Cropping removed all depth bins.")
        return X_out

    @staticmethod
    def remove_unstable_ends_depth(X_time_depth, n_remove=20):
        if n_remove < 0:
            raise ValueError("n_remove must be >= 0")
        if X_time_depth.shape[1] <= 2 * n_remove:
            raise ValueError(
                f"Depth too small ({X_time_depth.shape[1]}) to remove {n_remove} samples from both ends."
            )
        if n_remove == 0:
            return X_time_depth
        return X_time_depth[:, n_remove:-n_remove]

    @staticmethod
    def gaussian_kernel1d(sigma, radius=None):
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
    def gaussian_filter_depth(X_time_depth, sigma=2.0):
        X = X_time_depth.astype(np.float32, copy=False)
        k = Transforms.gaussian_kernel1d(sigma=sigma)
        pad = len(k) // 2
        Xpad = np.pad(X, ((0, 0), (pad, pad)), mode="reflect")
        out = np.empty_like(X, dtype=np.float32)
        for i in range(X.shape[0]):
            out[i] = np.convolve(Xpad[i], k, mode="valid")
        return out

    @staticmethod
    def sigmoid(x):
        return 1.0 / (1.0 + np.exp(-x))

    @staticmethod
    def extract_features_sliding(
        X_time_depth,
        win=10,
        step=5,
        b=3.0,
        include_mean=True,
        include_var=True,
        include_energy_sigmoid=True,
    ):
        X = X_time_depth.astype(np.float32, copy=False)
        T, D = X.shape
        if D < win:
            raise ValueError(f"Depth ({D}) is smaller than window ({win}).")
        starts = np.arange(0, D - win + 1, step, dtype=np.int32)

        n_features_per_window = 0
        if include_mean:
            n_features_per_window += 1
        if include_var:
            n_features_per_window += 1
        if include_energy_sigmoid:
            n_features_per_window += 1
        if n_features_per_window == 0:
            raise ValueError("At least one feature must be enabled.")

        feats = np.empty((T, len(starts) * n_features_per_window), dtype=np.float32)
        for t in range(T):
            row = X[t]
            j = 0
            for s in starts:
                w = row[s:s + win]
                m = w.mean()
                v = ((w - m) ** 2).mean()
                energy = np.sqrt((w * w).sum())
                es = Transforms.sigmoid(energy - b)
                if include_mean:
                    feats[t, j] = m
                    j += 1
                if include_var:
                    feats[t, j] = v
                    j += 1
                if include_energy_sigmoid:
                    feats[t, j] = es
                    j += 1
        return feats

    @staticmethod
    def preprocess_to_features(
        X_time_depth,
        session_norm="per_depth",
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
    ):
        X0 = Transforms.normalize_per_session(X_time_depth, mode=session_norm)
        X1 = Transforms.crop_depth_middle(X0, crop_first, crop_last)
        X2 = Transforms.remove_unstable_ends_depth(X1, unstable_remove)
        X3 = Transforms.gaussian_filter_depth(X2, sigma=gauss_sigma)
        F = Transforms.extract_features_sliding(
            X3,
            win=win,
            step=step,
            b=b,
            include_mean=include_mean,
            include_var=include_var,
            include_energy_sigmoid=include_energy_sigmoid,
        )
        return F


def load_one_session_csv(csv_path: Path):
    arr = pd.read_csv(csv_path, header=None).values.astype(np.float32)

    # input CSV:
    # rows = depth
    # cols = A-mode frames
    # model expects:
    # rows = frames
    # cols = depth
    X = arr.T

    if X.ndim != 2:
        raise ValueError(f"{csv_path}: expected 2D array, got shape {X.shape}")

    return X, None


def apply_postprocessing(X, scaler=None, pca=None):
    if scaler is None:
        raise ValueError("Scaler must not be None.")
    X_scaled = scaler.transform(X).astype(np.float32)
    if pca is None:
        return X_scaled
    return pca.transform(X_scaled).astype(np.float32)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--session_csv", type=str, required=True)
    parser.add_argument("--model_dir", type=str, required=True)
    parser.add_argument("--save_csv", type=str, default=None)
    parser.add_argument("--save_npy", type=str, default=None)
    parser.add_argument("--no_plot", action="store_true")
    args = parser.parse_args()

    model_dir = Path(args.model_dir)
    cfg = json.loads((model_dir / "best_config.json").read_text(encoding="utf-8"))
    model = joblib.load(model_dir / "best_model.joblib")
    scaler = joblib.load(model_dir / "best_scaler.joblib")
    pca_path = model_dir / "best_pca.joblib"
    pca = joblib.load(pca_path) if pca_path.exists() else None

    X_raw, _ = load_one_session_csv(Path(args.session_csv))

    X_feat = Transforms.preprocess_to_features(
        X_raw,
        session_norm=cfg["session_norm"],
        crop_first=cfg["crop_first"],
        crop_last=cfg["crop_last"],
        unstable_remove=cfg["unstable_remove"],
        gauss_sigma=cfg["gauss_sigma"],
        win=cfg["win"],
        step=cfg["step"],
        b=cfg["b"],
        include_mean=cfg["include_mean"],
        include_var=cfg["include_var"],
        include_energy_sigmoid=cfg["include_energy_sigmoid"],
    )

    X_proc = apply_postprocessing(X_feat, scaler=scaler, pca=pca)
    pred = model.predict(X_proc).astype(np.float32)

    print(f"session_norm from best_config: {cfg['session_norm']}")
    print(f"raw csv transposed shape (T, D): {X_raw.shape}")
    print(f"feature shape: {X_feat.shape}")
    print(f"processed shape: {X_proc.shape}")
    print(f"pred shape: {pred.shape}")

    if args.save_npy:
        np.save(args.save_npy, pred)
        print(f"saved npy: {args.save_npy}")

    if args.save_csv:
        df = pd.DataFrame({
            "frame_idx": np.arange(len(pred)),
            "pred": pred,
        })
        df.to_csv(args.save_csv, index=False)
        print(f"saved csv: {args.save_csv}")

    if not args.no_plot:
        plt.figure(figsize=(14, 5))
        plt.plot(pred, label="prediction")
        plt.title("Test session prediction")
        plt.xlabel("frame")
        plt.ylabel("predicted target")
        plt.legend()
        plt.tight_layout()
        plt.show()


if __name__ == "__main__":
    main()