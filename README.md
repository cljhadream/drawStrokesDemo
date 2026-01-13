# Android 高性能手写渲染引擎

一个基于 OpenGL ES 的高性能 Android 手写渲染引擎，专注于流畅渲染大量矢量笔划，并支持无损矢量缩放。

## 阶段进度

- 阶段一（已完成）
  - 集成 CMake/NDK，配置 `externalNativeBuild` 与 ABI 过滤（含模拟器 `x86/x86_64`）
  - 建立 `GLSurfaceView` 与自定义 `Renderer`
  - 创建 Kotlin↔C++ 的 JNI 桥接与本地库加载
  - C++ 层完成 OpenGL ES 基本初始化（混合、清屏色、视口）

- 阶段二（已完成）
  - 搭建 ES 3.2 渲染管线（顶点/几何/片元着色器），启用几何着色器
  - 设计 VAO/VBO（半精度 `half` 压缩）与 SSBO（位置与笔划元数据）
  - 单次实例化绘制：`glDrawArraysInstanced(GL_POINTS, 0, kMaxPointsPerStroke, totalStrokes)`
  - 每条笔划独立颜色与透明度，片元混合启用

- 阶段三（已完成）
  - 触摸输入采集与三点滑动平均平滑、固定步长重采样
  - Kotlin 侧打包数据并通过 `NativeBridge.addStroke(...)` 提交到 C++ 层
  - 新增 `NativeBridge.setViewScale(scale)`，每帧更新 `uViewScale`，实现无损矢量缩放
  - 几何着色器健壮性修正（退化方向长度判定）

- 阶段四（规划中）
  - 批量渲染与数据管线优化（批次提交、分块更新、回收策略）
  - 更高质量轨迹拟合（Catmull-Rom/贝塞尔），速度/压力自适应采样
  - 视图交互（双指缩放/平移）与撤销/重做、数据持久化

## 当前技术要点

- OpenGL ES 3.2 管线：顶点 + 几何 + 片元着色器，`uResolution` / `uViewScale` uniform
- 几何构造：几何着色器按方向与法线生成四点条带，保持笔宽物理不变
- GPU 缓冲：
  - VBO：`half(x, y, pressure)` 三元组，降低带宽与内存占用
  - SSBO(binding=1)：`vec2 positions[]` 存储位置
  - SSBO(binding=0)：笔划元数据（`start`, `count`, `baseWidth`, `color`）
  - 动态扩容与重新分配，避免频繁创建对象
- 实例化绘制：基于 `gl_InstanceID`/`gl_VertexID` 索引 SSBO 数据，一次调用渲染全部笔划
- 触摸输入：`StrokeInputProcessor` 在 `ACTION_UP` 打包并提交点与压力，默认颜色 RGBA
- 透明混合：`glEnable(GL_BLEND)` 与 `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`
- 设备要求：Manifest 要求 `OpenGL ES 3.2`；Gradle `compileSdk=35`, `minSdk=34`
- ABI 支持：`arm64-v8a`, `armeabi-v7a`, `x86`, `x86_64`

## 项目结构

```
app/src/main/
├── java/com/example/myapplication/
│   ├── MainActivity.kt              # 主 Activity
│   ├── StrokeGLSurfaceView.kt      # OpenGL 渲染视图
│   ├── NativeRenderer.kt           # 渲染器回调
│   ├── NativeBridge.kt             # JNI 接口桥接（含 setViewScale）
│   └── StrokeInputProcessor.kt     # 触摸输入处理、平滑与重采样
└── cpp/
    ├── CMakeLists.txt              # 构建 native-lib
    └── native-lib.cpp              # C++ 渲染引擎核心（ES 3.2 + 几何着色器）
```

## 构建与运行

### 环境要求
- Android SDK/NDK（自动安装：NDK side-by-side 27.x，CMake 3.22.1）
- 设备支持 OpenGL ES 3.2（或使用 `x86/x86_64` 模拟器）

### 构建
```powershell
./gradlew.bat assembleDebug -x lint
```

### 安装与启动
```powershell
./gradlew.bat installDebug
```
安装后打开应用，在屏幕上滑动即可生成并渲染笔迹。

### 缩放测试
在业务代码中调用：
```kotlin
NativeBridge.setViewScale(2.0f) // 将视图缩放至 2 倍
```

## 调试与日志
- C++ 渲染日志：`NativeLib`
- 输入处理日志：`StrokeInput`
- 渲染器日志：`NativeRenderer`

## 后续计划
- 批量提交与管线优化、手势缩放/平移、轨迹拟合、撤销/重做、数据持久化等