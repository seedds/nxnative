#!/usr/bin/env bash
# Uploads an age-encrypted game bundle to private S3 storage, triggers a Bitrise
# build by pushing a one-use tag, then downloads the IPA back to the NSP folder.
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/build_ipa_linux.sh <NSP folder> --keys <prod.keys>

Required environment:
  NX_S3_BUCKET       private S3 bucket configured in Bitrise
  NX_AGE_RECIPIENT   age public recipient whose private identity is stored in Bitrise

Requirements on Linux: aws CLI, age, Python 3.12+, Git, an authenticated Git remote,
and AWS credentials with access to the nxnative/ bucket prefix.
EOF
}

if [ "$#" -lt 3 ]; then usage >&2; exit 2; fi
NSP_DIR=$1; shift
KEYS=
while [ "$#" -gt 0 ]; do
  case "$1" in
    --keys)
      [ "$#" -ge 2 ] || { usage >&2; exit 2; }
      KEYS=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

: "${NX_S3_BUCKET:?Set NX_S3_BUCKET to the private bucket configured in Bitrise}"
: "${NX_AGE_RECIPIENT:?Set NX_AGE_RECIPIENT to the Bitrise identity's age public recipient}"
[ -n "$KEYS" ] || { echo "--keys is required" >&2; exit 2; }
for tool in aws age python3 git; do command -v "$tool" >/dev/null || { echo "Missing required tool: $tool" >&2; exit 1; }; done
[ -d "$NSP_DIR" ] || { echo "NSP folder does not exist" >&2; exit 1; }
[ -f "$KEYS" ] || { echo "prod.keys file does not exist" >&2; exit 1; }

ROOT=$(cd "$(dirname "$0")/.." && pwd)
NSP_DIR=$(cd "$NSP_DIR" && pwd)
KEYS=$(cd "$(dirname "$KEYS")" && pwd)/$(basename "$KEYS")
git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1 || { echo "Run this from a checked-out NxNative repository" >&2; exit 1; }
git -C "$ROOT" cat-file -e HEAD:bitrise.yml 2>/dev/null || { echo "Commit and push the Bitrise workflow before building" >&2; exit 1; }
ORIGIN=$(git -C "$ROOT" remote get-url origin 2>/dev/null || true)
case "$ORIGIN" in
  https://github.com/seedds/nxnative|https://github.com/seedds/nxnative.git|git@github.com:seedds/nxnative|git@github.com:seedds/nxnative.git|ssh://git@github.com/seedds/nxnative|ssh://git@github.com/seedds/nxnative.git)
    echo "Set origin to your own NxNative repository before building; this script must not push tags to the upstream project" >&2
    exit 1 ;;
esac
if [ -n "$(git -C "$ROOT" status --porcelain)" ]; then
  echo "Commit the NxNative changes before starting a remote build" >&2
  exit 1
fi

TAG="nxnative-build-$(python3 -c 'import uuid; print(uuid.uuid4().hex)')"
INPUT_KEY="nxnative/incoming/${TAG#nxnative-build-}.tar.age"
OUTPUT_KEY="nxnative/outgoing/${TAG#nxnative-build-}.ipa"
STATUS_KEY="nxnative/status/${TAG#nxnative-build-}.txt"
INPUT_URI="s3://${NX_S3_BUCKET}/${INPUT_KEY}"
OUTPUT_URI="s3://${NX_S3_BUCKET}/${OUTPUT_KEY}"
STATUS_URI="s3://${NX_S3_BUCKET}/${STATUS_KEY}"
ARCHIVE_DIR=$(mktemp -d "$NSP_DIR/.nxnative-input.XXXXXX")
ARCHIVE="$ARCHIVE_DIR/input.tar.age"
STATUS_FILE=$(mktemp)
cleanup() { rm -rf "$ARCHIVE_DIR"; rm -f "$STATUS_FILE"; }
trap cleanup EXIT HUP INT TERM

python3 "$ROOT/scripts/cloud_bundle.py" "$NSP_DIR" "$KEYS" "$NX_AGE_RECIPIENT" "$ARCHIVE"
aws s3 cp "$ARCHIVE" "$INPUT_URI" --only-show-errors --sse AES256
git -C "$ROOT" tag "$TAG"
git -C "$ROOT" push origin "refs/tags/$TAG"

echo "Bitrise build started. Waiting for its status..."
MAX_WAIT=${NX_BUILD_TIMEOUT_SECONDS:-21600}
ELAPSED=0
while [ "$ELAPSED" -lt "$MAX_WAIT" ]; do
  if aws s3 cp "$STATUS_URI" "$STATUS_FILE" --only-show-errors 2>/dev/null; then
    RESULT=$(cat "$STATUS_FILE")
    case "$RESULT" in
      done)
        DEST="$NSP_DIR/nxnative-${TAG#nxnative-build-}.ipa"
        aws s3 cp "$OUTPUT_URI" "$DEST" --only-show-errors
        echo "IPA saved to: $DEST"
        exit 0 ;;
      failed)
        echo "Remote build failed. Open the Bitrise build for this tag to read its diagnostics: $TAG" >&2
        exit 1 ;;
    esac
  fi
  sleep 30
  ELAPSED=$((ELAPSED + 30))
done
echo "Timed out waiting for Bitrise after ${MAX_WAIT}s. The build may still be running; check Bitrise for tag $TAG." >&2
exit 1
