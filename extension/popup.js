/*
 * fanyi 扩展 — popup.js
 * 工具栏弹窗：本页翻译开关、全局总开关、打开设置。
 */
const $ = (id) => document.getElementById(id);

function sendBG(msg) {
  return new Promise((resolve) => {
    try {
      chrome.runtime.sendMessage(msg, (r) => {
        if (chrome.runtime.lastError) resolve({ ok: false, error: chrome.runtime.lastError.message });
        else resolve(r || { ok: false, error: 'no response' });
      });
    } catch (e) { resolve({ ok: false, error: String(e) }); }
  });
}

function sendTab(tabId, msg) {
  return new Promise((resolve) => {
    try {
      chrome.tabs.sendMessage(tabId, msg, (r) => {
        if (chrome.runtime.lastError) resolve(null);
        else resolve(r || null);
      });
    } catch { resolve(null); }
  });
}

function sendBG(msg) {
  return new Promise((resolve) => {
    let done = false;
    const finish = (v) => { if (!done) { done = true; resolve(v); } };
    const timer = setTimeout(() => finish(null), 25000);
    try {
      chrome.runtime.sendMessage(msg, (r) => {
        if (done) return;
        done = true; clearTimeout(timer);
        chrome.runtime.lastError ? resolve(null) : resolve(r || null);
      });
    } catch { done = true; clearTimeout(timer); resolve(null); }
  });
}

async function init() {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  const cfg = await sendBG({ type: 'config' });

  /* 服务器状态 + 全局开关 + 方向 */
  if (cfg.ok) {
    $('srvDot').classList.add('on');
    $('srvText').textContent = '服务运行中';
    $('globalToggle').checked = !!cfg.data.enabled;
    $('dirText').textContent = `翻译方向：${cfg.data.source_lang} → ${cfg.data.target_lang}`;
  } else {
    $('warn').style.display = 'block';
    $('warn').textContent = '✗ 无法连接 fanyi-server，请先启动 Fanyi';
    $('srvText').textContent = '未运行';
    $('globalToggle').disabled = true;
  }

  /* 本页翻译状态（仅顶层帧的内容脚本会应答） */
  let st = null;
  if (tab && tab.id != null && /^https?:/.test(tab.url || '')) {
    st = await sendTab(tab.id, { type: 'popupGetState' });
  }
  if (!st) {
    $('pageToggle').disabled = true;
    $('pageSub').textContent = '此页面不可用（需为普通网页）';
  } else {
    $('pageToggle').checked = !!st.on;
    $('pageSub').textContent = st.translating ? '翻译中…'
      : st.on ? `已译 ${st.done} 段`
      : st.ratio != null ? `外文占比 ${Math.round(st.ratio * 100)}%` : '当前页面';
    if (!st.serverUp) { $('pageToggle').disabled = true; $('pageSub').textContent = '服务未运行'; }
    else if (cfg.ok && cfg.data.enabled === false) { $('pageToggle').disabled = true; $('pageSub').textContent = '总开关已暂停'; }
  }

  $('pageToggle').addEventListener('change', async (e) => {
    if (!tab || tab.id == null) return;
    await sendTab(tab.id, { type: 'popupTogglePage' });
    const ns = await sendTab(tab.id, { type: 'popupGetState' });
    e.target.checked = !!(ns && ns.on);
    if (ns) $('pageSub').textContent = ns.translating ? '翻译中…' : ns.on ? `已译 ${ns.done} 段` : '已还原';
  });

  $('globalToggle').addEventListener('change', async (e) => {
    const r = await sendBG({ type: 'toggleEnabled' });
    if (!r.ok) e.target.checked = !!r.enabled;   /* 失败回滚 */
    $('warn').style.display = r.ok ? 'none' : 'block';
    if (!r.ok) $('warn').textContent = '✗ ' + (r.error || '操作失败');
  });

  /* 视频字幕开关与模式（chrome.storage，video.js 监听生效） */
  chrome.storage.local.get(['video_enabled', 'video_mode'], (d) => {
    $('videoToggle').checked = d.video_enabled !== false;
    $('videoMode').value = d.video_mode || 'bilingual';
    $('videoModeRow').style.display = $('videoToggle').checked ? 'flex' : 'none';
  });
  $('videoToggle').addEventListener('change', async (e) => {
    chrome.storage.local.set({ video_enabled: e.target.checked });
    await SEND({ type: 'setVideoSubtitle', value: e.target.checked });   /* 与设置页/托盘同步 */
    $('videoModeRow').style.display = e.target.checked ? 'flex' : 'none';
  });
  $('videoMode').addEventListener('change', (e) => {
    chrome.storage.local.set({ video_mode: e.target.value });
  });

  $('settingsBtn').addEventListener('click', () => {
    const port = (cfg.ok && cfg.data.port) || 8765;
    chrome.tabs.create({ url: `http://127.0.0.1:${port}/settings` });
    window.close();
  });
}

init();
