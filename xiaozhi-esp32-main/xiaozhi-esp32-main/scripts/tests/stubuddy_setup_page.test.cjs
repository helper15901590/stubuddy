const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const html = fs.readFileSync(path.join(__dirname, '../../main/boards/stubuddy-s3-n16r8/setup.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];

async function page(options = {}) {
  const elements = new Map(), windowListeners = {};
  let activeElement = null, focusCalls = 0;
  function element(tag = 'div') {
    return {
      tagName: tag.toUpperCase(), value: '', checked: false, disabled: false,
      readOnly: false, hidden: false, type: 'password', content: 'a'.repeat(32),
      textContent: '', className: '', dataset: {}, children: [], listeners: {}, attributes: {}, parentNode: null,
      addEventListener(name, action) { this.listeners[name] = action; },
      setAttribute(name, value) { this.attributes[name] = value; },
      append(child) {
        if (child.parentNode) child.parentNode.children.splice(child.parentNode.children.indexOf(child), 1);
        this.children.push(child); child.parentNode = this;
      },
      replaceChildren() { for (const child of this.children) child.parentNode = null; this.children = []; },
      contains(child) { return Boolean(child) && (child === this || this.children.some(node => node.contains(child))); },
      focus() { focusCalls++; activeElement = this; },
      blur() { if (activeElement === this) activeElement = null; if (this.listeners.blur) this.listeners.blur(); },
      scrollIntoView() {},
      set innerHTML(_) { throw new Error('Untrusted text must not be rendered as HTML'); },
    };
  }
  const get = id => {
    if (!elements.has(id)) elements.set(id, element());
    return elements.get(id);
  };
  get('editor').hidden = true;
  get('editor-host').append(get('editor'));
  get('editor').append(get('ssid')); get('editor').append(get('password'));
  get('password').value = options.initialPassword || '';
  const calls = [];
  const snapshot = {
    phase: 'setup', message: '请选择家庭 Wi-Fi', busy: false, scanning: false,
    networks: [], saved_networks: [], failed: false, ...options.snapshot,
  };
  const context = {
    document: {getElementById:get, querySelector:() => element(), createElement:element,
      get activeElement() { return activeElement; }},
    window: {addEventListener(name, action) { windowListeners[name] = action; }},
    TextEncoder, AbortController, setTimeout:() => 1, clearTimeout:() => {},
    fetch: async (url, init) => {
      calls.push({url, ...init});
      if (url === '/api/connect' || url === '/api/reconnect') {
        if (options.reject) return {ok:false, json:async () => ({error:options.reject})};
        snapshot.phase = 'connecting'; snapshot.busy = true; snapshot.message = '正在连接';
        return {ok:true, json:async () => ({accepted:true})};
      }
      if (url === '/api/forget') {
        if (options.reject) return {ok:false, json:async () => ({error:options.reject})};
        return {ok:true, json:async () => ({accepted:true, operation:7})};
      }
      return {ok:true, json:async () => structuredClone(snapshot)};
    },
  };
  vm.runInNewContext(script, context, {filename:'setup.html'});
  for (let i = 0; i < 3; i++) await new Promise(resolve => setImmediate(resolve));
  const networkRows = id => get(id).children.filter(row => row.dataset.ssid !== undefined);
  return {get, calls, snapshot, context, windowListeners, networkRows,
    get focusCalls() { return focusCalls; }, get activeElement() { return activeElement; },
    poll:() => vm.runInNewContext('poll()', context),
    submit:() => get('submit').listeners.click({isTrusted:true}),
    manual:() => get('manual').listeners.click(),
    choose:(id, index = 0) => networkRows(id)[index].children[0].listeners.click({isTrusted:true}),
    forget:(index = 0) => networkRows('saved')[index].children[1].listeners.click({isTrusted:true}),
    tap:id => { if (!get(id).disabled && !get(id).readOnly) get(id).focus(); }};
}

test('entry hides the editor without autofocus or readonly text-field restrictions', async () => {
  assert.doesNotMatch(html, /<select\b/i);
  assert.doesNotMatch(html, /\sautofocus(?:[=\s>])/i);
  assert.doesNotMatch(html, /\sreadonly(?:[=\s>])/i);
  const p = await page();
  p.windowListeners.pageshow();
  assert.equal(p.get('editor').hidden, true);
  assert.equal(p.get('ssid').readOnly, false);
  assert.equal(p.get('password').readOnly, false);
  assert.equal(p.focusCalls, 0);
});

test('show both lists directly and render untrusted SSIDs as text', async () => {
  const ssid = '<img src=x onerror=alert(1)>';
  const p = await page({snapshot:{saved_networks:['home'], networks:[{ssid, secured:true}, {ssid:'open', secured:false}]}});
  assert.equal(p.networkRows('saved').length, 1);
  assert.equal(p.networkRows('networks').length, 2);
  assert.equal(p.networkRows('networks')[0].children[0].children[0].textContent, ssid);
});

test('selecting a nearby network expands its own row without opening a keyboard', async () => {
  const p = await page({snapshot:{networks:[{ssid:'home', secured:true}]}});
  await p.choose('networks');
  assert.equal(p.get('editor').parentNode, p.networkRows('networks')[0]);
  assert.equal(p.get('editor').hidden, false);
  assert.equal(p.get('ssid-field').hidden, true);
  assert.equal(p.get('ssid').value, 'home');
  assert.equal(p.get('password').readOnly, false);
  assert.equal(p.focusCalls, 0);
});

test('manual entry shows native editable fields without automatically focusing them', async () => {
  const p = await page(); p.manual();
  assert.equal(p.get('ssid-field').hidden, false);
  assert.equal(p.focusCalls, 0);
  p.tap('ssid');
  assert.equal(p.get('ssid').readOnly, false);
  assert.equal(p.activeElement, p.get('ssid'));
  p.get('ssid').blur();
  assert.equal(p.get('ssid').readOnly, false);
});

test('returning to a cached page blurs focus and leaves native input editable', async () => {
  const p = await page(); p.manual();
  p.tap('ssid'); p.get('ssid').value = 'home';
  p.windowListeners.pageshow();
  assert.equal(p.activeElement, null);
  assert.equal(p.get('ssid').readOnly, false);
  assert.equal(p.get('ssid').value, 'home');
});

test('polling never steals focus or discards a typed password during a changed scan', async () => {
  const p = await page({snapshot:{networks:[{ssid:'home', secured:true, rssi:-40}]}});
  await p.choose('networks');
  p.tap('password'); p.get('password').value = 'typed-secret';
  const row = p.networkRows('networks')[0];
  p.snapshot.networks.push({ssid:'another', secured:true});
  await p.poll();
  assert.equal(p.networkRows('networks')[0], row);
  assert.equal(p.activeElement, p.get('password'));
  assert.equal(p.get('password').value, 'typed-secret');
  assert.equal(p.focusCalls, 1);
  p.get('password').blur(); await p.poll();
  assert.equal(p.networkRows('networks').length, 2);
  assert.equal(p.get('password').value, 'typed-secret');
  assert.equal(p.get('editor').parentNode, p.networkRows('networks')[0]);
});

test('repeated status polling in manual setup never submits a connection or locks a new password', async () => {
  const p = await page({snapshot:{manual_setup:true, saved_networks:['old-home'], networks:[{ssid:'new-home', secured:true}]}});
  for (let i = 0; i < 12; i++) await p.poll();
  assert.equal(p.calls.filter(x => x.method === 'POST').length, 0);
  await p.choose('networks');
  for (let i = 0; i < 8; i++) await p.poll();
  assert.equal(p.get('password').disabled, false);
  assert.equal(p.get('password').readOnly, false);
  p.tap('password');
  assert.equal(p.activeElement, p.get('password'));
  assert.equal(p.calls.filter(x => x.method === 'POST').length, 0);
});

test('reject short passwords and excessive UTF-8 SSID byte counts before POST', async () => {
  const p = await page(); p.manual();
  p.get('ssid').value = 'home'; p.get('password').value = 'short'; await p.submit();
  assert.match(p.get('status').textContent, /8至63/);
  p.get('ssid').value = '学'.repeat(11); p.get('password').value = '12345678'; await p.submit();
  assert.match(p.get('status').textContent, /32字节/);
  assert.equal(p.calls.filter(x => x.method === 'POST').length, 0);
});

test('submit a Chinese SSID with a token and retain pending connection feedback', async () => {
  const p = await page(); p.manual();
  p.get('ssid').value = '学习网络'; p.get('password').value = '12345678'; await p.submit();
  const post = p.calls.find(x => x.method === 'POST');
  assert.deepEqual(JSON.parse(post.body), {ssid:'学习网络', password:'12345678'});
  assert.equal(post.headers['X-Setup-Token'], 'a'.repeat(32));
  assert.equal(post.headers['X-Setup-Action'], 'manual-connect');
  assert.equal(p.get('submit').disabled, true);
  assert.match(p.get('status').textContent, /正在连接/);
});

test('an open nearby network disables password entry and never posts an old password', async () => {
  const p = await page({snapshot:{networks:[{ssid:'open', secured:false}]}});
  await p.choose('networks'); p.get('password').value = 'old-password';
  assert.equal(p.get('password').disabled, true);
  await p.submit();
  assert.equal(JSON.parse(p.calls.find(x => x.method === 'POST').body).password, '');
});

test('expired-page errors survive status polling until the input changes', async () => {
  const p = await page({reject:'页面已失效，请刷新后重试'}); p.manual();
  p.get('ssid').value = 'home'; p.get('password').value = '12345678'; await p.submit();
  assert.match(p.get('status').textContent, /页面已失效/);
  assert.match(p.get('status').className, /error/);
});

test('successful setup clears the password, hides entry, and disables submissions', async () => {
  const p = await page({initialPassword:'secret123', snapshot:{phase:'connected', message:'连接成功'}});
  assert.equal(p.get('password').value, ''); assert.equal(p.get('editor').hidden, true);
  assert.equal(p.get('submit').disabled, true); await p.submit();
  assert.equal(p.calls.filter(x => x.method === 'POST').length, 0);
});

test('a fatal network error disables all network choices', async () => {
  const p = await page({snapshot:{phase:'error', message:'网络模块异常，请重新上电', saved_networks:['home']}});
  assert.equal(p.get('submit').disabled, true);
  assert.equal(p.networkRows('saved')[0].children[0].disabled, true);
  await p.choose('saved'); assert.equal(p.calls.filter(x => x.method === 'POST').length, 0);
});

test('history click posts only the SSID and blocks rapid duplicate clicks', async () => {
  const ssid = '<img src=x onerror=alert(1)>';
  const p = await page({snapshot:{saved_networks:[ssid, '家里']}});
  await p.choose('saved');
  assert.deepEqual(JSON.parse(p.calls.find(x => x.url === '/api/reconnect').body), {ssid});
  assert.equal(p.get('password').value, '');
  assert.equal(p.get('editor').hidden, true);
  assert.equal(p.networkRows('saved')[1].children[0].disabled, true);
  await p.choose('saved', 1);
  assert.equal(p.calls.filter(x => x.method === 'POST').length, 1);
  assert.equal(p.focusCalls, 0);
});

test('a saved network in the nearby list is selected without reconnecting', async () => {
  const p = await page({snapshot:{saved_networks:['home'], networks:[{ssid:'home', secured:true}]}});
  await p.choose('networks');
  assert.equal(p.calls.filter(x => x.method === 'POST').length, 0);
  assert.equal(p.get('ssid').value, 'home');
  assert.equal(p.get('password').disabled, false);
});

test('empty lists remain visible with clear placeholders', async () => {
  const p = await page();
  assert.match(p.get('saved').children[0].textContent, /暂无记录/);
  assert.match(p.get('networks').children[0].textContent, /未找到网络/);
});

test('history reconnect never opens the editor, including after failure', async () => {
  const p = await page({reject:'认证失败，请检查 Wi-Fi 密码', snapshot:{saved_networks:['home'], networks:[{ssid:'home', secured:true}]}});
  await p.choose('saved');
  assert.match(p.get('status').textContent, /认证失败/);
  assert.equal(p.get('editor').hidden, true);
  assert.equal(p.networkRows('saved')[0].children[0].attributes['aria-pressed'], 'true');
  assert.equal(p.networkRows('saved')[0].children[0].attributes['aria-expanded'], 'false');
  assert.equal(p.focusCalls, 0);
  await p.choose('networks');
  assert.equal(p.get('editor').hidden, false);
  p.tap('password'); p.get('password').value = 'new-password'; await p.submit();
  assert.deepEqual(JSON.parse(p.calls.find(x => x.url === '/api/connect').body), {ssid:'home', password:'new-password'});
});

test('history click immediately hides an already open new-network editor', async () => {
  const p = await page({snapshot:{saved_networks:['old-home'], networks:[{ssid:'new-home', secured:true}]}});
  await p.choose('networks'); p.tap('password'); p.get('password').value = 'unused-secret';
  const request = p.choose('saved');
  assert.equal(p.get('editor').hidden, true);
  assert.equal(p.get('password').value, '');
  await request;
  assert.equal(p.get('editor').hidden, true);
  assert.deepEqual(JSON.parse(p.calls.find(x => x.url === '/api/reconnect').body), {ssid:'old-home'});
});

test('idle state omits repeated instructions and list rows contain only SSID and selection mark', async () => {
  const p = await page({snapshot:{saved_networks:['home'], networks:[{ssid:'home', secured:true}]}});
  assert.equal(p.get('status').hidden, true);
  assert.equal(p.get('status').textContent, '');
  assert.equal(p.networkRows('saved')[0].children[0].children.length, 2);
  assert.equal(p.networkRows('networks')[0].children[0].children.length, 2);
  assert.doesNotMatch(html, /已保存密码 ·|扫描到的网络 ·|需要密码<|network-detail/);
});

test('form submit from autofill or Return and synthetic clicks never connect', async () => {
  const p = await page({snapshot:{saved_networks:['old-home']}});
  p.manual(); p.get('ssid').value = 'new-home'; p.get('password').value = '12345678';
  let prevented = false;
  p.get('form').listeners.submit({isTrusted:true, preventDefault() { prevented = true; }});
  await p.get('submit').listeners.click({isTrusted:false});
  await p.networkRows('saved')[0].children[0].listeners.click({isTrusted:false});
  assert.equal(prevented, true);
  assert.equal(p.calls.filter(x => x.method === 'POST').length, 0);
});

test('selection is visible on one row and survives a status refresh', async () => {
  const p = await page({snapshot:{saved_networks:['old-home'], networks:[{ssid:'one', secured:true}, {ssid:'two', secured:true}]}});
  await p.choose('networks');
  assert.equal(p.networkRows('networks')[0].children[0].attributes['aria-pressed'], 'true');
  assert.equal(p.networkRows('networks')[0].children[0].children[1].hidden, false);
  await p.poll(); await p.choose('networks', 1);
  assert.equal(p.networkRows('networks')[0].children[0].attributes['aria-pressed'], 'false');
  assert.equal(p.networkRows('networks')[1].children[0].attributes['aria-pressed'], 'true');
  assert.equal(p.networkRows('saved')[0].children[0].attributes['aria-pressed'], 'false');
  assert.equal(p.calls.filter(x => x.method === 'POST').length, 0);
});

test('the single embedded page stays under 24KiB and has no external assets', () => {
  assert.ok(Buffer.byteLength(html) < 24 * 1024);
  assert.doesNotMatch(html, /<(?:script|link|img)\b[^>]*(?:src|href)=/i);
});

test('each saved network has an accessible delete button; nearby networks do not', async () => {
  const ssid = '<img src=x onerror=alert(1)>';
  const p = await page({snapshot:{saved_networks:[ssid], networks:[{ssid, secured:true}]}});
  const row = p.networkRows('saved')[0];
  assert.equal(row.children.length, 2);
  assert.equal(row.children[1].textContent, '×');
  assert.equal(row.children[1].attributes['aria-label'], '删除已保存网络 ' + ssid);
  assert.equal(p.networkRows('networks')[0].children.length, 1);
});

test('delete posts only the selected SSID and waits for a matching device result', async () => {
  const p = await page({snapshot:{saved_networks:['office', 'home']}});
  const button = p.networkRows('saved')[0].children[1];
  button.focus();
  await p.forget();
  const post = p.calls.find(x => x.url === '/api/forget');
  assert.deepEqual(JSON.parse(post.body), {ssid:'office'});
  assert.equal(post.headers['X-Setup-Action'], 'history-forget');
  assert.equal(post.headers['X-Setup-Token'], 'a'.repeat(32));
  assert.equal(p.networkRows('saved').length, 2);
  assert.equal(p.networkRows('saved')[1].children[1].disabled, true);
  assert.equal(p.get('editor').hidden, true);
  await p.forget(1); await p.choose('saved', 1);
  assert.equal(p.calls.filter(x => x.method === 'POST').length, 1);
  // An earlier completed operation must not unlock or remove the selected row.
  p.snapshot.history_operation = 6; p.snapshot.history_ok = true;
  await p.poll(); assert.equal(p.networkRows('saved').length, 2);
  assert.match(p.get('progress-text').textContent, /删除/);
  // NVS completion updates the list even while the old delete button has focus.
  p.snapshot.history_operation = 7; p.snapshot.history_ok = true;
  p.snapshot.saved_networks = ['home']; await p.poll();
  assert.equal(p.networkRows('saved').length, 1);
  assert.equal(p.networkRows('saved')[0].dataset.ssid, 'home');
  assert.equal(p.networkRows('saved')[0].children[1].disabled, false);
  assert.match(p.get('status').textContent, /已删除/);
  assert.equal(p.get('progress').hidden, true);
  assert.equal(p.calls.some(x => x.url === '/api/reconnect'), false);
  p.snapshot.failed = true; p.snapshot.message = '扫描失败，可以重试'; await p.poll();
  assert.match(p.get('status').textContent, /扫描失败/);
  assert.doesNotMatch(p.get('status').textContent, /已删除/);
});

test('failed persistence preserves the visible record and permits a deliberate retry', async () => {
  const p = await page({snapshot:{saved_networks:['home']}});
  await p.forget();
  p.snapshot.history_operation = 7; p.snapshot.history_ok = false;
  await p.poll();
  assert.equal(p.networkRows('saved').length, 1);
  assert.equal(p.networkRows('saved')[0].children[1].disabled, false);
  assert.match(p.get('status').textContent, /删除未能完成/);
  assert.match(p.get('status').className, /error/);
  assert.equal(p.get('progress').hidden, true);
});

test('HTTP delete rejection and an inconsistent success receipt never hide a saved network', async () => {
  const rejected = await page({reject:'页面已失效，请刷新后重试', snapshot:{saved_networks:['home']}});
  await rejected.forget();
  assert.equal(rejected.networkRows('saved').length, 1);
  assert.match(rejected.get('status').textContent, /页面已失效/);
  const p = await page({snapshot:{saved_networks:['home']}});
  await p.forget(); p.snapshot.history_operation = 7; p.snapshot.history_ok = true;
  await p.poll();
  assert.equal(p.networkRows('saved').length, 1);
  assert.match(p.get('status').textContent, /删除未能完成/);
});

test('a synthetic delete and clicks while connecting never submit a deletion', async () => {
  const p = await page({snapshot:{saved_networks:['home']}});
  await p.networkRows('saved')[0].children[1].listeners.click({isTrusted:false});
  p.snapshot.phase = 'connecting'; p.snapshot.busy = true; await p.poll();
  assert.equal(p.networkRows('saved')[0].children[1].disabled, true);
  await p.forget();
  assert.equal(p.calls.filter(x => x.method === 'POST').length, 0);
});

test('scan progress is visible without stealing focus and stops when scanning completes', async () => {
  const p = await page({snapshot:{scanning:true}});
  assert.equal(p.get('progress').hidden, false);
  assert.match(p.get('progress-text').textContent, /查找/);
  p.snapshot.scanning = false; await p.poll();
  assert.equal(p.get('progress').hidden, true);
  assert.equal(p.focusCalls, 0);
  assert.equal(p.calls.filter(x => x.method === 'POST').length, 0);
});

test('connection progress appears immediately while POST is pending', async () => {
  const p = await page(); p.manual();
  p.get('ssid').value = 'home'; p.get('password').value = '12345678';
  const fetch = p.context.fetch;
  let complete;
  p.context.fetch = (url, init) => url === '/api/connect' ? new Promise(resolve => {
    complete = () => {
      p.snapshot.phase = 'connecting'; p.snapshot.busy = true;
      resolve({ok:true, json:async () => ({accepted:true})});
    };
  }) : fetch(url, init);
  const submission = p.submit();
  assert.equal(p.get('progress').hidden, false);
  assert.match(p.get('progress-text').textContent, /提交/);
  complete(); await submission;
  assert.equal(p.get('progress').hidden, false);
  assert.match(p.get('progress-text').textContent, /连接/);
  assert.equal(p.focusCalls, 0);
});

test('connection failure stops progress and preserves the error feedback', async () => {
  const p = await page({snapshot:{phase:'connecting',busy:true}});
  assert.equal(p.get('progress').hidden, false);
  p.snapshot.phase = 'setup'; p.snapshot.busy = false; p.snapshot.failed = true;
  p.snapshot.message = '密码错误，请重试'; await p.poll();
  assert.equal(p.get('progress').hidden, true);
  assert.match(p.get('status').textContent, /密码错误/);
  assert.equal(p.get('submit').disabled, false);
});

test('success stops progress and displays the three-second hotspot closing notice', async () => {
  const p = await page({snapshot:{phase:'connected',message:'连接成功，设备热点将在3秒后关闭'}});
  assert.equal(p.get('progress').hidden, true);
  assert.match(p.get('status').textContent, /3秒/);
  assert.equal(p.get('editor').hidden, true);
});
