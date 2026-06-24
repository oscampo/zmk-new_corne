"""
keyboard_display_ios.py — Pythonista (iPad/iPhone)
Envía texto al display del teclado Corne via BLE usando CoreBluetooth.

Instalar en Pythonista: copiar este archivo a la carpeta de Pythonista y ejecutar.
"""

import cb
import ui
import threading
import time
import json
import urllib.request
import urllib.parse
import unicodedata
from datetime import timezone, datetime

# ── Constantes BLE ────────────────────────────────────────────────────────────
SERVICE_UUID   = '00001523-1212-EFDE-1523-785FEABCD123'
CHAR_UUID      = '00001524-1212-EFDE-1523-785FEABCD123'
KEYBOARD_NAMES = ['corne', 'zmk', 'eyelash']

# ── Rangos de fuente ──────────────────────────────────────────────────────────
_FONT_RANGES = [
    (0x20,   0x7F),
    (0xA1,   0xFF),
    (0xE0A0, 0xE0D4),
    (0xE200, 0xE2A9),
    (0xE300, 0xE3FF),
    (0xEE00, 0xEE0F),
    (0xF000, 0xF2E0),
]
_EXTRA_MAP = str.maketrans({'ß':'ss','æ':'ae','Æ':'AE','œ':'oe','Œ':'OE'})

def to_display(text):
    text = text.translate(_EXTRA_MAP)
    result = []
    for ch in text:
        cp = ord(ch)
        if any(lo <= cp <= hi for lo, hi in _FONT_RANGES) or ch in '\n\r\x01':
            result.append(ch)
        else:
            nfd = unicodedata.normalize('NFD', ch)
            base = nfd[0]
            if any(lo <= ord(base) <= hi for lo, hi in _FONT_RANGES):
                result.append(base)
    return ''.join(result)

# ── Pomodoro ──────────────────────────────────────────────────────────────────
_PB_FIRST       = ''
_PB_MID         = ''
_PB_LAST        = ''
_PB_EMPTY       = ''
_PB_FIRST_EMPTY = ''
_PB_END_EMPTY   = ''
_ICON_WORK  = ''  # gavel
_ICON_BREAK = ''  # coffee
_ICON_LONG  = ''  # hourglass

POMODORO_PRESETS = {
    'classic': (25,  5, 4, 15),
    'short':   (15,  3, 4, 10),
    'long':    (50, 10, 3, 20),
}

def _pomo_bar(done, total):
    segs = []
    for i in range(total):
        is_last = i == total - 1
        if i >= done:
            segs.append(_PB_FIRST_EMPTY if i == 0 else (_PB_END_EMPTY if is_last else _PB_EMPTY))
        elif i == 0:
            segs.append(_PB_FIRST)
        elif is_last:
            segs.append(_PB_LAST)
        else:
            segs.append(_PB_MID)
    return ''.join(segs)

def _fmt_time(seconds):
    m, s = divmod(seconds, 60)
    return f'{m:02d}:{s:02d}'

# ── Clima ─────────────────────────────────────────────────────────────────────
_WMO_CONDITIONS = [
    (range(0,   1),  '', 'Sunny'),
    (range(1,   2),  '', 'PCloudy'),
    (range(2,   3),  '', 'Cloudy'),
    (range(3,   4),  '', 'Overcast'),
    (range(45,  49), '', 'Fog'),
    (range(51,  68), '', 'Rain'),
    (range(71,  78), '', 'Snow'),
    (range(80,  83), '', 'Showers'),
    (range(85,  87), '', 'Snowshwrs'),
    (range(95,  96), '', 'Storm'),
    (range(96, 100), '', 'HvyStorm'),
]

def _wmo_icon(code):
    for r, icon, _ in _WMO_CONDITIONS:
        if code in r: return icon
    return ''

def _wmo_label(code):
    for r, _, label in _WMO_CONDITIONS:
        if code in r: return label
    return f'WMO{code}'

def fetch_weather(city=''):
    def _get(url):
        with urllib.request.urlopen(url, timeout=10) as r:
            return json.loads(r.read().decode('utf-8'))
    if city:
        geo = _get('https://geocoding-api.open-meteo.com/v1/search'
                   f'?name={urllib.parse.quote(city)}&count=1&language=en&format=json')
        results = geo.get('results')
        if not results:
            raise ValueError(f'Ciudad no encontrada: {city!r}')
        r0 = results[0]
        lat, lon, name = r0['latitude'], r0['longitude'], r0.get('name', city)
    else:
        loc = _get('https://ipapi.co/json/')
        lat, lon, name = loc['latitude'], loc['longitude'], loc.get('city', '?')
    wx = _get(f'https://api.open-meteo.com/v1/forecast'
              f'?latitude={lat}&longitude={lon}'
              f'&current_weather=true&temperature_unit=celsius')
    cw = wx['current_weather']
    return {'city': name, 'temp_c': cw['temperature'], 'wmo': int(cw['weathercode'])}

# ── NFL ───────────────────────────────────────────────────────────────────────
_NFL_TROPHY = ''
_NFL_BOLT   = ''
NFL_URL = 'https://site.api.espn.com/apis/site/v2/sports/football/nfl/scoreboard?limit=100'

def _espn_get(url):
    with urllib.request.urlopen(url, timeout=10) as r:
        return json.loads(r.read().decode('utf-8'))

def _parse_games(data, team_filter=''):
    week_num = data.get('week', {}).get('number', '')
    games = []
    for event in data.get('events', []):
        comp = event.get('competitions', [{}])[0]
        competitors = comp.get('competitors', [])
        if len(competitors) < 2: continue
        teams = {}
        for c in competitors:
            side = c.get('homeAway', 'home')
            teams[side] = {
                'abbr':  c.get('team', {}).get('abbreviation', '???').upper(),
                'score': c.get('score', ''),
            }
        away, home = teams.get('away', {}), teams.get('home', {})
        st = comp.get('status', {}).get('type', {})
        games.append({
            'away': away.get('abbr','???'), 'home': home.get('abbr','???'),
            'away_score': away.get('score',''), 'home_score': home.get('score',''),
            'status_detail': st.get('shortDetail',''),
            'status_state':  st.get('state','pre'),
            'week': f'Wk{week_num}' if week_num else '',
        })
    if team_filter:
        tf = team_filter.upper()
        games = [g for g in games if tf in (g['away'], g['home'])]
    return games

def fetch_nfl_results(team_filter=''):
    try:
        data = _espn_get(NFL_URL)
        season     = data.get('season',{}).get('year',2024) or 2024
        week_num   = data.get('week',{}).get('number',18) or 18
        seasontype = data.get('season',{}).get('type',2) or 2
        for step in range(25):
            if step == 0:
                games = _parse_games(data, team_filter)
            else:
                wk, st, yr = week_num - step, seasontype, season
                if wk < 1: yr -= 1; st = 3; wk = 5
                games = _parse_games(
                    _espn_get(f'{NFL_URL}&season={yr}&seasontype={st}&week={wk}'),
                    team_filter)
            final = [g for g in games if g['status_state'] == 'post']
            if final: return final
    except Exception as e:
        print(f'NFL error: {e}')
    return []

def format_nfl_game(game):
    away, home, state = game['away'], game['home'], game['status_state']
    if state == 'post':
        return f"{away}  {home}\n{game['away_score']}  {game['home_score']}\n{_NFL_TROPHY} Final"
    elif state == 'in':
        return f"{_NFL_BOLT}{away}-{home}\n{game['status_detail'][:8]}\n {game['away_score']}-{game['home_score']}"
    else:
        parts = game['status_detail'].split(' - ', 1)
        date_s = parts[0].strip()
        time_s = ''
        if len(parts) > 1:
            t = parts[1].strip().split()
            if len(t) >= 2: time_s = f'{t[0]}{t[1][0].lower()}'
        return f'{away}  {home}\n{date_s}\n{time_s}'

# ── Reloj ─────────────────────────────────────────────────────────────────────
def _clock_cmd():
    local_now = datetime.now().astimezone()
    offset_s  = int(local_now.utcoffset().total_seconds())
    local_unix = int(datetime.now(timezone.utc).timestamp()) + offset_s
    return f'T:{local_unix}:H'

# ── Recuperar periféricos ya conectados al sistema (objc_util) ───────────────
def _retrieve_connected_keyboard():
    """
    Use CBCentralManager.retrieveConnectedPeripherals to find the keyboard
    even when iOS hides it from normal BLE scans (because it's a HID device).
    Returns a cb-compatible peripheral or None.
    """
    try:
        from objc_util import ObjCClass, ObjCInstance, ns
        CBUUID          = ObjCClass('CBUUID')
        CBCentralManager = ObjCClass('CBCentralManager')

        # A temporary manager is enough to call retrieveConnectedPeripherals
        tmp = CBCentralManager.alloc().init()
        uuid      = CBUUID.UUIDWithString_(SERVICE_UUID)
        found     = tmp.retrieveConnectedPeripheralsWithServices_([uuid])
        peripherals = list(found) if found else []

        for p_objc in peripherals:
            name = str(p_objc.name() or '')
            uid  = str(p_objc.identifier())
            print(f'Recuperado (objc): {name}  {uid}')
            # Wrap as Pythonista cb peripheral so cb.connect_peripheral accepts it
            return ObjCInstance(p_objc)

    except Exception as e:
        print(f'objc_util retrieve falló: {e}')
    return None


# ── Delegate CoreBluetooth ────────────────────────────────────────────────────
class KeyboardDelegate:
    def __init__(self, app):
        self.app       = app
        self.peripheral = None
        self.char       = None
        self._ready     = threading.Event()

    # *args makes callbacks safe across Pythonista versions with different signatures
    def did_update_state(self, *args):
        self.app._try_retrieve()

    def did_discover_peripheral(self, p, *args):
        name = p.name or '(sin nombre)'
        print(f'BLE: {name}  |  {p.uuid}')
        name_lower = name.lower()
        self.app._set_status(f'Visto: {name}')
        if any(k in name_lower for k in KEYBOARD_NAMES):
            self.peripheral = p
            cb.stop_scan()
            self.app._set_status(f'Conectando a {name}...')
            cb.connect_peripheral(p)

    def did_connect_peripheral(self, p, *args):
        self.app._set_status(f'Conectado: {p.name} ✓')
        self.app._set_connected(True)
        p.discover_services()

    def did_fail_to_connect_peripheral(self, p, *args):
        self.app._set_status('Error al conectar — reintentando...')
        self.peripheral = None
        cb.scan_for_peripherals()

    def did_disconnect_peripheral(self, p, *args):
        self.app._set_status('Desconectado — escaneando...')
        self.app._set_connected(False)
        self.peripheral = None
        self.char = None
        self._ready.clear()
        cb.scan_for_peripherals()

    def did_discover_services(self, p, *args):
        for svc in p.services:
            if SERVICE_UUID in svc.uuid.upper():
                p.discover_characteristics(svc)

    def did_discover_characteristics(self, svc, *args):
        for char in svc.characteristics:
            if CHAR_UUID in char.uuid.upper():
                self.char = char
                self._ready.set()
                break

    def write(self, text):
        if not self._ready.wait(timeout=12):
            self.app._set_status('Error: teclado no listo')
            return False
        if not self.peripheral or not self.char:
            return False
        data = to_display(text).encode('utf-8')[:64]
        self.peripheral.write_characteristic_value(data, self.char, False)
        return True

# ── Interfaz de usuario ───────────────────────────────────────────────────────
class KeyboardApp(ui.View):

    BG     = '#111827'
    BG_BTN = '#1f2937'
    BLUE   = '#1d4ed8'
    GREEN  = '#065f46'
    RED    = '#7f1d1d'
    ORANGE = '#7c2d12'
    GRAY   = '#374151'

    def __init__(self):
        self.delegate   = KeyboardDelegate(self)
        self._stop_evt  = threading.Event()
        self._bg_thread = None
        self._build_ui()
        # BT se inicia después de que la vista sea visible
        ui.delay(self._start_ble, 0.3)

    def _start_ble(self):
        self._set_status('Buscando teclado...')
        cb.set_central_delegate(self.delegate)
        ui.delay(self._try_retrieve, 0.8)

    def _try_retrieve(self):
        """Try to find keyboard via retrieveConnectedPeripherals (works for HID-connected devices)."""
        p = _retrieve_connected_keyboard()
        if p is not None:
            self.delegate.peripheral = p
            self._set_status(f'Conectando a {p.name}...')
            cb.connect_peripheral(p)
        else:
            self._set_status('Escaneando...')
            cb.scan_for_peripherals()

    # ── Construcción UI ───────────────────────────────────────────────────────
    def _build_ui(self):
        self.background_color = self.BG
        self.name = 'Corne Display'

        W, PAD, GAP, BH = 380, 12, 8, 54

        # ── Indicador de conexión + status ────────────────────────────────────
        self._dot = ui.Label(frame=(PAD, PAD, 16, 16))
        self._dot.text = '●'
        self._dot.font = ('<system>', 14)
        self._dot.text_color = '#ef4444'
        self.add_subview(self._dot)

        self._lbl_status = ui.Label(frame=(PAD+20, PAD, W-PAD-20, 20))
        self._lbl_status.text = 'Iniciando Bluetooth...'
        self._lbl_status.text_color = '#9ca3af'
        self._lbl_status.font = ('<system>', 13)
        self.add_subview(self._lbl_status)

        # ── Modo activo ───────────────────────────────────────────────────────
        self._lbl_mode = ui.Label(frame=(PAD, PAD+22, W-PAD*2, 24))
        self._lbl_mode.text = ''
        self._lbl_mode.text_color = '#f3f4f6'
        self._lbl_mode.font = ('<system-bold>', 15)
        self.add_subview(self._lbl_mode)

        y = PAD + 54
        bw2 = (W - PAD*2 - GAP) // 2
        bw3 = (W - PAD*2 - GAP*2) // 3

        def btn(title, action, x, yy, w, color=None):
            b = ui.Button(frame=(x, yy, w, BH))
            b.title = title
            b.background_color = color or self.BG_BTN
            b.tint_color = '#f9fafb'
            b.corner_radius = 10
            b.font = ('<system-bold>', 15)
            b.action = action
            self.add_subview(b)
            return b

        # Fila 1: Reloj / Limpiar
        btn('Reloj', self._do_clock, PAD, y, bw2, self.BLUE)
        btn('Limpiar', self._do_clear, PAD + bw2 + GAP, y, bw2, self.GRAY)
        y += BH + GAP

        # Fila 2: Campo de ciudad
        self._tf_city = ui.TextField(frame=(PAD, y, W-PAD*2, BH))
        self._tf_city.placeholder = 'Ciudad para clima (vacío = auto)'
        self._tf_city.background_color = '#1f2937'
        self._tf_city.text_color = '#f9fafb'
        self._tf_city.tint_color = '#60a5fa'
        self._tf_city.corner_radius = 10
        self._tf_city.font = ('<system>', 15)
        self.add_subview(self._tf_city)
        y += BH + GAP

        # Fila 3: Clima
        btn('Clima', self._do_weather, PAD, y, W-PAD*2, self.GREEN)
        y += BH + GAP

        # Fila 4: Pomodoro presets
        btn('Pomo 25/5', self._do_pomo_classic, PAD,           y, bw3, self.ORANGE)
        btn('Pomo 15/3', self._do_pomo_short,   PAD+bw3+GAP,   y, bw3, self.ORANGE)
        btn('Pomo 50/10',self._do_pomo_long,    PAD+bw3*2+GAP*2,y,bw3, self.ORANGE)
        y += BH + GAP

        # Fila 5: Detener / NFL
        btn('Detener', self._do_stop, PAD, y, bw2, self.RED)
        btn('NFL', self._do_nfl, PAD + bw2 + GAP, y, bw2, '#14532d')
        y += BH + GAP

        # Fila 6: UUID/dirección manual
        self._tf_addr = ui.TextField(frame=(PAD, y, W-PAD*2, BH))
        self._tf_addr.placeholder = 'UUID del teclado (opcional, para conectar directo)'
        self._tf_addr.background_color = '#1f2937'
        self._tf_addr.text_color = '#f9fafb'
        self._tf_addr.tint_color = '#60a5fa'
        self._tf_addr.corner_radius = 10
        self._tf_addr.font = ('<system>', 13)
        self.add_subview(self._tf_addr)
        y += BH + GAP

        # Fila 7: Reconectar
        btn('Reconectar BLE', self._do_reconnect, PAD, y, W-PAD*2, self.GRAY)
        y += BH + GAP

        self.frame = (0, 0, W, y + PAD)

    # ── Actualizaciones de UI (thread-safe) ───────────────────────────────────
    def _set_status(self, text):
        self._lbl_status.text = text

    def _set_mode(self, text):
        self._lbl_mode.text = text

    def _set_connected(self, connected):
        self._dot.text_color = '#22c55e' if connected else '#ef4444'

    # ── Control de hilo de fondo ──────────────────────────────────────────────
    def _stop_bg(self):
        self._stop_evt.set()
        if self._bg_thread and self._bg_thread.is_alive():
            self._bg_thread.join(timeout=3)
        self._stop_evt.clear()

    def _run_bg(self, fn):
        self._stop_bg()
        self._bg_thread = threading.Thread(target=fn, daemon=True)
        self._bg_thread.start()

    def _write(self, text):
        return self.delegate.write(text)

    # ── Acciones de botones ───────────────────────────────────────────────────
    def _do_clock(self, sender):
        def _go():
            self._set_mode('Modo: Reloj')
            self._write(_clock_cmd())
        self._run_bg(_go)

    def _do_clear(self, sender):
        def _go():
            self._set_mode('')
            self._write('')
        self._run_bg(_go)

    def _do_stop(self, sender):
        self._stop_bg()
        def _go():
            self._write(_clock_cmd())
            self._set_mode('Modo: Reloj')
        threading.Thread(target=_go, daemon=True).start()

    def _do_weather(self, sender):
        city = (self._tf_city.text or '').strip()
        def _go():
            self._set_mode('Obteniendo clima...')
            try:
                data   = fetch_weather(city)
                temp   = f"{data['temp_c']:.0f}°C"
                icon   = _wmo_icon(data['wmo'])
                label  = _wmo_label(data['wmo'])
                name   = data['city'][:12]
                self._write(f'{name}\n{temp}\n{label}\x01{icon}')
                self._set_mode(f'Clima: {name} {temp} {label}')
            except Exception as e:
                self._set_mode(f'Error clima: {e}')
        self._run_bg(_go)

    def _do_pomo(self, preset):
        work, brk, cycles, long_brk = POMODORO_PRESETS[preset]
        def _go():
            for cycle in range(1, cycles + 1):
                # Trabajo
                bar = _pomo_bar(cycle - 1, cycles)
                for remaining in range(work * 60, -1, -1):
                    if self._stop_evt.is_set(): return
                    self._write(f'{_fmt_time(remaining)}\n {bar}\x01{_ICON_WORK}')
                    self._set_mode(f'Trabajo {cycle}/{cycles}  {_fmt_time(remaining)}')
                    if remaining: time.sleep(1)
                if self._stop_evt.is_set(): return
                # Descanso
                is_last = cycle == cycles
                brk_min  = long_brk if (is_last and long_brk) else brk
                brk_icon = _ICON_LONG if (is_last and long_brk) else _ICON_BREAK
                bar2 = _pomo_bar(cycle, cycles)
                for remaining in range(brk_min * 60, -1, -1):
                    if self._stop_evt.is_set(): return
                    self._write(f'{_fmt_time(remaining)}\n {bar2}\x01{brk_icon}')
                    self._set_mode(f'Descanso {cycle}/{cycles}  {_fmt_time(remaining)}')
                    if remaining: time.sleep(1)
            self._write(_clock_cmd())
            self._set_mode('Pomodoro completado ✓')
        self._run_bg(_go)

    def _do_pomo_classic(self, sender): self._do_pomo('classic')
    def _do_pomo_short(self, sender):   self._do_pomo('short')
    def _do_pomo_long(self, sender):    self._do_pomo('long')

    def _do_reconnect(self, sender):
        self.delegate.peripheral = None
        self.delegate.char = None
        self.delegate._ready.clear()
        self._set_connected(False)
        try:
            cb.stop_scan()
        except Exception:
            pass
        self._try_retrieve()

    def _do_nfl(self, sender):
        def _go():
            self._set_mode('Consultando NFL...')
            games = fetch_nfl_results()
            if not games:
                self._set_mode('Sin partidos NFL disponibles')
                return
            wk = games[0].get('week','')
            self._set_mode(f'NFL {wk} — {len(games)} partidos')
            for game in games:
                if self._stop_evt.is_set(): break
                self._write(format_nfl_game(game))
                time.sleep(5)
            self._write(_clock_cmd())
            self._set_mode('NFL completado')
        self._run_bg(_go)


# ── Lanzar ────────────────────────────────────────────────────────────────────
if __name__ == '__main__':
    app = KeyboardApp()
    app.present('sheet')
