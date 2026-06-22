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


def _mac_to_int(mac: str) -> int:
    return int(mac.replace(":", ""), 16)


def _run_ps(script: str, debug: bool = False):
    """Write a PowerShell script to a temp .ps1 file and run it."""
    import subprocess, tempfile, os
    with tempfile.NamedTemporaryFile(mode='w', suffix='.ps1', delete=False,
                                     encoding='utf-8') as f:
        f.write(script)
        ps_file = f.name
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive",
             "-ExecutionPolicy", "Bypass", "-File", ps_file],
            capture_output=True, text=True, timeout=20,
        )
    finally:
        os.unlink(ps_file)
    if r.stderr.strip() and debug:
        print(f"[debug] PowerShell stderr: {r.stderr.strip()[:300]}")
    return r.stdout.strip().splitlines()


def _get_paired_macs_pnp(debug: bool = False) -> list[tuple[str, str]]:
    """Find paired BLE devices via Get-PnpDevice; returns [(name, mac), ...]."""
    ps = r"""
try {
    $devs = Get-PnpDevice | Where-Object { $_.InstanceId -match 'BTHLE\\DEV_' }
    foreach ($d in $devs) {
        if ($d.InstanceId -match 'DEV_([0-9A-Fa-f]{12})') {
            $hex = $Matches[1]
            $mac = ($hex -split '(?<=\G.{2})(?=.)') -join ':'
            Write-Output "DEVICE|$($d.FriendlyName)|$mac"
        }
    }
} catch {
    Write-Output "ERROR|$_"
}
"""
    results = []
    for line in _run_ps(ps, debug=debug):
        if line.startswith("DEVICE|"):
            parts = line.split("|", 2)
            if len(parts) == 3:
                _, name, mac = parts
                name, mac = name.strip(), mac.strip().upper()
                if debug:
                    print(f"[debug] PnP paired: {name!r}  {mac}")
                results.append((name, mac))
    return results


async def send_text_winrt(mac: str, text: str, debug: bool = False) -> bool:
    """
    Send text directly via WinRT GATT — used on Windows when the keyboard is
    already connected as HID and BleakClient can't scan-and-connect to it.
    BluetoothLEDevice.from_bluetooth_address_async() works for paired devices
    even without active advertising.
    """
    try:
        from winrt.windows.devices.bluetooth import BluetoothLEDevice  # type: ignore
        from winrt.windows.devices.bluetooth.genericattributeprofile import (  # type: ignore
            GattCommunicationStatus,
        )
        from winrt.windows.storage.streams import DataWriter  # type: ignore
        import uuid as _uuid

        addr = _mac_to_int(mac)
        dev = await BluetoothLEDevice.from_bluetooth_address_async(addr)
        if not dev:
            if debug:
                print("[debug] WinRT: from_bluetooth_address_async returned None")
            return False

        if debug:
            print(f"[debug] WinRT device: {dev.name}  id={dev.device_id}")

        # Get our custom GATT service
        svc_uuid = _uuid.UUID(SERVICE_UUID)
        svc_result = await dev.get_gatt_services_for_uuid_async(svc_uuid)
        if svc_result.status != GattCommunicationStatus.SUCCESS or not svc_result.services:
            print(f"BLE error: custom GATT service not found (status={svc_result.status})")
            return False

        service = svc_result.services[0]

        # Get our characteristic
        char_uuid = _uuid.UUID(CHAR_UUID)
        char_result = await service.get_characteristics_for_uuid_async(char_uuid)
        if char_result.status != GattCommunicationStatus.SUCCESS or not char_result.characteristics:
            print(f"BLE error: characteristic not found (status={char_result.status})")
            return False

        characteristic = char_result.characteristics[0]

        # Write the text
        data = text.encode("utf-8")[:64]
        writer = DataWriter()
        writer.write_bytes(data)  # winrt DataWriter expects bytes, not list
        buf = writer.detach_buffer()

        status = await characteristic.write_value_with_result_async(buf)
        if status.status == GattCommunicationStatus.SUCCESS:
            display = text if len(text) <= 40 else text[:37] + "..."
            print(f"✓ Enviado: {display!r}")
            return True
        else:
            print(f"BLE error: write failed (status={status.status})")
            return False

    except Exception as e:
        if debug:
            print(f"[debug] WinRT send failed: {e}")
        return False


async def find_keyboard(timeout: float = 6.0, debug: bool = False):
    """Scan for keyboards by name, with fallback to Windows paired-device lookup."""
    print(f"Buscando teclado ({timeout}s)...")

    all_devices = await BleakScanner.discover(timeout=timeout)
    if debug and all_devices:
        for d in all_devices:
            print(f"[debug] BLE scan: {d.name!r}  {d.address}")
    by_name = [
        d for d in all_devices
        if d.name and any(k in d.name.lower() for k in KEYBOARD_NAMES)
    ]
    if by_name:
        return by_name

    import platform
    if platform.system() == "Windows":
        print("Buscando dispositivos BLE emparejados (Windows)...")
        paired = _get_paired_macs_pnp(debug=debug)
        keyboards = [(n, m) for n, m in paired if any(k in n.lower() for k in KEYBOARD_NAMES)]
        if keyboards:
            class _Dev:
                def __init__(self, name, mac):
                    self.name = name
                    self.address = mac
                    self._is_paired_windows = True
            return [_Dev(n, m) for n, m in keyboards]

    return []


async def send_text(address: str, text: str, paired_windows: bool = False,
                    debug: bool = False) -> bool:
    """Connect to the keyboard and write text to the display characteristic."""
    # For Windows paired devices, use WinRT directly (BleakClient can't connect
    # to already-connected HID devices that aren't advertising)
    if paired_windows:
        return await send_text_winrt(address, text, debug=debug)

    print(f"Conectando a {address}...")
    try:
        async with BleakClient(address, timeout=10.0) as client:
            if not client.is_connected:
                print("Error: no se pudo conectar.")
                return False
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
    parser.add_argument(
        "--debug", action="store_true",
        help="Mostrar información de depuración BLE.",
    )
    args = parser.parse_args()

    # ── Resolve keyboard address ──────────────────────────────────────────────
    address = args.address
    paired_windows = False

    if not address:
        devices = await find_keyboard(debug=args.debug)
        if not devices:
            print(
                "No se encontró ningún teclado.\n"
                "Verifica que:\n"
                "  • El teclado esté encendido y el firmware nuevo esté flasheado\n"
                "  • BLE esté habilitado en el teclado\n"
                "  • Estés dentro del rango Bluetooth\n"
                "\nPrueba con --debug para ver todos los dispositivos encontrados."
            )
            sys.exit(1)

        if len(devices) == 1:
            address = devices[0].address
            paired_windows = getattr(devices[0], "_is_paired_windows", False)
            print(f"Teclado encontrado: {devices[0].name} ({address})")
        else:
            print("Múltiples teclados encontrados:")
            for i, d in enumerate(devices):
                print(f"  [{i}] {d.name} — {d.address}")
            idx = int(input("Selecciona [0]: ") or 0)
            address = devices[idx].address
            paired_windows = getattr(devices[idx], "_is_paired_windows", False)

    if args.scan:
        return

    # ── Send ──────────────────────────────────────────────────────────────────
    if args.clear:
        await send_text(address, "", paired_windows=paired_windows, debug=args.debug)

    elif args.watch:
        print("Modo watch activo. Envía líneas (Ctrl-C para salir)...")
        try:
            for line in sys.stdin:
                text = line.rstrip("\n")
                await send_text(address, text, paired_windows=paired_windows,
                                debug=args.debug)
        except KeyboardInterrupt:
            print("\nSaliendo.")

    elif args.text is not None:
        await send_text(address, args.text, paired_windows=paired_windows,
                        debug=args.debug)

    else:
        parser.print_help()


if __name__ == "__main__":
    asyncio.run(main())
