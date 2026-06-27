#!/usr/bin/env python3
"""SearchEngine V3.1 HTTP end-to-end load test using only the Python standard library."""

import argparse
import http.client
import json
import math
import statistics
import threading
import time
from pathlib import Path


DEFAULT_QUERIES = [
    "汽车 召回",
    "人工智能",
    "搜索引擎",
    "网络 安全",
    "数据 分析",
    "机器 学习",
    "新能源 汽车",
    "云计算",
]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Measure end-to-end HTTP latency and throughput of search_server."
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18888)
    parser.add_argument("--mode", choices=("search", "suggest"), default="search")
    parser.add_argument("--query-file", type=Path)
    parser.add_argument(
        "--query",
        help="repeat one explicit query; useful for cold-key singleflight tests",
    )
    parser.add_argument(
        "--workload",
        choices=("stable", "scan"),
        default="stable",
        help="use the normal query list or generate the 6200-request scan workload",
    )
    parser.add_argument(
        "--scan-namespace",
        default="v31_scan",
        help="prefix generated scan keys so repeated cold-cache runs do not overlap",
    )
    parser.add_argument("--requests", type=int, default=1000)
    parser.add_argument("--concurrency", type=int, default=16)
    parser.add_argument("--warmup", type=int, default=100)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument(
        "--connection-mode",
        choices=("keep-alive", "close"),
        default="keep-alive",
        help="reuse one HTTP connection per worker or reconnect for every request",
    )
    return parser.parse_args()


def load_queries(path):
    if path is None:
        return DEFAULT_QUERIES
    queries = [line.strip() for line in path.read_text(encoding="utf-8").splitlines()]
    queries = [query for query in queries if query and not query.startswith("#")]
    if not queries:
        raise ValueError("query file contains no usable queries")
    return queries


def build_scan_queries(namespace):
    """Build the same 6200-request scan shape used by the original V3 report."""
    # 20 hot keys are first repeated ten times so they enter the cache/frequency sketch.
    hot_queries = [f"{namespace}_hot_{index}" for index in range(20)]
    queries = hot_queries * 10

    # Each round inserts 200 never-reused scan keys, then revisits every hot key five
    # times. The resulting 4020-key workload has a theoretical maximum 35.16% hit rate.
    for round_index in range(20):
        queries.extend(
            f"{namespace}_round_{round_index}_scan_{index}" for index in range(200)
        )
        queries.extend(hot_queries * 5)
    return queries


def percentile(sorted_values, percent):
    if not sorted_values:
        return 0.0
    rank = max(0, math.ceil(percent / 100.0 * len(sorted_values)) - 1)
    return sorted_values[rank]


def send_request(connection, path, query, keep_alive):
    body = json.dumps({"query": query}, ensure_ascii=False).encode("utf-8")
    started = time.perf_counter_ns()
    connection.request(
        "POST",
        path,
        body=body,
        headers={
            "Content-Type": "application/json",
            "Connection": "keep-alive" if keep_alive else "close",
        },
    )
    response = connection.getresponse()
    response.read()
    status = response.status
    elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000.0
    return status, elapsed_ms


def warm_up(args, path, queries):
    errors = 0
    keep_alive = args.connection_mode == "keep-alive"
    connection = None
    try:
        for index in range(max(0, args.warmup)):
            try:
                if connection is None:
                    connection = http.client.HTTPConnection(
                        args.host, args.port, timeout=args.timeout
                    )
                status, _ = send_request(
                    connection, path, queries[index % len(queries)], keep_alive
                )
                errors += int(status < 200 or status >= 300)
            except Exception:
                errors += 1
                if connection is not None:
                    connection.close()
                connection = None
            if not keep_alive and connection is not None:
                connection.close()
                connection = None
    finally:
        if connection is not None:
            connection.close()
    if errors:
        raise RuntimeError(f"warmup failed: {errors}/{args.warmup} requests")


def run_load(args, path, queries):
    total_requests = max(1, args.requests)
    concurrency = max(1, min(args.concurrency, total_requests))
    next_index = 0
    index_lock = threading.Lock()
    start_barrier = threading.Barrier(concurrency + 1)
    worker_results = [[] for _ in range(concurrency)]
    worker_errors = [0 for _ in range(concurrency)]
    status_counts = [{} for _ in range(concurrency)]

    def worker(worker_id):
        nonlocal next_index
        keep_alive = args.connection_mode == "keep-alive"
        connection = None
        start_barrier.wait()
        try:
            while True:
                with index_lock:
                    if next_index >= total_requests:
                        break
                    request_index = next_index
                    next_index += 1
                query = queries[request_index % len(queries)]
                try:
                    if connection is None:
                        connection = http.client.HTTPConnection(
                            args.host, args.port, timeout=args.timeout
                        )
                    status, latency_ms = send_request(
                        connection, path, query, keep_alive
                    )
                    worker_results[worker_id].append(latency_ms)
                    statuses = status_counts[worker_id]
                    statuses[status] = statuses.get(status, 0) + 1
                    if status < 200 or status >= 300:
                        worker_errors[worker_id] += 1
                except Exception:
                    worker_errors[worker_id] += 1
                    if connection is not None:
                        connection.close()
                    connection = None
                if not keep_alive and connection is not None:
                    connection.close()
                    connection = None
        finally:
            if connection is not None:
                connection.close()

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(concurrency)]
    for thread in threads:
        thread.start()
    start_barrier.wait()
    started = time.perf_counter()
    for thread in threads:
        thread.join()
    elapsed = time.perf_counter() - started

    latencies = sorted(value for values in worker_results for value in values)
    errors = sum(worker_errors)
    statuses = {}
    for partial in status_counts:
        for status, count in partial.items():
            statuses[status] = statuses.get(status, 0) + count

    completed = len(latencies)
    qps = completed / elapsed if elapsed > 0 else 0.0
    return {
        "requested": total_requests,
        "completed": completed,
        "errors": errors,
        "elapsed_seconds": elapsed,
        "qps": qps,
        "connection_mode": args.connection_mode,
        "latency_ms": {
            "mean": statistics.fmean(latencies) if latencies else 0.0,
            "p50": percentile(latencies, 50),
            "p95": percentile(latencies, 95),
            "p99": percentile(latencies, 99),
            "max": latencies[-1] if latencies else 0.0,
        },
        "status_counts": statuses,
    }


def main():
    args = parse_args()
    if args.workload == "scan":
        if args.query_file is not None or args.query is not None:
            raise ValueError("--query/--query-file cannot be combined with --workload scan")
        queries = build_scan_queries(args.scan_namespace)
        # One pass is the complete workload. Reject accidental truncation/repetition so
        # different benchmark runs retain exactly the same request distribution.
        if args.requests != len(queries):
            raise ValueError("scan workload requires --requests 6200")
    else:
        if args.query is not None and args.query_file is not None:
            raise ValueError("--query and --query-file are mutually exclusive")
        queries = [args.query] if args.query is not None else load_queries(args.query_file)
    path = "/api/search" if args.mode == "search" else "/api/suggest"

    warm_up(args, path, queries)
    result = run_load(args, path, queries)
    print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))
    if result["errors"] != 0 or result["completed"] != result["requested"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
