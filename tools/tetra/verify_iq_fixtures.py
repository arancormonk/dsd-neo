#!/usr/bin/env python3
"""Regenerate deterministic TETRA IQ fixtures and require byte identity."""

import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import tempfile

# Fixture generation uses only small deterministic vector operations. Avoid
# environment-dependent BLAS worker pools, which can fail to initialize after
# a large parallel Windows build and provide no benefit here.
os.environ["OPENBLAS_NUM_THREADS"] = "1"
os.environ["OMP_NUM_THREADS"] = "1"
os.environ["MKL_NUM_THREADS"] = "1"


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def load_generator(path: Path):
    spec = importlib.util.spec_from_file_location("dsd_neo_iq_fixtures", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load fixture generator: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generator", required=True, type=Path)
    parser.add_argument("--fixtures", required=True, type=Path)
    args = parser.parse_args()

    generator = load_generator(args.generator.resolve())
    names = (
        generator.TETRA_SYNTH_NAME,
        generator.TETRA_INVERTED_NAME,
        generator.TETRA_IMPAIRED_NAME,
        generator.TETRA_THRESHOLD_PASS_NAME,
        generator.TETRA_THRESHOLD_REJECT_NAME,
        generator.TETRA_MULTIPATH_NAME,
        generator.TETRA_CLOCK_FAST_NAME,
        generator.TETRA_CLOCK_SLOW_NAME,
        generator.TETRA_BAD_CRC_NAME,
        generator.TETRA_BAD_CRC_IMPAIRED_NAME,
    )

    failures = []
    with tempfile.TemporaryDirectory(prefix="dsd-neo-tetra-iq-") as temporary:
        generated_dir = Path(temporary)
        generator.build_tetra_synth(str(generated_dir))
        for name in names:
            for suffix in (".iq", ".iq.json"):
                expected = args.fixtures / f"{name}{suffix}"
                actual = generated_dir / f"{name}{suffix}"
                if not expected.is_file():
                    failures.append(f"missing fixture: {expected}")
                elif expected.read_bytes() != actual.read_bytes():
                    failures.append(
                        f"stale fixture: {expected} "
                        f"(expected sha256 {digest(actual)}, found {digest(expected)})"
                    )

    real_air_tool = Path(__file__).with_name("prepare_real_air_fixture.py")
    real_air = load_generator(real_air_tool)
    try:
        real_air.verify(
            args.fixtures / "tetra_scbs_real_air_50k.cs16",
            args.fixtures / "tetra_scbs_real_air_54k.iq",
        )
        real_air.verify_marginal(
            args.fixtures / "tetra_cc_marginal_real_air_144k.iq",
            args.fixtures / "tetra_cc_marginal_real_air_144k_inverted.iq",
        )
    except (OSError, ValueError) as error:
        failures.append(str(error))

    if failures:
        print("\n".join(failures))
        print("regenerate with: python tools/build_iq_fixtures.py --derived-only")
        return 1
    print(f"verified {len(names)} synthetic TETRA IQ fixtures plus four real-air fixture files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
