#!/usr/bin/env python3
import os
import argparse
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns

def evaluate_telemetry_quality(dataset_path: str, plot_output_path: str) -> bool:
    if not os.path.exists(dataset_path):
        raise FileNotFoundError(f"Dataset not found at: {dataset_path}")

    telemetry_df = pd.read_csv(dataset_path)
    target_col = "target_state" if "target_state" in telemetry_df.columns else "label"

    print(f"--- Telemetry QA: {os.path.basename(dataset_path)} ---")
    print(f"Total Observations: {len(telemetry_df):,}")

    # 1. Data Integrity Gate
    null_counts = telemetry_df.isnull().sum()
    has_nulls = null_counts.any()
    print("\n[Gate 1: Data Integrity]")
    print(f"Missing Values:\n{null_counts.to_string()}")
    print(f"Schema Types:\n{telemetry_df.dtypes.to_string()}")

    # 2. Class Balance Gate (Target: ~60% Focus, ~40% Relax)
    counts = telemetry_df[target_col].value_counts()
    percentages = telemetry_df[target_col].value_counts(normalize=True) * 100.0
    focus_pct = percentages.get(1, 0.0)
    relax_pct = percentages.get(0, 0.0)

    print("\n[Gate 2: Class Balance]")
    print(f"Focus (1): {counts.get(1, 0):,} ({focus_pct:.2f}%)")
    print(f"Relax (0): {counts.get(0, 0):,} ({relax_pct:.2f}%)")
    balance_pass = 50.0 <= focus_pct <= 70.0

    # 3. Statistical Realism & Physical Bounds
    print("\n[Gate 3: Statistical Realism & Human Physical Limits]")
    feature_cols = ["typing_speed_wpm", "window_switches_per_min"]
    stats_by_class = telemetry_df.groupby(target_col)[feature_cols].agg(["mean", "std", "min", "max"])
    print(stats_by_class.to_string())

    min_wpm = telemetry_df["typing_speed_wpm"].min()
    max_wpm = telemetry_df["typing_speed_wpm"].max()
    min_switches = telemetry_df["window_switches_per_min"].min()
    max_switches = telemetry_df["window_switches_per_min"].max()

    bounds_pass = (min_wpm >= 0.0 and max_wpm <= 200.0 and min_switches >= 0.0 and max_switches <= 60.0)
    print(f"Physical Bounds Check: {'PASSED' if bounds_pass else 'FAILED'} (WPM: [{min_wpm}, {max_wpm}], Switches: [{min_switches}, {max_switches}])")

    # 4. Visual Separability Plot Export
    sns.set_theme(style="darkgrid")
    fig, ax = plt.subplots(figsize=(10, 6), dpi=150)

    palette = {0: "#e74c3c", 1: "#2ecc71"}
    labels_map = {0: "Relax (0)", 1: "Focus (1)"}
    plot_df = telemetry_df.copy()
    plot_df["State"] = plot_df[target_col].map(labels_map)

    sns.scatterplot(
        data=plot_df,
        x="typing_speed_wpm",
        y="window_switches_per_min",
        hue="State",
        palette={"Relax (0)": "#e74c3c", "Focus (1)": "#2ecc71"},
        alpha=0.6,
        edgecolor=None,
        s=25,
        ax=ax
    )

    ax.set_title("CATRA Telemetry Feature Space (Focus vs Relax Clusters + 5% Noise)", fontsize=13, weight="bold")
    ax.set_xlabel("Typing Speed (WPM)", fontsize=11)
    ax.set_ylabel("Window Focus Switches (per min)", fontsize=11)
    ax.legend(title="Target State", loc="upper right")

    os.makedirs(os.path.dirname(os.path.abspath(plot_output_path)), exist_ok=True)
    plt.savefig(plot_output_path, bbox_inches="tight")
    plt.close(fig)
    print(f"\n[Gate 4: Visual Separability]")
    print(f"Scatter plot saved -> {plot_output_path}")

    all_gates_passed = (not has_nulls) and balance_pass and bounds_pass
    print(f"\nOverall QA Status: {'PASSED' if all_gates_passed else 'FAILED'}")
    return all_gates_passed

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="CATRA Telemetry Dataset QA & EDA")
    parser.add_argument("--data", type=str, default="catra_training_dataset.csv")
    parser.add_argument("--out", type=str, default="dataset_scatter.png")
    args = parser.parse_args()

    base_dir = os.path.dirname(os.path.abspath(__file__))
    csv_file = args.data if os.path.isabs(args.data) else os.path.join(base_dir, args.data)
    if not os.path.exists(csv_file):
        fallback = os.path.join(base_dir, "telemetry_dataset.csv")
        if os.path.exists(fallback):
            csv_file = fallback

    plot_file = args.out if os.path.isabs(args.out) else os.path.join(base_dir, args.out)
    evaluate_telemetry_quality(csv_file, plot_file)
