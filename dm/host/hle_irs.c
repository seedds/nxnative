// Direct Joy-Con IR camera service. The host has no IR camera. The client may
// initialize and map the status manager, which lists disconnected cameras.
#include "hle.h"

enum { IRS_CAMERA_COUNT = 9, IRS_DEVICE_SIZE = 0xe30, IRS_UNCONNECTED = 2 };
static const struct {
    struct { uint32_t status; uint8_t rest[IRS_DEVICE_SIZE - 4]; } device[IRS_CAMERA_COUNT];
    uint8_t aruid[0x50];
} irs_status = { .device = { [0 ... IRS_CAMERA_COUNT - 1] = { .status = IRS_UNCONNECTED } } };
_Static_assert(sizeof irs_status == 0x8000, "IRS StatusManager ABI");
static const hle_readonly_shmem irs_memory = { &irs_status, sizeof irs_status };

static Result irs_answer(sf_req *r) {
    switch (r->cmd) {
    case 302: case 303: case 319:   // Activate, deactivate, activate with function level
    case 305: case 318:            // Stop an image processor that is already stopped
        return RES_OK;
    case 307: {                   // RunClusteringProcessor: camera handle, padding, u64 ARUID, packed configuration
        if (r->in_len < 0x38) return 0x1ce0au;
        if (r->in[0] >= IRS_CAMERA_COUNT || r->in[1] != 0) return 0x198cdu;   // player index and camera type
        uint8_t gain = r->in[0x10 + 9];   // packed configuration starts at +0x10, gain at +9 within it
        if (gain < 1 || gain > 16) return 0x19ccdu;
        // Retail firmware 20.5.0 and 21.2.0 accept the initial configuration before a worker tries to start the camera.
        // The status page reports the camera disconnected, with no samples.
        return RES_OK;
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
    case 306: case 308: case 309: case 310: case 312:
    case 313: case 314: case 315: case 316: case 317:
        return 0xdccdu;            // IRS module 205, description 110: camera unconnected
    default:
        LOG("[RED] irs: unimplemented command %u", r->cmd);
        return 0xf601u;
    }
}
SF_SERVICE("irs", irs_answer);
