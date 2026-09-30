# fanyi 三平台构建指南

## 获取代码与第三方依赖

```bash
git clone --recurse-submodules <本项目地址>
# 或已克隆后补拉依赖：
git submodule update --init --depth 1      # llama.cpp v0.5.0（固定版本）
```

`third_party/cJSON` 与 cpp-httplib 为仓库内文件，随代码一起克隆。

## 依赖

| 平台 | 编译器 | 其他 |
|---|---|---|
| macOS | Xcode Command Line Tools（clang） | CMake ≥ 3.16 |
| Windows | Visual Studio 2022（C++ 桌面开发）或 `clang` | CMake ≥ 3.16 |
| Linux | gcc ≥ 9 或 clang | CMake ≥ 3.16、pthread、dl |

模型文件 `models/Hy-MT2-1.8B-Q8_0.gguf`（约 1.9 GB）需放在项目根的 `models/` 下
（或安装目录旁 `models/`，或设置页自定义路径）。

```bash
# 从官方仓库下载（任选一量化版本；也可启动服务后在设置页一键下载）
curl -L -o models/Hy-MT2-1.8B-Q8_0.gguf   "https://hf-mirror.com/tencent/Hy-MT2-1.8B-GGUF/resolve/main/Hy-MT2-1.8B-Q8_0.gguf"
# 低内存机器可换 Q6_K（1.5 GB），并在设置页把模型路径指向它
```

## macOS

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/fanyi-server            # 常驻托盘（状态栏"译"图标）
./build/fanyi-server --no-tray  # 无托盘运行
# 测试工具：./build/fanyi-cli -m models/Hy-MT2-1.8B-Q8_0.gguf "Hello world"
```

- 默认启用 Metal GPU 加速（shader 已嵌入二进制，无额外文件）。

## Windows

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
.\build\Release\fanyi-server.exe   # 托盘图标在右下角通知区
```

- 若用 Visual Studio 生成器，产物在 `build/Release/`；
  把 `settings.html` 与 `models\` 放在 exe 旁（CMake 已自动复制 settings.html）。
- 纯 CPU 推理，无需任何 GPU 运行库。

## Linux

```bash
sudo apt install cmake build-essential        # Debian/Ubuntu
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/fanyi-server --no-tray                # 推荐：无 GUI 依赖的常驻服务
```

- 可选托盘（X11/GNOME/KDE）：先装依赖再开开关
  ```bash
  sudo apt install libayatana-appindicator3-dev libgtk-3-dev
  cmake -B build -DCMAKE_BUILD_TYPE=Release -DFANYI_TRAY=ON
  cmake --build build -j
  ```
- Wayland 下托盘兼容性参差，推荐 `--no-tray` + systemd 用户服务：
  ```ini
  # ~/.config/systemd/user/fanyi.service
  [Unit]
  Description=fanyi local translation server
  [Service]
  ExecStart=%h/Projects/fanyi/build/fanyi-server --no-tray
  Restart=on-failure
  [Install]
  WantedBy=default.target
  ```
  `systemctl --user enable --now fanyi`

## 开机自启

- **macOS**：系统设置 → 通用 → 登录项 → 添加 `fanyi-server`
- **Windows**：`Win+R` 输入 `shell:startup`，放入 fanyi-server.exe 的快捷方式
- **Linux**：上面的 systemd 用户服务，或桌面环境的"自启动"设置

## 安装浏览器扩展（一次性）

1. 打开浏览器扩展管理页：
   - Chrome/Edge：地址栏输入 `chrome://extensions`（Edge 为 `edge://extensions`）
2. 右上角开启 **开发者模式**
3. 点 **加载已解压的扩展程序**，选择本项目的 `extension/` 目录
4. Firefox：直接加载 `extension-firefox/` 文件夹（manifest 已预置 Firefox 版）：
   `about:debugging#/runtime/this-firefox` →「临时载入附加组件」→ 选其中的
   `manifest.json`；长期使用可用 [web-ext](https://mozilla.github.io/web-ext/) 打包签名。
   扩展代码有改动后运行 `scripts/sync_firefox.sh` 同步两份文件夹。

## 安装包

```bash
# macOS：生成 dist/Fanyi.app（内含模型与扩展）和 dist/fanyi-macos.zip
./scripts/package_macos.sh
```

- Windows/Linux 安装包（Inno Setup / AppImage / deb）需在对应系统上构建：
  CMake 产物 + models/ + extension/ 三样打包即可，服务无其它运行时依赖

## 构建常见问题

- **`unknown model architecture: 'hunyuan-dense'`**：llama.cpp 版本过旧
  （需要 2025-07 之后，本仓库固定 v0.5.0 无此问题）
- **端口被占用**：`--port N` 换端口，或设置页改端口后重启服务
- **首次翻译慢**：冷启动需加载模型（约 1-3 秒 + Metal 首次编译着色器），
  之后常驻内存；空闲 10 分钟（可配置）自动卸载
