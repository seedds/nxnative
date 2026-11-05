// Host CMIF services. X buffers are input Pointer descriptors; A/B are MapAlias.
// hle_sf owns returned child objects and calls close when their last session/domain reference goes away.
#pragma once
#include "hle.h"

typedef struct { void *data; size_t size; unsigned mode; } sf_buffer;
typedef struct {
    uint32_t command;
    const uint8_t *data;
    size_t size;
    unsigned nx, na, nb, nw, nc, ncopy, nmove, nobjects;
    int send_pid;
    uint64_t pid;   // authenticated guest process identity, never the client's placeholder PID
    uint32_t copies[15], moves[15]; // borrowed input handle values; handlers validate/retain what they use
    sf_buffer x[15], a[15], b[15];
} sf_request;

typedef struct sf_reply sf_reply;
typedef struct {
    Result (*request)(void *state, const sf_request *request, sf_reply *reply);
    void (*close)(void *state);
    void *state;
} sf_object;

struct sf_reply {
    uint8_t data[64];
    size_t size;   // CMIF output struct size, including its ABI padding
    uint32_t copy; // client-owned readable handle; the service retains its own handle if needed
    sf_object child;
};

void sf_close_handle(uint32_t handle);
sf_object sf_prepo_root(unsigned permissions);
