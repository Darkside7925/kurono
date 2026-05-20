#!/usr/bin/env python3
"""
Simple stress test harness that repeatedly calls the model pipeline and records latency.
Use this locally after quantizing/loading the model.

Usage example:
  python scripts/stress_test.py --repo-dir models --which agents --concurrency 4 --iterations 100 --prompt "Test"

Note: This runs inference in-process and is intended for functional/latency measurement only.
For production testing, run a proper server and use a separate load generator.
"""
import argparse
import os
import time
import statistics
from concurrent.futures import ThreadPoolExecutor, as_completed

try:
    from transformers import AutoTokenizer, AutoModelForCausalLM, pipeline
except Exception:
    print("Missing required packages. Install docs/models/requirements.txt")
    raise


def worker(gen, prompt, idx):
    t0 = time.time()
    out = gen(prompt, max_new_tokens=32, do_sample=False)
    t1 = time.time()
    return t1 - t0


def run_stress(repo_path, device, concurrency, iterations, prompt, use_8bit):
    tokenizer = AutoTokenizer.from_pretrained(repo_path, use_fast=True)
    load_kwargs = {}
    if use_8bit:
        load_kwargs["load_in_8bit"] = True
        load_kwargs["device_map"] = "auto"
    else:
        load_kwargs["device_map"] = "auto" if device.startswith("cuda") else "cpu"

    print("Loading model for stress test...")
    model = AutoModelForCausalLM.from_pretrained(repo_path, **load_kwargs)
    gen = pipeline("text-generation", model=model, tokenizer=tokenizer, device=0 if device.startswith("cuda") else -1)

    latencies = []
    with ThreadPoolExecutor(max_workers=concurrency) as ex:
        futures = []
        for i in range(iterations):
            futures.append(ex.submit(worker, gen, prompt, i))
        for f in as_completed(futures):
            try:
                lat = f.result()
                latencies.append(lat)
            except Exception as e:
                print(f"Worker failed: {e}")

    if latencies:
        print(f"Requests: {len(latencies)}")
        print(f"Min: {min(latencies):.4f}s, Max: {max(latencies):.4f}s, Mean: {statistics.mean(latencies):.4f}s, Median: {statistics.median(latencies):.4f}s")
        p95 = statistics.quantiles(latencies, n=100)[94]
        print(f"P95: {p95:.4f}s")
        throughput = len(latencies) / sum(latencies)
        print(f"Approx throughput: {throughput:.2f} req/s")
    else:
        print("No successful requests recorded.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-dir", default="models")
    parser.add_argument("--which", choices=["brain","agents"], default="agents")
    parser.add_argument("--device", default="cuda")
    parser.add_argument("--concurrency", type=int, default=2)
    parser.add_argument("--iterations", type=int, default=50)
    parser.add_argument("--prompt", default="Hello world")
    parser.add_argument("--use-8bit", action="store_true")
    args = parser.parse_args()

    MODEL_MAP = {
        "brain": "google/gemma-4-E4B-it",
        "agents": "google/gemma-4-E2B",
    }
    repo_id = MODEL_MAP[args.which]
    repo_path = os.path.join(args.repo_dir, repo_id.replace("/", "__"))
    if not os.path.exists(repo_path):
        alt = os.path.join(args.repo_dir, repo_id)
        if os.path.exists(alt):
            repo_path = alt

    if not os.path.exists(repo_path):
        print(f"Model path not found: {repo_path}")
        raise SystemExit(1)

    run_stress(repo_path, args.device, args.concurrency, args.iterations, args.prompt, args.use_8bit)
