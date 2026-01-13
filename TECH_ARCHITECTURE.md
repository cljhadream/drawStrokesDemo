# 项目技术架构与渲染方案（OpenGL ES 矢量笔迹）

## 1. 目标与约束

- 目标：在 Android 设备上实现流畅的手写矢量笔迹渲染，并支持画布缩放与平移。
- 约束：渲染核心使用 OpenGL ES（当前使用 ES 3.1 着色器路径），SSBO 路径单帧通过一次实例化绘制调用渲染全部笔迹（`glDrawArraysInstanced`）。
- 大规模场景目标：10 万条笔划、每条约 1000 点（总点数达 1e8 级别），要求渲染路径可扩展、缓冲复用并尽量减少 CPU↔GPU 往返。

## 2. 技术栈

- UI/交互层：Kotlin
  - `GLSurfaceView` 承载渲染
  - 触摸采样、手势缩放（`ScaleGestureDetector`）
  - JNI 桥接（`NativeBridge`）
- 渲染引擎层：C++（Android NDK）
  - OpenGL ES 3.1：SSBO + Instanced Draw
  - 回退路径：OpenGL ES 3.0（不使用 SSBO，仅用于可见性诊断）
- 着色器：GLSL ES
  - 顶点着色器在单个 instance 内按 `gl_VertexID` 生成整条笔迹的三角条带几何（端帽 + 主体）
  - 片元着色器对主体边缘与端帽做抗锯齿（`fwidth` + `smoothstep`），并输出预乘 alpha
  - 结合 EGL 多重采样（MSAA）进一步平滑几何边缘（优先 4x，失败回退无 MSAA）

## 3. 运行时模块划分（Kotlin）

### 3.1 界面与渲染循环

- `MainActivity` 创建 `StrokeGLSurfaceView` 并在首次 `onResume` 绘制示例笔迹：`app/src/main/java/com/example/myapplication/MainActivity.kt:1-214`
- `StrokeGLSurfaceView`：
  - 创建 OpenGL 上下文、启用连续渲染：`app/src/main/java/com/example/myapplication/StrokeGLSurfaceView.kt:56-71`
  - 手势缩放与平移参数维护，并通过 JNI 同步给原生：`StrokeGLSurfaceView.kt:32-54`
- `NativeRenderer`：`GLSurfaceView.Renderer` 的 Kotlin 端实现，逐帧调用原生渲染：`app/src/main/java/com/example/myapplication/NativeRenderer.kt:11-25`

### 3.2 输入采样与点生成

- `StrokeInputProcessor` 负责：
  - 采集单指触摸点（世界坐标）与压力：`app/src/main/java/com/example/myapplication/StrokeInputProcessor.kt:40-94`
  - 基于 Catmull-Rom→Bezier 的固定步长重采样，并将超长笔迹按 `1024` 点分段：`StrokeInputProcessor.kt:240-373`
  - 实时预览：移动中以 ~16ms 节流更新当前“Live Stroke”，并在超过 `1024` 点时把已满段提前提交为正式笔迹：`StrokeInputProcessor.kt:96-123`

### 3.3 Kotlin→JNI 桥

- `NativeBridge` 暴露渲染与数据提交接口：`app/src/main/java/com/example/myapplication/NativeBridge.kt:11-36`
  - 生命周期：`onNativeSurfaceCreated/Changed/DrawFrame`
  - 视图变换：`setViewTransform(scale, cx, cy)`
  - 批量笔划：`addStrokeBatch(pointsFlat, pressuresFlat, counts, colors)`
  - 实时预览：`beginLiveStroke/updateLiveStroke/endLiveStroke`

## 4. 坐标系与视图变换（缩放/平移）

### 4.1 坐标系定义

- 屏幕坐标（Android 触摸）：左上为原点，Y 向下。
- 世界坐标（本项目约定）：以“未缩放的屏幕像素”为世界单位（世界坐标仍是像素度量，但不包含缩放/平移）。
  - Kotlin 在触摸时做逆变换得到世界坐标：`StrokeGLSurfaceView.kt:20-23`
- NDC（OpenGL）：X/Y ∈ [-1, 1]，Y 向上。
  - 顶点着色器在末端完成屏幕像素→NDC，并做 Y 翻转：`app/src/main/cpp/native-lib.cpp:518-525`

### 4.2 视图参数传递

- Kotlin 层维护：
  - `currentScale`（缩放倍数）
  - `translateX/translateY`（屏幕像素平移）
  - 通过 `NativeBridge.setViewTransform` 更新原生：`StrokeGLSurfaceView.kt:40-48`
- 原生层保存为 `gViewScale/gViewTranslateX/gViewTranslateY`，并在每帧作为 uniform 传入着色器：`app/src/main/cpp/native-lib.cpp:663-668`

## 5. GPU 数据组织与“矢量化绘制”

### 5.1 数据组织（SSBO + Meta）

- 单条笔迹不在 CPU 侧预生成完整三角形网格，而是上传“中心线采样点 + 压力”，由 GPU 在顶点/片元阶段生成覆盖区域（三角条带 + 抗锯齿边缘）。
- 固定上限：每条笔迹最多 `kMaxPointsPerStroke=1024` 个点：`app/src/main/cpp/native-lib.cpp:14-19`
- 元数据结构（每条笔迹一条）：
  - `start`：该笔迹在点池中的起始索引（按 `strokeId * 1024` 布局）
  - `count`：实际点数
  - `baseWidth`：基准宽度
  - `pad`：效果标记（0=普通透明混合，1=变暗/Darken）
  - `color[4]`：每条笔迹独立 RGBA
  - 定义：`app/src/main/cpp/native-lib.cpp:51-57`
- SSBO 绑定：
  - `binding=0`：meta 数组
  - `binding=1`：positions（vec2）
  - `binding=2`：pressures（float）
  - GLSL 声明：`app/src/main/cpp/native-lib.cpp:304-318`

### 5.2 实例化绘制（单次 Draw Call）

- 每条笔迹对应一个 Instance（`gl_InstanceID` 为笔迹 id）。
- 单个 Instance 内，`gl_VertexID` 遍历 `[0, kVertsPerStroke)`，生成整条笔迹的三角条带：
  - `kVertsPerStroke = kMaxPointsPerStroke * 2 + 8`（起笔端帽 4 个顶点 + 主体 2*1024 个顶点 + 收笔端帽 4 个顶点）
  - 顶点着色器用 `count/start` 从 SSBO 读取中心线点与压力，并在屏幕空间计算偏移生成条带
- 真正绘制调用（SSBO 路径）：
  - `glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, kVertsPerStroke, drawCount)`
  - 位置：`app/src/main/cpp/native-lib.cpp:988-1001`

### 5.3 顶点着色器生成条带几何（主体 + 端帽）

- 主体条带：对每个采样点计算前后方向，在屏幕空间求法线，并对左右两侧生成偏移顶点（每点 2 个顶点）。
- 拐点连接：默认使用 miter，但会做限制以避免回折造成异常扇形几何：
  - 当方向几乎相反（`dot(dirPrev, dirNext)` 很小/为负）时退化为 bevel
  - 对 miter 长度做上限钳制（例如 `miterLen <= 4.0`）
- 端帽：起笔/收笔各生成一个 4 顶点矩形，通过片元阶段的圆/半平面 SDF 裁切得到圆头端帽。
- 宽度与缩放：当前半径按 `baseWidth * pressure * uViewScale` 参与屏幕空间偏移，缩放时笔触宽度会随缩放一起变化。

### 5.4 片元着色器抗锯齿与透明度

- 主体：使用插值后的 `vEdgeSigned/vHalfWidth` 构造到边缘距离，并用 `fwidth` + `smoothstep` 做抗锯齿。
- 端帽：使用 `vCapLocal/vCapRadius/vCapSign` 构造圆形 SDF，并与平面 SDF 做并/交得到“圆头/半圆”效果，再用 `fwidth` + `smoothstep` 抗锯齿。
- 输出：统一输出预乘 alpha（`fragColor = vec4(rgb * outA, outA)`）。

### 5.7 抗锯齿增强方案（MSAA + 着色器 AA 加宽）

- EGL 多重采样（MSAA）：
  - `GLSurfaceView` 使用自定义 `EGLConfigChooser` 优先选择 4x MSAA：`EGL_SAMPLE_BUFFERS=1`、`EGL_SAMPLES=4`，若设备不支持则回退到无 MSAA 的 8888+Depth16 配置。
  - 位置：`app/src/main/java/com/example/myapplication/StrokeGLSurfaceView.kt`
- 着色器 AA 加宽：
  - 在片元着色器中将 `fwidth(...)` 乘以系数（当前为 `1.5`），相当于增加过渡带宽度，降低高对比边缘的锯齿感：
    - `aaBody = max(fwidth(vEdgeSigned) * 1.5, 1.0)`
    - `aaCap = max(fwidth(sdf) * 1.5, 1.0)`
  - 对三种片元路径一致生效：普通输出、EXT framebuffer fetch、ARM framebuffer fetch：`app/src/main/cpp/native-lib.cpp`

### 5.5 颜色混合（普通透明 + 变暗）

- 背景：当前清屏为白色（便于观察变暗效果）：`app/src/main/cpp/native-lib.cpp:472-480` 与 `native-lib.cpp:787-791`
- 普通透明混合（预乘 alpha）：
  - `outRGB = srcRGB + dstRGB * (1 - srcA)`
  - `outA = srcA + dstA * (1 - srcA)`
- 变暗（Darken/变暗模式，按“笔划”生效）：
  - 记 `S`/`D` 为未预乘颜色（0..1），`Sa`/`Da` 为 alpha，`Sp=S*Sa`、`Dp=D*Da` 为预乘颜色
  - `B = min(D, S)`（逐通道取较小值）
  - `outA = Sa + Da - Sa * Da`
  - `outRGB(pre-mul) = Dp * (1 - Sa) + Sp * (1 - Da) + (Sa * Da) * B`
- SSBO 路径实现方式：
  - 若设备支持 `GL_EXT_shader_framebuffer_fetch` 或 `GL_ARM_shader_framebuffer_fetch`，在片元着色器中读取当前 framebuffer 的 `dst` 颜色，并基于每条笔划的 `pad` 标记选择“普通透明/变暗”，从而保持单次 `glDrawArraysInstanced` 绘制：`app/src/main/cpp/native-lib.cpp:460-589` 与 `native-lib.cpp:655-701`
  - 若设备不支持 framebuffer fetch 扩展，则 SSBO 路径退化为“固定功能混合 + 普通透明”，`pad` 不会触发变暗（仍保持单次 draw）。
  - ES 3.0 回退路径（逐条 `GL_LINE_STRIP`）会按笔划 `pad` 选择混合函数，因此仍可看到变暗，但该路径不满足“单次实例化绘制”约束，仅用于可见性诊断：`app/src/main/cpp/native-lib.cpp:880-902`

### 5.6 同一笔迹自交“并集”策略（深度缓冲）

- 目标：同一条笔迹自相交时，不出现重复混合导致的局部加深；视觉上更接近“并集覆盖”。
- 做法：
  - 顶点着色器为每条笔迹分配稳定深度（按 strokeId 映射到 depth 0..1）。
  - 渲染开启深度测试与深度写入（`GL_DEPTH_TEST` + `glDepthMask(GL_TRUE)`），并在每帧清除深度缓冲。
  - 由于同一条笔迹的所有片元深度相同，深度函数使用 `GL_LESS` 时，同一像素第一次写入后，后续等深片元会被拒绝，从而避免“自交处重复叠加”。
- 依赖：需要 EGL 配置包含深度缓冲（当前 `GLSurfaceView` 请求 16-bit depth：`StrokeGLSurfaceView.kt:72-74`），并在原生层清除 `GL_DEPTH_BUFFER_BIT`。

## 6. 性能优化点（当前实现）

### 6.1 渲染侧（GPU/Draw）

- 单次实例化绘制：用一次 `glDrawArraysInstanced` 画出所有笔迹，避免每条笔迹多次 draw。
- SSBO 承载大数据：positions/pressures/meta 走 `std430`，减少 attribute 带宽压力：`native-lib.cpp:301-307`
- 预分配大容量：启动时 `gAllocatedStrokes=4096`，减少频繁扩容与重分配：`native-lib.cpp:559-604`
- 缓冲扩容采用“新建更大缓冲+拷贝旧数据”：`resizeBufferCopy()`：`native-lib.cpp:66-79`
- 顶点数据 half-float（如果驱动支持）：降低 VBO 带宽与体积：`native-lib.cpp:538-574`

### 6.2 数据提交侧（CPU/JNI）

- Kotlin 批量提交：`StrokeBatcher` 将多条笔迹拼接后一次 `addStrokeBatch`：`app/src/main/java/com/example/myapplication/StrokeBatcher.kt:21-75`
- 固定每条笔迹最大点数 `1024`，超长笔迹 Kotlin 侧分段，避免原生侧被动截断导致形状缺失：`StrokeInputProcessor.kt:240-309`
- 实时绘制采用节流（~16ms）更新 Live Stroke，避免每个 MOVE 事件都触发一次 JNI 大数组传输：`StrokeInputProcessor.kt:96-100`
- 触摸抬笔时的批量提交通过 `queueEvent` 在 GL 线程触发，避免 UI 线程直接调用 JNI/GL：`app/src/main/java/com/example/myapplication/StrokeGLSurfaceView.kt:112-121`

### 6.3 鲁棒性（减少异常几何/伪影）

- 拐点连接对 miter 做限制并钳制最大长度，避免回折时产生异常扇形几何：`app/src/main/cpp/native-lib.cpp:453-486`
- 深度缓冲按笔迹做稳定排序，并用于避免同笔迹自交重复叠加：`app/src/main/cpp/native-lib.cpp:518-525`

## 7. 实时书写（Live Stroke）机制

- 目标：不需要抬笔，移动过程中笔迹实时显示。
- 方案：
  - 已提交的“完整段”作为正式笔迹进入 `gMetas`，会被实例化绘制覆盖。
  - 当前正在书写的最后一段作为 Live Stroke：
    - `strokeId = gMetas.size()`，复用预留槽位写入 positions/pressures/meta（不会 push 进 `gMetas`）。
    - 通过 `updateLiveStrokeWithCount` 高频覆盖该槽位的数据，实现实时预览：`app/src/main/cpp/native-lib.cpp:1158-1244`
  - 渲染时把 `drawCount = committedStrokes + (gLiveActive ? 1 : 0)` 作为实例数，并固定 `uBaseInstance=0`：`app/src/main/cpp/native-lib.cpp:939-1001`
  - 抬笔后：
    - 关闭 live 状态（`gLiveActive=false`），清空 `gLiveMeta.count`。
    - 对本次手势期间提交进 `gMetas` 的笔迹段批量设置 `pad=1`（用于 framebuffer fetch 设备的“变暗”效果）：`app/src/main/cpp/native-lib.cpp:1247-1274`
- Kotlin 触发点：
  - `ACTION_DOWN`：`beginLiveStroke` + 初始 `updateLiveStrokeWithCount`：`StrokeInputProcessor.kt:47-58`
  - `ACTION_MOVE`：`updateLivePreview()`：`StrokeInputProcessor.kt:60-65`
  - `ACTION_UP`：提交剩余段为正式笔迹 + `endLiveStroke`：`StrokeInputProcessor.kt:67-85`

## 8. 当前实现的边界说明

- 每条实例固定 `1024` 点上限，通过分段实现“无限长笔迹”。这会增加实例数量，但仍维持“单次 instanced draw”。
- 当前笔触宽度随视图缩放一起变化（位置与宽度都会按 `uViewScale` 进入屏幕空间）。如需“矢量缩放但笔触物理宽度不变”，可将半径从乘 `uViewScale` 改为除 `uViewScale`（以具体交互定义为准）。
