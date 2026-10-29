#!/usr/bin/env python3
"""Install a signed iOS app bundle on a connected iPhone or iPad and launch it.

The bundle is checked before any device write: its Info.plist, its code
signature, its embedded development provisioning profile, the profile's expiry
and the selected device's UDID. This is the command-line counterpart to the
Xcode Run button for a prepared game; it does not sign, build or modify the
bundle.

Usage:
  python3 scripts/install_ios_app.py <path/to/App.app> [--device <name|UDID>]
  python3 scripts/install_ios_app.py <path/to/App.app> --list

`--list` only reads the app and the connected devices. Without --device, a
single connected iPhone or iPad is used; with several, pass one by name or UDID.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import plistlib
import re
import subprocess
import sys
import tempfile
from pathlib import Path


@dataclass(frozen=True)
class Device:
    name: str
    udid: str
    model: str
    connected: bool


class DeviceListError(RuntimeError):
    """devicectl could not give a trustworthy device list."""


@dataclass(frozen=True)
class App:
    path: Path
    bundle_id: str
    name: str
    version: str
    expiry: datetime | None
    provisioned: frozenset[str]
    note: str = ""
    error: str | None = None

    @property
    def installable(self) -> bool:
        return self.error is None and self.expiry is not None and self.expiry > datetime.now(timezone.utc)


def _utc(value: datetime) -> datetime:
    return value.replace(tzinfo=timezone.utc) if value.tzinfo is None else value.astimezone(timezone.utc)


def _text(info: dict, key: str, fallback: str = "") -> str:
    value = info.get(key)
    return value if isinstance(value, str) and value else fallback


def _platforms(profile: dict) -> set[str]:
    value = profile.get("Platform", [])
    if isinstance(value, str):
        return {value}
    return {item for item in value if isinstance(item, str)}


def _decode_profile(path: Path) -> dict:
    """Decode the embedded CMS profile without printing its contents."""
    with tempfile.TemporaryDirectory(prefix="nxnative-profile-") as temporary:
        decoded = Path(temporary) / "profile.plist"
        result = subprocess.run(
            ["openssl", "smime", "-verify", "-inform", "DER", "-noverify", "-in", str(path), "-out", str(decoded)],
            text=True,
            capture_output=True,
            check=False,
        )
        if result.returncode != 0 or not decoded.is_file():
            detail = (result.stderr or "the profile could not be decoded").strip().splitlines()[-1]
            raise ValueError(detail)
        with decoded.open("rb") as stream:
            profile = plistlib.load(stream)
    if not isinstance(profile, dict):
        raise ValueError("the embedded provisioning profile is not a property list")
    return profile


def inspect_app(path: Path, now: datetime | None = None) -> App:
    """Check the bundle identity, signature, profile and expiry without a device."""
    path = path.expanduser()
    try:
        with (path / "Info.plist").open("rb") as stream:
            info = plistlib.load(stream)
    except (OSError, plistlib.InvalidFileException, ValueError) as error:
        return App(path, "", path.stem, "", None, frozenset(), error=f"Info.plist is unreadable ({error})")
    if not isinstance(info, dict):
        return App(path, "", path.stem, "", None, frozenset(), error="Info.plist is not a property list")
    bundle_id = _text(info, "CFBundleIdentifier")
    name = _text(info, "CFBundleDisplayName") or _text(info, "CFBundleName") or bundle_id or path.stem
    version = _text(info, "CFBundleShortVersionString") or _text(info, "CFBundleVersion")
    if not bundle_id:
        return App(path, "", name, version, None, frozenset(), error="Info.plist has no CFBundleIdentifier")

    profile_path = path / "embedded.mobileprovision"
    if not profile_path.is_file():
        return App(path, bundle_id, name, version, None, frozenset(), error="embedded.mobileprovision is missing")
    try:
        profile = _decode_profile(profile_path)
    except (ValueError, OSError, plistlib.InvalidFileException) as error:
        return App(path, bundle_id, name, version, None, frozenset(), error=f"the provisioning profile could not be checked ({error})")

    entitlements = profile.get("Entitlements", {})
    entitlement = entitlements.get("application-identifier", "") if isinstance(entitlements, dict) else ""
    if not isinstance(entitlement, str) or not entitlement:
        return App(path, bundle_id, name, version, None, frozenset(), error="the provisioning profile has no application identifier")
    team = entitlement.split(".", 1)[0]
    signed = f"{team}.{bundle_id}"
    matches = entitlement == signed or (entitlement.endswith(".*") and signed.startswith(entitlement[:-1]))
    if not matches:
        return App(path, bundle_id, name, version, None, frozenset(), error="the provisioning profile is for a different app")
    teams = profile.get("TeamIdentifier", [])
    if isinstance(teams, str):
        teams = [teams]
    if teams and team not in teams:
        return App(path, bundle_id, name, version, None, frozenset(), error="the profile team and app identifier do not match")
    if "iOS" not in _platforms(profile):
        return App(path, bundle_id, name, version, None, frozenset(), error="the profile is not provisioned for iOS")
    expires = profile.get("ExpirationDate")
    if not isinstance(expires, datetime):
        return App(path, bundle_id, name, version, None, frozenset(), error="the profile has no expiration date")
    expiry = _utc(expires)
    if expiry <= _utc(now or datetime.now(timezone.utc)):
        return App(path, bundle_id, name, version, expiry, frozenset(), error=f"the provisioning profile expired {expiry:%Y-%m-%d %H:%M UTC}")

    devices = profile.get("ProvisionedDevices", [])
    provisioned = frozenset(item for item in devices if isinstance(item, str)) if isinstance(devices, list) else frozenset()
    if not provisioned:
        return App(path, bundle_id, name, version, expiry, provisioned, error="the profile names no devices; use a development profile")

    display = subprocess.run(["codesign", "-dv", "--verbose=2", str(path)], text=True, capture_output=True, check=False)
    details = display.stdout + display.stderr
    identifier = re.search(r"^Identifier=(.+)$", details, re.MULTILINE)
    signature_team = re.search(r"^TeamIdentifier=(.+)$", details, re.MULTILINE)
    if not identifier or identifier.group(1).strip() != bundle_id:
        return App(path, bundle_id, name, version, expiry, provisioned, error="the code signature identity is missing or mismatched")
    if teams and (not signature_team or signature_team.group(1).strip() not in teams):
        return App(path, bundle_id, name, version, expiry, provisioned, error="the code signature team does not match the profile")

    verify = subprocess.run(["codesign", "--verify", "--deep", "--strict", str(path)], text=True, capture_output=True, check=False)
    if verify.returncode != 0:
        lines = [line.strip() for line in (verify.stdout + verify.stderr).splitlines() if line.strip()]
        only_trust = (
            any("CSSMERR_TP_NOT_TRUSTED" in line for line in lines)
            and all("CSSMERR_TP_NOT_TRUSTED" in line or line.startswith("In architecture:") for line in lines)
        )
        if not only_trust:
            return App(path, bundle_id, name, version, expiry, provisioned, error="the code signature did not verify")
        return App(
            path, bundle_id, name, version, expiry, provisioned,
            note="macOS could not establish certificate trust; the device makes the final signature check",
        )
    return App(path, bundle_id, name, version, expiry, provisioned)


def discover_devices(timeout: int = 12) -> list[Device]:
    """Every physical iPhone or iPad paired to this Mac, connected ones first."""
    try:
        result = subprocess.run(
            ["xcrun", "devicectl", "list", "devices", "--json-output", "-", "--timeout", str(timeout)],
            text=True,
            capture_output=True,
            timeout=timeout + 5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise DeviceListError("the device service did not respond; no installation was attempted") from error
    if result.returncode != 0:
        raise DeviceListError("the device service could not confirm device availability; no installation was attempted")
    try:
        entries = json.loads(result.stdout)["result"]["devices"]
    except (json.JSONDecodeError, KeyError, TypeError):
        raise DeviceListError("devicectl returned an unrecognized device list; no installation was attempted") from None

    found: dict[str, Device] = {}
    for entry in entries:
        hardware = entry.get("hardwareProperties") or {}
        props = entry.get("deviceProperties") or {}
        connection = entry.get("connectionProperties") or {}
        if hardware.get("deviceType") not in {"iPhone", "iPad"} or hardware.get("reality") == "simulated":
            continue
        udid = hardware.get("udid")
        if not isinstance(udid, str) or not udid:
            continue
        name = props.get("name") or hardware.get("marketingName") or "device"
        model = hardware.get("marketingName", "")
        found[udid] = Device(name, udid, model, connection.get("tunnelState") == "connected")
    return sorted(found.values(), key=lambda device: (not device.connected, device.name.casefold()))


def choose_device(devices: list[Device], selection: str | None) -> Device:
    if selection:
        wanted = selection.casefold()
        matches = [device for device in devices if wanted in {device.name.casefold(), device.udid.casefold()}]
        if len(matches) != 1:
            raise ValueError(f"no single device matches {selection!r}; use its name or UDID")
        if not matches[0].connected:
            raise ValueError(f"{matches[0].name} is not connected; no installation was attempted")
        return matches[0]
    connected = [device for device in devices if device.connected]
    if not connected:
        raise ValueError("no iPhone or iPad is connected; unlock it, trust the Mac and try again")
    if len(connected) > 1:
        names = ", ".join(device.name for device in connected)
        raise ValueError(f"more than one device is connected ({names}); choose one with --device")
    return connected[0]


def check_for_device(app: App, device: Device) -> None:
    if app.error:
        raise ValueError(f"{app.name} is not installable: {app.error}")
    if app.expiry is not None and app.expiry <= datetime.now(timezone.utc):
        raise ValueError(f"the provisioning profile expired {app.expiry:%Y-%m-%d %H:%M UTC}")
    if device.udid not in app.provisioned:
        raise ValueError(f"{app.name} is not provisioned for {device.name}; add its UDID to the profile and re-sign the app")


def install_and_launch(app: App, device: Device, launch: bool = True) -> int:
    print(f"Installing {app.name} on {device.name}…")
    install = subprocess.run(
        ["xcrun", "devicectl", "device", "install", "app", "--device", device.udid, str(app.path)],
        check=False,
    )
    if install.returncode != 0:
        print("Installation failed; the app was not launched.", file=sys.stderr)
        return install.returncode or 1
    if not launch:
        print(f"Installed {app.name} on {device.name}.")
        return 0
    print(f"Launching {app.bundle_id}…")
    launched = subprocess.run(
        ["xcrun", "devicectl", "device", "process", "launch", "--device", device.udid, app.bundle_id],
        check=False,
    )
    if launched.returncode != 0:
        print("The app installed, but launching it failed.", file=sys.stderr)
        return launched.returncode or 1
    print(f"Installed and launched {app.name} on {device.name}.")
    return 0


def _print_app(app: App) -> None:
    print(f"App:      {app.name}")
    print(f"Bundle:   {app.bundle_id or 'unknown'}")
    print(f"Version:  {app.version or 'unknown'}")
    print(f"Build:    {app.path}")
    if app.expiry is not None:
        print(f"Profile:  expires {app.expiry:%Y-%m-%d %H:%M UTC}")
    if app.note:
        print(f"Signing:  {app.note}")
    if app.error:
        print(f"Status:   not installable: {app.error}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Install a signed iOS app bundle on a connected iPhone or iPad and launch it")
    parser.add_argument("app", type=Path, help="path to the signed .app bundle")
    parser.add_argument("--device", help="connected iPhone or iPad name or UDID (required when several are connected)")
    parser.add_argument("--list", action="store_true", help="show the app and connected devices without installing")
    parser.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    parser.add_argument("--no-launch", action="store_true", help="install without launching the app")
    args = parser.parse_args(argv)

    app = inspect_app(args.app)
    _print_app(app)
    try:
        devices = discover_devices()
    except DeviceListError as error:
        print(f"Device status unavailable: {error}", file=sys.stderr)
        return 2
    print("\nConnected devices:")
    if not devices:
        print("  none")
    for index, device in enumerate(devices, start=1):
        state = "connected" if device.connected else "not connected"
        model = f" · {device.model}" if device.model else ""
        print(f"  {index}. {device.name}{model} — {state}")

    if args.list:
        return 0 if app.error is None else 2
    try:
        device = choose_device(devices, args.device)
        check_for_device(app, device)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2

    print(f"\nSelected: {app.name} on {device.name} ({device.model or 'device'})")
    if not args.yes:
        try:
            answer = input("Install" + (" and open" if not args.no_launch else "") + "? [y/N] ").strip().casefold()
        except (EOFError, KeyboardInterrupt):
            print("\nCancelled; no installation was attempted.")
            return 0
        if answer not in {"y", "yes"}:
            print("Cancelled; no installation was attempted.")
            return 0
    return install_and_launch(app, device, launch=not args.no_launch)


if __name__ == "__main__":
    raise SystemExit(main())
