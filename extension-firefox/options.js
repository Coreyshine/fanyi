/*
 * fanyi 扩展 — options.js
 * options 页面的服务状态检查（CSP 禁止内联脚本，必须放外部文件）
 */
fetch("http://127.0.0.1:8765/v1/status")
  .then((r) => r.json())
  .then((j) => {
    const el = document.getElementById("status");
    el.className = "st ok";
    el.textContent =
      "✓ fanyi-server 运行中（" + j.version + "，模型" +
      (j.model_loaded ? "已加载" : "待加载") + "，翻译方向 " +
      j.source_lang + " → " + j.target_lang + "）";
  })
  .catch(() => {
    const el = document.getElementById("status");
    el.className = "st bad";
    el.textContent = "✗ 连不上 fanyi-server —— 请先启动 fanyi-server（见项目 README.md）";
  });
