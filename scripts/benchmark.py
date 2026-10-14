#!/usr/bin/env python3
"""Compare HEAD (or --baseline-ref) with the working tree, using isolated databases."""
import argparse
import bisect
import hashlib
import json
import math
from pathlib import Path
import shutil
import sqlite3
import subprocess
import tarfile
import tempfile

REPO = Path(__file__).resolve().parents[1]
QUERIES = ["Safari", "Terminal", "README", "swift", "项目", "中文", "orbital", "Argus"]


def run(arguments, **kwargs):
    return subprocess.run([str(item) for item in arguments], check=True, **kwargs)


def distribution(values):
    values = sorted(values)
    if not values:
        return {"count": 0}
    return {"count": len(values), "p50_ms": values[(len(values) - 1) // 2],
            "p95_ms": values[math.ceil(len(values) * .95) - 1],
            "p99_ms": values[math.ceil(len(values) * .99) - 1], "max_ms": values[-1]}


def frame_latency(app, frame_file):
    frames = sorted((json.loads(line) for line in frame_file.read_text().splitlines()),
                    key=lambda frame: frame["display_mach"])
    times = [frame["display_mach"] for frame in frames]
    samples = {"typing": [], "reopen": []}
    unobserved = []
    for trial in app["trials"]:
        if not trial["measured"]:
            continue
        before = bisect.bisect_left(times, trial["event_start_mach"]) - 1
        initial = frames[before]["result_region_hash"] if before >= 0 else None
        at = bisect.bisect_left(times, trial["result_commit_mach"])
        while at < len(frames) and frames[at]["result_region_hash"] == initial:
            at += 1
        if at == len(frames):
            unobserved.append(trial["sequence"])
            continue
        elapsed = (times[at] - trial["event_start_mach"]) * app["mach_timebase_numer"] / app["mach_timebase_denom"] / 1e6
        if elapsed > 500:
            unobserved.append(trial["sequence"])
            continue
        samples[trial["mode"]].append(elapsed)
    return {"method": "NSTextInputClient entry to first changed result-region window frame after result view display/CA commit; region x18...600,y85...135. Excludes keyboard/OS event delivery and photons. Capture cadence/animation/glass composition remain in this measurement; identical visible first-row pixels can delay change detection.",
            "distributions": {mode: distribution(values) for mode, values in samples.items()},
            "samples_ms": samples, "unobserved_sequences": unobserved}


def measure_app(app, storage, output, observer=None, home=None):
    arguments = [app, "--benchmark", output, "--benchmark-storage", storage]
    if home:
        arguments += ["--benchmark-home", home, "--benchmark-index-only"]
    else:
        arguments += ["--benchmark-no-scan", "--benchmark-frames"]
    with output.with_suffix(".log").open("w") as log:
        process = subprocess.Popen([str(item) for item in arguments], stdout=log, stderr=log)
        capture = None
        if observer:
            capture = subprocess.Popen([str(observer), str(process.pid), str(output) + ".frames"], stdout=log, stderr=log)
        try:
            process.wait(timeout=940 if home else 240)
            if capture:
                capture.wait(timeout=15)
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
            if capture and capture.poll() is None:
                capture.terminate()
                capture.wait(timeout=10)
    result = json.loads(output.read_text())
    if result["failures"]:
        raise RuntimeError(result["failures"])
    if observer:
        result["window_frames"] = frame_latency(result, Path(str(output) + ".frames"))
    return result


def prepare_sources(root, baseline_ref):
    baseline, optimized = root / "baseline", root / "optimized"
    baseline.mkdir(); optimized.mkdir()
    archive = root / "baseline.tar"
    run(["git", "archive", "--format=tar", "--output", archive, baseline_ref], cwd=REPO)
    with tarfile.open(archive) as source:
        source.extractall(baseline, filter="data")
    for name in ["Sources", "Resources"]:
        shutil.copytree(REPO / name, optimized / name)
    for name in ["LICENSE", "scripts/build.sh", "Tests/AppPerformance.swift", "Tests/BenchmarkMemory.swift"]:
        destination = optimized / name
        destination.parent.mkdir(exist_ok=True)
        shutil.copy2(REPO / name, destination)
    # Match measurement-only app setup; the old search/scanner/production UI remain original.
    for name in ["Sources/App/App.swift", "Tests/AppPerformance.swift", "Tests/BenchmarkMemory.swift", "scripts/build.sh"]:
        shutil.copy2(REPO / name, baseline / name)
    index = baseline / "Sources/Search/FileIndex.swift"
    text = index.read_text()
    if "func prepareBenchmarkIndex()" not in text:
        hook = """#if ARGUS_BENCHMARK
    func prepareBenchmarkIndex() {
        writerQueue.async {
            guard let writer = self.writer else { return }
            self.report("Index ready · frozen benchmark corpus", Int(argus_index_count(writer)))
        }
    }
#endif

"""
        if "    func start() {" not in text:
            raise RuntimeError("Baseline FileIndex does not support this benchmark adapter")
        index.write_text(text.replace("    func start() {", hook + "    func start() {", 1))
    return {"baseline": baseline, "optimized": optimized}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", default="HEAD")
    parser.add_argument("--corpus", type=Path, required=True, help="Existing index.sqlite; copied read-only")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--files", type=int, default=100000)
    args = parser.parse_args()
    if args.files < 1:
        parser.error("--files must be positive")
    temporary_parent = Path(subprocess.check_output(["getconf", "DARWIN_USER_TEMP_DIR"], text=True).strip()).resolve()
    with tempfile.TemporaryDirectory(prefix="argus-performance-", dir=temporary_parent) as folder:
        root = Path(folder)
        sources = prepare_sources(root, args.baseline_ref)
        engines = {}
        for version, source in sources.items():
            run([source / "scripts/build.sh", "--benchmark"], cwd=source)
            engines[version] = root / (version + ".dylib")
            run(["xcrun", "clang", "-O3", "-dynamiclib", "-I", source / "Sources/SearchCore",
                 *sorted((source / "Sources/SearchCore").glob("*.c")), "-lsqlite3", "-framework", "CoreFoundation",
                 "-o", engines[version]])
        driver, observer = root / "search-performance", root / "frame-observer"
        run(["xcrun", "clang", "-O3", "-I", REPO / "Sources/SearchCore", REPO / "Tests/SearchPerformance.c", "-o", driver])
        run(["xcrun", "swiftc", "-O", "-parse-as-library", REPO / "Tests/FrameObserver.swift",
             "-framework", "ScreenCaptureKit", "-framework", "CoreMedia", "-framework", "CoreVideo",
             "-framework", "AppKit", "-o", observer])
        report = {"baseline_ref": subprocess.check_output(["git", "rev-parse", args.baseline_ref], cwd=REPO, text=True).strip(),
                  "working_tree_diff_sha256": hashlib.sha256(subprocess.check_output(["git", "diff"], cwd=REPO)).hexdigest(),
                  "hardware": subprocess.check_output(["sysctl", "-n", "machdep.cpu.brand_string"], text=True).strip(),
                  "macOS": subprocess.check_output(["sw_vers", "-productVersion"], text=True).strip(),
                  "method": "Release -O3 / Swift WMO, serial runs, frozen committed corpus copies, existing user preferences. Empty-database full build uses warm filesystem cache; no kernel cache purge.",
                  "core": {}, "ui": {}, "build": {}}
        for version in sources:
            storage = root / (version + "-corpus"); storage.mkdir()
            with sqlite3.connect(args.corpus.resolve().as_uri() + "?mode=ro", uri=True) as source:
                with sqlite3.connect(storage / "index.sqlite") as target:
                    source.backup(target)
            core = subprocess.check_output([str(driver), str(engines[version]), str(storage / "index.sqlite"), *QUERIES], text=True)
            report["core"][version] = json.loads(core)
        # Reverse order reduces a systematic advantage from running second.
        for version in ["optimized", "baseline"]:
            print("Measuring", version, "native UI", flush=True)
            app = sources[version] / "dist/Argus.app/Contents/MacOS/Argus"
            report["ui"][version] = measure_app(app, root / (version + "-corpus"), root / (version + "-ui.json"), observer=observer)
        home = root / "fixture-home"; home.mkdir()
        body = ("年度项目 预算 SearchFixtureText 中文全文测试 Cafe résumé orbital align.\n" * 2).encode()
        for number in range(args.files):
            directory = home / ("Group%04d" % (number // 100))
            directory.mkdir(exist_ok=True)
            (directory / ("Project%03d-report-%06d.txt" % (number % 997, number))).write_bytes(body)
        report["build_fixture"] = {"files": args.files, "directories": math.ceil(args.files / 100), "body_bytes_per_file": len(body)}
        for version in sources:
            print("Measuring", version, "full index build", flush=True)
            app = sources[version] / "dist/Argus.app/Contents/MacOS/Argus"
            value = measure_app(app, root / (version + "-build"), root / (version + "-build.json"), home=home)
            if value["indexed_rows"] != args.files + math.ceil(args.files / 100):
                raise RuntimeError("Scan policy excluded benchmark data")
            with sqlite3.connect(root / (version + "-build/index.sqlite")) as database:
                rows, bodies, body_bytes = database.execute(
                    "SELECT count(*),sum(body IS NOT NULL),sum(length(cast(body AS blob))) FROM entries").fetchone()
            value["persisted_rows"] = rows
            value["persisted_bodies"] = bodies
            value["persisted_body_bytes"] = body_bytes
            if bodies != args.files:
                raise RuntimeError("Content indexing disabled, budget exceeded, or incomplete benchmark bodies")
            report["build"][version] = value
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
