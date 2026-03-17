import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation
import pandas as pd
from argparse import ArgumentParser
from statistics import median

def read_csv(abs_path: str):
    return pd.read_csv(abs_path, header=None)

def split_signal_and_timestamps(data: pd.DataFrame):
    numeric = data.apply(pd.to_numeric, errors="coerce")
    if numeric.isna().any().any():
        raise ValueError("CSV contains non-numeric values that cannot be plotted.")

    timestamps = None
    signal = numeric
    first_row = numeric.iloc[0]
    positive_values = first_row[first_row > 0]
    looks_like_timestamps = (
        numeric.shape[0] > 1
        and not positive_values.empty
        and positive_values.min() >= 1_000_000_000_000
    )

    if looks_like_timestamps:
        timestamps = first_row.astype("int64").tolist()
        signal = numeric.iloc[1:].reset_index(drop=True)

    return signal, timestamps

def format_frame_timestamp(timestamps, frame_idx: int) -> str:
    if not timestamps or frame_idx >= len(timestamps):
        return ""

    base_ts = timestamps[0]
    delta_ms = (timestamps[frame_idx] - base_ts) / 1_000_000.0
    return f" | t={delta_ms:.1f} ms"

def get_animation_interval_ms(timestamps) -> int:
    if not timestamps or len(timestamps) < 2:
        return 1000 // 30

    deltas_ns = [
        timestamps[i + 1] - timestamps[i]
        for i in range(len(timestamps) - 1)
        if timestamps[i + 1] > timestamps[i]
    ]
    if not deltas_ns:
        return 1000 // 30

    return max(1, int(round(median(deltas_ns) / 1_000_000.0)))

def plot_A_mode_frame(data, timestamps=None) -> None:
    y = data.iloc[:, 0].values
    x = range(len(y))
    plt.plot(x, y)
    plt.xlabel("Sample (Depth)")
    plt.ylabel("Log Amplitude")
    plt.title(f"A-Mode Trace (numPoints = {len(y)}){format_frame_timestamp(timestamps, 0)}")
    plt.show()

def plot_A_mode_burst(data, timestamps=None) -> None:
    y0 = data.iloc[:, 0].values
    x = range(len(y0))
    num_frames = data.shape[1]

    fig, ax = plt.subplots()
    (line,) = ax.plot(x, y0)
    ax.set_xlim(0, len(y0) - 1)
    ymin = float(data.min().min()); ymax = float(data.max().max())
    if ymin == ymax: ymin, ymax = ymin - 1, ymax + 1
    ax.set_ylim(ymin, ymax)
    ax.set_xlabel("Sample (Depth)"); ax.set_ylabel("Amplitude")

    def update(i):
        line.set_ydata(data.iloc[:, i].values)
        ax.set_title(f"Frame {i+1}/{num_frames}{format_frame_timestamp(timestamps, i)}")
        return (line,)

    anim = FuncAnimation(
        fig,
        update,
        frames=num_frames,
        interval=get_animation_interval_ms(timestamps),
        blit=False,
    )
    plt.show()

if __name__ == "__main__":
    arg_parser = ArgumentParser(prog="US Visualization", description="Plot A-Mode from raw CSV")
    arg_parser.add_argument("-filepath", help="Provide full file path to data CSV.")

    args = arg_parser.parse_args()
    csv_path = args.filepath
    csv_df = read_csv(csv_path)
    signal_df, timestamps = split_signal_and_timestamps(csv_df)

    # if multiple columns should be burst data can modify later 
    if signal_df.shape[1] > 1:
        plot_A_mode_burst(signal_df, timestamps)
    else:
        plot_A_mode_frame(signal_df, timestamps)
