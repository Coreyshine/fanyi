/*
 * fanyi 扩展测试床 — shim.js
 * 在普通页面里模拟 chrome.runtime.sendMessage / chrome.storage.local，
 * 然后注入 ../extension/content.js 走真实流程（真实 HTTP 调 fanyi-server）。
 */
window.chrome = {
  runtime: {
    _listeners: [],
    onMessage: {
      addListener(fn) { chrome.runtime._listeners.push(fn); },
    },
    sendMessage(msg, cb) {
      (async () => {
        try {
          const SERVER = 'http://127.0.0.1:8765';
          if (msg.type === 'config') {
            cb({ ok: true, data: await (await fetch(SERVER + '/v1/config')).json() });
          } else if (msg.type === 'translate') {
            const r = await fetch(SERVER + '/v1/translate', {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ segments: msg.segments, target: msg.target }),
            });
            const text = await r.text();
            const results = new Array(msg.segments.length).fill(null);
            for (const line of text.split('\n')) {
              if (!line.trim()) continue;
              const j = JSON.parse(line);
              if (typeof j.i === 'number') results[j.i] = j.text;
            }
            cb({ ok: true, results });
          } else cb({ ok: false, error: 'unknown' });
        } catch (e) { cb({ ok: false, error: String(e) }); }
      })();
    },
  },
  storage: {
    local: {
      _m: {},
      async get(k) { return { [k]: this._m[k] }; },
      async set(o) { Object.assign(this._m, o); },
    },
  },
};
/* 测试辅助：模拟右键菜单点击（真实扩展里由 background 发 inputTranslate 消息） */
window.__fanyiMenuClick = () => {
  for (const fn of chrome.runtime._listeners) fn({ type: 'inputTranslate' }, {}, () => {});
};

const s = document.createElement('script');
s.src = '../extension/content.js';
document.head.appendChild(s);
