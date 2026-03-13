#!/usr/bin/env python3
"""
Generate minimal ONNX signal models for testing Spec C v4.

Usage:
    python make_test_signal_model.py [output_dir] [--format FORMAT]

Creates:
    output_dir/cira_model.json - Nested JSON manifest
    output_dir/model.onnx      - Minimal ONNX model

Output formats:
    label_prob       - Classification with probabilities (default)
    softmax          - Classification with softmax
    reconstruction   - Autoencoder reconstruction
    anomaly_score    - Single anomaly score
"""

import argparse
import json
import os
import sys

def create_manifest(num_channels, channel_names, mins, maxs,
                   window_size, stride, sample_rate,
                   num_features, output_format, num_classes):
    """Generate JSON manifest matching cira.c parser format."""

    # Generate all 46 canonical feature names
    feature_names = []
    for ch_idx in range(num_channels):
        ch_name = channel_names[ch_idx]
        # Time domain features (0-27)
        feature_names.extend([
            f"mean_{ch_name}", f"std_{ch_name}", f"var_{ch_name}",
            f"min_{ch_name}", f"max_{ch_name}", f"range_{ch_name}",
            f"median_{ch_name}", f"q25_{ch_name}", f"q75_{ch_name}", f"iqr_{ch_name}",
            f"mad_{ch_name}", f"skew_{ch_name}", f"kurtosis_{ch_name}",
            f"abs_mean_{ch_name}", f"abs_std_{ch_name}", f"abs_max_{ch_name}",
            f"zero_cross_rate_{ch_name}", f"mean_abs_diff_{ch_name}",
            f"mean_diff_{ch_name}", f"autocorr_lag1_{ch_name}",
            f"energy_{ch_name}", f"log_energy_{ch_name}",
            f"sum_{ch_name}", f"abs_sum_{ch_name}",
            f"entropy_{ch_name}", f"rms_{ch_name}", f"peak_to_peak_{ch_name}",
            f"crest_factor_{ch_name}",
            # Frequency domain features (28-45)
            f"fft_mean_{ch_name}", f"fft_std_{ch_name}", f"fft_var_{ch_name}",
            f"fft_energy_{ch_name}", f"fft_entropy_{ch_name}",
            f"spectral_centroid_{ch_name}", f"spectral_spread_{ch_name}",
            f"spectral_skew_{ch_name}", f"spectral_kurtosis_{ch_name}",
            f"spectral_rolloff_{ch_name}", f"peak_frequency_{ch_name}",
            f"peak_magnitude_{ch_name}",
            f"band_power_low_{ch_name}", f"band_power_mid_{ch_name}",
            f"band_power_high_{ch_name}", f"spectral_flux_{ch_name}"
        ])

    manifest = {
        "input_type": "signal",
        "normalization": {
            "sensor_columns": channel_names,
            "channel_min": mins,
            "channel_max": maxs
        },
        "windowing": {
            "window_size": window_size,
            "stride": stride
        },
        "feature_extraction": {
            "num_features": num_features,
            "method": "canonical_46",
            "sampling_rate_hz": sample_rate
        },
        "feature_selection": {
            "selected_features": feature_names[:num_features * num_channels]
        },
        "output": {
            "format": output_format
        }
    }

    # Add format-specific fields
    if output_format in ["label_prob", "softmax"]:
        manifest["num_classes"] = num_classes
        manifest["labels"] = [f"class_{i}" for i in range(num_classes)]
    elif output_format == "reconstruction":
        manifest["output"]["input_size"] = num_features
    elif output_format == "anomaly_score":
        manifest["output"]["anomaly_threshold"] = 0.5

    return manifest


def create_onnx_model(num_features, output_format, num_classes, output_path):
    """Create minimal ONNX model using numpy arrays."""
    try:
        import numpy as np
        import onnx
        from onnx import helper, TensorProto
    except ImportError:
        print("ERROR: This script requires onnx and numpy packages.")
        print("Install with: pip install onnx numpy")
        sys.exit(1)

    # Input tensor: [1, num_features]
    input_tensor = helper.make_tensor_value_info(
        'input', TensorProto.FLOAT, [1, num_features]
    )

    # Output tensor depends on format
    if output_format in ["label_prob", "softmax"]:
        output_shape = [1, num_classes]
        output_name = 'output'
    elif output_format == "reconstruction":
        output_shape = [1, num_features]
        output_name = 'reconstructed'
    elif output_format == "anomaly_score":
        output_shape = [1, 1]
        output_name = 'score'
    else:
        raise ValueError(f"Unknown output format: {output_format}")

    output_tensor = helper.make_tensor_value_info(
        output_name, TensorProto.FLOAT, output_shape
    )

    # Create a simple identity-like operation with a weight matrix
    # This creates a minimal valid model that can run inference
    # For MatMul: [1, num_features] @ [num_features, output_size] = [1, output_size]
    weight_shape = [num_features, output_shape[1]]
    weight_data = np.random.randn(*weight_shape).astype(np.float32) * 0.01

    weight_tensor = helper.make_tensor(
        name='weight',
        data_type=TensorProto.FLOAT,
        dims=weight_shape,
        vals=weight_data.flatten().tolist()
    )

    # MatMul node
    matmul_node = helper.make_node(
        'MatMul',
        inputs=['input', 'weight'],
        outputs=[output_name],
        name='matmul'
    )

    # Create graph
    graph = helper.make_graph(
        nodes=[matmul_node],
        name='test_signal_model',
        inputs=[input_tensor],
        outputs=[output_tensor],
        initializer=[weight_tensor]
    )

    # Create model
    model = helper.make_model(graph, producer_name='cira-test')
    model.opset_import[0].version = 13

    # Check and save
    try:
        onnx.checker.check_model(model)
    except Exception as e:
        print(f"WARNING: ONNX model validation failed: {e}")
        print("Proceeding anyway - model may still work with ONNX Runtime")

    onnx.save(model, output_path)
    print(f"Created ONNX model: {output_path}")
    print(f"  Input: [{1}, {num_features}] float32")
    print(f"  Output: {output_shape} float32")


def main():
    parser = argparse.ArgumentParser(
        description='Generate test ONNX signal model for Spec C v4'
    )
    parser.add_argument(
        'output_dir',
        nargs='?',
        default='test_signal_model',
        help='Output directory (default: test_signal_model)'
    )
    parser.add_argument(
        '--format',
        choices=['label_prob', 'softmax', 'reconstruction', 'anomaly_score'],
        default='label_prob',
        help='Output format (default: label_prob)'
    )
    parser.add_argument(
        '--channels',
        type=int,
        default=3,
        help='Number of signal channels (default: 3)'
    )
    parser.add_argument(
        '--window',
        type=int,
        default=128,
        help='Window size (default: 128)'
    )
    parser.add_argument(
        '--stride',
        type=int,
        default=64,
        help='Stride (default: 64)'
    )
    parser.add_argument(
        '--sample-rate',
        type=float,
        default=1000.0,
        help='Sample rate in Hz (default: 1000.0)'
    )
    parser.add_argument(
        '--classes',
        type=int,
        default=3,
        help='Number of classes for classification (default: 3)'
    )

    args = parser.parse_args()

    # Fixed parameters
    num_features = 46  # Canonical feature count

    # Generate channel metadata
    channel_names = [f"ch{i}" for i in range(args.channels)]
    mins = [-10.0] * args.channels
    maxs = [10.0] * args.channels

    # Create output directory
    os.makedirs(args.output_dir, exist_ok=True)

    # Generate manifest
    manifest = create_manifest(
        num_channels=args.channels,
        channel_names=channel_names,
        mins=mins,
        maxs=maxs,
        window_size=args.window,
        stride=args.stride,
        sample_rate=args.sample_rate,
        num_features=num_features,
        output_format=args.format,
        num_classes=args.classes
    )

    manifest_path = os.path.join(args.output_dir, 'cira_model.json')
    with open(manifest_path, 'w') as f:
        json.dump(manifest, f, indent=2)

    print(f"Created manifest: {manifest_path}")
    print(f"  Channels: {args.channels}")
    print(f"  Window: {args.window} samples @ {args.sample_rate} Hz")
    print(f"  Stride: {args.stride}")
    print(f"  Output format: {args.format}")

    # Generate ONNX model
    model_path = os.path.join(args.output_dir, 'model.onnx')
    create_onnx_model(
        num_features=num_features,
        output_format=args.format,
        num_classes=args.classes,
        output_path=model_path
    )

    print(f"\n✓ Test model created in: {args.output_dir}")
    print(f"  Run C tests:       cd runtime/build && ./test_signal ../../{args.output_dir}")
    print(f"  Run gateway tests: bash tools/test_signal_gateway.sh")


if __name__ == '__main__':
    main()
