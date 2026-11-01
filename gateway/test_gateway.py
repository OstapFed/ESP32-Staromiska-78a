import json
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlsplit
from gateway import Device, RpcServer, ValidationError, config_from_html, water_fields


def fixture():
    rows = []
    def field(name, value):
        rows.append(f'<input name="{name}" value="{value}">')
    for i in range(4):
        p = f'w{i}'
        rows.append(f'<input type="checkbox" name="{p}en" '+('checked' if i != 2 else '')+'>')
        rows.append(f'<input type="checkbox" name="{p}d1" checked>')
        for k, v in dict(h=6, m=i*10, dur=10+i, intH=24, intM=0).items():
            field(p+k, v)
    for k, v in dict(dayTemp=21.5, nightTemp=19, dayH=6, dayM=30, nightH=23, nightM=0,
                     l0onh=19, l0onm=0, l0offh=23, l0offm=30, rainDelay=12).items():
        field(k, v)
    rows.extend(['<input type="checkbox" name="l0en" checked>', '<input type="checkbox" name="l0d6" checked>',
                 '<input type="checkbox" name="rainEn" checked>', '<input type="checkbox" id="ds18Enable">',
                 '<input type="password" name="password" value="private">', '<textarea id="memoText">Тест &amp; сад</textarea>'])
    return ''.join(rows)


STATE = dict(firmware='Garden_U_V36.1', boiler_mode=1, vacation=False, manual_setpoint=21,
             heating_selected_command=1, wifi_ssid='Home', lights=[False], gates=[], xiaomi_sensors=[])


class MockESP(BaseHTTPRequestHandler):
    writes = []
    def log_message(self, *args):
        pass
    def do_GET(self):
        if self.path == '/':
            payload, kind = fixture().encode(), 'text/html'
        else:
            payload, kind = json.dumps(STATE).encode(), 'application/json'
        self.send_response(200); self.send_header('Content-Type', kind); self.end_headers(); self.wfile.write(payload)
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        self.writes.append((urlsplit(self.path).path, parse_qs(body.decode(), keep_blank_values=True)))
        if self.path == '/settings':
            self.send_response(303); self.send_header('Location', '/'); self.end_headers()
        else:
            self.send_response(200); self.send_header('Content-Type', 'application/json'); self.end_headers(); self.wfile.write(b'{"ok":true}')


class GatewayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.http = HTTPServer(('127.0.0.1', 0), MockESP)
        cls.worker = threading.Thread(target=cls.http.serve_forever, daemon=True); cls.worker.start()
        cls.device = Device(f'http://127.0.0.1:{cls.http.server_port}')
    @classmethod
    def tearDownClass(cls):
        cls.http.shutdown(); cls.http.server_close(); cls.worker.join()
    def setUp(self):
        MockESP.writes.clear()
    def test_full_config_decodes_without_secrets(self):
        c = self.device.config()
        self.assertEqual(c['heat']['dayTime'], '06:30')
        self.assertEqual(c['memo'], 'Тест & сад')
        self.assertNotIn('private', json.dumps(c))
        self.assertFalse(c['water'][2]['enabled'])
        self.assertTrue(c['rain']['enabled'])
    def test_light_save_preserves_water_and_rain(self):
        old = self.device.config()
        self.device.execute('light.save', dict(enabled=True, onTime='18:30', offTime='22:45', days=[1, 4]))
        self.assertEqual(len(MockESP.writes), 1)
        path, p = MockESP.writes[0]
        self.assertEqual(path, '/settings')
        for key, value in water_fields(old['water']).items():
            self.assertEqual(p[key], [str(value)])
        self.assertNotIn('w2en', p)
        self.assertEqual(p['rainEn'], ['1'])
        self.assertEqual(p['rainDelay'], ['12'])
        self.assertNotIn('dayTemp', p)
        self.assertNotIn('mode', p)
    def test_water_save_preserves_other_zones(self):
        c = self.device.config()['water']; edited = dict(c[1], duration=45, enabled=False)
        self.device.execute('water.save', dict(zone=1, config=edited))
        path, p = MockESP.writes[0]
        self.assertEqual(path, '/waterSettingsSave'); self.assertNotIn('w1en', p)
        self.assertEqual(p['w1dur'], ['45']); self.assertEqual(p['w3dur'], ['13'])
    def test_gate_settings_atomic_and_no_motion(self):
        valid = dict(gate=0, name='Заїзд', pinEnabled=True, reedsEnabled=False, pulseMs=1000, newPin='1234')
        for invalid in (dict(valid, pulseMs=700), dict(valid, newPin='a123'), dict(valid, gate=2)):
            with self.assertRaises(ValidationError):
                self.device.execute('gate.save', invalid)
        self.assertEqual(MockESP.writes, [])
        self.device.execute('gate.save', valid)
        self.assertEqual([p for p, _ in MockESP.writes], ['/gateSettings'])
        self.assertEqual(MockESP.writes[0][1]['gate'], ['0'])
    def test_auto_settings_never_send_mode(self):
        self.device.execute('heat.auto.save', dict(dayTemp=22, nightTemp=18, dayTime='06:15', nightTime='22:30'))
        path, p = MockESP.writes[0]
        self.assertEqual(path, '/heatingSettings'); self.assertNotIn('mode', p); self.assertEqual(p['dayM'], ['15'])
    def test_rpc_duplicate_expired_retained_and_changed_id(self):
        class Counter:
            count = 0
            def execute(self, op, args):
                self.count += 1; return {'count': self.count}
        d = Counter(); server = RpcServer(d)
        r = dict(version=1, client='pwa-'+'a'*32, id='aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa', op='gate.command', args={}, created=100000, expires=120000)
        raw = json.dumps(r)
        self.assertIsNone(server.process(raw, retained=True, now=101000)); self.assertEqual(d.count, 0)
        first = server.process(raw, now=101000); self.assertTrue(first[1]['ok'])
        self.assertEqual(server.process(raw, now=102000), first); self.assertEqual(d.count, 1)
        self.assertIsNone(server.process(json.dumps(dict(r, op='water.start')), now=102000))
        self.assertIsNone(server.process(raw, now=121000)); self.assertEqual(d.count, 1)
    def test_invalid_config_fails_closed(self):
        with self.assertRaises(ValidationError):
            config_from_html('<input name="dayTemp" value="21">', STATE)
    def test_no_arbitrary_url_operation(self):
        with self.assertRaises(ValidationError):
            self.device.execute('fetch', {'url': 'http://example.com'})
        self.assertEqual(MockESP.writes, [])


if __name__ == '__main__':
    unittest.main()
