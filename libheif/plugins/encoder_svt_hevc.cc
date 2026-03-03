/*
 * HEIF codec.
 * Copyright (c) 2024 libheif contributors
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

#include "libheif/heif.h"
#include "libheif/heif_plugin.h"
#include "encoder_svt_hevc.h"
#include <memory>
#include <string>
#include <cstring>
#include <cstdio>
#include <cassert>
#include <deque>
#include <vector>

#include <EbApi.h>


// ============================================================
//                    Error message constants
// ============================================================

static const char* kError_unsupported_bit_depth =
    "Bit depth not supported by SVT-HEVC (only 8 and 10 bit supported)";
static const char* kError_encoder_init_failed =
    "SVT-HEVC encoder initialization failed";
static const char* kError_encode_failed =
    "SVT-HEVC encoding failed";


// ============================================================
//                    Encoder state
// ============================================================

struct encoder_struct_svt_hevc
{
  // SVT-HEVC encoder component
  EB_COMPONENTTYPE* svt_encoder = nullptr;
  EB_H265_ENC_CONFIGURATION enc_params = {};
  bool encoder_initialized = false;

  // --- output queue ---

  struct Packet
  {
    std::vector<uint8_t> data;
    uintptr_t frameNr = 0;
  };

  std::deque<Packet> output_packets;
  std::vector<uint8_t> active_output_nal;

  // --- encoder parameters ---

  int quality = 50;      // 0-100, mapped to QP
  int enc_preset = 7;    // 0 (highest quality) - 11 (fastest)
  int qp = 32;           // direct QP value
  int threads = 0;       // 0 = auto
  int log_level = 0;     // 0 = no logging
  int hierarchical_levels = 0; // 0 = suitable for single frame, 3 = default for sequences
  int intra_period = -2; // -2 = auto

  std::string last_error_message;
};


// ============================================================
//                    Parameter definitions
// ============================================================

static const char* kParam_preset = "preset";
static const char* kParam_qp = "qp";
static const char* kParam_threads = "threads";

static const int SVT_HEVC_PLUGIN_PRIORITY = 90;

#define MAX_PLUGIN_NAME_LENGTH 80
static char plugin_name[MAX_PLUGIN_NAME_LENGTH];

#define MAX_NPARAMETERS 10
static heif_encoder_parameter svt_hevc_encoder_params[MAX_NPARAMETERS];
static const heif_encoder_parameter* svt_hevc_encoder_parameter_ptrs[MAX_NPARAMETERS + 1];


// ============================================================
//                    Plugin name
// ============================================================

static const char* svt_hevc_plugin_name()
{
  snprintf(plugin_name, MAX_PLUGIN_NAME_LENGTH, "SVT-HEVC encoder");
  return plugin_name;
}


// ============================================================
//                    Parameter initialization
// ============================================================

static void svt_hevc_init_parameters()
{
  heif_encoder_parameter* p = svt_hevc_encoder_params;
  const heif_encoder_parameter** d = svt_hevc_encoder_parameter_ptrs;
  int i = 0;

  // quality parameter (0-100)
  assert(i < MAX_NPARAMETERS);
  p->version = 2;
  p->name = heif_encoder_parameter_name_quality;
  p->type = heif_encoder_parameter_type_integer;
  p->integer.default_value = 50;
  p->has_default = true;
  p->integer.have_minimum_maximum = true;
  p->integer.minimum = 0;
  p->integer.maximum = 100;
  p->integer.valid_values = NULL;
  p->integer.num_valid_values = 0;
  d[i++] = p++;

  // lossless parameter (SVT-HEVC does not support lossless, always false)
  assert(i < MAX_NPARAMETERS);
  p->version = 2;
  p->name = heif_encoder_parameter_name_lossless;
  p->type = heif_encoder_parameter_type_boolean;
  p->boolean.default_value = false;
  p->has_default = true;
  d[i++] = p++;

  // preset parameter (0-11)
  assert(i < MAX_NPARAMETERS);
  p->version = 2;
  p->name = kParam_preset;
  p->type = heif_encoder_parameter_type_integer;
  p->integer.default_value = 7;
  p->has_default = true;
  p->integer.have_minimum_maximum = true;
  p->integer.minimum = 0;
  p->integer.maximum = 11;
  p->integer.valid_values = NULL;
  p->integer.num_valid_values = 0;
  d[i++] = p++;

  // qp parameter (0-51)
  assert(i < MAX_NPARAMETERS);
  p->version = 2;
  p->name = kParam_qp;
  p->type = heif_encoder_parameter_type_integer;
  p->integer.default_value = 32;
  p->has_default = true;
  p->integer.have_minimum_maximum = true;
  p->integer.minimum = 0;
  p->integer.maximum = 51;
  p->integer.valid_values = NULL;
  p->integer.num_valid_values = 0;
  d[i++] = p++;

  // threads parameter
  assert(i < MAX_NPARAMETERS);
  p->version = 2;
  p->name = kParam_threads;
  p->type = heif_encoder_parameter_type_integer;
  p->integer.default_value = 0;
  p->has_default = true;
  p->integer.have_minimum_maximum = true;
  p->integer.minimum = 0;
  p->integer.maximum = 256;
  p->integer.valid_values = NULL;
  p->integer.num_valid_values = 0;
  d[i++] = p++;

  d[i++] = nullptr;
}


// ============================================================
//                    Plugin lifecycle
// ============================================================

static void svt_hevc_init_plugin()
{
  svt_hevc_init_parameters();
}


static void svt_hevc_cleanup_plugin()
{
}

static void svt_hevc_set_default_parameters(void* encoder);

static heif_error svt_hevc_new_encoder(void** enc)
{
  auto* encoder = new encoder_struct_svt_hevc();
  *enc = encoder;

  svt_hevc_set_default_parameters(encoder);

  return heif_error_ok;
}


static void svt_hevc_free_encoder(void* encoder_raw)
{
  auto* encoder = (encoder_struct_svt_hevc*) encoder_raw;

  if (encoder->encoder_initialized) {
    EbDeinitEncoder(encoder->svt_encoder);
    encoder->encoder_initialized = false;
  }

  if (encoder->svt_encoder) {
    EbDeinitHandle(encoder->svt_encoder);
    encoder->svt_encoder = nullptr;
  }

  delete encoder;
}


// ============================================================
//                    Parameter setter/getter
// ============================================================

static heif_error svt_hevc_set_parameter_quality(void* encoder_raw, int quality)
{
  auto* encoder = (encoder_struct_svt_hevc*) encoder_raw;

  if (quality < 0 || quality > 100) {
    return heif_error_invalid_parameter_value;
  }

  encoder->quality = quality;

  // Map quality to QP: quality=0 -> qp=MAX_QP, quality=MAX_QUALITY -> qp=0
  static const int MAX_QP = 51;
  static const int MAX_QUALITY = 100;
  encoder->qp = (int)((MAX_QUALITY - quality) * MAX_QP / (double)MAX_QUALITY + 0.5);

  return heif_error_ok;
}


static heif_error svt_hevc_get_parameter_quality(void* encoder_raw, int* quality)
{
  auto* encoder = (encoder_struct_svt_hevc*) encoder_raw;
  *quality = encoder->quality;
  return heif_error_ok;
}


static heif_error svt_hevc_set_parameter_lossless(void* encoder_raw, int enable)
{
  // SVT-HEVC does not support lossless encoding
  if (enable) {
    return heif_error_unsupported_parameter;
  }
  return heif_error_ok;
}


static heif_error svt_hevc_get_parameter_lossless(void* encoder_raw, int* enable)
{
  *enable = 0; // always lossy
  return heif_error_ok;
}


static heif_error svt_hevc_set_parameter_logging_level(void* encoder_raw, int logging)
{
  auto* encoder = (encoder_struct_svt_hevc*) encoder_raw;

  if (logging < 0 || logging > 4) {
    return heif_error_invalid_parameter_value;
  }

  encoder->log_level = logging;
  return heif_error_ok;
}


static heif_error svt_hevc_get_parameter_logging_level(void* encoder_raw, int* loglevel)
{
  auto* encoder = (encoder_struct_svt_hevc*) encoder_raw;
  *loglevel = encoder->log_level;
  return heif_error_ok;
}


static heif_error svt_hevc_set_parameter_integer(void* encoder_raw, const char* name, int value)
{
  auto* encoder = (encoder_struct_svt_hevc*) encoder_raw;

  if (strcmp(name, heif_encoder_parameter_name_quality) == 0) {
    return svt_hevc_set_parameter_quality(encoder_raw, value);
  }
  else if (strcmp(name, heif_encoder_parameter_name_lossless) == 0) {
    return svt_hevc_set_parameter_lossless(encoder_raw, value);
  }
  else if (strcmp(name, kParam_preset) == 0) {
    if (value < 0 || value > 11) {
      return heif_error_invalid_parameter_value;
    }
    encoder->enc_preset = value;
    return heif_error_ok;
  }
  else if (strcmp(name, kParam_qp) == 0) {
    if (value < 0 || value > 51) {
      return heif_error_invalid_parameter_value;
    }
    encoder->qp = value;
    return heif_error_ok;
  }
  else if (strcmp(name, kParam_threads) == 0) {
    if (value < 0 || value > 256) {
      return heif_error_invalid_parameter_value;
    }
    encoder->threads = value;
    return heif_error_ok;
  }

  return heif_error_unsupported_parameter;
}


static heif_error svt_hevc_get_parameter_integer(void* encoder_raw, const char* name, int* value)
{
  auto* encoder = (encoder_struct_svt_hevc*) encoder_raw;

  if (strcmp(name, heif_encoder_parameter_name_quality) == 0) {
    return svt_hevc_get_parameter_quality(encoder_raw, value);
  }
  else if (strcmp(name, heif_encoder_parameter_name_lossless) == 0) {
    return svt_hevc_get_parameter_lossless(encoder_raw, value);
  }
  else if (strcmp(name, kParam_preset) == 0) {
    *value = encoder->enc_preset;
    return heif_error_ok;
  }
  else if (strcmp(name, kParam_qp) == 0) {
    *value = encoder->qp;
    return heif_error_ok;
  }
  else if (strcmp(name, kParam_threads) == 0) {
    *value = encoder->threads;
    return heif_error_ok;
  }

  return heif_error_unsupported_parameter;
}


static heif_error svt_hevc_set_parameter_boolean(void* encoder, const char* name, int value)
{
  if (strcmp(name, heif_encoder_parameter_name_lossless) == 0) {
    return svt_hevc_set_parameter_lossless(encoder, value);
  }
  return heif_error_unsupported_parameter;
}


static heif_error svt_hevc_get_parameter_boolean(void* encoder, const char* name, int* value)
{
  if (strcmp(name, heif_encoder_parameter_name_lossless) == 0) {
    return svt_hevc_get_parameter_lossless(encoder, value);
  }
  return heif_error_unsupported_parameter;
}


static heif_error svt_hevc_set_parameter_string(void* encoder_raw, const char* name,
                                                 const char* value)
{
  return heif_error_unsupported_parameter;
}


static heif_error svt_hevc_get_parameter_string(void* encoder_raw, const char* name,
                                                 char* value, int value_size)
{
  return heif_error_unsupported_parameter;
}


static const heif_encoder_parameter** svt_hevc_list_parameters(void* encoder)
{
  return svt_hevc_encoder_parameter_ptrs;
}


static void svt_hevc_set_default_parameters(void* encoder)
{
  for (const heif_encoder_parameter** p = svt_hevc_encoder_parameter_ptrs; *p; p++) {
    const heif_encoder_parameter* param = *p;

    if (param->has_default) {
      switch (param->type) {
        case heif_encoder_parameter_type_integer:
          svt_hevc_set_parameter_integer(encoder, param->name, param->integer.default_value);
          break;
        case heif_encoder_parameter_type_boolean:
          svt_hevc_set_parameter_boolean(encoder, param->name, param->boolean.default_value);
          break;
        case heif_encoder_parameter_type_string:
          svt_hevc_set_parameter_string(encoder, param->name, param->string.default_value);
          break;
      }
    }
  }
}


// ============================================================
//                    Colorspace query
// ============================================================

static void svt_hevc_query_input_colorspace(heif_colorspace* colorspace, heif_chroma* chroma)
{
  if (*colorspace == heif_colorspace_monochrome) {
    *colorspace = heif_colorspace_monochrome;
    *chroma = heif_chroma_monochrome;
  }
  else {
    *colorspace = heif_colorspace_YCbCr;
    *chroma = heif_chroma_420;
  }
}


static void svt_hevc_query_input_colorspace2(void* encoder_raw,
                                              heif_colorspace* colorspace,
                                              heif_chroma* chroma)
{
  if (*colorspace == heif_colorspace_monochrome) {
    *colorspace = heif_colorspace_monochrome;
    *chroma = heif_chroma_monochrome;
  }
  else {
    *colorspace = heif_colorspace_YCbCr;
    *chroma = heif_chroma_420;
  }
}


// ============================================================
//              NAL unit parsing helper
// ============================================================

// Extract NAL units from SVT-HEVC Annex B bitstream output
static void parse_nal_units_from_bitstream(
    const uint8_t* bitstream, uint32_t bitstream_size,
    std::deque<encoder_struct_svt_hevc::Packet>& output_packets,
    uintptr_t frameNr)
{
  uint32_t pos = 0;

  while (pos < bitstream_size) {
    // Find start code (0x000001 or 0x00000001)
    uint32_t nal_start = pos;
    bool found = false;

    while (pos + 2 < bitstream_size) {
      if (bitstream[pos] == 0 && bitstream[pos + 1] == 0) {
        if (bitstream[pos + 2] == 1) {
          found = true;
          break;
        }
        if (pos + 3 < bitstream_size && bitstream[pos + 2] == 0 && bitstream[pos + 3] == 1) {
          found = true;
          break;
        }
      }
      pos++;
    }

    if (!found && nal_start == 0) {
      // No start code found, treat the entire buffer as one NAL
      encoder_struct_svt_hevc::Packet pkt;
      pkt.data.assign(bitstream, bitstream + bitstream_size);
      pkt.frameNr = frameNr;
      output_packets.push_back(std::move(pkt));
      return;
    }

    if (!found) {
      break;
    }

    // Skip past the start code
    uint32_t nal_data_start = pos;
    if (bitstream[pos + 2] == 1) {
      nal_data_start = pos + 3;
    }
    else {
      nal_data_start = pos + 4;
    }

    // Find next start code to determine end of current NAL
    uint32_t nal_end = nal_data_start;
    while (nal_end + 2 < bitstream_size) {
      if (bitstream[nal_end] == 0 && bitstream[nal_end + 1] == 0 &&
          (bitstream[nal_end + 2] == 1 ||
           (nal_end + 3 < bitstream_size && bitstream[nal_end + 2] == 0 && bitstream[nal_end + 3] == 1))) {
        break;
      }
      nal_end++;
    }

    if (nal_end + 2 >= bitstream_size) {
      nal_end = bitstream_size;
    }

    // Extract NAL data (without start code)
    if (nal_end > nal_data_start) {
      uint32_t nal_size = nal_end - nal_data_start;
      const uint8_t* nal_data = bitstream + nal_data_start;

      // Skip "unregistered user data SEI" (similar to x265 plugin handling)
      // HEVC NAL type for prefix SEI = 39 (0x4e >> 1), SEI payload type 5 = unregistered
      static const uint8_t NAL_TYPE_PREFIX_SEI_BYTE = 0x4e;
      static const uint8_t SEI_PAYLOAD_UNREGISTERED_USER_DATA = 5;
      if (nal_size >= 3 && nal_data[0] == NAL_TYPE_PREFIX_SEI_BYTE
          && nal_data[2] == SEI_PAYLOAD_UNREGISTERED_USER_DATA) {
        // Skip unregistered user data SEI
      }
      else {
        encoder_struct_svt_hevc::Packet pkt;
        pkt.data.assign(nal_data, nal_data + nal_size);
        pkt.frameNr = frameNr;
        output_packets.push_back(std::move(pkt));
      }
    }

    pos = nal_end;
  }
}


// ============================================================
//                    Encoder initialization
// ============================================================

static heif_error svt_hevc_init_encoder(encoder_struct_svt_hevc* encoder,
                                         const heif_image* image,
                                         bool image_sequence,
                                         uint32_t framerate_num,
                                         uint32_t framerate_denom)
{
  int width = heif_image_get_width(image, heif_channel_Y);
  int height = heif_image_get_height(image, heif_channel_Y);
  int bit_depth = heif_image_get_bits_per_pixel_range(image, heif_channel_Y);

  if (bit_depth != 8 && bit_depth != 10) {
    return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Unsupported_bit_depth,
        kError_unsupported_bit_depth
    };
  }

  // Clean up any previous encoder instance
  if (encoder->encoder_initialized) {
    EbDeinitEncoder(encoder->svt_encoder);
    encoder->encoder_initialized = false;
  }
  if (encoder->svt_encoder) {
    EbDeinitHandle(encoder->svt_encoder);
    encoder->svt_encoder = nullptr;
  }

  // Step 1: Create encoder handle
  EB_ERRORTYPE eb_err = EbInitHandle(
      &encoder->svt_encoder, nullptr, &encoder->enc_params);

  if (eb_err != EB_ErrorNone) {
    return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Encoder_initialization,
        kError_encoder_init_failed
    };
  }

  // Step 2: Configure encoding parameters
  EB_H265_ENC_CONFIGURATION* config = &encoder->enc_params;

  config->sourceWidth = width;
  config->sourceHeight = height;
  config->encoderBitDepth = bit_depth;
  config->encoderColorFormat = EB_YUV420;
  config->encMode = static_cast<uint8_t>(encoder->enc_preset);
  config->qp = encoder->qp;
  config->rateControlMode = 0;  // CQP mode
  config->threadCount = encoder->threads;

  if (image_sequence) {
    config->intraPeriodLength = encoder->intra_period;
    config->hierarchicalLevels = 3;
    config->predStructure = 2; // Random Access
    config->framesToBeEncoded = 0; // unknown frame count

    if (framerate_denom > 0) {
      config->frameRateNumerator = framerate_num;
      config->frameRateDenominator = framerate_denom;
    }
    else {
      config->frameRate = 30;
    }
  }
  else {
    // Still image mode
    config->intraPeriodLength = -1;
    config->hierarchicalLevels = 0;
    config->predStructure = 2;
    config->framesToBeEncoded = 1;
    config->frameRate = 1;
    config->frameRateNumerator = 0;
    config->frameRateDenominator = 0;
  }

  // Profile based on bit depth
  if (bit_depth == 8) {
    config->profile = 1;  // Main
  }
  else {
    config->profile = 2;  // Main 10
  }

  config->tier = 0;   // Main tier
  config->level = 0;  // Auto

  // Generate VPS/SPS/PPS
  config->codeVpsSpsPps = 1;
  config->codeEosNal = 0;

  // HDR support
  heif_color_profile_nclx* nclx = nullptr;
  heif_error err = heif_image_get_nclx_color_profile(image, &nclx);
  if (err.code == heif_error_Ok && nclx) {
    if (nclx->transfer_characteristics == 16) {  // PQ
      config->highDynamicRangeInput = 1;
      config->videoUsabilityInfo = 1;
    }

    config->maxCLL = 0;
    config->maxFALL = 0;

    heif_nclx_color_profile_free(nclx);
  }

  // Step 2: Set parameters
  eb_err = EbH265EncSetParameter(encoder->svt_encoder, config);
  if (eb_err != EB_ErrorNone) {
    EbDeinitHandle(encoder->svt_encoder);
    encoder->svt_encoder = nullptr;
    return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Encoder_initialization,
        kError_encoder_init_failed
    };
  }

  // Step 3: Initialize encoder
  eb_err = EbInitEncoder(encoder->svt_encoder);
  if (eb_err != EB_ErrorNone) {
    EbDeinitHandle(encoder->svt_encoder);
    encoder->svt_encoder = nullptr;
    return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Encoder_initialization,
        kError_encoder_init_failed
    };
  }

  encoder->encoder_initialized = true;

  // Get stream header (VPS/SPS/PPS)
  EB_BUFFERHEADERTYPE* stream_header = nullptr;
  eb_err = EbH265EncStreamHeader(encoder->svt_encoder, &stream_header);
  if (eb_err == EB_ErrorNone && stream_header && stream_header->nFilledLen > 0) {
    parse_nal_units_from_bitstream(
        stream_header->pBuffer, stream_header->nFilledLen,
        encoder->output_packets, 0);
    EbH265EncReleaseStreamHeader(stream_header);
  }

  return heif_error_ok;
}


// ============================================================
//                    Frame encoding
// ============================================================

static heif_error svt_hevc_encode_frame(encoder_struct_svt_hevc* encoder,
                                         const heif_image* image,
                                         uintptr_t frame_nr,
                                         bool is_last_frame)
{
  int bit_depth = heif_image_get_bits_per_pixel_range(image, heif_channel_Y);
  bool isGreyscale = (heif_image_get_colorspace(image) == heif_colorspace_monochrome);

  // Prepare input buffer
  EB_BUFFERHEADERTYPE input_buffer;
  memset(&input_buffer, 0, sizeof(input_buffer));
  input_buffer.nSize = sizeof(EB_BUFFERHEADERTYPE);

  EB_H265_ENC_INPUT input_pic;
  memset(&input_pic, 0, sizeof(input_pic));

  int y_stride, cb_stride, cr_stride;

  // Get image planes
  input_pic.luma = (uint8_t*) heif_image_get_plane_readonly(image, heif_channel_Y, &y_stride);
  input_pic.yStride = y_stride / (bit_depth > 8 ? 2 : 1);

  if (!isGreyscale) {
    input_pic.cb = (uint8_t*) heif_image_get_plane_readonly(image, heif_channel_Cb, &cb_stride);
    input_pic.cr = (uint8_t*) heif_image_get_plane_readonly(image, heif_channel_Cr, &cr_stride);
    input_pic.cbStride = cb_stride / (bit_depth > 8 ? 2 : 1);
    input_pic.crStride = cr_stride / (bit_depth > 8 ? 2 : 1);
  }

  input_buffer.pBuffer = (uint8_t*) &input_pic;
  input_buffer.nAllocLen = sizeof(EB_H265_ENC_INPUT);
  input_buffer.nFilledLen = sizeof(EB_H265_ENC_INPUT);
  // EOS is sent via a separate empty buffer (matching SVT-HEVC reference app)
  input_buffer.nFlags = 0;
  input_buffer.pts = frame_nr;
  input_buffer.pAppPrivate = reinterpret_cast<void*>(frame_nr);
  // EB_INVALID_PICTURE: let encoder decide slice type
  input_buffer.sliceType = EB_INVALID_PICTURE;

  // Send picture
  EB_ERRORTYPE eb_err = EbH265EncSendPicture(encoder->svt_encoder, &input_buffer);
  if (eb_err != EB_ErrorNone) {
    return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Unspecified,
        kError_encode_failed
    };
  }

  // Non-blocking: retrieve available encoded results
  EB_BUFFERHEADERTYPE* output_buffer = nullptr;

  for (;;) {
    eb_err = EbH265GetPacket(encoder->svt_encoder, &output_buffer, 0);

    if (eb_err == EB_NoErrorEmptyQueue) {
      break;
    }

    if (eb_err != EB_ErrorNone) {
      break;
    }

    if (output_buffer && output_buffer->nFilledLen > 0) {
      uintptr_t out_frame_nr = reinterpret_cast<uintptr_t>(output_buffer->pAppPrivate);
      parse_nal_units_from_bitstream(
          output_buffer->pBuffer, output_buffer->nFilledLen,
          encoder->output_packets, out_frame_nr);
    }

    if (output_buffer) {
      EbH265ReleaseOutBuffer(&output_buffer);
    }
  }

  return heif_error_ok;
}


// Helper: Send EOS and flush remaining encoded data
static heif_error svt_hevc_flush_encoder(encoder_struct_svt_hevc* encoder)
{
  // Send a separate EOS buffer (matching SVT-HEVC reference app ProcessInputBuffer)
  EB_BUFFERHEADERTYPE eos_buffer;
  memset(&eos_buffer, 0, sizeof(eos_buffer));
  eos_buffer.nSize = sizeof(EB_BUFFERHEADERTYPE);
  eos_buffer.nFlags = EB_BUFFERFLAG_EOS;
  eos_buffer.pBuffer = nullptr;
  eos_buffer.nFilledLen = 0;
  eos_buffer.nAllocLen = 0;
  eos_buffer.pAppPrivate = nullptr;
  eos_buffer.sliceType = EB_INVALID_PICTURE;

  EB_ERRORTYPE eos_err = EbH265EncSendPicture(encoder->svt_encoder, &eos_buffer);
  if (eos_err != EB_ErrorNone) {
    return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Unspecified,
        kError_encode_failed
    };
  }

  // Blocking: retrieve all remaining encoded data (picSendDone=1)
  EB_BUFFERHEADERTYPE* output_buffer = nullptr;
  for (;;) {
    EB_ERRORTYPE eb_err = EbH265GetPacket(encoder->svt_encoder, &output_buffer, 1);

    if (eb_err != EB_ErrorNone) {
      break;
    }

    if (output_buffer && output_buffer->nFilledLen > 0) {
      uintptr_t frame_nr = reinterpret_cast<uintptr_t>(output_buffer->pAppPrivate);
      parse_nal_units_from_bitstream(
          output_buffer->pBuffer, output_buffer->nFilledLen,
          encoder->output_packets, frame_nr);
    }

    if (output_buffer) {
      EbH265ReleaseOutBuffer(&output_buffer);
    }
  }

  return heif_error_ok;
}


// ============================================================
//              Single-frame encoding (heif_encoder_plugin)
// ============================================================

static heif_error svt_hevc_encode_image(void* encoder_raw, const heif_image* image,
                                         heif_image_input_class input_class)
{
  auto* encoder = (encoder_struct_svt_hevc*) encoder_raw;

  // Initialize encoder
  heif_error err = svt_hevc_init_encoder(encoder, image, false, 1, 25);
  if (err.code) {
    return err;
  }

  // Encode the single frame
  err = svt_hevc_encode_frame(encoder, image, 0, false);
  if (err.code) {
    return err;
  }

  // Flush encoder
  err = svt_hevc_flush_encoder(encoder);
  if (err.code) {
    EbDeinitEncoder(encoder->svt_encoder);
    encoder->encoder_initialized = false;
    return err;
  }

  // Clean up encoder
  EbDeinitEncoder(encoder->svt_encoder);
  encoder->encoder_initialized = false;

  return heif_error_ok;
}


// ============================================================
//              Sequence encoding (heif_encoder_plugin v4)
// ============================================================

static heif_error svt_hevc_start_sequence_encoding(void* encoder_raw, const heif_image* image,
                                                    heif_image_input_class input_class,
                                                    uint32_t framerate_num, uint32_t framerate_denom,
                                                    const heif_sequence_encoding_options* options)
{
  auto* encoder = (encoder_struct_svt_hevc*) encoder_raw;

  return svt_hevc_init_encoder(encoder, image, true, framerate_num, framerate_denom);
}


static heif_error svt_hevc_encode_sequence_frame(void* encoder_raw, const heif_image* image,
                                                  uintptr_t frame_nr)
{
  auto* encoder = (encoder_struct_svt_hevc*) encoder_raw;

  if (!encoder->encoder_initialized) {
    return {
        heif_error_Usage_error,
        heif_suberror_Unspecified,
        "called plugin encode_sequence_frame() without start_sequence_encoding()"
    };
  }

  return svt_hevc_encode_frame(encoder, image, frame_nr, false);
}


static heif_error svt_hevc_end_sequence_encoding(void* encoder_raw)
{
  auto* encoder = (encoder_struct_svt_hevc*) encoder_raw;

  if (!encoder->encoder_initialized) {
    return heif_error_ok;
  }

  // Flush encoder
  heif_error err = svt_hevc_flush_encoder(encoder);

  // Clean up encoder
  EbDeinitEncoder(encoder->svt_encoder);
  encoder->encoder_initialized = false;

  return err;
}


// ============================================================
//              Get compressed data
// ============================================================

static heif_error svt_hevc_get_compressed_data_intern(void* encoder_raw, uint8_t** data,
                                                       int* size, uintptr_t* out_frame_nr)
{
  auto* encoder = (encoder_struct_svt_hevc*) encoder_raw;

  if (encoder->output_packets.empty()) {
    *data = nullptr;
    *size = 0;
    return heif_error_ok;
  }

  encoder->active_output_nal = std::move(encoder->output_packets.front().data);

  if (out_frame_nr) {
    *out_frame_nr = encoder->output_packets.front().frameNr;
  }

  encoder->output_packets.pop_front();

  *data = encoder->active_output_nal.data();
  *size = static_cast<int>(encoder->active_output_nal.size());

  return heif_error_ok;
}


static heif_error svt_hevc_get_compressed_data(void* encoder_raw, uint8_t** data, int* size,
                                                heif_encoded_data_type* type)
{
  return svt_hevc_get_compressed_data_intern(encoder_raw, data, size, nullptr);
}


static heif_error svt_hevc_get_compressed_data2(void* encoder_raw, uint8_t** data, int* size,
                                                 uintptr_t* frame_nr, int* is_keyframe,
                                                 int* more_frame_packets)
{
  return svt_hevc_get_compressed_data_intern(encoder_raw, data, size, frame_nr);
}


// ============================================================
//              Plugin struct definition
// ============================================================

static const heif_encoder_plugin encoder_plugin_svt_hevc
    {
        /* plugin_api_version */ 4,
        /* compression_format */ heif_compression_HEVC,
        /* id_name */ "svt-hevc",
        /* priority */ SVT_HEVC_PLUGIN_PRIORITY,
        /* supports_lossy_compression */ true,
        /* supports_lossless_compression */ false,
        /* get_plugin_name */ svt_hevc_plugin_name,
        /* init_plugin */ svt_hevc_init_plugin,
        /* cleanup_plugin */ svt_hevc_cleanup_plugin,
        /* new_encoder */ svt_hevc_new_encoder,
        /* free_encoder */ svt_hevc_free_encoder,
        /* set_parameter_quality */ svt_hevc_set_parameter_quality,
        /* get_parameter_quality */ svt_hevc_get_parameter_quality,
        /* set_parameter_lossless */ svt_hevc_set_parameter_lossless,
        /* get_parameter_lossless */ svt_hevc_get_parameter_lossless,
        /* set_parameter_logging_level */ svt_hevc_set_parameter_logging_level,
        /* get_parameter_logging_level */ svt_hevc_get_parameter_logging_level,
        /* list_parameters */ svt_hevc_list_parameters,
        /* set_parameter_integer */ svt_hevc_set_parameter_integer,
        /* get_parameter_integer */ svt_hevc_get_parameter_integer,
        /* set_parameter_boolean */ svt_hevc_set_parameter_boolean,
        /* get_parameter_boolean */ svt_hevc_get_parameter_boolean,
        /* set_parameter_string */ svt_hevc_set_parameter_string,
        /* get_parameter_string */ svt_hevc_get_parameter_string,
        /* query_input_colorspace */ svt_hevc_query_input_colorspace,
        /* encode_image */ svt_hevc_encode_image,
        /* get_compressed_data */ svt_hevc_get_compressed_data,
        /* query_input_colorspace (v2) */ svt_hevc_query_input_colorspace2,
        /* query_encoded_size (v3) */ nullptr,
        /* minimum_required_libheif_version */ LIBHEIF_MAKE_VERSION(1,21,0),
        /* start_sequence_encoding (v4) */ svt_hevc_start_sequence_encoding,
        /* encode_sequence_frame (v4) */ svt_hevc_encode_sequence_frame,
        /* end_sequence_encoding (v4) */ svt_hevc_end_sequence_encoding,
        /* get_compressed_data2 (v4) */ svt_hevc_get_compressed_data2,
        /* does_indicate_keyframes (v4) */ 0
    };


const heif_encoder_plugin* get_encoder_plugin_svt_hevc()
{
  return &encoder_plugin_svt_hevc;
}


#if PLUGIN_SvtHevcEnc
heif_plugin_info plugin_info{
    1,
    heif_plugin_type_encoder,
    &encoder_plugin_svt_hevc
};
#endif
