#!/bin/bash
# TSR PHASE · DEPTH · MOTION (TSR Audio) · build + test + installer per macOS
# Uso:  ./scripts/build_mac.sh          (sul Mac o su GitHub Actions)
# Risultato in dist/: installer .pkg, zip dei plugin, log di test, auval, pluginval.
# Facoltativo (distribuzione senza avvisi di Gatekeeper):
#   DEV_ID_APP="Developer ID Application: ..."  DEV_ID_INSTALLER="Developer ID Installer: ..."
set -euo pipefail
cd "$(dirname "$0")/.."
NAME="TSR PHASE DEPTH MOTION"
VER=$(sed -n 's/^project(TSR_PHASE_DEPTH_MOTION VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$VER" ] || { echo "ERRORE: versione non trovata in CMakeLists.txt"; exit 1; }
OUT="dist"; mkdir -p "$OUT"
command -v cmake >/dev/null || { echo "ERRORE: serve CMake"; exit 1; }
xcode-select -p >/dev/null || { echo "ERRORE: servono gli Xcode Command Line Tools (xcode-select --install)"; exit 1; }
[ -d JUCE/modules ] || git clone -q --depth 1 --branch 8.0.15 https://github.com/juce-framework/JUCE.git JUCE

echo "== 1/7 configurazione $NAME $VER (universale arm64 + x86_64, macOS 11+) =="
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0

echo "== 2/7 build AU, VST3, Standalone, test =="
cmake --build build-mac --config Release -j"$(sysctl -n hw.ncpu)" \
  --target TSRPhaseDepthMotion_AU TSRPhaseDepthMotion_VST3 TSRPhaseDepthMotion_Standalone EngineTests
A="build-mac/TSRPhaseDepthMotion_artefacts/Release"
AU="$A/AU/$NAME.component"; VST="$A/VST3/$NAME.vst3"; APP="$A/Standalone/$NAME.app"
for b in "$AU" "$VST" "$APP"; do [ -d "$b" ] || { echo "ERRORE: manca $b"; exit 1; }; done
lipo -info "$AU/Contents/MacOS/$NAME" | tee "$OUT/architetture.txt"
grep -q arm64 "$OUT/architetture.txt" && grep -q x86_64 "$OUT/architetture.txt" || { echo "ERRORE: binario non universale"; exit 1; }

echo "== 3/7 test del motore DSP =="
"build-mac/EngineTests" | tee "$OUT/test_dsp.txt"
grep -q "RESULT: [0-9]* passed, 0 failed" "$OUT/test_dsp.txt" || { echo "ERRORE: test DSP falliti"; exit 1; }

echo "== 4/7 produttore e firma =="
VENDOR=$(/usr/libexec/PlistBuddy -c 'Print :AudioComponents:0:name' "$AU/Contents/Info.plist")
echo "AU name: $VENDOR" | tee "$OUT/produttore.txt"
case "$VENDOR" in "TSR Audio: "*) ;; *) echo "ERRORE: produttore AU non 'TSR Audio'"; exit 1;; esac
SIGN="${DEV_ID_APP:--}"
for b in "$AU" "$VST" "$APP"; do
  codesign --force --deep --options runtime --timestamp=none -s "$SIGN" "$b"
  codesign --verify --deep --strict "$b"
done

echo "== 5/7 auval (formato Apple) =="
mkdir -p ~/Library/Audio/Plug-Ins/Components ~/Library/Audio/Plug-Ins/VST3
rm -rf ~/Library/Audio/Plug-Ins/Components/"$NAME.component"
cp -R "$AU" ~/Library/Audio/Plug-Ins/Components/
killall -9 AudioComponentRegistrar 2>/dev/null || true; sleep 3
auval -v aufx Tpdm Tsra | tee "$OUT/auval.txt" | tail -3
grep -q "AU VALIDATION SUCCEEDED" "$OUT/auval.txt" || { echo "ERRORE: auval non superato"; exit 1; }

echo "== 6/7 pluginval strictness 10 (VST3 e AU) =="
PV=$(command -v pluginval || echo /Applications/pluginval.app/Contents/MacOS/pluginval)
if [ -x "$PV" ]; then
  "$PV" --strictness-level 10 --validate-in-process --output-dir "$OUT" "$VST" | tee "$OUT/pluginval_vst3.txt" | tail -2
  grep -q "SUCCESS" "$OUT/pluginval_vst3.txt" || { echo "ERRORE: pluginval VST3"; exit 1; }
  "$PV" --strictness-level 10 --validate-in-process --output-dir "$OUT" "$AU" | tee "$OUT/pluginval_au.txt" | tail -2
  grep -q "SUCCESS" "$OUT/pluginval_au.txt" || { echo "ERRORE: pluginval AU"; exit 1; }
else echo "(pluginval non trovato: https://github.com/Tracktion/pluginval/releases)"; fi

echo "== 7/7 installer .pkg + zip =="
STG=$(mktemp -d); mkdir -p "$STG/au" "$STG/vst3/TSR Audio" "$STG/app/TSR Audio" "$STG/scripts"
cp -R "$AU" "$STG/au/"; cp -R "$VST" "$STG/vst3/TSR Audio/"; cp -R "$APP" "$STG/app/TSR Audio/"
cp installer/postinstall "$STG/scripts/postinstall"; chmod +x "$STG/scripts/postinstall"
pkgbuild --root "$STG/au"   --identifier com.tsraudio.tsrphasedepthmotion.au   --version "$VER" --install-location "/Library/Audio/Plug-Ins/Components" --scripts "$STG/scripts" "$STG/au.pkg"
pkgbuild --root "$STG/vst3" --identifier com.tsraudio.tsrphasedepthmotion.vst3 --version "$VER" --install-location "/Library/Audio/Plug-Ins/VST3" "$STG/vst3.pkg"
pkgbuild --root "$STG/app"  --identifier com.tsraudio.tsrphasedepthmotion.app  --version "$VER" --install-location "/Applications" "$STG/app.pkg"
PKG="$OUT/$NAME $VER (TSR Audio).pkg"
productbuild --package "$STG/au.pkg" --package "$STG/vst3.pkg" --package "$STG/app.pkg" ${DEV_ID_INSTALLER:+--sign "$DEV_ID_INSTALLER"} "$PKG"
EXP=$(mktemp -d)/x; pkgutil --expand "$PKG" "$EXP"
: > "$OUT/contenuto_pkg.txt"
for p in au vst3 app; do
  loc=$(sed -n 's/.*install-location="\([^"]*\)".*/\1/p' "$EXP/$p.pkg/PackageInfo")
  (cd "$EXP" && gzip -dc "$p.pkg/Payload" | cpio -t 2>/dev/null) | grep -E "\.(component|vst3|app)$" | sed "s#^\.#$loc#" | tee -a "$OUT/contenuto_pkg.txt"
done
[ "$(wc -l < "$OUT/contenuto_pkg.txt")" -eq 3 ] || { echo "ERRORE: l'installer non contiene AU, VST3 e app"; exit 1; }
(cd "$A" && ditto -c -k --keepParent "AU/$NAME.component" "$OLDPWD/$OUT/$NAME $VER AU.zip" && ditto -c -k --keepParent "VST3/$NAME.vst3" "$OLDPWD/$OUT/$NAME $VER VST3.zip")
echo "OK: $PKG"
