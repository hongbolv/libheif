# Design Document: Integrating iVSR Super Resolution into libheif Scale Functionality

## 1. Background

### 1.1 Problem Statement

libheif currently provides an image scaling API (`heif_image_scale_image`) that uses a **nearest-neighbor** algorithm. While this is fast, it produces low-quality results when scaling up (upscaling) images — edges become jagged and fine details are lost. Modern AI-based super resolution techniques can produce significantly higher quality upscaled images.

### 1.2 Objective

Integrate **Intel Video Super Resolution (iVSR)** SDK into libheif's scale functionality so that when a user scales up an image, the iVSR AI-based super resolution engine can be optionally used to produce high-quality upscaled output, leveraging Intel CPUs and GPUs via OpenVINO.

### 1.3 About iVSR

[iVSR](https://github.com/OpenVisualCloud/iVSR) (Intel Video Super Resolution) is an SDK developed by Intel that facilitates AI media processing with exceptional quality and performance on Intel hardware. Key characteristics:

- **Patch-based inference**: Splits frames into small patches for efficient processing on hardware with limited memory.
- **Heterogeneous execution**: Supports CPU, GPU (Intel Flex 170, Arc 770), and multi-GPU configurations.
- **Multiple models**: Supports Enhanced BasicVSR (multi-frame, 2× upscale), Enhanced EDSR (single-frame, 2× upscale), TSENet (multi-frame, 2× upscale), and SVP (same-resolution enhancement).
- **OpenVINO backend**: Uses Intel OpenVINO for inference.
- **Simple C API**: `ivsr_init` → `ivsr_process` → `ivsr_deinit` lifecycle.

### 1.4 About libheif Scaling

libheif's current scaling implementation:

- **Public C API**: `heif_image_scale_image(input, &output, width, height, options)` defined in `heif_image.h`
- **C++ API**: `Image::scale_image(width, height, options)` in `heif_cxx.h`
- **Internal implementation**: `HeifPixelImage::scale_nearest_neighbor()` in `pixelimage.cc`
- **Options struct**: `heif_scaling_options` is declared but **not yet defined** — currently users pass `NULL`
- **Algorithm**: Nearest-neighbor only (simple pixel replication)
- **Color format support**: RGB, YCbCr (4:2:0/4:2:2/4:4:4), monochrome; 8-bit and 16-bit

---

## 2. Architecture Overview

### 2.1 High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        User Application                            │
│                                                                     │
│   heif_image_scale_image(input, &output, w, h, &scaling_options)   │
└──────────────────────────────┬──────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────────┐
│                     libheif Scale Dispatcher                        │
│                    (heif_image.cc / pixelimage.cc)                   │
│                                                                     │
│   ┌─────────────────────┐     ┌──────────────────────────────────┐ │
│   │ Is scale-up AND     │ YES │  iVSR Super Resolution Path      │ │
│   │ iVSR enabled in     ├────►│  (ivsr_scaling_plugin.cc)        │ │
│   │ scaling_options?     │     │                                  │ │
│   └──────────┬──────────┘     │  1. Convert HeifPixelImage→RGB   │ │
│              │ NO              │  2. ivsr_init()                  │ │
│              ▼                 │  3. ivsr_process()               │ │
│   ┌─────────────────────┐     │  4. Convert RGB→HeifPixelImage   │ │
│   │ Nearest-Neighbor    │     │  5. ivsr_deinit()                │ │
│   │ (existing path)     │     └──────────────────────────────────┘ │
│   └─────────────────────┘                                          │
└─────────────────────────────────────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────────┐
│                     iVSR SDK (libivsr.so)                           │
│                                                                     │
│   ┌──────────┐  ┌───────────────┐  ┌─────────────────────────────┐ │
│   │ Patch    │  │ Task          │  │ OpenVINO Inference Engine   │ │
│   │ Solution │─►│ Scheduler     │─►│ (CPU / GPU / Multi-GPU)    │ │
│   └──────────┘  └───────────────┘  └─────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.2 Design Principles

1. **Backward compatibility**: Existing code passing `NULL` for `heif_scaling_options` continues to use nearest-neighbor scaling unchanged.
2. **Optional dependency**: iVSR is an optional build-time dependency. libheif builds and works without it.
3. **Plugin architecture**: iVSR integration follows libheif's existing plugin pattern — it can be compiled as a built-in module or a dynamic plugin.
4. **Minimal API surface**: Extend the existing `heif_scaling_options` struct rather than adding new API functions.
5. **Single-image focus**: Since libheif processes still images (not video), use single-frame SR models (Enhanced EDSR) as the primary model.

---

## 3. API Design

### 3.1 Extending `heif_scaling_options`

Currently, `heif_scaling_options` is declared but not defined. We define it now to support algorithm selection:

```c
// In heif_image.h

/**
 * @brief Scaling algorithm selection.
 */
enum heif_scaling_algorithm {
    /** Nearest-neighbor interpolation (default, fastest). */
    heif_scaling_algorithm_nearest_neighbor = 0,

    /** AI-based super resolution using iVSR (best quality for upscaling). */
    heif_scaling_algorithm_super_resolution = 1
};

/**
 * @brief Options for image scaling.
 *
 * Use heif_scaling_options_alloc() to create and
 * heif_scaling_options_free() to release.
 */
typedef struct heif_scaling_options {
    /** Version of this struct for future extensibility. Must be 1. */
    int version;

    /** Scaling algorithm to use. Default: heif_scaling_algorithm_nearest_neighbor */
    enum heif_scaling_algorithm algorithm;

    /**
     * iVSR configuration (only used when algorithm == heif_scaling_algorithm_super_resolution).
     * All fields below are ignored for other algorithms.
     */

    /** Path to the OpenVINO IR model file (.xml). Required for SR. */
    const char* ivsr_model_path;

    /** Target device for inference. Default: "CPU". Options: "CPU", "GPU", "MULTI:GPU.0,GPU.1" */
    const char* ivsr_device;

    /** Inference precision. Default: "f32". Options: "f32", "f16", "bf16" */
    const char* ivsr_precision;

    /** Path to custom extension library (for Enhanced BasicVSR). Optional. */
    const char* ivsr_extension_lib;

    /** Path to custom op XML config (for Enhanced BasicVSR). Optional. */
    const char* ivsr_cldnn_config;

    /** Normalization factor for the model. Default: 255.0.
     *  - Set to 255.0 for Enhanced BasicVSR and most models.
     *  - Set to 1.0 for Enhanced EDSR.
     *  The normalize_factor must match what the model expects. */
    float ivsr_normalize_factor;

    /** Scale factor of the SR model. Default: 2 (produces 2× output).
     *  This must match the actual model's scale factor. If the model produces
     *  a different scale factor, the output dimensions will be incorrect.
     *  After the SR pass, a nearest-neighbor resize is applied if the SR output
     *  doesn't match the requested target dimensions. */
    int ivsr_scale_factor;
} heif_scaling_options;

/**
 * @brief Allocate scaling options with default values.
 *
 * Default values:
 *   - algorithm: heif_scaling_algorithm_nearest_neighbor
 *   - ivsr_device: "CPU"
 *   - ivsr_precision: "f32"
 *   - ivsr_normalize_factor: 255.0
 *   - ivsr_scale_factor: 2
 *
 * @return Pointer to allocated options, or NULL on failure.
 *         Must be freed with heif_scaling_options_free().
 */
LIBHEIF_API
heif_scaling_options* heif_scaling_options_alloc(void);

/**
 * @brief Free scaling options allocated by heif_scaling_options_alloc().
 */
LIBHEIF_API
void heif_scaling_options_free(heif_scaling_options* options);
```

### 3.2 C++ API Extension

```cpp
// In heif_cxx.h

class ScalingOptions {
public:
    enum Algorithm {
        NearestNeighbor = heif_scaling_algorithm_nearest_neighbor,
        SuperResolution = heif_scaling_algorithm_super_resolution
    };

    ScalingOptions() : m_options(nullptr) {}

    /** Configure for AI super resolution. */
    void set_super_resolution(const std::string& model_path,
                              const std::string& device = "CPU",
                              const std::string& precision = "f32",
                              float normalize_factor = 255.0f,
                              int scale_factor = 2);

    // Internal: get the C struct pointer
    const heif_scaling_options* get_c_options() const { return m_options; }

    ~ScalingOptions();

private:
    heif_scaling_options* m_options;
};
```

### 3.3 Backward Compatibility

The existing API contract is fully preserved:

```c
// This continues to work exactly as before (nearest-neighbor):
heif_image_scale_image(input, &output, width, height, NULL);

// New usage with super resolution:
heif_scaling_options* opts = heif_scaling_options_alloc();
opts->algorithm = heif_scaling_algorithm_super_resolution;
opts->ivsr_model_path = "/path/to/edsr_model.xml";
opts->ivsr_device = "GPU";
heif_image_scale_image(input, &output, width, height, opts);
heif_scaling_options_free(opts);
```

---

## 4. Internal Implementation Design

### 4.1 Scale Dispatcher Logic

The `heif_image_scale_image()` function is updated to dispatch based on options:

```c
// In heif_image.cc

heif_error heif_image_scale_image(const heif_image* input,
                                  heif_image** output,
                                  int width, int height,
                                  const heif_scaling_options* options)
{
    // Determine if this is an upscale operation.
    // Both dimensions must be >= original (at least one strictly larger) to qualify.
    // If one dimension is larger but the other is smaller (anisotropic scaling),
    // nearest-neighbor is used since SR models produce uniform scale factors.
    bool is_upscale = (width >= (int)input->image->get_width() &&
                       height >= (int)input->image->get_height()) &&
                      (width > (int)input->image->get_width() ||
                       height > (int)input->image->get_height());

    // Use iVSR super resolution if:
    //   1. options is not NULL
    //   2. algorithm is set to super_resolution
    //   3. this is an upscale operation
    //   4. iVSR support is compiled in
    if (options != NULL &&
        options->algorithm == heif_scaling_algorithm_super_resolution &&
        is_upscale) {
#if HAVE_IVSR
        return heif_image_scale_with_ivsr(input, output, width, height, options);
#else
        return {heif_error_Unsupported_feature, heif_suberror_Unspecified,
                "iVSR super resolution support not compiled in"};
#endif
    }

    // If SR was explicitly requested but this is not an upscale, return an error
    if (options != NULL &&
        options->algorithm == heif_scaling_algorithm_super_resolution &&
        !is_upscale) {
        return {heif_error_Usage_error, heif_suberror_Unspecified,
                "Super resolution can only be used for upscaling"};
    }

    // Default: nearest-neighbor scaling (existing behavior)
    std::shared_ptr<HeifPixelImage> out_img;
    Error err = input->image->scale_nearest_neighbor(out_img, width, height, nullptr);
    if (err) {
        return err.error_struct(input->image.get());
    }
    *output = new heif_image;
    (*output)->image = std::move(out_img);
    return Error::Ok.error_struct(input->image.get());
}
```

### 4.2 iVSR Integration Module

A new file `ivsr_scaling_plugin.cc` (and `.h`) implements the bridge between libheif's pixel image format and iVSR's expected input/output:

```
libheif/
├── ivsr_scaling_plugin.h
└── ivsr_scaling_plugin.cc
```

#### 4.2.1 Core Processing Flow

```cpp
// ivsr_scaling_plugin.cc (pseudocode)

heif_error heif_image_scale_with_ivsr(const heif_image* input,
                                      heif_image** output,
                                      int target_width, int target_height,
                                      const heif_scaling_options* options)
{
    // Step 1: Validate parameters
    if (!options->ivsr_model_path) {
        return error("ivsr_model_path is required");
    }

    int src_width  = heif_image_get_width(input, heif_channel_interleaved);
    int src_height = heif_image_get_height(input, heif_channel_interleaved);

    // Step 2: Convert HeifPixelImage to RGB 8-bit interleaved (iVSR input format)
    // If the image is in YCbCr or other format, convert to RGB first
    heif_image* rgb_input = convert_to_rgb_interleaved(input);

    // Step 3: Prepare iVSR configuration
    ivsr_config_t* configs = build_ivsr_config(options, src_width, src_height);

    // Step 4: Initialize iVSR
    ivsr_handle handle = nullptr;
    IVSRStatus status = ivsr_init(configs, &handle);
    if (status != OK) {
        return error("Failed to initialize iVSR engine");
    }

    // Step 5: Query the model output dimensions
    tensor_desc_t output_desc;
    ivsr_get_attr(handle, OUTPUT_TENSOR_DESC, &output_desc);
    int sr_width  = output_desc.shape[3];  // NCHW layout
    int sr_height = output_desc.shape[2];

    // Step 6: Allocate input/output buffers
    size_t input_size  = src_width * src_height * 3;       // RGB u8
    size_t output_size = sr_width * sr_height * 3 * sizeof(float);  // RGB fp32
    char* input_data   = get_rgb_plane_data(rgb_input);
    char* output_data  = allocate_buffer(output_size);

    // Step 7: Run iVSR inference
    ivsr_cb_t cb = {completion_callback, &cb_args};
    status = ivsr_process(handle, input_data, output_data, &cb);
    if (status != OK) {
        ivsr_deinit(handle);
        return error("iVSR processing failed");
    }

    // Step 8: Convert iVSR output (NCHW fp32) back to HeifPixelImage
    *output = convert_nchw_fp32_to_heif_image(output_data, sr_width, sr_height,
                                               options->ivsr_normalize_factor,
                                               input->image->get_colorspace(),
                                               input->image->get_chroma_format());

    // Step 9: If SR output size != target size, do final nearest-neighbor resize
    if (sr_width != target_width || sr_height != target_height) {
        heif_image* final_output;
        heif_image_scale_image(*output, &final_output, target_width, target_height, NULL);
        heif_image_release(*output);
        *output = final_output;
    }

    // Step 10: Cleanup
    ivsr_deinit(handle);
    free_buffer(output_data);
    free_ivsr_configs(configs);

    return heif_error_ok;
}
```

### 4.3 Data Format Conversion

iVSR expects specific data formats. The conversion pipeline:

```
┌──────────────────────────────────────────────────────────────────────┐
│                     Data Format Conversion Pipeline                   │
│                                                                      │
│  ┌─────────────┐    ┌─────────────┐    ┌──────────────────────────┐ │
│  │ HeifPixel   │    │ RGB 8-bit   │    │ iVSR Input               │ │
│  │ Image       │───►│ Interleaved │───►│ NHWC u8 BGR/RGB          │ │
│  │ (any format)│    │ (HWC)       │    │ [1, H, W, 3]             │ │
│  └─────────────┘    └─────────────┘    └──────────┬───────────────┘ │
│                                                    │                 │
│  libheif color-conversion/                    ivsr_process()        │
│  already supports all needed                       │                 │
│  conversions (YCbCr→RGB, etc.)                     ▼                 │
│                                                                      │
│  ┌─────────────┐    ┌─────────────┐    ┌──────────────────────────┐ │
│  │ HeifPixel   │    │ RGB 8-bit   │    │ iVSR Output              │ │
│  │ Image       │◄───│ Interleaved │◄───│ NCHW fp32 RGB            │ │
│  │ (original   │    │ (HWC)       │    │ [1, 3, 2H, 2W]          │ │
│  │  format)    │    └─────────────┘    └──────────────────────────┘ │
│  └─────────────┘                                                     │
│                     Post-processing:                                 │
│                     - Clamp to [0, 1]                                │
│                     - Multiply by normalize_factor (255)             │
│                     - Convert fp32 → u8                              │
│                     - Transpose NCHW → HWC                           │
│                     - Optionally convert RGB → YCbCr                 │
└──────────────────────────────────────────────────────────────────────┘
```

#### 4.3.1 Input Conversion (HeifPixelImage → iVSR)

```cpp
// Convert any HeifPixelImage to RGB interleaved u8 for iVSR input
static std::vector<uint8_t> heif_to_ivsr_input(const HeifPixelImage& image) {
    // Use libheif's existing color conversion infrastructure
    // to convert any colorspace to heif_colorspace_RGB + heif_chroma_interleaved_RGB

    auto rgb_image = convert_colorspace(image,
                                         heif_colorspace_RGB,
                                         heif_chroma_interleaved_RGB,
                                         nullptr, 8, nullptr);

    size_t stride;
    const uint8_t* data = rgb_image->get_plane(heif_channel_interleaved, &stride);

    int width = rgb_image->get_width();
    int height = rgb_image->get_height();

    // Copy to contiguous buffer (iVSR expects contiguous NHWC data)
    std::vector<uint8_t> buffer(width * height * 3);
    for (int y = 0; y < height; y++) {
        memcpy(buffer.data() + y * width * 3, data + y * stride, width * 3);
    }
    return buffer;
}
```

#### 4.3.2 Output Conversion (iVSR → HeifPixelImage)

```cpp
// Convert iVSR NCHW fp32 output to HeifPixelImage
static std::shared_ptr<HeifPixelImage> ivsr_output_to_heif(
    const float* nchw_data, int width, int height,
    float normalize_factor,
    heif_colorspace target_colorspace,
    heif_chroma target_chroma)
{
    // Step 1: Create RGB interleaved image
    auto img = std::make_shared<HeifPixelImage>();
    img->create(width, height, heif_colorspace_RGB, heif_chroma_interleaved_RGB);
    img->add_plane(heif_channel_interleaved, width, height, 8, nullptr);

    size_t stride;
    uint8_t* out = img->get_plane(heif_channel_interleaved, &stride);

    // Step 2: Transpose NCHW to HWC and convert fp32 to u8
    // NCHW layout: data[c * H * W + y * W + x]
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            for (int c = 0; c < 3; c++) {
                float val = nchw_data[c * height * width + y * width + x];
                val = val * normalize_factor;
                val = std::clamp(val, 0.0f, 255.0f);
                out[y * stride + x * 3 + c] = static_cast<uint8_t>(val + 0.5f);
            }
        }
    }

    // Step 3: Convert to target colorspace if needed
    if (target_colorspace != heif_colorspace_RGB ||
        target_chroma != heif_chroma_interleaved_RGB) {
        img = convert_colorspace(img, target_colorspace, target_chroma,
                                  nullptr, 8, nullptr);
    }

    return img;
}
```

---

## 5. Build System Integration

### 5.1 CMake Configuration

iVSR is an optional dependency, following the same pattern as other optional features in libheif:

```cmake
# In top-level CMakeLists.txt

option(WITH_IVSR "Build with iVSR super resolution support" OFF)

if (WITH_IVSR)
    # Find iVSR SDK
    find_library(IVSR_LIBRARY NAMES ivsr PATHS ${IVSR_SDK_PATH}/lib)
    find_path(IVSR_INCLUDE_DIR NAMES ivsr.h PATHS ${IVSR_SDK_PATH}/include)

    if (IVSR_LIBRARY AND IVSR_INCLUDE_DIR)
        message(STATUS "iVSR SDK found: ${IVSR_LIBRARY}")
        set(HAVE_IVSR ON)
    else()
        message(WARNING "iVSR SDK not found. Building without super resolution support.")
        set(HAVE_IVSR OFF)
    endif()
endif()
```

```cmake
# In libheif/CMakeLists.txt

if (HAVE_IVSR)
    target_sources(heif PRIVATE
        ivsr_scaling_plugin.cc
        ivsr_scaling_plugin.h
    )
    target_include_directories(heif PRIVATE ${IVSR_INCLUDE_DIR})
    target_link_libraries(heif PRIVATE ${IVSR_LIBRARY})
    target_compile_definitions(heif PRIVATE HAVE_IVSR=1)
endif()
```

### 5.2 Dependency Chain

```
libheif (with iVSR support)
  └── libivsr.so (iVSR SDK)
       └── OpenVINO (inference engine)
            ├── CPU plugin
            └── GPU plugin (optional, requires Intel GPU drivers)
```

### 5.3 Build Instructions

```bash
# Build libheif with iVSR super resolution support
mkdir build && cd build
cmake .. -DWITH_IVSR=ON -DIVSR_SDK_PATH=/path/to/ivsr_sdk
make -j$(nproc)

# Build without iVSR (default, no change from current behavior)
cmake ..
make -j$(nproc)
```

---

## 6. Detailed Module Design

### 6.1 New Files

| File | Purpose |
|------|---------|
| `libheif/ivsr_scaling_plugin.h` | Header for iVSR integration module |
| `libheif/ivsr_scaling_plugin.cc` | Implementation of iVSR-based scaling |

### 6.2 Modified Files

| File | Change |
|------|--------|
| `libheif/api/libheif/heif_image.h` | Define `heif_scaling_options` struct, add `heif_scaling_algorithm` enum, add `heif_scaling_options_alloc/free` |
| `libheif/api/libheif/heif_image.cc` | Update `heif_image_scale_image()` to dispatch to iVSR when configured |
| `libheif/api/libheif/heif_cxx.h` | Extend `ScalingOptions` C++ class |
| `libheif/pixelimage.h` | No changes needed |
| `libheif/pixelimage.cc` | No changes needed (nearest-neighbor stays as-is) |
| `CMakeLists.txt` | Add `WITH_IVSR` option |
| `libheif/CMakeLists.txt` | Conditionally compile iVSR module |

### 6.3 `ivsr_scaling_plugin.h`

```cpp
#ifndef LIBHEIF_IVSR_SCALING_PLUGIN_H
#define LIBHEIF_IVSR_SCALING_PLUGIN_H

#include "libheif/heif_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Scale an image using iVSR AI super resolution.
 *
 * This function is called internally by heif_image_scale_image() when
 * the scaling options specify super resolution algorithm.
 *
 * @param input       Source image
 * @param output      Pointer to receive the scaled output image
 * @param width       Target width
 * @param height      Target height
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
```

### 6.4 `ivsr_scaling_plugin.cc` Key Implementation

```cpp
#include "ivsr_scaling_plugin.h"
#include "ivsr.h"
#include "pixelimage.h"
#include "color-conversion/colorconversion.h"
#include <vector>
#include <cstring>
#include <algorithm>
#include <cmath>

// Callback for iVSR synchronous processing
static void ivsr_completion_callback(void* args) {
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
    int src_width  = src_image->get_width();
    int src_height = src_image->get_height();
    int scale_factor = options->ivsr_scale_factor > 0 ? options->ivsr_scale_factor : 2;
    float normalize_factor = options->ivsr_normalize_factor > 0
                             ? options->ivsr_normalize_factor : 255.0f;

    // --- Step 1: Convert to RGB interleaved 8-bit ---
    // Use libheif's existing color conversion infrastructure
    std::shared_ptr<HeifPixelImage> rgb_image;
    if (src_image->get_colorspace() != heif_colorspace_RGB ||
        src_image->get_chroma_format() != heif_chroma_interleaved_RGB) {
        // Convert using libheif color conversion pipeline
        // (colorconversion.h provides convert_colorspace())
        rgb_image = convert_colorspace(src_image,
                                        heif_colorspace_RGB,
                                        heif_chroma_interleaved_RGB,
                                        nullptr, /*target_profile*/
                                        8,       /*output_bpp*/
                                        nullptr  /*security_limits*/);
        if (!rgb_image) {
            return {heif_error_Encoding_error, heif_suberror_Unspecified,
                    "Failed to convert image to RGB for iVSR"};
        }
    } else {
        rgb_image = src_image;
    }

    // --- Step 2: Extract contiguous RGB buffer and swap to BGR for iVSR ---
    // iVSR expects BGR byte order (tensor_color_format="BGR"),
    // while libheif's interleaved RGB stores data in R,G,B order.
    size_t in_stride;
    const uint8_t* rgb_data = rgb_image->get_plane(heif_channel_interleaved, &in_stride);
    std::vector<uint8_t> input_buffer(src_width * src_height * 3);
    for (int y = 0; y < src_height; y++) {
        for (int x = 0; x < src_width; x++) {
            int src_idx = y * in_stride + x * 3;
            int dst_idx = y * src_width * 3 + x * 3;
            input_buffer[dst_idx + 0] = rgb_data[src_idx + 2]; // B
            input_buffer[dst_idx + 1] = rgb_data[src_idx + 1]; // G
            input_buffer[dst_idx + 2] = rgb_data[src_idx + 0]; // R
        }
    }

    // --- Step 3: Build iVSR configuration linked list ---
    // (Following the pattern from ivsr_sdk/samples/vsr_sample.cpp)
    std::vector<ivsr_config_t> configs;
    auto add_config = [&configs](IVSRConfigKey key, const void* value) {
        ivsr_config_t cfg;
        cfg.key = key;
        cfg.value = value;
        cfg.next = nullptr;
        configs.push_back(cfg);
        if (configs.size() > 1) {
            configs[configs.size() - 2].next = &configs.back();
        }
    };

    add_config(INPUT_MODEL, options->ivsr_model_path);

    const char* device = options->ivsr_device ? options->ivsr_device : "CPU";
    add_config(TARGET_DEVICE, device);

    if (options->ivsr_precision) {
        add_config(PRECISION, options->ivsr_precision);
    }
    if (options->ivsr_extension_lib) {
        add_config(CUSTOM_LIB, options->ivsr_extension_lib);
    }
    if (options->ivsr_cldnn_config) {
        add_config(CLDNN_CONFIG, options->ivsr_cldnn_config);
    }

    std::string input_res = std::to_string(src_width) + "," + std::to_string(src_height);
    add_config(INPUT_RES, input_res.c_str());

    // Configure tensor descriptors for single-image SR.
    // Note: tensor_color_format="BGR" indicates the byte order of input data as fed to iVSR.
    // model_color_format="RGB" indicates the color order expected by the model internally.
    // iVSR handles the BGR→RGB conversion internally based on these descriptors.
    // We provide data in BGR order from libheif's RGB interleaved format
    // by swapping R and B channels during the input buffer copy.
    tensor_desc_t input_tensor_desc = {
        .precision = "u8",
        .layout = "NHWC",
        .tensor_color_format = "BGR",
        .model_color_format = "RGB",
        .scale = normalize_factor,
        .dimension = 4,
        .shape = {1, (size_t)src_height, (size_t)src_width, 3}
    };
    tensor_desc_t output_tensor_desc = {
        .precision = "fp32",
        .layout = "NCHW",
        .tensor_color_format = {0},
        .model_color_format = {0},
        .scale = 0.0,
        .dimension = 4,
        .shape = {1, 3, (size_t)(src_height * scale_factor), (size_t)(src_width * scale_factor)}
    };
    add_config(INPUT_TENSOR_DESC_SETTING, &input_tensor_desc);
    add_config(OUTPUT_TENSOR_DESC_SETTING, &output_tensor_desc);

    // --- Step 4: Initialize iVSR ---
    ivsr_handle handle = nullptr;
    IVSRStatus status = ivsr_init(&configs[0], &handle);
    if (status != OK) {
        return {heif_error_Plugin_loading_error, heif_suberror_Unspecified,
                "Failed to initialize iVSR engine"};
    }

    // --- Step 5: Query actual output dimensions ---
    tensor_desc_t actual_output_desc = {0};
    ivsr_get_attr(handle, OUTPUT_TENSOR_DESC, &actual_output_desc);
    int sr_height = actual_output_desc.shape[2];
    int sr_width  = actual_output_desc.shape[3];

    // --- Step 6: Allocate output buffer and run inference ---
    std::vector<float> output_buffer(sr_width * sr_height * 3, 0.0f);

    int completion_flag = 0;
    ivsr_cb_t cb;
    cb.ivsr_cb = ivsr_completion_callback;
    cb.args = &completion_flag;

    status = ivsr_process(handle, reinterpret_cast<char*>(input_buffer.data()),
                          reinterpret_cast<char*>(output_buffer.data()), &cb);
    if (status != OK) {
        ivsr_deinit(handle);
        return {heif_error_Encoding_error, heif_suberror_Unspecified,
                "iVSR processing failed"};
    }

    // --- Step 7: Convert NCHW fp32 output to HeifPixelImage ---
    auto out_img = std::make_shared<HeifPixelImage>();
    out_img->create(sr_width, sr_height, heif_colorspace_RGB, heif_chroma_interleaved_RGB);
    out_img->add_plane(heif_channel_interleaved, sr_width, sr_height, 8, nullptr);

    size_t out_stride;
    uint8_t* out_data = out_img->get_plane(heif_channel_interleaved, &out_stride);

    for (int y = 0; y < sr_height; y++) {
        for (int x = 0; x < sr_width; x++) {
            for (int c = 0; c < 3; c++) {
                float val = output_buffer[c * sr_height * sr_width + y * sr_width + x];
                val *= normalize_factor;
                val = std::clamp(val, 0.0f, 255.0f);
                out_data[y * out_stride + x * 3 + c] = static_cast<uint8_t>(val + 0.5f);
            }
        }
    }

    // --- Step 8: Convert back to original colorspace if needed ---
    heif_colorspace orig_cs = src_image->get_colorspace();
    heif_chroma orig_chroma = src_image->get_chroma_format();
    if (orig_cs != heif_colorspace_RGB || orig_chroma != heif_chroma_interleaved_RGB) {
        out_img = convert_colorspace(out_img, orig_cs, orig_chroma,
                                      nullptr, 8, nullptr);
    }

    // --- Step 9: Final resize if SR output doesn't match target ---
    *output = new heif_image;
    (*output)->image = std::move(out_img);

    if (sr_width != target_width || sr_height != target_height) {
        heif_image* resized = nullptr;
        heif_error resize_err = heif_image_scale_image(*output, &resized,
                                                        target_width, target_height, NULL);
        if (resize_err.code != heif_error_Ok) {
            heif_image_release(*output);
            ivsr_deinit(handle);
            return resize_err;
        }
        heif_image_release(*output);
        *output = resized;
    }

    // --- Step 10: Cleanup ---
    ivsr_deinit(handle);

    return {heif_error_Ok, heif_suberror_Unspecified, "Success"};
}
```

---

## 7. Error Handling

### 7.1 Error Scenarios

| Scenario | Behavior |
|----------|----------|
| `options == NULL` | Use nearest-neighbor (existing behavior) |
| `algorithm == super_resolution` but `HAVE_IVSR` not defined | Return `heif_error_Unsupported_feature` |
| `ivsr_model_path` is NULL | Return `heif_error_Usage_error` |
| Model file not found | Return `heif_error_Plugin_loading_error` (from iVSR init failure) |
| iVSR init fails | Return `heif_error_Plugin_loading_error` with iVSR status |
| iVSR inference fails | Return `heif_error_Encoding_error`, cleanup resources |
| Downscale with SR algorithm | Return `heif_error_Usage_error` with a message indicating SR is only for upscaling |
| Unsupported color format | Convert to RGB first using libheif's color conversion |
| HDR (16-bit) input | Convert to 8-bit RGB for iVSR, upscale, then user may re-encode |

### 7.2 Resource Safety

All iVSR resources are managed with RAII-style cleanup:

```cpp
// Use a guard to ensure ivsr_deinit is always called
struct IVSRGuard {
    ivsr_handle handle;
    ~IVSRGuard() { if (handle) ivsr_deinit(handle); }
};
```

---

## 8. Scaling Strategy for Arbitrary Dimensions

iVSR models produce fixed scale factors (typically 2×). To support arbitrary target dimensions:

```
Input: 640×480 → Target: 1920×1080

Strategy:
1. Run iVSR 2× SR: 640×480 → 1280×960 (AI upscale)
2. If still smaller than target, run iVSR again: 1280×960 → 2560×1920
3. Final nearest-neighbor resize: 2560×1920 → 1920×1080 (crop/fit)

Alternative (simpler, recommended for v1):
1. Run iVSR 2× SR: 640×480 → 1280×960
2. Nearest-neighbor resize: 1280×960 → 1920×1080
```

The initial implementation uses the simpler approach (single SR pass + nearest-neighbor adjustment). Multi-pass SR can be added in a future version.

---

## 9. Usage Examples

### 9.1 C API Example

```c
#include <libheif/heif.h>

int main() {
    // Decode a HEIF image
    heif_context* ctx = heif_context_alloc();
    heif_context_read_from_file(ctx, "input.heif", NULL);

    heif_image_handle* handle;
    heif_context_get_primary_image_handle(ctx, &handle);

    heif_image* image;
    heif_decode_image(handle, &image, heif_colorspace_RGB,
                      heif_chroma_interleaved_RGB, NULL);

    // Scale up 2× using AI super resolution
    heif_scaling_options* opts = heif_scaling_options_alloc();
    opts->algorithm = heif_scaling_algorithm_super_resolution;
    opts->ivsr_model_path = "/models/enhanced_edsr.xml";
    opts->ivsr_device = "GPU";
    opts->ivsr_normalize_factor = 1.0;  // Enhanced EDSR requires 1.0 (overrides default 255.0)

    int orig_w = heif_image_get_width(image, heif_channel_interleaved);
    int orig_h = heif_image_get_height(image, heif_channel_interleaved);

    heif_image* sr_image;
    heif_error err = heif_image_scale_image(image, &sr_image,
                                             orig_w * 2, orig_h * 2, opts);
    if (err.code != heif_error_Ok) {
        fprintf(stderr, "Super resolution failed: %s\n", err.message);
        // Fall back to default scaling
        heif_image_scale_image(image, &sr_image,
                                orig_w * 2, orig_h * 2, NULL);
    }

    // Use sr_image...

    heif_scaling_options_free(opts);
    heif_image_release(sr_image);
    heif_image_release(image);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return 0;
}
```

### 9.2 C++ API Example

```cpp
#include <libheif/heif_cxx.h>

int main() {
    heif::Context ctx;
    ctx.read_from_file("input.heif");

    heif::ImageHandle handle = ctx.get_primary_image_handle();
    heif::Image image = handle.decode_image(heif_colorspace_RGB,
                                             heif_chroma_interleaved_RGB);

    int w = image.get_width(heif_channel_interleaved);
    int h = image.get_height(heif_channel_interleaved);

    // Configure super resolution scaling
    heif::Image::ScalingOptions opts;
    opts.set_super_resolution("/models/enhanced_edsr.xml", "CPU", "f32", 1.0, 2);

    // Scale up with AI super resolution
    heif::Image sr_image = image.scale_image(w * 2, h * 2, opts);

    return 0;
}
```

### 9.3 Command-Line Tool Integration (heif_dec)

```bash
# Decode and upscale using AI super resolution
heif_dec input.heif -o output.png --scale 2 \
        --sr-model /models/enhanced_edsr.xml \
        --sr-device GPU
```

---

## 10. Performance Considerations

### 10.1 Expected Performance

| Operation | 720p → 1440p | 1080p → 2160p (4K) |
|-----------|-------------|---------------------|
| Nearest-neighbor | < 1ms | < 2ms |
| iVSR (EDSR, CPU) | ~200-500ms | ~800-2000ms |
| iVSR (EDSR, GPU) | ~50-150ms | ~200-600ms |

### 10.2 Optimization Strategies

1. **Lazy initialization**: Cache the iVSR handle across multiple calls with the same model, avoiding repeated model loading.
2. **Patch-based processing**: iVSR's built-in patch solution handles large images that exceed GPU memory.
3. **Async processing**: Use `ivsr_process_async` for non-blocking operation when processing multiple images.
4. **Model selection**: Use Enhanced EDSR (single-frame) for still images — no need for multi-frame models like BasicVSR.

### 10.3 Handle Caching (Future Enhancement)

```cpp
// Thread-local iVSR handle cache for repeated use
class IVSRHandleCache {
    struct CacheEntry {
        std::string model_path;
        std::string device;
        ivsr_handle handle;
    };
    static thread_local std::optional<CacheEntry> cached;

public:
    static ivsr_handle get_or_create(const heif_scaling_options* opts);
    static void release_all();
};
```

---

## 11. Testing Plan

### 11.1 Unit Tests

| Test Case | Description |
|-----------|-------------|
| `test_scale_options_null` | Verify NULL options still uses nearest-neighbor |
| `test_scale_options_default` | Verify default-initialized options use nearest-neighbor |
| `test_scale_sr_no_model` | Verify error when SR selected but no model path |
| `test_scale_downscale_sr` | Verify downscale with SR falls back to nearest-neighbor |
| `test_scale_options_alloc_free` | Verify alloc/free lifecycle |

### 11.2 Integration Tests (require iVSR SDK + model files)

| Test Case | Description |
|-----------|-------------|
| `test_sr_rgb_2x` | 2× upscale of RGB image via EDSR |
| `test_sr_ycbcr_2x` | 2× upscale of YCbCr 4:2:0 image (auto-convert) |
| `test_sr_arbitrary_size` | Upscale to non-2× size (SR + nearest-neighbor) |
| `test_sr_gpu_device` | Run inference on GPU |
| `test_sr_roundtrip` | Decode HEIF → SR upscale → encode HEIF |

### 11.3 Quality Validation

- Compare SR output against nearest-neighbor and bilinear interpolation using PSNR/SSIM metrics
- Visual inspection of edge preservation and detail restoration

---

## 12. Future Enhancements

1. **Multi-pass SR**: Apply SR multiple times for > 2× upscale (e.g., 4× = two passes of 2×).
2. **Model auto-detection**: Automatically select the best model based on image characteristics.
3. **Handle caching**: Reuse iVSR handles across multiple `heif_image_scale_image` calls.
4. **Async API**: Add `heif_image_scale_image_async` for non-blocking SR processing.
5. **Additional SR backends**: Support other SR engines beyond iVSR (e.g., ONNX Runtime, TensorRT).
6. **Dynamic plugin loading**: Package iVSR integration as a dynamically loadable libheif plugin.
7. **SVP support**: Integrate iVSR's Smart Video Processing for bandwidth-optimized encoding.
8. **HDR support**: Support 16-bit HDR images through appropriate model and conversion pipeline.
9. **Bilinear/bicubic fallback**: Add traditional interpolation algorithms as intermediate options between nearest-neighbor and AI SR.

---

## 13. Summary

This design integrates iVSR AI super resolution into libheif's existing scaling API with minimal changes:

- **1 new enum** (`heif_scaling_algorithm`) for algorithm selection
- **1 struct definition** (`heif_scaling_options`) completing the existing forward declaration
- **2 new helper functions** (`heif_scaling_options_alloc/free`) for lifecycle management
- **2 new source files** (`ivsr_scaling_plugin.cc/.h`) for the iVSR bridge
- **Minor updates** to the scale dispatcher, CMake build system, and C++ wrapper

The design preserves full backward compatibility — existing code passing `NULL` for scaling options continues to work identically. iVSR is an optional compile-time dependency, and the feature gracefully degrades when not available.
