"""Garden V36.1: outbound MQTT RPC -> fixed local ESP32 HTTP API. Python 3.10+."""
import argparse
import base64
import hashlib
import json
import math
import os
import queue
import re
import ssl
import time
from collections import OrderedDict
from html.parser import HTMLParser
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlsplit
from urllib.request import Request, build_opener, HTTPRedirectHandler, ProxyHandler


class ValidationError(ValueError):
    pass


def number(value, low, high, integer=False):
    if isinstance(value, bool) or value is None or str(value).strip() == '':
        raise ValidationError('Некоректне число')
    try:
        n = float(value)
    except (ValueError, TypeError):
        raise ValidationError('Некоректне число') from None
    if not math.isfinite(n) or not low <= n <= high or (integer and not n.is_integer()):
        raise ValidationError('Число поза дозволеними межами')
    return int(n) if integer else n


def boolean(value):
    if type(value) is not bool:
        raise ValidationError('Очікується перемикач true/false')
    return value


def text(value, maximum, minimum=0):
    if not isinstance(value, str) or not minimum <= len(value.encode('utf-8')) <= maximum or '\x00' in value:
        raise ValidationError('Некоректний текст або довжина')
    return value


def clock(value):
    if not isinstance(value, str) or not re.fullmatch(r'(?:[01]\d|2[0-3]):[0-5]\d', value):
        raise ValidationError('Час має бути ГГ:ХХ')
    return tuple(map(int, value.split(':')))


def weekdays(value):
    if not isinstance(value, list) or len(value) > 7:
        raise ValidationError('Некоректні дні тижня')
    return sorted(set(number(v, 0, 6, True) for v in value))


class Fields(HTMLParser):
    """Parse only inputs/textarea, never execute device scripts or return passwords."""
    def __init__(self, html):
        super().__init__(convert_charrefs=True)
        self.values, self.ids = {}, {}
        self.area = None
        self.feed(html)

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == 'input':
            kind = a.get('type', 'text')
            if kind == 'password':
                return
            value = ('checked' in a) if kind == 'checkbox' else a.get('value', '')
            if a.get('name'):
                self.values[a['name']] = value
            if a.get('id'):
                self.ids[a['id']] = value
        if tag == 'textarea':
            self.area = (a.get('name'), a.get('id'), [])

    def handle_data(self, data):
        if self.area:
            self.area[2].append(data)

    def handle_endtag(self, tag):
        if tag == 'textarea' and self.area:
            name, ident, parts = self.area
            if name:
                self.values[name] = ''.join(parts)
            if ident:
                self.ids[ident] = ''.join(parts)
            self.area = None


def config_from_html(html, state):
    fields = Fields(html)
    v = fields.values

    def n(key, lo, hi):
        if key not in v:
            raise ValidationError('Версія локальної панелі несумісна: відсутнє поле '+key)
        return number(v[key], lo, hi, True)

    def at(h, m):
        return f'{h:02d}:{m:02d}'

    water = []
    for i in range(4):
        p = f'w{i}'
        water.append(dict(enabled=v.get(p+'en') is True,
                          time=at(n(p+'h', 0, 23), n(p+'m', 0, 59)), duration=n(p+'dur', 1, 240),
                          days=[d for d in range(7) if v.get(p+'d'+str(d)) is True],
                          intervalEnabled=v.get(p+'intEn') is True,
                          intervalHours=n(p+'intH', 0, 720), intervalMinutes=n(p+'intM', 0, 59)))
    heat = dict(dayTemp=number(v.get('dayTemp'), 5, 30), nightTemp=number(v.get('nightTemp'), 5, 30),
                dayTime=at(n('dayH', 0, 23), n('dayM', 0, 59)), nightTime=at(n('nightH', 0, 23), n('nightM', 0, 59)))
    light = dict(enabled=v.get('l0en') is True, onTime=at(n('l0onh', 0, 23), n('l0onm', 0, 59)),
                 offTime=at(n('l0offh', 0, 23), n('l0offm', 0, 59)),
                 days=[d for d in range(7) if v.get('l0d'+str(d)) is True])
    if 'rainEn' not in v or 'rainDelay' not in v or 'ds18Enable' not in fields.ids:
        raise ValidationError('Локальна панель не містить повних налаштувань')
    return dict(state=state, heat=heat, water=water, light=light,
                rain=dict(enabled=v['rainEn'] is True, hours=n('rainDelay', 0, 168)),
                extra=fields.ids['ds18Enable'] is True, memo=fields.ids.get('memoText', ''), ssid=state.get('wifi_ssid', ''))


def water_fields(configs):
    out = {}
    if not isinstance(configs, list) or len(configs) != 4:
        raise ValidationError('Потрібні чотири зони')
    for i, c in enumerate(configs):
        p = f'w{i}'
        if boolean(c.get('enabled')):
            out[p+'en'] = '1'
        out[p+'h'], out[p+'m'] = clock(c.get('time'))
        out[p+'dur'] = number(c.get('duration'), 1, 240, True)
        if boolean(c.get('intervalEnabled')):
            out[p+'intEn'] = '1'
        out[p+'intH'] = number(c.get('intervalHours'), 0, 720, True)
        out[p+'intM'] = number(c.get('intervalMinutes'), 0, 59, True)
        for d in weekdays(c.get('days')):
            out[p+'d'+str(d)] = '1'
    return out


def light_fields(c):
    out = {}
    if boolean(c.get('enabled')):
        out['l0en'] = '1'
    out['l0onh'], out['l0onm'] = clock(c.get('onTime'))
    out['l0offh'], out['l0offm'] = clock(c.get('offTime'))
    for d in weekdays(c.get('days')):
        out['l0d'+str(d)] = '1'
    return out


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class Device:
    def __init__(self, url, username='', password=''):
        u = urlsplit(url)
        if u.scheme not in ('http', 'https') or not u.hostname or u.username or u.password or u.query or u.fragment or u.path not in ('', '/'):
            raise ValidationError('ESP_URL має містити лише адресу ESP32, без шляху')
        self.base = url.rstrip('/')
        self.auth = base64.b64encode((username+':'+password).encode()).decode() if password else None
        self.opener = build_opener(ProxyHandler({}), NoRedirect())

    def request(self, path, params=None, post=False):
        # Paths are selected by execute(), never taken from the request envelope.
        encoded = urlencode(params or {}).encode()
        headers = {'Cache-Control': 'no-store'}
        if self.auth:
            headers['Authorization'] = 'Basic '+self.auth
        if post:
            headers['Content-Type'] = 'application/x-www-form-urlencoded'
        req = Request(self.base+path+('?' + encoded.decode() if encoded and not post else ''),
                      data=encoded if post else None, headers=headers)
        try:
            with self.opener.open(req, timeout=8) as r:
                raw = r.read(1024*1024+1)
                if len(raw) > 1024*1024:
                    raise ValidationError('Надто велика відповідь ESP32')
                result = raw.decode('utf-8')
                if 'json' in r.headers.get('Content-Type', ''):
                    result = json.loads(result)
                    if isinstance(result, dict) and result.get('ok') is False:
                        raise ValidationError('ESP32 відхилила зміни')
                return result
        except HTTPError as ex:
            ex.close()
            if ex.code == 303 and post:
                return {'ok': True}
            if ex.code in (401, 403):
                raise ValidationError('ESP32 відхилила доступ. Перевірте пароль панелі або PIN воріт.') from None
            raise ValidationError('Помилка HTTP ESP32: '+str(ex.code)) from None
        except (URLError, TimeoutError, OSError):
            raise ValidationError('ESP32 недоступна у домашній мережі') from None

    def state(self):
        s = self.request('/api/status')
        if not isinstance(s, dict) or not any(v in s.get('firmware', '') for v in ('V36.1', 'V36.2')):
            raise ValidationError('Шлюз потребує прошивку Garden_U V36.1 / V36.2')
        return s

    def config(self):
        return config_from_html(self.request('/'), self.state())

    def execute(self, op, a):
        if not isinstance(a, dict):
            raise ValidationError('Параметри мають бути об’єктом')
        if op == 'config.get':
            return self.config()
        if op == 'heat.mode':
            commands = {'stop': ('off', ''), 'manual-on': ('manual', ''), 'manual-off': ('off', 'manual'),
                        'auto-on': ('auto', ''), 'auto-off': ('off', 'auto')}
            cmd = a.get('command')
            if cmd in ('vacation-on', 'vacation-off'):
                self.request('/api/vacation', {'state': 'on' if cmd.endswith('-on') else 'off'}, True)
            elif cmd in commands:
                mode, selection = commands[cmd]
                self.request('/api/boiler', dict(mode=mode, selection=selection), True)
            else:
                raise ValidationError('Невідомий режим')
        elif op == 'heat.manual':
            target = number(a.get('value'), 5, 30)
            current = number(self.state().get('manual_setpoint'), 5, 30)
            delta = round((target-current)*10)
            if abs(target*2-round(target*2)) > .001 or abs(delta) not in (0, 5):
                raise ValidationError('Температура змінилась: оновіть дані й повторіть крок 0,5 °C')
            if delta:
                self.request('/heatTemp', {'delta': delta, 'ajax': 1})
        elif op == 'heat.auto.save':
            dh, dm = clock(a.get('dayTime')); nh, nm = clock(a.get('nightTime'))
            self.request('/heatingSettings', dict(section='auto', dayTemp=number(a.get('dayTemp'), 5, 30),
                         nightTemp=number(a.get('nightTemp'), 5, 30), dayH=dh, dayM=dm, nightH=nh, nightM=nm), True)
        elif op == 'heat.vacation.save':
            self.request('/heatingSettings', dict(section='vacation', vacTemp=number(a.get('vacTemp'), 5, 30)), True)
        elif op == 'gate.save':
            gi = number(a.get('gate'), 0, 1, True)
            pulse = number(a.get('pulseMs'), 500, 5000, True)
            if pulse not in (500, 1000, 2000, 3000, 5000):
                raise ValidationError('Некоректний імпульс')
            p = dict(gate=gi, name=text(a.get('name', '').strip(), 128, 1),
                     pinEnabled=int(boolean(a.get('pinEnabled'))), reedsEnabled=int(boolean(a.get('reedsEnabled'))), pulseMs=pulse)
            pin = text(a.get('newPin', ''), 12)
            if pin:
                if not re.fullmatch(r'\d{4,12}', pin, re.ASCII):
                    raise ValidationError('PIN має містити 4–12 цифр')
                p['newPin'] = pin
            self.request('/gateSettings', p, True)
        elif op == 'gate.command':
            gi = number(a.get('gate'), 0, 1, True); cmd = a.get('command')
            if cmd not in ('open', 'close'):
                raise ValidationError('Невідома команда воріт')
            self.request('/api/gate', dict(gate=gi, cmd=cmd, pin=text(a.get('pin', ''), 12)), True)
        elif op in ('water.start', 'water.stop', 'water.all'):
            p = {'action': 'stop'}
            if op == 'water.start':
                p = dict(action='start', zone=number(a.get('zone'), 1, 4, True))
            elif op == 'water.all':
                if a.get('action') not in ('all', 'alltogether'):
                    raise ValidationError('Невідомий спосіб поливу')
                p['action'] = a['action']
            self.request('/api/water', p, True)
        elif op == 'water.save':
            index = number(a.get('zone'), 0, 3, True)
            # Validate candidate before reading, then merge into a fresh complete snapshot.
            candidate = a.get('config')
            water_fields([candidate]*4)
            zones = self.config()['water']; zones[index] = candidate
            self.request('/waterSettingsSave', water_fields(zones), True)
        elif op == 'light.set':
            self.request('/api/light', dict(channel=1, state='on' if boolean(a.get('on')) else 'off'), True)
        elif op == 'light.save':
            p = light_fields(a)
            c = self.config()
            # /settings also rewrites water/rain: preserve their current full values.
            p.update(water_fields(c['water']))
            p['rainDelay'] = c['rain']['hours']
            if c['rain']['enabled']:
                p['rainEn'] = '1'
            self.request('/settings', p, True)
        elif op == 'rain.save':
            self.request('/rainSave', dict(enabled=int(boolean(a.get('enabled'))), hours=number(a.get('hours'), 0, 168, True)), True)
        elif op == 'sensors.save':
            sensors = a.get('sensors')
            if not isinstance(sensors, list) or len(sensors) != 4:
                raise ValidationError('Потрібні чотири датчики')
            p = {'source': number(a.get('source'), 0, 4, True)}
            for i, s in enumerate(sensors):
                enabled = boolean(s.get('enabled')); mac = text(s.get('mac', ''), 17).upper()
                if (enabled or mac) and not re.fullmatch(r'(?:[0-9A-F]{2}:){5}[0-9A-F]{2}', mac):
                    raise ValidationError('Некоректна MAC-адреса')
                p.update({f'en{i}': int(enabled), f'name{i}': text(s.get('name'), 32, 1), f'mac{i}': mac})
            self.request('/xiaomiSensors', p, True)
        elif op == 'extra.save':
            self.request('/extraTemp', dict(enabled=int(boolean(a.get('enabled')))))
        elif op == 'memo.save':
            self.request('/memoSave', dict(text=text(a.get('text'), 3000)), True)
        elif op == 'homekit.save':
            self.request('/homekitSave', dict(mode=number(a.get('mode'), 0, 2, True)), True)
            return {'restarting': True}
        elif op == 'wifi.save':
            self.request('/wifiSave', dict(ssid=text(a.get('ssid'), 32, 1), password=text(a.get('password', ''), 64)), True)
            return {'restarting': True}
        else:
            raise ValidationError('Операція не дозволена')
        try:
            return {'state': self.state()}
        except ValidationError:
            # The write succeeded. Do not call it again if a subsequent read fails.
            return {'applied': True, 'awaiting_state': True}


class RpcServer:
    def __init__(self, device):
        self.device = device
        self.seen = OrderedDict()

    def process(self, raw, retained=False, now=None):
        now = time.time()*1000 if now is None else now
        if retained or len(raw) > 16384:
            return None
        try:
            r = json.loads(raw)
            if not isinstance(r, dict) or r.get('version') != 1:
                return None
            client, ident = r.get('client', ''), r.get('id', '')
            if not isinstance(client, str) or not re.fullmatch(r'pwa-[a-f0-9]{32}', client):
                return None
            if not isinstance(ident, str) or not re.fullmatch(r'[a-f0-9-]{36}', ident):
                return None
            created, expires = r.get('created'), r.get('expires')
            if type(created) not in (int, float) or type(expires) not in (int, float):
                return None
            if not all(map(math.isfinite, (created, expires))) or not created-5000 <= now <= expires or not 0 < expires-created <= 20000:
                return None
            key = (client, ident)
            digest = hashlib.sha256(raw.encode()).digest()
            # Entries live beyond the maximum request lifetime; never evict a live ID.
            for old in list(self.seen):
                if self.seen[old][0] < now-60000:
                    del self.seen[old]
            if key in self.seen:
                _, old_digest, reply = self.seen[key]
                return (client, reply) if digest == old_digest else None
            if len(self.seen) >= 512:
                return None
            reply = dict(id=ident, ok=False)
            self.seen[key] = (now, digest, reply)
            try:
                reply.update(ok=True, data=self.device.execute(r.get('op'), r.get('args', {})))
            except ValidationError as ex:
                reply.update(ok=False, error=str(ex))
            except Exception:
                reply.update(ok=False, error='Помилка шлюзу. Перевірте стан перед повторенням.')
            return client, reply
        except (ValueError, TypeError):
            return None


def load_env(path):
    if not os.path.exists(path):
        return
    with open(path, encoding='utf-8-sig') as f:
        for row in f:
            row = row.strip()
            if not row or row.startswith('#') or '=' not in row:
                continue
            key, value = row.split('=', 1)
            value = value.strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in ('"', "'"):
                value = value[1:-1]
            if re.fullmatch(r'[A-Z_]+', key.strip()):
                os.environ.setdefault(key.strip(), value)


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--env', default='.env'); args = ap.parse_args()
    load_env(args.env)
    import paho.mqtt.client as mqtt
    host = os.environ.get('MQTT_HOST', '')
    user = os.environ.get('MQTT_USERNAME', ''); password = os.environ.get('MQTT_PASSWORD', '')
    base = os.environ.get('MQTT_BASE', 'garden/controller').strip().rstrip('/')
    if not host or not user or not password or not base or re.search(r'[+#\x00-\x1f]', base):
        raise SystemExit('Заповніть MQTT_HOST, MQTT_USERNAME, MQTT_PASSWORD та MQTT_BASE у .env')
    device = Device(os.environ.get('ESP_URL', ''), os.environ.get('ESP_USERNAME', 'admin'), os.environ.get('ESP_PASSWORD', ''))
    rpc = RpcServer(device); inbox = queue.Queue(maxsize=32)
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id='garden-pwa-gateway', clean_session=True)
    client.username_pw_set(user, password); client.tls_set_context(ssl.create_default_context())
    client.will_set(base+'/pwa/availability', 'offline', qos=0, retain=True)

    def connected(c, userdata, flags, reason, properties):
        if reason.is_failure:
            print('Брокер відхилив підключення; перевірте .env', flush=True); return
        c.subscribe(base+'/pwa/request', qos=0)
        c.publish(base+'/pwa/availability', 'online', qos=0, retain=True)
        print('MQTT підключено. Шлюз готовий.', flush=True)

    def message(c, userdata, msg):
        if msg.topic != base+'/pwa/request' or msg.retain or len(msg.payload) > 16384:
            return
        try:
            inbox.put_nowait(msg.payload.decode('utf-8'))
        except (queue.Full, UnicodeError):
            pass

    client.on_connect = connected; client.on_message = message
    client.connect(host, int(os.environ.get('MQTT_PORT', '8883')), 60); client.loop_start()
    try:
        while True:
            try:
                raw = inbox.get(timeout=30)
            except queue.Empty:
                if client.is_connected():
                    client.publish(base+'/pwa/availability', 'online', qos=0, retain=True)
                continue
            response = rpc.process(raw)
            if response and client.is_connected():
                who, payload = response
                client.publish(base+'/pwa/reply/'+who, json.dumps(payload, ensure_ascii=False), qos=0, retain=False)
    except KeyboardInterrupt:
        pass
    finally:
        if client.is_connected():
            client.publish(base+'/pwa/availability', 'offline', qos=0, retain=True).wait_for_publish(timeout=2)
        client.disconnect(); client.loop_stop()


if __name__ == '__main__':
    main()
