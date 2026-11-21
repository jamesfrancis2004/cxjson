import matplotlib.pyplot as plt
import numpy as np
import shutil

# Set style
plt.style.use('dark_background')
fig, axs = plt.subplots(2, 2, figsize=(16, 12))
fig.suptitle('cxjson vs Industry Standards: Ultra-Fast itoa Performance & Memory', fontsize=18, fontweight='bold', color='#60a5fa', y=0.98)

# Color palette
colors = {
    'cxjson': '#3b82f6',     # Electric Blue
    'orjson': '#10b981',     # Emerald Green
    'msgspec': '#8b5cf6',    # Purple
    'ujson': '#f59e0b',      # Amber
    'rapidjson': '#ec4899',  # Pink
    'std json': '#6b7280',   # Gray
    'simplejson': '#ef4444', # Red
}

# 1. Real-World Datasets Throughput (MB/s)
ax1 = axs[0, 0]
datasets = ['canada.json\n(2.25MB Floats)', 'citm_catalog.json\n(1.73MB Web)', 'twitter.json\n(632KB Strings)', 'github.json\n(56KB Mixed)']
cxjson_mb = [1363.6, 1151.6, 2107.6, 2362.6]
orjson_mb = [760.9, 1022.5, 1880.6, 1826.6]
msgspec_mb = [578.8, 908.0, 1439.2, 1645.0]
ujson_mb = [149.5, 207.2, 417.3, 490.4]

x = np.arange(len(datasets))
width = 0.2
ax1.bar(x - 1.5*width, cxjson_mb, width, label='cxjson (Ours)', color=colors['cxjson'])
ax1.bar(x - 0.5*width, orjson_mb, width, label='orjson (Rust)', color=colors['orjson'])
ax1.bar(x + 0.5*width, msgspec_mb, width, label='msgspec (C)', color=colors['msgspec'])
ax1.bar(x + 1.5*width, ujson_mb, width, label='ujson (C)', color=colors['ujson'])

ax1.set_title('Real-World Production Datasets Throughput (MB/s) [Higher is Better]', fontsize=13, fontweight='bold', pad=10)
ax1.set_xticks(x)
ax1.set_xticklabels(datasets, fontsize=10)
ax1.set_ylabel('Throughput (MB/s)', fontsize=11)
ax1.grid(axis='y', linestyle='--', alpha=0.3)
ax1.legend(loc='upper left', framealpha=0.8)

# 2. Synthetic Microbenchmarks Throughput (MB/s)
ax2 = axs[0, 1]
synth = ['10,000 Ints\n(jeaiii itoa)', '10,000 Floats\n(zmij Ryu)', '1,000 Dict Records\n(Mixed Payload)']
c_synth = [2310.3, 1044.5, 1004.6]
orjson_synth = [1270.8, 579.7, 908.0]
msgspec_synth = [833.6, 293.2, 636.1]
ujson_synth = [406.4, 112.7, 208.6]

x2 = np.arange(len(synth))
ax2.bar(x2 - 1.5*width, c_synth, width, label='cxjson (Ours)', color=colors['cxjson'])
ax2.bar(x2 - 0.5*width, orjson_synth, width, label='orjson (Rust)', color=colors['orjson'])
ax2.bar(x2 + 0.5*width, msgspec_synth, width, label='msgspec (C)', color=colors['msgspec'])
ax2.bar(x2 + 1.5*width, ujson_synth, width, label='ujson (C)', color=colors['ujson'])

ax2.set_title('Microbenchmarks Throughput (MB/s) [Higher is Better]', fontsize=13, fontweight='bold', pad=10)
ax2.set_xticks(x2)
ax2.set_xticklabels(synth, fontsize=10)
ax2.set_ylabel('Throughput (MB/s)', fontsize=11)
ax2.grid(axis='y', linestyle='--', alpha=0.3)
ax2.legend(loc='upper right', framealpha=0.8)

# Annotate the 10k Ints triumph!
ax2.annotate('+81.8% faster\nthan orjson!', 
             xy=(0 - 1.5*width, 2310.3), 
             xytext=(0 - 1.5*width, 2550),
             ha='center', fontsize=10, fontweight='bold', color='#60a5fa',
             arrowprops=dict(facecolor='#60a5fa', shrink=0.05, width=1.5, headwidth=6))

# 3. Peak Serialization Overhead / Memory Inflation Ratio
ax3 = axs[1, 0]
libs = ['cxjson', 'msgspec', 'orjson', 'ujson', 'rapidjson', 'std json']
ratios = [1.00, 1.05, 1.99, 2.43, 2.50, 2.85]
bar_colors = [colors['cxjson'], colors['msgspec'], colors['orjson'], colors['ujson'], colors['rapidjson'], colors['std json']]

y_pos = np.arange(len(libs))
bars = ax3.barh(y_pos, ratios, color=bar_colors, height=0.6)
ax3.set_yticks(y_pos)
ax3.set_yticklabels(libs, fontsize=11, fontweight='bold')
ax3.invert_yaxis()
ax3.set_xlabel('RAM Allocation Multiplier Relative to Payload Size [Lower is Better]', fontsize=11)
ax3.set_title('Memory Overhead: RAM Inflation Ratio (Lower = Zero Copy)', fontsize=13, fontweight='bold', pad=10)
ax3.grid(axis='x', linestyle='--', alpha=0.3)
ax3.axvline(1.0, color='#10b981', linestyle=':', linewidth=2, label='Theoretical Ideal (1.00x)')

for bar, r in zip(bars, ratios):
    ax3.text(r + 0.05, bar.get_y() + bar.get_height()/2, f'{r:.2f}x ({(r-1)*100:.0f}% extra)' if r > 1.0 else '1.00x (0% extra!)', 
             va='center', fontsize=10, fontweight='bold', color='white')
ax3.legend(loc='lower right')

# 4. Integer Array Serializer Speedup vs orjson
ax4 = axs[1, 1]
int_libs = ['cxjson (jeaiii itoa)', 'orjson (itoap)', 'msgspec', 'ujson', 'std json', 'simplejson', 'rapidjson']
int_ops = [38387.1, 21115.7, 13851.5, 6751.9, 1859.9, 1532.5, 1405.0]
int_colors = [colors['cxjson'], colors['orjson'], colors['msgspec'], colors['ujson'], colors['std json'], colors['simplejson'], colors['rapidjson']]

y_pos4 = np.arange(len(int_libs))
bars4 = ax4.barh(y_pos4, int_ops, color=int_colors, height=0.6)
ax4.set_yticks(y_pos4)
ax4.set_yticklabels(int_libs, fontsize=10, fontweight='bold')
ax4.invert_yaxis()
ax4.set_xlabel('Operations per Second [Higher is Better]', fontsize=11)
ax4.set_title('10,000 Integer Serialization Ops/Sec', fontsize=13, fontweight='bold', pad=10)
ax4.grid(axis='x', linestyle='--', alpha=0.3)

for bar, ops in zip(bars4, int_ops):
    speedup = ops / 24657.9
    label = f'{ops:,.0f} ops/s ({speedup:.2f}x)'
    ax4.text(ops + 500, bar.get_y() + bar.get_height()/2, label, 
             va='center', fontsize=9, fontweight='bold', color='white')

plt.tight_layout(rect=[0, 0.03, 1, 0.95])
plt.savefig('benchmark_speed_and_memory.png', dpi=300)
print('Successfully saved benchmark_speed_and_memory.png')

# Copy to artifacts directory
artifact_path = '/Users/jamesfrancis/.gemini/antigravity-ide/brain/b8abe025-255a-4094-bd2b-951d36801f7a/benchmark_speed_and_memory.png'
shutil.copy('benchmark_speed_and_memory.png', artifact_path)
print(f'Copied chart to {artifact_path}')
