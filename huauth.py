# Copyright (c) 2026 nivanikov
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Resolve the head-unit root password without hard-coding it in the scripts.

Resolution order:
  1. the ``HU_PW`` environment variable, else
  2. ``.secrets/hu-password.txt`` at the repository root (git-ignored, never committed).

EntryNav2 head units are known to ship a shared default root password, but it still
stays out of git: ``tools/verify_repo.py`` refuses to push any blob that contains it.
Run the scripts from the repository root (same convention as the ``HU_HOST`` default).
"""
import os
import pathlib

USER = "root"
_SECRET = pathlib.Path(__file__).resolve().parent / ".secrets" / "hu-password.txt"


def hu_password() -> str:
    """The head-unit root password from $HU_PW, or .secrets/hu-password.txt."""
    pw = os.environ.get("HU_PW")
    if pw:
        return pw
    if _SECRET.exists():
        return _SECRET.read_text(encoding="utf-8").strip()
    raise SystemExit(
        "Head-unit password not set. Export HU_PW=... or create .secrets/hu-password.txt "
        "with the head unit's root password (git-ignored)."
    )
