#!/usr/bin/env python3
"""
keyboard_display.py — Send any text to the ZMK Corne keyboard display via BLE.

Requirements:
    pip install bleak

Usage examples:
    # Send a simple message
    python keyboard_display.py "Hola mundo"

    # Clear the display
    python keyboard_display.py --clear

    # Scan for keyboards without sending anything
    python keyboard_display.py --scan

    # Specify keyboard address directly (skip scan)
    python keyboard_display.py --address AA:BB:CC:DD:EE:FF "texto"

    # Watch mode: read lines from stdin and send each one
    echo "ARG 1 - 0 FRA" | python keyboard_display.py --watch

    # Pipe from any command
    curl -s 'https://api.example.com/score' | python keyboard_display.py --watch
"""

import asyncio
import argparse
import sys
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

# Must match config/ble_display_service.c
SERVICE_UUID = "00001523-1212-efde-1523-785feabcd123"
CHAR_UUID    = "00001524-1212-efde-1523-785feabcd123"

# Keywords used to identify the keyboard in BLE scan results
KEYBOARD_NAMES = ["zmk", "corne", "eyelash"]


async def find_keyboard(timeout: float = 6.0):
    """Scan for keyboards by name or service UUID.

    Important: if the keyboard is already connected to the OS as an HID device
    it may not be advertising, so scanning can return nothing even though the
    keyboard is reachable.  In that case use --address with the paired address.
    """
    print(f"Buscando teclado ({timeout}s)...")

    # Scan all devices — ZMK does NOT include custom 128-bit UUIDs in its
    # advertisement packet, so filtering by SERVICE_UUID never works here.
    all_devices = await BleakScanner.discover(timeout=timeout)

    # Filter by name keywords
    by_name = [
        d for d in all_devices
        if d.name and any(k in d.name.lower() for k in KEYBOARD_NAMES)
    ]
    if by_name:
        return by_name

    # On Windows the keyboard may already be paired/connected and therefore
    # not advertising.  Try to enumerate paired BLE devices via the OS.
    try:
        import platform
        if platform.system() == "Windows":
            from bleak.backends.winrt.scanner import BleakScannerWinRT  # type: ignore
            paired = await BleakScannerWinRT.find_device_by_filter(
                lambda d, _: d.name and any(k in d.name.lower() for k in KEYBOARD_NAMES),
                timeout=2.0,
            )
            if paired:
                return [paired]
    except Exception:
        pass

    return []


async def send_text(address: str, text: str) -> bool:
    """Connect to the keyboard and write text to the display characteristic."""
    print(f"Conectando a {address}...")
    try:
        async with BleakClient(address, timeout=10.0) as client:
            if not client.is_connected:
                print("Error: no se pudo conectar.")
                return False

            # Encode as UTF-8, max 64 bytes (matches TEXT_MAX_LEN in firmware)
            data = text.encode("utf-8")[:64]
            await client.write_gatt_char(CHAR_UUID, data, response=False)
            display = text if len(text) <= 40 else text[:37] + "..."
            print(f"✓ Enviado: {display!r}")
            return True

    except BleakError as e:
        print(f"BLE error: {e}")
        return False


async def main():
    parser = argparse.ArgumentParser(
        description="Envía texto al display del teclado Corne via BLE.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "text", nargs="?",
        help="Texto a mostrar en el display del teclado (máx. 64 caracteres).",
    )
    parser.add_argument(
        "--scan", action="store_true",
        help="Escanear y listar teclados disponibles, luego salir.",
    )
    parser.add_argument(
        "--clear", action="store_true",
        help="Limpiar el texto del display.",
    )
    parser.add_argument(
        "--watch", action="store_true",
        help="Modo continuo: lee líneas desde stdin y envía cada una.",
    )
    parser.add_argument(
        "--address", metavar="ADDR",
        help="Dirección BLE del teclado (omite el escaneo).",
    )
    args = parser.parse_args()

    # ── Resolve keyboard address ──────────────────────────────────────────────
    address = args.address
    if not address:
        devices = await find_keyboard()
        if not devices:
            print(
                "No se encontró ningún teclado.\n"
                "Verifica que:\n"
                "  • El teclado esté encendido y el firmware nuevo esté flasheado\n"
                "  • BLE esté habilitado en el teclado\n"
                "  • Estés dentro del rango Bluetooth"
            )
            sys.exit(1)

        if len(devices) == 1:
            address = devices[0].address
            print(f"Teclado encontrado: {devices[0].name} ({address})")
        else:
            print("Múltiples teclados encontrados:")
            for i, d in enumerate(devices):
                print(f"  [{i}] {d.name} — {d.address}")
            idx = int(input("Selecciona [0]: ") or 0)
            address = devices[idx].address

    if args.scan:
        return

    # ── Send ──────────────────────────────────────────────────────────────────
    if args.clear:
        await send_text(address, "")

    elif args.watch:
        print("Modo watch activo. Envía líneas (Ctrl-C para salir)...")
        try:
            for line in sys.stdin:
                text = line.rstrip("\n")
                await send_text(address, text)
        except KeyboardInterrupt:
            print("\nSaliendo.")

    elif args.text is not None:
        await send_text(address, args.text)

    else:
        parser.print_help()


if __name__ == "__main__":
    asyncio.run(main())
