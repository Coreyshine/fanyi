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

  /* 目标语言来自服务配置 */
  SEND({ type: 'config' }).then((r) => {
    if (r && r.ok && r.data && r.data.target_lang) S.target = r.data.target_lang;
  });

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
