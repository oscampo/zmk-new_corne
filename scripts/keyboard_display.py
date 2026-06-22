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


async def find_paired_windows():
    """
    On Windows, paired BLE devices are not advertising so BleakScanner won't
    find them.  Use PowerShell to call WinRT DeviceInformation::FindAllAsync
    (Python winrt bindings don't expose static methods) and return a list of
    (name, device_id) tuples matching KEYBOARD_NAMES.
    """
    import subprocess

    ps = r"""
$null = [Windows.Devices.Enumeration.DeviceInformation,Windows.Devices.Enumeration,ContentType=WindowsRuntime]
$sel = "System.Devices.Aep.ProtocolId:=`"{bb7bb05e-5972-42b5-94fc-76eaa7084d49}`" AND System.Devices.Aep.IsPaired:=System.StructuredQueryType.Boolean#True"
$kind = [Windows.Devices.Enumeration.DeviceInformationKind]::AssociationEndpoint
try {
    $op = [Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($sel, $null, $kind)
    $devices = $op.AsTask().Result
    foreach ($d in $devices) {
        if ($d.Name) { Write-Output "$($d.Name)|$($d.Id)" }
    }
} catch {
    Write-Error $_
}
"""
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", "-"],
            input=ps,
            capture_output=True, text=True, timeout=20,
        )
        results = []
        for line in r.stdout.strip().splitlines():
            if "|" in line:
                name, dev_id = line.split("|", 1)
                name, dev_id = name.strip(), dev_id.strip()
                if name and any(k in name.lower() for k in KEYBOARD_NAMES):
                    results.append((name, dev_id))
        if not results and r.stderr.strip():
            print(f"[debug] PowerShell: {r.stderr.strip()[:300]}")
        return results
    except Exception as e:
        print(f"[debug] PowerShell lookup failed: {e}")
        return []


async def find_keyboard(timeout: float = 6.0):
    """Scan for keyboards by name, with fallback to Windows paired-device lookup."""
    print(f"Buscando teclado ({timeout}s)...")

    all_devices = await BleakScanner.discover(timeout=timeout)
    by_name = [
        d for d in all_devices
        if d.name and any(k in d.name.lower() for k in KEYBOARD_NAMES)
    ]
    if by_name:
        return by_name

    # Windows: paired devices don't advertise — look them up via PowerShell/WinRT
    import platform
    if platform.system() == "Windows":
        paired = await find_paired_windows()
        if paired:
            # Return fake device-like objects with .name and .address = Windows device ID
            class _Dev:
                def __init__(self, name, dev_id):
                    self.name = name
                    self.address = dev_id  # BleakClient accepts Windows device IDs
            return [_Dev(n, i) for n, i in paired]

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
