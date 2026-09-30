# fanyi — Local Real-Time Web Page Translation

**English** | [简体中文](README.md)

Translate any web page in real time with a **fully local AI model** (Tencent Hunyuan Hy-MT2-1.8B). Nothing ever leaves your machine.

- **Form**: C/C++ local translation service (llama.cpp inference + localhost HTTP API) + browser extension (Chrome/Edge/Firefox)
- **Default direction**: English → Chinese, any of 30+ language pairs configurable
- **Automatic**: translates when foreign text exceeds a threshold (default 60%); Chinese pages are left untouched
- **Floating switch**: translate/restore any page, draggable, shows progress
- **Selection & screenshot translation**: select text anywhere or capture a screen region and get the translation instantly
- **Lightweight**: lazy model loading, idle auto-unload, visible-area-first for long pages, local translation cache
- **Accurate**: Q8_0 quantization (near lossless) + optional greedy "precision mode"
- **Runs everywhere**: devices without a dedicated GPU do all inference on the CPU —
  integrated graphics and older machines work fine; Apple Silicon and common GPUs
  get automatic acceleration, zero configuration
- **Beyond the browser**: selection translation (select & translate) + screenshot
  translation (capture any screen region, built-in lightweight OCR, just 15 MB)

## Quick Start (macOS)

```bash
# 1. Build (requires Xcode Command Line Tools + CMake)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 2. Start the service (lives in the menu bar as「译」)
./build/fanyi-server

# 3. Install the browser extension
#    Chrome/Edge: chrome://extensions → Developer mode → Load unpacked → extension/ folder
#    Firefox: load extension-firefox/ via about:debugging
```

Open any foreign-language page — it translates automatically. The floating
switch (top-right of every page) pauses/restores; the tray menu opens
Settings or pauses translation globally.

Settings: <http://127.0.0.1:8765/settings>

## Model Download

On first use, open Settings → **Model** and pick one of three quantizations
(device RAM is auto-detected and a recommendation is highlighted):

| Version | Size | Quality | RAM in use | Suggested |
|---|---|---|---|---|
| Q8_0 (default) | 1.9 GB | Near lossless | ~2.5 GB | Recommended, accuracy first |
| Q6_K | 1.5 GB | Very good | ~2.1 GB | Balanced choice |
| Q4_K_M | 1.1 GB | Degraded | ~1.6 GB | Low-memory machines |

Downloads use automatic multi-mirror failover
(hf-mirror.com → HuggingFace) with resume support. The OCR models for
screenshot translation (~15 MB) download the same way.

## HTTP API (localhost only)

| Endpoint | Description |
|---|---|
| `GET /v1/status` | Service & model status |
| `GET /v1/config` | Current configuration |
| `POST /v1/config` | Update configuration |
| `POST /v1/translate` | `{"segments":["..."],"target":"zh"}` → NDJSON stream |
| `GET /v1/models` | Model catalog + device recommendation |
| `GET /settings` | Settings page |

## Translation Model

[Tencent Hunyuan Hy-MT2-1.8B-GGUF](https://huggingface.co/tencent/Hy-MT2-1.8B-GGUF)
(official conversion; downloaded in one click from the Settings page with
automatic multi-mirror failover).
Inference engine: [llama.cpp](https://github.com/ggml-org/llama.cpp) v0.5.0 (MIT).

## License

Apache License 2.0 — see [LICENSE](LICENSE).
