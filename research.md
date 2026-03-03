# Research Report: JPEG Decoder Output to SVT-HEVC Encoder Input — Memory Layout Compatibility Analysis

## 1. Executive Summary

This report analyzes whether the image data output by libheif's JPEG decoder can be directly fed into the SVT-HEVC encoder without intermediate memory copies or format conversions. The analysis covers both projects' internal memory layouts, colorspace representations, stride/padding strategies, and the actual data flow within libheif's plugin architecture.

**Key Finding:** In the current libheif architecture, the JPEG decoder output can be **directly passed** to the SVT-HEVC encoder **without any pixel data copying or format conversion**, because both operate on the same `HeifPixelImage` planar YCbCr 4:2:0 representation. The SVT-HEVC encoder plugin reads plane pointers and strides directly from the `HeifPixelImage` that the JPEG decoder produced. However, there are important nuances regarding stride semantics, alignment padding, and edge cases that must be understood.

---

## 2. Project Overview

### 2.1 libheif

libheif is a C/C++ library for reading and writing HEIF (High Efficiency Image File Format) and AVIF image files. It supports multiple codec backends through a plugin architecture, including:

- **Decoders:** JPEG, HEVC (libde265, ffmpeg), AVC, AV1, JPEG 2000, HTJ2K, VVC, etc.
- **Encoders:** HEVC (x265, SVT-HEVC), AV1 (aom, rav1e, SVT-AV1), JPEG, JPEG 2000, etc.

The core data structure for decoded images is `HeifPixelImage`, which stores pixel data in a planar format with per-plane strides and alignment.

### 2.2 SVT-HEVC (Scalable Video Technology for HEVC)

SVT-HEVC is an open-source HEVC encoder developed by Intel (OpenVisualCloud). It provides a C API (`EbApi.h`) for encoding raw YUV frames into HEVC/H.265 bitstreams. It accepts input via the `EB_H265_ENC_INPUT` structure, which contains pointers to separate Y, Cb, Cr planes with per-plane strides.

---

## 3. JPEG Decoder Output Memory Layout (libheif)

### 3.1 Source Files

- Plugin decoder: `libheif/plugins/decoder_jpeg.cc`
- Codec-level parser: `libheif/codecs/jpeg_dec.cc`

### 3.2 Decoding Process

The JPEG decoder plugin uses `libjpeg` (or `libjpeg-turbo`) to decompress JPEG data. The key steps are:

1. **Initialize libjpeg decompressor** (`jpeg_create_decompress`)
2. **Set output colorspace** to `JCS_YCbCr` (for color images) or `JCS_GRAYSCALE` (for grayscale)
3. **Create a `heif_image`** with the appropriate colorspace and chroma format
4. **Allocate planes** in the `HeifPixelImage`
5. **Read scanlines** from libjpeg and manually de-interleave into separate Y, Cb, Cr planes

### 3.3 Output Format

| Property | Value |
|----------|-------|
| **Colorspace** | `heif_colorspace_YCbCr` (color) or `heif_colorspace_monochrome` (grayscale) |
| **Chroma format** | `heif_chroma_420` (always 4:2:0 for color JPEG) |
| **Bit depth** | 8 bits per component |
| **Storage layout** | **Planar** — separate Y, Cb, Cr buffers |
| **Data type** | `uint8_t` (unsigned 8-bit integer) |

### 3.4 Plane Dimensions

For a JPEG image with dimensions `W × H`:

| Plane | Width | Height |
|-------|-------|--------|
| **Y (Luma)** | `W` | `H` |
| **Cb (Chroma Blue)** | `(W+1)/2` | `(H+1)/2` |
| **Cr (Chroma Red)** | `(W+1)/2` | `(H+1)/2` |

### 3.5 Scanline De-interleaving

libjpeg outputs interleaved YCbCr samples when `out_color_space = JCS_YCbCr`. The decoder plugin manually de-interleaves this into planar format with 4:2:0 chroma subsampling:

```c
// For every pair of rows (even row y and odd row y+1):
// Even row: extract Y1, Cb, Cr, Y2 from interleaved buffer → store in separate planes
// Odd row: extract only Y values (chroma is shared with even row for 4:2:0)

// Even scanline processing:
for (x = 0; x < width; x += 2) {
    py[y * y_stride + x]         = Y1;     // First luma sample
    pcb[y/2 * cb_stride + x/2]   = Cb;     // Chroma blue (shared with next row)
    pcr[y/2 * cr_stride + x/2]   = Cr;     // Chroma red (shared with next row)
    py[y * y_stride + x + 1]     = Y2;     // Second luma sample
}

// Odd scanline processing:
for (x = 0; x < width; x++) {
    py[y * y_stride + x] = Y;              // Only luma, chroma reused from even row
}
```

### 3.6 Memory Alignment and Stride

When `heif_image_add_plane_safe()` is called, the `HeifPixelImage::ImageComponent::alloc()` method allocates memory with:

1. **Width rounding:** `m_mem_width = rounded_size(width)`, where `rounded_size()` rounds up to the next even number with a minimum of 64 pixels.
2. **Stride calculation:** `stride = m_mem_width * bytes_per_pixel`, then rounded up to 16-byte alignment: `stride = (stride + 15) & ~15`.
3. **Memory alignment:** The pixel data pointer is aligned to a 16-byte boundary.

**Example:** For a 1920×1080 JPEG image:

| Plane | Logical Size | `m_mem_width` | `stride` (bytes) |
|-------|-------------|---------------|-------------------|
| Y     | 1920×1080   | 1920          | 1920 (already 16-aligned) |
| Cb    | 960×540     | 960           | 960 (already 16-aligned) |
| Cr    | 960×540     | 960           | 960 (already 16-aligned) |

**Example:** For a 1001×751 JPEG image:

| Plane | Logical Size | `m_mem_width` | `stride` (bytes) |
|-------|-------------|---------------|-------------------|
| Y     | 1001×751    | 1002          | 1008 (rounded to 16) |
| Cb    | 501×376     | 502           | 512 (rounded to 16) |
| Cr    | 501×376     | 502           | 512 (rounded to 16) |

**Key observation:** The stride may be larger than the logical width due to alignment padding. Pixel data within a row occupies only the first `width` bytes; the remaining `stride - width` bytes are padding.

---

## 4. SVT-HEVC Encoder Input Memory Layout

### 4.1 Source Files

- SVT-HEVC API: `Source/API/EbApi.h` (in [OpenVisualCloud/SVT-HEVC](https://github.com/OpenVisualCloud/SVT-HEVC))
- libheif plugin: `libheif/plugins/encoder_svt_hevc.cc`

### 4.2 EB_H265_ENC_INPUT Structure

```c
typedef struct EB_H265_ENC_INPUT
{
    // Hosts 8 bit or 16 bit input YUV420p / YUV420p10le
    uint8_t *luma;
    uint8_t *cb;
    uint8_t *cr;

    // Hosts LSB 2 bits of 10bit input when the compressed 10bit format is used
    uint8_t *lumaExt;
    uint8_t *cbExt;
    uint8_t *crExt;

    uint32_t yStride;
    uint32_t crStride;
    uint32_t cbStride;
    EB_SEI_MESSAGE dolbyVisionRpu;
} EB_H265_ENC_INPUT;
```

### 4.3 Input Requirements

| Property | Value |
|----------|-------|
| **Color format** | `EB_YUV420` (YUV 4:2:0 only) |
| **Bit depth** | 8-bit or 10-bit |
| **Layout** | **Planar** — separate luma, cb, cr buffers |
| **Stride unit** | **Samples** (not bytes) for 8-bit; divide byte-stride by 2 for 10-bit |
| **Plane pointers** | Direct pointers to raw pixel data |

### 4.4 How libheif Feeds Data to SVT-HEVC

From `encoder_svt_hevc.cc` (lines 722–746):

```c
EB_H265_ENC_INPUT input_pic;
memset(&input_pic, 0, sizeof(input_pic));

int y_stride, cb_stride, cr_stride;

// Get plane pointers directly from HeifPixelImage
input_pic.luma    = (uint8_t*) heif_image_get_plane_readonly(image, heif_channel_Y, &y_stride);
input_pic.yStride = y_stride / (bit_depth > 8 ? 2 : 1);

input_pic.cb       = (uint8_t*) heif_image_get_plane_readonly(image, heif_channel_Cb, &cb_stride);
input_pic.cbStride = cb_stride / (bit_depth > 8 ? 2 : 1);

input_pic.cr       = (uint8_t*) heif_image_get_plane_readonly(image, heif_channel_Cr, &cr_stride);
input_pic.crStride = cr_stride / (bit_depth > 8 ? 2 : 1);

// Pass structure to SVT-HEVC
input_buffer.pBuffer   = (uint8_t*) &input_pic;
input_buffer.nAllocLen = sizeof(EB_H265_ENC_INPUT);
input_buffer.nFilledLen= sizeof(EB_H265_ENC_INPUT);
```

**Critical detail:** The encoder plugin reads plane pointers and strides **directly** from the `HeifPixelImage`. No pixel data is copied. The `EB_H265_ENC_INPUT` structure merely holds pointers into the existing `HeifPixelImage` memory.

### 4.5 SVT-HEVC Internal Handling of Stride

SVT-HEVC accepts stride values in **sample units** (not bytes). For 8-bit content, 1 sample = 1 byte, so the byte stride equals the sample stride. For 10-bit content, samples are stored as 16-bit values, so the byte stride must be divided by 2.

SVT-HEVC internally copies the input planes into its own padded buffers for processing. The encoder handles the stride difference between input data and its internal representation. Therefore, **any stride value is acceptable** — SVT-HEVC does not require a specific alignment or padding scheme for input data.

---

## 5. Compatibility Analysis

### 5.1 Side-by-Side Comparison

| Aspect | JPEG Decoder Output | SVT-HEVC Encoder Input | Compatible? |
|--------|-------------------|----------------------|-------------|
| **Colorspace** | YCbCr | YCbCr (YUV) | ✅ Yes |
| **Chroma format** | 4:2:0 | 4:2:0 (EB_YUV420) | ✅ Yes |
| **Bit depth** | 8-bit | 8-bit or 10-bit | ✅ Yes (8-bit) |
| **Storage layout** | Planar (Y/Cb/Cr) | Planar (luma/cb/cr) | ✅ Yes |
| **Data type** | uint8_t | uint8_t (8-bit) | ✅ Yes |
| **Stride unit** | Bytes | Samples (=bytes for 8-bit) | ✅ Yes |
| **Y plane size** | W × H | W × H | ✅ Yes |
| **Cb/Cr plane size** | (W+1)/2 × (H+1)/2 | W/2 × H/2 | ⚠️ See §5.3 |
| **Stride alignment** | 16-byte aligned | No requirement | ✅ Yes |
| **Memory alignment** | 16-byte aligned start | No requirement | ✅ Yes |
| **Grayscale** | Monochrome (Y only) | Supported (cb/cr = NULL) | ✅ Yes |

### 5.2 Data Flow in Current libheif Architecture

```
┌─────────────────────┐
│   JPEG Compressed   │
│       Data          │
└─────────┬───────────┘
          │
          ▼
┌─────────────────────┐
│  JPEG Decoder Plugin │  (decoder_jpeg.cc)
│  - libjpeg decompress│
│  - De-interleave to  │
│    planar YCbCr 420  │
└─────────┬───────────┘
          │ Creates HeifPixelImage
          │ with planar Y/Cb/Cr planes
          ▼
┌─────────────────────┐
│   HeifPixelImage     │  (pixelimage.h/cc)
│  ┌───────────────┐   │
│  │ Y Plane       │   │  stride: aligned to 16 bytes
│  │ (W × H)       │   │  pointer: aligned to 16 bytes
│  └───────────────┘   │
│  ┌───────────────┐   │
│  │ Cb Plane      │   │  stride: aligned to 16 bytes
│  │ ((W+1)/2 ×    │   │  pointer: aligned to 16 bytes
│  │  (H+1)/2)     │   │
│  └───────────────┘   │
│  ┌───────────────┐   │
│  │ Cr Plane      │   │  stride: aligned to 16 bytes
│  │ ((W+1)/2 ×    │   │  pointer: aligned to 16 bytes
│  │  (H+1)/2)     │   │
│  └───────────────┘   │
└─────────┬───────────┘
          │ Direct pointer access
          │ (NO copy)
          ▼
┌─────────────────────┐
│ SVT-HEVC Encoder    │  (encoder_svt_hevc.cc)
│ Plugin              │
│                     │
│ EB_H265_ENC_INPUT:  │
│  .luma   → Y ptr    │  Zero-copy: pointers reference
│  .cb     → Cb ptr   │  HeifPixelImage memory directly
│  .cr     → Cr ptr   │
│  .yStride  = stride  │
│  .cbStride = stride  │
│  .crStride = stride  │
└─────────────────────┘
```

**The current architecture already achieves zero-copy transfer.** The SVT-HEVC encoder plugin obtains plane pointers via `heif_image_get_plane_readonly()` and passes them directly to SVT-HEVC through `EB_H265_ENC_INPUT`. No intermediate buffer allocation or pixel-level copy occurs.

### 5.3 Edge Case: Odd-Dimension Images

There is a subtle difference in how chroma plane dimensions are calculated:

- **libheif JPEG decoder:** Cb/Cr dimensions = `(W+1)/2 × (H+1)/2` (rounds up)
- **SVT-HEVC convention:** typically expects `W/2 × H/2` for even dimensions

For images with **even** width and height (which is the common case and also what JPEG typically produces), these formulas yield the same result. For **odd** dimensions, libheif allocates one extra row/column in the chroma planes, which is harmless — SVT-HEVC will simply read `W/2 × H/2` samples and ignore any extra data beyond its expected dimensions.

The SVT-HEVC encoder configuration (`EB_H265_ENC_CONFIGURATION`) receives the image `sourceWidth` and `sourceHeight`, and internally computes chroma plane dimensions from these values. The extra padded memory in libheif's chroma planes will not be accessed beyond the expected area.

### 5.4 Stride Padding Analysis

The libheif `HeifPixelImage` allocates planes with potential stride padding:

- `m_mem_width ≥ logical_width` (rounded to even, minimum 64)
- `stride ≥ m_mem_width * bytes_per_pixel` (rounded to 16-byte boundary)

This means `stride ≥ logical_width` for 8-bit data. SVT-HEVC accepts arbitrary stride values through `yStride`, `cbStride`, `crStride` — it reads exactly `width` samples per row and then skips to the next row using the provided stride. Therefore:

**✅ The stride padding in libheif's HeifPixelImage is fully compatible with SVT-HEVC's stride-based access model.**

### 5.5 What About JPEG Images with Non-4:2:0 Chroma?

The JPEG standard supports multiple chroma subsampling modes (4:4:4, 4:2:2, 4:2:0). However, examining `decoder_jpeg.cc`:

```c
cinfo.out_color_space = JCS_YCbCr;
// ...
heif_image_create(width, height, heif_colorspace_YCbCr, heif_chroma_420, &heif_img);
```

The libheif JPEG decoder **always** creates a 4:2:0 output image regardless of the source JPEG's native chroma. libjpeg is configured to output interleaved YCbCr, and the plugin manually performs 4:2:0 downsampling during de-interleaving. This means:

- **4:2:0 JPEG → 4:2:0 output** ✅ (native, no quality loss in chroma)
- **4:2:2 JPEG → 4:2:0 output** ⚠️ (implicit chroma downsampling, some quality loss)
- **4:4:4 JPEG → 4:2:0 output** ⚠️ (implicit chroma downsampling, more quality loss)

Since SVT-HEVC only accepts 4:2:0, this forced downsampling is actually **necessary** for encoder compatibility.

---

## 6. Detailed Memory Layout Diagrams

### 6.1 HeifPixelImage Y Plane (8-bit, 1920×1080)

```
Address (16-byte aligned)
│
├─ Row 0:   [Y(0,0)] [Y(1,0)] [Y(2,0)] ... [Y(1919,0)] [pad...] ← stride bytes
├─ Row 1:   [Y(0,1)] [Y(1,1)] [Y(2,1)] ... [Y(1919,1)] [pad...]
├─ Row 2:   [Y(0,2)] [Y(1,2)] [Y(2,2)] ... [Y(1919,2)] [pad...]
│   ...
├─ Row 1079: [Y(0,1079)] ... [Y(1919,1079)]              [pad...]
├─ (extra rows up to m_mem_height, uninitialized)
└─ End of allocation

stride = 1920 bytes (1920 is already 16-byte aligned)
m_mem_width = 1920, m_mem_height = 1080
```

### 6.2 EB_H265_ENC_INPUT Mapping

```
EB_H265_ENC_INPUT:
┌──────────────┬─────────────────────────────────────────────┐
│ .luma        │ → HeifPixelImage Y plane .mem pointer       │
│ .yStride     │ = HeifPixelImage Y plane .stride (bytes)    │
│ .cb          │ → HeifPixelImage Cb plane .mem pointer      │
│ .cbStride    │ = HeifPixelImage Cb plane .stride (bytes)   │
│ .cr          │ → HeifPixelImage Cr plane .mem pointer      │
│ .crStride    │ = HeifPixelImage Cr plane .stride (bytes)   │
│ .lumaExt     │ = NULL (8-bit mode)                         │
│ .cbExt       │ = NULL (8-bit mode)                         │
│ .crExt       │ = NULL (8-bit mode)                         │
└──────────────┴─────────────────────────────────────────────┘
```

---

## 7. Can JPEG Decoder Output Be Directly Fed to SVT-HEVC?

### 7.1 Answer: Yes — And It Already Is

In the current libheif implementation, when a JPEG image is transcoded to HEIC format using the SVT-HEVC encoder:

1. The JPEG decoder plugin creates a `HeifPixelImage` with planar YCbCr 4:2:0 data.
2. The SVT-HEVC encoder plugin receives the same `heif_image` object.
3. The encoder extracts plane pointers using `heif_image_get_plane_readonly()`.
4. These pointers point directly into the `HeifPixelImage`'s memory — **no copy occurs**.
5. SVT-HEVC internally reads from these pointers using the provided stride values.

### 7.2 Why This Works

The compatibility is achieved because:

1. **Same colorspace:** Both use YCbCr.
2. **Same chroma subsampling:** Both use 4:2:0.
3. **Same bit depth:** JPEG outputs 8-bit, SVT-HEVC accepts 8-bit.
4. **Same storage layout:** Both use planar (separate Y/Cb/Cr buffers).
5. **Stride-aware access:** SVT-HEVC uses per-plane stride values, accommodating any padding/alignment in the source buffers.
6. **Pointer-based interface:** `EB_H265_ENC_INPUT` holds pointers, not copies, so zero-copy transfer is natural.

### 7.3 Conditions Where Direct Transfer Would NOT Work

The following hypothetical scenarios would break direct compatibility:

| Scenario | Issue | Solution |
|----------|-------|----------|
| 10-bit JPEG output | JPEG only outputs 8-bit; SVT-HEVC 10-bit needs different stride handling | N/A (JPEG is always 8-bit) |
| JPEG outputs RGB | SVT-HEVC expects YCbCr | Color conversion needed |
| JPEG outputs interleaved YCbCr | SVT-HEVC expects planar | De-interleaving needed |
| JPEG outputs 4:4:4 or 4:2:2 directly | SVT-HEVC only accepts 4:2:0 | Chroma downsampling needed |

In the current implementation, all of these are already handled: the JPEG decoder always outputs planar YCbCr 4:2:0 8-bit data, which exactly matches SVT-HEVC's requirements.

---

## 8. Performance Implications

### 8.1 Zero-Copy Advantage

The current architecture avoids an extra memory copy by sharing plane pointers between the decoder output and encoder input. For a 4K image (3840×2160), this saves copying approximately:

- Y plane: 3840 × 2160 = 8,294,400 bytes ≈ 7.9 MB
- Cb plane: 1920 × 1080 = 2,073,600 bytes ≈ 2.0 MB
- Cr plane: 1920 × 1080 = 2,073,600 bytes ≈ 2.0 MB
- **Total saved: ~11.9 MB per frame**

### 8.2 Alignment Benefits

The 16-byte alignment of plane data and strides in `HeifPixelImage` is beneficial for SVT-HEVC's internal SIMD operations when it copies data into its own processing buffers. While SVT-HEVC handles misaligned input, aligned data may provide a slight performance improvement during the internal copy.

---

## 9. Conclusion

The JPEG decoder output in libheif is **fully compatible** with the SVT-HEVC encoder input requirements. Both use planar YCbCr 4:2:0 8-bit format with stride-based row access. The current libheif architecture already achieves **zero-copy** transfer between the two: the SVT-HEVC encoder plugin reads plane pointers directly from the `HeifPixelImage` created by the JPEG decoder, passing them to SVT-HEVC via `EB_H265_ENC_INPUT` without any intermediate buffer allocation or pixel data copying.

The 16-byte stride alignment in libheif's `HeifPixelImage` is a bonus for SIMD performance and is fully compatible with SVT-HEVC's flexible stride-based input model. The only implicit processing that occurs is the chroma 4:2:0 downsampling performed by the JPEG decoder plugin itself (for non-4:2:0 source JPEGs), which is necessary regardless because SVT-HEVC only supports 4:2:0 input.

---

## Appendix A: Key Source File References

| File | Description |
|------|-------------|
| `libheif/plugins/decoder_jpeg.cc` | JPEG decoder plugin — libjpeg-based decoding, YCbCr 4:2:0 planar output |
| `libheif/codecs/jpeg_dec.cc` | JPEG SOF parser — detects chroma subsampling from JPEG headers |
| `libheif/plugins/encoder_svt_hevc.cc` | SVT-HEVC encoder plugin — feeds HeifPixelImage planes to SVT-HEVC |
| `libheif/pixelimage.h` | HeifPixelImage class — ImageComponent structure, plane metadata |
| `libheif/pixelimage.cc` | HeifPixelImage implementation — memory allocation, stride calculation, alignment |
| `SVT-HEVC/Source/API/EbApi.h` | SVT-HEVC public API — EB_H265_ENC_INPUT, EB_BUFFERHEADERTYPE structures |

## Appendix B: Key Data Structure Definitions

### HeifPixelImage::ImageComponent (libheif)

```cpp
struct ImageComponent {
    uint32_t m_width, m_height;       // Logical dimensions
    uint32_t m_mem_width, m_mem_height; // Allocated dimensions (padded)
    uint8_t m_bit_depth;              // Bits per component
    void* mem;                        // Aligned pixel data pointer
    size_t stride;                    // Bytes per row (16-byte aligned)
    uint8_t* allocated_mem;           // Raw allocation pointer
};
```

### EB_H265_ENC_INPUT (SVT-HEVC)

```c
typedef struct EB_H265_ENC_INPUT {
    uint8_t *luma;        // Y plane pointer
    uint8_t *cb;          // Cb plane pointer
    uint8_t *cr;          // Cr plane pointer
    uint8_t *lumaExt;     // Extended Y (10-bit unpacked)
    uint8_t *cbExt;       // Extended Cb (10-bit unpacked)
    uint8_t *crExt;       // Extended Cr (10-bit unpacked)
    uint32_t yStride;     // Y stride in samples
    uint32_t crStride;    // Cr stride in samples
    uint32_t cbStride;    // Cb stride in samples
    EB_SEI_MESSAGE dolbyVisionRpu;
} EB_H265_ENC_INPUT;
```
