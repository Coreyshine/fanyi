# fanyi — 本地模型实时网页翻译

[简体中文] | [English](README.en.md)

用本地翻译模型（腾讯混元 Hy-MT2-1.8B）对你浏览的**所有网页实时翻译**，全程不联网。
只要 **fanyi-server 在运行**，浏览器里的每个页面都会自动检测、自动翻译。

- **形态**：C/C++ 本地翻译服务（llama.cpp 推理 + localhost HTTP API）+ 浏览器扩展（Chrome/Edge/Firefox）
- **默认方向**：英译汉，可在设置页改成任意方向（30+ 语言互译）
- **自动翻译**：页面外文占比超过阈值（默认 60%）才启动，中文页面不浪费算力
- **悬浮开关**：页面右上角"译"按钮，点击翻译/还原，可拖动，显示进度
- **输入翻译**：在任何输入框/文本域/富文本编辑区右键 →「输入翻译内容」→ 面板里输入中文、
  选目标语言、点翻译，译文自动填入原输入框（保留光标位置，兼容 React/Vue 等框架）。
  双入口：浏览器原生右键菜单（Chromium/Firefox）+ 页面内浮动按钮（全浏览器通用，含 iframe 内输入框）
- **低资源**：模型懒加载、空闲自动卸载、长页面只翻可见区、翻译结果本地缓存
- **精准**：Q8_0 量化（近无损）+ 可选贪心"精准模式"

## 快速开始（macOS）

```bash
# 1. 构建（需要 Xcode Command Line Tools + CMake）
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 2. 启动服务（会常驻托盘，图标为"译"）
./build/fanyi-server

# 3. 安装浏览器扩展
#    Chrome/Edge: 扩展管理 → 开发者模式 → 加载已解压的扩展程序 → 选择 extension/ 目录
#    Firefox:     同样方式加载 extension-firefox/ 目录（manifest 已是 Firefox 专用版）
```

打开任意英文网页（如 `https://www.bbc.com/news`），外文占比超 60% 时自动翻译，
右上角悬浮开关可随时暂停/还原。

设置页：浏览器访问 <http://127.0.0.1:8765/settings>，或在托盘菜单点"打开设置…"。

## Windows / Linux

见 [docs/BUILD.md](docs/BUILD.md)。

## 目录结构

```
fanyi/
├── CMakeLists.txt            # 构建脚本
├── models/                   # Hy-MT2-1.8B-Q8_0.gguf（约 1.9 GB）
├── server/                   # C/C++ 翻译服务
│   ├── include/fanyi/translator.h   # 翻译核心 C 接口
│   ├── src/translator.c      # llama.cpp 封装（并行批量解码）
│   ├── src/http_server.cpp   # REST API + 设置页
│   ├── src/config.c          # JSON 配置持久化
│   ├── src/tray_mac.mm / tray_win.cpp / tray_linux.cpp  # 托盘
│   └── assets/settings.html  # 设置页面
├── extension/                # 浏览器扩展（MV3）
│   ├── manifest.json
│   ├── background.js         # 扩展与本地服务通信
│   ├── content.js            # 取词/检测/替换/悬浮开关
│   └── options.html
├── third_party/              # llama.cpp、cJSON、cpp-httplib（固定版本）
└── docs/BUILD.md             # 三平台构建说明
```

## HTTP API（仅监听 127.0.0.1）

| 端点 | 说明 |
|---|---|
| `GET /v1/status` | 服务状态、模型加载状态 |
| `GET /v1/config` | 当前配置 |
| `POST /v1/config` | 更新配置（JSON 字段同 GET） |
| `POST /v1/translate` | `{"segments":["..."],"target":"zh"}` → NDJSON 流式返回 |
| `GET /settings` | 设置页面 |

## 常见问题

- **悬浮开关显示"服务未运行"**：启动 `fanyi-server` 后点一下开关会自动重连。
- **首次翻译很慢**：模型加载需 1-3 秒，之后常驻；空闲 10 分钟（可设）自动卸载省内存。
- **某些网站不翻译**：代码块（`code/pre`）、输入框内容默认不翻译。
- **换模型**：设置页可改模型路径，例如低内存机器可换 Q6_K 版本（约 1.5 GB）。

## 量化版本说明

| 版本 | 体积 | 质量 | 建议 |
|---|---|---|---|
| Q8_0（默认） | 1.9 GB | 近无损 | **推荐**，翻译精准优先 |
| Q6_K | 1.5 GB | 很好 | 低内存机器备选 |
| Q4_K_M | 1.1 GB | 有折损 | 不推荐用于正式翻译 |

翻译模型：[腾讯混元 Hy-MT2-1.8B-GGUF](https://huggingface.co/tencent/Hy-MT2-1.8B-GGUF)（官方转换版；启动后在设置页一键下载，内置多镜像自动切换）。
推理引擎：[llama.cpp](https://github.com/ggml-org/llama.cpp) v0.5.0（MIT）。
