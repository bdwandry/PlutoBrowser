/*
 * PlutoBrowser — pdfimg.h
 * Minimal PDF raster-image extractor: renders an embedded image XObject
 * (FlateDecode RGB/Gray, or DCTDecode via the JPEG decoder). Vector-only PDFs
 * (no /Subtype /Image) return NULL — the benchmark corpus contains one such
 * file, which will report "unsupported" exactly like before.
 */
#ifndef PLUTO_PDFIMG_H
#define PLUTO_PDFIMG_H

#include <stddef.h>
#include <stdint.h>
#include "pd_api.h"

/* Extract + decode the first embedded raster image of a PDF to a dithered
 * 1-bit LCDBitmap (caller frees) or NULL when the PDF has no raster image. */
LCDBitmap *pdfimg_decode(const uint8_t *data, size_t len);

#endif /* PLUTO_PDFIMG_H */
