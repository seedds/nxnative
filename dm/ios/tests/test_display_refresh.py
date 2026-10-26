#!/usr/bin/env python3
"""Exercise screen-request policy and independent timestamp/FPS measurements."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class DisplayRefresh(unittest.TestCase):
    def test_policy_and_measured_cadence(self):
        harness = r'''
#include <assert.h>
#include <math.h>
#include "DmDisplayRefresh.h"
static void near(double a, double b) { assert(fabs(a-b) < 0.000001); }
static void window(dm_display_probe *probe, int callbacks, int presents_per_four_ticks,
                   double start, uint64_t base, double expected_callback, double expected_fps) {
    dm_display_probe_result result;
    assert(!dm_display_probe_tick(probe, start, start+1.0/callbacks, base, &result));
    for (int i=1; i<=callbacks*2; ++i) {
        double timestamp=start+(double)i/callbacks;
        uint64_t presented=base+(uint64_t)i*presents_per_four_ticks/4;
        int ready=dm_display_probe_tick(probe,timestamp,timestamp+1.0/callbacks,presented,&result);
        if (i<callbacks*2) assert(!ready);
        else {
            assert(ready);
            near(result.callback_hz,expected_callback);
            near(result.presented_fps,expected_fps);
            near(result.target_period_ms,1000.0/callbacks);
            near(result.min_period_ms,1000.0/callbacks);
            near(result.max_period_ms,1000.0/callbacks);
        }
    }
}
int main(void) {
    assert(dm_display_hz_parse(0)==0);
    assert(dm_display_hz_parse("")==0);
    assert(dm_display_hz_parse("60")==60);
    assert(dm_display_hz_parse("120")==120);
    assert(dm_display_hz_parse("120fps")==-1);
    assert(dm_display_hz_parse("0")==-1);
    assert(dm_display_hz_select(120,120)==120);
    assert(dm_display_hz_select(60,120)==60);
    assert(dm_display_hz_select(120,60)==60);
    assert(dm_display_hz_select(120,90)==90);
    assert(dm_display_hz_select(120,144)==120);
    assert(dm_display_hz_select(120,0)==60);
    assert(dm_display_hz_select(60,30)==30);
    assert(dm_display_hz_select(0,120)==0);
    assert(dm_display_hz_select(-1,120)==0);
    dm_display_probe probe={0};
    // 120 display callbacks and 30 actual drawable presentations remain two
    // separate measurements; requesting 120 cannot fabricate rendered FPS.
    window(&probe,120,1,10,500,120,30);
    dm_display_probe_reset(&probe);
    window(&probe,60,4,20,1000,60,60);
    // A background pause is excluded, then the new 120/30 window is measured.
    dm_display_probe_result result;
    assert(!dm_display_probe_tick(&probe,100,100+1.0/120,1200,&result));
    dm_display_probe_reset(&probe);
    window(&probe,120,1,110,1200,120,30);
    // A counter reset or clock discontinuity seeds a fresh window.
    assert(!dm_display_probe_tick(&probe,112.01,112.02,0,&result));
    near(probe.start,112.01);
    assert(!dm_display_probe_tick(&probe,111,111.01,1,&result));
    near(probe.start,111);
    assert(!dm_display_probe_tick(&probe,NAN,112,2,&result));
    near(probe.start,111);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            unit, executable = Path(directory) / 'refresh.c', Path(directory) / 'refresh'
            unit.write_text(harness)
            subprocess.run(['xcrun', 'clang', '-Wall', '-Werror', '-I',
                            str(ROOT / 'dm/ios/Sources'), str(unit), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
