#!/usr/bin/env python3
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import warnings
warnings.filterwarnings('ignore')

# Stable Power
f1 = pd.read_csv("all-energ-crash.csv", error_bad_lines=False)
# Intermittent Power
f2 = pd.read_csv("all-energ-crash1.csv", error_bad_lines=False)

# timestamp
f1['timestamp'] = f1['s'] + f1['us'] / 1e6
f2['timestamp'] = f2['s'] + f2['us'] / 1e6

# تطبيع الوقت ليبدأ من 0
f1['timestamp'] = f1['timestamp'] - f1['timestamp'].min()
f2['timestamp'] = f2['timestamp'] - f2['timestamp'].min()

# حذف s و us
f1 = f1.drop(columns=['s', 'us'])
f2 = f2.drop(columns=['s', 'us'])

# متوسط جميع أعمدة power
power_cols_f1 = [c for c in f1.columns if 'power' in c]
power_cols_f2 = [c for c in f2.columns if 'power' in c]

f1['mean_power'] = f1[power_cols_f1].mean(axis=1).rolling(window=10000).mean()
f2['mean_power'] = f2[power_cols_f2].mean(axis=1).rolling(window=10000).mean()

fig, ax = plt.subplots(figsize=(12, 5))
ax.set_xlabel("Time (sec)", fontsize=12)
ax.set_ylabel("Energy (W)", fontsize=12)
ax.set_title("RPL Energy Consumption: Stable vs Intermittent Power", fontsize=13)
ax.minorticks_on()
ax.tick_params(direction='in', right=True, top=True, labelsize=10)
ax.grid(True, linestyle='--', color='gray', alpha=0.5, zorder=1)

ax.plot(f1['timestamp'], f1['mean_power'],
        color='#0348a1', linewidth=1.2, zorder=2, label='Stable Power - RPL')

ax.plot(f2['timestamp'], f2['mean_power'],
        color='#c3121e', linewidth=1.2, zorder=2, label='Intermittent Power - RPL')

ax.legend(fontsize=11)
plt.tight_layout()
plt.savefig('result.png', dpi=300, bbox_inches="tight")
plt.close()
print("Done! result.png saved")
