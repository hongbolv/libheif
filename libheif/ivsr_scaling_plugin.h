/*
 * HEIF codec.
 * Copyright (c) 2024 Intel Corporation
 *
 * This file is part of libheif.
 *
 * libheif is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libheif is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libheif.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBHEIF_IVSR_SCALING_PLUGIN_H
#define LIBHEIF_IVSR_SCALING_PLUGIN_H

#include "libheif/heif_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Scale an image using iVSR AI super resolution (Enhanced EDSR fp32).
 *
 * This function is called internally by heif_image_scale_image() when
 * the scaling options specify super resolution algorithm and the target
 * dimensions are exactly 2x or 4x of the source.
 *
 * For 4x scaling, two passes of 2x SR are applied internally.
 *
 * @param input       Source image
 * @param output      Pointer to receive the scaled output image
 * @param width       Target width (must be exactly 2x or 4x of source)
 * @param height      Target height (must be exactly 2x or 4x of source)
 * @param options     Scaling options with iVSR configuration
 * @return heif_error indicating success or failure
 */
heif_error heif_image_scale_with_ivsr(const heif_image* input,
                                      heif_image** output,
                                      int width, int height,
                                      const heif_scaling_options* options);

#ifdef __cplusplus
}
#endif

#endif // LIBHEIF_IVSR_SCALING_PLUGIN_H
