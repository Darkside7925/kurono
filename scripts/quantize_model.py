#!/usr/bin/env python3
"""
Simple quantization / sanity-check loader using transformers + bitsandbytes.
This script attempts to load a model repo downloaded with `download_models.py` and
run a short generation to confirm the model can be loaded with 8-bit (if requested).

It does not perform advanced GPTQ conversion or GGML conversion  -  use dedicated tools for that.

Usage:
  python scripts/quantize_model.py --repo-dir models --which brain --device cuda --use-8bit

"""
import os
import argparse
import time

try:
    import torch
    from transformers import AutoTokenizer, AutoModelForCausalLM, pipeline
except Exception as e:
    print("Missing required packages. Install docs/models/requirements.txt")
    raise


def load_and_test(repo_path: str, device: str, use_8bit: bool, prompt: str):
    tokenizer = AutoTokenizer.from_pretrained(repo_path, use_fast=True)

    load_kwargs = {}
    if use_8bit:
        try:
            import bitsandbytes as bnb  # noqa: F401
            load_kwargs["load_in_8bit"] = True
            load_kwargs["device_map"] = "auto"
        except Exception:
            print("bitsandbytes not installed or not available. Install it to use 8-bit mode.")
            return False
    else:
        load_kwargs["device_map"] = {"": device} if device != "cpu" else "cpu"

    print(f"Loading model from {repo_path} (8-bit={use_8bit}) ...")
    t0 = time.time()
    try:
        model = AutoModelForCausalLM.from_pretrained(repo_path, **load_kwargs)
    except Exception as e:
        print(f"Failed to load model: {e}")
        return False
    load_time = time.time() - t0
    print(f"Model loaded in {load_time:.2f}s")

    gen = pipeline("text-generation", model=model, tokenizer=tokenizer, device=0 if device.startswith("cuda") else -1)
    print("Running a short generation as sanity check...")
    t1 = time.time()
    out = gen(prompt, max_new_tokens=16, do_sample=False)
    latency = time.time() - t1
    print(f"Sanity-check generation latency: {latency:.3f}s")
    print(out[0]["generated_text"])
    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-dir", default="models", help="Directory containing downloaded model repos")
    parser.add_argument("--which", choices=["brain","agents"], default="brain")
    parser.add_argument("--device", default="cuda", help="Device to use (cuda or cpu)")
    parser.add_argument("--use-8bit", action="store_true")
    parser.add_argument("--prompt", default="Hello world.", help="Sanity-check prompt")
    args = parser.parse_args()

    MODEL_MAP = {
        "brain": "google/gemma-4-E4B-it",
        "agents": "google/gemma-4-E2B",
    }

    repo_id = MODEL_MAP[args.which]
    repo_path = os.path.join(args.repo_dir, repo_id.replace("/", "__"))
    # snapshot_download stores in cache directory with original repo name; allow passing direct path
    if not os.path.exists(repo_path):
        # try repo-dir/repo_id
        alt = os.path.join(args.repo_dir, repo_id)
        if os.path.exists(alt):
            repo_path = alt

    if not os.path.exists(repo_path):
        print(f"Model path not found: {repo_path}")
        print("Make sure you ran download_models.py and set --out-dir accordingly.")
        raise SystemExit(1)

    ok = load_and_test(repo_path, args.device, args.use_8bit, args.prompt)
    if not ok:
        raise SystemExit(2)
    print("Sanity check complete.")
