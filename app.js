'use strict';
(() => {
  const $ = id => document.getElementById(id);
  let client = null;
  let session = 0;
  const status = (label, kind = '') => { $('status').textContent = label; $('status').className = `status ${kind}`; };
  const controls = connected => {
    $('connect').disabled = connected;
    $('disconnect').disabled = !connected;
    $('publish').disabled = !connected;
  };
  const appendMessage = (topic, payload) => {
    const area = $('messages');
    const empty = area.querySelector('.empty'); if (empty) empty.remove();
    const item = document.createElement('div'); item.className = 'message';
    const name = document.createElement('strong'); name.textContent = topic;
    const data = document.createElement('pre'); data.textContent = payload;
    const time = document.createElement('small'); time.textContent = new Date().toLocaleTimeString('uk-UA');
    item.append(name, data, time); area.prepend(item);
    while (area.children.length > 70) area.lastElementChild.remove();
  };
  $('connect').addEventListener('click', () => {
    $('error').textContent = '';
    if (!window.mqtt) { $('error').textContent = 'Не завантажено MQTT-бібліотеку. Перевірте інтернет.'; return; }
    const url = $('broker').value.trim();
    const username = $('username').value.trim();
    const password = $('password').value;
    const topic = $('subscribeTopic').value.trim();
    if (!/^wss:\/\//i.test(url) || !username || !password || !topic) {
      $('error').textContent = 'Заповніть WSS-адресу, користувача, пароль і тему підписки.'; return;
    }
    if (client) { client.end(true); client = null; }
    const mySession = ++session;
    status('Підключення…', 'pending'); $('connect').disabled = true;
    const c = mqtt.connect(url, {username, password, clientId: `staromiska_web_${Math.random().toString(16).slice(2, 12)}`, clean: true, connectTimeout: 12000, reconnectPeriod: 4000, protocolVersion: 4});
    client = c;
    c.on('connect', () => {
      if (mySession !== session) return;
      status('Підключено', 'online'); controls(true);
      c.subscribe(topic, {qos: 0}, err => { if (err) $('error').textContent = `Помилка підписки: ${err.message}`; });
    });
    c.on('message', (topicName, bytes) => { if (mySession === session) appendMessage(topicName, bytes.toString()); });
    c.on('reconnect', () => { if (mySession === session) status('Перепідключення…', 'pending'); });
    c.on('close', () => { if (mySession === session) { status('Не підключено'); controls(false); } });
    c.on('error', err => { if (mySession === session) { $('error').textContent = `MQTT: ${err.message}`; $('connect').disabled = false; } });
  });
  $('disconnect').addEventListener('click', () => {
    ++session;
    if (client) { client.end(true); client = null; }
    status('Не підключено'); controls(false); $('error').textContent = '';
    $('password').value = '';
  });
  $('publish').addEventListener('click', () => {
    $('publishResult').textContent = '';
    const topic = $('publishTopic').value.trim();
    if (!client?.connected) { $('publishResult').textContent = 'Немає з’єднання.'; return; }
    if (!topic || /[+#]/.test(topic)) { $('publishResult').textContent = 'Вкажіть точну тему без символів + і #.'; return; }
    const payload = $('payload').value;
    client.publish(topic, payload, {qos: 0, retain: false}, err => {
      $('publishResult').textContent = err ? `Помилка: ${err.message}` : 'Повідомлення надіслано брокеру (виконання ESP32 не підтверджене).';
    });
  });
  $('clear').addEventListener('click', () => { $('messages').replaceChildren(); const e = document.createElement('div'); e.className = 'empty'; e.textContent = 'Повідомлень поки немає'; $('messages').append(e); });
  window.addEventListener('pagehide', () => { if (client) client.end(true); });
})();
