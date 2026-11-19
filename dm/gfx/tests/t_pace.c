// Pacer unit tests: given a clock sequence, the expected wait durations are hand-calculated. The period 16.666 ms is written as P = 16666667 ns.
#include "../nvn/nvn_pace.h"
#include <stdio.h>

int main(void) {
    const uint64_t P = 16666667, MS = 1000000;
    struct { uint64_t now, period, want; const char *why; } c[] = {
        {1000 * MS, P, 0, "first frame: starts the beat, no wait"},
        {1005 * MS, P, 1000 * MS + P - 1005 * MS, "5 ms early: wait until 1000+P"},
        {1000 * MS + 2 * P - 2 * MS, P, 2 * MS, "third frame 2 ms early: wait 2 ms"},
        {1000 * MS + 3 * P + 3 * MS, P, 0, "3 ms late (less than a period): present at once, beat point stays 1000+3P"},
        {1000 * MS + 4 * P - 1 * MS, P, 1 * MS, "next frame 1 ms early: wait until 1000+4P in the original phase"},
        {1000 * MS + 7 * P, P, 0, "two periods behind (stall): restart the beat now, no burst to catch up"},
        {1000 * MS + 7 * P + 4 * MS, P, P - 4 * MS, "next frame after the restart: wait until the new target"},
        {1000 * MS + 9 * P, 0, 0, "period 0 (DM_PRESENT_HZ=0): no wait"},
        {1000 * MS + 9 * P + 1 * MS, 2 * P, 2 * P - 1 * MS, "back to interval 2: beat point = previous frame time 9P + 2P"},
        {1000 * MS + 11 * P + 3 * MS, 2 * P, 2 * P - 3 * MS, "interval 2, next frame: beat point 13P"},
    };
    nvn_pacer p = {0}; int fail = 0, n = sizeof c / sizeof *c;
    for (int i = 0; i < n; i++) {
        uint64_t got = nvn_pace(&p, c[i].now, c[i].period);
        int ok = got == c[i].want;
        printf("%-52s expected wait %8.3f ms → got %8.3f ms %s\n", c[i].why, c[i].want / 1e6, got / 1e6, ok ? "✓" : "✗");
        fail += !ok;
    }
    // The acquire / present cycle of hle_nvn.c, replayed: acquire waits for its texture's slot time avail[i] (when the beat paces the frame), the frame takes `work`,
    // present schedules the slot (nvn_pace, or nvn_pace_follow for a frame the display link paced) and the texture comes free one period after it
    {
        // A caller that waits, 1 ms of work a frame: 60.0 fps with any swap chain. With three or four textures the slot runs two or three periods ahead of the present,
        // and a cap on that lead (one period) presented at 87 and 116 fps
        for (int ntex = 2; ntex <= 4; ntex++) {
            nvn_pacer q = {0}; uint64_t avail[4] = {0}, t = 1000 * MS, t100 = 0; int k;
            for (k = 0; k < 1100; k++) { if (k == 100) t100 = t; if (avail[k % ntex] > t) t = avail[k % ntex]; t += MS; avail[k % ntex] = t + nvn_pace(&q, t, P) + P; }
            double fps = 1000.0 / ((t - t100) / 1e9); int ok = fps > 59.9 && fps < 60.1; n++; fail += !ok;
            printf("%d textures, the beat paces, 1 ms of work             %6.2f fps (60) %s\n", ntex, fps, ok ? "✓" : "✗");
        }
        // The display link paces 18,000 frames 12.5 ms apart (Ocarina of Time on a 120 Hz variable-refresh link), then goes silent and the beat takes over: the first wait
        // for a slot is within one period of the last present (stepping the beat for the link's frames left it 75 s ahead), and the beat then runs at the period
        for (int ntex = 2; ntex <= 3; ntex++) {
            nvn_pacer q = {0}; uint64_t avail[4] = {0}, t = 5000 * MS, worst = 0, t0 = 0; int k;
            for (k = 0; k < 18000; k++) { t += 12500000ull; nvn_pace_follow(&q, t); avail[k % ntex] = t + P; }
            for (; k < 18000 + 600; k++) {
                uint64_t w = avail[k % ntex] > t ? avail[k % ntex] - t : 0; if (w > worst) worst = w;
                t += w + MS; if (k == 18100) t0 = t;
                avail[k % ntex] = t + nvn_pace(&q, t, P) + P;
            }
            double per = (t - t0) / 499.0 / 1e6; int ok = worst <= P && per > 16.66 && per < 16.67; n++; fail += !ok;
            printf("%d textures, link paces 18,000 early frames, then silent: worst wait %.3f ms (≤ P), then %.3f ms a frame %s\n", ntex, worst / 1e6, per, ok ? "✓" : "✗");
        }
        // Handing back and forth: the link paces at 12.5 ms, goes silent for 300 frames, comes back, goes silent again. Every wait stays within one period
        { nvn_pacer q = {0}; uint64_t avail[3] = {0}, t = 5000 * MS, worst = 0; int k = 0;
          for (int phase = 0; phase < 6; phase++)
              for (int j = 0; j < 300; j++, k++) {
                  if (phase % 2 == 0) { t += 12500000ull; nvn_pace_follow(&q, t); avail[k % 3] = t + P; continue; }
                  uint64_t w = avail[k % 3] > t ? avail[k % 3] - t : 0; if (w > worst) worst = w;
                  t += w + 4 * MS; avail[k % 3] = t + nvn_pace(&q, t, P) + P;
              }
          int ok = worst <= P; n++; fail += !ok;
          printf("%-52s worst wait %8.3f ms (≤ P) %s\n", "link paced / silent / paced ... (3 textures)", worst / 1e6, ok ? "✓" : "✗"); }
    }
    printf("== %d checks, %d failed ==\n", n, fail);
    return fail != 0;
}
