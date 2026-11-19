// Direct Joy-Con IR camera service. The host has no IR camera. The client may
// initialize and map the status manager, which lists disconnected cameras.
#include "hle.h"

enum { IRS_CAMERA_COUNT = 9, IRS_DEVICE_SIZE = 0xe30, IRS_UNCONNECTED = 2 };
enum { IRS_BAD_REQUEST = 0x1ce0a, IRS_BAD_HANDLE = 0x198cd, IRS_BAD_CONFIG = 0x19ccd, IRS_TRANSFER_FAILED = 0x19acd };
static const struct {
    struct { uint32_t status; uint8_t rest[IRS_DEVICE_SIZE - 4]; } device[IRS_CAMERA_COUNT];
    uint8_t aruid[0x50];
} irs_status = { .device = { [0 ... IRS_CAMERA_COUNT - 1] = { .status = IRS_UNCONNECTED } } };
_Static_assert(sizeof irs_status == 0x8000, "IRS StatusManager ABI");
static const hle_readonly_shmem irs_memory = { &irs_status, sizeof irs_status };

static uint64_t irs_u64(const uint8_t *p) {
    uint64_t value;
    memcpy(&value, p, sizeof value);
    return value;
}

static int irs_valid_handle(const uint8_t *handle) {
    return handle[0] < IRS_CAMERA_COUNT && handle[1] == 0;   // player index, device type
}

static Result irs_run_input(const sf_req *r, size_t size, size_t aruid_offset) {
    if (r->in_len < size) return IRS_BAD_REQUEST;
    uint64_t aruid = irs_u64(r->in + aruid_offset);
    if (aruid && aruid != DM_APPLET_RESOURCE_USER_ID) return 0x60au;   // the IPC sender's applet ID
    return irs_valid_handle(r->in) ? RES_OK : IRS_BAD_HANDLE;
}

static int irs_valid_exposure(const uint8_t *config) {
    uint64_t exposure = irs_u64(config);
    return exposure >= 7000 && exposure <= 600000 && config[8] <= 3 && config[9] >= 1 && config[9] <= 16;
}

static int irs_valid_window(const uint8_t *window, int min_width, int min_height) {
    int16_t rect[4];
    memcpy(rect, window, sizeof rect);
    return rect[0] >= 0 && rect[1] >= 0 && rect[2] >= min_width && rect[3] >= min_height &&
           rect[0] + rect[2] <= 320 && rect[1] + rect[3] <= 240;
}

static int irs_valid_crop(const uint8_t *config) {
    // Firmware sign-extends OriginalFormat for the comparison and uses the low five bits for the shifts.
    int original = config[0x14], trimmed = config[0x15];
    if (original >= 0x80) original -= 0x100;
    if (trimmed > 4 || original > trimmed) return 0;
    unsigned shift = (unsigned)original & 31;
    uint16_t corner[2];
    memcpy(corner, config + 0x16, sizeof corner);
    return corner[0] <= (320 >> shift) - (320 >> trimmed) &&
           corner[1] <= (240 >> shift) - (240 >> trimmed);
}

static Result irs_answer(sf_req *r) {
    switch (r->cmd) {
    case 302: case 303: case 319:   // Activate, deactivate, activate with function level
    case 305: case 318:            // Stop an image processor that is already stopped
        return RES_OK;
    // The retail handlers accept a valid request before a worker tries to start the camera.
    // With no camera the host leaves the shared status disconnected and no samples are produced.
    case 306: {                   // RunMomentProcessor
        Result rc = irs_run_input(r, 0x30, 8);
        if (rc) return rc;
        const uint8_t *config = r->in + 0x10;
        return irs_valid_exposure(config) && config[0x1c] <= 1 && irs_valid_window(config + 0x10, 8, 6)
               ? RES_OK : IRS_BAD_CONFIG;
    }
    case 307: {                   // RunClusteringProcessor: camera handle, padding, u64 ARUID, packed configuration
        if (r->in_len < 0x38) return IRS_BAD_REQUEST;
        if (!irs_valid_handle(r->in)) return IRS_BAD_HANDLE;
        uint8_t gain = r->in[0x10 + 9];   // packed configuration starts at +0x10, gain at +9 within it
        if (gain < 1 || gain > 16) return IRS_BAD_CONFIG;
        // Retail firmware 20.5.0 and 21.2.0 accept the initial configuration before a worker tries to start the camera.
        // The status page reports the camera disconnected, with no samples.
        return RES_OK;
    }
    case 308: case 316: {         // RunImageTransferProcessor / RunImageTransferExProcessor
        int extended = r->cmd == 316;
        Result rc = irs_run_input(r, extended ? 0x38 : 0x30, 8);
        if (rc) return rc;
        if (r->ncopy != 1 || r->nmove) return IRS_BAD_REQUEST;
        const uint8_t *config = r->in + 0x10;
        if (!irs_valid_exposure(config) || (extended ? !irs_valid_crop(config) : config[0x14] > 4))
            return IRS_BAD_CONFIG;
        uint64_t size = irs_u64(config + (extended ? 0x20 : 0x18));
        return hle_map_transfer_memory(r->copy[0], size, 0) ? RES_OK : IRS_TRANSFER_FAILED;
    }
    case 310: case 317:            // RunTeraPluginProcessor / RunIrLedProcessor: configuration is interpreted by the worker
        return irs_run_input(r, 0x18, 0x10);
    case 312: {                   // RunPointingProcessor
        Result rc = irs_run_input(r, 0x18, 0x10);
        if (rc) return rc;
        return irs_valid_window(r->in + 4, 1, 1) ? RES_OK : IRS_BAD_CONFIG;
    }
    case 304:
        r->out_copy = hle_new_readonly_shmem(&irs_memory);
        return RES_OK;
    case 311: {                    // GetNpadIrCameraHandle: player number in byte 0
        uint32_t npad;
        if (r->in_len < sizeof npad) return 0xe401u;
        memcpy(&npad, r->in, sizeof npad);
        if (npad == 0x20) npad = IRS_CAMERA_COUNT - 1;   // handheld
        else if (npad >= IRS_CAMERA_COUNT - 1) return 0xdecd;   // IRS: controller does not support an IR camera
        memcpy(r->out, &npad, sizeof npad);
        r->out_len = sizeof npad;
        return RES_OK;
    }
    case 309: case 313: case 314: case 315:
        return 0xdccdu;            // IRS module 205, description 110: camera unconnected
    default:
        LOG("[RED] irs: unimplemented command %u", r->cmd);
        return 0xf601u;
    }
}
SF_SERVICE("irs", irs_answer);
