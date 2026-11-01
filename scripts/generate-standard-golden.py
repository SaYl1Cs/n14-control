#!/usr/bin/env python3
"""Execute pinned Rust processors against fake statuses, never live hardware.

Requires rustc only at fixture regeneration time. Supply downloaded upstream
files (basenames from reference/standard-source-lock.json) with --source-dir.
The script performs no network access and refuses changed sources. Only serde
decorations/log imports and unrelated modules/tests are removed; algorithm
bodies, Function definitions, graph processor and utils remain upstream code.
"""
import argparse
import hashlib
import json
from pathlib import Path
import random
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIELDS = ("step_min", "step_max", "down_min", "down_max", "delay", "deviance",
          "only_downward", "hopping", "extremes", "poll_rate")
DEFAULT = dict(zip(FIELDS, (1, 100, 0, 0, 0, 2.0, False, True, False, 1.0)))
CPU = [[0, 0], [60, 0], [61, 10], [75, 54], [90, 100], [100, 100]]
GPU = [[0, 0], [50, 0], [65, 55], [75, 100], [100, 100]]
RAMP = [[20, 30], [40, 30], [60, 45], [90, 85], [100, 100]]


def scenarios():
    cases = []

    def add(name, temperatures, *, graph=None, history=(), edits=None, limit=100, **settings):
        function = {**DEFAULT, **settings}
        ticks = []
        for index, temperature in enumerate(temperatures):
            function = {**function, **(edits or {}).get(index, {})}
            ticks.append({"input": temperature, "function": function.copy()})
        cases.append(dict(name=name, graph=graph or [[0, 0], [100, 100]],
                          temperature_limit=limit, initial_history=list(history), ticks=ticks))

    add("installed-cpu", [55, 60, 61, 64.9, 65, 65.1, 70, 75, 90, 85, 60, 59, 0], graph=CPU, deviance=5)
    add("installed-gpu", [40, 50, 51, 51.01, 65, 66, 66.1, 75, 74, 50, 0], graph=GPU, deviance=1)
    add("zero-is-output-not-no-change", [20, 20, 21, 30, 60, 0, 0], graph=CPU, deviance=0)
    add("zero-delay-first-cycle", [40, 60, 20], graph=[[20, 25], [40, 50], [60, 75], [80, 100]])
    add("delay-three-and-spike-flattening", [40]*4 + [44, 41, 41, 41, 60, 60, 60, 60, 40, 40, 40], delay=3)
    add("inclusive-deviance", [40, 42, 38, 42.0001, 40.0001, 40, 37.9999], deviance=2)
    add("startup-history-backfill", [80, 80, 80, 80], history=[20, 30, 40, 50, 60, 70], delay=3)
    add("startup-short-history", [40, 60, 60, 60, 60], history=[20], delay=5)
    add("zero-temperature-sentinel", [0, 0, 1, 2, 3, 4, 4, 0], delay=3, deviance=1)
    add("latch-hops-after-thirty-quiet-cycles", [40] + [41]*64)
    add("latch-without-hopping", [40] + [41]*64, hopping=False)
    add("initial-latch-without-hopping", [60]*5, hopping=False, delay=0)
    add("only-downward-fast-rise-delayed-fall", [20]*6 + [80, 79, 78, 79, 80] + [40]*10,
        delay=5, only_downward=True, step_max=5)
    add("only-downward-noise", [80]*5 + [50]*6 + [51, 49, 52, 48, 50, 51, 49, 50, 52, 50],
        delay=3, step_min=5, only_downward=True)
    add("symmetric-fixed-steps", [50] + [75]*10 + [20]*15, step_min=5, step_max=0)
    add("asymmetric-fixed-decrease", [50, 80, 80] + [20]*12, down_min=5, down_max=0)
    add("symmetric-ignores-decreasing-max", [50, 80, 80, 20, 20], down_min=0, down_max=1)
    add("ramp-down-issue-602", [75]*6 + [45]*80, graph=RAMP, delay=3, down_min=1, down_max=1)
    add("ramp-up-every-cycle", [45]*10 + [75]*45, graph=RAMP, delay=3, step_max=1, down_min=1, down_max=1)
    add("unclamped-deadband-remains", [60]*6 + [61]*20, graph=RAMP, delay=3)
    add("extreme-bypass-disabled", [98] + [100]*40 + [2] + [0]*40, step_min=5)
    add("extreme-bypass-enabled", [98, 100, 99, 2, 0, 1], step_min=5, extremes=True, deviance=0)
    add("extreme-bypass-respects-max", [90] + [100]*10 + [0]*40, step_min=5, step_max=6,
        down_min=5, down_max=6, extremes=True, deviance=0)
    add("latch-never-bypasses-max", [50] + [51]*40 + [90]*30, step_min=5, step_max=2, deviance=0)
    add("raise-live-delay", [20]*6 + [60]*16, delay=2, edits={6: {"delay": 10}})
    add("lower-live-delay", [20]*12 + [60]*6, delay=10, edits={12: {"delay": 2}})
    add("shrink-already-filled-stack", [20]*6 + [60]*4 + [70]*5, delay=8, edits={10: {"delay": 2}})
    add("live-poll-rate-change", [20]*6 + [60]*12, delay=3, edits={6: {"poll_rate": 0.5}})
    add("fractional-poll-ceil", [20]*7 + [60]*10, delay=3, poll_rate=0.7)
    add("delay-byte-saturation", [20] + [60]*260, delay=255, poll_rate=0.5)
    add("latch-sixty-second-cap", [40] + [41]*130, delay=100, poll_rate=1, history=[40]*100)
    add("zero-minimum-allows-equal-output", [40]*5 + [60]*5, step_min=0, deviance=0)
    add("normalized-duplicates-and-decrease", [0, 20, 49, 50, 75, 90, 100],
        graph=[[50, 20], [0, 0], [50, 60], [75, 40], [100, 100]])
    add("source-failsafe-limit", [0, 70, 80, 90, 100], graph=[[0, 0], [100, 0]], limit=80)
    add("first-point-already-full", [0, 10, 90], graph=[[0, 100], [90, 100]])
    for index in range(8):
        rng = random.Random(0x4E3134 + index)
        temperatures = [round(rng.uniform(0, 100), 3) for _ in range(100)]
        add(f"deterministic-state-trace-{index}", temperatures, delay=index,
            only_downward=bool(index & 1), hopping=bool(index & 2), extremes=bool(index & 4),
            step_min=1+index, step_max=9+index, down_min=index, down_max=3+index,
            deviance=index/2, poll_rate=0.5 if index & 2 else 1.0)
    return cases


def without_serde(text):
    text = re.sub(r"^use (?:serde|log)::.*;\n", "", text, flags=re.M)
    text = re.sub(r"^\s*#\[serde\([^\n]*\)\]\n", "\n", text, flags=re.M)
    def derive(match):
        names = [name.strip() for name in match.group(1).split(",")
                 if name.strip() not in {"Serialize", "Deserialize", "JsonSchema", "Display", "EnumString"}]
        return "#[derive(" + ", ".join(names) + ")]"
    return re.sub(r"#\[derive\(([^\n]*)\)\]", derive, text)


def generate(source_dir, output):
    lock = json.loads((ROOT / "reference/standard-source-lock.json").read_text())
    sources = {}
    for name, entry in lock["files"].items():
        data = (source_dir / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != entry["sha256"]:
            raise ValueError(f"Upstream source checksum mismatch: {name}")
        sources[name] = data.decode()
    cases = scenarios()
    tokens = [len(cases)]
    for case in cases:
        tokens.append(len(case["graph"]))
        tokens.extend(value for point in case["graph"] for value in point)
        tokens.extend([case["temperature_limit"], len(case["initial_history"])])
        tokens.extend(case["initial_history"])
        tokens.append(len(case["ticks"]))
        for tick in case["ticks"]:
            tokens.extend(int(tick["function"][f]) if isinstance(tick["function"][f], bool)
                          else tick["function"][f] for f in FIELDS)
            tokens.append(tick["input"])
    with tempfile.TemporaryDirectory(prefix="standard-oracle-", dir=ROOT / "build") as directory:
        directory = Path(directory)
        # Keep the original Function definitions/defaults/accessors and engine
        # data/Processor definitions; omit unrelated modules and serialization.
        setting = sources["setting.rs"].split("pub struct Function {", 1)[1]
        setting = "#[derive(Debug, Clone)]\npub struct Function {" + setting
        setting = setting.split("\n#[derive(\n", 1)[0]
        engine = "#[derive(Debug, Clone)]" + sources["mod.rs"].split(
            "#[derive(Debug, Clone, Serialize, Deserialize)]", 1)[1]
        engine = engine.replace("#[derive(Debug, Clone)]\npub struct NormalizedGraphProfile {",
                                "#[derive(Debug, Clone)]\npub struct NormalizedGraphProfile {")
        for name, data in {
            "setting": without_serde(setting),
            "engine": without_serde(engine),
            "functions": without_serde(sources["functions.rs"].split("#[cfg(test)]", 1)[0]),
            "utils": sources["utils.rs"], "profiles": sources["profiles.rs"],
        }.items():
            (directory / f"oracle_{name}.rs").write_text(data)
        (directory / "main.rs").write_bytes((ROOT / "tests/standard_oracle.rs").read_bytes())
        executable = directory / "oracle"
        subprocess.run(["rustc", "--edition=2021", "-O", str(directory / "main.rs"),
                        "-o", str(executable)], check=True)
        result = subprocess.run([str(executable)], input=" ".join(map(str, tokens)),
                                text=True, capture_output=True, check=True)
    lines = iter(result.stdout.splitlines())
    count = 0
    for case in cases:
        for tick in case["ticks"]:
            tick["expected"] = json.loads(next(lines))
            count += 1
    if next(lines, None) is not None:
        raise ValueError("Unexpected oracle output")
    fixture = dict(schema=1, upstream_commit=lock["commit"], source_sha256={
        name: value["sha256"] for name, value in lock["files"].items()}, cases=cases)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(fixture, separators=(",", ":"), allow_nan=False) + "\n")
    print(f"Generated {len(cases)} cases / {count} cycles: {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", required=True, type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "tests/fixtures/standard-golden.json")
    args = parser.parse_args()
    generate(args.source_dir.resolve(), args.output.resolve())