import argparse
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

# Parse arguments
parser = argparse.ArgumentParser(description='Visualize M-Mode Data')
parser.add_argument('--filepath', type=str, required=True, help='Path to the burst CSV file')
args = parser.parse_args()

df = pd.read_csv(args.filepath, header=None)
data = df.values

# 2. Visual Check: Plot the M-Mode Image
plt.figure(figsize=(10, 6))

# We transpose (.T) because typically M-Mode puts Time on X-axis
plt.imshow(data, aspect='auto', cmap='gray')
plt.title("M-Mode Visualization (Check for Stripes)")
plt.xlabel("Time (Frames)")
plt.ylabel("Depth (Samples)")
plt.colorbar(label="Log Signal Strength")
plt.show()

# 3. Math Check: Signal vs Noise
mean_val = np.mean(data)
max_val = np.max(data)
min_val = np.min(data)

print(f"Max Signal: {max_val} (Should be > 2.0 for strong echoes)")
print(f"Min Signal: {min_val} (Should be < 1.0 for background)")