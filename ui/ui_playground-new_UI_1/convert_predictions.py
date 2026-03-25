"""
python convert_predictions.py --input  session_010_full_predictions.csv --output vector_simulation_3col.csv

convert_predictions.py

Converts session_010_full_predictions.csv (frame_idx, pred) into the new
1x3 vector format expected by the updated cv_etch_a_sketch:

    hand_state, flexion, up_down

  - hand_state : 0.0  (placeholder — pen always down for now)
  - flexion    : raw regression value from the DT model (–80 to +50 deg)
                 negative  → left
                 positive  → right
  - up_down    : 0.0  (not yet available)

Usage:
    python convert_predictions.py \
        --input  session_010_full_predictions.csv \
        --output vector_simulation_3col.csv
"""

import csv
import argparse
import os


def convert(input_path: str, output_path: str) -> None:
    rows_written = 0

    with open(input_path, newline="") as infile, \
            open(output_path, "w", newline="") as outfile:

        reader = csv.DictReader(infile)
        writer = csv.writer(outfile)

        # Optional header — comment out if your C++ reader skips nothing
        # writer.writerow(["hand_state", "flexion", "up_down"])

        for row in reader:
            flexion = float(row["pred"])
            hand_state = 0.0   # placeholder
            up_down    = 0.0   # not yet available
            writer.writerow([f"{hand_state:.6f}",
                             f"{flexion:.6f}",
                             f"{up_down:.6f}"])
            rows_written += 1

    print(f"Done — wrote {rows_written} rows to '{output_path}'")


def main():
    parser = argparse.ArgumentParser(description="Convert prediction CSV to 3-column vector format.")
    parser.add_argument("--input",  default="session_010_full_predictions.csv",
                        help="Path to the source CSV (frame_idx, pred)")
    parser.add_argument("--output", default="vector_simulation_3col.csv",
                        help="Path for the output 3-column CSV")
    args = parser.parse_args()

    if not os.path.exists(args.input):
        raise FileNotFoundError(f"Input file not found: {args.input}")

    convert(args.input, args.output)


if __name__ == "__main__":
    main()