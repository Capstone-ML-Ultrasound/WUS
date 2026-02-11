import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation
import pandas as pd
from argparse import ArgumentParser

def read_csv(abs_path: str):
    return pd.read_csv(abs_path)

def plot_A_mode_frame(data) -> None:
    y = data.iloc[:, 0].values
    x = range(len(y))
    plt.plot(x, y)
    plt.xlabel("Sample (Depth)")
    plt.ylabel("Log Amplitude")
    plt.title(f"A-Mode Trace (numPoints = {len(y)})")
    plt.show()

def plot_A_mode_burst(data) -> None:
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
        ax.set_title(f"Frame {i+1}/{num_frames}")
        return (line,)

    anim = FuncAnimation(fig, update, frames=num_frames, interval=1000//30, blit=False)
    plt.show()

if __name__ == "__main__":
    arg_parser = ArgumentParser(prog="US Visualization", description="Plot A-Mode from raw CSV")
    arg_parser.add_argument("-filepath", help="Provide full file path to data CSV.")

    args = arg_parser.parse_args()
    csv_path = args.filepath
    csv_df = read_csv(csv_path)

    # if multiple columns should be burst data can modify later 
    if csv_df.shape[1] > 1:
        plot_A_mode_burst(csv_df)
    else:
        plot_A_mode_frame(csv_df)
