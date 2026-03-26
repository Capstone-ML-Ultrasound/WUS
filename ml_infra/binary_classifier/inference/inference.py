"""Compatibility entrypoint.

The production Kafka binary-inference worker now lives in `src/inference.py`.
This shim keeps older paths working:

    python ml_infra/binary_classifier/inference/inference.py
"""

import sys
from pathlib import Path


def _run() -> None:
    repo_root = Path(__file__).resolve().parents[3]
    src_dir = repo_root / "src"
    if str(src_dir) not in sys.path:
        sys.path.insert(0, str(src_dir))

    from inference import main as inference_main

    inference_main()


if __name__ == "__main__":
    _run()
