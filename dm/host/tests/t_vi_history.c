// nn::vi::ListFrameHistory answers as the SDK fills FrameInfo (sdk 20.5.15 sdk+0x4510a0): a null buffer gets the history's size, 8; entries come newest first, status 2 for a
// frame the present queue handed on with a present tick (one the Mac dropped too: a Switch shows every frame it is handed), 1 for one still queued, the frame numbers of GetLatestFrameNumber.
// Unreal's RHI takes the first status-2 entry as its latest flip (Minecraft Dungeons II main+0x142b0b0), so the newest shown frame has to come first. Without a Metal presented time,
// a frame with a pacer grid is shown at the vsync a display would give it (the first grid tick at or after its present and its GPU work's end, latched once reached), not first-seen time.
#include "../../gfx/hle_vi.c"
#include <assert.h>

static uint64_t g_count, g_in, g_done, g_gns = 5000000000ull, g_hns = 9000000000ull;
static int g_vsync_hz = 60;
static double g_pt[64];   // presented time per ticket (seconds on the now_ns clock), 0 not yet, -1 dropped
static int g_gd[64]; static uint64_t g_gde[64];   // mtl_gpu_done per GPU frame: state (-1 no record, 0 running, 1 done) and end time
uint64_t dm_nvn_frame_count(void) { return g_count; }
void mtl_present_progress(uint64_t *q, uint64_t *d) { *q = g_in; *d = g_done; }
double mtl_presented_time(uint64_t n) { return n < 64 ? g_pt[n] : 0; }
int mtl_gpu_done(uint64_t f, uint64_t *t1) { if (f >= 64) return -1; *t1 = g_gde[f]; return g_gd[f]; }
void mtl_present_layer(uint32_t w, uint32_t h) { (void)w; (void)h; }
uint64_t guest_ns(void) { return g_gns; }
uint64_t now_ns(void) { return g_hns; }
int dm_sdk_ounce(void) { return 0; }
int dm_opmode(void) { return 0; }
int nvn_vsync_hz(void) { return g_vsync_hz; }

static void present(void) { g_in++; g_count++; dm_vi_frame_queued(g_count, 0, 0, 0); dm_vi_frame_ticket(g_count, g_in); }   // no pacer grid (DM_DETCLOCK, display-link follow, unpaced): first-seen stays
static uint64_t g_at, g_grid;   // the present time and the pacer grid a paced present is queued with
static void ppresent(void) { g_in++; g_count++; g_done = g_in; g_pt[g_count] = 0; dm_vi_frame_queued(g_count, g_at, g_grid, 16666667ull); dm_vi_frame_ticket(g_count, g_in); }   // handed on, no presented time
static uint64_t ticks(uint64_t ns) { return ns * 12 / 625; }

int main(void) {
    void *display;
    uint32_t rate = 0;
    assert(h_vi_OpenDisp(&display) == RES_OK);
    assert(h_vi_RefreshRate(&rate, display) == RES_OK && rate == 60);
    g_vsync_hz = 120;
    assert(h_vi_RefreshRate(&rate, display) == RES_OK && rate == 120);
    assert(h_vi_RefreshRate(NULL, display) == RES_OK);
    assert(h_vi_RefreshRate(&rate, NULL) == 0x272 && rate == 120);

    uint64_t fi[10][5];
    for (int i = 0; i < 64; i++) g_gd[i] = -1;   // no GPU record until a case sets one
    assert(h_vi_FrameHist(NULL, 0, NULL) == 8 && h_vi_FrameHist(fi, 0, NULL) == 8);   // the SDK's count query
    assert(h_vi_FrameHist(fi, 10, NULL) == 0);                                        // nothing presented yet
    for (int i = 0; i < 10; i++) { present(); g_gns += 16666667; g_hns += 16666667; }  // frames 1-10, tickets 1-10
    g_done = 7;                                   // 1-7 handed on, 8-10 still queued
    g_pt[7] = (g_hns - 100000000ull) / 1e9;       // 7 shown 0.1 s before host now
    g_pt[6] = -1;                                 // 6 dropped by the system
    memset(fi, 0xa5, sizeof fi);
    int n = h_vi_FrameHist(fi, 10, NULL);
    assert(n == 8);                               // the last 8: frames 10 down to 3
    for (int k = 0; k < n; k++) assert(fi[k][1] == (uint64_t)(10 - k));
    for (int k = 0; k < 3; k++) assert(fi[k][0] == 1 && fi[k][3] == 0 && fi[k][4] == 0);   // 10, 9, 8: queued
    assert(fi[3][0] == 2 && fi[3][3] == fi[3][4] && fi[3][3] == ticks(g_gns - 100000000ull));   // 7: its presented time on the guest clock
    assert(fi[4][0] == 2 && fi[4][3] == ticks(g_gns));   // 6: dropped by the Mac, shown from when it was first seen handed on (Orbitals' frame-end sync waited for its flip for ever)
    assert(fi[5][0] == 2 && fi[5][3] == ticks(g_gns));   // 5: handed on, no presented time and no grid (DM_DETCLOCK / display-link follow): the time it was first seen
    assert(fi[0][2] == ticks(5000000000ull + 9 * 16666667ull));   // queue tick of frame 10
    assert(fi[8][0] == 0xa5a5a5a5a5a5a5a5ull);    // nothing written past the entries returned
    g_gns += 1000000000ull;
    n = h_vi_FrameHist(fi, 10, NULL);
    assert(fi[5][3] == ticks(g_gns - 1000000000ull));   // the first-seen time stays
    assert(h_vi_FrameHist(fi, 2, NULL) == 2 && fi[0][1] == 10 && fi[1][1] == 9);   // a smaller buffer gets the newest

    // --- the virtual vsync: frames 11 on, past the gridless ones above, fill the ring of 8 with paced presents ---
    uint64_t const T = 16666667ull, G0 = 10000000000ull;   // one vsync and the pacer's grid, on the now_ns clock
    g_gns = 6000000000ull; g_hns = 9500000000ull;          // the guest clock apart from the host one, as they run
    g_at = G0; g_grid = G0; ppresent(); ppresent();        // frames 11, 12: the ring past the gridless frames
    g_at = G0 - 20000000ull; g_gd[12] = 1; g_gde[12] = G0 - 10000000ull;   // frame 13 presented before the grid, its GPU work done before the vsync
    ppresent(); g_hns = G0 - 1000000ull;                   // frame 13
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][0] == 1 && fi[0][3] == 0 && fi[0][4] == 0);   // a: its vsync still to come: not shown yet
    g_hns = G0 + 5000000ull;
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][0] == 2 && fi[0][3] == fi[0][4] && fi[0][3] == ticks(g_gns - (g_hns - G0)));   // a, g: shown at the grid's vsync, as the guest clock dates it
    g_gd[12] = 0; g_gde[12] = G0 + 50000000ull;            // latching: once shown, a later GPU-state change cannot revert it to queued or move its flip tick
    g_hns += 1000000ull; g_gns += 1000000ull;
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][0] == 2 && fi[0][3] == ticks(g_gns - (g_hns - G0)));
    g_at = G0 + 1000000ull; g_gd[13] = 1; g_gde[13] = G0 + 20000000ull;   // frame 14 presented late, its GPU work ending later still
    ppresent(); g_hns = G0 + 3 * T;                       // frame 14
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][0] == 2 && fi[0][3] == fi[0][4] && fi[0][3] == ticks(g_gns - (g_hns - (G0 + 2 * T))));   // b: shown at the second vsync past the grid, past its GPU end
    g_at = G0; g_gd[14] = 0;                              // frame 15's GPU work still running
    ppresent(); g_hns = G0 + 1000000000ull;               // frame 15
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][0] == 1 && fi[0][3] == 0);               // c: however late the query, not shown while its GPU work runs
    g_at = G0 + 10000000ull;                              // frame 16: no GPU record (g_gd[15] stays -1)
    ppresent(); g_hns = G0 + 2 * T;                       // frame 16
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][0] == 2 && fi[0][3] == ticks(g_gns - (g_hns - (G0 + T))));   // d: no record: shown at the first grid tick at or after its present
    g_at = G0 - 5000000ull; g_gd[16] = 0; g_gd[17] = 1; g_gde[17] = G0 - 1000000ull;   // frame 17: the GPU work asked about is 16 (running), not 17 itself (done)
    ppresent(); g_hns = G0 + T;                           // frame 17
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][0] == 1 && fi[0][3] == 0);               // e: the GPU frame is frame - 1: 16 running holds it back though 17 itself is done
    g_at = G0;                                            // frame 18
    ppresent(); g_pt[18] = (g_hns - 30000000ull) / 1e9;   // Metal reports it presented 30 ms before now, before its vsync
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][0] == 2 && fi[0][3] == ticks(g_gns - 30000000ull));   // f: a presented time wins over the virtual vsync
    g_at = G0 - 2000000ull; g_gd[18] = 1; g_gde[18] = G0 - 1000000ull;   // frame 19: dropped by the Mac on a paced grid still resolves as shown at the virtual vsync
    ppresent(); g_pt[19] = -1; g_hns = G0 + 5000000ull;
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][0] == 2 && fi[0][3] == ticks(g_gns - (g_hns - G0)));

    // --- async ticket binding (dm_vi_frame_ticket from nq_worker) ---
    // frame 20: queued on game thread before nq_worker assigns its ticket; must stay status 1 even though g_done already passed frame 19 and hnow is past grid
    g_count++; g_hns = G0 + 5000000ull;
    dm_vi_frame_queued(20, G0 - 2000000ull, G0, T);
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][1] == 20 && fi[0][0] == 1 && fi[0][3] == 0 && fi[0][4] == 0);
    // once nq_worker assigns ticket 20 (still not handed on, g_done == 19), it stays queued; once g_done reaches 20, its own presentedTime wins
    g_in++; dm_vi_frame_ticket(20, g_in); g_pt[20] = (g_hns - 12000000ull) / 1e9;
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][1] == 20 && fi[0][0] == 1 && fi[0][3] == 0);
    g_done = g_in;
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][1] == 20 && fi[0][0] == 2 && fi[0][3] == ticks(g_gns - 12000000ull));
    // frame 21: reverse order (nq_worker finishes before dm_vi_frame_queued); ticket is preserved when dm_vi_frame_queued runs
    g_in++; g_done = g_in; g_pt[21] = (g_hns - 8000000ull) / 1e9;
    dm_vi_frame_ticket(21, g_in);
    g_count++;
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][1] == 20);   // frame 21 not yet recorded by dm_vi_frame_queued, skipped cleanly
    dm_vi_frame_queued(21, 0, 0, 0);
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][1] == 21 && fi[0][0] == 2 && fi[0][3] == ticks(g_gns - 8000000ull));
    // stale ticket for frame 13 (same slot 21 % 8) must not clobber frame 21
    dm_vi_frame_ticket(13, 13);
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][1] == 21 && fi[0][0] == 2 && fi[0][3] == ticks(g_gns - 8000000ull));
    // frame 22: discarded / unsubmitted present (ticket 0) still resolves as shown
    g_count++; dm_vi_frame_queued(22, 0, 0, 0); dm_vi_frame_ticket(22, 0);
    h_vi_FrameHist(fi, 10, NULL);
    assert(fi[0][1] == 22 && fi[0][0] == 2 && fi[0][3] == ticks(g_gns));

    puts("vi frame history: count query 8, newest first, shown once handed on with its own physical ticket, presented, first-seen or latched virtual-vsync tick, dropped frames shown, queued ones not");
    return 0;
}
