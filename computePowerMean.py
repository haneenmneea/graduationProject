#!/usr/bin/env python3
import os
import glob
import pandas as pd

# Reading the files.
header = ['1','2', '3','s','us','power','voltage','current']
all_df = []

for file in glob.glob('./m3_*.oml'):  # <-- التصحيح هنا
    print(f'Loading {file}')
    new_df = pd.read_csv(file, skiprows=10, delimiter='\t')
    new_df.columns = header
    new_df = new_df.drop(columns=['1','2','3', 'current', 'voltage'])
    all_df.append(new_df)

print(f"Total files loaded: {len(all_df)}")  # للتأكد من عدد الملفات

# Combining the dfs
df = pd.concat(all_df, axis=1)
cols = pd.Series(df.columns)

# Rename the cols so that we can work on it
for dup in df.columns[df.columns.duplicated(keep=False)]:
    cols[df.columns.get_loc(dup)] = ([dup + '.' + str(d_idx)
                                     if d_idx != 0
                                     else dup
                                     for d_idx in range(df.columns.get_loc(dup).sum())]
                                    )

# Keep one timeline only
df.columns = cols
df.drop([col for col in df.columns if "s." in col], axis=1, inplace=True)

# Save to csv
df.to_csv("all-energ-crash.csv", index=False)

# Process the timestamp
df['timestamp'] = df['s'] + df['us'] / 1e6

# Drop the original cols
df = df.drop(columns=['s','us'])

# Collect power cols
power = []
for i in df.columns:
    if "power" in i:
        power.append(i)

# Calculate the mean of the power values
df['mean'] = df.iloc[:,1].rolling(window=50).mean()

print("Done! Data saved to all-energ-crash.csv")
print(df.head())
