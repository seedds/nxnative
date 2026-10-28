#!/usr/bin/env python3
"""Stream supported Switch packages and prod.keys into an age-encrypted tar archive."""

import argparse
from pathlib import Path
import subprocess
import tarfile


PACKAGES = {".nsp", ".xci", ".nsz", ".xcz"}


def create(game_input: Path, keys: Path, recipient: str, output: Path) -> None:
    game_input = game_input.expanduser().resolve(strict=True)
    keys = keys.expanduser().resolve(strict=True)
    output = output.expanduser().resolve()
    if not keys.is_file() or keys.stat().st_size == 0:
        raise SystemExit("prod.keys must be a non-empty file")
    if not recipient.startswith("age1"):
        raise SystemExit("NX_AGE_RECIPIENT must be an age recipient (age1…)")

    if game_input.is_dir():
        packages = sorted(
            p for p in game_input.iterdir()
            if p.is_file() and not p.is_symlink() and p.suffix.lower() in PACKAGES
        )
    elif game_input.is_file() and game_input.suffix.lower() in PACKAGES:
        packages = [game_input]
    elif game_input.is_file():
        raise SystemExit("Game file must end in .nsp, .xci, .nsz, or .xcz")
    else:
        raise SystemExit(f"Game path is not a file or directory: {game_input}")
    if not packages:
        raise SystemExit("No .nsp, .xci, .nsz, or .xcz files found directly in the game folder")
    if output.exists():
        raise SystemExit("Refusing to overwrite the encrypted archive")

    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with output.open("xb") as encrypted:
            age = subprocess.Popen(["age", "--recipient", recipient], stdin=subprocess.PIPE, stdout=encrypted)
            try:
                assert age.stdin is not None
                with tarfile.open(fileobj=age.stdin, mode="w|") as archive:
                    for package in packages:
                        archive.add(package, arcname=f"game/{package.name}", recursive=False)
                    archive.add(keys, arcname="prod.keys", recursive=False)
                age.stdin.close()
                status = age.wait()
            except BaseException:
                age.kill()
                age.wait()
                raise
        if status:
            raise SystemExit(f"age encryption failed with exit code {status}")
    except BaseException:
        try:
            output.unlink()
        except FileNotFoundError:
            pass
        raise

    print(f"Encrypted input archive created ({len(packages)} game package(s)); file names and keys were not printed.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game_input", type=Path)
    parser.add_argument("keys", type=Path)
    parser.add_argument("recipient")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    create(args.game_input, args.keys, args.recipient, args.output)


if __name__ == "__main__":
    main()
