#!/usr/bin/env python3
import os
import argparse
import numpy as np
import pandas as pd
from datetime import datetime, timedelta

def generate_telemetry_dataset(
    sample_count: int = 5000,
    label_noise_ratio: float = 0.05,
    poll_interval_sec: int = 5,
    random_seed: int = 42,
    output_filepath: str = "telemetry_dataset.csv"
) -> pd.DataFrame:
    np.random.seed(random_seed)

    # 2-state Markov model: enforces behavioral inertia (typical work vs distraction sessions)
    p_stay_focus = 0.985
    p_stay_relax = 0.970

    user_states = np.empty(sample_count, dtype=np.int8)
    curr_state = np.random.choice([0, 1])

    for idx in range(sample_count):
        threshold = p_stay_focus if curr_state == 1 else (1.0 - p_stay_relax)
        if np.random.rand() > threshold:
            curr_state = 1 - curr_state
        user_states[idx] = curr_state

    focus_mask = (user_states == 1)
    
    typing_wpm = np.empty(sample_count, dtype=np.float64)
    typing_wpm[focus_mask] = np.random.normal(72.0, 12.0, size=np.count_nonzero(focus_mask))
    typing_wpm[~focus_mask] = np.random.normal(18.0, 8.5, size=np.count_nonzero(~focus_mask))

    window_switches = np.empty(sample_count, dtype=np.float64)
    window_switches[focus_mask] = np.random.normal(1.8, 1.0, size=np.count_nonzero(focus_mask))
    window_switches[~focus_mask] = np.random.normal(15.0, 3.8, size=np.count_nonzero(~focus_mask))

    # Additive hardware polling jitter
    typing_wpm += np.random.normal(0, 1.2, sample_count)
    window_switches += np.random.normal(0, 0.4, sample_count)

    np.clip(typing_wpm, 0.0, 160.0, out=typing_wpm)
    np.clip(window_switches, 0.0, 40.0, out=window_switches)

    labels = user_states.copy()
    if label_noise_ratio > 0.0:
        noise_budget = int(sample_count * label_noise_ratio)
        flip_indices = np.random.choice(sample_count, size=noise_budget, replace=False)
        labels[flip_indices] ^= 1

    epoch_anchor = datetime(2026, 11, 2, 9, 0, 0)
    timestamps = [
        (epoch_anchor + timedelta(seconds=i * poll_interval_sec)).strftime("%Y-%m-%d %H:%M:%S")
        for i in range(sample_count)
    ]

    telemetry_df = pd.DataFrame({
        "timestamp": timestamps,
        "typing_speed_wpm": np.round(typing_wpm, 2),
        "window_switches_per_min": np.round(window_switches, 2),
        "label": labels
    })

    target_dir = os.path.dirname(os.path.abspath(output_filepath))
    if target_dir:
        os.makedirs(target_dir, exist_ok=True)

    telemetry_df.to_csv(output_filepath, index=False)
    return telemetry_df

if __name__ == "__main__":
    cli = argparse.ArgumentParser(description="CATRA Telemetry Synthesizer")
    cli.add_argument("--rows", type=int, default=5000)
    cli.add_argument("--noise", type=float, default=0.05)
    cli.add_argument("--seed", type=int, default=42)
    cli.add_argument("--out", type=str, default="telemetry_dataset.csv")
    args = cli.parse_args()

    out_path = args.out if os.path.isabs(args.out) else os.path.join(os.path.dirname(__file__), args.out)
    df = generate_telemetry_dataset(
        sample_count=args.rows,
        label_noise_ratio=args.noise,
        random_seed=args.seed,
        output_filepath=out_path
    )
    print(f"Generated {len(df)} telemetry samples -> {out_path}")
