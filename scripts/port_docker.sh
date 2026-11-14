#!/bin/sh
# The port's Docker images and its verification on a machine short of disk (docker/README.md, "The images and the verification on a small disk").
# Builds nxnative-build (docker/Dockerfile.build) and nxnative-aot (docker/Dockerfile.aot) only when missing or when their Dockerfile changed, checks
# the free space on Docker's data root before each build, and runs the sequence the sessions ran by hand: nxaot and its CTest, the corpus packs, then
# the host with the packs and its CTest twice. Run from anywhere; `port_docker.sh help` prints the usage.
set -u

usage() {
    cat <<'EOF'
usage: scripts/port_docker.sh <command> [options]

commands:
  images [build|aot|all]   build the named images (default: all) when missing, or when their
                           Dockerfile changed since this script built them (label
                           org.nxnative.port-docker.dockerfile-sha256); checks the free space on
                           Docker's data root first and prunes the builder cache after each build
  status                   which images exist and are current, the free space, and what a full
                           verification needs
  verify [--low-disk] [--work <dir>]
                           in nxnative-aot: build dm/tools/aot, the corpus packs, copy nxaot out,
                           nxaot's CTest; in nxnative-build: the host with the packs
                           (GAME=010015100B514000, NXHOST_PROGRAM=ON), all nxhost-run nxlauncher,
                           CTest plain and with DM_AOT_ARENA_CHECK=1. Build trees on tmpfs; packs,
                           nxaot and the stage logs in the work folder.
                           --low-disk: never hold both images: the aot image is removed after its
                           stage when this run built it, before the build image is built
  clean [--work <dir>]     remove the images this script built (by label) and the work folder
  help                     this text

options:
  --work <dir>   work folder (default: ${TMPDIR:-/tmp}/nxnative-port-docker)
  --force        images: rebuild even when current, or when built without this script's label

environment:
  PORT_DOCKER_MARGIN_MB    free space kept above an image's size (default 1024)
  PORT_DOCKER_BUILD_MB     disk size assumed for nxnative-build (default 1900)
  PORT_DOCKER_AOT_MB       disk size assumed for nxnative-aot (default 2000)

exit status: 0 success, 1 a build or test failed, 2 usage error, 3 not enough free disk space
EOF
}

red() { echo "[RED] $*" >&2; }
die() { red "$*"; exit 1; }

here=$(cd "$(dirname "$0")" && pwd) || exit 2
repo=$(cd "$here/.." && pwd) || exit 2
uid=$(id -u) gid=$(id -g)

LABEL_SHA=org.nxnative.port-docker.dockerfile-sha256
LABEL_BY=org.nxnative.port-docker.built-by
BY=port_docker.sh
MARGIN_MB=${PORT_DOCKER_MARGIN_MB:-1024}
GAME_ID=010015100B514000
BUILT_THIS_RUN=

# image <key>: sets IMG (tag), DF (Dockerfile path), SIZE_MB
image() {
    case $1 in
        build) IMG=nxnative-build DF=$repo/docker/Dockerfile.build SIZE_MB=${PORT_DOCKER_BUILD_MB:-1900} ;;
        aot) IMG=nxnative-aot DF=$repo/docker/Dockerfile.aot SIZE_MB=${PORT_DOCKER_AOT_MB:-2000} ;;
        *) echo "unknown image: $1 (build, aot or all)" >&2; exit 2 ;;
    esac
}

dockerfile_sha() { sha256sum "$1" | cut -d' ' -f1; }

# label_value <labels as JSON> <key>: the label's value, empty when absent (the JSON is null for an image without labels)
label_value() { printf '%s\n' "$1" | tr ',{}' '\n\n\n' | sed -n "s/^ *\"$2\": *\"\\([^\"]*\\)\" *\$/\\1/p" | head -n 1; }

# image_state <key>: sets STATE to missing, current, stale (labelled, other Dockerfile) or unlabelled (built outside this script)
image_state() {
    image "$1"
    if ! labels=$(docker image inspect --format '{{json .Config.Labels}}' "$IMG" 2>/dev/null); then
        STATE=missing; return
    fi
    have_sha=$(label_value "$labels" "$LABEL_SHA")
    have_by=$(label_value "$labels" "$LABEL_BY")
    if [ "$have_by" != "$BY" ] || [ -z "$have_sha" ]; then STATE=unlabelled
    elif [ "$have_sha" = "$(dockerfile_sha "$DF")" ]; then STATE=current
    else STATE=stale; fi
}

data_root() {
    root=$(docker info --format '{{.DockerRootDir}}' 2>/dev/null) || root=
    [ -n "$root" ] || root=/var/lib/docker
    echo "$root"
}

# free_mb <dir>: free megabytes on the file system of <dir> (or of its nearest existing parent)
free_mb() {
    d=$1
    while [ ! -e "$d" ] && [ "$d" != / ]; do d=$(dirname "$d"); done
    kb=$(df -Pk "$d" 2>/dev/null | awk 'NR == 2 { print $4 }')
    case $kb in ''|*[!0-9]*) echo 0 ;; *) echo $((kb / 1024)) ;; esac
}

# what_to_remove: the images a user could remove to make room, ours first
what_to_remove() {
    ours=$(docker image ls --filter "label=$LABEL_BY=$BY" --format '{{.Repository}}:{{.Tag}} ({{.Size}})' 2>/dev/null)
    if [ -n "$ours" ]; then
        echo "  images this script built (scripts/port_docker.sh clean removes them):"
        echo "$ours" | sed 's/^/    /'
    fi
    others=$(docker image ls --format '{{.Repository}}:{{.Tag}} ({{.Size}})' 2>/dev/null | grep -v -F -x -e "$ours" 2>/dev/null)
    [ -n "$ours" ] || others=$(docker image ls --format '{{.Repository}}:{{.Tag}} ({{.Size}})' 2>/dev/null)
    if [ -n "$others" ]; then
        echo "  other images (not built by this script; remove by hand with docker image rm if you do not need them):"
        echo "$others" | sed 's/^/    /'
    fi
    echo "  the builder cache and unused data: docker system df shows them, docker builder prune -a removes the cache"
}

# ensure_image <key>: build when missing or stale (or unlabelled with --force); exit 3 when it does not fit
ensure_image() {
    image_state "$1"
    case $STATE in
        current) [ "$FORCE" = 1 ] || { echo "$IMG: current (Dockerfile unchanged since it was built)"; return 0; } ;;
        unlabelled)
            if [ "$FORCE" != 1 ]; then
                echo "$IMG: present, built outside this script (no $LABEL_BY label): used as it is; --force rebuilds it"
                return 0
            fi ;;
    esac
    image "$1"
    if [ "$STATE" = stale ] || { [ "$STATE" = current ] && [ "$FORCE" = 1 ]; }; then
        echo "$IMG: $(basename "$DF") changed since this script built it, or --force: removing the old image first"
        docker image rm "$IMG" >/dev/null || red "$IMG: could not remove the old image (a container uses it?); building beside it"
    fi
    root=$(data_root)
    need=$((SIZE_MB + MARGIN_MB))
    have=$(free_mb "$root")
    if [ "$have" -lt "$need" ]; then
        red "not enough disk space to build $IMG: Docker's data root $root has $have MB free, the build needs about $need MB ($SIZE_MB MB for the image + $MARGIN_MB MB margin)"
        echo "Make room by removing what you do not need:" >&2
        what_to_remove >&2
        exit 3
    fi
    echo "$IMG: building from $(basename "$DF") ($have MB free on $root, about $need MB needed)"
    sha=$(dockerfile_sha "$DF")
    t0=$(date +%s)
    docker build --label "$LABEL_SHA=$sha" --label "$LABEL_BY=$BY" -t "$IMG" -f "$DF" "$repo/docker"
    rc=$?
    echo "pruning the builder cache"
    # -a: with the containerd image store a build's layers stay in the builder cache as well (2 GB beside a 2 GB image); without -a the prune keeps them
    docker builder prune -af >/dev/null || red "docker builder prune failed"
    [ $rc -eq 0 ] || die "$IMG: docker build failed (exit $rc)"
    BUILT_THIS_RUN="$BUILT_THIS_RUN $1"
    echo "$IMG: built in $(($(date +%s) - t0)) s; $(free_mb "$root") MB free on $root"
}

built_this_run() { case " $BUILT_THIS_RUN " in *" $1 "*) return 0 ;; esac; return 1; }

cmd_images() {
    which=${1:-all}
    case $which in
        build|aot) ensure_image "$which" ;;
        all) ensure_image aot; ensure_image build ;;
        *) usage >&2; exit 2 ;;
    esac
}

cmd_status() {
    root=$(data_root)
    have=$(free_mb "$root")
    need_both=0 need_max=0
    for k in build aot; do
        image_state "$k"
        case $STATE in
            missing) note="missing" ;;
            current) note="current" ;;
            stale) note="stale: $(basename "$DF") changed since it was built (images $k rebuilds it)" ;;
            unlabelled) note="present, built outside this script: not compared with $(basename "$DF") (images $k --force rebuilds it)" ;;
        esac
        if [ "$STATE" != missing ]; then
            sz=$(docker image inspect --format '{{.Size}}' "$IMG" 2>/dev/null) || sz=0
            case $sz in ''|*[!0-9]*) sz=0 ;; esac
            note="$note, $((sz / 1048576)) MB"
        fi
        printf '%-15s %s\n' "$IMG" "$note"
        if [ "$STATE" = missing ] || [ "$STATE" = stale ]; then
            need_both=$((need_both + SIZE_MB))
            [ "$SIZE_MB" -gt "$need_max" ] && need_max=$SIZE_MB
        fi
    done
    echo "Docker data root: $root, $have MB free"
    wfree=$(free_mb "$WORK")
    echo "work folder: $WORK ($wfree MB free there)"
    if [ "$need_both" -eq 0 ]; then
        echo "verify: both images are present; the work folder needs about 400 MB (packs, nxaot, logs); the build trees go to tmpfs (RAM)"
    else
        echo "verify needs about $((need_both + MARGIN_MB)) MB free on the data root for the images it builds ($need_both MB + $MARGIN_MB MB margin)"
        echo "verify --low-disk needs about $((need_max + MARGIN_MB)) MB (one image at a time, the largest $need_max MB + $MARGIN_MB MB margin)"
        if [ "$have" -ge $((need_both + MARGIN_MB)) ]; then echo "fits: verify"
        elif [ "$have" -ge $((need_max + MARGIN_MB)) ]; then echo "fits: verify --low-disk only"
        else red "neither fits: free $(( need_max + MARGIN_MB - have )) MB more"; fi
    fi
}

# The stages run inside the containers; each step's output goes to /work/logs/<step>.log and a line to /work/results.tsv
write_inner() {
    cat > "$WORK/inner.sh" <<'EOF'
#!/bin/sh
# Written by scripts/port_docker.sh: one stage of `verify`, inside a container (repository at /src, work folder at /work, build trees on tmpfs)
set -u
res=/work/results.tsv
step() {
    name=$1; shift
    t0=$(date +%s)
    "$@" > "/work/logs/$name.log" 2>&1
    rc=$?
    printf '%s\t%s\t%s\n' "$name" "$rc" "$(($(date +%s) - t0))" >> "$res"
    return $rc
}
case $1 in
aot)
    if step aot-configure cmake -S dm/tools/aot -B build/aot -G Ninja -DCMAKE_BUILD_TYPE=Release &&
       step aot-build cmake --build build/aot; then
        step packs sh dm/tools/aot/tests/build_packs.sh build/aot /work/packs
        step nxaot-copy cp build/aot/nxaot /work/nxaot
        step aot-ctest ctest --test-dir build/aot --output-on-failure
    fi ;;
aot-static)
    step nxaot-static-configure cmake -S dm/tools/aot -B build/aot-static -G Ninja -DCMAKE_BUILD_TYPE=Release -DNXAOT_LLVM_STATIC=ON &&
    step nxaot-static-build cmake --build build/aot-static --target nxaot &&
    step nxaot-static-copy cp build/aot-static/nxaot /work/nxaot ;;
host)
    if [ -x /work/nxaot ]; then
        export NXAOT=/work/nxaot
        step host-nxaot-libs sh -c 'ldd /work/nxaot; ! ldd /work/nxaot | grep -q "not found"'
    fi
    if step host-configure cmake -S dm/host -B build/linux -G Ninja -DGAME=$2 -DNXAOT_PACKS=/work/packs -DNXHOST_PROGRAM=ON &&
       step host-build cmake --build build/linux --target all nxhost-run nxlauncher; then
        step host-ctest ctest --test-dir build/linux --output-on-failure
        step host-ctest-arena env DM_AOT_ARENA_CHECK=1 ctest --test-dir build/linux --output-on-failure
    fi ;;
esac
exit 0
EOF
}

run_stage() { # run_stage <image> <mode>
    mkdir -p "$repo/build" "$WORK/packs" "$WORK/logs" || die "cannot create $repo/build or $WORK"
    echo "== stage $2 in $1"
    docker run --rm -u "$uid:$gid" -e HOME=/tmp -v "$repo":/src -w /src \
        --tmpfs "/src/build:exec,uid=$uid,gid=$gid" -v "$WORK":/work "$1" sh /work/inner.sh "$2" "$GAME_ID" ||
        red "stage $2: docker run failed"
}

# build_has_libllvm: whether nxnative-build provides libLLVM.so.18.1 for the shared nxaot; sets EVIDENCE
build_has_libllvm() {
    image_state build
    if [ "$STATE" != missing ]; then
        if docker run --rm nxnative-build sh -c 'ldconfig -p | grep libLLVM.so.18.1' >/dev/null 2>&1; then
            EVIDENCE="nxnative-build's ldconfig lists libLLVM.so.18.1"; return 0
        fi
        EVIDENCE="nxnative-build's ldconfig has no libLLVM.so.18.1"; return 1
    fi
    # Ubuntu's libllvm18 ships libLLVM.so.18.1; llvm-18, lld-18 and clang-18 depend on it
    pkgs=$(grep -v '^ *#' "$repo/docker/Dockerfile.build" | grep -o -w -E 'libllvm18|llvm-18|lld-18|clang-18' | sort -u | tr '\n' ' ')
    if [ -n "$pkgs" ]; then
        EVIDENCE="nxnative-build is not built yet; Dockerfile.build installs ${pkgs% }, which depend on libllvm18 (libLLVM.so.18.1)"; return 0
    fi
    EVIDENCE="nxnative-build is not built yet and Dockerfile.build installs none of libllvm18, llvm-18, lld-18, clang-18"; return 1
}

report() {
    echo
    echo "== results (logs in $WORK/logs)"
    pass=0 fail=0 notrun=0
    for s in $EXPECTED; do
        line=$(grep "^$s	" "$WORK/results.tsv" 2>/dev/null | tail -n 1)
        if [ -z "$line" ]; then echo "NOT RUN $s"; notrun=$((notrun + 1)); continue; fi
        rc=$(echo "$line" | cut -f2) secs=$(echo "$line" | cut -f3)
        if [ "$rc" = 0 ]; then echo "PASS $s ($secs s)"; pass=$((pass + 1))
        else red "FAIL $s (exit $rc, $secs s; see $WORK/logs/$s.log)"; fail=$((fail + 1)); fi
        case $s in *ctest*)
            grep -E 'tests passed|tests failed out of|Total Test time' "$WORK/logs/$s.log" 2>/dev/null | sed 's/^/    /'
            sed -n '/The following tests FAILED/,$p' "$WORK/logs/$s.log" 2>/dev/null | sed 's/^/    /' ;;
        esac
    done
    echo "total: $pass passed, $fail failed, $notrun not run"
    [ "$fail" -eq 0 ] && [ "$notrun" -eq 0 ]
}

cmd_verify() {
    command -v docker >/dev/null || die "docker is not installed"
    mkdir -p "$WORK/packs" "$WORK/logs" && : > "$WORK/.port_docker" || die "cannot create the work folder $WORK"
    rm -f "$WORK/results.tsv" "$WORK/nxaot" "$WORK"/logs/*.log
    wfree=$(free_mb "$WORK")
    [ "$wfree" -ge 400 ] || { red "the work folder $WORK has $wfree MB free; it needs about 400 MB (packs, nxaot, logs): choose another with --work"; exit 3; }
    write_inner
    EXPECTED="aot-configure aot-build packs nxaot-copy aot-ctest"
    if [ "$LOW_DISK" = 1 ]; then ensure_image aot; else ensure_image aot; ensure_image build; fi
    run_stage nxnative-aot aot
    if build_has_libllvm; then
        echo "nxaot for the host stage: shared ($EVIDENCE)"
    else
        echo "nxaot for the host stage: static, -DNXAOT_LLVM_STATIC=ON ($EVIDENCE)"
        EXPECTED="$EXPECTED nxaot-static-configure nxaot-static-build nxaot-static-copy"
        run_stage nxnative-aot aot-static
    fi
    if [ "$LOW_DISK" = 1 ]; then
        if built_this_run aot; then
            echo "--low-disk: removing nxnative-aot (built by this run)"
            docker image rm nxnative-aot >/dev/null || red "could not remove nxnative-aot"
            docker builder prune -af >/dev/null || red "docker builder prune failed"   # the removed image's layers are still in the cache (the first real run stopped here)
        else
            echo "--low-disk: nxnative-aot was not built by this run: kept"
        fi
        ensure_image build
    fi
    EXPECTED="$EXPECTED host-nxaot-libs host-configure host-build host-ctest host-ctest-arena"
    run_stage nxnative-build host
    report || exit 1
}

cmd_clean() {
    ids=$(docker image ls --filter "label=$LABEL_BY=$BY" --format '{{.Repository}}:{{.Tag}}' 2>/dev/null)
    for i in $ids; do
        case $i in *'<none>'*) continue ;; esac
        echo "removing image $i"
        docker image rm "$i" >/dev/null || red "could not remove $i"
    done
    dangling=$(docker image ls -q --filter dangling=true --filter "label=$LABEL_BY=$BY" 2>/dev/null)
    for i in $dangling; do echo "removing untagged image $i"; docker image rm "$i" >/dev/null || red "could not remove $i"; done
    if [ -d "$WORK" ]; then
        if [ -e "$WORK/.port_docker" ]; then echo "removing $WORK"; rm -rf "$WORK"
        else red "$WORK was not made by this script (no .port_docker in it): left alone"; fi
    fi
}

[ $# -ge 1 ] || { usage >&2; exit 2; }
cmd=$1; shift
WORK=${TMPDIR:-/tmp}/nxnative-port-docker LOW_DISK=0 FORCE=0 ARG=
while [ $# -gt 0 ]; do
    case $1 in
        --work) [ $# -ge 2 ] || { usage >&2; exit 2; }; WORK=$2; shift ;;
        --low-disk) LOW_DISK=1 ;;
        --force) FORCE=1 ;;
        -*) usage >&2; exit 2 ;;
        *) [ -z "$ARG" ] || { usage >&2; exit 2; }; ARG=$1 ;;
    esac
    shift
done
case $WORK in /*) ;; *) WORK=$(pwd)/$WORK ;; esac
case $cmd in
    images) cmd_images "$ARG" ;;
    status) [ -z "$ARG" ] || { usage >&2; exit 2; }; cmd_status ;;
    verify) [ -z "$ARG" ] || { usage >&2; exit 2; }; cmd_verify ;;
    clean) [ -z "$ARG" ] || { usage >&2; exit 2; }; cmd_clean ;;
    help|-h|--help) usage ;;
    *) usage >&2; exit 2 ;;
esac
