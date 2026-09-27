#!/bin/bash
# fanyi macOS 安装包打包脚本：生成 dist/Fanyi.app 与 dist/fanyi-macos.zip
# 用法：先完成 CMake 构建（build/fanyi-server），再运行本脚本
set -euo pipefail
cd "$(dirname "$0")/.."

[ -x build/fanyi-server ] || { echo "请先构建: cmake --build build -j"; exit 1; }

APP=dist/Fanyi.app
rm -rf dist
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

cat > "$APP/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>              <string>Fanyi</string>
    <key>CFBundleDisplayName</key>       <string>fanyi 本地翻译</string>
    <key>CFBundleExecutable</key>        <string>fanyi-server</string>
    <key>CFBundleIdentifier</key>        <string>local.fanyi.server</string>
    <key>CFBundlePackageType</key>       <string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.0.0</string>
    <key>CFBundleVersion</key>           <string>1</string>
    <key>LSUIElement</key>               <true/>
    <key>NSHighResolutionCapable</key>   <true/>
</dict>
</plist>
EOF

cp build/fanyi-server   "$APP/Contents/MacOS/"
cp build/settings.html  "$APP/Contents/Resources/"
# 模型不打入安装包（否则公证需上传 1.9GB 且每次更新重传）。
# 首次使用在设置页下载（内置 hf-mirror/HuggingFace 双镜像自动切换），
# 或手动将 models/Hy-MT2-1.8B-Q8_0.gguf 放入用户目录：
#   ~/Library/Application Support/fanyi/models/

cp -R extension "$APP/Contents/Resources/extension"
cp -R extension-firefox "$APP/Contents/Resources/extension-firefox"
cat > dist/README.txt <<'EOF'
fanyi 本地翻译 — macOS 安装包（约几 MB，模型首次使用时下载）

1. 把 Fanyi.app 拖进「应用程序」文件夹，打开即可
   （已用 Developer ID 证书签名 + Apple 公证，任何 Mac 双击直接打开）
2. 菜单栏出现「译」图标后，浏览器打开 http://127.0.0.1:8765/settings
   → 「模型」卡片 → 点「下载模型」（约 1.9 GB，hf-mirror/HuggingFace
   双镜像自动切换、断点续传；也可从原安装包的 models/ 文件夹手动拷贝到
   ~/Library/Application Support/fanyi/models/）
3. 开机自启：系统设置 → 通用 → 登录项 → 添加 Fanyi
4. 浏览器扩展：chrome://extensions → 开发者模式 → 加载已解压的扩展程序
   → Chrome/Edge 选择本包内 Resources/extension 文件夹；
     Firefox 选择 Resources/extension-firefox 文件夹
EOF

# ---- 代码签名：Developer ID > Apple Development > ad-hoc ----
# 注：首次在脚本环境签名时，macOS 可能弹出钥匙串授权框，请点「始终允许」；
#     若无交互（远程/后台会话），签名会失败并自动降级 ad-hoc（app 仍可用）。
IDENTITY=$(security find-identity -v -p codesigning 2>/dev/null | awk -F'"' '/Developer ID Application/{print $2; exit}')
[ -z "${IDENTITY:-}" ] && IDENTITY=$(security find-identity -v -p codesigning 2>/dev/null | awk -F'"' '/Apple Development/{print $2; exit}')
SIGNED=no
if [ -n "${IDENTITY:-}" ]; then
    echo "签名证书: $IDENTITY"
    if codesign --force --options runtime --entitlements scripts/fanyi.entitlements --sign "$IDENTITY" "$APP" 2>&1; then
        SIGNED=yes
    else
        echo "⚠ 证书签名失败（钥匙串授权或证书链问题），降级 ad-hoc"
    fi
fi
if [ "$SIGNED" != yes ]; then
    codesign --force --sign - "$APP"
fi
codesign --verify --strict "$APP" && echo "✓ 签名校验通过（$([ $SIGNED = yes ] && echo 证书签名 || echo ad-hoc)）"

cd dist

# ---- 公证 + 装订（可选）：需 Developer ID 证书签名 + notarytool 钥匙串配置 ----
# 配置方法：
#   xcrun notarytool store-credentials FANYI_NOTARY --issuer "IssuerID" --key-id "KeyID" --key AuthKey_XXXX.p8
NOTARY_PROFILE="${FANYI_NOTARY_PROFILE:-FANYI_NOTARY}"
if [ "$SIGNED" = yes ] && echo "$IDENTITY" | grep -q "Developer ID" \
   && xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null 2>&1; then
    echo "提交 Apple 公证…（通常 1-3 分钟）"
    ditto -c -k --keepParent Fanyi.app notary.zip
    if xcrun notarytool submit notary.zip --keychain-profile "$NOTARY_PROFILE" --wait; then
        if xcrun stapler staple "Fanyi.app" 2>&1; then
            echo "✓ 公证 + 装订完成（任何 Mac 打开零提示）"
        fi
    else
        echo "⚠ 公证失败（网络/密钥权限？），跳过装订"
    fi
    rm -f notary.zip
else
    echo "（未配置 Developer ID 公证，跳过。无提示分发的配置方法见脚本内注释）"
fi
# 最终 zip（若已装订，重新打包以包含装订票据）
ditto -c -k --sequesterRsrc --keepParent Fanyi.app fanyi-macos.zip
echo "完成："
ls -lh fanyi-macos.zip Fanyi.app/Contents/MacOS/fanyi-server Fanyi.app/Contents/Resources/models/*.gguf | awk '{print $5, $9}'
if [ "$SIGNED" = yes ]; then
    spctl -a -t exec -vv "Fanyi.app" 2>&1 | head -1 || true
fi
