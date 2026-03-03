# SVT-HEVC Encoder Plugin 实现计划

> **目标**: 为 libheif 实现 SVT-HEVC 编码器插件，使其可以作为 HEIC 格式的编码器使用  
> **参考**: 现有的 x265 编码器插件 (`encoder_x265.cc`) 和 SVT-AV1 编码器插件 (`encoder_svt.cc`)  
> **状态**: ✅ 已完成

---

## 目录

1. [概述](#1-概述)
2. [SVT-HEVC API 分析](#2-svt-hevc-api-分析)
3. [需要修改的文件清单](#3-需要修改的文件清单)
4. [详细实现步骤](#4-详细实现步骤)
5. [代码实现细节](#5-代码实现细节)
6. [heif-enc 命令行工具集成](#6-heif-enc-命令行工具集成)
7. [注意事项](#7-注意事项)
8. [测试方案](#8-测试方案)

---

## 1. 概述

### 1.1 背景

libheif 当前支持两种 HEVC (H.265) 编码器：
- **x265** — 默认 HEVC 编码器，优先级 100
- **kvazaar** — 备选 HEVC 编码器，优先级 100

SVT-HEVC (Scalable Video Technology for HEVC) 是 Intel 开源的 HEVC 编码器，针对 Intel 处理器优化，以高并行性和高速度著称。将 SVT-HEVC 集成为 libheif 的编码器插件，可以为用户提供另一种高性能的 HEIC 编码选择。

### 1.2 与现有插件的关系

| 特性 | x265 | kvazaar | SVT-HEVC (新增) |
|------|-------|---------|----------------|
| 编码格式 | HEVC | HEVC | HEVC |
| 位深度 | 8/10/12 | 编译时固定 | 8/10 |
| 色度采样 | 420/422/444 | 420/422/444 | 420/422/444 |
| 无损编码 | ✅ | ✅ | ❌ |
| 序列编码 | ✅ | ✅ | ✅ |
| API 风格 | C 函数指针 | C 函数指针 | 异步发送/接收 |
| 分辨率范围 | 无限制 | 无限制 | 64×64 ~ 8192×4320 |

### 1.3 建议优先级

由于 SVT-HEVC 不支持无损编码，且该项目已于 2021 年停止维护（Intel 已停止对该项目的开发和贡献），建议将其优先级设置为 **90**（低于 x265 的 100），作为一个可选择的备选编码器。SVT-HEVC 的优势在于针对 Intel 处理器的高并行优化，适合追求编码速度的场景。

> **注意**: SVT-HEVC 支持从 64×64 到 8192×4320 的全范围分辨率，并非仅限于高分辨率视频。编码预设范围因分辨率而异：
> - 所有分辨率：预设 0-9
> - ≥1080p：预设 0-10
> - ≥4K：预设 0-11

---

## 2. SVT-HEVC API 分析

### 2.1 核心 API 流程

SVT-HEVC 的编码流程与 x265 有显著不同，采用异步发送/接收模式：

```
步骤 1: EbInitHandle()       — 创建编码器句柄，获取默认配置
步骤 2: EbH265EncSetParameter() — 设置编码参数
步骤 3: EbInitEncoder()      — 初始化编码器
可选:   EbH265EncStreamHeader() — 获取 VPS/SPS/PPS 头
步骤 4: EbH265EncSendPicture() — 发送图像帧（可重复调用）
步骤 5: EbH265GetPacket()     — 获取编码后的数据包（可重复调用）
步骤 5-1: EbH265ReleaseOutBuffer() — 释放输出缓冲区
步骤 6: EbDeinitEncoder()    — 反初始化编码器
步骤 7: EbDeinitHandle()     — 销毁编码器句柄
```

#### 单帧编码流程（HEIC 静态图像）

```
EbInitHandle() → 获取默认配置
   ↓
配置参数: framesToBeEncoded=1, intraPeriodLength=-1, hierarchicalLevels=0
   ↓
EbH265EncSetParameter() → 应用配置
   ↓
EbInitEncoder() → 初始化编码器
   ↓
EbH265EncStreamHeader() → 获取 VPS/SPS/PPS
   ↓
EbH265EncSendPicture(picture) → 发送唯一帧
   ↓
EbH265EncSendPicture(EOS) → 发送 EOS 空缓冲区
   ↓
循环 EbH265GetPacket(picSendDone=1) → 阻塞获取所有编码数据
   ↓ (直到 EB_NoErrorEmptyQueue)
EbDeinitEncoder() → EbDeinitHandle()
```

#### 多帧编码流程（HEIC 序列/图像组）

```
EbInitHandle() → 获取默认配置
   ↓
配置参数: framesToBeEncoded=0, intraPeriodLength=-2(auto), hierarchicalLevels=3
   ↓
EbH265EncSetParameter() → 应用配置
   ↓
EbInitEncoder() → 初始化编码器
   ↓
EbH265EncStreamHeader() → 获取 VPS/SPS/PPS (解析为 NAL 单元存入输出队列)
   ↓
┌─ 对每一帧 (frame 0, 1, 2, ...):
│    ↓
│    EbH265EncSendPicture(picture, nFlags=0) → 发送帧
│    ↓
│    循环 EbH265GetPacket(picSendDone=0) → 非阻塞获取已有数据
│    │  ↓ (解析 Annex B NAL, 存入输出队列)
│    │  EbH265ReleaseOutBuffer() → 释放输出缓冲区
│    └─ 直到 EB_NoErrorEmptyQueue
│
└─ 所有帧发送完毕后:
     ↓
     EbH265EncSendPicture(EOS) → 发送 EOS 空缓冲区 (pBuffer=NULL, nFlags=EB_BUFFERFLAG_EOS)
     ↓
     循环 EbH265GetPacket(picSendDone=1) → 阻塞获取剩余编码数据
     │  ↓ (解析 Annex B NAL, 存入输出队列)
     │  EbH265ReleaseOutBuffer()
     └─ 直到 EB_NoErrorEmptyQueue
     ↓
     EbDeinitEncoder() → EbDeinitHandle()
```

**关键说明**:
- `picSendDone=0`: 非阻塞模式，立即返回可用数据或 `EB_NoErrorEmptyQueue`
- `picSendDone=1`: 阻塞模式，等待直到数据可用或队列为空（用于最终刷新）
- EOS 通过一个 **单独的空缓冲区** 发送，与最后一帧分开（参考 SVT-HEVC 官方应用的实现）
- 由于 SVT-HEVC 的流水线设计，发送帧和接收编码结果之间存在延迟，因此异步模型是必要的

### 2.2 关键数据结构

```c
// 编码配置
typedef struct EB_H265_ENC_CONFIGURATION {
    uint8_t   encMode;          // 预设 0(最高质量) - 11(最高速度)
    uint32_t  sourceWidth;      // 输入宽度
    uint32_t  sourceHeight;     // 输入高度
    uint32_t  encoderBitDepth;  // 位深度 8 或 10
    EB_COLOR_FORMAT encoderColorFormat; // 色度格式
    uint32_t  qp;               // 量化参数 0-51
    uint32_t  rateControlMode;  // 0=CQP, 1=VBR
    uint32_t  targetBitRate;    // 目标码率
    int32_t   intraPeriodLength; // I帧间隔
    uint32_t  hierarchicalLevels; // 层次级别
    uint32_t  profile;          // 1=Main, 2=Main10
    uint32_t  tier;             // 0=Main, 1=High
    uint32_t  level;            // 0=自动
    uint32_t  threadCount;      // 线程数
    // ... 更多参数
} EB_H265_ENC_CONFIGURATION;

// 输入图像
typedef struct EB_H265_ENC_INPUT {
    uint8_t *luma;     // Y 平面
    uint8_t *cb;       // Cb 平面
    uint8_t *cr;       // Cr 平面
    uint8_t *lumaExt;  // 10位扩展
    uint8_t *cbExt;    // 10位扩展
    uint8_t *crExt;    // 10位扩展
    uint32_t yStride;  // Y 平面步长
    uint32_t cbStride; // Cb 平面步长
    uint32_t crStride; // Cr 平面步长
} EB_H265_ENC_INPUT;

// 缓冲区头
typedef struct EB_BUFFERHEADERTYPE {
    uint32_t nSize;
    uint8_t* pBuffer;
    uint32_t nFilledLen;
    uint32_t nAllocLen;
    void*    pAppPrivate;
    int64_t  dts, pts;
    uint32_t sliceType;
    uint32_t nFlags;
    // ...
} EB_BUFFERHEADERTYPE;

// 色彩格式枚举
typedef enum EB_COLOR_FORMAT {
    EB_YUV400,  // 单色
    EB_YUV420,  // 4:2:0
    EB_YUV422,  // 4:2:2
    EB_YUV444   // 4:4:4
} EB_COLOR_FORMAT;
```

### 2.3 关键注意事项

1. **异步 API**: SVT-HEVC 使用发送/接收模式，`EbH265EncSendPicture()` 发送帧，`EbH265GetPacket()` 异步获取编码结果。`picSendDone` 参数控制阻塞行为：0=非阻塞，1=阻塞等待
2. **内存管理**: 输出缓冲区需要通过 `EbH265ReleaseOutBuffer()` 释放
3. **10位输入**: 支持 packed (16-bit per sample) 和 unpacked (8-bit + 2-bit 分离平面) 两种 10 位输入格式。unpacked 格式使用 `lumaExt`/`cbExt`/`crExt` 字段存储额外 2 位
4. **色度格式**: 支持 4:2:0 (`EB_YUV420`, 默认)、4:2:2 (`EB_YUV422`)、4:4:4 (`EB_YUV444`) 三种格式。注意 HEVC Main/Main10 profile 只支持 4:2:0，使用 4:2:2/4:4:4 可能需要编码器内部自动调整 profile
5. **无损编码**: SVT-HEVC **不支持** 无损编码。其 API 中没有无损模式的参数，rate control 仅支持 CQP (QP=0~51) 和 VBR 两种模式，即使 QP=0 也仍然是有损编码
6. **图像尺寸**: 支持 64×64 到 8192×4320 的分辨率范围，宽高不是 8 的倍数时会自动填充 (padding)
7. **EOS 处理**: 编码结束时需要发送一个单独的 EOS 缓冲区（`pBuffer=NULL`, `nFlags=EB_BUFFERFLAG_EOS`），编码器收到 EOS 后将刷新所有剩余的编码数据

---

## 3. 需要修改的文件清单

### 3.1 新增文件

| 文件 | 说明 |
|------|------|
| `libheif/plugins/encoder_svt_hevc.h` | SVT-HEVC 编码器插件头文件 |
| `libheif/plugins/encoder_svt_hevc.cc` | SVT-HEVC 编码器插件实现 |
| `cmake/modules/FindSvtHevcEnc.cmake` | CMake 查找模块 |

### 3.2 需要修改的文件

| 文件 | 修改内容 |
|------|---------|
| `CMakeLists.txt` (根目录) | 添加 SVT-HEVC 编码器选项和查找逻辑 |
| `libheif/plugins/CMakeLists.txt` | 添加 SVT-HEVC 插件编译配置 |
| `libheif/plugin_registry.cc` | 注册 SVT-HEVC 编码器插件 |

---

## 4. 详细实现步骤

### ✅ 步骤 1: 创建 CMake 查找模块

**文件**: `cmake/modules/FindSvtHevcEnc.cmake`

```cmake
include(LibFindMacros)
libfind_pkg_check_modules(SvtHevcEnc_PKGCONF SvtHevcEnc)

find_path(SvtHevcEnc_INCLUDE_DIR
    NAMES EbApi.h
    HINTS ${SvtHevcEnc_PKGCONF_INCLUDE_DIRS} ${SvtHevcEnc_PKGCONF_INCLUDEDIR}
    PATH_SUFFIXES svt-hevc
)

find_library(SvtHevcEnc_LIBRARY
    NAMES SvtHevcEnc libSvtHevcEnc
    HINTS ${SvtHevcEnc_PKGCONF_LIBRARY_DIRS} ${SvtHevcEnc_PKGCONF_LIBDIR}
)

set(SvtHevcEnc_PROCESS_LIBS SvtHevcEnc_LIBRARY)
set(SvtHevcEnc_PROCESS_INCLUDES SvtHevcEnc_INCLUDE_DIR)
libfind_process(SvtHevcEnc)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SvtHevcEnc
    REQUIRED_VARS
        SvtHevcEnc_INCLUDE_DIR
        SvtHevcEnc_LIBRARIES
)
```

### ✅ 步骤 2: 修改根 CMakeLists.txt

**文件**: `CMakeLists.txt`

在 kvazaar 配置之后（约第 155 行），添加 SVT-HEVC 选项：

```cmake
# svt-hevc
plugin_option(SvtHevcEnc "SVT-HEVC encoder" OFF ON)
if (WITH_SvtHevcEnc)
    find_package(SvtHevcEnc)
endif()
```

在编解码器编译汇总部分（约第 316 行之后），添加显示信息：

```cmake
plugin_compilation_info(SvtHevcEnc SvtHevcEnc "SVT-HEVC encoder")
```

在 HEIC 编码支持判断处（约第 361-362 行），将 `SvtHevcEnc_FOUND` 加入条件：

```cmake
if (X265_FOUND OR KVAZAAR_FOUND OR SvtHevcEnc_FOUND)
    set(SUPPORTS_HEIC_ENCODING TRUE)
endif()
```

### ✅ 步骤 3: 修改插件 CMakeLists.txt

**文件**: `libheif/plugins/CMakeLists.txt`

在 kvazaar 编译配置之后（约第 94 行），添加：

```cmake
set(SvtHevcEnc_sources encoder_svt_hevc.cc encoder_svt_hevc.h)
set(SvtHevcEnc_extra_plugin_sources)
plugin_compilation(svthevc SvtHevcEnc SvtHevcEnc_FOUND SvtHevcEnc SvtHevcEnc)
```

### ✅ 步骤 4: 修改插件注册表

**文件**: `libheif/plugin_registry.cc`

添加头文件包含（约第 42 行之后）：

```cpp
#if HAVE_SvtHevcEnc
#include "plugins/encoder_svt_hevc.h"
#endif
```

在 `register_default_plugins()` 函数中添加注册代码（约第 164 行之后）：

```cpp
#if HAVE_SvtHevcEnc
  register_encoder(get_encoder_plugin_svt_hevc());
#endif
```

### ✅ 步骤 5: 创建编码器头文件

**文件**: `libheif/plugins/encoder_svt_hevc.h`

```cpp
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

#ifndef LIBHEIF_ENCODER_SVT_HEVC_H
#define LIBHEIF_ENCODER_SVT_HEVC_H

#include "common_utils.h"

const struct heif_encoder_plugin* get_encoder_plugin_svt_hevc();

#if PLUGIN_SvtHevcEnc
extern "C" {
MAYBE_UNUSED LIBHEIF_API extern heif_plugin_info plugin_info;
}
#endif

#endif
```

### ✅ 步骤 6: 创建编码器实现文件

**文件**: `libheif/plugins/encoder_svt_hevc.cc`

（完整代码见[第 5 节](#5-代码实现细节)）

---

## 5. 代码实现细节

### 5.1 完整的 encoder_svt_hevc.cc 实现

```cpp
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
#include <cassert>
#include <deque>
#include <vector>

#include <EbApi.h>


// ============================================================
//                    错误消息常量
// ============================================================

static const char* kError_unsupported_bit_depth =
    "Bit depth not supported by SVT-HEVC (only 8 and 10 bit supported)";
static const char* kError_encoder_init_failed =
    "SVT-HEVC encoder initialization failed";
static const char* kError_encode_failed =
    "SVT-HEVC encoding failed";


// ============================================================
//                    编码器状态结构体
// ============================================================

struct encoder_struct_svt_hevc
{
  // SVT-HEVC 编码器组件
  EB_COMPONENTTYPE* svt_encoder = nullptr;
  EB_H265_ENC_CONFIGURATION enc_params = {};
  bool encoder_initialized = false;

  // --- 输出队列 ---

  struct Packet
  {
    std::vector<uint8_t> data;
    uintptr_t frameNr = 0;
  };

  std::deque<Packet> output_packets;
  std::vector<uint8_t> active_output_nal;

  // --- 编码器参数 ---

  int quality = 50;      // 0-100, 映射到 QP
  int enc_preset = 7;    // 0(最高质量) - 11(最快速度)
  int qp = 32;           // 直接 QP 值
  int threads = 0;       // 0 = 自动
  int log_level = 0;     // 0 = 无日志
  int hierarchical_levels = 0; // 0 = 适合单帧, 3 = 默认序列
  int intra_period = -2; // -2 = 自动

  std::string last_error_message;
};


// ============================================================
//                    参数定义
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
//                    插件名称
// ============================================================

static const char* svt_hevc_plugin_name()
{
  strcpy(plugin_name, "SVT-HEVC encoder");
  return plugin_name;
}


// ============================================================
//                    参数初始化
// ============================================================

static void svt_hevc_init_parameters()
{
  heif_encoder_parameter* p = svt_hevc_encoder_params;
  const heif_encoder_parameter** d = svt_hevc_encoder_parameter_ptrs;
  int i = 0;

  // quality 参数 (0-100)
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

  // lossless 参数 (SVT-HEVC 不支持，始终为 false)
  assert(i < MAX_NPARAMETERS);
  p->version = 2;
  p->name = heif_encoder_parameter_name_lossless;
  p->type = heif_encoder_parameter_type_boolean;
  p->boolean.default_value = false;
  p->has_default = true;
  d[i++] = p++;

  // preset 参数 (0-11)
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

  // qp 参数 (0-51)
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

  // threads 参数
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
//                    插件生命周期
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
//                    参数 setter/getter
// ============================================================

static heif_error svt_hevc_set_parameter_quality(void* encoder_raw, int quality)
{
  auto* encoder = (encoder_struct_svt_hevc*) encoder_raw;

  if (quality < 0 || quality > 100) {
    return heif_error_invalid_parameter_value;
  }

  encoder->quality = quality;

  // 映射 quality 到 QP: quality=0 -> qp=MAX_QP, quality=MAX_QUALITY -> qp=0
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
  // SVT-HEVC 不支持无损编码
  if (enable) {
    return heif_error_unsupported_parameter;
  }
  return heif_error_ok;
}


static heif_error svt_hevc_get_parameter_lossless(void* encoder_raw, int* enable)
{
  *enable = 0; // 始终为有损
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
//                    颜色空间查询
// ============================================================

static void svt_hevc_query_input_colorspace(heif_colorspace* colorspace, heif_chroma* chroma)
{
  // SVT-HEVC 支持 YCbCr 4:2:0/4:2:2/4:4:4，默认使用 4:2:0
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
  // SVT-HEVC 支持 YCbCr 4:2:0/4:2:2/4:4:4，默认使用 4:2:0
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
//              NAL 单元解析辅助函数
// ============================================================

// 从 SVT-HEVC 输出的比特流中提取 NAL 单元
// SVT-HEVC 输出带有 0x00000001 起始码的 Annex B 格式
static void parse_nal_units_from_bitstream(
    const uint8_t* bitstream, uint32_t bitstream_size,
    std::deque<encoder_struct_svt_hevc::Packet>& output_packets,
    uintptr_t frameNr)
{
  uint32_t pos = 0;

  while (pos < bitstream_size) {
    // 查找起始码 (0x000001 或 0x00000001)
    uint32_t nal_start = pos;
    bool found = false;

    while (pos + 2 < bitstream_size) {
      if (bitstream[pos] == 0 && bitstream[pos + 1] == 0) {
        if (bitstream[pos + 2] == 1) {
          // 3 字节起始码
          found = true;
          break;
        }
        if (pos + 3 < bitstream_size && bitstream[pos + 2] == 0 && bitstream[pos + 3] == 1) {
          // 4 字节起始码
          found = true;
          break;
        }
      }
      pos++;
    }

    if (!found && nal_start == 0) {
      // 没有找到起始码，整个buffer作为一个NAL
      encoder_struct_svt_hevc::Packet pkt;
      pkt.data.assign(bitstream, bitstream + bitstream_size);
      pkt.frameNr = frameNr;
      output_packets.push_back(std::move(pkt));
      return;
    }

    if (!found) {
      break;
    }

    // 跳过起始码
    uint32_t nal_data_start = pos;
    if (bitstream[pos + 2] == 1) {
      nal_data_start = pos + 3;
    }
    else {
      nal_data_start = pos + 4;
    }

    // 查找下一个起始码以确定当前 NAL 的结束位置
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

    // 提取 NAL 数据（不含起始码）
    if (nal_end > nal_data_start) {
      uint32_t nal_size = nal_end - nal_data_start;
      const uint8_t* nal_data = bitstream + nal_data_start;

      // 跳过 "unregistered user data SEI" (类似 x265 插件的处理)
      // NAL type prefix SEI = 0x4e (39<<1), SEI payload type unregistered = 5
      static const uint8_t NAL_TYPE_PREFIX_SEI_BYTE = 0x4e;
      static const uint8_t SEI_PAYLOAD_UNREGISTERED_USER_DATA = 5;
      if (nal_size >= 3 && nal_data[0] == NAL_TYPE_PREFIX_SEI_BYTE
          && nal_data[2] == SEI_PAYLOAD_UNREGISTERED_USER_DATA) {
        // 跳过
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
//                    编码实现
// ============================================================

static heif_error svt_hevc_init_encoder(encoder_struct_svt_hevc* encoder,
                                         const heif_image* image,
                                         bool image_sequence,
                                         uint32_t framerate_num,
                                         uint32_t framerate_denom)
{
  // 获取图像信息
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

  // 清理之前的编码器
  if (encoder->encoder_initialized) {
    EbDeinitEncoder(encoder->svt_encoder);
    encoder->encoder_initialized = false;
  }
  if (encoder->svt_encoder) {
    EbDeinitHandle(encoder->svt_encoder);
    encoder->svt_encoder = nullptr;
  }

  // 步骤 1: 创建编码器句柄
  EB_ERRORTYPE eb_err = EbInitHandle(
      &encoder->svt_encoder, nullptr, &encoder->enc_params);

  if (eb_err != EB_ErrorNone) {
    return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Encoder_initialization,
        kError_encoder_init_failed
    };
  }

  // 步骤 2: 配置编码参数
  EB_H265_ENC_CONFIGURATION* config = &encoder->enc_params;

  config->sourceWidth = width;
  config->sourceHeight = height;
  config->encoderBitDepth = bit_depth;
  config->encoderColorFormat = EB_YUV420;
  config->encMode = encoder->enc_preset;
  config->qp = encoder->qp;
  config->rateControlMode = 0;  // CQP 模式
  config->threadCount = encoder->threads;

  if (image_sequence) {
    config->intraPeriodLength = encoder->intra_period;
    config->hierarchicalLevels = 3;
    config->predStructure = 2; // Random Access
    config->framesToBeEncoded = 0; // 未知帧数

    if (framerate_denom > 0) {
      config->frameRateNumerator = framerate_num;
      config->frameRateDenominator = framerate_denom;
    }
    else {
      config->frameRate = 30;
    }
  }
  else {
    // 静态图像模式
    config->intraPeriodLength = -1;
    config->hierarchicalLevels = 0;
    config->predStructure = 2;
    config->framesToBeEncoded = 1;
    config->frameRate = 1;
    config->frameRateNumerator = 0;
    config->frameRateDenominator = 0;
  }

  // 位深度对应的 profile
  if (bit_depth == 8) {
    config->profile = 1;  // Main
  }
  else {
    config->profile = 2;  // Main 10
  }

  config->tier = 0;   // Main tier
  config->level = 0;  // 自动

  // 产生 VPS/SPS/PPS
  config->codeVpsSpsPps = 1;
  config->codeEosNal = 0;

  // HDR 支持
  heif_color_profile_nclx* nclx = nullptr;
  heif_error err = heif_image_get_nclx_color_profile(image, &nclx);
  if (err.code == heif_error_Ok && nclx) {
    if (nclx->transfer_characteristics == 16) {  // PQ
      config->highDynamicRangeInput = 1;
      config->videoUsabilityInfo = 1;
    }

    // MDCV/CLLI 如果可用的话
    config->maxCLL = 0;
    config->maxFALL = 0;

    heif_nclx_color_profile_free(nclx);
  }

  // 步骤 2: 设置参数
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

  // 步骤 3: 初始化编码器
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

  // 获取流头 (VPS/SPS/PPS)
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


static heif_error svt_hevc_encode_frame(encoder_struct_svt_hevc* encoder,
                                         const heif_image* image,
                                         uintptr_t frame_nr,
                                         bool is_last_frame)
{
  int width = heif_image_get_width(image, heif_channel_Y);
  int height = heif_image_get_height(image, heif_channel_Y);
  int bit_depth = heif_image_get_bits_per_pixel_range(image, heif_channel_Y);
  bool isGreyscale = (heif_image_get_colorspace(image) == heif_colorspace_monochrome);

  // 分配输入缓冲区
  EB_BUFFERHEADERTYPE input_buffer;
  memset(&input_buffer, 0, sizeof(input_buffer));
  input_buffer.nSize = sizeof(EB_BUFFERHEADERTYPE);

  EB_H265_ENC_INPUT input_pic;
  memset(&input_pic, 0, sizeof(input_pic));

  int y_stride, cb_stride, cr_stride;

  // 获取图像平面
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
  // 注意: 不在图像帧上设置 EOS 标记
  // EOS 通过单独的空缓冲区发送（与 SVT-HEVC 参考应用一致）
  input_buffer.nFlags = 0;
  input_buffer.pts = frame_nr;
  input_buffer.pAppPrivate = reinterpret_cast<void*>(frame_nr);
  // EB_INVALID_PICTURE 表示由编码器自动决定 slice 类型
  input_buffer.sliceType = EB_INVALID_PICTURE;

  // 发送帧
  EB_ERRORTYPE eb_err = EbH265EncSendPicture(encoder->svt_encoder, &input_buffer);
  if (eb_err != EB_ErrorNone) {
    return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Unspecified,
        kError_encode_failed
    };
  }

  // 非阻塞获取可用的编码结果
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


// ============================================================
//              单帧编码 (heif_encoder_plugin)
// ============================================================

static heif_error svt_hevc_encode_image(void* encoder_raw, const heif_image* image,
                                         heif_image_input_class input_class)
{
  auto* encoder = (encoder_struct_svt_hevc*) encoder_raw;

  // 初始化编码器
  heif_error err = svt_hevc_init_encoder(encoder, image, false, 1, 25);
  if (err.code) {
    return err;
  }

  // 编码单帧 (不标记 EOS，与参考应用一致)
  err = svt_hevc_encode_frame(encoder, image, 0, false);
  if (err.code) {
    return err;
  }

  // 发送单独的 EOS 缓冲区（与 SVT-HEVC 参考应用 ProcessInputBuffer 的做法一致）
  EB_BUFFERHEADERTYPE eos_buffer;
  memset(&eos_buffer, 0, sizeof(eos_buffer));
  eos_buffer.nSize = sizeof(EB_BUFFERHEADERTYPE);
  eos_buffer.nFlags = EB_BUFFERFLAG_EOS;
  eos_buffer.pBuffer = nullptr;
  eos_buffer.nFilledLen = 0;
  eos_buffer.nAllocLen = 0;
  // EB_INVALID_PICTURE 表示由编码器自动决定 slice 类型（与参考应用一致）
  eos_buffer.sliceType = EB_INVALID_PICTURE;

  EB_ERRORTYPE eos_err = EbH265EncSendPicture(encoder->svt_encoder, &eos_buffer);
  if (eos_err != EB_ErrorNone) {
    EbDeinitEncoder(encoder->svt_encoder);
    encoder->encoder_initialized = false;
    return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Unspecified,
        kError_encode_failed
    };
  }

  // 阻塞获取所有编码数据
  EB_BUFFERHEADERTYPE* output_buffer = nullptr;
  for (;;) {
    EB_ERRORTYPE eb_err = EbH265GetPacket(encoder->svt_encoder, &output_buffer, 1);

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

  // 清理编码器
  EbDeinitEncoder(encoder->svt_encoder);
  encoder->encoder_initialized = false;

  return heif_error_ok;
}


// ============================================================
//              序列编码 (heif_encoder_plugin v4)
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

  // 编码帧 (不是最后一帧)
  return svt_hevc_encode_frame(encoder, image, frame_nr, false);
}


static heif_error svt_hevc_end_sequence_encoding(void* encoder_raw)
{
  auto* encoder = (encoder_struct_svt_hevc*) encoder_raw;

  if (!encoder->encoder_initialized) {
    return heif_error_ok;
  }

  // 发送 EOS 标记以刷新编码器（与 SVT-HEVC 参考应用 ProcessInputBuffer 一致）
  EB_BUFFERHEADERTYPE eos_buffer;
  memset(&eos_buffer, 0, sizeof(eos_buffer));
  eos_buffer.nSize = sizeof(EB_BUFFERHEADERTYPE);
  eos_buffer.nFlags = EB_BUFFERFLAG_EOS;
  eos_buffer.pBuffer = nullptr;
  eos_buffer.nFilledLen = 0;
  eos_buffer.nAllocLen = 0;
  eos_buffer.pAppPrivate = nullptr;
  // EB_INVALID_PICTURE 表示由编码器自动决定 slice 类型（与参考应用一致）
  eos_buffer.sliceType = EB_INVALID_PICTURE;

  EB_ERRORTYPE eos_err = EbH265EncSendPicture(encoder->svt_encoder, &eos_buffer);
  if (eos_err != EB_ErrorNone) {
    EbDeinitEncoder(encoder->svt_encoder);
    encoder->encoder_initialized = false;
    return {
        heif_error_Encoder_plugin_error,
        heif_suberror_Unspecified,
        kError_encode_failed
    };
  }

  // 阻塞获取所有剩余的编码数据 (picSendDone=1)
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

  // 清理编码器
  EbDeinitEncoder(encoder->svt_encoder);
  encoder->encoder_initialized = false;

  return heif_error_ok;
}


// ============================================================
//              获取压缩数据
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
//              插件结构体定义
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
        /* set_parameter_boolean */ svt_hevc_set_parameter_integer, // boolean maps to integer (same pattern as x265)
        /* get_parameter_boolean */ svt_hevc_get_parameter_integer, // boolean maps to integer (same pattern as x265)
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
heif_plugin_info plugin_info {
  1,
  heif_plugin_type_encoder,
  &encoder_plugin_svt_hevc
};
#endif
```

---

## 6. heif-enc 命令行工具集成

### 6.1 使用方式

heif-enc 工具已经支持通过 `--encoder` / `-e` 参数选择编码器，**无需修改 heif-enc 代码**。SVT-HEVC 编码器注册后，可以直接通过以下方式使用：

```bash
# 列出所有可用编码器（确认 svt-hevc 已注册）
heif-enc --list-encoders

# 使用 SVT-HEVC 编码器编码 HEIC 文件
heif-enc -e svt-hevc -o output.heic input.png

# 设置质量参数
heif-enc -e svt-hevc -q 80 -o output.heic input.png

# 设置 SVT-HEVC 特定参数
heif-enc -e svt-hevc -p preset=3 -o output.heic input.png
heif-enc -e svt-hevc -p qp=28 -o output.heic input.png
heif-enc -e svt-hevc -p threads=4 -o output.heic input.png
```

### 6.2 heif-enc 的编码器选择机制

heif-enc 中的编码器选择逻辑（无需修改）：

```cpp
// examples/heif-enc.cc 中的相关代码

// 1. 获取所有可用的编码器描述符
heif_get_encoder_descriptors(heif_compression_HEVC, nullptr, descriptors, MAX_ENCODERS);

// 2. 如果用户指定了 --encoder svt-hevc
if (!encoderId.empty()) {
    for (int i = 0; i < count; i++) {
        if (encoderId == heif_encoder_descriptor_get_id_name(descriptors[i])) {
            selected = i;
            break;
        }
    }
}

// 3. 创建编码器实例
heif_context_get_encoder_for_format(context, heif_compression_HEVC, &encoder);
// 或者使用指定的描述符
heif_context_get_encoder(context, descriptors[selected], &encoder);

// 4. 设置参数（用户通过 -p key=value 传递）
heif_encoder_set_parameter(encoder, name, value);
```

### 6.3 可用参数列表

| 参数名 | 类型 | 范围 | 默认值 | 说明 |
|--------|------|------|--------|------|
| `quality` | int | 0-100 | 50 | 质量级别，映射到 QP |
| `preset` | int | 0-11 | 7 | 编码预设，0=最高质量，11=最高速度 |
| `qp` | int | 0-51 | 32 | 直接 QP 值 |
| `threads` | int | 0-256 | 0 | 线程数，0=自动 |

---

## 7. 注意事项

### 7.1 SVT-HEVC 与 x265 的关键差异

1. **异步 API 模型**:
   - x265 使用同步 `encoder_encode()` 调用，发送帧后立即返回编码结果
   - SVT-HEVC 使用异步 `EbH265EncSendPicture()` + `EbH265GetPacket()` 模式，发送和接收之间存在流水线延迟
   - **影响**: 编码和获取数据的时序需要特殊处理，需要在 `encode_image()` 中先发送帧再循环获取所有结果

2. **NAL 输出格式**:
   - x265 直接返回 `x265_nal` 结构体数组，每个 NAL 独立
   - SVT-HEVC 返回 Annex B 格式的比特流（带 0x00000001 起始码），多个 NAL 合并在一个缓冲区中
   - **影响**: 需要实现 `parse_nal_units_from_bitstream()` 函数来拆分

3. **无损编码**:
   - x265 支持无损编码 (`bLossless = 1`)
   - SVT-HEVC **不支持** 无损编码（API 中无此参数，CQP 模式下 QP=0 仍为有损）
   - **影响**: `supports_lossless_compression` 设为 `false`，`set_parameter_lossless(true)` 返回错误

4. **色度格式支持**:
   - x265 支持 4:2:0、4:2:2、4:4:4
   - SVT-HEVC 也支持 4:2:0、4:2:2、4:4:4（通过 `encoderColorFormat` 参数），默认为 4:2:0
   - **影响**: `query_input_colorspace()` 可根据配置返回对应的色度格式，默认使用 `heif_chroma_420`

5. **位深度**:
   - x265 支持 8/10/12 位
   - SVT-HEVC 仅支持 8 和 10 位
   - **影响**: 12 位输入需要返回错误

6. **EOS 处理**:
   - x265 通过传入 NULL 帧触发刷新
   - SVT-HEVC 需要发送一个单独的 EOS 缓冲区（`pBuffer=NULL`, `nFlags=EB_BUFFERFLAG_EOS`）
   - **影响**: `end_sequence_encoding()` 中需要发送 EOS 缓冲区并阻塞获取所有剩余数据

### 7.2 编码器初始化时机

与 x265 类似，SVT-HEVC 编码器需要在知道图像尺寸后才能初始化。因此：
- `new_encoder()` 中**不创建** SVT-HEVC 编码器实例
- 在 `encode_image()` 或 `start_sequence_encoding()` 中延迟初始化

### 7.3 内存管理

1. **输入图像**: SVT-HEVC 的 `EbH265EncSendPicture()` 不会拷贝输入数据，而是直接引用
   - 需要确保在 `EbH265GetPacket()` 获取结果之前，输入数据保持有效
   - 对于静态图像编码（单帧），这不是问题，因为在同一函数调用中完成

2. **输出缓冲区**: 必须通过 `EbH265ReleaseOutBuffer()` 释放
   - 在解析 NAL 单元后立即释放

3. **编码器句柄**: 必须按顺序调用 `EbDeinitEncoder()` → `EbDeinitHandle()`

### 7.4 静态图像编码特殊处理

SVT-HEVC 设计为视频编码器，用于静态图像（单帧）编码需要特殊配置：
- `intraPeriodLength = -1` (无周期性 I 帧)
- `hierarchicalLevels = 0` (无层次结构，Flat 模式)
- `predStructure = 2` (Random Access，与单帧无关但需要有效值)
- `framesToBeEncoded = 1` (仅 1 帧)
- `frameRate = 1` (最低帧率)
- 发送帧后紧接着发送一个 **单独的 EOS 缓冲区**（`pBuffer=NULL`, `nFlags=EB_BUFFERFLAG_EOS`），与 SVT-HEVC 参考应用的做法一致
- 使用 `EbH265GetPacket(handle, &output, picSendDone=1)` 阻塞等待编码完成

### 7.5 图像尺寸要求

SVT-HEVC 对图像尺寸有明确要求（来自官方文档）：
- **最小尺寸**: 64×64 像素
- **最大尺寸**: 8192×4320 像素
- **对齐要求**: 宽度和高度不是 8 的倍数时，SVT-HEVC 会自动进行 padding
- 如果使用 4:2:0，宽高应为偶数（libheif 的色彩空间转换会处理）

### 7.6 SVT-HEVC 库的可用性

- SVT-HEVC 项目地址: https://github.com/OpenVisualCloud/SVT-HEVC
- pkg-config 名称: `SvtHevcEnc`
- 头文件: `EbApi.h`（位于 `svt-hevc/` 或 `EbApi.h`）
- 库文件: `libSvtHevcEnc.so` / `SvtHevcEnc.lib`
- **注意**: SVT-HEVC 项目已于 2021 年由 Intel 宣布停止维护（DISCONTINUATION OF PROJECT），但代码仍然可用且功能完整。对于需要高并行 HEVC 编码的场景，它仍然是一个有价值的选择

### 7.7 与 SVT-AV1 插件的命名冲突

现有的 `encoder_svt.cc` 是 SVT-AV1 编码器。为避免命名冲突：
- 新插件文件命名为 `encoder_svt_hevc.h/cc`
- CMake 变量使用 `SvtHevcEnc` 前缀
- 插件 id_name 设为 `"svt-hevc"`
- 编译宏使用 `HAVE_SvtHevcEnc` 和 `PLUGIN_SvtHevcEnc`

### 7.8 编译为动态插件或内建后端

通过 CMake 选项控制：
- `WITH_SvtHevcEnc=ON` — 启用编译
- `WITH_SvtHevcEnc_PLUGIN=ON` — 编译为动态插件（.so）
- `WITH_SvtHevcEnc_PLUGIN=OFF` — 编译为内建后端

默认建议：`OFF ON`（默认关闭，启用时编译为插件）

---

## 8. 测试方案

### 8.1 编译测试

```bash
# 安装 SVT-HEVC 依赖
sudo apt install libsvthevcenc-dev  # 或从源码编译

# 配置并编译
cmake -B build \
  -DWITH_SvtHevcEnc=ON \
  -DWITH_SvtHevcEnc_PLUGIN=OFF \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# 确认编码器已注册
./build/examples/heif-enc --list-encoders | grep svt-hevc
```

### 8.2 功能测试

```bash
# 基本编码测试
./build/examples/heif-enc -e svt-hevc -o test_svthevc.heic test_input.png

# 验证输出文件
./build/examples/heif-info test_svthevc.heic
./build/examples/heif-dec test_svthevc.heic -o test_decoded.png

# 不同质量级别
for q in 10 30 50 70 90; do
  ./build/examples/heif-enc -e svt-hevc -q $q -o test_q${q}.heic test_input.png
  echo "Quality $q: $(stat -c%s test_q${q}.heic) bytes"
done

# 不同预设
for p in 0 3 7 11; do
  ./build/examples/heif-enc -e svt-hevc -p preset=$p -o test_p${p}.heic test_input.png
done

# 序列编码测试
./build/examples/heif-enc -e svt-hevc -o test_seq.heic frame1.png frame2.png frame3.png
```

### 8.3 边界条件测试

```bash
# 小图像测试
convert -size 64x64 xc:red small.png
./build/examples/heif-enc -e svt-hevc -o small.heic small.png

# 大图像测试
convert -size 4096x3072 xc:blue large.png
./build/examples/heif-enc -e svt-hevc -o large.heic large.png

# 10位测试 (如有10位源)
./build/examples/heif-enc -e svt-hevc -o test_10bit.heic input_10bit.png

# 无损编码应该报错
./build/examples/heif-enc -e svt-hevc -L -o test_lossless.heic test_input.png
# 预期: 报错，因为 SVT-HEVC 不支持无损
```

### 8.4 与 x265 对比测试

```bash
# 相同质量，比较文件大小和编码速度
time ./build/examples/heif-enc -e x265 -q 50 -o test_x265.heic test_input.png
time ./build/examples/heif-enc -e svt-hevc -q 50 -o test_svthevc.heic test_input.png

echo "x265:     $(stat -c%s test_x265.heic) bytes"
echo "svt-hevc: $(stat -c%s test_svthevc.heic) bytes"
```

---

## 附录: 完整修改清单

### A. 新增文件

1. **`cmake/modules/FindSvtHevcEnc.cmake`** — CMake 查找模块
2. **`libheif/plugins/encoder_svt_hevc.h`** — 插件头文件
3. **`libheif/plugins/encoder_svt_hevc.cc`** — 插件实现（~650 行）

### B. 修改文件

4. **`CMakeLists.txt`** (根目录)
   - 第 155 行后: 添加 `plugin_option(SvtHevcEnc ...)` 和 `find_package(SvtHevcEnc)`
   - 第 316 行后: 添加 `plugin_compilation_info(SvtHevcEnc ...)`
   - 第 361 行: 修改 HEIC 编码支持条件，加入 `SvtHevcEnc_FOUND`

5. **`libheif/plugins/CMakeLists.txt`**
   - 第 94 行后: 添加 `set(SvtHevcEnc_sources ...)` 和 `plugin_compilation(...)`

6. **`libheif/plugin_registry.cc`**
   - 第 42 行后: 添加 `#if HAVE_SvtHevcEnc` 头文件包含
   - 第 164 行后: 添加 `register_encoder(get_encoder_plugin_svt_hevc())`

### C. 总计修改量

| 项目 | 行数 |
|------|------|
| encoder_svt_hevc.cc | ~650 行（新增） |
| encoder_svt_hevc.h | ~35 行（新增） |
| FindSvtHevcEnc.cmake | ~25 行（新增） |
| CMakeLists.txt 修改 | ~8 行 |
| plugins/CMakeLists.txt 修改 | ~3 行 |
| plugin_registry.cc 修改 | ~6 行 |
| **总计** | **~727 行** |

---

## 实施状态

> ✅ **所有实施步骤已完成**

| 步骤 | 状态 | 说明 |
|------|------|------|
| 步骤 1: FindSvtHevcEnc.cmake | ✅ 已完成 | CMake 查找模块已创建 |
| 步骤 2: 根 CMakeLists.txt | ✅ 已完成 | 选项、find_package、编译信息、HEIC编码条件均已添加 |
| 步骤 3: 插件 CMakeLists.txt | ✅ 已完成 | 插件编译配置已添加 |
| 步骤 4: plugin_registry.cc | ✅ 已完成 | 编码器注册已添加 |
| 步骤 5: encoder_svt_hevc.h | ✅ 已完成 | 头文件已创建 |
| 步骤 6: encoder_svt_hevc.cc | ✅ 已完成 | 完整实现（~700 行）|
| 编译验证 | ✅ 已完成 | CMake配置通过，项目编译成功 |
| 测试验证 | ✅ 已完成 | 现有测试全部通过 |
