/*
 * fanyi 扩展 — background.js（MV3 service worker）
 * 中转：content script ↔ http://127.0.0.1:8765（扩展有 host 权限，不受页面 CORS 限制）
 * 另负责注册右键菜单「输入翻译内容」。
 */
const SERVER = "http://127.0.0.1:8765";

/* 右键菜单：只在可输入区域显示 */
chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.create({
    id: "fanyi-input-translate",
    title: "输入翻译内容",
    contexts: ["editable"],
  });
  /* 安装/重载后，自动给所有已打开的网页注入脚本——旧页面无需手动刷新 */
  injectIntoExistingTabs();
});

/* 向所有已打开的 http(s) 标签页注入 content script，并通知版本变化。
   已有旧脚本的页面（版本号不一致）会把悬浮开关变为「点击刷新」提示。 */
async function injectIntoExistingTabs() {
  const version = chrome.runtime.getManifest().version;
  let tabs = [];
  try {
    tabs = await chrome.tabs.query({});
  } catch {
    return;
  }
  for (const t of tabs) {
    if (t.id == null) continue;
    try {
      await chrome.scripting.executeScript({
        target: { tabId: t.id, allFrames: true },
        files: ["content.js"],
      });
      chrome.tabs.sendMessage(t.id, { type: "extensionUpdated", version }, () => void chrome.runtime.lastError);
    } catch {}   /* chrome:// 等特殊页面无法注入，忽略 */
  }
}

chrome.contextMenus.onClicked.addListener((info, tab) => {
  if (info.menuItemId !== "fanyi-input-translate" || tab?.id == null) return;
  const msg = { type: "inputTranslate" };
  /* 优先发给右键所在的帧（iframe 里的输入框）；没有内容脚本则退回顶层帧 */
  chrome.tabs
    .sendMessage(tab.id, msg, { frameId: info.frameId })
    .catch(() => chrome.tabs.sendMessage(tab.id, msg).catch(() => {}));
});

async function fetchJson(path, opts) {
  const r = await fetch(SERVER + path, opts);
  if (!r.ok) {
    let msg = "HTTP " + r.status;
    try { const j = await r.json(); if (j.error) msg = j.error; } catch {}
    throw new Error(msg);
  }
  return r.json();
}

chrome.runtime.onMessage.addListener((msg, _sender, sendResponse) => {
  (async () => {
    try {
      if (msg.type === "config") {
        sendResponse({ ok: true, data: await fetchJson("/v1/config") });
      } else if (msg.type === "toggleEnabled") {
        const cur = await fetchJson("/v1/config");
        await fetchJson("/v1/config", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ enabled: !cur.enabled }),
        });
        sendResponse({ ok: true, enabled: !cur.enabled });
      } else if (msg.type === "setVideoSubtitle") {
        const cur = await fetchJson("/v1/config");
        await fetchJson("/v1/config", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ video_subtitle: !!msg.value }),
        });
        sendResponse({ ok: true, value: !!msg.value });
      } else if (msg.type === "status") {
        sendResponse({ ok: true, data: await fetchJson("/v1/status") });
      } else if (msg.type === "translate") {
        const ac = new AbortController();
        const kill = setTimeout(() => ac.abort(), 20000);   /* 服务无响应 20s 熔断 */
        const r = await fetch(SERVER + "/v1/translate", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ segments: msg.segments, target: msg.target }),
          signal: ac.signal,
        });
        clearTimeout(kill);
        if (!r.ok) {
          let m = "HTTP " + r.status;
          try { const j = await r.json(); if (j.error) m = j.error; } catch {}
          throw new Error(m);
        }
        /* NDJSON：每行 {"i":n,"text":".."} */
        const text = await r.text();
        const results = new Array(msg.segments.length).fill(null);
        for (const line of text.split("\n")) {
          if (!line.trim()) continue;
          try {
            const j = JSON.parse(line);
            if (typeof j.i === "number" && typeof j.text === "string") results[j.i] = j.text;
          } catch {}
        }
        sendResponse({ ok: true, results });
      } else {
        sendResponse({ ok: false, error: "unknown message" });
      }
    } catch (e) {
      sendResponse({ ok: false, error: String(e.message || e) });
    }
  })();
  return true;   /* 异步 sendResponse */
});
