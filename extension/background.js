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
});

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
      } else if (msg.type === "status") {
        sendResponse({ ok: true, data: await fetchJson("/v1/status") });
      } else if (msg.type === "translate") {
        const r = await fetch(SERVER + "/v1/translate", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ segments: msg.segments, target: msg.target }),
        });
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
