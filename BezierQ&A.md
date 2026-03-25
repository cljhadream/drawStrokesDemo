# Bezier 回放 Q&A

本文记录贝塞尔回放页面（`BezierReplayActivity`）在接入 `sdk-jni` 日志、回放显示、缩放与防抖过程中遇到的问题与解决思路，便于后续复现与调整参数。

---

## Q1：如何把 sdk-jni 的 log 变成「一条笔划」？

**现象 / 需求**
- `sdk-jni-2026_03_24.log` 中包含 DOWN / MOVE / UP 的报点日志。
- 需要按 `DOWN -> MOVE -> … -> UP` 聚合为一条笔划，并把点序列喂给回放模块。

**解决思路**
- 基于正则解析三类行：
  - DOWN：`onDown end ... X:...,Y:...,Time:...`
  - MOVE：`Touch onMove touchPoints ... isPredict:0 ... x:..., y:..., time:...`
  - UP：`onUp end ... X:...,Y:...,Time:...`
- 只采集 `isPredict:0`，过滤预测点。
- 连续重复点做去重，避免同点重复造成局部“堆点”。
- 时间戳统一减去首点时间戳（从 0 开始），便于回放/对齐。
- 若出现“未结束笔划又遇到新的 DOWN”，插入 `CANCEL` 结束上一条笔划，保证切分正确。

**关键位置**
- `BezierReplayActivity.kt`：`parseSdkJniLogToReplayPoints()`

---

## Q2：log 如何给进去？能不能直接写死？

**结论**
可以写死，但推荐使用 assets 或应用私有目录的文件加载。

**方案 A（推荐）：打包进 APK 的 assets**
- 把 log 放到：`app/src/main/assets/sdk-jni-2026_03_24.log`
- 应用启动时读取 assets 作为样本来源。

**方案 B：adb push 到应用外部文件目录**
- 推到：`/sdcard/Android/data/<package>/files/sdk-jni-2026_03_24.log`
- 适合频繁替换日志，不需要重新打包 APK。

**方案 C：把整份 log 写进 Kotlin 字符串（不推荐）**
- 大文件会让编译/安装/内存压力都变大，不利于迭代。

**关键位置**
- `BezierReplayActivity.kt`：`readSdkJniLogTextOrNull()` / `readSdkJniLogFromAssetsOrNull()`

---

## Q3：增加双指缩放，放大看细节；轨迹不变、粗细不变但能矢量放大

**目标解释**
- “矢量放大”指的是：通过视图变换（矩阵缩放和平移）放大画面，而不是重采样或拉伸位图。
- “粗细不变”指的是：视觉上的线宽保持不随视图缩放变化。

**解决思路（回放 View / Canvas 绘制）**
- 使用 `Matrix` 对整个 canvas 做 `scale + translate`。
- 为保持线宽不变，绘制时用 `strokeWidth = baseStrokeWidthPx / currentScale` 做反向补偿。
- 原始点圆点半径也做同样的反向补偿，避免放大后点变“很粗”遮挡细节。
- 放大后支持单指拖拽平移。

**解决思路（OpenGL 主画布）**
- 通过双指缩放计算 `currentScale/translateX/translateY`，并通过 JNI 传给 native 渲染层（例如 `NativeBridge.setViewTransform`）。
- 在 shader 中按缩放因子做线宽反向补偿，保持物理宽度不变。

**关键位置**
- `BezierReplayActivity.kt`：`onDraw()`（矩阵变换与线宽补偿）、`onTouchEvent()`（缩放/平移）
- `StrokeGLSurfaceView.kt`：`ScaleGestureDetector` + `NativeBridge.setViewTransform(...)`

---

## Q4：放大后发现「粉线」和「蓝点」不重合，曲线超出点轨迹

**现象**
放大局部后发现回放曲线会“超过”原始报点轨迹，尤其拐角与折线区域更明显。

**原因分析**
- 如果采用“拟合二次贝塞尔控制点”的方式（例如挑选离直线最远点推导控制点），在拐角/曲率变化处很容易产生超调（overshoot）。
- 超调在正常比例下不一定明显，但放大后非常直观。

**解决思路**
- 回放曲线改为更“贴点”的分段二次平滑：
  - 用相邻点的中点作为段连接点
  - 段控制点直接使用原始点（或去噪后的点）
- 该方案的特性是更贴近点序列，不容易产生明显外扩。

**关键位置**
- `BezierReplayActivity.kt`：`buildFinalSegments()`

---

## Q5：放大看细节时，硬件细微抖动也被放大了，需要防抖

**目标**
- 去掉硬件采样造成的高频细抖动，但尽量不破坏笔画拐角与结构。
- 原始报点仍可显示用于对照，便于评估防抖强度是否合适。

**解决思路（两级去噪 + 门控平滑）**
1) 距离阈值过滤（去掉“几乎没移动”的点）
- 相邻点距离小于 `minDistPx`（如 0.8px）则丢弃，压掉“围绕轨迹抖动但位移很小”的噪声点。

2) 角度 + 步长门控的弱平滑（只在近似直线的小步长区域平滑）
- 对每个中间点，用前后向量夹角余弦 `cos` 判断是否“接近直线”：
  - `cos` 大（如 ≥ 0.95）才平滑
  - 拐角处（`cos` 小）不动，避免把拐角磨平
- 额外限制最大步长 `maxSegPx`（如 8px），避免跨较大结构造成形变。
- 平滑公式使用弱 3 点核：`p1' = (p0 + 2*p1 + p2) / 4`，重复少量轮次（如 2 次）。

3) 最后再次做一次距离过滤
- 避免平滑产生过密的点，减少视觉“毛边”。

**关键位置**
- `BezierReplayActivity.kt`：`denoiseStroke()`

**可调参数建议**
- 抖动仍明显：增大 `minDistPx` 或增加平滑轮次
- 觉得过于平滑：减小 `minDistPx` 或减少平滑轮次，或提高 `cosThreshold`（更严格才平滑）

---

## Q6：验证方式

- 本地构建：`./gradlew.bat :app:lintDebug :app:assembleDebug`
- 回放页验证点：
  - 蓝点（原始报点）是否连续、笔划分段是否正确（DOWN/MOVE/UP）
  - 放大后线宽是否保持不变
  - 防抖是否仅削弱抖动且不破坏拐角结构

---

## Q7：如何解决二次贝塞尔曲线后“丢失用户真实笔迹”的问题？

**现象**
- 使用“拟合二次贝塞尔 + 重采样”的路径后，笔迹会出现：
  - 拐角被磨平、细节丢失（用户真实的微小转折被平滑掉）
  - 局部超调（曲线超过报点轨迹），放大后尤为明显
  - 节奏变化丢失（原始点密度/速度信息被重采样均匀化）

**根因**
- “拟合”在数学上是在找一条更平滑/更简洁的曲线去解释点集，本质上会牺牲细节。
- 控制点推导在拐角/高曲率处容易产生超调；再叠加重采样，会把原始点的分布特征抹掉。

**解决思路**
1) 原始数据与渲染数据分离
- 蓝点始终绘制原始报点（真值），不参与任何拟合与平滑，保证可对照与可回归。

2) 放弃“全局拟合 + 重采样”，改为“局部贴点”的分段二次曲线
- 使用相邻点中点作为段连接点；
- 段控制点直接取原始点（或轻量去噪后的点）；
- 每段只受局部 3 个点影响，不会因单段拟合而把整体结构带偏，且基本不会出现明显外扩。

3) 防抖只做在渲染点序列上，且强度可控
- 防抖作用于粉线用的点序列，不修改蓝点；
- 通过“距离阈值过滤 + 角度/步长门控弱平滑”压掉硬件高频抖动，同时尽量保留拐角与结构。

**对应实现**
- 原始点：`rawStrokePointsScreen`
- 防抖后点：`smoothedStrokePointsScreen`（`denoiseStroke()`）
- 贴点分段二次曲线：`buildFinalSegments()`

---

## Q8：当前功能业务逻辑（从报点到显示）

本节用“业务视角”描述目前已经具备的功能链路，便于沟通与验收。整体分为两条：**Bezier 回放页（用于复现/分析）** 与 **OpenGL 主画布（用于实时渲染/性能验证）**。

### 8.1 Bezier 回放页（日志驱动的离线回放）

**入口**
- 主界面 `MainActivity` 点击“贝塞尔回放”进入 `BezierReplayActivity`。

**数据源优先级**
1) 应用外部文件目录：`/sdcard/Android/data/<package>/files/sdk-jni-2026_03_24.log`
2) 应用内部文件目录：`filesDir/sdk-jni-2026_03_24.log`
3) APK assets：`app/src/main/assets/sdk-jni-2026_03_24.log` 或 `sdk-jni.log`
4) 兜底：内置 `FIXED_POINTS_JSON`

**报点解析与笔划切分**
- 解析 `DOWN/MOVE/UP` 日志行，生成 `ReplayInputPoint` 序列（过滤预测点 `isPredict!=0`）。
- 按 `DOWN -> MOVE -> ... -> UP` 切分为多条笔划；异常情况下用 `CANCEL` 终止上一条笔划。

**坐标归一化与映射**
- 把日志坐标归一化到 0..1000 基准空间，保证不同日志都能在回放画布中显示完整。
- 根据 View 宽度把基准坐标映射到屏幕坐标。

**防抖与曲线生成**
- 蓝点：始终绘制原始报点（不做任何修改），用于对照。
- 粉线：对点序列做防抖（可调强度）后，生成“贴点分段二次曲线”用于绘制，避免拟合超调导致偏离报点轨迹。

**交互与显示**
- 双指缩放 + 单指平移查看细节。
- 缩放时线宽与点半径做反向补偿，保证视觉粗细不变。
- 可勾选“显示原始报点”开关进行对比。

### 8.2 OpenGL 主画布（实时输入 + Native 渲染）

**入口**
- 启动 App 默认进入 `MainActivity`，底层是 `StrokeGLSurfaceView` 承载 OpenGL ES 渲染。
- 左上角显示当前缩放比例（用于确认缩放状态）。

**输入采集与变换**
- `StrokeGLSurfaceView` 接收触摸事件。
- 将屏幕坐标反变换到世界坐标（考虑当前 scale/translate），保证缩放/平移时输入仍正确。

**笔划构建**
- `StrokeInputProcessor` 负责采样、平滑/重采样、压力处理等，把输入转成可渲染的点/压力序列。
- 通过 JNI 把笔划数据提交到 native（支持 live 更新与结束）。

**渲染与缩放**
- native 使用实例化绘制把多条笔划在尽量少的 draw call 内绘制出来。
- 双指缩放通过 `NativeBridge.setViewTransform(...)` 更新视图变换；线宽在 shader 侧按缩放因子反向补偿，保证物理宽度不变。
