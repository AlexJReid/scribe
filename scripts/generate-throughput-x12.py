#!/usr/bin/env python3
import argparse
from pathlib import Path


FIXTURES = {
    "270": "tests/fixtures/sample_270.edi",
    "271": "tests/fixtures/sample_271.edi",
    "834": "tests/fixtures/sample_834.edi",
    "835": "tests/fixtures/sample_835.edi",
    "837": "tests/fixtures/sample_837.edi",
}


def replacements_for(index: int):
    isa = f"{index:09d}"
    group = index % 900000 or 900000
    st = f"{((index - 1) % 9000) + 1:04d}"

    return [
        ("000000001", isa),
        ("000000002", isa),
        ("000000006", isa),
        ("000000007", isa),
        ("000000008", isa),
        ("*1*X", f"*{group}*X"),
        ("*2*X", f"*{group}*X"),
        ("*6*X", f"*{group}*X"),
        ("*7*X", f"*{group}*X"),
        ("*8*X", f"*{group}*X"),
        ("GE*1*1", f"GE*1*{group}"),
        ("GE*1*2", f"GE*1*{group}"),
        ("GE*1*6", f"GE*1*{group}"),
        ("GE*1*7", f"GE*1*{group}"),
        ("GE*1*8", f"GE*1*{group}"),
        ("*0001", f"*{st}"),
        ("PAYERCLM123", f"PAYER{index:09d}"),
        ("CLM123", f"CLM{index:09d}"),
        ("SUB12345", f"SUB{index:09d}"),
        ("PAT67890", f"PAT{index:09d}"),
        ("MEM12345", f"MEM{index:09d}"),
        ("TRACE270001", f"TRACE{index:09d}"),
        ("ELIG270001", f"ELIG270{index:09d}"),
        ("ELIG271001", f"ELIG271{index:09d}"),
        ("8340001", f"834{index:09d}"),
        ("EFT123456", f"EFT{index:09d}"),
    ]


def render_fixture(template: str, index: int) -> str:
    out = template
    for old, new in replacements_for(index):
        out = out.replace(old, new)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate synthetic X12 files for throughput tests."
    )
    parser.add_argument("--type", required=True, choices=sorted(FIXTURES))
    parser.add_argument("--count", required=True, type=int)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--list-out", required=True)
    args = parser.parse_args()

    if args.count < 1:
        parser.error("--count must be at least 1")

    repo_root = Path(__file__).resolve().parents[1]
    fixture_path = repo_root / FIXTURES[args.type]
    out_dir = Path(args.out_dir)
    list_path = Path(args.list_out)

    template = fixture_path.read_text(encoding="utf-8")
    out_dir.mkdir(parents=True, exist_ok=True)
    list_path.parent.mkdir(parents=True, exist_ok=True)

    input_bytes = 0
    with list_path.open("w", encoding="utf-8") as list_fp:
        for index in range(1, args.count + 1):
            seq = f"{index:09d}"
            edi_path = out_dir / f"{args.type}-{seq}.edi"
            rendered = render_fixture(template, index)
            data = rendered.encode("utf-8")
            edi_path.write_bytes(data)
            input_bytes += len(data)
            list_fp.write(f"{edi_path}\n")

    print(input_bytes)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
