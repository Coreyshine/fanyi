window.__errs = [];
window.addEventListener('error', e => window.__errs.push(String(e.message).slice(0, 90)));
window.addEventListener('unhandledrejection', e => window.__errs.push('REJ: ' + String(e.reason && e.reason.message || e.reason).slice(0, 90)));
/*
 * fanyi 扩展测试床 — shim.js
 * 在普通页面里模拟 chrome.runtime.sendMessage / chrome.storage.local，
 * 然后注入 ../extension/content.js 走真实流程（真实 HTTP 调 fanyi-server）。
 */
window.chrome = {
  runtime: {
    _listeners: [],
    getManifest: () => ({ version: '1.2.0' }),
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
    onChanged: { addListener(fn) { chrome.storage.local._listeners.push(fn); } },
    local: {
      _m: {},
      _listeners: [],
      get(k, cb) {
        const keys = Array.isArray(k) ? k : [k];
        const out = {};
        for (const key of keys) out[key] = this._m[key];
        if (cb) cb(out);                       /* 兼容回调风格（真实浏览器两种都支持） */
        return Promise.resolve(out);
      },
      set(o, cb) {
        Object.assign(this._m, o);
        for (const fn of chrome.storage.local._listeners) fn(o, 'local');
        if (cb) cb();
        return Promise.resolve();
      },
    },
  },
};
/* 测试辅助：模拟右键菜单点击（真实扩展里由 background 发 inputTranslate 消息） */
window.__fanyiMenuClick = () => {
  for (const fn of chrome.runtime._listeners) fn({ type: 'inputTranslate' }, {}, () => {});
};

const VER = String(Date.now());   /* 破缓存：确保每次加载最新扩展脚本 */
for (const f of ['sites.js', 'content.js', 'video.js']) {
  const s = document.createElement('script');
  s.src = 'http://127.0.0.1:8899/extension/' + f + '?v=' + VER;
  document.head.appendChild(s);
}

/*
 * fanyi 扩展 — sites.js
 * 视频字幕站点选择器配置表：主机名（后缀匹配）→ 字幕文本元素选择器列表。
 * 站点改版后在此更新即可；未列出的站点走通用「视频覆盖层文本」检测。
 */
window.__fanyiSites = {
  'youtube.com': ['.ytp-caption-segment'],
  'm.youtube.com': ['.ytp-caption-segment'],
  'bilibili.com': ['.bpx-player-subtitle-text', '.bilibili-player-video-subtitle-text'],
  'netflix.com': ['.player-timedtext-text-container span'],
  'iqiyi.com': ['.iqp-player-subtitle', '.iqp-txt-wrapper'],
  'youku.com': ['.subtitle-text', '.kui-subtitle-page'],
  'v.qq.com': ['.txp-subtitle-text'],
  'vip.1905.com': ['.v-subtitle-text'],
  'coursera.org': ['.video__caption', '.rc-Phrase'],
  'udemy.com': ['.well--container--pFYwq', '.captions-display--captions-cued--358Xq'],
  'ted.com': ['.p_breadcrumb', 'h2.talk-transcript__teaser'],
};

/*
 * fanyi 扩展 — content.js
 * 页面实时翻译：取词 → 外文占比检测(>阈值自动翻译) → 批量送本地服务 → 流式替换。
 * 悬浮开关（Shadow DOM，可拖动）；SPA 路由监听；长页面按可见区域分批（省资源）。
 */
(() => {
  'use strict';
  if (window.__fanyiLoaded) return;
  window.__fanyiLoaded = true;
  window.__fanyiVersion = chrome.runtime.getManifest().version;

  /* ---------- 常量 ---------- */
  const CHUNK = 8;                  // 每请求段数
  const NODE_MIN_CHARS = 2;         // 短于该长度不翻
  const LAZY_THRESHOLD = 150;       // 文本块超过此数 → 只翻可见区（滚动续翻）
  const SKIP_TAGS = new Set([
    'SCRIPT', 'STYLE', 'NOSCRIPT', 'TEMPLATE', 'CODE', 'PRE', 'KBD', 'SAMP',
    'TEXTAREA', 'INPUT', 'SELECT', 'OPTION', 'SVG', 'MATH', 'CANVAS', 'IFRAME',
  ]);
  const FANYI_HOST_ID = 'fanyi-float-host';

  /* ---------- 状态 ---------- */
  const S = {
    cfg: null,            // {enabled, target_lang, auto_translate, foreign_ratio, ...}
    serverUp: false,
    on: false,            // 本页翻译开关
    outdated: false,      // 扩展更新后旧页面等待刷新
    origs: new Map(),     // TextNode -> 原文
    queue: [],            // {node, text, prio, seq}
    queued: new Set(),    // node 去重
    translating: false,
    doneCount: 0,
    scanCount: 0,
    mo: null,
    io: null,
    lazyMode: false,
    routeHooked: false,
  };

  /* ---------- 与后台通信 ---------- */
  function send(msg) {
    return new Promise((resolve) => {
      try {
        chrome.runtime.sendMessage(msg, (r) => {
          if (chrome.runtime.lastError) resolve({ ok: false, error: chrome.runtime.lastError.message });
          else resolve(r || { ok: false, error: 'no response' });
        });
      } catch (e) { resolve({ ok: false, error: String(e) }); }
    });
  }

  /* ---------- 文本节点收集（按文本节点逐段，保留行内结构） ---------- */
  function skipNode(node) {
    const p = node.parentElement;
    if (!p) return true;
    let el = p;
    while (el && el !== document.body) {
      if (SKIP_TAGS.has(el.tagName)) return true;
      if (el.isContentEditable) return true;
      el = el.parentElement;
    }
    return false;
  }

  function collectTextNodes(root) {
    const out = [];
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT, {
      acceptNode(n) {
        const v = n.nodeValue;
        if (!v || v.trim().length < NODE_MIN_CHARS) return NodeFilter.FILTER_REJECT;
        if (S.origs.has(n) || S.queued.has(n)) return NodeFilter.FILTER_REJECT;
        if (skipNode(n)) return NodeFilter.FILTER_REJECT;
        return NodeFilter.FILTER_ACCEPT;
      },
    });
    while (walker.nextNode()) out.push(walker.currentNode);
    return out;
  }

  /* ---------- 外文占比检测 ---------- */
  const RE = {
    cjk: /[\u3400-\u4DBF\u4E00-\u9FFF\uF900-\uFAFF\u3040-\u30FF\u31F0-\u31FF\uAC00-\uD7AF\u3130-\u318F]/,
  };
  const STOPWORDS = {
    en: 'the a an and or but of to in on for with at by from as is are was were be been it its this that these those not have has had will would can could you your we they he she'.split(' '),
    fr: 'le la les un une des et ou de du au aux en est sont il elle ce cette ces que qui dans pour sur avec pas plus'.split(' '),
    de: 'der die das und oder ein eine einen dem den des ist sind war nicht mit von zu auf für im am er sie es wir ihr sich'.split(' '),
    es: 'el la los las un una unos unas y o de del al en es son era para con por que se lo su más pero como'.split(' '),
    it: 'il lo la i gli le un uno una e o di del della in sono era per con che non si più come della'.split(' '),
    pt: 'o a os as um uma uns umas e ou de do da dos das em é são era para com por que se não mais como'.split(' '),
    ru: 'и в не на я быть он с что а по это она этот к но они мы вы из за от как у или для'.split(' '),
    nl: 'de het een en van in is zijn dat deze niet met voor op aan er hij zij we jij je maar ook'.split(' '),
  };

  function classifyRatio(nodes, target) {
    let foreign = 0, native = 0;
    const targetCJK = /^(zh|ja|ko)/.test(target);
    if (targetCJK) {
      for (const n of nodes) {
        for (const ch of n.nodeValue) {
          if (RE.cjk.test(ch)) native++;
          else if (/\p{L}/u.test(ch)) foreign++;
        }
      }
    } else {
      const stop = new Set(STOPWORDS[target] || []);
      for (const n of nodes) {
        for (const w of n.nodeValue.toLowerCase().match(/\p{L}+/gu) || []) {
          if (stop.has(w)) native++;
          else foreign++;
        }
      }
      if (native === 0 && foreign > 0) { /* 未知目标语言：一律视为外文 */ native = 0; }
    }
    const total = foreign + native;
    return total === 0 ? 0 : foreign / total;
  }

  /* ---------- 翻译缓存（chrome.storage，LRU 上限 2000 条） ---------- */
  const CACHE_KEY = 'fanyi_cache';
  const CACHE_MAX = 2000;
  function fnv(str) {
    let h = 0x811c9dc5;
    for (let i = 0; i < str.length; i++) { h ^= str.charCodeAt(i); h = (h * 0x01000193) >>> 0; }
    return h.toString(36) + '_' + str.length.toString(36);
  }
  async function cacheGet(keys) {
    const o = (await chrome.storage.local.get(CACHE_KEY))[CACHE_KEY] || {};
    return keys.map((k) => (Object.prototype.hasOwnProperty.call(o, k) ? o[k] : null));
  }
  async function cachePut(pairs) {
    const o = (await chrome.storage.local.get(CACHE_KEY))[CACHE_KEY] || {};
    for (const [k, v] of pairs) { delete o[k]; o[k] = v; }
    const ks = Object.keys(o);
    if (ks.length > CACHE_MAX) for (const k of ks.slice(0, ks.length - CACHE_MAX)) delete o[k];
    await chrome.storage.local.set({ [CACHE_KEY]: o });
  }

  /* ---------- 翻译执行 ---------- */
  function applyText(node, text) {
    if (!node.isConnected) return;
    if (!S.origs.has(node)) S.origs.set(node, node.nodeValue);
    node.nodeValue = text;
    S.doneCount++;
  }

  async function pump() {
    if (S.translating) return;
    S.translating = true;
    updateFloat();
    try {
      while (S.queue.length) {
        if (!S.on) { S.queue.length = 0; S.queued.clear(); break; }
        const batch = S.queue.splice(0, CHUNK);
        for (const it of batch) S.queued.delete(it.node);
        const texts = batch.map((b) => b.text);
        const keys = texts.map((t) => fnv(t + '|' + S.cfg.target_lang));

        const hit = await cacheGet(keys);
        const need = [], needIdx = [];
        batch.forEach((it, i) => {
          if (typeof hit[i] === 'string') applyText(it.node, hit[i]);
          else { need.push(texts[i]); needIdx.push(i); }
        });

        if (need.length) {
          const r = await send({ type: 'translate', segments: need, target: S.cfg.target_lang });
          if (!r.ok) {
            S.serverUp = false;
            updateFloat('error', r.error);
            S.queue.length = 0; S.queued.clear();
            break;
          }
          S.serverUp = true;
          const pairs = [];
          needIdx.forEach((idx, k) => {
            const out = r.results && r.results[k];
            if (typeof out === 'string' && out) {
              applyText(batch[idx].node, out);
              pairs.push([keys[idx], out]);
            }
          });
          if (pairs.length) cachePut(pairs);   /* 异步写，不等待 */
        }
        updateFloat('progress');
        await new Promise((r) => setTimeout(r, 20));  /* 让出主线程 */
      }
    } finally {
      S.translating = false;
      updateFloat();
    }
  }

  function enqueue(nodes, prio) {
    for (const n of nodes) {
      if (S.queued.has(n) || S.origs.has(n)) continue;
      const text = n.nodeValue.replace(/\s+/g, ' ').trim();
      if (text.length < NODE_MIN_CHARS) continue;
      S.queued.add(n);
      S.queue.push({ node: n, text, prio, seq: S.seq++ });
    }
    S.queue.sort((a, b) => a.prio - b.prio || a.seq - b.seq);
  }
  S.seq = 0;

  /* ---------- 悬浮开关 ---------- */
  function buildFloat() {
    if (document.getElementById(FANYI_HOST_ID)) return;
    const host = document.createElement('div');
    host.id = FANYI_HOST_ID;
    host.style.cssText = 'position:fixed;z-index:2147483647;top:16px;right:16px;';
    const shadow = host.attachShadow({ mode: 'open' });
    shadow.innerHTML = `
      <style>
        .pill {
          display:flex;align-items:center;gap:6px;height:34px;padding:0 12px 0 9px;
          background:rgba(28,32,40,.88);color:#fff;border-radius:18px;cursor:pointer;
          font:600 13px/1 -apple-system,"PingFang SC","Microsoft YaHei",sans-serif;
          box-shadow:0 2px 10px rgba(0,0,0,.25);user-select:none;backdrop-filter:blur(6px);
          transition:background .2s, transform .15s; touch-action:none;
        }
        .pill:active { transform:scale(.94); }
        .dot { width:10px;height:10px;border-radius:50%;background:#8a93a3;flex-shrink:0; }
        .pill.on .dot { background:#31d07e; }
        .pill.busy .dot { background:#3b82f6;animation:blink 1s infinite; }
        .pill.err .dot { background:#ef4444; }
        .pill.upd .dot { background:#f59e0b; }
        .pill.off .dot { background:#8a93a3; }
        @keyframes blink { 50% { opacity:.3; } }
        .label { white-space:nowrap; }
      </style>
      <div class="pill off"><span class="dot"></span><span class="label">译</span></div>
    `;
    const root = document.documentElement;
    root.appendChild(host);

    const pill = shadow.querySelector('.pill');
    const label = shadow.querySelector('.label');
    S.floatPill = pill; S.floatLabel = label;

    /* 拖动 + 点击 */
    let moved = 0, sx = 0, sy = 0, ox = 0, oy = 0;
    pill.addEventListener('pointerdown', (e) => {
      moved = 0; sx = e.clientX; sy = e.clientY;
      ox = host.offsetLeft; oy = host.offsetTop;
      pill.setPointerCapture(e.pointerId);
    });
    pill.addEventListener('pointermove', (e) => {
      const dx = e.clientX - sx, dy = e.clientY - sy;
      if (Math.abs(dx) + Math.abs(dy) < 5 || !pill.hasPointerCapture(e.pointerId)) return;
      moved = 1;
      host.style.left = Math.max(4, Math.min(innerWidth - 60, ox + dx)) + 'px';
      host.style.top = Math.max(4, Math.min(innerHeight - 50, oy + dy)) + 'px';
      host.style.right = 'auto';
    });
    pill.addEventListener('pointerup', (e) => {
      if (moved) { localStorage.fanyiFloatPos = JSON.stringify([host.style.left, host.style.top]); return; }
      onFloatClick();
    });
    try {
      const pos = JSON.parse(localStorage.fanyiFloatPos || 'null');
      if (pos && pos[0]) { host.style.left = pos[0]; host.style.top = pos[1]; host.style.right = 'auto'; }
    } catch {}
  }

  function updateFloat(mode, info) {
    if (!S.floatPill || !S.cfg) return;
    const pill = S.floatPill, label = S.floatLabel;
    pill.className = 'pill';
    if (S.outdated) { pill.classList.add('upd'); label.textContent = '扩展已更新 · 点击刷新页面'; return; }
    if (mode === 'error') { pill.classList.add('err'); label.textContent = '服务未运行'; return; }
    if (S.cfg.enabled === false) { pill.classList.add('off'); label.textContent = '已停用'; return; }
    if (S.translating) {
      pill.classList.add('busy');
      label.textContent = `翻译中 ${S.doneCount}/${S.scanCount}`;
    } else if (S.on) {
      pill.classList.add('on');
      label.textContent = `已译 ${S.doneCount} 段 · 点击还原`;
    } else {
      pill.classList.add('off');
      label.textContent = '译 · 点击翻译本页';
    }
    if (info) pill.title = info;
  }

  async function onFloatClick() {
    if (S.outdated) { location.reload(); return; }   /* 扩展已更新：点击刷新页面加载新版 */
    if (!S.cfg) return;
    if (!S.serverUp) { await loadCfg(); if (!S.serverUp) return; }
    if (S.on) {
      /* 还原 */
      S.on = false;
      S.queue.length = 0; S.queued.clear();
      for (const [node, orig] of S.origs) if (node.isConnected) node.nodeValue = orig;
      S.origs.clear();          /* 关键：还原后节点已回到原文，清掉记录才能再次翻译 */
      S.doneCount = 0; S.scanCount = 0;
      updateFloat();
    } else {
      S.on = true;
      S.doneCount = 0;
      scanAndEnqueue(0);
      pump();
    }
  }

  /* ---------- 扫描与自动翻译 ---------- */
  function scanAndEnqueue(prio) {
    const nodes = collectTextNodes(document.body);
    if (!nodes.length) return 0;
    S.scanCount = nodes.length;

    /* 外文占比：整页可见文本判定 */
    const ratio = classifyRatio(nodes, S.cfg.target_lang);
    S.lastRatio = ratio;

    if (S.lazyMode === false && nodes.length > LAZY_THRESHOLD) {
      /* 长页面：只立即翻可见块，滚动续翻 */
      S.lazyMode = true;
      setupIntersectionObserver();
    }

    if (S.lazyMode) {
      /* 注意：IO 只接受元素，观察文本节点的父元素（去重） */
      const els = new Set();
      for (const n of nodes) {
        const el = n.parentElement;
        if (el && !els.has(el)) { els.add(el); S.io.observe(el); }
      }
    } else {
      enqueue(nodes, prio);
    }
    return nodes.length;
  }

  function setupIntersectionObserver() {
    if (S.io) return;
    S.io = new IntersectionObserver((entries) => {
      const vis = [];
      for (const e of entries) {
        if (e.isIntersecting) {
          vis.push(...collectTextNodes(e.target));   /* 收集该元素内的待翻文本 */
          S.io.unobserve(e.target);
        }
      }
      if (vis.length && S.on) { enqueue(vis, 0); pump(); }
    }, { rootMargin: '60% 0px 60% 0px' });
  }

  function setupMutationObserver() {
    let timer = null;
    S.mo = new MutationObserver(() => {
      clearTimeout(timer);
      timer = setTimeout(() => {
        if (!S.on) return;
        const nodes = collectTextNodes(document.body);
        if (!nodes.length) return;
        if (S.lazyMode) {
          for (const n of nodes) if (n.parentElement) S.io.observe(n.parentElement);
        } else {
          enqueue(nodes, 3);
          pump();
        }
      }, 500);
    });
    S.mo.observe(document.body, { childList: true, subtree: true, characterData: false });
  }

  /* SPA 路由变化 → 重置状态重新扫描 */
  function hookRouting() {
    if (S.routeHooked) return;
    S.routeHooked = true;
    let last = location.href;
    const check = () => {
      if (location.href !== last) {
        last = location.href;
        resetPage(false);
        setTimeout(init, 600);
      }
    };
    const wrap = (fn) => function () { const r = fn.apply(this, arguments); setTimeout(check, 100); return r; };
    try {
      history.pushState = wrap(history.pushState);
      history.replaceState = wrap(history.replaceState);
      addEventListener('popstate', () => setTimeout(check, 100));
    } catch {}
  }

  function resetPage(clearDom = true) {
    S.queue.length = 0; S.queued.clear();
    if (clearDom) for (const [node, orig] of S.origs) if (node.isConnected) node.nodeValue = orig;
    S.origs = new Map();
    S.on = false; S.doneCount = 0; S.scanCount = 0; S.lazyMode = false;
    if (S.io) { S.io.disconnect(); S.io = null; }
  }

  async function loadCfg() {
    const r = await send({ type: 'config' });
    if (r.ok) { S.cfg = r.data; S.serverUp = true; }
    else { S.serverUp = false; }
    updateFloat();
    return S.serverUp;
  }

  async function init() {
    buildFloat();
    const ok = await loadCfg();
    if (!ok) {
      updateFloat('error', '无法连接 fanyi-server（127.0.0.1:8765）');
      /* 服务稍后可用时自动恢复 */
      setTimeout(async () => { if (!S.serverUp) { if (await loadCfg()) init2(); } }, 8000);
      return;
    }
    init2();
  }

  async function init2() {
    if (!S.cfg.enabled) { updateFloat(); return; }
    setupMutationObserver();
    hookRouting();
    if (S.cfg.auto_translate) {
      /* 先判定外文比例，超阈值才自动开 */
      const nodes = collectTextNodes(document.body);
      const ratio = classifyRatio(nodes, S.cfg.target_lang);
      S.lastRatio = ratio;
      if (ratio > (S.cfg.foreign_ratio ?? 0.6)) {
        S.on = true;
        scanAndEnqueue(0);
        pump();
        return;
      }
      updateFloat();
    }
  }

  /* ---------- 输入翻译面板（右键「输入翻译内容」） ---------- */
  const INPUT_LANGS = [
    ['zh', '中文'], ['zh-TW', '繁体中文'], ['en', '英语'], ['ja', '日语'], ['ko', '韩语'],
    ['fr', '法语'], ['de', '德语'], ['es', '西班牙语'], ['ru', '俄语'], ['pt', '葡萄牙语'],
    ['it', '意大利语'], ['ar', '阿拉伯语'], ['th', '泰语'], ['vi', '越南语'], ['id', '印尼语'],
    ['ms', '马来语'], ['hi', '印地语'], ['tr', '土耳其语'], ['pl', '波兰语'], ['nl', '荷兰语'],
    ['uk', '乌克兰语'],
  ];

  function isEditable(el) {
    if (!el || !el.tagName) return false;
    if (el.isContentEditable) return true;
    if (el.tagName === 'TEXTAREA') return true;
    if (el.tagName === 'INPUT') {
      return /^(text|search|url|email|number|tel|password|)$/.test(el.type || 'text');
    }
    return false;
  }

  /* 把译文写进原输入框（原生 setter + input 事件，React/Vue 等框架可感知） */
  function insertIntoEditable(el, text) {
    el.focus();
    if (el.isContentEditable) {
      el.focus();
      const sel = window.getSelection();
      if (sel && sel.rangeCount === 0 && el) {
        const range = document.createRange();
        range.selectNodeContents(el);
        range.collapse(false);
        sel.addRange(range);
      }
      document.execCommand('insertText', false, text);   /* 走浏览器撤销栈 */
      return;
    }
    const proto = el.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
    const setter = Object.getOwnPropertyDescriptor(proto, 'value').set;
    const cur = el.value ?? '';
    const s = el.selectionStart ?? cur.length;
    const e = el.selectionEnd ?? cur.length;
    setter.call(el, cur.slice(0, s) + text + cur.slice(e));
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
    try { el.selectionStart = el.selectionEnd = s + text.length; } catch {}
  }

  function closeInputPanel() {
    const host = document.getElementById('fanyi-input-host');
    if (host) host.remove();
  }

  let lastEditable = null;   /* 最近一次右键/聚焦的可输入元素（点击面板按钮时焦点会丢失，提前记下） */

  async function openInputTranslate() {
    closeInputPanel();
    const act = document.activeElement;
    let target = isEditable(act) ? act
               : (lastEditable && lastEditable.isConnected && isEditable(lastEditable) ? lastEditable : null);

    const host = document.createElement('div');
    host.id = 'fanyi-input-host';
    host.style.cssText = 'position:fixed;z-index:2147483647;';
    const shadow = host.attachShadow({ mode: 'open' });
    shadow.innerHTML = `
      <style>
        .panel {
          width: 340px; background: #1c2028; color: #e8ebf0; border-radius: 12px;
          box-shadow: 0 8px 30px rgba(0,0,0,.4); padding: 12px;
          font: 13px/1.5 -apple-system,"PingFang SC","Microsoft YaHei",sans-serif;
        }
        .head { display:flex; align-items:center; justify-content:space-between; margin-bottom:8px; }
        .title { font-weight:600; font-size:13px; }
        .title small { color:#8a93a3; font-weight:400; margin-left:6px; }
        .x { cursor:pointer; color:#8a93a3; font-size:16px; padding:0 4px; }
        .x:hover { color:#fff; }
        textarea {
          width:100%; box-sizing:border-box; min-height:72px; resize:vertical;
          background:#262b34; color:#e8ebf0; border:1px solid #3a4150; border-radius:8px;
          padding:8px; font:13px/1.5 inherit;
        }
        textarea:focus { outline:none; border-color:#3b82f6; }
        .row { display:flex; gap:8px; margin-top:8px; align-items:center; }
        select {
          flex:1; background:#262b34; color:#e8ebf0; border:1px solid #3a4150;
          border-radius:8px; padding:6px 8px; font-size:13px;
        }
        button {
          background:#2563eb; color:#fff; border:none; border-radius:8px;
          padding:7px 18px; cursor:pointer; font-size:13px; font-weight:600;
        }
        button:disabled { opacity:.55; cursor:default; }
        .status { margin-top:7px; font-size:12px; color:#8a93a3; min-height:16px; }
        .status.err { color:#ef4444; }
        .status.ok { color:#31d07e; }
      </style>
      <div class="panel">
        <div class="head">
          <span class="title">输入翻译<small>译后填入输入框</small></span>
          <span class="x" title="关闭">✕</span>
        </div>
        <textarea placeholder="用中文输入想表达的内容…"></textarea>
        <div class="row">
          <select></select>
          <button>翻 译</button>
        </div>
        <div class="status">${target ? '' : '⚠ 未检测到输入框：请先右键点击某个输入区域'}</div>
      </div>
    `;
    document.documentElement.appendChild(host);

    const panel = shadow.querySelector('.panel');
    const ta = shadow.querySelector('textarea');
    const sel = shadow.querySelector('select');
    const btn = shadow.querySelector('button');
    const status = shadow.querySelector('.status');
    const x = shadow.querySelector('.x');

    for (const [code, name] of INPUT_LANGS) {
      const opt = document.createElement('option');
      opt.value = code; opt.textContent = name;
      sel.appendChild(opt);
    }
    /* 默认目标语言：与页面翻译方向互补（页面英→汉时，输入翻译默认汉→英），并记住上次选择 */
    try {
      const last = (await chrome.storage.local.get('fanyi_input_lang')).fanyi_input_lang;
      if (last) sel.value = last;
      else {
        if (!S.cfg) { const r = await send({ type: 'config' }); if (r.ok) S.cfg = r.data; }
        sel.value = (S.cfg && S.cfg.target_lang !== 'zh') ? 'zh' : 'en';
      }
    } catch {}

    /* 定位：目标输入框下方，超出视口则内收 */
    let px = innerWidth / 2 - 170, py = 60;
    if (target && target.getBoundingClientRect) {
      const r = target.getBoundingClientRect();
      px = Math.min(Math.max(8, r.left), innerWidth - 356);
      py = r.bottom + 8;
      if (py + 200 > innerHeight) py = Math.max(8, r.top - 210);
    }
    host.style.left = px + 'px';
    host.style.top = py + 'px';

    const close = () => { document.removeEventListener('mousedown', onOutside, true); host.remove(); };
    const onOutside = (e) => {
      if (!host.contains(e.target) && e.target !== target) close();
    };
    document.addEventListener('mousedown', onOutside, true);
    x.addEventListener('click', close);
    document.addEventListener('keydown', (e) => { if (e.key === 'Escape') close(); }, { once: true });

    btn.addEventListener('click', async () => {
      const text = ta.value.trim();
      if (!text) { ta.focus(); return; }
      btn.disabled = true;
      status.className = 'status'; status.textContent = '翻译中…';
      const langName = INPUT_LANGS.find((l) => l[0] === sel.value)?.[1] || sel.value;
      const r = await send({ type: 'translate', segments: [text], target: sel.value });
      if (!r.ok) {
        status.className = 'status err';
        status.textContent = '✗ ' + (r.error || '服务未运行') + '（127.0.0.1:8765）';
        btn.disabled = false;
        return;
      }
      const out = r.results && r.results[0];
      if (typeof out !== 'string' || !out) {
        status.className = 'status err'; status.textContent = '✗ 翻译结果为空';
        btn.disabled = false;
        return;
      }
      if (target && target.isConnected) {
        insertIntoEditable(target, out);
        status.className = 'status ok';
        status.textContent = `✓ 已把${langName}内容填入输入框`;
        chrome.storage.local.set({ fanyi_input_lang: sel.value });
        setTimeout(close, 900);
      } else {
        status.className = 'status err'; status.textContent = '✗ 原输入框已不存在';
        btn.disabled = false;
      }
    });

    ta.focus();
  }

  /* 右键菜单消息（顶层帧与子帧都响应；Chromium/Firefox 原生菜单入口） */
  chrome.runtime.onMessage.addListener((msg, _sender, sendResponse) => {
    if (!msg) return;
    if (msg.type === 'inputTranslate') {
      const act = document.activeElement;
      if (isEditable(act)) lastEditable = act;
      try { openInputTranslate(); } catch (e) { console.error('[fanyi] input panel:', e); }
    } else if (msg.type === 'extensionUpdated') {
      /* 装载的是旧版脚本而扩展已更新：亮出「点击刷新」提示（新版脚本版本一致则忽略） */
      if (window.top === window && msg.version &&
          msg.version !== chrome.runtime.getManifest().version) {
        S.outdated = true;
        updateFloat();
      }
      sendResponse && sendResponse({ ok: true });
    }
  });

  /* 通用入口（全浏览器适用，不依赖 contextMenus API）：
     右键可输入区域时，在输入框右上角浮现「输入翻译内容」按钮。
     不拦截浏览器原生右键菜单（复制/粘贴不受影响），按钮 8 秒后自动消失。 */
  function setupUniversalInputEntry() {
    let chip = null, chipTimer = null;
    const removeChip = () => {
      if (chip) { chip.remove(); chip = null; }
      if (chipTimer) { clearTimeout(chipTimer); chipTimer = null; }
    };
    document.addEventListener('contextmenu', (e) => {
      const t = e.target;
      if (!t || !isEditable(t)) return;
      lastEditable = t;
      removeChip();
      chip = document.createElement('div');
      chip.id = 'fanyi-input-chip';
      chip.textContent = '输入翻译内容';
      chip.style.cssText =
        'position:fixed;z-index:2147483646;background:#1c2028;color:#fff;' +
        'padding:6px 13px;border-radius:15px;cursor:pointer;user-select:none;' +
        'font:600 12.5px/1.4 -apple-system,"PingFang SC","Microsoft YaHei",sans-serif;' +
        'box-shadow:0 3px 12px rgba(0,0,0,.35);';
      document.documentElement.appendChild(chip);
      const r = t.getBoundingClientRect();
      const x = Math.min(Math.max(6, r.right - 118), innerWidth - 130);
      let y = r.top - 34;
      if (y < 4) y = Math.min(r.bottom + 8, innerHeight - 38);
      chip.style.left = x + 'px';
      chip.style.top = y + 'px';
      chip.addEventListener('click', (ev) => {
        ev.preventDefault(); ev.stopPropagation();
        removeChip();
        openInputTranslate();
      }, true);
      chipTimer = setTimeout(removeChip, 8000);
    }, true);
    document.addEventListener('mousedown', (e) => {
      if (chip && e.target !== chip) removeChip();
    }, true);
  }
  setupUniversalInputEntry();

  const IN_IFRAME = (() => { try { return window.self !== window.top; } catch { return true; } })();

  /* 工具栏弹窗通信（仅顶层帧应答） */
  chrome.runtime.onMessage.addListener((msg, _sender, sendResponse) => {
    if (!msg || IN_IFRAME) return;
    if (msg.type === 'popupGetState') {
      sendResponse({
        on: S.on, translating: S.translating, done: S.doneCount,
        serverUp: S.serverUp, enabled: S.cfg ? S.cfg.enabled : null,
        ratio: S.lastRatio != null ? S.lastRatio : null,
      });
    } else if (msg.type === 'popupTogglePage') {
      onFloatClick();
      sendResponse({ ok: true });
    }
  });

  /* 子帧（iframe）：只提供输入翻译面板，不做页面翻译（省资源、避免翻广告帧） */
  if (IN_IFRAME) return;

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', () => setTimeout(init, 300));
  } else {
    setTimeout(init, 300);
  }
})();

/*
 * fanyi 扩展 — video.js
 * 网页视频字幕实时翻译：站点选择器优先（sites.js），通用「覆盖视频元素的高频
 * 文本」检测兜底。双语/仅译文两种模式，同句去重，不干扰页面翻译主模块。
 */
(() => {
  'use strict';
  if (window.__fanyiVideoLoaded) return;
  window.__fanyiVideoLoaded = true;

  const SEND = (msg) => new Promise((res) => {
    try {
      chrome.runtime.sendMessage(msg, (r) =>
        chrome.runtime.lastError ? res(null) : res(r || null));
    } catch { res(null); }
  });

  const CJK = /[\u3400-\u4DBF\u4E00-\u9FFF\uF900-\uFAFF\u3040-\u30FF\uAC00-\uD7AF]/;
  const SUB_CLASS = 'fanyi-video-sub';

  const S = {
    enabled: true,
    serverVideo: true,        // 服务端「视频字幕」开关（设置页/托盘可控）
    mode: 'bilingual',        // bilingual | translated
    target: 'zh',
    lastText: '',             // 上一条已处理字幕
    observer: null,
    siteSelectors: null,      // 当前站点的选择器数组（null = 通用模式）
    lastByEl: new WeakMap(),  // 元素 → 上次处理的文本
  };

  /* ---------- 工具 ---------- */
  const siteKey = () => location.hostname.replace(/^www\./, '');
  function matchSiteSelectors() {
    const host = siteKey();
    const table = window.__fanyiSites || {};
    for (const k in table) {
      if (host === k || host.endsWith('.' + k) || host.endsWith(k)) return table[k];
    }
    return null;
  }

  function overlapsVideo(el) {
    let r;
    try { r = el.getBoundingClientRect(); } catch { return false; }
    if (r.width < 2 || r.height < 2) return false;
    for (const v of document.querySelectorAll('video')) {
      const vr = v.getBoundingClientRect();
      if (r.left < vr.right && r.right > vr.left &&
          r.top < vr.bottom && r.bottom > vr.top) return true;
    }
    return false;
  }

  function isTargetLang(text) {
    if (/^(zh|ja|ko)/.test(S.target)) return CJK.test(text);
    return !CJK.test(text) && /\p{L}/u.test(text);   /* 拉丁系目标语言：粗判 */
  }

  function collectCandidates() {
    if (S.siteSelectors) {
      const out = [];
      for (const sel of S.siteSelectors)
        for (const el of document.querySelectorAll(sel))
          if (el.textContent.trim()) out.push(el);
      return out;
    }
    /* 通用模式：覆盖在视频上、含直接文本的元素 */
    const out = [];
    const seen = new Set();
    for (const el of document.querySelectorAll('div,span,p')) {
      if (seen.has(el)) continue;
      const v = [...el.childNodes].filter(n => n.nodeType === 3)
        .map(n => n.nodeValue).join('').trim();
      if (v.length < 2) continue;
      if (overlapsVideo(el)) { out.push(el); seen.add(el); }
    }
    return out;
  }

  /* ---------- 渲染 ---------- */
  function renderBilingual(el, text) {
    let t = el.nextElementSibling;
    if (!t || !t.classList || !t.classList.contains(SUB_CLASS)) {
      t = document.createElement('div');
      t.className = SUB_CLASS;
      t.style.cssText =
        'margin:4px auto 0;text-align:center;color:#fff;background:rgba(8,8,8,.72);' +
        'padding:2px 10px;border-radius:4px;font-size:.92em;line-height:1.35;' +
        'width:fit-content;max-width:100%;position:relative;z-index:2147483646;';
      el.after(t);
    }
    t.textContent = text;
  }

  function renderTranslated(el, text) {
    if (!el.dataset.fanyiOrig) el.dataset.fanyiOrig = el.textContent;
    el.textContent = text;
  }

  function restoreAll() {
    for (const t of document.querySelectorAll('.' + SUB_CLASS)) t.remove();
    for (const el of document.querySelectorAll('[data-fanyi-orig]')) {
      el.textContent = el.dataset.fanyiOrig;
      delete el.dataset.fanyiOrig;
    }
  }

  /* ---------- 翻译流程 ---------- */
  let busy = false;
  const pending = new Map();   // el → 原文

  async function process() {
    if (busy) return;
    if (!S.serverVideo) return;   /* 服务端「视频字幕」开关已关闭 */
    busy = true;
    try {
      for (const el of collectCandidates()) {
        const text = el.textContent.replace(/\s+/g, ' ').trim();
        if (!text || text === S.lastText) continue;
        if (S.lastByEl.get(el) === text) continue;
        if (isTargetLang(text)) continue;         /* 已是目标语言 */
        if (pending.has(el)) continue;
        pending.set(el, text);
        S.lastByEl.set(el, text);

        const r = await SEND({ type: 'translate', segments: [text], target: S.target });
        const out = r && r.ok && r.results && r.results[0];
        if (typeof out !== 'string' || !out) { pending.delete(el); continue; }

        if (!el.isConnected) { pending.delete(el); continue; }
        if (S.mode === 'translated') renderTranslated(el, out);
        else renderBilingual(el, out);
        pending.delete(el);
        S.lastText = text;
      }
    } finally { busy = false; }
  }

  /* ---------- 观察 ---------- */
  function start() {
    if (S.observer) return;
    if (!document.body) {   /* 脚本在 head 注入时 body 可能尚未解析 */
      document.addEventListener('DOMContentLoaded', () => start(), { once: true });
      return;
    }
    S.siteSelectors = matchSiteSelectors();
    S.observer = new MutationObserver(() => {
      clearTimeout(S.debounce);
      S.debounce = setTimeout(process, 200);
    });
    S.observer.observe(document.body, { childList: true, subtree: true, characterData: true });
    process();
  }
  function stop() {
    if (S.observer) { S.observer.disconnect(); S.observer = null; }
    restoreAll();
    S.lastText = '';
  }

  /* ---------- 初始化 ---------- */
  chrome.storage.local.get(['video_enabled', 'video_mode'], (d) => {
    if (d.video_enabled === false) S.enabled = false;
    if (d.video_mode) S.mode = d.video_mode;
    if (S.enabled) start();
  });
  if (!(chrome.storage.onChanged && chrome.storage.onChanged.addListener)) {
    return;   /* 环境无 storage 事件（异常环境），跳过监听 */
  }
  chrome.storage.onChanged.addListener((changes, area) => {
    if (area !== 'local') return;
    if (changes.video_enabled) {
      S.enabled = changes.video_enabled.newValue !== false;
      if (S.enabled) start(); else stop();
    }
    if (changes.video_mode) {
      S.mode = changes.video_mode.newValue || 'bilingual';
      restoreAll();
      if (S.enabled) process();
    }
  });

  /* 目标语言与视频字幕开关来自服务配置（10s 轮询） */
  async function pollConfig() {
    const r = await SEND({ type: 'config' });
    if (r && r.ok && r.data) {
      if (r.data.target_lang) S.target = r.data.target_lang;
      S.serverVideo = r.data.video_subtitle !== false;
    }
  }
  pollConfig();
  setInterval(pollConfig, 10000);

  /* 状态探针（调试与测试用） */
  window.__fanyiVideoDebug = {
    get state() {
      return {
        enabled: S.enabled, mode: S.mode, target: S.target, busy,
        observer: !!S.observer, lastText: S.lastText,
        selectors: S.siteSelectors ? S.siteSelectors.length : 'generic',
        bodyReady: !!document.body,
      };
    },
  };
})();

