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
- **Multiple models**: Supports Enhanced BasicVSR (multi-frame, 2× upscale), Enhanced EDSR (single-frame, 2× upscale), TSENet (multi-frame, 2× upscale), and SVP (same-resolution enhancement). **This integration uses Enhanced EDSR fp32** as the first step.
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
│   ┌──────────────────────────────────────────────────────────────┐ │
│   │ Algorithm Selection (upfront, no fallback):                  │ │
│   │                                                              │ │
│   │ 1. Nearest-Neighbor (existing path)                          │ │
│   │    Used when: options==NULL, algorithm==nearest_neighbor,     │ │
│   │    OR algorithm==super_resolution with non-2×/4× scale       │ │
│   │                                                              │ │
│   │ 2. iVSR Super Resolution Path                                │ │
│   │    Used when: algorithm==super_resolution AND scale is 2×/4× │ │
│   │    (ivsr_scaling_plugin.cc)                                   │ │
│   │    a. Check input format — OpenVINO-compatible? (RGB, BGR,    │ │
│   │       NV12, I420, GRAY)                                       │ │
│   │       → YES: pass pointer directly (zero-copy), set tensor   │ │
│   │         descriptor so prepostProcessor converts to RGB        │ │
│   │       → NO (HEIF YCbCr planar, 16-bit HDR, etc.):            │ │
│   │         libheif convert_colorspace() to RGB interleaved first │ │
│   │    b. ivsr_init() with tensor descriptors                    │ │
│   │    c. ivsr_process() — prepostProcessor handles all internal  │ │
│   │       format conversion (layout, precision, color)            │ │
│   │    d. iVSR writes into pre-allocated output plane (zero-copy)│ │
│   │    e. ivsr_deinit()                                          │ │
│   └──────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────────┐
│                     iVSR SDK (libivsr.so)                           │
│                                                                     │
│   ┌──────────┐  ┌───────────────┐  ┌─────────────────────────────┐ │
│   │ Patch    │  │ Task          │  │ OpenVINO Inference Engine   │ │
│   │ Solution │─►│ Scheduler     │─►│ (CPU / GPU)                │ │
│   └──────────┘  └───────────────┘  └─────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.2 Design Principles

1. **Backward compatibility**: Existing code passing `NULL` for `heif_scaling_options` continues to use nearest-neighbor scaling unchanged.
2. **Optional dependency**: iVSR is an optional build-time dependency. libheif builds and works without it.
3. **Plugin architecture**: iVSR integration follows libheif's existing plugin pattern — it can be compiled as a built-in module or a dynamic plugin.
4. **Minimal API surface**: Extend the existing `heif_scaling_options` struct rather than adding new API functions.
5. **Single-image focus**: Since libheif processes still images (not video), use Enhanced EDSR (single-frame, fp32) as the SR model.
6. **Exact scale factors only**: iVSR is invoked only for exact 2× or 4× upscaling. All other scale factors use nearest-neighbor.

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

    /** AI-based super resolution using iVSR (best quality for 2×/4× upscaling). */
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
     *
     * The default configuration targets Enhanced EDSR fp32 model.
     */

    /** Path to the Enhanced EDSR OpenVINO IR model file (.xml). Required for SR. */
    const char* ivsr_model_path;

    /** Target device for inference. Default: "CPU". Options: "CPU", "GPU" */
    const char* ivsr_device;
} heif_scaling_options;

/**
 * @brief Allocate scaling options with default values.
 *
 * Default values:
 *   - algorithm: heif_scaling_algorithm_nearest_neighbor
 *   - ivsr_model_path: NULL
 *   - ivsr_device: "CPU"
 *
 * The configuration is pre-set for Enhanced EDSR fp32 model (normalize_factor=1.0,
 * precision=fp32, scale_factor=2). These are internal defaults not exposed to the user.
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

    /** Configure for AI super resolution using Enhanced EDSR fp32 model. */
    void set_super_resolution(const std::string& model_path,
                              const std::string& device = "CPU");

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

// New usage with super resolution (2× upscale with EDSR):
heif_scaling_options* opts = heif_scaling_options_alloc();
opts->algorithm = heif_scaling_algorithm_super_resolution;
opts->ivsr_model_path = "/path/to/edsr_model.xml";
heif_image_scale_image(input, &output, width * 2, height * 2, opts);
heif_scaling_options_free(opts);
```

---

## 4. Internal Implementation Design

### 4.1 Scale Dispatcher Logic

The `heif_image_scale_image()` function is updated to dispatch based on options. The algorithm selection is done entirely upfront — iVSR is only invoked for exact 2× or 4× upscaling:

```c
// In heif_image.cc

heif_error heif_image_scale_image(const heif_image* input,
                                  heif_image** output,
                                  int width, int height,
                                  const heif_scaling_options* options)
{
    int src_w = (int)input->image->get_width();
    int src_h = (int)input->image->get_height();

    // --- Algorithm selection (all decisions made here, no fallback later) ---

    if (options != NULL &&
        options->algorithm == heif_scaling_algorithm_super_resolution) {

        // Determine the scale factor
        // iVSR only supports exact 2× or 4× upscaling
        bool is_2x = (width == src_w * 2 && height == src_h * 2);
        bool is_4x = (width == src_w * 4 && height == src_h * 4);

        if (is_2x || is_4x) {
#if HAVE_IVSR
            // Dispatch to iVSR super resolution
            // For 4×: internally runs two passes of 2× SR
            return heif_image_scale_with_ivsr(input, output, width, height, options);
#else
            return {heif_error_Unsupported_feature, heif_suberror_Unspecified,
                    "iVSR super resolution support not compiled in"};
#endif
        }

        // For non-2×/4× scale factors, use nearest-neighbor
        // (SR models only produce exact 2× output per pass)
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

    // Step 2: Determine number of SR passes
    // The dispatcher guarantees target is exactly 2× or 4× of source
    int num_passes = (target_width == src_width * 4) ? 2 : 1;

    // Step 3: Prepare input — select best conversion path.
    // Initial implementation: RGB/BGR interleaved → OpenVINO (zero-copy).
    // All other formats (HEIF YCbCr planar, NV12, I420, GRAY, 16-bit HDR)
    // → libheif convert_colorspace() fallback.
    // (OpenVINO supports NV12/I420/GRAY natively — deferred to future enhancement.)
    std::shared_ptr<HeifPixelImage> converted_image;
    const char* tensor_color_format = nullptr;
    size_t in_stride;
    uint8_t* input_ptr = prepare_ivsr_input(input->image, converted_image,
                                             &tensor_color_format, &in_stride);
    if (!input_ptr) {
        return error("Failed to prepare input for iVSR");
    }

    // Step 4: Prepare iVSR configuration (EDSR fp32 defaults)
    // Configure tensor descriptors so prepostProcessor handles:
    //   - Input:  <tensor_color_format> NHWC u8 → model's internal format
    //   - Output: model's internal format → NHWC u8 RGB
    ivsr_config_t* configs = build_ivsr_config(options, src_width, src_height,
                                                tensor_color_format);

    // Step 5: Initialize iVSR
    ivsr_handle handle = nullptr;
    IVSRStatus status = ivsr_init(configs, &handle);
    if (status != OK) {
        return error("Failed to initialize iVSR engine");
    }

    // Step 6: Run SR passes
    // Pass HeifPixelImage plane pointers directly to iVSR — zero copy
    // when input is in an OpenVINO-compatible format.
    // iVSR's prepostProcessor handles all format conversions internally.
    char* current_input = (char*)input_ptr;
    int cur_w = src_width, cur_h = src_height;

    // Pre-allocate output HeifPixelImage and pass its plane pointer to iVSR
    std::shared_ptr<HeifPixelImage> out_img;
    for (int pass = 0; pass < num_passes; pass++) {
        int out_w = cur_w * 2, out_h = cur_h * 2;
        size_t out_stride;
        uint8_t* output_ptr = create_output_image_and_get_ptr(out_img,
                                                               out_w, out_h, &out_stride);

        ivsr_cb_t cb = {completion_callback, &cb_args};
        status = ivsr_process(handle, current_input, (char*)output_ptr, &cb);
        if (status != OK) {
            ivsr_deinit(handle);
            return error("iVSR processing failed");
        }

        // For second pass (4×), use output image plane as next input
        if (pass < num_passes - 1) {
            current_input = (char*)output_ptr;
            cur_w = out_w;
            cur_h = out_h;
        }
    }

    // Step 7: Output is always RGB interleaved u8.
    // iVSR wrote directly into out_img's RGB plane buffer — no conversion needed.
    // The caller (e.g., heif_enc) handles any further colorspace conversion
    // required by the encoder (x265, SVT-HEVC, etc.).
    *output = out_img;

    // Step 8: Cleanup
    ivsr_deinit(handle);
    free_ivsr_configs(configs);

    return heif_error_ok;
}
```

### 4.3 Data Format Conversion

#### 4.3.1 OpenVINO prepostProcessor Supported Formats

OpenVINO's prepostProcessor (used internally by iVSR) supports the following format conversions.
The format conversion selection logic in this integration follows the principle: **use OpenVINO's
prepostProcessor for conversions it can handle; fall back to libheif's `convert_colorspace()` only
for conversions OpenVINO cannot handle.**

**Supported color formats** (from `ov::preprocess::ColorFormat`):

| Color Format | Description | Multi-plane |
|--------------|-------------|-------------|
| `RGB` | RGB interleaved (3 channels) | No |
| `BGR` | BGR interleaved (3 channels) | No |
| `RGBX` | RGBX interleaved (4 channels, X ignored) | No |
| `BGRX` | BGRX interleaved (4 channels, X ignored) | No |
| `NV12_SINGLE_PLANE` | NV12 (Y + interleaved UV) as single tensor | No |
| `NV12_TWO_PLANES` | NV12 as separate Y and UV tensors | Yes (2) |
| `I420_SINGLE_PLANE` | I420 (YUV planar) as single tensor | No |
| `I420_THREE_PLANES` | I420 as separate Y, U, V tensors | Yes (3) |
| `GRAY` | Grayscale (1 channel) | No |

**Supported color conversions** (via `preprocess().convert_color()`):

| From → To | Supported |
|-----------|-----------|
| NV12 → RGB / BGR | ✅ |
| I420 → RGB / BGR | ✅ |
| RGB ↔ BGR | ✅ (also via `reverse_channels()`) |
| RGBX / BGRX → RGB / BGR | ✅ |
| GRAY → RGB / BGR | ✅ |
| RGB / BGR → GRAY | ✅ |
| **HEIF YCbCr planar (4:2:0/4:2:2/4:4:4) → RGB** | ❌ Not directly supported |

**Supported element types** (via `set_element_type()` / `convert_element_type()`):

| Type | Description |
|------|-------------|
| `u8` | Unsigned 8-bit integer |
| `u16` | Unsigned 16-bit integer |
| `f16` | IEEE 754 half-precision float |
| `f32` | IEEE 754 single-precision float |
| `f64` | IEEE 754 double-precision float |
| `i8`, `i16`, `i32`, `i64` | Signed integers |
| `bf16` | Brain floating point 16-bit |

**Supported layout conversions** (via `convert_layout()`):

Any layout transposition is supported (e.g., NHWC ↔ NCHW), as long as source and
destination layouts have the same number of dimensions.

**Supported preprocessing operations**:

| Operation | Description |
|-----------|-------------|
| `convert_element_type` | Convert tensor element precision (e.g., u8 → f32) |
| `convert_color` | Convert between supported color formats |
| `convert_layout` | Transpose tensor dimensions (e.g., NHWC → NCHW) |
| `scale` | Divide each element by a value (per-tensor or per-channel) |
| `mean` | Subtract a value from each element (per-tensor or per-channel) |
| `resize` | Resize spatial dimensions (linear, cubic, nearest, bilinear/bicubic Pillow) |
| `crop` | Crop input tensor |
| `clamp` | Clamp values to [min, max] range |
| `reverse_channels` | Reverse channel order (e.g., RGB → BGR) |
| `pad` | Pad tensor edges with constants |
| `custom` | User-defined callback function |

**Supported postprocessing operations**:

| Operation | Description |
|-----------|-------------|
| `convert_element_type` | Convert output element precision (e.g., f32 → u8) |
| `convert_layout` | Transpose output dimensions (e.g., NCHW → NHWC) |
| `convert_color` | Convert output color format |
| `clamp` | Clamp output values to [min, max] range |
| `custom` | User-defined callback function |

#### 4.3.2 Format Conversion Selection Logic (Decode → SR → Encode)

The complete decode → SR → encode pipeline involves multiple format transitions. The selection
logic follows: **use OpenVINO prepostProcessor when it can handle the conversion; fall back to
libheif's `convert_colorspace()` only when OpenVINO cannot.**

```
┌─────────────────────────────────────────────────────────────────────────┐
│              Decode → SR → Encode Format Conversion Pipeline            │
│                                                                         │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │ 1. HEIF Decode Output                                            │  │
│  │    Typical formats: YCbCr 4:2:0 planar (HEIF), RGB interleaved   │  │
│  │    (PNG/JPEG), monochrome, YCbCr 4:2:2/4:4:4                     │  │
│  └────────────────────────────┬──────────────────────────────────────┘  │
│                               │                                         │
│                               ▼                                         │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │ 2. Input Format Check & Conversion                               │  │
│  │                                                                   │  │
│  │    Initial implementation — two paths:                            │  │
│  │                                                                   │  │
│  │    ✅ OpenVINO handles directly (zero-copy):                      │  │
│  │      - RGB interleaved → pass directly, no conversion             │  │
│  │      - BGR interleaved → OpenVINO convert_color(BGR→RGB)          │  │
│  │                                                                   │  │
│  │    ❌ libheif fallback (convert_colorspace → RGB interleaved):    │  │
│  │      - HEIF YCbCr planar (4:2:0/4:2:2/4:4:4)                     │  │
│  │      - NV12, I420, GRAY ¹                                         │  │
│  │      - 16-bit HDR images                                          │  │
│  │                                                                   │  │
│  │    ¹ OpenVINO supports NV12/I420/GRAY natively — deferred to     │  │
│  │      future enhancement (requires multi-plane tensor setup)       │  │
│  └────────────────────────────┬──────────────────────────────────────┘  │
│                               │                                         │
│                               ▼                                         │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │ 3. iVSR SR Processing (zero-copy)                                │  │
│  │                                                                   │  │
│  │    OpenVINO prepostProcessor handles internally:                  │  │
│  │      - Input:  NHWC u8 → NCHW f32 (layout + precision)           │  │
│  │      - Input:  RGB → model color format (if needed)               │  │
│  │      - Input:  normalization (scale by normalize_factor)          │  │
│  │      - Output: NCHW f32 → NHWC u8 (layout + precision)           │  │
│  │      - Output: model color format → RGB (if needed)               │  │
│  │      - Output: clamp + quantize to [0, 255]                       │  │
│  │                                                                   │  │
│  │    Input:  HeifPixelImage plane pointer → ivsr_process() input    │  │
│  │    Output: ivsr_process() writes into pre-allocated HeifPixelImage│  │
│  └────────────────────────────┬──────────────────────────────────────┘  │
│                               │                                         │
│                               ▼                                         │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │ 4. Output: RGB interleaved u8                                    │  │
│  │    The scaling function always returns RGB interleaved.           │  │
│  │    The caller (encoder pipeline) handles any further conversion:  │  │
│  │      - x265 encoder → caller converts to YCbCr 4:2:0             │  │
│  │      - SVT-HEVC encoder → caller converts to YCbCr 4:2:0         │  │
│  │      - Other encoders → caller handles as needed                  │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

**Format conversion selection summary (initial implementation):**

| Input Image Format | Who Converts | Method | Zero-Copy Input |
|-------------------|--------------|--------|-----------------|
| RGB interleaved u8 | — (no conversion needed) | Pass pointer directly | ✅ Yes |
| BGR interleaved u8 | OpenVINO prepostProcessor | `set_color_format(BGR)`, `convert_color(RGB)` | ✅ Yes |
| NV12 | libheif `convert_colorspace()` ¹ | Convert to RGB interleaved first | ❌ No (copy) |
| I420 | libheif `convert_colorspace()` ¹ | Convert to RGB interleaved first | ❌ No (copy) |
| GRAY | libheif `convert_colorspace()` ¹ | Convert to RGB interleaved first | ❌ No (copy) |
| HEIF YCbCr planar (4:2:0/4:2:2/4:4:4) | libheif `convert_colorspace()` | Convert to RGB interleaved first | ❌ No (copy) |
| 16-bit HDR | libheif `convert_colorspace()` | Convert to 8-bit RGB first | ❌ No (copy) |

¹ OpenVINO prepostProcessor supports NV12, I420, and GRAY color conversion natively.
In a future enhancement, these could be passed directly to OpenVINO for zero-copy
processing using multi-plane tensor setup. The initial implementation uses libheif
fallback for simplicity.

#### 4.3.3 Input Preparation (HeifPixelImage → iVSR)

```cpp
// Get a pointer to image data suitable for iVSR input.
// Determines the best conversion path based on input format:
//   1. OpenVINO-compatible format → pass pointer directly (zero-copy),
//      set tensor descriptor color format so prepostProcessor handles it
//   2. HEIF YCbCr planar → libheif convert_colorspace() to RGB first (fallback)
//
// Returns the plane pointer and sets tensor_color_format for the input tensor descriptor.
static uint8_t* prepare_ivsr_input(
    std::shared_ptr<HeifPixelImage>& image,
    std::shared_ptr<HeifPixelImage>& converted_image,  // holds converted image if needed
    const char** tensor_color_format,
    size_t* out_stride)
{
    heif_colorspace cs = image->get_colorspace();
    heif_chroma chroma = image->get_chroma_format();

    // Path 1: OpenVINO prepostProcessor can handle these formats directly
    if (cs == heif_colorspace_RGB && chroma == heif_chroma_interleaved_RGB) {
        *tensor_color_format = "RGB";
        return image->get_plane(heif_channel_interleaved, out_stride);
    }
    if (cs == heif_colorspace_RGB && chroma == heif_chroma_interleaved_BGR) {
        *tensor_color_format = "BGR";
        return image->get_plane(heif_channel_interleaved, out_stride);
    }
    // NV12 and I420 could also be passed directly to OpenVINO, but they
    // require multi-plane tensor setup. For simplicity in the first step,
    // we fall through to libheif conversion for these planar formats.

    // Path 2: libheif fallback — convert to RGB interleaved
    // Handles: HEIF YCbCr planar (4:2:0/4:2:2/4:4:4), monochrome, 16-bit, etc.
    converted_image = convert_colorspace(image,
                                          heif_colorspace_RGB,
                                          heif_chroma_interleaved_RGB,
                                          nullptr, 8, nullptr);
    if (!converted_image) {
        return nullptr;  // caller should return error
    }
    *tensor_color_format = "RGB";
    return converted_image->get_plane(heif_channel_interleaved, out_stride);
}
```

#### 4.3.4 Output Handling (iVSR output → HeifPixelImage, zero-copy)

```cpp
// Pre-allocate an output HeifPixelImage and return its plane pointer
// so iVSR can write directly into it — no intermediate buffer or copy.
// iVSR's prepostProcessor already handles NCHW fp32 → NHWC u8 conversion
// and clamping based on the output tensor descriptor.
static uint8_t* create_output_image_and_get_ptr(
    std::shared_ptr<HeifPixelImage>& out_img,
    int width, int height, size_t* out_stride)
{
    out_img = std::make_shared<HeifPixelImage>();
    out_img->create(width, height, heif_colorspace_RGB, heif_chroma_interleaved_RGB);
    out_img->add_plane(heif_channel_interleaved, width, height, 8, nullptr);

    return out_img->get_plane(heif_channel_interleaved, out_stride);
}

// Output is always RGB interleaved u8 — no colorspace conversion is performed
// by the scaling function. The caller (encoder pipeline) handles any further
// conversion needed for the target codec:
//   - x265 encoder: caller converts to YCbCr 4:2:0
//   - SVT-HEVC encoder: caller converts to YCbCr 4:2:0
//   - Other encoders: caller handles as needed
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
| `libheif/api/libheif/heif_image.h` | Define `heif_scaling_options` struct (model path + device), add `heif_scaling_algorithm` enum, add `heif_scaling_options_alloc/free` |
| `libheif/api/libheif/heif_image.cc` | Update `heif_image_scale_image()` to dispatch to iVSR for exact 2×/4× upscaling |
| `libheif/api/libheif/heif_cxx.h` | Extend `ScalingOptions` C++ class |
| `examples/heif_enc.cc` | Add `--sr-model` and `--sr-device` options, pass `heif_scaling_options` to scale API |
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
 * @brief Scale an image using iVSR AI super resolution (Enhanced EDSR fp32).
 *
 * This function is called internally by heif_image_scale_image() when
 * the scaling options specify super resolution algorithm and the target
 * dimensions are exactly 2× or 4× of the source.
 *
 * For 4× scaling, two passes of 2× SR are applied internally.
 *
 * @param input       Source image
 * @param output      Pointer to receive the scaled output image
 * @param width       Target width (must be exactly 2× or 4× of source)
 * @param height      Target height (must be exactly 2× or 4× of source)
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

    // EDSR fp32 defaults (hardcoded for first step)
    const int scale_factor = 2;         // EDSR produces 2× output per pass
    const float normalize_factor = 1.0f; // Enhanced EDSR uses normalize_factor=1.0
    const char* precision = "f32";       // fp32 inference

    // Determine number of passes: 1 for 2×, 2 for 4×
    // The dispatcher guarantees target is exactly 2× or 4× of source
    int num_passes = (target_width == src_width * 4) ? 2 : 1;

    // --- Step 1: Prepare input — select best conversion path ---
    // Format conversion selection logic (initial implementation):
    //   1. RGB, BGR interleaved → OpenVINO prepostProcessor handles directly (zero-copy)
    //   2. All other formats (HEIF YCbCr planar, NV12, I420, GRAY, 16-bit HDR)
    //      → fall back to libheif convert_colorspace() to RGB interleaved
    // Note: OpenVINO can handle NV12/I420/GRAY natively, but multi-plane tensor
    // setup is deferred to a future enhancement.
    std::shared_ptr<HeifPixelImage> converted_image;
    const char* tensor_color_format = nullptr;
    size_t in_stride;
    uint8_t* input_ptr = nullptr;

    heif_colorspace cs = src_image->get_colorspace();
    heif_chroma chroma = src_image->get_chroma_format();

    if (cs == heif_colorspace_RGB && chroma == heif_chroma_interleaved_RGB) {
        // OpenVINO: RGB interleaved — zero-copy, no conversion needed
        tensor_color_format = "RGB";
        input_ptr = src_image->get_plane(heif_channel_interleaved, &in_stride);
    } else if (cs == heif_colorspace_RGB && chroma == heif_chroma_interleaved_BGR) {
        // OpenVINO: BGR interleaved — zero-copy, prepostProcessor converts BGR→RGB
        tensor_color_format = "BGR";
        input_ptr = src_image->get_plane(heif_channel_interleaved, &in_stride);
    } else {
        // libheif fallback: HEIF YCbCr planar (4:2:0/4:2:2/4:4:4), monochrome,
        // 16-bit HDR, or other formats OpenVINO cannot handle directly.
        // Convert to RGB interleaved using libheif's convert_colorspace().
        converted_image = convert_colorspace(src_image,
                                              heif_colorspace_RGB,
                                              heif_chroma_interleaved_RGB,
                                              nullptr, /*target_profile*/
                                              8,       /*output_bpp*/
                                              nullptr  /*security_limits*/);
        if (!converted_image) {
            return {heif_error_Encoding_error, heif_suberror_Unspecified,
                    "Failed to convert image to RGB for iVSR"};
        }
        tensor_color_format = "RGB";
        input_ptr = converted_image->get_plane(heif_channel_interleaved, &in_stride);
    }

    // --- Step 2: Build iVSR configuration linked list ---
    // Simplified for Enhanced EDSR fp32 model (no extension libs or custom ops needed)
    // The tensor_color_format is set based on the input format check above.
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

    add_config(PRECISION, precision);

    std::string input_res = std::to_string(src_width) + "," + std::to_string(src_height);
    add_config(INPUT_RES, input_res.c_str());

    // Configure tensor descriptors for Enhanced EDSR fp32 model.
    // iVSR's prepostProcessor uses these descriptors to handle all format
    // conversions internally:
    //   Input:  NHWC u8 <tensor_color_format> (what we provide) → model's expected format
    //   Output: model's internal format → NHWC u8 RGB (what we receive)
    // This eliminates the need for manual BGR swapping, layout transposition,
    // or precision conversion in our code.
    // tensor_color_format is set dynamically based on input format:
    //   "RGB" if input is RGB interleaved (or converted from YCbCr)
    //   "BGR" if input is BGR interleaved (OpenVINO handles BGR→RGB)
    tensor_desc_t input_tensor_desc = {
        .precision = "u8",
        .layout = "NHWC",
        .tensor_color_format = tensor_color_format,
        .model_color_format = "RGB",
        .scale = normalize_factor,  // 1.0 for EDSR
        .dimension = 4,
        .shape = {1, (size_t)src_height, (size_t)src_width, 3}
    };
    tensor_desc_t output_tensor_desc = {
        .precision = "u8",
        .layout = "NHWC",
        .tensor_color_format = "RGB",
        .model_color_format = "RGB",
        .scale = normalize_factor,  // 1.0 for EDSR
        .dimension = 4,
        .shape = {1, (size_t)(src_height * scale_factor), (size_t)(src_width * scale_factor), 3}
    };
    add_config(INPUT_TENSOR_DESC_SETTING, &input_tensor_desc);
    add_config(OUTPUT_TENSOR_DESC_SETTING, &output_tensor_desc);

    // --- Step 3: Initialize iVSR ---
    ivsr_handle handle = nullptr;
    IVSRStatus status = ivsr_init(&configs[0], &handle);
    if (status != OK) {
        return {heif_error_Plugin_loading_error, heif_suberror_Unspecified,
                "Failed to initialize iVSR engine"};
    }

    // --- Step 4: Query actual output dimensions ---
    tensor_desc_t actual_output_desc = {0};
    ivsr_get_attr(handle, OUTPUT_TENSOR_DESC, &actual_output_desc);
    // NHWC layout: shape = {N, H, W, C}
    int sr_height = actual_output_desc.shape[1];
    int sr_width  = actual_output_desc.shape[2];

    // --- Step 5: Pre-allocate output HeifPixelImage and run inference (zero-copy) ---
    // Allocate the output image first, then pass its plane pointer directly
    // to iVSR so it writes the result in-place — no intermediate buffer.
    auto out_img = std::make_shared<HeifPixelImage>();
    out_img->create(sr_width, sr_height, heif_colorspace_RGB, heif_chroma_interleaved_RGB);
    out_img->add_plane(heif_channel_interleaved, sr_width, sr_height, 8, nullptr);

    size_t out_stride;
    uint8_t* output_ptr = out_img->get_plane(heif_channel_interleaved, &out_stride);

    int completion_flag = 0;
    ivsr_cb_t cb;
    cb.ivsr_cb = ivsr_completion_callback;
    cb.args = &completion_flag;

    // Pass input plane pointer and output plane pointer directly to iVSR.
    // iVSR reads from input_ptr and writes to output_ptr — zero data copy
    // when input is in an OpenVINO-compatible format.
    // Note: ivsr_process() does not modify the input buffer.
    status = ivsr_process(handle, reinterpret_cast<char*>(input_ptr),
                          reinterpret_cast<char*>(output_ptr), &cb);
    if (status != OK) {
        ivsr_deinit(handle);
        return {heif_error_Encoding_error, heif_suberror_Unspecified,
                "iVSR processing failed"};
    }

    // --- Step 6: Return result ---
    // Output is always RGB interleaved u8 — no colorspace conversion performed.
    // The caller (encoder pipeline) handles any further conversion needed
    // for the target codec (x265 → YCbCr 4:2:0, SVT-HEVC → YCbCr 4:2:0, etc.).
    *output = new heif_image;
    (*output)->image = std::move(out_img);

    // --- Step 7: Cleanup ---
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
| `algorithm == super_resolution` but scale is not 2× or 4× | Use nearest-neighbor (decided at dispatch) |
| `ivsr_model_path` is NULL | Return `heif_error_Usage_error` |
| Model file not found | Return `heif_error_Plugin_loading_error` (from iVSR init failure) |
| iVSR init fails | Return `heif_error_Plugin_loading_error` with iVSR status |
| iVSR inference fails | Return `heif_error_Encoding_error`, cleanup resources |
| RGB/BGR interleaved input | OpenVINO prepostProcessor handles directly (zero-copy) |
| HEIF YCbCr planar input | libheif `convert_colorspace()` converts to RGB first (fallback) |
| HDR (16-bit) input | libheif `convert_colorspace()` converts to 8-bit RGB first (fallback) |

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

## 8. Scaling Strategy

iVSR Enhanced EDSR model produces a fixed 2× scale factor per pass. The dispatcher routes scaling requests as follows:

| Requested Scale | Algorithm Used |
|-----------------|---------------|
| Exactly 2× (w×2, h×2) | iVSR SR (single pass) |
| Exactly 4× (w×4, h×4) | iVSR SR (two passes of 2×) |
| Any other factor | Nearest-neighbor (SR not applicable) |

```
Example 1: 640×480 → 1280×960 (2×)
  → Single iVSR 2× SR pass

Example 2: 640×480 → 2560×1920 (4×)
  → Pass 1: iVSR 2× SR: 640×480 → 1280×960
  → Pass 2: iVSR 2× SR: 1280×960 → 2560×1920

Example 3: 640×480 → 1920×1080 (3×, non-exact)
  → Nearest-neighbor scaling (iVSR not used)
```

---

## 9. Usage Example: heif_enc Integration

The iVSR super resolution is integrated into the `heif_enc` encoder sample. This adds two new command-line options to enable SR when using the existing `--scale` option:

### 9.1 New Command-Line Options for heif_enc

```
--sr-model <path>    Path to Enhanced EDSR OpenVINO IR model (.xml). Enables SR for --scale.
--sr-device <device> Device for SR inference: CPU (default) or GPU.
```

### 9.2 Usage

```bash
# Scale up 2× using AI super resolution on CPU
heif_enc input.png -o output.heif --scale 1280x960 \
         --sr-model /models/enhanced_edsr.xml

# Scale up 2× using AI super resolution on GPU
heif_enc input.png -o output.heif --scale 1280x960 \
         --sr-model /models/enhanced_edsr.xml --sr-device GPU

# Scale up 4× using AI super resolution (two SR passes)
heif_enc input.png -o output.heif --scale 2560x1920 \
         --sr-model /models/enhanced_edsr.xml

# Non-2×/4× scaling falls back to nearest-neighbor even if --sr-model is set
heif_enc input.png -o output.heif --scale 1920x1080 \
         --sr-model /models/enhanced_edsr.xml
```

### 9.3 heif_enc Code Changes

```cpp
// In examples/heif_enc.cc

// New option constants
const int OPTION_SR_MODEL  = 1042;
const int OPTION_SR_DEVICE = 1043;

// New global variables
std::string sr_model_path;
std::string sr_device = "CPU";

// Add to long_options array:
{(char* const) "sr-model",  required_argument, nullptr, OPTION_SR_MODEL},
{(char* const) "sr-device", required_argument, nullptr, OPTION_SR_DEVICE},

// Add to option parsing:
case OPTION_SR_MODEL:
    sr_model_path = optarg;
    break;
case OPTION_SR_DEVICE:
    sr_device = optarg;
    break;

// Update scaling section:
if (scale_width > 0 && scale_height > 0) {
    heif_image* scaled_image = nullptr;
    heif_scaling_options* scale_opts = nullptr;

    // If SR model is provided, configure super resolution
    if (!sr_model_path.empty()) {
        scale_opts = heif_scaling_options_alloc();
        scale_opts->algorithm = heif_scaling_algorithm_super_resolution;
        scale_opts->ivsr_model_path = sr_model_path.c_str();
        scale_opts->ivsr_device = sr_device.c_str();
    }

    heif_error err = heif_image_scale_image(image.get(), &scaled_image,
                                            scale_width, scale_height,
                                            scale_opts);
    if (scale_opts) {
        heif_scaling_options_free(scale_opts);
    }
    if (err.code) {
        std::cerr << "Could not scale image: " << err.message << "\n";
        return 1;
    }
    image = std::shared_ptr<heif_image>(scaled_image, heif_image_release);
}
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
| `test_scale_non_2x_4x_sr` | Verify non-2×/4× scale with SR falls back to nearest-neighbor |
| `test_scale_options_alloc_free` | Verify alloc/free lifecycle |

### 11.2 Integration Tests (require iVSR SDK + model files)

| Test Case | Description |
|-----------|-------------|
| `test_sr_rgb_2x` | 2× upscale of RGB image via EDSR |
| `test_sr_ycbcr_2x` | 2× upscale of YCbCr 4:2:0 image (auto-convert) |
| `test_sr_4x` | 4× upscale via two SR passes |
| `test_sr_gpu_device` | Run inference on GPU |
| `test_sr_heif_enc` | heif_enc --scale with --sr-model end-to-end |

### 11.3 Quality Validation

- Compare SR output against nearest-neighbor and bilinear interpolation using PSNR/SSIM metrics
- Visual inspection of edge preservation and detail restoration

---

## 12. Future Enhancements

1. **Additional SR models**: Support Enhanced BasicVSR (multi-frame) and TSENet models with configurable options.
2. **Model auto-detection**: Automatically select the best model based on image characteristics.
3. **Handle caching**: Reuse iVSR handles across multiple `heif_image_scale_image` calls.
4. **Async API**: Add `heif_image_scale_image_async` for non-blocking SR processing.
5. **Additional SR backends**: Support other SR engines beyond iVSR (e.g., ONNX Runtime, TensorRT).
6. **Dynamic plugin loading**: Package iVSR integration as a dynamically loadable libheif plugin.
7. **HDR support**: Support 16-bit HDR images through appropriate model and conversion pipeline.
8. **Bilinear/bicubic algorithms**: Add traditional interpolation algorithms as intermediate options between nearest-neighbor and AI SR.
9. **Arbitrary scale factors**: Support non-2×/4× upscaling by combining SR with final resize.

---

## 13. Summary

This design integrates iVSR AI super resolution (Enhanced EDSR fp32) into libheif's existing scaling API with minimal changes:

- **1 new enum** (`heif_scaling_algorithm`) for algorithm selection
- **1 struct definition** (`heif_scaling_options`) completing the existing forward declaration (model path + device only)
- **2 new helper functions** (`heif_scaling_options_alloc/free`) for lifecycle management
- **2 new source files** (`ivsr_scaling_plugin.cc/.h`) for the iVSR bridge
- **Minor updates** to the scale dispatcher, CMake build system, C++ wrapper, and heif_enc sample

The dispatcher selects the algorithm upfront: iVSR is used only for exact 2× or 4× upscaling; all other scale factors use nearest-neighbor. The design preserves full backward compatibility — existing code passing `NULL` for scaling options continues to work identically. iVSR is an optional compile-time dependency, and the feature gracefully degrades when not available.
