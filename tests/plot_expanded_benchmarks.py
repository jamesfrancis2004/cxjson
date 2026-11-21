import json
import os
import shutil
import matplotlib.pyplot as plt
import numpy as np

# Load benchmark results
results_file = "benchmark_expanded_results.json"
with open(results_file, "r") as f:
    data = json.load(f)

# Sort datasets by cxjson speedup ratio vs orjson descending
data.sort(key=lambda x: x["ratio"], reverse=True)

names = [d["name"] for d in data]
c_mb = [d["cxjson_mb"] for d in data]
or_mb = [d["orjson_mb"] for d in data]
msg_mb = [d["msgspec_mb"] for d in data]
ratios = [d["ratio"] for d in data]

# Styling setup
plt.style.use("dark_background")
fig = plt.figure(figsize=(22, 14), facecolor="#0b0f19")
gs = fig.add_gridspec(1, 2, width_ratios=[1.35, 1.0], wspace=0.25)

# Color tokens
C_JSON_COLOR = "#38bdf8"    # Electric Sky Blue
ORJSON_COLOR = "#34d399"    # Mint Emerald Green
MSGSPEC_COLOR = "#a78bfa"   # Soft Violet
WIN_COLOR = "#38bdf8"
LOSE_COLOR = "#f43f5e"

# ==========================================
# SUBPLOT 1: Absolute Throughput (MB/s)
# ==========================================
ax1 = fig.add_subplot(gs[0], facecolor="#0f172a")

y = np.arange(len(names))
height = 0.26

rects1 = ax1.barh(y - height, c_mb, height, label="cxjson (Ours - Pure C)", color=C_JSON_COLOR, edgecolor="none", zorder=3)
rects2 = ax1.barh(y, or_mb, height, label="orjson (Rust / PyO3)", color=ORJSON_COLOR, edgecolor="none", zorder=3)
rects3 = ax1.barh(y + height, msg_mb, height, label="msgspec (C)", color=MSGSPEC_COLOR, edgecolor="none", zorder=3)

ax1.set_yticks(y)
ax1.set_yticklabels(names, fontsize=11, fontweight="medium", color="#e2e8f0")
ax1.invert_yaxis()
ax1.set_xlabel("Throughput (MB/s) — Higher is Better", fontsize=13, fontweight="bold", color="#94a3b8", labelpad=12)
ax1.set_title("Canonical Internet JSON Datasets: Throughput (MB/s)", fontsize=16, fontweight="bold", color="#f8fafc", pad=15)
ax1.grid(axis="x", linestyle="--", alpha=0.2, color="#94a3b8", zorder=0)
ax1.set_axisbelow(True)
ax1.spines["top"].set_visible(False)
ax1.spines["right"].set_visible(False)
ax1.spines["left"].set_color("#334155")
ax1.spines["bottom"].set_color("#334155")
ax1.tick_params(colors="#cbd5e1", labelsize=11)
ax1.legend(loc="lower right", framealpha=0.9, facecolor="#1e293b", edgecolor="#334155", fontsize=11)

# Annotate values on bars
for bar in rects1:
    width = bar.get_width()
    ax1.text(width + 40, bar.get_y() + bar.get_height() / 2, f"{width:,.0f}",
             va="center", ha="left", fontsize=9, fontweight="bold", color="#38bdf8")

# ==========================================
# SUBPLOT 2: Relative Speedup vs orjson (%)
# ==========================================
ax2 = fig.add_subplot(gs[1], facecolor="#0f172a")

pct_diff = [(r - 1.0) * 100 for r in ratios]
bar_colors = [WIN_COLOR if p >= 0 else LOSE_COLOR for p in pct_diff]

bars_pct = ax2.barh(y, pct_diff, height=0.6, color=bar_colors, edgecolor="none", zorder=3)
ax2.axvline(0, color="#64748b", linewidth=1.5, linestyle="-", zorder=4)
ax2.set_yticks(y)
ax2.set_yticklabels([])  # shared with left
ax2.invert_yaxis()
ax2.set_xlabel("cxjson Advantage over orjson (%)", fontsize=13, fontweight="bold", color="#94a3b8", labelpad=12)
ax2.set_title("Relative Speedup vs orjson (1.00x Baseline)", fontsize=16, fontweight="bold", color="#f8fafc", pad=15)
ax2.grid(axis="x", linestyle="--", alpha=0.2, color="#94a3b8", zorder=0)
ax2.set_axisbelow(True)
ax2.spines["top"].set_visible(False)
ax2.spines["right"].set_visible(False)
ax2.spines["left"].set_color("#334155")
ax2.spines["bottom"].set_color("#334155")
ax2.tick_params(colors="#cbd5e1", labelsize=11)

for bar, p, r in zip(bars_pct, pct_diff, ratios):
    val_str = f"+{p:.1f}% ({r:.2f}x)" if p >= 0 else f"{p:.1f}% ({r:.2f}x)"
    x_pos = bar.get_width() + (3 if p >= 0 else -3)
    align = "left" if p >= 0 else "right"
    ax2.text(x_pos, bar.get_y() + bar.get_height() / 2, val_str,
             va="center", ha=align, fontsize=10, fontweight="bold",
             color="#38bdf8" if p >= 0 else "#f43f5e")

ax2.set_xlim(-35, 175)

# Overall Header & Summary Banner
fig.suptitle(
    "cxjson Dominance Across 20 Canonical Internet JSON Benchmark Datasets\n"
    "19 / 20 Datasets Won (95% Win Rate) | Apple M-Series Silicon | Python 3.12",
    fontsize=20, fontweight="bold", color="#f8fafc", y=0.98
)

plt.tight_layout(rect=[0.02, 0.02, 0.98, 0.93])

output_png = "benchmark_20_datasets.png"
plt.savefig(output_png, dpi=300, bbox_inches="tight", facecolor=fig.get_facecolor())
print(f"Chart successfully generated: {output_png}")

# Also copy to artifacts directory
artifact_dir = "/Users/jamesfrancis/.gemini/antigravity-ide/brain/b8abe025-255a-4094-bd2b-951d36801f7a"
if os.path.exists(artifact_dir):
    shutil.copy(output_png, os.path.join(artifact_dir, output_png))
    print(f"Copied to artifact directory: {os.path.join(artifact_dir, output_png)}")
