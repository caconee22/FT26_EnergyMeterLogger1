#!/usr/bin/env python3
import argparse
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("pyserial is required. Install it with:")
    print("  python -m pip install -r tools/requirements.txt")
    sys.exit(1)


DEFAULT_BAUD = 921600
DEFAULT_CHUNK_SIZE = 512
DEFAULT_PORT = "COM2"
UID = "12345678-90ABCDEF-00000001"


def is_log_file(path: Path) -> bool:
    suffix = path.suffix.lower()
    return suffix == ".log" or (suffix.startswith(".log") and suffix[4:].isdigit())


def scan_sd_dir(sd_dir: Path):
    if not sd_dir.is_dir():
        raise FileNotFoundError(f"SD folder not found: {sd_dir}")

    files = [p for p in sd_dir.iterdir() if p.is_file() and is_log_file(p)]
    files.sort(key=lambda p: p.name, reverse=True)

    return [
        {
            "index": index,
            "name": path.name,
            "size": path.stat().st_size,
            "path": path,
        }
        for index, path in enumerate(files)
    ]


def device_time_text() -> str:
    now = datetime.now()
    return (
        f"{now.year:04d}-{now.month:02d}-{now.day:02d}-"
        f"{now.hour:02d}-{now.minute:02d}-{now.second:02d}-{now.microsecond // 1000:03d}"
    )


def write_ascii(port: serial.Serial, text: str) -> None:
    port.write(text.encode("ascii"))
    port.flush()


def read_line(port: serial.Serial) -> str:
    raw = port.readline()
    if not raw:
        return ""
    return raw.decode("ascii", errors="replace").strip()


def send_file(port: serial.Serial, file_entry: dict, chunk_size: int) -> None:
    size = file_entry["size"]
    write_ascii(port, f"OK READ {file_entry['index']} {size}\r\n")

    sent = 0
    with file_entry["path"].open("rb") as handle:
        while sent < size:
            chunk = handle.read(chunk_size)
            if not chunk:
                raise RuntimeError(f"Unexpected end of file: {file_entry['name']}")

            write_ascii(port, f"CHUNK {sent} {len(chunk)}\r\n")
            port.write(chunk)
            port.flush()
            sent += len(chunk)

    write_ascii(port, f"OK DONE {sent}\r\n")


def handle_command(port: serial.Serial, line: str, args) -> None:
    print(f"< {line}", flush=True)

    if line.upper().startswith("HELLO"):
        files = scan_sd_dir(args.sd_dir)
        response = f"OK HELLO {UID} {device_time_text()} sd=1 rtc=1 files={len(files)}\r\n"
        write_ascii(port, response)
        print(f"> {response.strip()}", flush=True)
        return

    if line.upper().startswith("LIST"):
        files = scan_sd_dir(args.sd_dir)
        write_ascii(port, f"OK LIST {len(files)}\r\n")
        for entry in files:
            write_ascii(port, f"{entry['index']} {entry['name']} {entry['size']}\r\n")
        write_ascii(port, "END\r\n")
        print(f"> OK LIST {len(files)} ... END", flush=True)
        return

    parts = line.split()
    if len(parts) == 2 and parts[0].upper() == "READ" and parts[1].isdigit():
        files = scan_sd_dir(args.sd_dir)
        index = int(parts[1])
        entry = next((item for item in files if item["index"] == index), None)
        if entry is None:
            write_ascii(port, "ERR READ INDEX\r\n")
            print("> ERR READ INDEX", flush=True)
            return

        print(f"> streaming {entry['name']} ({entry['size']} bytes)", flush=True)
        send_file(port, entry, args.chunk_size)
        print(f"> OK DONE {entry['size']}", flush=True)
        return

    if line.upper().startswith("RTC"):
        write_ascii(port, "OK RTC\r\n")
        print("> OK RTC", flush=True)
        return

    if line.upper().startswith("DEL"):
        parts = line.split()
        if len(parts) != 2 or not parts[1].isdigit():
            write_ascii(port, "ERR DEL INDEX\r\n")
            print("> ERR DEL INDEX", flush=True)
            return

        files = scan_sd_dir(args.sd_dir)
        index = int(parts[1])
        entry = next((item for item in files if item["index"] == index), None)
        if entry is None:
            write_ascii(port, "ERR DEL INDEX\r\n")
            print("> ERR DEL INDEX", flush=True)
            return

        try:
            entry["path"].unlink()
        except OSError as error:
            write_ascii(port, "ERR DEL REMOVE\r\n")
            print(f"> ERR DEL REMOVE {error}", flush=True)
            return

        write_ascii(port, f"OK DEL {index} {entry['name']}\r\n")
        print(f"> OK DEL {index} {entry['name']}", flush=True)
        return

    write_ascii(port, "ERR COMMAND\r\n")
    print("> ERR COMMAND", flush=True)


def print_dry_run(args) -> None:
    files = scan_sd_dir(args.sd_dir)
    print(f"Virtual SD: {args.sd_dir}")
    print(f"OK HELLO {UID} {device_time_text()} sd=1 rtc=1 files={len(files)}")
    print(f"OK LIST {len(files)}")
    for entry in files:
        print(f"{entry['index']} {entry['name']} {entry['size']}")
    print("END")


def print_ports() -> None:
    ports = list(list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return
    for port in ports:
        desc = port.description or ""
        print(f"{port.device}\t{desc}")


def parse_args():
    parser = argparse.ArgumentParser(description="FT26 virtual serial device backed by a local SD folder.")
    parser.add_argument("--port", default=DEFAULT_PORT, help="Serial port opened by this virtual device.")
    parser.add_argument("--sd-dir", default="./samples", type=Path, help="Folder to expose as the SD card root.")
    parser.add_argument("--baud", default=DEFAULT_BAUD, type=int, help="Serial baud rate.")
    parser.add_argument("--chunk-size", default=DEFAULT_CHUNK_SIZE, type=int, help="READ transfer chunk size.")
    parser.add_argument("--delete-enabled", action="store_true", help="Compatibility option; DEL index removes only the selected file.")
    parser.add_argument("--dry-run", action="store_true", help="Print HELLO/LIST output without opening a port.")
    parser.add_argument("--list-ports", action="store_true", help="List available serial ports and exit.")
    args = parser.parse_args()
    args.sd_dir = args.sd_dir.resolve()
    return args


def main() -> int:
    args = parse_args()

    if args.list_ports:
        print_ports()
        return 0

    if args.dry_run:
        print_dry_run(args)
        return 0

    files = scan_sd_dir(args.sd_dir)
    print("FT26 virtual device")
    print(f"Port: {args.port} @ {args.baud} bps")
    print(f"SD:   {args.sd_dir}")
    print(f"Files: {len(files)}")
    print("Open the paired COM port from the viewer.")

    with serial.Serial(args.port, args.baud, timeout=0.5, write_timeout=5) as port:
        write_ascii(port, f"COM READY baud={args.baud} sd=1 rtc=1 files={len(files)}\r\n")
        while True:
            line = read_line(port)
            if not line:
                time.sleep(0.01)
                continue
            handle_command(port, line, args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nStopped.")
