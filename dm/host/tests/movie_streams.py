#!/usr/bin/env python3
"""Movie test streams without an encoder, for t_movie_mf (the Windows movie decoders, hle_movie_win32.c), written as a C header.

H.264: pictures whose decoded samples are known exactly. Intra pictures are made of I_PCM macroblocks, which carry their samples as they are (H.264 7.3.5: mb_type 25 in an
I slice, then the raw luma and chroma bytes); a P picture whose macroblocks are all skipped (P_Skip, no neighbour with motion: motion vector 0 into the first reference) is a
copy of the last reference picture; a B picture whose macroblocks are all skipped (B_Skip, spatial direct with no motion: reference index 0 in both lists) is the rounded
average of its two references, (L0 + L1 + 1) >> 1 sample by sample (8.4.2.3, default weighted prediction). CAVLC, the deblocking filter off in every slice header.
  - "base": Baseline, 96x64 coded and cropped to 96x60, its SPS and PPS in an avcC record and its access units with 4-byte length prefixes (as an MP4 demuxer gives
    them): I (IDR), P, I, P, a moving pattern; the VUI says BT.601, video range, no reordering.
  - "main": Main, 64x48, Annex B with the SPS and PPS before the first slice, decode order I0 I4 B2 I8 B6 (picture order counts), presentation order I0 B2 I4 B6 I8;
    the VUI says BT.709, video range, one picture of reordering.
AAC: raw frames (as an MP4 demuxer gives them) with an AudioSpecificConfig. A frame with a signal needs the spectral Huffman codebooks (ISO 14496-3, 4.A.1), which are not
written by hand here; these frames are silent: each channel's ics_info has max_sfb 0, so every spectral coefficient is zero and the decoded samples are all zero.
  - "aac2": AAC-LC 48000 Hz stereo (channel_pair_element), "aac1": AAC-LC 44100 Hz mono (single_channel_element).

Usage: movie_streams.py <out.h>
"""
import sys


class Bits:
    def __init__(self):
        self.b = []

    def u(self, n, v):
        for i in range(n - 1, -1, -1):
            self.b.append((v >> i) & 1)

    def ue(self, k):
        k1 = k + 1
        n = k1.bit_length()
        self.u(n - 1, 0)
        self.u(n, k1)

    def se(self, k):
        self.ue(2 * k - 1 if k > 0 else -2 * k)

    def align(self):
        while len(self.b) % 8:
            self.b.append(0)

    def trailing(self):   # rbsp_trailing_bits
        self.b.append(1)
        self.align()

    def raw(self, data):  # whole bytes, after an alignment
        assert len(self.b) % 8 == 0
        for x in data:
            self.u(8, x)

    def bytes(self):
        assert len(self.b) % 8 == 0
        return bytes(int(''.join(map(str, self.b[i:i + 8])), 2) for i in range(0, len(self.b), 8))


def nal(ref_idc, kind, rbsp):   # header byte and the RBSP with emulation prevention bytes (7.4.1)
    out = bytearray([ref_idc << 5 | kind])
    zeros = 0
    for x in rbsp:
        if zeros >= 2 and x <= 3:
            out.append(3)
            zeros = 0
        out.append(x)
        zeros = zeros + 1 if x == 0 else 0
    return bytes(out)


def sps(profile, wmb, hmb, crop_bottom, poc_type, matrix, reorder):
    b = Bits()
    b.u(8, profile); b.u(8, 0); b.u(8, 30)        # profile_idc, constraint flags, level_idc 3.0
    b.ue(0)                                       # seq_parameter_set_id
    b.ue(0)                                       # log2_max_frame_num_minus4: frame_num in 4 bits
    b.ue(poc_type)
    if poc_type == 0:
        b.ue(2)                                   # log2_max_pic_order_cnt_lsb_minus4: 6 bits
    b.ue(2)                                       # max_num_ref_frames
    b.u(1, 0)                                     # gaps_in_frame_num_value_allowed_flag
    b.ue(wmb - 1); b.ue(hmb - 1)
    b.u(1, 1)                                     # frame_mbs_only_flag
    b.u(1, 1)                                     # direct_8x8_inference_flag
    b.u(1, 1 if crop_bottom else 0)
    if crop_bottom:
        b.ue(0); b.ue(0); b.ue(0); b.ue(crop_bottom // 2)   # 4:2:0 crops in units of 2 lines
    b.u(1, 1)                                     # vui_parameters_present_flag
    b.u(1, 0); b.u(1, 0)                          # aspect_ratio_info, overscan_info
    b.u(1, 1); b.u(3, 5); b.u(1, 0); b.u(1, 1)    # video_signal_type: unspecified format, video range, colour description
    b.u(8, matrix); b.u(8, matrix); b.u(8, matrix)   # colour primaries, transfer, matrix (1 = BT.709, 6 = BT.601)
    b.u(1, 0); b.u(1, 0)                          # chroma_loc_info, timing_info
    b.u(1, 0); b.u(1, 0)                          # nal_hrd, vcl_hrd
    b.u(1, 0)                                     # pic_struct_present_flag
    b.u(1, 1)                                     # bitstream_restriction_flag
    b.u(1, 1); b.ue(0); b.ue(0); b.ue(16); b.ue(16)  # mvs over picture boundaries, no byte or bit bounds, mv lengths
    b.ue(reorder); b.ue(2)                        # max_num_reorder_frames, max_dec_frame_buffering
    b.trailing()
    return nal(3, 7, b.bytes())


def pps():
    b = Bits()
    b.ue(0); b.ue(0)                              # pic_parameter_set_id, seq_parameter_set_id
    b.u(1, 0); b.u(1, 0)                          # CAVLC, no bottom-field POC
    b.ue(0)                                       # num_slice_groups_minus1
    b.ue(0); b.ue(0)                              # one reference in each list
    b.u(1, 0); b.u(2, 0)                          # no weighted prediction: default bi-prediction average
    b.se(0); b.se(0); b.se(0)                     # pic_init_qp, pic_init_qs, chroma_qp_index_offset
    b.u(1, 1)                                     # deblocking_filter_control_present_flag (each slice turns it off)
    b.u(1, 0); b.u(1, 0)                          # constrained_intra_pred, redundant_pic_cnt_present
    b.trailing()
    return nal(3, 8, b.bytes())


I, P, B = 'I', 'P', 'B'


def slice_header(b, kind, idr, frame_num, idr_id, poc_lsb, ref):
    b.ue(0)                                       # first_mb_in_slice
    b.ue({I: 7, P: 5, B: 6}[kind])                # slice_type (every slice of the picture this type)
    b.ue(0)                                       # pic_parameter_set_id
    b.u(4, frame_num)
    if idr:
        b.ue(idr_id)
    if poc_lsb is not None:
        b.u(6, poc_lsb)
    if kind == B:
        b.u(1, 1)                                 # direct_spatial_mv_pred_flag
    if kind in (P, B):
        b.u(1, 0)                                 # num_ref_idx_active_override_flag
        b.u(1, 0)                                 # ref_pic_list_modification_flag_l0
        if kind == B:
            b.u(1, 0)                             # ref_pic_list_modification_flag_l1
    if ref:
        if idr:
            b.u(1, 0); b.u(1, 0)                  # no_output_of_prior_pics_flag, long_term_reference_flag
        else:
            b.u(1, 0)                             # adaptive_ref_pic_marking_mode_flag: sliding window
    b.se(0)                                       # slice_qp_delta
    b.ue(1)                                       # disable_deblocking_filter_idc: off


def pcm_slice(pic, w, h, idr, frame_num, idr_id, poc_lsb):
    y, cb, cr = pic
    b = Bits()
    slice_header(b, I, idr, frame_num, idr_id, poc_lsb, True)
    for my in range(h // 16):
        for mx in range(w // 16):
            b.ue(25)                              # mb_type I_PCM
            b.align()                             # pcm_alignment_zero_bit
            b.raw(y[(my * 16 + r) * w + mx * 16 + c] for r in range(16) for c in range(16))
            for plane in (cb, cr):
                b.raw(plane[(my * 8 + r) * (w // 2) + mx * 8 + c] for r in range(8) for c in range(8))
    b.trailing()
    return nal(3 if idr else 2, 5 if idr else 1, b.bytes())


def skip_slice(kind, w, h, frame_num, poc_lsb, ref):
    b = Bits()
    slice_header(b, kind, False, frame_num, 0, poc_lsb, ref)
    b.ue((w // 16) * (h // 16))                   # mb_skip_run: every macroblock
    b.trailing()
    return nal(2 if ref else 0, 1, b.bytes())


def pattern(w, h, t):   # a moving pattern: diagonal ramps that shift with t and a bright 16x16 square that moves right
    y = bytearray(16 + (x * 3 + yy * 2 + t * 9) % 200 for yy in range(h) for x in range(w))
    for yy in range(8, 24):
        for x in range(8 * t, 8 * t + 16):
            if x < w:
                y[yy * w + x] = 235
    cb = bytearray(64 + (x * 5 + t * 7) % 128 for yy in range(h // 2) for x in range(w // 2))
    cr = bytearray(64 + (yy * 6 + x + t * 11) % 128 for yy in range(h // 2) for x in range(w // 2))
    return bytes(y), bytes(cb), bytes(cr)


def avg(a, b):
    return tuple(bytes((p + q + 1) >> 1 for p, q in zip(pa, pb)) for pa, pb in zip(a, b))


def nv12(pic, w, h, crop_h):   # the expected picture as NV12 of the display size: Y rows, then Cb Cr interleaved
    y, cb, cr = pic
    out = bytearray(y[:w * crop_h])
    for r in range((crop_h + 1) // 2):
        for c in range(w // 2):
            out += bytes((cb[r * (w // 2) + c], cr[r * (w // 2) + c]))
    return bytes(out)


def annexb(*units):
    return b''.join(b'\0\0\0\1' + u for u in units)


def lenpre(*units):
    return b''.join(len(u).to_bytes(4, 'big') + u for u in units)


def streams():
    out = {}
    # base: Baseline, 96x64 cropped to 96x60, avcC, length-prefixed access units; POC type 2 (output order = decode order)
    w, h, ch = 96, 64, 60
    s, p = sps(66, w // 16, h // 16, h - ch, 2, 6, 0), pps()
    avcc = bytes([1, 66, 0, 30, 0xFF, 0xE1]) + len(s).to_bytes(2, 'big') + s + bytes([1]) + len(p).to_bytes(2, 'big') + p
    p0, p2 = pattern(w, h, 0), pattern(w, h, 2)
    aus = [(lenpre(pcm_slice(p0, w, h, True, 0, 0, None)), 0), (lenpre(skip_slice(P, w, h, 1, None, True)), 33333),
           (lenpre(pcm_slice(p2, w, h, False, 2, 0, None)), 66666), (lenpre(skip_slice(P, w, h, 3, None, True)), 100000)]
    pics = [(0, p0), (33333, p0), (66666, p2), (100000, p2)]
    out['base'] = dict(w=w, h=ch, coded_h=h, avcc=avcc, aus=aus, pics=[(t, nv12(x, w, h, ch)) for t, x in pics], matrix=601)
    # main: Main, 64x48, Annex B with in-band parameter sets; decode order I0 I4 B2 I8 B6, the B pictures the average of their references
    w, h = 64, 48
    s, p = sps(77, w // 16, h // 16, 0, 0, 1, 1), pps()
    q0, q2, q4 = pattern(w, h, 0), pattern(w, h, 2), pattern(w, h, 4)
    aus = [(annexb(s, p, pcm_slice(q0, w, h, True, 0, 0, 0)), 0), (annexb(pcm_slice(q2, w, h, False, 1, 0, 4)), 66666),
           (annexb(skip_slice(B, w, h, 2, 2, False)), 33333), (annexb(pcm_slice(q4, w, h, False, 2, 0, 8)), 133333),
           (annexb(skip_slice(B, w, h, 3, 6, False)), 100000)]
    pics = [(0, q0), (33333, avg(q0, q2)), (66666, q2), (100000, avg(q2, q4)), (133333, q4)]
    out['main'] = dict(w=w, h=h, coded_h=h, avcc=b'', aus=aus, pics=[(t, nv12(x, w, h, h)) for t, x in pics], matrix=709)
    return out


def aac_frame(channels):   # one silent raw_data_block (ISO 14496-3 4.4.2.1)
    b = Bits()
    b.u(3, 1 if channels == 2 else 0)             # id_syn_ele: CPE or SCE
    b.u(4, 0)                                     # element_instance_tag
    if channels == 2:
        b.u(1, 0)                                 # common_window
    for _ in range(channels):                     # individual_channel_stream
        b.u(8, 100)                               # global_gain
        b.u(1, 0); b.u(2, 0); b.u(1, 0)           # ics_reserved_bit, ONLY_LONG_SEQUENCE, sine window
        b.u(6, 0)                                 # max_sfb 0: no sections, no scale factors, no spectral data
        b.u(1, 0)                                 # predictor_data_present
        b.u(1, 0); b.u(1, 0); b.u(1, 0)           # pulse, tns, gain control: none
    b.u(3, 7)                                     # ID_END
    b.align()
    return b.bytes()


def asc(rate_index, channels):   # AudioSpecificConfig: AAC LC, sampling frequency index, channel configuration, GASpecificConfig 0 0 0
    b = Bits()
    b.u(5, 2); b.u(4, rate_index); b.u(4, channels); b.u(3, 0)
    return b.bytes()


def c_bytes(name, data):
    rows = [', '.join('0x%02x' % x for x in data[i:i + 24]) for i in range(0, len(data), 24)]
    return 'static const uint8_t %s[%d] = {\n    %s\n};\n' % (name, max(len(data), 1), ',\n    '.join(rows) if rows else '0')


def main():
    out = ['// Written by dm/host/tests/movie_streams.py (see its docstring): H.264 and AAC test streams and the pictures and samples they decode to. Do not edit.',
           '#pragma once', '#include <stdint.h>', '',
           'typedef struct { const uint8_t *p; int n; int64_t pts_us; } mst_au;',
           'typedef struct { int64_t pts_us; const uint8_t *nv12; } mst_pic;',
           'typedef struct { int w, h, coded_h, matrix; const uint8_t *avcc; int avcc_n; const mst_au *au; int nau; const mst_pic *pic; int npic; } mst_h264;',
           'typedef struct { int rate, channels; const uint8_t *asc; int asc_n; const uint8_t *frame; int frame_n; } mst_aac;', '']
    for name, s in streams().items():
        out.append(c_bytes('mst_%s_avcc' % name, s['avcc']))
        for i, (data, _) in enumerate(s['aus']):
            out.append(c_bytes('mst_%s_au%d' % (name, i), data))
        out.append('static const mst_au mst_%s_aus[] = {%s};' % (name, ', '.join('{mst_%s_au%d, %d, %d}' % (name, i, len(d), t) for i, (d, t) in enumerate(s['aus']))))
        for i, (_, pic) in enumerate(s['pics']):
            out.append(c_bytes('mst_%s_pic%d' % (name, i), pic))
        out.append('static const mst_pic mst_%s_pics[] = {%s};' % (name, ', '.join('{%d, mst_%s_pic%d}' % (t, name, i) for i, (t, _) in enumerate(s['pics']))))
        out.append('static const mst_h264 mst_%s = {%d, %d, %d, %d, mst_%s_avcc, %d, mst_%s_aus, %d, mst_%s_pics, %d};\n' % (
            name, s['w'], s['h'], s['coded_h'], s['matrix'], name, len(s['avcc']), name, len(s['aus']), name, len(s['pics'])))
    for name, rate, idx, ch in (('aac2', 48000, 3, 2), ('aac1', 44100, 4, 1)):
        a, f = asc(idx, ch), aac_frame(ch)
        out.append(c_bytes('mst_%s_asc' % name, a))
        out.append(c_bytes('mst_%s_frame' % name, f))
        out.append('static const mst_aac mst_%s = {%d, %d, mst_%s_asc, %d, mst_%s_frame, %d};\n' % (name, rate, ch, name, len(a), name, len(f)))
    with open(sys.argv[1], 'w') as fh:
        fh.write('\n'.join(out))


if __name__ == '__main__':
    main()
