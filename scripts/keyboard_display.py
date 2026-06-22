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


def _run_ps(script: str, debug: bool = False):
    """Write PowerShell script to a temp file and run it; return stdout lines."""
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
    if debug:
        print(f"[debug] PowerShell stdout:\n{r.stdout.strip()}")
    if r.stderr.strip():
        print(f"[debug] PowerShell stderr: {r.stderr.strip()[:400]}")
    return r.stdout.strip().splitlines()


async def find_paired_windows(debug: bool = False):
    """
    On Windows, paired BLE devices are not advertising so BleakScanner won't
    find them.  Use WinRT via a temp .ps1 file (WinRT type loading fails when
    piped via stdin) to call BluetoothLEDevice::GetDeviceSelectorFromPairingState
    and DeviceInformation::FindAllAsync, returning the Windows AEP device ID
    that BleakClient needs (not just the MAC address).
    Falls back to Get-PnpDevice + MAC if WinRT is unavailable.
    """
    # Primary: WinRT — returns proper Windows BLE device IDs for BleakClient
    ps_winrt = r"""
try {
    $null = [Windows.Devices.Bluetooth.BluetoothLEDevice,Windows.Devices.Bluetooth,ContentType=WindowsRuntime]
    $null = [Windows.Devices.Enumeration.DeviceInformation,Windows.Devices.Enumeration,ContentType=WindowsRuntime]
    $sel = [Windows.Devices.Bluetooth.BluetoothLEDevice]::GetDeviceSelectorFromPairingState($true)
    $op = [Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($sel)
    $devices = $op.AsTask().Result
    foreach ($d in $devices) {
        Write-Output "DEVICE|$($d.Name)|$($d.Id)"
    }
} catch {
    Write-Output "ERROR|$_"
}
"""
    lines = _run_ps(ps_winrt, debug=debug)
    results = []
    winrt_ok = False
    for line in lines:
        if line.startswith("ERROR|"):
            if debug:
                print(f"[debug] WinRT error: {line[6:]}")
            break
        if line.startswith("DEVICE|"):
            winrt_ok = True
            parts = line.split("|", 2)
            if len(parts) == 3:
                _, name, dev_id = parts
                name, dev_id = name.strip(), dev_id.strip()
                if debug:
                    print(f"[debug] WinRT paired: {name!r}")
                if name and any(k in name.lower() for k in KEYBOARD_NAMES):
                    results.append((name, dev_id))

    if winrt_ok or results:
        return results

    # Fallback: Get-PnpDevice — no WinRT needed, extracts MAC from InstanceId
    if debug:
        print("[debug] WinRT unavailable, falling back to Get-PnpDevice")
    ps_pnp = r"""
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
    lines = _run_ps(ps_pnp, debug=debug)
    for line in lines:
        if line.startswith("DEVICE|"):
            parts = line.split("|", 2)
            if len(parts) == 3:
                _, name, mac = parts
                name, mac = name.strip(), mac.strip().upper()
                if debug:
                    print(f"[debug] PnP paired: {name!r}  {mac}")
                if name and any(k in name.lower() for k in KEYBOARD_NAMES):
                    results.append((name, mac))
    return results


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

    # Windows: paired HID devices don't advertise — look them up via PowerShell
    import platform
    if platform.system() == "Windows":
        print("Buscando dispositivos BLE emparejados (Windows)...")
        paired = await find_paired_windows(debug=debug)
        if paired:
            class _Dev:
                def __init__(self, name, address):
                    self.name = name
                    self.address = address
            return [_Dev(n, a) for n, a in paired]

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
    parser.add_argument(
        "--debug", action="store_true",
        help="Mostrar información de depuración BLE.",
    )
    args = parser.parse_args()

    # ── Resolve keyboard address ──────────────────────────────────────────────
    address = args.address
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
