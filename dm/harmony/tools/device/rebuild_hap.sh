#!/bin/sh
# 用 hvigor 重新编译原生库，然后在 Docker 里打包+签名（宿主机 JVM 在内存压力下起不来）。
# Rebuild native libs with hvigor, then package + sign inside Docker.
#
# Usage: rebuild_hap.sh <output.hap>
# Required env:
#   DM_WORK          仓外 App 工程目录（prepare_app.py 的输出，含 app/） | out-of-tree app project (prepare_app.py output)
#   OHOS_SDK         官方 OHOS SDK 根目录（含 26.0.0/ 与 build-tools） | OHOS SDK root
#   HAP_KEYSTORE     密钥库（.p12）路径 | keystore path
#   HAP_CERT         调试证书（.cer）路径 | debug certificate path
#   HAP_KEY_ALIAS    密钥别名 | key alias
#   HAP_KEY_PWD      密钥口令 | key password
#   HAP_STORE_PWD    密钥库口令 | keystore password
#   HAP_PROFILE      调试 profile（.p7b）路径 | debug profile path
# Optional:
#   CLT_BIN          command-line-tools bin（含 hvigorw 需要的环境） | CLT bin dir
#   DOCKER_IMAGE     默认 eclipse-temurin:17-jdk
set -e
OUT=${1:?output hap}
W=${DM_WORK:?set DM_WORK to the prepared app workspace}
SDK=${OHOS_SDK:?set OHOS_SDK to the OHOS SDK root}
K=${HAP_KEYSTORE:?set HAP_KEYSTORE}
C=${HAP_CERT:?set HAP_CERT}
T=$SDK/26.0.0/toolchains/lib
APP=$W/app
I=$APP/entry/build/default/intermediates
O=$APP/entry/build/default/outputs/default
export DEVECO_SDK_HOME=$SDK OHOS_BASE_SDK_HOME=$SDK
[ -n "$CLT_BIN" ] && export PATH="$CLT_BIN:$PATH"
cd "$APP"
hvigorw assembleHap --mode module -p product=default -p buildMode=debug --no-daemon 2>&1 \
  | grep -E "BuildNativeWithNinja|DoNativeStrip|error:" || true
rm -f "$O/entry-default-unsigned.hap"
mkdir -p "$(dirname "$OUT")"
docker run --rm -v "$W:$W" -v "$SDK:$SDK" -v "$K:$K" -v "$C:$C" -v "$HAP_PROFILE:$HAP_PROFILE" -v "$(cd "$(dirname "$OUT")" && pwd):$(cd "$(dirname "$OUT")" && pwd)" "${DOCKER_IMAGE:-eclipse-temurin:17-jdk}" sh -c "
java -Dfile.encoding=utf-8 -jar $T/app_packing_tool.jar --mode hap --force true \
  --lib-path $I/stripped_native_libs/default --json-path $I/package/default/module.json \
  --resources-path $I/res/default/resources --index-path $I/res/default/resources.index \
  --pack-info-path $O/pack.info --out-path $O/entry-default-unsigned.hap \
  --rpcid-path $I/syscap/default/rpcid.sc --ets-path $I/loader_out/default/ets \
  --pkg-context-path $I/loader/default/pkgContextInfo.json >/dev/null &&
java -jar $T/hap-sign-tool.jar sign-app -mode localSign -keyAlias $HAP_KEY_ALIAS -keyPwd $HAP_KEY_PWD \
  -keystoreFile $K -keystorePwd $HAP_STORE_PWD -appCertFile $C \
  -profileFile $HAP_PROFILE -signAlg SHA256withECDSA \
  -inFile $O/entry-default-unsigned.hap -outFile $OUT -signCode 1" 2>&1 | tail -1
# 包内运行库 SHA 必须与构建产物一致 | packaged libs must match the build output
for l in libxblhost.so libxblr_execution.so; do
  a=$(unzip -p "$OUT" libs/arm64-v8a/$l | shasum -a 256 | cut -c1-12)
  b=$(shasum -a 256 "$I/stripped_native_libs/default/arm64-v8a/$l" | cut -c1-12)
  [ "$a" = "$b" ] || { echo "MISMATCH $l"; exit 1; }
  echo "$l $a"
done
