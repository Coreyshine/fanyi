#!/bin/bash
# 从 extension/ 重新生成 extension-firefox/（Firefox 与 Chrome 的 MV3 后台机制
# 互不兼容：Chrome 只认 service_worker，Firefox 用 scripts。改完扩展代码后
# 跑一次本脚本即可保持两份同步。
set -euo pipefail
cd "$(dirname "$0")/.."

[ -f extension/manifest.json ] || { echo "extension/ 不存在"; exit 1; }

mkdir -p extension-firefox/icons
cp extension/background.js extension/content.js extension/options.html extension/popup.html extension/popup.js extension-firefox/
cp extension/icons/*.png extension-firefox/icons/
# Firefox 差异：background 用 scripts、gecko 元数据；其余与 Chrome 版一致
python3 - <<'EOF'
import json
m = json.load(open('extension/manifest.json'))
m.pop('background', None)
m['background'] = {'scripts': ['background.js']}
m['browser_specific_settings'] = {'gecko': {'id': 'fanyi@local.desktop', 'strict_min_version': '115.0'}}
json.dump(m, open('extension-firefox/manifest.json', 'w'), ensure_ascii=False, indent=2)
EOF
echo "extension-firefox/ 已同步"
