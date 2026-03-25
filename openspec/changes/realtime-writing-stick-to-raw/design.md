# 技术设计（Technical Design）

## 现状与问题定位
当前工程存在两套与“轨迹贴合”相关的实现线索：
1) 回放调试（Canvas）侧已经实现了“原始点分离 + 轻量去噪 + 贴点分段二次曲线”的核心思路：
   - `denoiseStroke()` 与 `buildFinalSegments()` 位于 `BezierReplayActivity.kt`
2) 实时书写（GL）侧已具备 live stroke JNI 通道与输入处理器：
   - `StrokeInputProcessor` 负责采集触摸点、生成预览点、落笔后提交
   - `NativeBridge.beginLiveStroke/updateLiveStrokeWithCount/endLiveStroke` 与 `native-lib.cpp` 对应实现

当前需要补齐的是：把 Q7 的“贴点分段二次曲线”几何生成策略接入实时书写，并保证“预览末端严格对齐报点末端”，同时避免 live stroke 在 native 侧以“每次更新新增一条 stroke”的方式导致的计数膨胀与内存增长。

## 总体方案
在 Kotlin 输入层与 C++ 渲染层分别完成两件事：
1) Kotlin：建立“原始报点序列(raw) 与 渲染点序列(render) 分离”的实时几何生成器
   - raw：按触摸事件采集、仅用于真值与最终构造输入
   - render：对 raw 做“尾段稳定化平滑 + 贴点分段二次曲线采样”得到
   - 关键约束：render 的最后点始终等于 raw 的最后点（末端对齐）
2) C++：将 live stroke 更新从“追加一条新 stroke”改为“复用一个固定 live slot 原地更新”
   - live slot 常驻一个 strokeId，inactive 时 count=0
   - update 仅更新该 slot 的 positions/pressures 与 meta
   - getStrokeCount 等统计口径排除 live slot

## Kotlin 侧：实时几何生成（贴点分段二次曲线）
### 数据结构
- `rawPointsWorld: MutableList<PointF>`：世界坐标系原始报点（真值）
- `rawPressures: MutableList<Float>`：与 rawPointsWorld 一一对应
- `committedAnchorWorld: MutableList<PointF>`：已稳定的渲染锚点（不会回滚重算）
- `tailRawWorld: MutableList<PointF>`：最近 K 个原始点窗口（用于回滚重算）
- `tailAnchorWorld: MutableList<PointF>`：Tail 对应的渲染锚点（由稳定化平滑生成）
- `renderPointsWorld: FloatArray` + `renderPressures: FloatArray`：用于提交给 native 的渲染点（Committed 曲线 + Tail 曲线采样拼接）

### 尾段稳定化平滑（仅作用在 Tail 的渲染锚点）
目标是在“末点下一个点未知”的实时阶段，既保持末端低时延与贴合，又能降低轻微抖动对观感的影响，并尽量不磨平折笔。

约束：
- 绝不修改 `rawPointsWorld`（真值保留）
- 末点严格对齐：`tailAnchorWorld.last == tailRawWorld.last`

建议采用“两级门控”的轻量平滑（实现可选其一，取工程可控性优先）：
1) 距离阈值过滤（抗微抖）：
   - 当新 raw 点与上一个 raw 点距离小于阈值时，允许不推动 Tail 重建（或只更新末点对齐）
2) 角度/步长门控的指数平滑（保角）：
   - 仅当最近两段夹角足够小且段长足够短时，对 Tail 的中间点做轻量指数平滑
   - 在高曲率/明显折笔处，直接透传 raw 作为 anchor，避免圆角化

平滑对象仅限 Tail 区间的渲染锚点 `tailAnchorWorld`，并在点“转正”为稳定点时冻结进 `committedAnchorWorld`。

### 贴点分段二次曲线
两段都使用“局部贴点”的分段二次策略，但 Tail 的末端控制点使用末端切线构造以提升平滑观感。

**基础贴点段（适用于 Committed 与 Tail 的非末端部分）**
- 输入锚点 `p0..p(n-1)`（这里的 `p` 指渲染锚点，不是 raw）
- 中点：`m(i)= (p(i)+p(i+1))/2`
- 二次段：
  - 首段：`(p0, p0, m0)`
  - 中间段：`(m(i-1), p(i), m(i))`

**Tail 末端段（末点对齐 + 末端切线）**
- 末点 `pEnd` 必须等于最新 raw 点（末端对齐）
- 末端方向 `dir = normalize(pEnd - pPrev)`（pPrev 为倒数第二个渲染锚点）
- 末端控制点按切线回退构造：`cEnd = pEnd - dir * L`
- 将 Tail 的最后一段（或最后两段）替换为“以切线为导向”的二次段，确保尾端不出现导数趋零的“钝收尾”
- `L` 为可调参数，并应随当前视图缩放换算（以屏幕像素意义稳定）

### 曲线采样为渲染点（polyline）
渲染层消费的是点序列（polyline），因此需将二次段采样为点：
- 采样步长以“屏幕像素”定义（例如 1px），再除以当前视图缩放换算到世界坐标
- 每段采样需保证包含段终点 `p2`，从而自动保证最终末端对齐 raw 末端

### 实时更新策略
在 `ACTION_MOVE` 的节流周期内：
- 追加 raw 点
- 维护“两段曲线”：
  - Committed：只追加，不回滚
  - Tail：最近 K 个点窗口，允许每次重建与回修
- 以 `committedAnchorWorld + tailAnchorWorld` 生成两段曲线采样后的 render 点
- 将 render 点通过 `updateLiveStrokeWithCount(points, pressures, count)` 发送到 native

为了降低“修正感”，K 为可调参数：K 越大，回修越不明显但重建开销越高；K 越小，延迟更低但回修可能更显著。默认值以观感优先（例如 8~12）并提供调参入口。

### 落笔提交策略
在 `ACTION_UP`：
- 将 Tail 中的渲染锚点冻结进 Committed（或对全量 raw 重新生成一次最终 render，确保一致）
- 基于完整 raw 序列生成最终 render 点（与实时预览同一算法与参数集合）
- 通过 `addStroke(points, pressures, color, type)` 提交一次最终笔迹
- 清空 live stroke（`endLiveStroke()`）

该策略的关键收益是：预览与最终一致，不再出现“预览一条，落笔另一条”的突变。

## C++ 侧：live slot 原地更新
### 目标
把 `updateLiveStrokeWithCount` 的语义改为“更新同一条 live stroke”，避免每次更新都占用一个新的 strokeId。

### 设计
- 新增全局状态：
  - `int gLiveStrokeId = -1;`
  - `bool gLiveActive;`（已有）
- 初始化/第一次 begin：
  - 若 `gLiveStrokeId < 0`：为 live slot 分配一个 stroke（push 一个 meta，占一个固定 slot）
  - inactive 时 `count=0`，shader 会把该实例裁剪掉或输出 offscreen
- `beginLiveStroke`：
  - 设置 `gLiveActive=true`，并准备 live meta（颜色/type/宽度等）
- `updateLiveStrokeWithCount`：
  - 使用 `gLiveStrokeId` 计算 `start = gLiveStrokeId * kMaxPointsPerStroke`
  - 将 points/pressures 写入对应 SSBO 区域并更新该 slot 的 meta（count=N 与 bounds）
- `endLiveStroke`：
  - `gLiveActive=false`
  - 将 live slot `count=0`，隐藏预览
- `getStrokeCount/getBlueStrokeCount`：
  - 统计时排除 `gLiveStrokeId`（避免 UI/测试将 live slot 计入笔划数）

## 可验证性与调试
- 复用现有回放数据（或录制输入）验证：
  - 端点一致性（最后渲染点 == 最后原始点）
  - 角点保留（折笔形态可见、无明显外扩）
- 推荐在回放界面复用同一套“去噪 + 贴点分段二次 + 采样”实现，确保算法可独立回归
