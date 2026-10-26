#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GUI_DIR="$REPO_ROOT/gui"
WORKSPACE_DIR="$(cd "$REPO_ROOT/.." && pwd)"

echo "==> Compilando NXNative GUI em modo release..."
cd "$GUI_DIR"
swift build -c release

RELEASE_BIN="$GUI_DIR/.build/release/NXNativeGUI"
ICON_ICNS="$GUI_DIR/Resources/AppIcon.icns"
ICON_PNG="$GUI_DIR/Resources/AppIcon.png"

build_app_bundle() {
    local DEST_DIR="$1"
    local APP_BUNDLE="$DEST_DIR/NxNative_Launcher.app"
    
    # Remove old name if exists
    rm -rf "$DEST_DIR/NXNative.app"
    
    echo "==> Criando bundle em: $APP_BUNDLE"
    rm -rf "$APP_BUNDLE"
    mkdir -p "$APP_BUNDLE/Contents/MacOS"
    mkdir -p "$APP_BUNDLE/Contents/Resources"
    
    cp "$RELEASE_BIN" "$APP_BUNDLE/Contents/MacOS/NXNativeGUI"
    chmod +x "$APP_BUNDLE/Contents/MacOS/NXNativeGUI"
    
    if [ -d "$GUI_DIR/Resources" ]; then
        cp "$GUI_DIR/Resources/"*.icns "$APP_BUNDLE/Contents/Resources/" 2>/dev/null || true
        cp "$GUI_DIR/Resources/"*.png "$APP_BUNDLE/Contents/Resources/" 2>/dev/null || true
        if [ -d "$GUI_DIR/Resources/NxNative_Launcher.icon" ]; then
            cp -R "$GUI_DIR/Resources/NxNative_Launcher.icon" "$APP_BUNDLE/Contents/Resources/"
        fi
        if [ -d "$GUI_DIR/Resources/patches" ]; then
            mkdir -p "$APP_BUNDLE/Contents/Resources/patches"
            cp -R "$GUI_DIR/Resources/patches/" "$APP_BUNDLE/Contents/Resources/patches/"
        fi
    fi
    
    cat <<EOF > "$APP_BUNDLE/Contents/Info.plist"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleDisplayName</key>
    <string>NxNative_Launcher</string>
    <key>CFBundleExecutable</key>
    <string>NXNativeGUI</string>
    <key>CFBundleIconFile</key>
    <string>AppIcon</string>
    <key>CFBundleIdentifier</key>
    <string>org.nxnative.launcher</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>NxNative_Launcher</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.2.0</string>
    <key>CFBundleVersion</key>
    <string>3</string>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSPrincipalClass</key>
    <string>NSApplication</string>
</dict>
</plist>
EOF

    echo "==> Assinando bundle ad-hoc..."
    codesign --force --deep --sign - "$APP_BUNDLE"
}

# Cria o aplicativo na raiz de NativePort e dentro de nxnative
build_app_bundle "$WORKSPACE_DIR"
build_app_bundle "$REPO_ROOT"

# Limpa artefatos intermediários pesados do build para economizar espaço
rm -rf "$GUI_DIR/.build" 2>/dev/null || true

echo "==> Sucesso! Aplicativo criado com ícone em:"
echo "    $WORKSPACE_DIR/NxNative_Launcher.app"
echo "    $REPO_ROOT/NxNative_Launcher.app"
