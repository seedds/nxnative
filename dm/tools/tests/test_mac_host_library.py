"""Headless Mac launcher/library integration: no game, real host entry, GPU or windows.

Only synthetic libraries execute an entry point. Set NXNATIVE_TEST_HOST_LIBRARY
for an optional view-only smoke test of a locally built real host library.
"""
import os
from pathlib import Path
import plistlib
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[3]
HOST = REPO / 'dm/host'
TITLE = '0100000000000000'
TEST_ROOT = Path(os.environ.get('NXNATIVE_TEST_ROOT', tempfile.gettempdir()))

# Include the real launcher, including its static loader and main. The replacement
# settings UI only constructs a view; it never starts an AppKit run loop/window.
HELPER = r'''
#import <AppKit/AppKit.h>
#define main launcher_main
#include "launcher.c"
#undef main
#include <assert.h>
// Walk actual AppKit frames without creating a window or running an event loop.
static void assert_layout(NSView *view) {
    // AppKit owns the internal geometry of native controls.
    if ([view isKindOfClass:NSControl.class]) return;
    if ([view isKindOfClass:NSScrollView.class]) {
        NSScrollView *scroll = (NSScrollView *)view;
        assert(!scroll.hasHorizontalScroller);
        assert(scroll.documentView.frame.size.width <= scroll.contentSize.width);
        assert_layout(scroll.documentView);
        return;
    }
    for (NSView *child in view.subviews) {
        assert(NSMinX(child.frame) >= 0 && NSMaxX(child.frame) <= NSWidth(view.bounds));
        assert(NSMinY(child.frame) >= 0 && NSMaxY(child.frame) <= NSHeight(view.bounds));
        for (NSView *other in view.subviews) {
            if (other != child) assert(!NSIntersectsRect(child.frame, other.frame));
        }
        assert_layout(child);
    }
}
static void assert_plain_player_copy(NSView *view) {
    if (view.hidden) return;
    NSString *text = nil;
    if ([view isKindOfClass:NSTextField.class]) text = ((NSTextField *)view).stringValue;
    else if ([view isKindOfClass:NSButton.class]) text = ((NSButton *)view).title;
    if (text) {
        NSRange mode = [text rangeOfString:@"\\b(own|share|shared)\\b"
            options:NSRegularExpressionSearch | NSCaseInsensitiveSearch];
        assert(mode.location == NSNotFound);
    }
    if ([view isKindOfClass:NSControl.class]) return;
    for (NSView *child in view.subviews) assert_plain_player_copy(child);
}
static void assert_players_layout(NSView *panel) {
    assert([panel isKindOfClass:NSStackView.class]);
    NSStackView *stack = (NSStackView *)panel;
    assert(stack.orientation == NSUserInterfaceLayoutOrientationVertical);
    assert([stack.arrangedSubviews.firstObject isKindOfClass:NSTextField.class]);
    NSTextField *summary = (NSTextField *)stack.arrangedSubviews.firstObject;
    NSButton *add = nil;
    NSScrollView *scroll = nil;
    for (NSView *child in stack.arrangedSubviews) {
        if ([child isKindOfClass:NSScrollView.class]) scroll = (NSScrollView *)child;
        if ([child isKindOfClass:NSButton.class] &&
            [((NSButton *)child).title isEqualToString:@"Add input device"])
            add = (NSButton *)child;
    }
    assert(add && scroll && scroll.documentView);
    // Check the default, open chooser and restored layouts without a window/events.
    for (int pass = 0; pass < 3; pass++) {
        [panel setFrameSize:panel.fittingSize];
        [panel layoutSubtreeIfNeeded];
        assert(NSWidth(panel.frame) == 430);
        assert(NSHeight(panel.frame) > 0);
        // Only the controlled keyboard-only fixture has a fixed compactness bound.
        // Real-library probes can discover many controllers and longer summaries.
        if (pass != 1 && [summary.stringValue isEqualToString:@"1 player\nPlayer 1: Keyboard"])
            assert(NSHeight(panel.frame) < 500);
        // Native stacks retain hidden arranged views; their stale frames do not
        // participate in layout. Check only the visible arranged views here.
        for (NSView *child in stack.arrangedSubviews) {
            if (child.hidden) continue;
            assert(NSContainsRect(panel.bounds, child.frame));
            for (NSView *other in stack.arrangedSubviews)
                if (other != child && !other.hidden)
                    assert(!NSIntersectsRect(child.frame, other.frame));
        }
        NSRect summaryFrame = [summary convertRect:summary.bounds toView:panel];
        NSRect scrollFrame = [scroll convertRect:scroll.bounds toView:panel];
        assert(panel.flipped ? NSMaxY(summaryFrame) <= NSMinY(scrollFrame) :
                               NSMinY(summaryFrame) >= NSMaxY(scrollFrame));
        assert_layout(scroll);
        assert_plain_player_copy(panel);
        NSMutableSet<NSString *> *players = [NSMutableSet new];
        NSUInteger controls = 0, removes = 0, assignments = 0;
        for (NSView *child in scroll.documentView.subviews) {
            if (child.hidden) continue;
            if ([child isKindOfClass:NSTextField.class]) {
                NSString *text = ((NSTextField *)child).stringValue;
                assert(![text isEqualToString:@"Own"] && ![text isEqualToString:@"Share"]);
                if ([text isEqualToString:@"Controls"]) {
                    controls++;
                    NSView *dropdown = nil, *remove = nil;
                    for (NSView *peer in scroll.documentView.subviews) {
                        if (peer.hidden || NSMaxY(peer.frame) <= NSMinY(child.frame) ||
                            NSMinY(peer.frame) >= NSMaxY(child.frame)) continue;
                        if ([peer isKindOfClass:NSPopUpButton.class]) dropdown = peer;
                        else if ([peer isKindOfClass:NSButton.class] &&
                                 [((NSButton *)peer).title isEqualToString:@"Remove"]) remove = peer;
                    }
                    assert(dropdown && remove);
                    assert(NSMaxX(child.frame) <= NSMinX(dropdown.frame));
                    assert(NSMaxX(dropdown.frame) <= NSMinX(remove.frame));
                }
            }
            if ([child isKindOfClass:NSPopUpButton.class]) {
                NSPopUpButton *picker = (NSPopUpButton *)child;
                if ([picker.itemTitles containsObject:@"New player"] ||
                    [picker.itemTitles.firstObject hasPrefix:@"Player "]) {
                    assignments++;
                    if ([picker.titleOfSelectedItem hasPrefix:@"Player "])
                        [players addObject:picker.titleOfSelectedItem];
                }
            } else if ([child isKindOfClass:NSButton.class]) {
                assert([((NSButton *)child).title isEqualToString:@"Remove"]);
                removes++;
            }
        }
        assert(controls == assignments && removes == assignments);
        NSArray<NSString *> *lines = [summary.stringValue componentsSeparatedByString:@"\n"];
        NSString *count = [NSString stringWithFormat:@"%lu player%@",
            (unsigned long)players.count, players.count == 1 ? @"" : @"s"];
        assert([lines.firstObject isEqualToString:count]);
        assert(lines.count == players.count + 1);
        NSMutableSet<NSString *> *summarized = [NSMutableSet new];
        for (NSUInteger i = 1; i < lines.count; i++) {
            NSRange separator = [lines[i] rangeOfString:@": "];
            assert(separator.location != NSNotFound);
            NSString *player = [lines[i] substringToIndex:separator.location];
            assert([players containsObject:player] && ![summarized containsObject:player]);
            assert(lines[i].length > NSMaxRange(separator));
            [summarized addObject:player];
        }
        // Exercise the available keyboard row even without an attached controller.
        if (pass < 2) [add performClick:nil];
    }
}
static void event(const char *s) {
    FILE *f = fopen(getenv("TEST_EVENTS"), "a"); assert(f);
    fprintf(f, "%s\n", s); fclose(f);
}
int dm_mac_launcher(void) {
    event("launcher");
    if (g_host_library) {
        int (*entries)(void) = dlsym(g_host_library, "test_entries");
        assert(entries && entries() == 0);
        [NSApplication sharedApplication];
        assert(dm_launcher_players_view(0) == NULL);
        if (!getenv("TEST_SKIP_PREPARE")) {
            void *a = dm_launcher_players_view(1);
            assert(a && a == dm_launcher_players_view(1));
            assert([(__bridge id)a isKindOfClass:NSView.class]);
            assert_players_layout((__bridge NSView *)a);
        }
        assert(entries() == 0);
    } else assert(dm_launcher_players_view(1) == NULL);
    assert(NSApp.windows.count == 0);
    setenv("TEST_FROM_UI", "selected", 1);
    const char *setup = getenv("TEST_POST_PLAY_SETUP");
    if (setup) setenv("DM_PLAYER_SETUP", setup, 1);
    return atoi(getenv("TEST_SHOWN"));
}
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc > 1 && !strcmp(argv[1], "--probe")) {
            assert(argc == 5);
            int loaded = load_host_library(argv[2], argv[3]);
            printf("loaded=%d\n", loaded);
            assert(loaded == atoi(argv[4]));
            if (!loaded) assert(!g_host_entry && !dm_launcher_players_view(1));
            return 0;
        }
        if (argc > 1 && !strcmp(argv[1], "--real-view")) {
            assert(argc == 3);
            void *h = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
            if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
            void *(*view)(void) = dlsym(h, "dm_mac_players_view");
            void (*finish)(int) = dlsym(h, "dm_mac_players_finish");
            assert(view && finish);
            [NSApplication sharedApplication];
            void *v = view();
            assert(v && [(__bridge id)v isKindOfClass:NSView.class]);
            assert(v == view());
            assert_players_layout((__bridge NSView *)v);
            finish(0);
            assert(NSApp.windows.count == 0);
            puts("real-view-ok");
            // Deliberately do not resolve or invoke the real host entry point.
            return 0;
        }
        return launcher_main(argc, argv);
    }
}
'''

LIBRARY = r'''
#import "players_mac.h"
#include "mac_host.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef TEST_IDENTITY
#define TEST_IDENTITY DM_MAC_HOST_ABI ":0100000000000000"
#endif
static DmMacPlayers *players;
static NSView *configuration;
static int entries, finished = -1;
static void event(const char *s) {
    FILE *f = fopen(getenv("TEST_EVENTS"), "a"); assert(f);
    fprintf(f, "%s\n", s); fclose(f);
}
__attribute__((constructor)) static void loaded(void) { event("loaded"); }
const char *dm_mac_host_identity(void) { return TEST_IDENTITY; }
int test_entries(void) { return entries; }
void *dm_mac_players_view(void) {
    assert(entries == 0);
    if (!players) {
        players = [DmMacPlayers new];
        players.keyboardPlayer = 3;
        configuration = [players configurationView];
    }
    assert(configuration == [players configurationView]);
    event("view");
    return (__bridge void *)configuration;
}
#ifndef TEST_MISSING_FINISH
void dm_mac_players_finish(int play) {
    assert(entries == 0);
    finished = play;
    if (players) assert(players.keyboardPlayer == 3 && configuration == [players configurationView]);
    event(play ? "finish:1" : "finish:0");
}
#endif
int dm_mac_host_entry(int argc, char **argv) {
    assert(entries++ == 0);
    assert(argc == 2 && strstr(argv[0], "/Contents/MacOS/dmhost"));
    assert(strstr(argv[1], "/Contents/Resources/dm"));
    if (getenv("TEST_EXPECT_PLAY")) {
        assert(finished == 1 && players && players.keyboardPlayer == 3);
        assert(configuration == [players configurationView]);
        assert(!strcmp(getenv("TEST_FROM_UI"), "selected"));
    } else assert(finished == 0);
    assert(NSApp.windows.count == 0);
    char cwd[4096]; assert(getcwd(cwd, sizeof cwd));
    assert(!strcmp(cwd, getenv("DM_ROOT")));
    event("entry");
    FILE *f = fopen(getenv("TEST_OUTPUT"), "w"); assert(f);
    const char *keys[] = {"DM_ROOT", "DM_SAVE", "DM_TEMP", "DM_ROMFS", "DM_MSL_DIR",
        "DM_FONT_DIR", "DM_LANG", "DM_WINDOW", "DM_AUDIO", "DM_ENV_FILE_KEYS",
        "TEST_FROM_FILE", "TEST_FROM_UI", NULL};
    for (int i = 0; keys[i]; i++) fprintf(f, "%s=%s\n", keys[i], getenv(keys[i]) ?: "");
    fclose(f);
    puts("synthetic library output");
    return 23;
}
'''

CHILD = r'''
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    FILE *f = fopen(getenv("TEST_EVENTS"), "a");
    if (!f || argc != 2) return 99;
    fprintf(f, "child\n"); fclose(f);
    puts("synthetic child output");
    return 17;
}
'''


# Link the real lifecycle, but never initialize a GPU, audio device or save sync.
LIFECYCLE = r'''
#include "hle.h"
#include <assert.h>
#include <dispatch/dispatch.h>
#include <time.h>
#include <unistd.h>
void dm_term_install(void);
static void event(const char *s) {
    FILE *f = fopen(getenv("TEST_EVENTS"), "a"); assert(f);
    fprintf(f, "%s\n", s); assert(fclose(f) == 0);
}
void hlecount_flush(void) { event("flush"); }
void dm_rec_finish(void) { event("record"); }
void dm_cheats_exit(void) { event("cheats"); }
void dm_audio_suspend(int on) { assert(on == 1); event("audio"); }
void dm_gfx_suspend(int on) { assert(on == 1); event("gfx"); }
uint64_t now_ns(void) {
    struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
int main(void) {
    dm_term_install();
    event("ready");
    dispatch_main();
}
'''


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('clang'), 'requires the macOS compiler and SDK')
class MultiProgramHostIdentity(unittest.TestCase):
    def test_real_host_compiles_with_constant_family_identity(self):
        with tempfile.TemporaryDirectory(dir=TEST_ROOT) as tmp:
            header = Path(tmp) / 'game_profile.h'
            subprocess.run([sys.executable, str(REPO / 'dm/games/game.py'), 'header', '010049900F546000', str(header)],
                           check=True, capture_output=True)
            self.assertIn('#define DM_GAME_PROFILE_ID "010049900F546000"', header.read_text())
            result = subprocess.run(['clang', '-fsyntax-only', '-fobjc-arc', '-Werror', '-Wno-deprecated-declarations',
                                     '-Wno-unused-but-set-variable', '-DDM_MAC_HOST_LIBRARY',
                                     '-DDM_MAC_EXECUTABLE_SHA_SYMBOL=test_executable_sha', '-I', tmp, '-I', str(HOST),
                                     str(HOST / 'main.c'), str(HOST / 'front_mac.m')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


class SaveCleanupStructure(unittest.TestCase):
    def test_cleanup_dispatch_precedes_all_launcher_startup(self):
        launcher = (REPO / 'dm/mac/launcher.c').read_text()
        main = launcher.split('int main(int argc, char **argv) {', 1)[1].lstrip()
        self.assertTrue(main.startswith('int cleanup = dm_save_cleanup_main(argc, argv);'))
        self.assertLess(main.index('if (cleanup >= 0) return cleanup;'),
                        main.index('_NSGetExecutablePath'))

    def test_native_cleanup_starts_before_entry_and_fallback_stays_parent_owned(self):
        launcher = (REPO / 'dm/mac/launcher.c').read_text()
        branch = launcher.split('if (loaded && shown == 1 && g_players_prepared', 1)[1]
        self.assertLess(branch.index('dm_save_cleanup_start(exe, id)'),
                        branch.index('return g_host_entry(2, host_argv);'))
        self.assertLess(branch.index('return g_host_entry(2, host_argv);'),
                        branch.index('waitpid(g_child'))
        self.assertLess(branch.index('waitpid(g_child'), branch.index('save_sync_after_exit('))
        self.assertEqual(launcher.count('save_sync_after_exit('), 1)

    def test_host_does_not_flush_cloud_during_shutdown(self):
        for path in (HOST / 'hle_lifecycle.c', HOST / 'main.c', REPO / 'dm/mac/launcher.c'):
            with self.subTest(path=path):
                source = path.read_text()
                self.assertNotRegex(source, r'\batexit\s*\(')
                if path.name != 'launcher.c':
                    self.assertNotIn('save_sync_after_exit', source)


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'requires macOS AppKit and clang')
class MacHostLibrary(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        TEST_ROOT.mkdir(parents=True, exist_ok=True)
        cls.scratch = tempfile.TemporaryDirectory(prefix='mac-host-library-', dir=TEST_ROOT)
        cls.addClassCleanup(cls.scratch.cleanup)
        cls.build = Path(cls.scratch.name).resolve()
        (cls.build / 'helper.m').write_text(HELPER)
        (cls.build / 'library.m').write_text(LIBRARY)
        (cls.build / 'child.c').write_text(CHILD)
        cls.compile('-fobjc-arc', '-I' + str(REPO / 'dm/mac'), cls.build / 'helper.m',
                    REPO / 'dm/mac/save_cleanup.c', HOST / 'save_sync.c',
                    '-framework', 'AppKit', '-framework', 'CoreFoundation',
                    '-o', cls.build / 'launcher')
        cls.compile(cls.build / 'child.c', '-o', cls.build / 'child')
        (cls.build / 'game_profile.h').write_text(f'#define DM_GAME_ID "{TITLE}"\n')
        (cls.build / 'lifecycle.c').write_text(LIFECYCLE)
        cls.compile('-fblocks', '-DDM_MAC_HOST_LIBRARY', '-I' + str(cls.build),
                    cls.build / 'lifecycle.c', HOST / 'hle_lifecycle.c',
                    '-o', cls.build / 'lifecycle')
        for name, flags in (
            ('valid', []), ('wrong-title', ['-DTEST_IDENTITY="1:0100000000000001"']),
            ('wrong-abi', ['-DTEST_IDENTITY="999:0100000000000000"']),
            ('missing-export', ['-DTEST_MISSING_FINISH=1']),
        ):
            cls.compile('-dynamiclib', '-fobjc-arc', *flags, cls.build / 'library.m',
                        HOST / 'players_mac.m', '-framework', 'AppKit', '-framework', 'GameController',
                        '-o', cls.build / (name + '.dylib'))

    @classmethod
    def compile(cls, *args):
        result = subprocess.run(['xcrun', 'clang', '-UNDEBUG', '-I' + str(HOST), *map(str, args)],
                                capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='mac-host-case-', dir=TEST_ROOT)
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.contents = self.root / 'Synthetic.app/Contents'
        self.macos = self.contents / 'MacOS'
        self.resources = self.contents / 'Resources'
        for d in (self.macos, self.resources / 'dm', self.resources / 'romfs', self.root / 'home'):
            d.mkdir(parents=True)
        shutil.copy2(self.build / 'launcher', self.macos / 'Launcher')
        shutil.copy2(self.build / 'child', self.macos / 'dmhost')
        (self.resources / 'romfs/marker').write_text('synthetic data')
        (self.resources / 'dm/main.patched.bin').write_bytes(b'synthetic module')
        (self.resources / 'dm/env.txt').write_text('TEST_FROM_FILE=bundle\n')
        (self.contents / 'Info.plist').write_bytes(plistlib.dumps({
            'CFBundleExecutable': 'Launcher', 'CFBundleIdentifier': 'test.nxnative.synthetic.' + self.root.name,
            'CFBundlePackageType': 'APPL', 'CFBundleName': 'Synthetic', 'DMTitleID': TITLE,
            'DMRomfsMarker': 'marker', 'DMModuleFiles': ['main.patched.bin'], 'DMLanguage': 'fr',
        }))
        # Never inherit a real game's paths, cloud-save folder or player preferences.
        self.env = {k: v for k, v in os.environ.items() if not k.startswith(('DM_', 'TEST_'))}
        self.env.update(HOME=str(self.root / 'home'), DM_SAVED_CHOICES='0', DM_SAVE_SYNC='',
                        TEST_EVENTS=str(self.root / 'events'), TEST_OUTPUT=str(self.root / 'output'), TEST_SHOWN='1')

    def library(self, name='valid'):
        shutil.copy2(self.build / (name + '.dylib'), self.macos / 'dmhost.dylib')

    def run_helper(self, *args, expected=0):
        result = subprocess.run([str(self.macos / 'Launcher'), *map(str, args)], env=self.env,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr + self.log())
        return result

    def events(self):
        p = self.root / 'events'
        return p.read_text().splitlines() if p.exists() else []

    def log(self):
        p = self.root / f'home/Library/Logs/nxnative/{TITLE}.log'
        return p.read_text() if p.exists() else ''

    def run_signal_cleanup(self, signum):
        (self.root / 'events').unlink(missing_ok=True)
        with subprocess.Popen([str(self.build / 'lifecycle')], env=self.env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as proc:
            try:
                deadline = time.monotonic() + 10
                while 'ready' not in self.events():
                    if proc.poll() is not None or time.monotonic() >= deadline:
                        self.fail('lifecycle did not become ready')
                    time.sleep(0.01)
                proc.send_signal(signum)
                stdout, stderr = proc.communicate(timeout=10)
                self.assertEqual(proc.returncode, 0, stdout + stderr)
                self.assertEqual(self.events(), [
                    'ready', 'flush', 'record', 'cheats', 'audio', 'gfx'])
                self.assertIn('graceful exit done', stderr)
                self.assertNotIn('not done in 1.5 seconds', stderr)
            finally:
                if proc.poll() is None:
                    proc.kill()
                proc.communicate()

    def test_native_library_signal_cleanup(self):
        for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            with self.subTest(signal=signum):
                self.run_signal_cleanup(signum)

    def test_loader_uses_fixed_macos_path_and_absence_means_fallback(self):
        # A library beside resources (or in cwd) must not be searched/loaded.
        shutil.copy2(self.build / 'valid.dylib', self.resources / 'dmhost.dylib')
        self.run_helper('--probe', self.contents, TITLE, '0')
        self.assertEqual(self.events(), [])
        self.library()
        self.run_helper('--probe', self.contents, TITLE, '1')
        self.assertEqual(self.events(), ['loaded'])  # no view and no guest before Play

    def test_loader_rejects_identity_and_missing_exports(self):
        for name in ('wrong-title', 'wrong-abi', 'missing-export'):
            with self.subTest(name=name):
                self.library(name)
                result = self.run_helper('--probe', self.contents, TITLE, '-1')
                self.assertIn('ABI/profile mismatch', result.stderr)
        self.assertEqual(self.events(), ['loaded'] * 3)

    def test_main_rejects_mismatch_before_ui_or_guest(self):
        self.library('wrong-title')
        self.run_helper(expected=1)
        self.assertEqual(self.events(), ['loaded'])
        self.assertIn('ABI/profile mismatch', self.log())
        self.assertFalse((self.root / 'output').exists())

    def test_invalid_dylib_is_not_silently_treated_as_legacy(self):
        (self.macos / 'dmhost.dylib').write_text('not a library')
        result = self.run_helper('--probe', self.contents, TITLE, '-1')
        self.assertIn('cannot load bundled host', result.stderr)
        self.assertEqual(self.events(), [])

    def test_play_shares_native_objects_and_hands_off_environment_and_output(self):
        self.library()
        self.env.update(TEST_EXPECT_PLAY='1', DM_PLAYER_SETUP='1')
        self.run_helper(expected=23)
        self.assertEqual(self.events(), ['loaded', 'launcher', 'finish:0', 'view', 'view', 'finish:1', 'entry'])
        values = dict(line.split('=', 1) for line in (self.root / 'output').read_text().splitlines())
        support = self.root / f'home/Library/Application Support/nxnative/{TITLE}'
        for key, path in {'DM_ROOT': support, 'DM_SAVE': support / 'save', 'DM_TEMP': support / 'temp',
                          'DM_ROMFS': self.resources / 'romfs', 'DM_MSL_DIR': self.resources / 'msl',
                          'DM_FONT_DIR': self.resources / 'dm/font'}.items():
            self.assertEqual(values[key], str(path))
        for key, value in {'DM_LANG': 'fr', 'DM_WINDOW': '1', 'DM_AUDIO': '1',
                           'TEST_FROM_FILE': 'bundle', 'TEST_FROM_UI': 'selected'}.items():
            self.assertEqual(values[key], value)
        self.assertIn('TEST_FROM_FILE', values['DM_ENV_FILE_KEYS'].split())
        self.assertIn('synthetic library output', self.log())

    def test_quit_never_enters_guest(self):
        self.library()
        self.env['TEST_SHOWN'] = '0'
        self.run_helper()
        self.assertEqual(self.events(), ['loaded', 'launcher', 'finish:0', 'view', 'view', 'finish:0'])
        self.assertFalse((self.root / 'output').exists())

    def test_bypasses_do_not_confirm_player_setup(self):
        self.library()
        self.env['DM_PLAYER_SETUP'] = '1'
        for bypass in ('environment', 'argument', 'saved-window-choice'):
            with self.subTest(bypass=bypass):
                (self.root / 'events').unlink(missing_ok=True)
                self.env.pop('DM_LAUNCHER', None)
                self.env['TEST_SHOWN'] = '2' if bypass == 'saved-window-choice' else '1'
                if bypass == 'environment': self.env['DM_LAUNCHER'] = '0'
                self.run_helper(*(['--no-launcher'] if bypass == 'argument' else []), expected=17)
                expected = ['loaded']
                if bypass == 'saved-window-choice': expected += ['launcher', 'finish:0', 'view', 'view']
                self.assertEqual(self.events(), expected + ['finish:0', 'child'])
                self.assertFalse((self.root / 'output').exists())
                self.assertIn('host exited with 17', self.log())

    def test_unselected_setup_keeps_loaded_library_but_spawns_child(self):
        self.library()
        # The synthetic UI creates a view even when setup is off: preparation
        # alone must not opt into the in-process host.
        for setup in (None, '0', '2'):
            with self.subTest(setup=setup):
                (self.root / 'events').unlink(missing_ok=True)
                self.env.pop('DM_PLAYER_SETUP', None)
                if setup is not None:
                    self.env['DM_PLAYER_SETUP'] = setup
                self.run_helper(expected=17)
                self.assertEqual(self.events(), [
                    'loaded', 'launcher', 'finish:0', 'view', 'view', 'finish:1', 'child'])
                self.assertFalse((self.root / 'output').exists())
                self.assertIn('host exited with 17', self.log())

    def test_post_play_setup_enters_library_only_with_prepared_view(self):
        self.library()
        self.env.update(DM_PLAYER_SETUP='0', TEST_POST_PLAY_SETUP='1', TEST_EXPECT_PLAY='1')
        for prepared in (False, True):
            with self.subTest(prepared=prepared):
                (self.root / 'events').unlink(missing_ok=True)
                self.env['DM_PLAYER_SETUP'] = '0'
                if prepared:
                    self.env.pop('TEST_SKIP_PREPARE', None)
                else:
                    self.env['TEST_SKIP_PREPARE'] = '1'
                self.run_helper(expected=23 if prepared else 17)
                expected = ['loaded', 'launcher', 'finish:0']
                if prepared:
                    expected += ['view', 'view']
                self.assertEqual(self.events(), expected + ['finish:1', 'entry' if prepared else 'child'])
                self.assertEqual((self.root / 'output').exists(), prepared)

    def test_post_play_setup_disabled_falls_back_despite_prepared_view(self):
        self.library()
        self.env.update(DM_PLAYER_SETUP='1', TEST_POST_PLAY_SETUP='0')
        self.run_helper(expected=17)
        self.assertEqual(self.events(), [
            'loaded', 'launcher', 'finish:0', 'view', 'view', 'finish:1', 'child'])
        self.assertFalse((self.root / 'output').exists())

    def test_legacy_fallback_spawns_child_and_preserves_exit_status(self):
        self.env['DM_LAUNCHER'] = '0'
        self.run_helper(expected=17)
        self.assertEqual(self.events(), ['child'])
        self.assertIn('synthetic child output', self.log())
        self.assertIn('host exited with 17', self.log())

    @unittest.skipUnless(os.environ.get('NXNATIVE_TEST_HOST_LIBRARY'), 'opt-in locally built real library view smoke')
    def test_real_library_view_only_never_calls_entry(self):
        library = Path(os.environ['NXNATIVE_TEST_HOST_LIBRARY']).resolve()
        result = self.run_helper('--real-view', library)
        self.assertIn('real-view-ok', result.stdout)


if __name__ == '__main__':
    unittest.main()
