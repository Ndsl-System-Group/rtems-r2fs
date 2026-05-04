#!/usr/bin/env python3

import argparse
import base64
import gzip
import json
import subprocess
import sys
from pathlib import Path


GCOV_BEGIN = "*** BEGIN OF GCOV INFO BASE64 ***"
GCOV_END = "*** END OF GCOV INFO BASE64 ***"


def extract_base64_stream(qemu_output: str) -> str:
    begin = qemu_output.find(GCOV_BEGIN)
    end = qemu_output.find(GCOV_END)

    if begin < 0 or end < 0 or end <= begin:
        raise RuntimeError("gcov base64 stream markers were not found in QEMU output")

    payload = qemu_output[begin + len(GCOV_BEGIN):end]
    lines = [line.strip() for line in payload.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError("gcov base64 payload is empty")

    return "".join(lines)


def run_command(argv, *, cwd=None, stdin_bytes=None):
    result = subprocess.run(
        argv,
        cwd=cwd,
        input=stdin_bytes,
        capture_output=True,
        check=False
    )
    if result.returncode != 0:
        stderr = result.stderr.decode("utf-8", errors="replace")
        stdout = result.stdout.decode("utf-8", errors="replace")
        raise RuntimeError(
            "command failed: {}\nstdout:\n{}\nstderr:\n{}".format(
                " ".join(argv),
                stdout,
                stderr
            )
        )
    return result


def clean_gcda(build_dir: Path):
    for path in build_dir.rglob("*.gcda"):
        path.unlink()


def parse_json_coverage(json_gz: Path):
    with gzip.open(json_gz, "rt", encoding="utf-8") as fp:
        payload = json.load(fp)

    file_entry = payload["files"][0]
    lines = file_entry["lines"]
    functions = file_entry.get("functions", [])

    executable_lines = 0
    covered_lines = 0

    for line in lines:
        if line.get("function_name") is None and line.get("count", 0) == 0 and not line.get("branches"):
            continue
        executable_lines += 1
        if line.get("count", 0) > 0:
            covered_lines += 1

    total_branches = 0
    taken_branches = 0
    for line in lines:
        for branch in line.get("branches", []):
            total_branches += 1
            if branch.get("count", 0) > 0:
                taken_branches += 1

    summary = {
        "file": file_entry["file"],
        "functions": len(functions),
        "executable_lines": executable_lines,
        "covered_lines": covered_lines,
        "line_coverage": (covered_lines / executable_lines * 100.0) if executable_lines else 100.0,
        "total_branches": total_branches,
        "taken_branches": taken_branches,
        "taken_branch_coverage_json": (taken_branches / total_branches * 100.0) if total_branches else 100.0,
    }
    return summary


def parse_gcov_stdout(stdout: str):
    metrics = {}
    for line in stdout.splitlines():
        if line.startswith("Lines executed:"):
            match = line.split("Lines executed:", 1)[1].split(" of ", 1)
            metrics["line_coverage"] = float(match[0].rstrip("%"))
            metrics["executable_lines"] = int(match[1])
        elif line.startswith("Branches executed:"):
            match = line.split("Branches executed:", 1)[1].split(" of ", 1)
            metrics["branch_coverage"] = float(match[0].rstrip("%"))
            metrics["total_branches"] = int(match[1])
        elif line.startswith("Taken at least once:"):
            match = line.split("Taken at least once:", 1)[1].split(" of ", 1)
            metrics["branch_taken_coverage"] = float(match[0].rstrip("%"))
            metrics["total_taken_branches"] = int(match[1])
    return metrics


def find_gcno_for_source(build_dir: Path, source_relative: Path) -> Path:
    expected_suffix = source_relative.as_posix() + ".1.gcno"
    matches = [path for path in build_dir.rglob("*.gcno") if path.as_posix().endswith(expected_suffix)]
    if len(matches) == 1:
        return matches[0]
    if not matches:
        raise RuntimeError(f"could not find gcno for source: {source_relative}")
    raise RuntimeError(f"found multiple gcno matches for source {source_relative}: {matches}")


def summary_filename_for_source(source_relative: Path) -> str:
    stem = source_relative.as_posix().replace("/", "__")
    return f"{stem}__coverage_summary.json"


def main():
    parser = argparse.ArgumentParser(description="Extract module gcov coverage from QEMU output.")
    parser.add_argument("--qemu-output", required=True, help="Path to captured QEMU stdout/stderr log.")
    parser.add_argument("--build-dir", default="build", help="Build directory containing .gcno files.")
    parser.add_argument(
        "--source",
        default="src/dir_inode/dir_handler.c",
        help="Source file to report coverage for."
    )
    parser.add_argument(
        "--gcov-tool",
        default="arm-rtems6-gcov-tool",
        help="gcov-tool executable."
    )
    parser.add_argument(
        "--gcov",
        default="arm-rtems6-gcov",
        help="gcov executable."
    )
    parser.add_argument(
        "--output-dir",
        default="build/coverage",
        help="Directory for intermediate artifacts and reports."
    )
    args = parser.parse_args()

    repo_root = Path.cwd()
    build_dir = (repo_root / args.build_dir).resolve()
    source = (repo_root / args.source).resolve()
    source_relative = source.relative_to(repo_root)
    output_dir = (repo_root / args.output_dir).resolve()

    if not build_dir.exists():
        raise RuntimeError(f"build directory does not exist: {build_dir}")
    if not source.exists():
        raise RuntimeError(f"source file does not exist: {source}")

    output_dir.mkdir(parents=True, exist_ok=True)
    clean_gcda(build_dir)

    qemu_output = Path(args.qemu_output).read_text(encoding="utf-8", errors="replace")
    b64_stream = extract_base64_stream(qemu_output)
    binary_stream = base64.b64decode(b64_stream)

    stream_path = output_dir / "gcov-stream.bin"
    stream_path.write_bytes(binary_stream)

    run_command([args.gcov_tool, "merge-stream", str(stream_path)], cwd=build_dir)

    gcno_path = find_gcno_for_source(build_dir, source_relative)
    source_object_dir = gcno_path.parent
    generated = source_object_dir / (gcno_path.stem + ".gcov.json.gz")
    if generated.exists():
        generated.unlink()

    gcov_result = run_command(
        [args.gcov, "-j", "-b", gcno_path.name],
        cwd=source_object_dir
    )

    if not generated.exists():
        raise RuntimeError(f"expected gcov json report was not generated: {generated}")

    summary = parse_json_coverage(generated)
    summary.update(parse_gcov_stdout(gcov_result.stdout.decode("utf-8", errors="replace")))
    summary["gcno"] = str(gcno_path)

    summary_path = output_dir / summary_filename_for_source(source_relative)
    summary_path.write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8"
    )

    print(json.dumps(summary, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(1)
