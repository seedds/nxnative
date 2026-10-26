#!/usr/bin/env python3
"""Stage the iOS bundle environment from an explicit file or game profile.

DM_ENV_FILE has priority when it is present in the environment. An explicitly
empty value disables a profile default. Otherwise <profile>/ios.env is used
when it exists. The selected file is copied byte-for-byte to the requested
output and its path is printed; no selection prints an empty line.
"""
import os
import shutil
import sys
from pathlib import Path


def select(profile: Path, environ: dict[str, str]) -> Path | None:
    if "DM_ENV_FILE" in environ:
        value = environ["DM_ENV_FILE"]
        return Path(value) if value else None
    default = profile / "ios.env"
    return default if default.is_file() else None


def stage(profile: Path, output: Path, environ: dict[str, str]) -> Path | None:
    source = select(profile, environ)
    if source is None:
        output.unlink(missing_ok=True)
        return None
    if not source.is_file():
        raise FileNotFoundError(f"iOS environment file not found: {source}")
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, output)
    return source


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: stage_profile_env.py <game-profile-dir> <output-env.txt>")
    try:
        source = stage(Path(sys.argv[1]), Path(sys.argv[2]), os.environ)
    except OSError as error:
        raise SystemExit(str(error)) from error
    print(source or "")


if __name__ == "__main__":
    main()
