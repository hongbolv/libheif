# libheif Scale 功能研究报告

## 目录

1. [项目概述](#1-项目概述)
2. [核心架构分析](#2-核心架构分析)
3. [Scale 功能现状分析](#3-scale-功能现状分析)
4. [heif-enc 编码器工作流程分析](#4-heif-enc-编码器工作流程分析)
5. [Scale 功能集成到 heif-enc 的方案设计](#5-scale-功能集成到-heif-enc-的方案设计)
6. [实现细节与代码示例](#6-实现细节与代码示例)
7. [未来优化方向](#7-未来优化方向)
8. [总结](#8-总结)

---

## 1. 项目概述

### 1.1 项目简介

libheif 是一个实现了 ISO/IEC 23008-12 (HEIF) 和 ISO/IEC 23008-12:2022 (AVIF) 标准的开源编解码库，版本 1.21.2。它支持以下图像编解码格式：

| 编解码格式 | 读取 | 写入 |
|-----------|------|------|
| HEIC (H.265/HEVC) | ✅ | ✅ |
| AVIF (AV1) | ✅ | ✅ |
| VVC (H.266) | ✅ | ✅ |
| AVC (H.264) | ✅ | ✅ |
| JPEG-in-HEIF | ✅ | ✅ |
| JPEG 2000 | ✅ | ✅ |
| Uncompressed (ISO 23001-17) | ✅ | ✅ |

### 1.2 项目目录结构

```
libheif/
├── libheif/                 # 核心库源码
│   ├── api/                 # 公共 C/C++ API 头文件和实现
│   │   └── libheif/
│   │       ├── heif.h                    # 主 API 头文件
│   │       ├── heif_image.h/.cc          # 图像操作 API（含 scale）
│   │       ├── heif_encoding.h/.cc       # 编码 API
│   │       ├── heif_aux_images.h/.cc     # 辅助图像（缩略图等）
│   │       └── ...
│   ├── codecs/              # 编解码器实现
│   ├── color-conversion/    # 颜色空间转换
│   ├── image-items/         # 图像元素处理
│   ├── pixelimage.h/.cc     # 核心像素图像类（scale 算法实现）
│   ├── context.h/.cc        # HEIF 上下文管理
│   └── security_limits.h    # 安全限制
├── examples/                # 命令行工具
│   ├── heif_enc.cc          # 编码器工具 ★ 需要集成 scale
│   ├── heif_dec.cc          # 解码器工具
│   ├── heif_thumbnailer.cc  # 缩略图工具（已使用 scale API）
│   ├── heif_info.cc         # 文件信息工具
│   └── common.cc/.h         # 公共工具函数
├── heifio/                  # I/O 工具库
│   ├── decoder_jpeg.h/.cc   # JPEG 加载
│   ├── decoder_png.h/.cc    # PNG 加载
│   ├── decoder_tiff.h/.cc   # TIFF 加载
│   ├── decoder_y4m.h/.cc    # Y4M 加载
│   ├── encoder_*.h/.cc      # 各格式编码输出
│   └── ...
├── tests/                   # 测试套件
├── codecs/                  # 编解码器插件
└── cmake/                   # CMake 构建模块
```

---

## 2. 核心架构分析

### 2.1 图像数据模型

libheif 的核心图像类为 `HeifPixelImage`（定义在 `libheif/pixelimage.h`），它是库内部的像素图像表示。公共 API 通过 `heif_image` 结构体暴露：

```cpp
// libheif/api/api_structs.h
struct heif_image {
  std::shared_ptr<HeifPixelImage> image;
};
```

`HeifPixelImage` 支持：
- **多种色彩空间**：`heif_colorspace_RGB`、`heif_colorspace_YCbCr`、`heif_colorspace_monochrome`
- **多种色度格式**：`heif_chroma_420`、`heif_chroma_422`、`heif_chroma_444`、`heif_chroma_interleaved_RGB`/`RGBA` 等
- **多平面数据**：Y、Cb、Cr、R、G、B、Alpha、Interleaved 等通道
- **SDR 和 HDR 支持**：8 位及 16 位位深度

### 2.2 图像变换操作

`HeifPixelImage` 类目前支持以下变换操作（定义在 `pixelimage.h`）：

| 操作 | 方法签名 | 说明 |
|------|---------|------|
| 缩放 | `scale_nearest_neighbor(output, width, height, limits)` | 最近邻插值缩放 |
| 旋转 | `rotate_ccw(angle_degrees, limits)` | 逆时针旋转（90°/180°/270°） |
| 镜像 | `mirror_inplace(direction, limits)` | 水平/垂直镜像 |
| 裁剪 | `crop(left, right, top, bottom, limits)` | 矩形区域裁剪 |
| 叠加 | `overlay(overlay, dx, dy)` | 图像合成叠加 |
| 区域提取 | `extract_image_area(x0, y0, w, h, limits)` | 提取子区域 |
| 扩展 | `extend_to_size_with_zero(width, height, limits)` | 零填充扩展 |

### 2.3 公共 API 层

公共 C API 定义在 `libheif/api/libheif/` 目录中，其中与图像操作相关的函数位于 `heif_image.h`：

```c
// 图像创建
heif_error heif_image_create(int width, int height,
                             heif_colorspace colorspace,
                             heif_chroma chroma,
                             heif_image** out_image);

// 图像缩放 ★
heif_error heif_image_scale_image(const heif_image* input,
                                  heif_image** output,
                                  int width, int height,
                                  const heif_scaling_options* options);

// 图像释放
void heif_image_release(const heif_image*);

// 获取图像尺寸
int heif_image_get_primary_width(const heif_image* img);
int heif_image_get_primary_height(const heif_image* img);
```

---

## 3. Scale 功能现状分析

### 3.1 公共 API

Scale 功能通过 `heif_image_scale_image()` API 暴露给用户（`heif_image.h`，第 264-267 行）：

```c
typedef struct heif_scaling_options heif_scaling_options;

// Currently, heif_scaling_options is not defined yet. Pass a NULL pointer.
LIBHEIF_API
heif_error heif_image_scale_image(const heif_image* input,
                                  heif_image** output,
                                  int width, int height,
                                  const heif_scaling_options* options);
```

**关键发现**：
- `heif_scaling_options` 目前仅有前向声明（`typedef`），**结构体内容未定义**
- API 注释明确说明当前应传 `NULL`
- 这意味着目前无法通过 options 选择不同的缩放算法

### 3.2 API 实现

`heif_image_scale_image()` 的实现（`heif_image.cc`，第 232-248 行）：

```cpp
heif_error heif_image_scale_image(const heif_image* input,
                                  heif_image** output,
                                  int width, int height,
                                  const heif_scaling_options* options)
{
  std::shared_ptr<HeifPixelImage> out_img;

  // 注意：options 参数被完全忽略，直接传 nullptr
  Error err = input->image->scale_nearest_neighbor(out_img, width, height, nullptr);
  if (err) {
    return err.error_struct(input->image.get());
  }

  *output = new heif_image;
  (*output)->image = std::move(out_img);

  return Error::Ok.error_struct(input->image.get());
}
```

**分析**：
- 直接调用内部的 `scale_nearest_neighbor()` 方法
- `options` 参数完全忽略（代码注释标注 "not defined yet"）
- 安全限制参数传 `nullptr`（不做安全检查）

### 3.3 核心缩放算法

`scale_nearest_neighbor()` 实现位于 `libheif/pixelimage.cc`（第 1760-1947 行），是目前唯一的缩放算法：

#### 3.3.1 算法原理

使用**最近邻插值**（Nearest-Neighbor Interpolation），对于输出图像的每个像素 `(x, y)`，从输入图像中采样最近的像素：

```
输出像素 (x, y) ← 输入像素 (ix, iy)
其中：
  ix = x * input_width / output_width
  iy = y * input_height / output_height
```

#### 3.3.2 支持的图像格式

算法分为四个处理路径：

| 路径 | 数据布局 | 位深度 | 数据类型 |
|------|---------|--------|---------|
| SDR Interleaved | 交错（RGBA/RGB） | ≤8 位 | `uint8_t` |
| HDR Interleaved | 交错（RGBA/RGB） | >8 位 | `uint16_t` |
| SDR Planar | 平面式（Y/Cb/Cr/R/G/B/Alpha） | ≤8 位 | `uint8_t` |
| HDR Planar | 平面式（Y/Cb/Cr/R/G/B/Alpha） | >8 位 | `uint16_t` |

#### 3.3.3 色度处理

对于 YCbCr 色彩空间，算法正确处理了色度子采样：

```cpp
uint32_t cw, ch;
get_subsampled_size(width, height, heif_channel_Cb, get_chroma_format(), &cw, &ch);
// Y 通道使用全尺寸 (width × height)
// Cb/Cr 通道使用子采样尺寸 (cw × ch)
```

#### 3.3.4 完整处理流程

```
1. 创建输出图像（相同色彩空间和色度格式）
2. 为每个通道分配对应尺寸的平面：
   - Interleaved: 单平面（width × height）
   - RGB 平面: R/G/B 各 (width × height)
   - YCbCr 平面: Y (width × height), Cb/Cr (cw × ch)
   - Monochrome: Y (width × height)
   - Alpha: (width × height)
3. 对每个平面执行最近邻采样
4. 返回缩放后的图像
```

### 3.4 现有 Scale API 使用场景

#### 3.4.1 heif_thumbnailer（缩略图工具）

`examples/heif_thumbnailer.cc` 是目前唯一在 example 中使用 scale API 的工具：

```cpp
// 计算保持宽高比的缩略图尺寸
if (input_width > input_height) {
    thumbnail_height = input_height * size / input_width;
    thumbnail_width = size;
} else if (input_height > 0) {
    thumbnail_width = input_width * size / input_height;
    thumbnail_height = size;
}

// 执行缩放
struct heif_image* scaled_image = NULL;
err = heif_image_scale_image(image, &scaled_image,
                             thumbnail_width, thumbnail_height, NULL);
if (err.code) {
    std::cerr << "Could not scale image : " << err.message << "\n";
    return 1;
}
heif_image_release(image);
image = scaled_image;
```

#### 3.4.2 内部缩略图编码

`libheif/context.cc` 中 `encode_thumbnail()` 内部也使用了缩放：

```cpp
// context.cc - 内部缩略图编码时使用 scale_nearest_neighbor
err = master_image->scale_nearest_neighbor(thumbnail_image,
                                           thumb_width, thumb_height,
                                           get_security_limits());
```

#### 3.4.3 Alpha 通道缩放

`libheif/image_item.cc` 中处理 Alpha 通道尺寸不匹配时也使用缩放。

---

## 4. heif-enc 编码器工作流程分析

### 4.1 整体架构

`heif-enc` 是 libheif 的命令行编码工具（`examples/heif_enc.cc`），其核心流程为：

```
命令行参数解析 → 编码器初始化 → 图像加载 → 编码 → 文件输出
```

### 4.2 详细流程分析

```
main()
 ├── 解析命令行参数（getopt_long）
 ├── 创建 heif_context
 ├── 获取编码器（heif_context_get_encoder_for_format）
 ├── 设置编码参数（质量、损失、参数等）
 ├── 对每个输入文件调用 do_encode_images()
 │    ├── load_image(input_filename, output_bit_depth)
 │    │    ├── 检测文件格式（PNG/JPEG/TIFF/Y4M）
 │    │    ├── 调用对应加载函数（loadPNG/loadJPEG/loadTIFF/loadY4M）
 │    │    └── 返回 InputImage（含 heif_image + orientation）
 │    │
 │    ├── ★ 此处是插入 scale 的最佳位置 ★
 │    │
 │    ├── 处理 tiling（如果启用）
 │    ├── 创建 nclx 颜色配置
 │    ├── 编码图像
 │    │    ├── heif_context_encode_image()  // 普通编码
 │    │    └── encode_tiled()               // 分块编码
 │    ├── 生成缩略图（如果启用）
 │    │    └── heif_context_encode_thumbnail()
 │    ├── 添加 EXIF/XMP 元数据
 │    └── 处理金字塔组（如果启用）
 ├── heif_context_write_to_file()  // 写入输出文件
 └── 清理资源
```

### 4.3 关键代码位置

```cpp
// examples/heif_enc.cc 中 do_encode_images() 函数内

// 第 2022 行：加载图像
input_image = load_image(input_filename, output_bit_depth);

// 第 2025 行：获取 heif_image 指针
std::shared_ptr<heif_image> image = input_image.image;

// ★★★ 最佳插入点：此处插入 scale 逻辑 ★★★

// 第 2027-2061 行：处理 tiling
if (use_tiling) { ... }

// 第 2095-2099 行：编码
error = heif_context_encode_image(context, image.get(), encoder, options, &handle);
```

### 4.4 现有命令行选项

heif-enc 当前不支持任何图像变换选项（无 crop、rotate、scale），仅有：
- `-t, --thumb #`：生成缩略图（由编码 API 内部处理缩放）
- 各种编码参数（质量、位深度、色彩配置等）

---

## 5. Scale 功能集成到 heif-enc 的方案设计

### 5.1 方案概述

在 heif-enc 中添加 `--scale` 命令行选项，使用户能够在编码前对输入图像执行缩放操作。该功能插入在「图像加载」和「图像编码」之间。

### 5.2 命令行接口设计

建议支持以下几种 scale 指定方式：

```
heif-enc [options] input.jpg -o output.heic

Scale 选项：
  --scale WxH              缩放到指定的宽×高（如 --scale 1920x1080）
  --scale-width W          缩放宽度为 W，高度按比例自动计算
  --scale-height H         缩放高度为 H，宽度按比例自动计算
  --scale-factor F         按比例因子缩放（如 --scale-factor 0.5 缩小一半）
```

### 5.3 数据流设计

```
输入文件 (JPEG/PNG/TIFF/Y4M)
    │
    ▼
load_image() ─── 加载为 heif_image
    │
    ▼
[新增] scale 处理（如果指定了 --scale 选项）
    │  ├── 计算目标尺寸（处理宽高比）
    │  ├── 调用 heif_image_scale_image()
    │  └── 替换 image 指针
    │
    ▼
heif_context_encode_image() ─── 编码
    │
    ▼
heif_context_write_to_file() ─── 输出
```

### 5.4 详细设计

#### 5.4.1 新增命令行参数

在 `heif_enc.cc` 的 `option` 数组中添加：

```cpp
// 在现有 option 定义中添加
{"scale",         required_argument, nullptr, OPTION_SCALE},
{"scale-width",   required_argument, nullptr, OPTION_SCALE_WIDTH},
{"scale-height",  required_argument, nullptr, OPTION_SCALE_HEIGHT},
{"scale-factor",  required_argument, nullptr, OPTION_SCALE_FACTOR},
```

#### 5.4.2 参数存储变量

建议使用结构体封装相关参数，避免命名空间污染：

```cpp
// 新增结构体
struct ScaleOptions {
    int target_width = 0;      // --scale WxH 或 --scale-width W
    int target_height = 0;     // --scale WxH 或 --scale-height H
    float factor = 0.0f;       // --scale-factor F
};

ScaleOptions scale_options;
```

#### 5.4.3 核心缩放逻辑

插入位置：`do_encode_images()` 函数中，`load_image()` 之后、tiling 处理之前（约第 2025 行后）：

```cpp
std::shared_ptr<heif_image> image = input_image.image;

// --- 新增：Scale 处理 ---
if (scale_options.target_width > 0 || scale_options.target_height > 0 || scale_options.factor > 0.0f) {
    int orig_width = heif_image_get_primary_width(image.get());
    int orig_height = heif_image_get_primary_height(image.get());
    int new_width = orig_width;
    int new_height = orig_height;

    if (scale_options.factor > 0.0f) {
        // 按比例因子缩放
        new_width = static_cast<int>(orig_width * scale_options.factor + 0.5f);
        new_height = static_cast<int>(orig_height * scale_options.factor + 0.5f);
    }
    else if (scale_options.target_width > 0 && scale_options.target_height > 0) {
        // 指定精确的宽×高
        new_width = scale_options.target_width;
        new_height = scale_options.target_height;
    }
    else if (scale_options.target_width > 0) {
        // 仅指定宽度，高度按比例（使用浮点运算避免精度损失）
        new_width = scale_options.target_width;
        new_height = static_cast<int>(orig_height * scale_options.target_width
                                      / static_cast<float>(orig_width) + 0.5f);
    }
    else if (scale_options.target_height > 0) {
        // 仅指定高度，宽度按比例（使用浮点运算避免精度损失）
        new_height = scale_options.target_height;
        new_width = static_cast<int>(orig_width * scale_options.target_height
                                     / static_cast<float>(orig_height) + 0.5f);
    }

    if (new_width <= 0 || new_height <= 0) {
        std::cerr << "Invalid scale dimensions: " << new_width << "x" << new_height << "\n";
        return 1;
    }

    if (new_width != orig_width || new_height != orig_height) {
        heif_image* scaled_image = nullptr;
        heif_error err = heif_image_scale_image(image.get(), &scaled_image,
                                                 new_width, new_height, nullptr);
        if (err.code != heif_error_Ok) {
            std::cerr << "Could not scale image: " << err.message << "\n";
            return 1;
        }

        image = std::shared_ptr<heif_image>(scaled_image, [](heif_image* img) {
            heif_image_release(img);
        });

        // 更新 input_image 以确保后续处理使用缩放后的图像
        input_image.image = image;
    }
}
// --- End Scale 处理 ---
```

### 5.5 使用示例

```bash
# 缩放到指定尺寸
heif-enc --scale 1920x1080 input.jpg -o output.heic

# 仅指定宽度，保持宽高比
heif-enc --scale-width 800 input.png -o output.avif

# 仅指定高度，保持宽高比
heif-enc --scale-height 600 input.jpg -o output.heic

# 按比例缩放（缩小到 50%）
heif-enc --scale-factor 0.5 input.jpg -o output.heic

# 与其他选项组合使用
heif-enc -q 85 --scale 1280x720 -t 128 input.jpg -o output.heic
```

---

## 6. 实现细节与代码示例

### 6.1 需要修改的文件

| 文件 | 修改内容 |
|------|---------|
| `examples/heif_enc.cc` | 添加 scale 命令行选项和缩放逻辑 |
| `examples/heif-enc.1` | 更新 man page 文档 |

### 6.2 完整代码修改（heif_enc.cc）

#### 6.2.1 添加选项常量定义

在已有的 option 枚举中添加（约第 80 行附近）：

```cpp
// 在 OPTION_ 枚举值列表中添加
OPTION_SCALE = 2000,
OPTION_SCALE_WIDTH,
OPTION_SCALE_HEIGHT,
OPTION_SCALE_FACTOR,
```

#### 6.2.2 注册 getopt_long 选项

在 `long_options[]` 数组中添加（约第 200 行附近）：

```cpp
{"scale",        required_argument, 0, OPTION_SCALE},
{"scale-width",  required_argument, 0, OPTION_SCALE_WIDTH},
{"scale-height", required_argument, 0, OPTION_SCALE_HEIGHT},
{"scale-factor", required_argument, 0, OPTION_SCALE_FACTOR},
```

#### 6.2.3 帮助文本

在 `show_help()` 函数中添加 scaling 部分：

```cpp
<< "\n"
<< "scaling:\n"
<< "      --scale WxH              scale the input image to the given width and height\n"
<< "      --scale-width W          scale the width to W, maintaining the aspect ratio\n"
<< "      --scale-height H         scale the height to H, maintaining the aspect ratio\n"
<< "      --scale-factor F         scale the image by the given factor (e.g. 0.5)\n"
```

#### 6.2.4 参数解析

在 getopt switch-case 中添加：

```cpp
case OPTION_SCALE: {
    std::string scale_str(optarg);
    auto pos = scale_str.find('x');
    if (pos == std::string::npos) {
        pos = scale_str.find('X');
    }
    if (pos == std::string::npos) {
        std::cerr << "Invalid scale format. Use WxH (e.g., 1920x1080)\n";
        return 5;
    }
    try {
        scale_options.target_width = std::stoi(scale_str.substr(0, pos));
        scale_options.target_height = std::stoi(scale_str.substr(pos + 1));
    } catch (const std::exception& e) {
        std::cerr << "Invalid scale dimensions: " << e.what() << "\n";
        return 5;
    }
    break;
}
case OPTION_SCALE_WIDTH:
    try {
        scale_options.target_width = std::stoi(optarg);
    } catch (const std::exception& e) {
        std::cerr << "Invalid scale width: " << e.what() << "\n";
        return 5;
    }
    break;
case OPTION_SCALE_HEIGHT:
    try {
        scale_options.target_height = std::stoi(optarg);
    } catch (const std::exception& e) {
        std::cerr << "Invalid scale height: " << e.what() << "\n";
        return 5;
    }
    break;
case OPTION_SCALE_FACTOR:
    try {
        scale_options.factor = std::stof(optarg);
    } catch (const std::exception& e) {
        std::cerr << "Invalid scale factor: " << e.what() << "\n";
        return 5;
    }
    if (scale_options.factor <= 0.0f) {
        std::cerr << "Scale factor must be positive\n";
        return 5;
    }
    break;
```

#### 6.2.5 缩放处理逻辑（插入到 `do_encode_images()` 中）

参见 5.4.3 节中的核心缩放逻辑代码。

### 6.3 与现有功能的交互

#### 6.3.1 与 Thumbnail 的交互

缩略图（`-t` 选项）在 scale 之后创建，因此缩略图会基于缩放后的图像生成。这是预期行为——用户先缩放到目标尺寸，再生成缩略图。

#### 6.3.2 与 Tiling 的交互

如果同时使用 `--scale` 和 tiling 选项（`--cut-tiles`、`-T`），需要注意：
- `--scale` 先于 tiling 执行
- Tiling 会基于缩放后的图像尺寸进行分块
- 建议在文档中说明两者的执行顺序

#### 6.3.3 与 Sequence/Video 的交互

序列模式（`-S`）下，每帧图像都会被独立缩放。这确保了输出序列的一致尺寸。

---

## 7. 未来优化方向

### 7.1 扩展 `heif_scaling_options`

当前 `heif_scaling_options` 结构体未定义。建议未来定义如下：

```c
struct heif_scaling_options {
    int version;                        // 版本号，用于向后兼容
    enum heif_scaling_algorithm {
        heif_scaling_nearest_neighbor,  // 最近邻（当前唯一实现）
        heif_scaling_bilinear,          // 双线性插值
        heif_scaling_bicubic,           // 双三次插值
        heif_scaling_lanczos,           // Lanczos 重采样
    } algorithm;
};
```

### 7.2 新增缩放算法

当前仅有最近邻插值，该算法速度快但质量较低（缩小时有锯齿、放大时有马赛克效应）。建议逐步添加：

| 算法 | 质量 | 速度 | 适用场景 |
|------|------|------|---------|
| 最近邻（已有） | ★☆☆☆ | ★★★★ | 像素艺术、快速预览 |
| 双线性插值 | ★★☆☆ | ★★★☆ | 一般用途 |
| 双三次插值 | ★★★☆ | ★★☆☆ | 照片缩放 |
| Lanczos | ★★★★ | ★☆☆☆ | 高质量缩放 |

### 7.3 对 heif-enc 的 `--scale` 选项扩展

```
--scale-algorithm ALGO   选择缩放算法（nearest, bilinear, bicubic, lanczos）
--scale-fit MODE         适配模式（fill, fit, stretch）
```

### 7.4 性能优化

- **SIMD 加速**：使用 SSE/AVX/NEON 指令集加速缩放计算
- **多线程**：对大图像使用多线程并行处理各行
- **整数定点运算**：将浮点坐标映射改为定点运算以提高精度和速度

---

## 8. 总结

### 8.1 当前状态

| 方面 | 状态 | 说明 |
|------|------|------|
| Scale API | ✅ 已有 | `heif_image_scale_image()` 公共 API |
| 缩放算法 | ⚠️ 仅最近邻 | 质量有限，适合缩略图 |
| `heif_scaling_options` | ❌ 未定义 | 预留扩展接口但尚未实现 |
| heif_thumbnailer 集成 | ✅ 已完成 | 缩略图工具已使用 scale API |
| heif-enc 集成 | ❌ 未实现 | 编码器无缩放选项 |
| 安全限制 | ⚠️ 部分 | API 层传 nullptr，内部使用有限 |

### 8.2 集成可行性

将 scale 功能集成到 heif-enc **完全可行**，理由如下：

1. **API 已就绪**：`heif_image_scale_image()` 是稳定的公共 API
2. **已有参考实现**：`heif_thumbnailer.cc` 提供了完整的使用示例
3. **插入点明确**：`load_image()` 之后、`heif_context_encode_image()` 之前
4. **改动范围小**：仅需修改 `heif_enc.cc` 一个文件（加上 man page）
5. **不影响现有功能**：scale 是可选操作，不改变默认行为

### 8.3 实现建议

1. **第一阶段**：在 heif-enc 中添加 `--scale`、`--scale-width`、`--scale-height`、`--scale-factor` 选项，使用现有的 `heif_image_scale_image()` API（最近邻算法）
2. **第二阶段**：定义并实现 `heif_scaling_options` 结构体，添加算法选择功能
3. **第三阶段**：实现双线性/双三次/Lanczos 等高质量缩放算法
4. **第四阶段**：添加 SIMD 优化和多线程支持
