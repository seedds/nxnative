#!/usr/bin/env python3
"""Build (and, with --run, run) an nxnative graphics regression on an iPhone.

The selected fixture in dm/gfx/tests/ is compiled unchanged: only its `main` is
renamed by a wrapper translation unit, and the app calls it on a background
thread. The app writes the test's stdout/stderr and a machine-readable report to
its Documents folder. No game files or keys are read; the profile header and the
host's Metal shaders are generated from source into the output directory.

Modes (exactly one is required, so nothing is installed by accident):
  --generate-only  write the project, headers and receipt, run xcodegen
  --build-only     generate, then build a signed app (optional build slot wrapper)
  --run            build, then install, launch and collect results on the device
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid


TARGET = "GfxClearVerification"
DEFAULT_TREE = Path(__file__).resolve().parents[3]
FRAMEWORKS = ("UIKit", "Foundation", "Metal", "MetalFX", "QuartzCore",
              "ImageIO", "CoreGraphics", "UniformTypeIdentifiers")


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(command, output, name, env=None):
    print(f"{name}...", flush=True)
    log = output / f"{name}.log"
    with log.open("w") as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, env=env)
    if result.returncode:
        print(log.read_text(errors="replace")[-16000:], file=sys.stderr)
        raise RuntimeError(f"{name} failed ({result.returncode}); see {log}")


def git(tree, *arguments):
    return subprocess.check_output(["git", "-C", str(tree), *arguments], text=True)


def support_sources(tree):
    """MTL_T_SRC of dm/gfx/tests/Makefile, minus nvn_mtl.m (the test includes it)."""
    gfx = tree / "dm/gfx"
    makefile = (gfx / "tests/Makefile").read_text()
    if "MTL_T_SRC=../nvn/hle_nvn.c ../../host/crash.c ../../host/hle_bigstack.c t_host_stubs.c " \
       "$(filter-out ../nvn/nvn_mtl.m,$(GFX))" not in makefile:
        raise RuntimeError("tests/Makefile MTL_T_SRC changed; update support_sources()")
    if "GFX=$(filter-out ../nvn/hle_nvn.c,$(sort $(wildcard ../nvn/*.c ../nvn/*.m ../nvn/*.S " \
       "../tex/*.c ../tex/*.m)))" not in makefile:
        raise RuntimeError("tests/Makefile GFX changed; update support_sources()")
    found = sorted(path.resolve() for pattern in ("nvn/*.c", "nvn/*.m", "nvn/*.S", "tex/*.c", "tex/*.m")
                   for path in gfx.glob(pattern))
    found = [path for path in found if path.name not in ("hle_nvn.c", "nvn_mtl.m")]
    return [gfx / "nvn/hle_nvn.c", tree / "dm/host/crash.c", tree / "dm/host/hle_bigstack.c",
            gfx / "tests/t_host_stubs.c", *found]


def tree_state(tree):
    """Revision plus a hash of every uncommitted change under dm/ (tracked diff and untracked files)."""
    digest = hashlib.sha256()
    digest.update(git(tree, "diff", "HEAD", "--", "dm").encode())
    for name in git(tree, "ls-files", "--others", "--exclude-standard", "-z", "--", "dm").split("\0"):
        if name:
            digest.update(name.encode() + b"\0" + (tree / name).read_bytes())
    return {
        "revision": git(tree, "rev-parse", "HEAD").strip(),
        "dirtyFiles": [line for line in git(tree, "status", "--short", "--", "dm").splitlines()],
        "dirtyDmContentSHA256": digest.hexdigest(),
    }


def generate(args, output, slot):
    tree = args.tree.resolve(strict=True)
    gfx = tree / "dm/gfx"
    test = gfx / f"tests/t_{args.fixture}.m"
    for needed in (test, gfx / "tests/Makefile", tree / "dm/games/game.py",
                   gfx / "tools/host_msl.py", tree / "dm/host/Makefile"):
        if not needed.is_file():
            raise RuntimeError(f"Missing source: {needed}")
    support = support_sources(tree)
    for source in support:
        if not source.is_file():
            raise RuntimeError(f"Missing support source: {source}")
    state_before = tree_state(tree)
    output.mkdir(parents=True, exist_ok=False)
    include = output / "Include"
    include.mkdir()
    run([sys.executable, str(tree / "dm/games/game.py"), "header", args.profile,
         str(include / "game_profile.h")], output, "profile")
    # The deployment minimum production compiles the host's shaders for (MSL_MIN_iphoneos).
    minimum = re.search(r"^MSL_MIN_iphoneos\s*=\s*(\S+)", (tree / "dm/host/Makefile").read_text(), re.M)
    if not minimum:
        raise RuntimeError("dm/host/Makefile has no MSL_MIN_iphoneos")
    metal_sources = sorted(str(path) for path in (gfx / "msl").glob("*.metal"))
    run([sys.executable, str(gfx / "tools/host_msl.py"), "--sdk", "iphoneos", "--min", minimum.group(1),
         str(include / "host_msl_gen.h"), *metal_sources], output, "host_msl")
    run_id = str(uuid.uuid4())
    (output / "fixture_entry.m").write_text(
        "// The fixture, unchanged: only its main is renamed. Its counters are file-static.\n"
        "#define main gfx_fixture_main\n"
        f"#include {json.dumps(str(test))}\n"
        "#undef main\n"
        "int gfx_fixture_checks(void) { return checks; }\n"
        "int gfx_fixture_failures(void) { return failed; }\n")
    (output / "main.m").write_text(APP_SOURCE.replace("RUN_ID", run_id).replace("FIXTURE_NAME", args.fixture))
    sources = [output / "main.m", output / "fixture_entry.m", *support]
    source_yaml = "\n".join(f"      - path: {json.dumps(str(path))}" for path in sources)
    dependency_yaml = "\n".join(f"      - sdk: {name}.framework" for name in FRAMEWORKS)
    (output / "project.yml").write_text(f"""name: {TARGET}
options:
  deploymentTarget:
    iOS: "{args.ios}"
targets:
  {TARGET}:
    type: application
    platform: iOS
    sources:
{source_yaml}
    dependencies:
{dependency_yaml}
    settings:
      base:
        PRODUCT_BUNDLE_IDENTIFIER: {json.dumps(args.bundle_id)}
        DEVELOPMENT_TEAM: {json.dumps(args.team)}
        CODE_SIGN_STYLE: Automatic
        TARGETED_DEVICE_FAMILY: "1"
        CLANG_ENABLE_OBJC_ARC: YES
        GCC_C_LANGUAGE_STANDARD: gnu11
        GCC_OPTIMIZATION_LEVEL: "1"
        DEAD_CODE_STRIPPING: NO
        ENABLE_DEBUG_DYLIB: NO
        HEADER_SEARCH_PATHS:
          - {json.dumps(str(include))}
          - {json.dumps(str(gfx / 'nvn/include'))}
        OTHER_CFLAGS:
          - -Wall
          - -Wno-unused-function
          - -Wno-deprecated-declarations
    info:
      path: Info.plist
      properties:
        CFBundleDisplayName: GFX Verification
        CFBundleShortVersionString: "1.0"
        CFBundleVersion: "1"
        UILaunchScreen: {{}}
        UISupportedInterfaceOrientations:
          - UIInterfaceOrientationPortrait
        UIApplicationSceneManifest:
          UIApplicationSupportsMultipleScenes: false
          UISceneConfigurations:
            UIWindowSceneSessionRoleApplication:
              - UISceneConfigurationName: Default Configuration
                UISceneDelegateClassName: GfxSceneDelegate
""")
    receipt = {
        "runID": run_id,
        "bundleID": args.bundle_id,
        "team": args.team,
        "iosDeploymentTarget": args.ios,
        "hostMslMinimum": minimum.group(1),
        "profile": args.profile,
        "test": args.fixture,
        "tree": str(tree),
        "treeBefore": state_before,
        "fixture": {"path": str(test), "sha256": sha256(test),
                    "modifiedFromHEAD": bool(git(tree, "status", "--short", "--", str(test.relative_to(tree))).strip())},
        "mtlTSrcFromMakefile": [str(path.relative_to(tree)) for path in support],
        "sourceSHA256": {str(path.relative_to(tree)): sha256(path) for path in support},
        "nvnMtlSHA256": sha256(gfx / "nvn/nvn_mtl.m"),
        "generated": {name: sha256(output / name) for name in
                      ("Include/game_profile.h", "Include/host_msl_gen.h", "fixture_entry.m", "main.m", "project.yml")},
        "slotWrapper": slot,
    }
    (output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    run(["xcodegen", "generate", "--spec", str(output / "project.yml")], output, "generate")
    return run_id, tree, receipt


def slot_wrapper(args):
    runner = args.slot_run
    if not runner:
        return None
    if not Path(runner).is_file():
        raise RuntimeError(f"Slot wrapper not found: {runner}")
    return {"script": str(Path(runner).resolve()), "dir": args.slot_dir, "name": args.slot_name}


def build(args, output, tree, receipt, slot):
    environment = dict(os.environ)
    project = output / f"{TARGET}.xcodeproj"
    derived = output / "DerivedData"
    command = ["xcodebuild", "-project", str(project),
               "-scheme", TARGET, "-configuration", "Debug", "-destination", "generic/platform=iOS",
               "-derivedDataPath", str(derived), "-allowProvisioningUpdates", "build"]
    if slot:
        environment["SLOT_RUN_DIR"] = slot["dir"]
        command = ["/bin/bash", slot["script"], slot["name"], *command]
    print("build...", flush=True)
    log = output / "build.log"
    started = time.time()
    with log.open("w") as stream:
        exit_code = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, env=environment).returncode
    receipt["build"] = {"exit": exit_code, "seconds": round(time.time() - started, 1), "log": str(log),
                        "derivedData": str(derived), "configuration": "Debug"}
    app = derived / f"Build/Products/Debug-iphoneos/{TARGET}.app"
    if exit_code == 0 and app.is_dir():
        executable = app / TARGET
        receipt["build"]["app"] = str(app)
        receipt["build"]["executableSHA256"] = sha256(executable)
        plist = subprocess.run(["/usr/libexec/PlistBuddy", "-c", "Print :CFBundleIdentifier", str(app / "Info.plist")],
                               capture_output=True, text=True)
        receipt["build"]["bundleID"] = plist.stdout.strip()
        sign = subprocess.run(["codesign", "-dvv", str(app)], capture_output=True, text=True).stderr
        receipt["build"]["codesign"] = [line for line in sign.splitlines()
                                        if line.startswith(("Identifier=", "TeamIdentifier=", "Authority=", "Signature"))]
        verify = subprocess.run(["codesign", "--verify", "--strict", "--deep", str(app)], capture_output=True, text=True)
        receipt["build"]["codesignVerifyExit"] = verify.returncode
        # Every file the compiler actually read from the tree, hashed after the build.
        dependencies = set()
        for dep in derived.glob("Build/Intermediates.noindex/**/*.d"):
            for token in re.split(r"(?<!\\)\s+", dep.read_text(errors="replace").replace("\\\n", " ")):
                token = token.replace("\\ ", " ")
                if token.startswith(str(tree)) and Path(token).is_file():
                    dependencies.add(token)
        receipt["build"]["treeFilesRead"] = {str(Path(path).relative_to(tree)): sha256(path)
                                             for path in sorted(dependencies)}
    receipt["treeAfterBuild"] = tree_state(tree)
    receipt["treeStableDuringBuild"] = (receipt["treeAfterBuild"]["revision"] == receipt["treeBefore"]["revision"] and
                                        receipt["treeAfterBuild"]["dirtyDmContentSHA256"] ==
                                        receipt["treeBefore"]["dirtyDmContentSHA256"])
    (output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    if exit_code:
        print(log.read_text(errors="replace")[-16000:], file=sys.stderr)
        raise RuntimeError(f"build failed ({exit_code}); see {log}")
    if not app.is_dir():
        raise RuntimeError(f"build exited 0 but {app} is missing")
    return app


def device_run(args, output, run_id, app):
    """Install, launch, then poll Documents for this build's completed report."""
    run(["xcrun", "devicectl", "--timeout", str(args.timeout), "device", "install", "app", "--device", args.device, str(app)], output, "install")
    run(["xcrun", "devicectl", "--timeout", str(args.timeout), "device", "process", "launch", "--device", args.device,
         "--terminate-existing", args.bundle_id], output, "launch")

    def pull(name, destination, timeout):
        result = subprocess.run(
            ["xcrun", "devicectl", "--timeout", str(timeout), "device", "copy", "from", "--device", args.device,
             "--domain-type", "appDataContainer", "--domain-identifier", args.bundle_id,
             "--source", f"Documents/{name}", "--destination", str(destination)],
            capture_output=True, text=True)
        with (output / "device-copy.log").open("a") as stream:
            stream.write(f"{name}: exit {result.returncode}\n{result.stdout}{result.stderr}\n")
        return result.returncode == 0

    report_path = output / "results.json"
    deadline = time.monotonic() + args.timeout
    report = None
    while time.monotonic() < deadline:
        report_path.unlink(missing_ok=True)
        if pull("results.json", report_path, max(1, min(15, deadline - time.monotonic()))):
            candidate = json.loads(report_path.read_text())
            if candidate.get("runID") == run_id and candidate.get("complete"):
                report = candidate
                break
        time.sleep(1)
    pull("gfx-fixture.log", output / "gfx-fixture.log", 15)
    if report is None:
        raise RuntimeError(f"No completed report for this run within {args.timeout:g}s; "
                           f"see {output / 'device-copy.log'} and check the device for a fixture crash")
    print(json.dumps({key: value for key, value in report.items() if key != "log"}, indent=2))
    return 0 if report.get("passed") is True else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tree", type=Path, default=DEFAULT_TREE,
                        help="nxnative checkout whose fixture and sources are built")
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="A new scratch directory for the project, build and reports")
    parser.add_argument("--fixture", choices=("clear_ds",), default="clear_ds")
    parser.add_argument("--device", help="devicectl device name or UDID (required for --run)")
    parser.add_argument("--team", required=True, help="Apple development team for signing")
    parser.add_argument("--bundle-id", required=True, help="Bundle identifier registered to your team")
    parser.add_argument("--ios", default="26.0", help="iOS deployment target")
    parser.add_argument("--profile", required=True,
                        help="Source profile title ID for compile-time constants; no game data is used")
    parser.add_argument("--timeout", type=float, default=120,
                        help="Seconds to wait for the device report after launch (--run only)")
    parser.add_argument("--slot-run", help="Optional build slot wrapper path")
    parser.add_argument("--slot-dir", default="/tmp/slot-run-shared", help="The fleet's SLOT_RUN_DIR")
    parser.add_argument("--slot-name", default="gfx-clear-ds", help="Owner name shown in the slot directory")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--generate-only", action="store_true")
    mode.add_argument("--build-only", action="store_true")
    mode.add_argument("--run", action="store_true", help="Build, then install and launch on --device")
    args = parser.parse_args()
    if args.run and not args.device:
        parser.error("--run requires --device")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    output = args.output_dir.expanduser().resolve()
    slot = None if args.generate_only else slot_wrapper(args)
    run_id, tree, receipt = generate(args, output, slot)
    if args.generate_only:
        print(f"Generated project: {output}\nReceipt: {output / 'receipt.json'}")
        return 0
    app = build(args, output, tree, receipt, slot)
    print(f"Built app: {app}\nReceipt: {output / 'receipt.json'}")
    if args.build_only:
        return 0
    return device_run(args, output, run_id, app)


APP_SOURCE = r'''#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// The fixture (main renamed by fixture_entry.m) and its counters.
extern int gfx_fixture_main(void), gfx_fixture_checks(void), gfx_fixture_failures(void);

@interface GfxDelegate : UIResponder <UIApplicationDelegate>
@end

@interface GfxSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation GfxDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)app; (void)options;
    fprintf(stderr, "[verification] application finished launching\n");
    return YES;
}
@end

static NSURL *documents_url(void) {
    return [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
}

static void write_report(NSDictionary *report, NSURL *results) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL];
    [data writeToURL:results options:NSDataWritingAtomic error:NULL];
}

@implementation GfxSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    UIWindowScene *windowScene = (UIWindowScene *)scene;
    self.window = [[UIWindow alloc] initWithWindowScene:windowScene];
    self.window.frame = windowScene.coordinateSpace.bounds;
    UIViewController *controller = [UIViewController new];
    controller.view.backgroundColor = UIColor.systemBackgroundColor;
    UILabel *label = [UILabel new];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.numberOfLines = 0;
    label.text = @"Running FIXTURE_NAME regression…";
    [controller.view addSubview:label];
    [NSLayoutConstraint activateConstraints:@[
        [label.leadingAnchor constraintEqualToAnchor:controller.view.safeAreaLayoutGuide.leadingAnchor constant:20],
        [label.trailingAnchor constraintEqualToAnchor:controller.view.safeAreaLayoutGuide.trailingAnchor constant:-20],
        [label.centerYAnchor constraintEqualToAnchor:controller.view.centerYAnchor]
    ]];
    self.window.rootViewController = controller;
    [self.window makeKeyAndVisible];
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        // A background worker, never the main thread: the fixture blocks on GPU completion.
        [NSThread detachNewThreadWithBlock:^{
            @autoreleasepool {
                NSURL *results = [documents_url() URLByAppendingPathComponent:@"results.json"];
                NSURL *log = [documents_url() URLByAppendingPathComponent:@"gfx-fixture.log"];
                NSMutableDictionary *report = [@{@"runID": @"RUN_ID", @"complete": @NO} mutableCopy];
                write_report(report, results);
                int exitCode = gfx_fixture_main();
                int checks = gfx_fixture_checks(), failed = gfx_fixture_failures();
                fflush(stdout); fflush(stderr);
                id<MTLDevice> device = MTLCreateSystemDefaultDevice();
                report[@"device"] = @{@"model": UIDevice.currentDevice.model,
                                      @"systemVersion": UIDevice.currentDevice.systemVersion,
                                      @"gpu": device.name ?: @"none",
                                      @"MTL_DEBUG_LAYER": @(getenv("MTL_DEBUG_LAYER") ?: "unset")};
                report[@"tests"] = @{@"FIXTURE_NAME": @{@"exitCode": @(exitCode), @"checks": @(checks), @"failed": @(failed)}};
                report[@"log"] = [NSString stringWithContentsOfURL:log encoding:NSUTF8StringEncoding error:NULL] ?: @"";
                report[@"complete"] = @YES;
                // A fixture that ran no checks, or exited early, did not pass.
                report[@"passed"] = exitCode == 0 && failed == 0 && checks > 0 ? @YES : @NO;
                write_report(report, results);
                dispatch_async(dispatch_get_main_queue(), ^{
                    label.text = [NSString stringWithFormat:@"%@\nFIXTURE_NAME: %d checks, %d failed",
                                  [report[@"passed"] boolValue] ? @"Passed" : @"Failed", checks, failed];
                });
            }
        }];
    });
}
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        // The fixture runs under Metal validation on the Mac (Makefile: check_t_clear_ds); request it here
        // before any device exists. Whether the device honours it is reported in results.json.
        setenv("MTL_DEBUG_LAYER", "1", 0);
        NSURL *log = [documents_url() URLByAppendingPathComponent:@"gfx-fixture.log"];
        freopen(log.path.fileSystemRepresentation, "w", stdout);
        dup2(fileno(stdout), STDERR_FILENO);
        setvbuf(stdout, NULL, _IOLBF, 0); setvbuf(stderr, NULL, _IOLBF, 0);
        fprintf(stderr, "[verification] entering UIApplicationMain\n");
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(GfxDelegate.class));
    }
}
'''


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(f"GFX verification failed: {error}", file=sys.stderr)
        sys.exit(1)
