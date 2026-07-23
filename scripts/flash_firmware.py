#!/usr/bin/env python3
"""Flash an Inx firmware image to an Xteink ESP32-C3 with a native file picker."""

from __future__ import annotations

import argparse
import glob
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = REPO_ROOT / ".pio" / "build" / "gh_release"
APP_OFFSET = "0x10000"
MAX_APP_SIZE = 0x640000
MIN_APP_SIZE = 64 * 1024
ESP_IMAGE_MAGIC = 0xE9
ESP32_C3_CHIP_ID = 5


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Pick and flash an Inx firmware.bin over USB.",
        epilog=(
            "Default safe mode flashes the Inx bootloader, partition table, OTA app0 selector, "
            "and the selected app image. Saved books and settings are preserved."
        ),
    )
    parser.add_argument("firmware", nargs="?", type=Path, help="firmware.bin; opens a native picker when omitted")
    parser.add_argument("--port", help="serial port, for example /dev/cu.usbmodem1101 or COM4")
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument(
        "--app-only",
        action="store_true",
        help="write only offset 0x10000; not recommended after OTA slot switching",
    )
    parser.add_argument("--build", action="store_true", help="build gh_release and flash its firmware.bin")
    parser.add_argument("--dry-run", action="store_true", help="validate and print the command without flashing")
    parser.add_argument("-y", "--yes", action="store_true", help="skip the FLASH confirmation")
    return parser.parse_args()


def pick_firmware() -> Path | None:
    if sys.platform == "darwin" and shutil.which("osascript"):
        script = (
            'POSIX path of (choose file with prompt "Choose Inx firmware.bin" '
            'of type {"com.apple.macbinary-archive","public.data"})'
        )
        result = subprocess.run(["osascript", "-e", script], text=True, capture_output=True)
        if result.returncode == 0 and result.stdout.strip():
            return Path(result.stdout.strip())
        return None

    if os.name == "nt" and shutil.which("powershell"):
        script = (
            "Add-Type -AssemblyName System.Windows.Forms; "
            "$p=New-Object System.Windows.Forms.OpenFileDialog; "
            "$p.Title='Choose Inx firmware.bin'; $p.Filter='Firmware (*.bin)|*.bin'; "
            "if($p.ShowDialog() -eq 'OK'){Write-Output $p.FileName}"
        )
        result = subprocess.run(
            ["powershell", "-NoProfile", "-Command", script], text=True, capture_output=True
        )
        if result.returncode == 0 and result.stdout.strip():
            return Path(result.stdout.strip())
        return None

    for picker in ("zenity", "kdialog"):
        if not shutil.which(picker):
            continue
        command = (
            [picker, "--file-selection", "--title=Choose Inx firmware.bin", "--file-filter=*.bin"]
            if picker == "zenity"
            else [picker, "--getopenfilename", str(Path.home()), "Firmware (*.bin)"]
        )
        result = subprocess.run(command, text=True, capture_output=True)
        if result.returncode == 0 and result.stdout.strip():
            return Path(result.stdout.strip())
        return None

    try:
        import tkinter as tk
        from tkinter import filedialog

        root = tk.Tk()
        root.withdraw()
        selected = filedialog.askopenfilename(
            title="Choose Inx firmware.bin",
            filetypes=[("Firmware image", "*.bin"), ("All files", "*.*")],
        )
        root.destroy()
        return Path(selected) if selected else None
    except (ImportError, RuntimeError, OSError):
        return None


def validate_firmware(path: Path) -> None:
    if not path.is_file():
        raise SystemExit(f"Firmware not found: {path}")
    if path.suffix.lower() != ".bin":
        raise SystemExit("Firmware must be a .bin file.")

    size = path.stat().st_size
    if size < MIN_APP_SIZE:
        raise SystemExit(f"Firmware is too small to be an app image ({size} bytes).")
    if size > MAX_APP_SIZE:
        raise SystemExit(
            f"Firmware is {size} bytes; the Inx OTA app slot allows {MAX_APP_SIZE} bytes."
        )

    with path.open("rb") as firmware_file:
        header = firmware_file.read(14)
    if len(header) < 14 or header[0] != ESP_IMAGE_MAGIC:
        raise SystemExit("File does not have a valid ESP application image header.")
    chip_id = int.from_bytes(header[12:14], "little")
    if chip_id != ESP32_C3_CHIP_ID:
        raise SystemExit(f"Firmware targets ESP chip ID {chip_id}, not ESP32-C3 ({ESP32_C3_CHIP_ID}).")


def find_esptool() -> list[str]:
    packaged = sorted(
        Path.home().glob(".platformio/packages/tool-esptoolpy*/esptool.py"),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    if packaged:
        python_candidates = [Path(sys.executable)]
        pio = shutil.which("pio")
        if pio:
            try:
                first_line = Path(pio).read_text(encoding="utf-8").splitlines()[0]
                if first_line.startswith("#!"):
                    python_candidates.append(Path(first_line[2:]))
            except (OSError, UnicodeError):
                pass
        python_candidates.append(Path.home() / ".platformio" / "penv" / "bin" / "python")

        for python in dict.fromkeys(python_candidates):
            if not python.exists():
                continue
            probe = subprocess.run(
                [str(python), str(packaged[0]), "version"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            if probe.returncode == 0:
                return [str(python), str(packaged[0])]

    if importlib.util.find_spec("esptool") is not None:
        return [sys.executable, "-m", "esptool"]

    executable = shutil.which("esptool") or shutil.which("esptool.py")
    if executable:
        return [executable]

    raise SystemExit(
        "esptool was not found. Install it with `python3 -m pip install esptool` "
        "or install PlatformIO."
    )


def serial_ports() -> list[str]:
    ports: set[str] = set()
    try:
        from serial.tools import list_ports

        ports.update(port.device for port in list_ports.comports())
    except ImportError:
        pass

    patterns = (
        "/dev/cu.usbmodem*",
        "/dev/cu.usbserial*",
        "/dev/ttyACM*",
        "/dev/ttyUSB*",
    )
    for pattern in patterns:
        ports.update(glob.glob(pattern))
    return sorted(ports)


def choose_port(explicit: str | None, dry_run: bool) -> str:
    if explicit:
        return explicit
    ports = serial_ports()
    if len(ports) == 1:
        return ports[0]
    if len(ports) > 1:
        print("Available serial ports:")
        for index, port in enumerate(ports, start=1):
            print(f"  {index}. {port}")
        try:
            choice = int(input("Port number: ").strip())
        except (ValueError, EOFError):
            raise SystemExit("No serial port selected.") from None
        if 1 <= choice <= len(ports):
            return ports[choice - 1]
        raise SystemExit("Invalid serial port selection.")
    if dry_run:
        return "<auto-detect>"
    raise SystemExit(
        "No USB serial port found. Wake the reader, connect a data-capable USB-C cable, "
        "or pass --port explicitly."
    )


def run_release_build() -> None:
    pio = shutil.which("pio")
    if not pio:
        raise SystemExit("PlatformIO `pio` is required to build the release support images.")
    subprocess.run([pio, "run", "-e", "gh_release"], cwd=REPO_ROOT, check=True)


def support_images(build_if_missing: bool) -> tuple[Path, Path, Path]:
    bootloader = BUILD_DIR / "bootloader.bin"
    partitions = BUILD_DIR / "partitions.bin"
    boot_selectors = sorted(
        Path.home().glob(
            ".platformio/packages/framework-arduinoespressif32*/tools/partitions/boot_app0.bin"
        )
    )
    if (not bootloader.exists() or not partitions.exists() or not boot_selectors) and build_if_missing:
        run_release_build()
        boot_selectors = sorted(
            Path.home().glob(
                ".platformio/packages/framework-arduinoespressif32*/tools/partitions/boot_app0.bin"
            )
        )
    if not bootloader.exists() or not partitions.exists() or not boot_selectors:
        raise SystemExit(
            "Safe flashing support images are missing. Run `pio run -e gh_release`, "
            "or use --app-only if you intentionally want only offset 0x10000."
        )
    return bootloader, partitions, boot_selectors[-1]


def flash_command(
    esptool: Sequence[str],
    firmware: Path,
    port: str,
    baud: int,
    app_only: bool,
) -> list[str]:
    command = [
        *esptool,
        "--chip",
        "esp32c3",
        "--port",
        port,
        "--baud",
        str(baud),
        "--before",
        "default_reset",
        "--after",
        "hard_reset",
        "write_flash",
        "-z",
        "--flash_mode",
        "dio",
        "--flash_freq",
        "80m",
        "--flash_size",
        "16MB",
    ]
    if app_only:
        return [*command, APP_OFFSET, str(firmware)]

    bootloader, partitions, boot_app0 = support_images(build_if_missing=True)
    return [
        *command,
        "0x0",
        str(bootloader),
        "0x8000",
        str(partitions),
        "0xe000",
        str(boot_app0),
        APP_OFFSET,
        str(firmware),
    ]


def display_command(command: Sequence[str]) -> str:
    import shlex

    return shlex.join(command)


def main() -> int:
    args = parse_args()
    if args.build:
        run_release_build()
        firmware = BUILD_DIR / "firmware.bin"
    elif args.firmware:
        firmware = args.firmware.expanduser().resolve()
    else:
        selected = pick_firmware()
        if selected is None:
            raise SystemExit(
                "No firmware selected. Pass a path explicitly, for example "
                "`python3 scripts/flash_firmware.py bin/firmware-1.1.0.bin`."
            )
        firmware = selected.expanduser().resolve()

    validate_firmware(firmware)
    esptool = find_esptool()
    port = choose_port(args.port, args.dry_run)
    command = flash_command(esptool, firmware, port, args.baud, args.app_only)

    print(f"Firmware: {firmware}")
    print(f"Size:     {firmware.stat().st_size:,} bytes")
    print(f"Port:     {port}")
    print(f"Mode:     {'app-only at 0x10000' if args.app_only else 'safe Inx layout + app0'}")
    print(f"Command:  {display_command(command)}")

    if args.dry_run:
        return 0
    if not args.yes:
        confirmation = input("Type FLASH to continue: ").strip()
        if confirmation != "FLASH":
            print("Cancelled.")
            return 1

    subprocess.run(command, cwd=REPO_ROOT, check=True)
    print("Flash complete. If the reader stays asleep, hold Power for 3–5 seconds.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        raise SystemExit(f"Flash command failed with exit code {error.returncode}.") from error
