/* This file is part of mpv, licensed under LGPL 2.1 or later. */
#ifndef MP_METAL_HDR_DECODER_H
#define MP_METAL_HDR_DECODER_H

#include <stdbool.h>
#include <frame_engine.h>

struct mp_filter;
struct mp_image;

// Same source interpretation/timing for live frames and independent preparation.
bool mp_hdr_frame_descriptor(struct mp_image *image, fe_frame *frame,
                            double reference_white, double hlg_peak,
                            char *error, size_t capacity);

// Returned provider owns one user reference; caller releases it after create
// has synchronously copied/retained the vtable. Reader state never uses the
// live playback demuxer, decoder, filter graph, or VO.
fe_preparation_decoder_provider mp_hdr_preparation_decoder(struct mp_filter *f,
                            double reference_white, double hlg_peak);

#endif
