#!/usr/bin/env python3
import argparse
import csv
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List


@dataclass
class RunConfig:
    run_id: int
    vic_data: str
    agg_data: str
    hammers: int
    attempts: int


L9_OA = [
    [1, 1, 1, 1],
    [1, 2, 2, 2],
    [1, 3, 3, 3],
    [2, 1, 2, 3],
    [2, 2, 3, 1],
    [2, 3, 1, 2],
    [3, 1, 3, 2],
    [3, 2, 1, 3],
    [3, 3, 2, 1],
]

# Factor levels (3 each): choose moderate ranges so each Condor job stays practical.
VICTIM_LEVELS = ["0x00", "0xff", "0xaa"]
AGGRESSOR_LEVELS = ["0xff", "0x00", "0x55"]
HAMMERS_LEVELS = [3_000_000, 5_000_000, 7_000_000]
ATTEMPTS_LEVELS = [40, 80, 100]
MAX_LAUNCH_RETRIES = 3


def run_cmd(cmd: str, cwd: Path) -> str:
    proc = subprocess.run(
        cmd,
        shell=True,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"Command failed ({proc.returncode}): {cmd}\n{proc.stdout}")
    return proc.stdout


def build_part4(root: Path, cxxflags: str) -> None:
    build_cmds = [
        f"make -B bin/part4 CXXFLAGS=\"{cxxflags}\"",
        f"make -B part4 CXXFLAGS=\"{cxxflags}\"",
        f"make -B CXXFLAGS=\"{cxxflags}\" part4",
        f"make -B CXXFLAGS=\"{cxxflags}\"",
    ]

    last_error = None
    for cmd in build_cmds:
        try:
            run_cmd(cmd, root)
            if (root / "bin" / "part4").exists():
                return
        except Exception as exc:
            last_error = exc

    raise RuntimeError(
        "Failed to build part4 with any known target name. "
        "Tried: bin/part4, part4, and default target. "
        f"Last error: {last_error}"
    )


def parse_successes(out_text: str) -> int:
    m = re.search(r"Number of bit-flip successes observed out of\s+\d+\s+attempts:\s+(\d+)", out_text)
    if not m:
        raise ValueError("Could not parse bit-flip success count from part4.out")
    return int(m.group(1))


def tail_text(text: str, max_lines: int = 20) -> str:
    lines = text.strip().splitlines()
    if not lines:
        return "<empty>"
    return "\n".join(lines[-max_lines:])


def build_configs() -> List[RunConfig]:
    configs: List[RunConfig] = []
    for idx, row in enumerate(L9_OA, start=1):
        a, b, c, d = row
        configs.append(
            RunConfig(
                run_id=idx,
                vic_data=VICTIM_LEVELS[a - 1],
                agg_data=AGGRESSOR_LEVELS[b - 1],
                hammers=HAMMERS_LEVELS[c - 1],
                attempts=ATTEMPTS_LEVELS[d - 1],
            )
        )
    return configs


def build_focused_best_configs() -> List[RunConfig]:
    # Focused candidates around the current best observations.
    # Keep attempts constant for fair comparison.
    return [
        RunConfig(run_id=1, vic_data="0xff", agg_data="0xff", hammers=5_000_000, attempts=100),
        RunConfig(run_id=2, vic_data="0xff", agg_data="0xff", hammers=7_000_000, attempts=100),
        RunConfig(run_id=3, vic_data="0xff", agg_data="0xff", hammers=3_000_000, attempts=100),
        RunConfig(run_id=4, vic_data="0xff", agg_data="0x55", hammers=3_000_000, attempts=100),
        RunConfig(run_id=5, vic_data="0xaa", agg_data="0x00", hammers=3_000_000, attempts=100),
    ]


def run_one_config(root: Path, log_dir: Path, cfg: RunConfig) -> dict:
    cxxflags = (
        "-std=c++20 "
        f"-DVIC_DATA={cfg.vic_data} "
        f"-DAGG_DATA={cfg.agg_data} "
        f"-DHAMMERS_PER_ITER={cfg.hammers} "
        f"-DNUM_HAMMER_ATTEMPTS={cfg.attempts}"
    )

    # Rebuild part4 with macro overrides. The exact target name varies across lab setups.
    build_part4(root, cxxflags)

    out_text = ""
    err_text = ""
    successes = None

    for attempt in range(1, MAX_LAUNCH_RETRIES + 1):
        run_cmd("./launch.sh part4", root)

        out_text = (log_dir / "part4.out").read_text(encoding="utf-8", errors="ignore")
        err_text = (log_dir / "part4.error").read_text(encoding="utf-8", errors="ignore")

        if err_text.strip():
            print("  Warning: part4.error is non-empty")

        try:
            successes = parse_successes(out_text)
            break
        except ValueError:
            print(f"  Parse failed on attempt {attempt}/{MAX_LAUNCH_RETRIES}; retrying...")

    if successes is None:
        raise RuntimeError(
            "Could not parse bit-flip success count from part4.out after retries.\n"
            f"part4.out tail:\n{tail_text(out_text)}\n\n"
            f"part4.error tail:\n{tail_text(err_text)}"
        )

    score = successes / max(cfg.attempts, 1)

    return {
        "run_id": cfg.run_id,
        "vic_data": cfg.vic_data,
        "agg_data": cfg.agg_data,
        "hammers_per_iter": cfg.hammers,
        "num_hammer_attempts": cfg.attempts,
        "successes": successes,
        "success_rate": f"{score:.4f}",
    }


def write_results_csv(results_path: Path, rows: List[dict]) -> None:
    with results_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "run_id",
                "vic_data",
                "agg_data",
                "hammers_per_iter",
                "num_hammer_attempts",
                "successes",
                "success_rate",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)


def rerun_top_configs(
    root: Path,
    log_dir: Path,
    top_rows: List[dict],
    repeats_per_config: int,
    results_path: Path,
) -> List[dict]:
    print("\nRunning confirmation reruns on top configurations...")
    rerun_rows: List[dict] = []

    for row in top_rows:
        cfg = RunConfig(
            run_id=int(row["run_id"]),
            vic_data=row["vic_data"],
            agg_data=row["agg_data"],
            hammers=int(row["hammers_per_iter"]),
            attempts=int(row["num_hammer_attempts"]),
        )

        total_successes = 0
        total_attempts = cfg.attempts * repeats_per_config

        print(
            f"\n[Confirm Run {cfg.run_id}] VIC={cfg.vic_data} AGG={cfg.agg_data} "
            f"HAMMERS={cfg.hammers} ATTEMPTS={cfg.attempts} x {repeats_per_config}"
        )

        for rep in range(1, repeats_per_config + 1):
            print(f"  repetition {rep}/{repeats_per_config}")
            out = run_one_config(root, log_dir, cfg)
            s = int(out["successes"])
            total_successes += s

        agg_rate = total_successes / max(total_attempts, 1)
        rerow = {
            "run_id": cfg.run_id,
            "vic_data": cfg.vic_data,
            "agg_data": cfg.agg_data,
            "hammers_per_iter": cfg.hammers,
            "num_hammer_attempts": total_attempts,
            "successes": total_successes,
            "success_rate": f"{agg_rate:.4f}",
        }
        rerun_rows.append(rerow)
        print(
            f"  aggregate successes: {total_successes} / {total_attempts} "
            f"(rate={agg_rate:.4f})"
        )

    rerun_rows.sort(key=lambda r: (float(r["success_rate"]), r["successes"]), reverse=True)
    write_results_csv(results_path, rerun_rows)

    print("\nConfirmation Top 3:")
    for row in rerun_rows[:3]:
        print(
            f"  Run {row['run_id']}: VIC={row['vic_data']} AGG={row['agg_data']} "
            f"HAMMERS={row['hammers_per_iter']} ATTEMPTS={row['num_hammer_attempts']} "
            f"successes={row['successes']} rate={row['success_rate']}"
        )

    print(f"\nSaved confirmation results to: {results_path}")
    return rerun_rows


def main() -> int:
    parser = argparse.ArgumentParser(description="Part4 OA tuner with confirmation reruns")
    parser.add_argument(
        "--mode",
        choices=["focused", "oa"],
        default="focused",
        help="focused: retest best candidates (default), oa: run full 9-run orthogonal array",
    )
    parser.add_argument(
        "--confirm-top",
        type=int,
        default=3,
        help="Number of top OA configs to rerun for confirmation (default: 3)",
    )
    parser.add_argument(
        "--confirm-repeats",
        type=int,
        default=3,
        help="How many repetitions per confirmed config (default: 3)",
    )
    args = parser.parse_args()

    root = Path(__file__).resolve().parent
    log_dir = root / "log"
    log_dir.mkdir(parents=True, exist_ok=True)

    results_path = log_dir / "part4_oa_results.csv"
    confirm_path = log_dir / "part4_oa_confirm_results.csv"

    if args.mode == "oa":
        configs = build_configs()
        print(f"Running {len(configs)} OA experiments for part4...")
    else:
        configs = build_focused_best_configs()
        print(f"Running {len(configs)} focused best-candidate experiments for part4...")

    results = []

    print("Each run rebuilds part4 with compile-time overrides, submits via launch.sh, and parses log/part4.out")

    for cfg in configs:
        print(
            f"\n[Run {cfg.run_id}] VIC={cfg.vic_data} AGG={cfg.agg_data} "
            f"HAMMERS={cfg.hammers} ATTEMPTS={cfg.attempts}"
        )

        row = run_one_config(root, log_dir, cfg)
        results.append(row)
        print(f"  Successes: {row['successes']} / {cfg.attempts} (rate={row['success_rate']})")

    results.sort(key=lambda r: (float(r["success_rate"]), r["successes"]), reverse=True)

    write_results_csv(results_path, results)

    print("\nTop 3 configurations:")
    for row in results[:3]:
        print(
            f"  Run {row['run_id']}: VIC={row['vic_data']} AGG={row['agg_data']} "
            f"HAMMERS={row['hammers_per_iter']} ATTEMPTS={row['num_hammer_attempts']} "
            f"successes={row['successes']} rate={row['success_rate']}"
        )

    print(f"\nSaved full results to: {results_path}")

    if results:
        best = results[0]
        print("\nCurrent winner:")
        print(
            f"  VIC={best['vic_data']} AGG={best['agg_data']} "
            f"HAMMERS={best['hammers_per_iter']} ATTEMPTS={best['num_hammer_attempts']} "
            f"successes={best['successes']} rate={best['success_rate']}"
        )

    top_n = max(args.confirm_top, 0)
    repeats = max(args.confirm_repeats, 1)
    if top_n > 0:
        rerun_top_configs(root, log_dir, results[:top_n], repeats, confirm_path)

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        raise SystemExit(1)
