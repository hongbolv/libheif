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

#include "ivsr_scaling_plugin.h"
#include "ivsr.h"
#include "pixelimage.h"
#include "color-conversion/colorconversion.h"
#include "api_structs.h"
#include "error.h"

#include <vector>
#include <cstring>
#include <string>
#include <stdexcept>


// Callback for iVSR synchronous processing
static void ivsr_completion_callback(void* args)
{
  int* flag = static_cast<int*>(args);
  *flag = 1;
}


heif_error heif_image_scale_with_ivsr(const heif_image* input,
                                      heif_image** output,
                                      int target_width, int target_height,
                                      const heif_scaling_options* options)
{
  // --- Validate inputs ---
  if (!input || !output || !options || !options->ivsr_model_path) {
    return {heif_error_Usage_error, heif_suberror_Null_pointer_argument,
            "Invalid arguments for iVSR scaling"};
  }

  const auto& src_image = input->image;
  int src_width = static_cast<int>(src_image->get_width());
  int src_height = static_cast<int>(src_image->get_height());

  // EDSR fp32 defaults (hardcoded for first step)
  const int scale_factor = 2;          // EDSR produces 2x output per pass
  const float normalize_factor = 1.0f; // Enhanced EDSR uses normalize_factor=1.0
  const char* precision = "f32";       // fp32 inference

  // Determine number of passes: 1 for 2x, 2 for 4x
  // The dispatcher guarantees target is exactly 2x or 4x of source
  int num_passes = (target_width == src_width * 4) ? 2 : 1;

  // --- Step 1: Prepare input as RGB interleaved ---
  // The iVSR process API accepts a single char* input pointer, so the input
  // must be a single contiguous RGB interleaved buffer. For images already in
  // RGB interleaved format, use them directly (zero-copy). For all other
  // formats (YCbCr 4:2:0, 4:2:2, 4:4:4, monochrome, HDR, etc.), convert to
  // RGB interleaved using libheif's convert_colorspace().
  std::shared_ptr<HeifPixelImage> converted_image;
  size_t in_stride = 0;
  uint8_t* input_ptr = nullptr;

  heif_colorspace cs = src_image->get_colorspace();
  heif_chroma chroma = src_image->get_chroma_format();

  if (cs == heif_colorspace_RGB && chroma == heif_chroma_interleaved_RGB) {
    // RGB interleaved - zero-copy, no conversion needed
    input_ptr = src_image->get_plane(heif_channel_interleaved, &in_stride);
  }
  else {
    // Convert any non-RGB format to RGB interleaved using libheif.
    heif_color_conversion_options conversion_options;
    conversion_options.version = 1;
    conversion_options.preferred_chroma_downsampling_algorithm = heif_chroma_downsampling_nearest_neighbor;
    conversion_options.preferred_chroma_upsampling_algorithm = heif_chroma_upsampling_nearest_neighbor;
    conversion_options.only_use_preferred_chroma_algorithm = false;

    auto result = convert_colorspace(src_image,
                                     heif_colorspace_RGB,
                                     heif_chroma_interleaved_RGB,
                                     nclx_profile{},
                                     8,
                                     conversion_options,
                                     nullptr,
                                     nullptr);
    if (!result) {
      return {heif_error_Encoding_error, heif_suberror_Unspecified,
              "Failed to convert image to RGB for iVSR"};
    }
    converted_image = *result;
    input_ptr = converted_image->get_plane(heif_channel_interleaved, &in_stride);
  }

  if (!input_ptr) {
    return {heif_error_Encoding_error, heif_suberror_Unspecified,
            "Failed to get input plane for iVSR"};
  }

  // --- Step 2: Build iVSR configuration linked list ---
  // Note: the linked list `next` pointers must be set AFTER all configs are
  // added to the vector. Setting them inside push_back causes dangling pointers
  // when the vector reallocates its internal buffer.
  std::vector<ivsr_config_t> configs;
  auto add_config = [&configs](IVSRConfigKey key, const void* value) {
    ivsr_config_t cfg;
    cfg.key = key;
    cfg.value = value;
    cfg.next = nullptr;
    configs.push_back(cfg);
  };
  auto link_configs = [&configs]() {
    for (size_t i = 0; i + 1 < configs.size(); i++) {
      configs[i].next = &configs[i + 1];
    }
  };

  add_config(INPUT_MODEL, options->ivsr_model_path);

  const char* device = options->ivsr_device ? options->ivsr_device : "CPU";
  add_config(TARGET_DEVICE, device);

  add_config(PRECISION, precision);

  std::string input_res = std::to_string(src_width) + "," + std::to_string(src_height);
  add_config(INPUT_RES, input_res.c_str());

  // Configure tensor descriptors for Enhanced EDSR fp32 model.
  // Input is always RGB interleaved u8 (conversion done above).
  tensor_desc_t input_tensor_desc;
  std::memset(&input_tensor_desc, 0, sizeof(input_tensor_desc));
  std::strncpy(input_tensor_desc.precision, "u8", sizeof(input_tensor_desc.precision) - 1);
  std::strncpy(input_tensor_desc.layout, "NHWC", sizeof(input_tensor_desc.layout) - 1);
  std::strncpy(input_tensor_desc.tensor_color_format, "RGB", sizeof(input_tensor_desc.tensor_color_format) - 1);
  std::strncpy(input_tensor_desc.model_color_format, "RGB", sizeof(input_tensor_desc.model_color_format) - 1);
  input_tensor_desc.scale = normalize_factor;
  input_tensor_desc.dimension = 4;
  input_tensor_desc.shape[0] = 1;
  input_tensor_desc.shape[1] = static_cast<size_t>(src_height);
  input_tensor_desc.shape[2] = static_cast<size_t>(src_width);
  input_tensor_desc.shape[3] = 3;

  tensor_desc_t output_tensor_desc;
  std::memset(&output_tensor_desc, 0, sizeof(output_tensor_desc));
  std::strncpy(output_tensor_desc.precision, "u8", sizeof(output_tensor_desc.precision) - 1);
  std::strncpy(output_tensor_desc.layout, "NHWC", sizeof(output_tensor_desc.layout) - 1);
  std::strncpy(output_tensor_desc.tensor_color_format, "RGB", sizeof(output_tensor_desc.tensor_color_format) - 1);
  std::strncpy(output_tensor_desc.model_color_format, "RGB", sizeof(output_tensor_desc.model_color_format) - 1);
  output_tensor_desc.scale = normalize_factor;
  output_tensor_desc.dimension = 4;
  output_tensor_desc.shape[0] = 1;
  output_tensor_desc.shape[1] = static_cast<size_t>(src_height * scale_factor);
  output_tensor_desc.shape[2] = static_cast<size_t>(src_width * scale_factor);
  output_tensor_desc.shape[3] = 3;

  add_config(INPUT_TENSOR_DESC_SETTING, &input_tensor_desc);
  add_config(OUTPUT_TENSOR_DESC_SETTING, &output_tensor_desc);
  link_configs();

  // --- Step 3: Initialize iVSR ---
  ivsr_handle handle = nullptr;
  IVSRStatus status;

  try {
    status = ivsr_init(&configs[0], &handle);
  }
  catch (const std::exception& e) {
    return {heif_error_Plugin_loading_error, heif_suberror_Unspecified,
            e.what()};
  }
  if (status != OK) {
    return {heif_error_Plugin_loading_error, heif_suberror_Unspecified,
            "Failed to initialize iVSR engine"};
  }

  // --- Step 4: Pre-allocate output HeifPixelImage and run inference (zero-copy) ---
  int sr_width = src_width * scale_factor;
  int sr_height = src_height * scale_factor;

  auto out_img = std::make_shared<HeifPixelImage>();
  out_img->create(sr_width, sr_height, heif_colorspace_RGB, heif_chroma_interleaved_RGB);
  out_img->add_plane(heif_channel_interleaved, sr_width, sr_height, 8, nullptr);

  size_t out_stride = 0;
  uint8_t* output_ptr = out_img->get_plane(heif_channel_interleaved, &out_stride);

  int completion_flag = 0;
  ivsr_cb_t cb;
  cb.ivsr_cb = ivsr_completion_callback;
  cb.args = &completion_flag;

  // Pass input plane pointer and output plane pointer directly to iVSR.
  try {
    status = ivsr_process(handle, reinterpret_cast<char*>(input_ptr),
                          reinterpret_cast<char*>(output_ptr), &cb);
  }
  catch (const std::exception& e) {
    ivsr_deinit(handle);
    return {heif_error_Encoding_error, heif_suberror_Unspecified,
            e.what()};
  }
  if (status != OK) {
    ivsr_deinit(handle);
    return {heif_error_Encoding_error, heif_suberror_Unspecified,
            "iVSR processing failed"};
  }

  // --- Step 5: Handle 4x (second pass) ---
  if (num_passes == 2) {
    // For 4x, use the 2x output as input for a second 2x pass
    int pass2_src_w = sr_width;
    int pass2_src_h = sr_height;
    int pass2_out_w = pass2_src_w * scale_factor;
    int pass2_out_h = pass2_src_h * scale_factor;

    // Update tensor descriptors for second pass
    input_tensor_desc.shape[1] = static_cast<size_t>(pass2_src_h);
    input_tensor_desc.shape[2] = static_cast<size_t>(pass2_src_w);
    output_tensor_desc.shape[1] = static_cast<size_t>(pass2_out_h);
    output_tensor_desc.shape[2] = static_cast<size_t>(pass2_out_w);

    // Reinitialize iVSR for second pass dimensions
    ivsr_deinit(handle);
    handle = nullptr;

    // Rebuild config with updated input resolution
    configs.clear();
    add_config(INPUT_MODEL, options->ivsr_model_path);
    add_config(TARGET_DEVICE, device);
    add_config(PRECISION, precision);
    std::string pass2_res = std::to_string(pass2_src_w) + "," + std::to_string(pass2_src_h);
    add_config(INPUT_RES, pass2_res.c_str());
    // Second pass always takes RGB from first pass output
    std::strncpy(input_tensor_desc.tensor_color_format, "RGB", sizeof(input_tensor_desc.tensor_color_format) - 1);
    add_config(INPUT_TENSOR_DESC_SETTING, &input_tensor_desc);
    add_config(OUTPUT_TENSOR_DESC_SETTING, &output_tensor_desc);
    link_configs();

    try {
      status = ivsr_init(&configs[0], &handle);
    }
    catch (const std::exception& e) {
      return {heif_error_Plugin_loading_error, heif_suberror_Unspecified,
              e.what()};
    }
    if (status != OK) {
      return {heif_error_Plugin_loading_error, heif_suberror_Unspecified,
              "Failed to initialize iVSR engine for 4x second pass"};
    }

    // Allocate second-pass output
    auto out_img_4x = std::make_shared<HeifPixelImage>();
    out_img_4x->create(pass2_out_w, pass2_out_h, heif_colorspace_RGB, heif_chroma_interleaved_RGB);
    out_img_4x->add_plane(heif_channel_interleaved, pass2_out_w, pass2_out_h, 8, nullptr);

    size_t out_stride_4x = 0;
    uint8_t* output_ptr_4x = out_img_4x->get_plane(heif_channel_interleaved, &out_stride_4x);

    completion_flag = 0;
    try {
      status = ivsr_process(handle, reinterpret_cast<char*>(output_ptr),
                            reinterpret_cast<char*>(output_ptr_4x), &cb);
    }
    catch (const std::exception& e) {
      ivsr_deinit(handle);
      return {heif_error_Encoding_error, heif_suberror_Unspecified,
              e.what()};
    }
    if (status != OK) {
      ivsr_deinit(handle);
      return {heif_error_Encoding_error, heif_suberror_Unspecified,
              "iVSR 4x second pass processing failed"};
    }

    out_img = std::move(out_img_4x);
  }

  // --- Step 6: Return result ---
  *output = new heif_image;
  (*output)->image = std::move(out_img);

  // --- Step 7: Cleanup ---
  ivsr_deinit(handle);

  return {heif_error_Ok, heif_suberror_Unspecified, "Success"};
}
