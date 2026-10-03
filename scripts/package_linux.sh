#!/bin/bash
# fanyi Linux 安装包打包脚本：生成 dist/fanyi-linux-x64.deb 和 dist/fanyi-linux-x64.AppImage
# 用法：先完成 CMake 构建（build/fanyi-server、build/fanyi-cli），再运行本脚本
#   ./scripts/package_linux.sh
# 版本号：优先取环境变量 FANYI_VERSION（CI 里为 tag 名，如 v1.1.0），否则用默认值
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${FANYI_VERSION:-1.1.0}"
VERSION="${VERSION#v}"

[ -x build/fanyi-server ] || { echo "请先构建: cmake --build build --target fanyi-server fanyi-cli"; exit 1; }

rm -rf dist
mkdir -p dist

# ============================================================
# 1) .deb 包（Debian/Ubuntu：dpkg -i fanyi-linux-x64.deb）
#    安装位置 /usr/local/lib/fanyi/，命令行入口 /usr/local/bin/fanyi-server
# ============================================================
DEBROOT=dist/deb-root
mkdir -p "$DEBROOT/DEBIAN" \
         "$DEBROOT/usr/local/lib/fanyi" \
         "$DEBROOT/usr/local/bin" \
         "$DEBROOT/usr/share/applications" \
         "$DEBROOT/usr/share/icons/hicolor/128x128/apps"

install -m 755 build/fanyi-server  "$DEBROOT/usr/local/lib/fanyi/fanyi-server"
install -m 755 build/fanyi-cli     "$DEBROOT/usr/local/lib/fanyi/fanyi-cli" 2>/dev/null || true
install -m 644 build/settings.html "$DEBROOT/usr/local/lib/fanyi/settings.html"
cp -r extension        "$DEBROOT/usr/local/lib/fanyi/extension"
cp -r extension-firefox "$DEBROOT/usr/local/lib/fanyi/extension-firefox" 2>/dev/null || true
install -m 644 extension/icons/fanyi128.png "$DEBROOT/usr/share/icons/hicolor/128x128/apps/fanyi.png"

ln -sf /usr/local/lib/fanyi/fanyi-server "$DEBROOT/usr/local/bin/fanyi-server"

cat > "$DEBROOT/usr/share/applications/fanyi.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=fanyi 本地翻译
Comment=本地模型实时网页翻译（翻译全程不联网）
Exec=/usr/local/lib/fanyi/fanyi-server
Icon=fanyi
Terminal=false
Categories=Network;Utility;
X-GNOME-Autostart-enabled=true
EOF

cat > "$DEBROOT/DEBIAN/control" <<EOF
Package: fanyi
Version: $VERSION
Section: net
Priority: optional
Architecture: amd64
Depends: curl
Maintainer: fanyi <fanyi@local.desktop>
Description: 本地模型实时网页翻译服务
 浏览器菜单栏"译"图标 + 浏览器扩展 = 打开外文网页自动变中文。
 翻译模型（约 1.9 GB）不随包附带，首次使用在设置页一键下载：
   http://127.0.0.1:8765/settings
EOF

cat > "$DEBROOT/DEBIAN/postinst" <<'EOF'
#!/bin/bash
# 升级时旧进程还在运行会占用文件，这里温和重启
pkill -f /usr/local/lib/fanyi/fanyi-server 2>/dev/null && sleep 1 || true
exit 0
EOF
chmod 755 "$DEBROOT/DEBIAN/postinst"

dpkg-deb --build --root-owner-group "$DEBROOT" "dist/fanyi-linux-x64.deb"
rm -rf "$DEBROOT"
echo "✓ dist/fanyi-linux-x64.deb"

# ============================================================
# 2) AppImage（免安装、免 root，任何 x86_64 发行版直接运行）
# ============================================================
APPDIR=dist/AppDir
mkdir -p "$APPDIR/usr/bin" \
         "$APPDIR/usr/lib/fanyi" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/128x128/apps"

install -m 755 build/fanyi-server  "$APPDIR/usr/lib/fanyi/fanyi-server"
install -m 755 build/fanyi-cli     "$APPDIR/usr/lib/fanyi/fanyi-cli" 2>/dev/null || true
install -m 644 build/settings.html "$APPDIR/usr/lib/fanyi/settings.html"
cp -r extension        "$APPDIR/usr/lib/fanyi/extension"
cp -r extension-firefox "$APPDIR/usr/lib/fanyi/extension-firefox" 2>/dev/null || true
install -m 644 extension/icons/fanyi128.png "$APPDIR/usr/share/icons/hicolor/128x128/apps/fanyi.png"

# AppRun（AppImage 入口）：转到真实目录再启动，保证 exe 旁 settings.html/models 可被找到
cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"
exec "$HERE/usr/lib/fanyi/fanyi-server" "$@"
EOF
chmod 755 "$APPDIR/AppRun"

# 命令行入口
cat > "$APPDIR/usr/bin/fanyi-server" <<'EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"
exec "$HERE/../lib/fanyi/fanyi-server" "$@"
EOF
chmod 755 "$APPDIR/usr/bin/fanyi-server"

cat > "$APPDIR/usr/share/applications/fanyi.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=fanyi 本地翻译
Comment=本地模型实时网页翻译（翻译全程不联网）
Exec=fanyi-server
Icon=fanyi
Terminal=false
Categories=Network;Utility;
X-GNOME-Autostart-enabled=true
EOF

# appimagetool 官方工具；--appimage-extract-and-run 免 FUSE，CI/容器环境可用
APPIMAGETOOL=dist/appimagetool.AppImage
curl -sL --retry 3 -o "$APPIMAGETOOL" \
  "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
chmod +x "$APPIMAGETOOL"
"$APPIMAGETOOL" --appimage-extract-and-run "$APPDIR" "dist/fanyi-linux-x64.AppImage"
rm -rf "$APPDIR" "$APPIMAGETOOL"
echo "✓ dist/fanyi-linux-x64.AppImage"

echo "Linux 打包完成："
ls -lh dist/fanyi-linux-x64.deb dist/fanyi-linux-x64.AppImage
