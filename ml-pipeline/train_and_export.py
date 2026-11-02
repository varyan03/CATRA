#!/usr/bin/env python3
import os
import shutil
import argparse
import pandas as pd
from sklearn.model_selection import train_test_split
from sklearn.metrics import classification_report, accuracy_score
from xgboost import XGBClassifier
import treelite
import tl2cgen

def train_and_export(
    dataset_path: str = "catra_training_dataset.csv",
    output_c_dir: str = "catra_treelite_c"
) -> None:
    if not os.path.exists(dataset_path):
        raise FileNotFoundError(f"Dataset not found: {dataset_path}")

    telemetry_df = pd.read_csv(dataset_path)
    feature_cols = ["typing_speed_wpm", "window_switches_per_min"]
    target_col = "target_state" if "target_state" in telemetry_df.columns else "label"

    X = telemetry_df[feature_cols]
    y = telemetry_df[target_col]

    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.20, random_state=42, stratify=y
    )

    # Constrain tree depth to ensure minimal branch miss penalties in the C daemon
    classifier = XGBClassifier(
        n_estimators=100,
        max_depth=3,
        learning_rate=0.1,
        eval_metric="logloss",
        random_state=42
    )
    classifier.fit(X_train, y_train)

    val_preds = classifier.predict(X_test)
    print(f"Validation Accuracy: {accuracy_score(y_test, val_preds) * 100:.2f}%\n")
    print(classification_report(y_test, val_preds, target_names=["Relax (0)", "Focus (1)"]))

    if os.path.exists(output_c_dir):
        shutil.rmtree(output_c_dir)
    os.makedirs(output_c_dir, exist_ok=True)

    booster = classifier.get_booster()
    treelite_model = treelite.frontend.from_xgboost(booster)
    # Output single translation unit to simplify daemon linking
    tl2cgen.generate_c_code(treelite_model, dirpath=output_c_dir, params={})

    print(f"Exported raw C sources to: {output_c_dir}/")
    for f in sorted(os.listdir(output_c_dir)):
        print(f"  - {f}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Train XGBoost and export C code via Treelite/TL2cgen")
    parser.add_argument("--data", type=str, default="catra_training_dataset.csv")
    parser.add_argument("--out", type=str, default="catra_treelite_c")
    args = parser.parse_args()

    base_dir = os.path.dirname(os.path.abspath(__file__))
    input_csv = args.data if os.path.isabs(args.data) else os.path.join(base_dir, args.data)
    out_dir = args.out if os.path.isabs(args.out) else os.path.join(base_dir, args.out)

    train_and_export(input_csv, out_dir)
