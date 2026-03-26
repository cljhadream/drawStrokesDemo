package com.example.myapplication

import android.graphics.PointF
import android.os.SystemClock
import android.view.MotionEvent

/**
 * 实时书写的输入处理器（Kotlin 侧）。
 *
 * 该类负责把「触摸报点」转换为「可渲染的点序列」，并通过 JNI 传给 native 渲染层。
 *
 * 核心思路（为尽量还原真实笔迹）：
 * - 原始报点只做最轻量的去重/距离过滤，尽量保留用户真实轨迹
 * - 使用“两段模型”：Committed 固定段 + Tail(K点)回滚段
 *   - 固定段一旦提交不再修改，保证历史轨迹稳定
 *   - 尾段允许回滚重算，用来在实时阶段获得更自然的末端曲线
 * - 曲线生成采用“局部贴点”的分段二次曲线，避免全局拟合导致超调/细节丢失
 * - MOVE 实时预览必须绘制到最新报点末端，降低书写时延感
 * - 可选：按真实业务链路验证时，会把点归一化到“宽=1000”的基准空间并做第二次贝塞尔拟合
 */
class StrokeInputProcessor(
    /**
     * 将屏幕坐标反变换到 world 坐标。
     * - world 坐标是 native 侧存储/渲染点坐标使用的空间
     * - 通过把输入变换到 world，可保证缩放/平移下采样一致
     */
    private val screenToWorld: (x: Float, y: Float) -> FloatArray,
    /**
     * 当前视图缩放（world -> screen 的缩放因子）。
     * - 用于把固定像素步长换算成 world 步长
     * - 用于尾段去噪的门控阈值（例如 0.8px、8px 等需换算到 world）
     */
    private val scaleProvider: () -> Float,
    /**
     * 当前 GLView 的尺寸（px）。
     * - 用于将 world 坐标归一化到“宽=1000”的业务基准空间
     */
    private val viewSizeProvider: () -> Pair<Int, Int>,
    /**
     * 提交正式笔划（UP 结束时）。
     * points: [x0,y0,x1,y1,...] world 坐标
     * pressures: 与点一一对应的压力
     */
    private val jniSubmit: (points: FloatArray, pressures: FloatArray, color: FloatArray, type: Int) -> Unit,
    /**
     * 开启实时预览笔划（DOWN）。
     */
    private val liveBegin: (color: FloatArray, type: Int) -> Unit,
    /**
     * 更新实时预览笔划（MOVE）。
     * - count 显式告诉 native 有效点数量，避免频繁创建新数组
     */
    private val liveUpdate: (points: FloatArray, pressures: FloatArray, count: Int) -> Unit,
    /**
     * 结束实时预览笔划（UP/CANCEL）。
     */
    private val liveEnd: () -> Unit,
) {
    /**
     * 单条笔划的最大点数上限（与 native 的 kMaxPointsPerStroke 对齐）。
     * - 实时预览与最终提交都受此上限约束
     */
    private val maxPoints = 1024

    /**
     * 原始输入点（world 坐标）与压力。
     * - 主要用于调试/备用算法
     * - 当前实时书写主链路使用“两段模型”的锚点序列
     */
    private val rawPoints = mutableListOf<PointF>()
    private val rawPressures = mutableListOf<Float>()

    /**
     * 压力做一个简单的低通滤波，减少压力抖动导致的粗细闪烁。
     */
    private var lastPressure = 0.5f

    /**
     * 实时预览更新节流：默认每 16ms 至多更新一次（约 60fps）。
     */
    private var lastLiveUpdateMs = 0L

    /**
     * 输出缓冲：
     * - live*Buf：最终喂给 native 的点与压力（world 坐标）
     * - tmp*Buf：中间过程缓冲（例如 base1000 空间的一次采样结果）
     */
    private val livePointsBuf = FloatArray(maxPoints * 2)
    private val livePressuresBuf = FloatArray(maxPoints)
    private val tmpPointsBuf = FloatArray(maxPoints * 2)
    private val tmpPressuresBuf = FloatArray(maxPoints)
    private var liveCount = 0

    /**
     * 尾段回滚窗口大小 K（可调）。
     * - K 越大：末端可回修范围越大，实时曲线更顺，但“回修感”也更明显
     * - K 越小：更贴近实时报点，但末端可能更像折线
     */
    var tailRollbackK: Int = 12

    /**
     * 业务流程验证开关：
     * - true：在“宽=1000”的基准空间里进行一次贴点二次曲线采样后，再做第二次贝塞尔拟合采样
     * - false：只做贴点二次曲线采样（更接近“贴点还原”的目标）
     */
    var enableBusinessSecondBezierFit: Boolean = true

    /**
     * 两段模型的数据结构：
     * - committedAnchorWorld：稳定锚点（历史轨迹），一旦进入就不再修改
     * - tailRawWorld：尾段原始报点（可回滚窗口），实时阶段允许重算
     */
    private val committedAnchorWorld = ArrayList<PointF>(2048)
    private val committedAnchorPressures = ArrayList<Float>(2048)
    private val tailRawWorld = ArrayList<PointF>(64)
    private val tailRawPressures = ArrayList<Float>(64)

    // 当前笔划颜色（RGBA），可在外部动态修改
    var currentColor: FloatArray = floatArrayOf(0.1f, 0.4f, 1.0f, 0.85f)
    var currentType: Int = 0

    /**
     * 取消当前笔划：
     * - 典型场景：双指缩放开始时，结束单指绘制并关闭 live 预览
     */
    fun cancelStroke() {
        rawPoints.clear()
        rawPressures.clear()
        lastLiveUpdateMs = 0L
        liveCount = 0
        committedAnchorWorld.clear()
        committedAnchorPressures.clear()
        tailRawWorld.clear()
        tailRawPressures.clear()
        liveEnd()
    }

    /**
     * 处理触摸事件（单指书写链路）。
     * - DOWN：开启 live 预览并写入首点
     * - MOVE：写入点 -> 触发实时预览更新（节流）
     * - UP：最后补一点并提交最终笔划 -> 关闭 live
     */
    fun onTouchEvent(ev: MotionEvent): Boolean {
        val xy = screenToWorld(ev.x, ev.y)
        val x = xy[0]
        val y = xy[1]
        val p = samplePressure(ev)

        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                rawPoints.clear()
                rawPressures.clear()
                lastPressure = p
                lastLiveUpdateMs = 0L
                committedAnchorWorld.clear()
                committedAnchorPressures.clear()
                tailRawWorld.clear()
                tailRawPressures.clear()
                rawPoints.add(PointF(x, y))
                rawPressures.add(p)
                liveBegin(currentColor, currentType)
                ingestRawPointToTwoSegment(PointF(x, y), p)
                rebuildLivePreview(force = true)
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                rawPoints.add(PointF(x, y))
                rawPressures.add(p)
                ingestRawPointToTwoSegment(PointF(x, y), p)
                rebuildLivePreview(force = false)
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                rawPoints.add(PointF(x, y))
                rawPressures.add(p)
                ingestRawPointToTwoSegment(PointF(x, y), p, forceEndPoint = true)
                submitFinalStroke()
                liveEnd()
                rawPoints.clear()
                rawPressures.clear()
                lastLiveUpdateMs = 0L
                liveCount = 0
                committedAnchorWorld.clear()
                committedAnchorPressures.clear()
                tailRawWorld.clear()
                tailRawPressures.clear()
                return true
            }
            MotionEvent.ACTION_POINTER_DOWN, MotionEvent.ACTION_POINTER_UP -> {
                return true
            }
        }
        return true
    }

    /**
     * 将一个原始报点写入“两段模型”：
     * - 先按最小像素位移阈值做过滤（避免围绕同一点抖动堆点）
     * - 再维护 tail 的 K 点窗口：当 tail 超过 K 时，把最老点转移到 committed
     *
     * forceEndPoint：
     * - UP/CANCEL 时为 true：即使位移很小也会强制把末点更新为最后报点，保证末端对齐
     */
    private fun ingestRawPointToTwoSegment(pWorld: PointF, pressure: Float, forceEndPoint: Boolean = false) {
        val scale = scaleProvider().coerceAtLeast(1e-4f)
        val minDistWorld = (0.8f / scale).coerceAtLeast(1e-6f)
        val minDist2 = minDistWorld * minDistWorld

        if (tailRawWorld.isEmpty()) {
            tailRawWorld.add(PointF(pWorld.x, pWorld.y))
            tailRawPressures.add(pressure)
            return
        }

        val last = tailRawWorld.last()
        val dx = pWorld.x - last.x
        val dy = pWorld.y - last.y
        val d2 = dx * dx + dy * dy
        if (d2 < minDist2) {
            if (forceEndPoint || tailRawWorld.size == 1) {
                last.x = pWorld.x
                last.y = pWorld.y
                tailRawPressures[tailRawPressures.lastIndex] = pressure
            }
        } else {
            tailRawWorld.add(PointF(pWorld.x, pWorld.y))
            tailRawPressures.add(pressure)
        }

        val k = tailRollbackK.coerceIn(2, 64)
        while (tailRawWorld.size > k) {
            committedAnchorWorld.add(tailRawWorld[0])
            committedAnchorPressures.add(tailRawPressures[0])
            tailRawWorld.removeAt(0)
            tailRawPressures.removeAt(0)
        }
    }

    /**
     * 重建实时预览（MOVE 链路）。
     *
     * 流程：
     * 1) 合并 committed + stabilizeTail(tailRaw)
     * 2) 映射到“宽=1000”的基准空间（用于对齐真实业务流程验证）
     * 3) 在基准空间做一次贴点二次曲线采样（局部贴点，避免超调）
     * 4) 可选：再做一次三次贝塞尔拟合采样（验证业务链路）
     * 5) 映射回 world 并通过 JNI 原地更新 live stroke
     */
    private fun rebuildLivePreview(force: Boolean) {
        val now = SystemClock.uptimeMillis()
        if (!force && now - lastLiveUpdateMs < 16L) return
        lastLiveUpdateMs = now

        val scale = scaleProvider().coerceAtLeast(1e-4f)
        val viewSize = viewSizeProvider()
        val viewWidthPx = viewSize.first.coerceAtLeast(1)
        val anchors = ArrayList<PointF>(committedAnchorWorld.size + tailRawWorld.size + 2)
        val pressures = ArrayList<Float>(committedAnchorPressures.size + tailRawPressures.size + 2)

        anchors.addAll(committedAnchorWorld)
        pressures.addAll(committedAnchorPressures)
        val stabilizedTail = stabilizeTail(tailRawWorld, tailRawPressures, scale)
        anchors.addAll(stabilizedTail.first)
        pressures.addAll(stabilizedTail.second)

        if (anchors.isEmpty()) return
        val viewWorldWidth = (viewWidthPx.toFloat() / scale).coerceAtLeast(1e-3f)
        val toBase = 1000f / viewWorldWidth
        val baseScale = (scale / toBase).coerceAtLeast(1e-6f)
        val anchorsBase = ArrayList<PointF>(anchors.size)
        for (p in anchors) anchorsBase.add(PointF(p.x * toBase, p.y * toBase))
        val stepBase = computeDesiredStepWorld(
            anchors = anchorsBase,
            scale = baseScale,
            targetPoints = 1000,
            maxPointsCap = (maxPoints - 2).coerceAtLeast(8)
        )
        var countBase = resampleQuadSplineIntoBuffers(
            anchors = anchorsBase,
            anchorPressures = pressures,
            stepWorld = stepBase,
            outPoints = tmpPointsBuf,
            outPressures = tmpPressuresBuf,
            maxOutPoints = maxPoints
        )
        if (enableBusinessSecondBezierFit && countBase >= 2) {
            countBase = resampleCubicBezierSecondFitIntoBuffers(
                inPoints = tmpPointsBuf,
                inPressures = tmpPressuresBuf,
                inCount = countBase,
                stepWorld = stepBase,
                outPoints = livePointsBuf,
                outPressures = livePressuresBuf,
                maxOutPoints = maxPoints
            )
        } else {
            java.lang.System.arraycopy(tmpPointsBuf, 0, livePointsBuf, 0, countBase * 2)
            java.lang.System.arraycopy(tmpPressuresBuf, 0, livePressuresBuf, 0, countBase)
        }
        for (i in 0 until countBase) {
            livePointsBuf[i * 2] = livePointsBuf[i * 2] / toBase
            livePointsBuf[i * 2 + 1] = livePointsBuf[i * 2 + 1] / toBase
        }
        liveCount = countBase
        if (liveCount > 0) liveUpdate(livePointsBuf, livePressuresBuf, liveCount)
    }

    /**
     * 提交最终笔划（UP 链路）。
     *
     * 注意：为避免“预览一套算法、落笔又换一套算法”导致跳变，
     * UP 与 MOVE 复用相同的点构建、去噪与采样流程，只是最终用 jniSubmit 提交为正式笔划。
     */
    private fun submitFinalStroke() {
        val scale = scaleProvider().coerceAtLeast(1e-4f)
        val viewSize = viewSizeProvider()
        val viewWidthPx = viewSize.first.coerceAtLeast(1)
        val anchors = ArrayList<PointF>(committedAnchorWorld.size + tailRawWorld.size + 2)
        val pressures = ArrayList<Float>(committedAnchorPressures.size + tailRawPressures.size + 2)

        anchors.addAll(committedAnchorWorld)
        pressures.addAll(committedAnchorPressures)
        val stabilizedTail = stabilizeTail(tailRawWorld, tailRawPressures, scale)
        anchors.addAll(stabilizedTail.first)
        pressures.addAll(stabilizedTail.second)

        if (anchors.size < 2) return
        val viewWorldWidth = (viewWidthPx.toFloat() / scale).coerceAtLeast(1e-3f)
        val toBase = 1000f / viewWorldWidth
        val baseScale = (scale / toBase).coerceAtLeast(1e-6f)
        val anchorsBase = ArrayList<PointF>(anchors.size)
        for (p in anchors) anchorsBase.add(PointF(p.x * toBase, p.y * toBase))
        val stepBase = computeDesiredStepWorld(
            anchors = anchorsBase,
            scale = baseScale,
            targetPoints = 1000,
            maxPointsCap = null
        )
        var countBase = resampleQuadSplineIntoBuffers(
            anchors = anchorsBase,
            anchorPressures = pressures,
            stepWorld = stepBase,
            outPoints = tmpPointsBuf,
            outPressures = tmpPressuresBuf,
            maxOutPoints = maxPoints
        )
        if (countBase < 2) return
        if (enableBusinessSecondBezierFit) {
            countBase = resampleCubicBezierSecondFitIntoBuffers(
                inPoints = tmpPointsBuf,
                inPressures = tmpPressuresBuf,
                inCount = countBase,
                stepWorld = stepBase,
                outPoints = livePointsBuf,
                outPressures = livePressuresBuf,
                maxOutPoints = maxPoints
            )
        } else {
            java.lang.System.arraycopy(tmpPointsBuf, 0, livePointsBuf, 0, countBase * 2)
            java.lang.System.arraycopy(tmpPressuresBuf, 0, livePressuresBuf, 0, countBase)
        }
        for (i in 0 until countBase) {
            livePointsBuf[i * 2] = livePointsBuf[i * 2] / toBase
            livePointsBuf[i * 2 + 1] = livePointsBuf[i * 2 + 1] / toBase
        }
        submitPointsAsStrokes(
            points = livePointsBuf,
            pressures = livePressuresBuf,
            count = countBase,
            maxStrokePoints = maxPoints
        )
    }

    /**
     * 将一条“已生成好的点序列”拆分并提交为 1..N 条笔划。
     * - 主要用于：业务验证链路中二次拟合后得到的点序列
     * - 之所以可能拆分：native 每条笔划有最大点数上限
     */
    private fun submitPointsAsStrokes(points: FloatArray, pressures: FloatArray, count: Int, maxStrokePoints: Int) {
        if (count < 2) return
        val pts = ArrayList<Float>(maxStrokePoints * 2)
        val prs = ArrayList<Float>(maxStrokePoints)

        fun flush() {
            if (prs.size < 2) return
            jniSubmit(pts.toFloatArray(), prs.toFloatArray(), currentColor, currentType)
            val keepX = pts[pts.size - 2]
            val keepY = pts[pts.size - 1]
            val keepP = prs[prs.size - 1]
            pts.clear()
            prs.clear()
            pts.add(keepX)
            pts.add(keepY)
            prs.add(keepP)
        }

        for (i in 0 until count) {
            if (prs.size >= maxStrokePoints) flush()
            pts.add(points[i * 2])
            pts.add(points[i * 2 + 1])
            prs.add(pressures[i])
        }
        if (prs.size >= 2) jniSubmit(pts.toFloatArray(), prs.toFloatArray(), currentColor, currentType)
    }

    /**
     * 第二次拟合（业务链路验证用）：把一次采样后的点序列再拟合成“分段三次贝塞尔”，并按步长重采样输出。
     *
     * 说明：
     * - 输入/输出都在“基准空间”（通常是宽=1000）中
     * - 使用 Catmull-Rom -> cubic Bezier 的局部转换（每段只依赖邻近 4 个点）
     * - 步长通过导数长度自适应近似固定弧长采样
     * - 末点强制对齐输入末点，保证实时末端贴合
     */
    private fun resampleCubicBezierSecondFitIntoBuffers(
        inPoints: FloatArray,
        inPressures: FloatArray,
        inCount: Int,
        stepWorld: Float,
        outPoints: FloatArray,
        outPressures: FloatArray,
        maxOutPoints: Int
    ): Int {
        if (inCount < 2) return 0
        var outCount = 0
        fun push(x: Float, y: Float, pr: Float) {
            if (outCount >= maxOutPoints) return
            if (outCount > 0) {
                val lx = outPoints[(outCount - 1) * 2]
                val ly = outPoints[(outCount - 1) * 2 + 1]
                if (lx == x && ly == y) {
                    outPressures[outCount - 1] = pr
                    return
                }
            }
            outPoints[outCount * 2] = x
            outPoints[outCount * 2 + 1] = y
            outPressures[outCount] = pr
            outCount++
        }

        fun clampIndex(i: Int): Int = when {
            i < 0 -> 0
            i >= inCount -> inCount - 1
            else -> i
        }

        fun bezierPoint(t: Float, p0: PointF, p1: PointF, p2: PointF, p3: PointF): PointF {
            val u = 1f - t
            val tt = t * t
            val uu = u * u
            val uuu = uu * u
            val ttt = tt * t
            val x = uuu * p0.x + 3f * uu * t * p1.x + 3f * u * tt * p2.x + ttt * p3.x
            val y = uuu * p0.y + 3f * uu * t * p1.y + 3f * u * tt * p2.y + ttt * p3.y
            return PointF(x, y)
        }

        fun bezierTangent(t: Float, p0: PointF, p1: PointF, p2: PointF, p3: PointF): PointF {
            val u = 1f - t
            val tt = t * t
            val uu = u * u
            val x = -3f * uu * p0.x + 3f * (uu - 2f * u * t) * p1.x + 3f * (2f * u * t - tt) * p2.x + 3f * tt * p3.x
            val y = -3f * uu * p0.y + 3f * (uu - 2f * u * t) * p1.y + 3f * (2f * u * t - tt) * p2.y + 3f * tt * p3.y
            return PointF(x, y)
        }

        push(inPoints[0], inPoints[1], inPressures[0])

        for (i in 0 until inCount - 1) {
            val i0 = clampIndex(i - 1)
            val i1 = i
            val i2 = i + 1
            val i3 = clampIndex(i + 2)

            val pPrev = PointF(inPoints[i0 * 2], inPoints[i0 * 2 + 1])
            val p1w = PointF(inPoints[i1 * 2], inPoints[i1 * 2 + 1])
            val p2w = PointF(inPoints[i2 * 2], inPoints[i2 * 2 + 1])
            val pNext = PointF(inPoints[i3 * 2], inPoints[i3 * 2 + 1])

            val b0 = p1w
            val b1 = PointF(
                p1w.x + (p2w.x - pPrev.x) / 6f,
                p1w.y + (p2w.y - pPrev.y) / 6f
            )
            val b2 = PointF(
                p2w.x - (pNext.x - p1w.x) / 6f,
                p2w.y - (pNext.y - p1w.y) / 6f
            )
            val b3 = p2w

            val prA = inPressures[i1]
            val prB = inPressures[i2]
            var t = 0f
            while (t < 1f && outCount < maxOutPoints) {
                val tan = bezierTangent(t, b0, b1, b2, b3)
                val len = kotlin.math.hypot(tan.x.toDouble(), tan.y.toDouble()).toFloat()
                val dt = if (len < 1e-3f) 0.25f else (stepWorld / len).coerceAtMost(0.5f)
                val tt = (t + dt).coerceAtMost(1f)
                val p = bezierPoint(tt, b0, b1, b2, b3)
                val pr = prA + (prB - prA) * tt
                push(p.x, p.y, pr)
                t = tt
            }
        }

        val endX = inPoints[(inCount - 1) * 2]
        val endY = inPoints[(inCount - 1) * 2 + 1]
        val endPr = inPressures[inCount - 1]
        if (outCount == 0) {
            push(endX, endY, endPr)
        } else {
            val lx = outPoints[(outCount - 1) * 2]
            val ly = outPoints[(outCount - 1) * 2 + 1]
            if (lx != endX || ly != endY) {
                if (outCount < maxOutPoints) {
                    push(endX, endY, endPr)
                } else {
                    outPoints[(outCount - 1) * 2] = endX
                    outPoints[(outCount - 1) * 2 + 1] = endY
                    outPressures[outCount - 1] = endPr
                }
            } else {
                outPressures[outCount - 1] = endPr
            }
        }
        return outCount.coerceAtLeast(1)
    }

    private fun samplePressure(ev: MotionEvent): Float {
        val p = ev.pressure.coerceIn(0f, 1f)
        val size = ev.size.coerceIn(0f, 1f)
        val pseudo = (size * 1.6f).coerceIn(0f, 1f)
        val raw = maxOf(p, pseudo)
        val filtered = lastPressure * 0.85f + raw * 0.15f
        lastPressure = filtered
        return filtered.coerceIn(0.05f, 1f)
    }

    /**
     * 尾段稳定化（只作用于渲染锚点，不修改原始报点）：
     * 1) 角度 + 步长门控的弱平滑：只在“近似直线且步长小”的区域平滑，尽量保拐角
     * 2) 两轮轻度平滑（弱 3 点核）
     * 3) 再做一次距离过滤，避免平滑产生过密点
     *
     * 关键约束：
     * - 首尾点强制保持与原始 tail 首尾一致，保证段连接与末端对齐
     */
    private fun stabilizeTail(
        tailWorld: List<PointF>,
        tailPressures: List<Float>,
        scale: Float
    ): Pair<List<PointF>, List<Float>> {
        if (tailWorld.isEmpty()) return Pair(emptyList(), emptyList())
        if (tailWorld.size == 1) return Pair(listOf(PointF(tailWorld[0].x, tailWorld[0].y)), listOf(tailPressures[0]))

        val maxSegWorld = (8.0f / scale).coerceAtLeast(1e-6f)
        val maxSeg2 = maxSegWorld * maxSegWorld
        val cosThreshold = 0.95f
        val eps = 1e-6f

        var curPts = tailWorld.map { PointF(it.x, it.y) }.toMutableList()
        var curPrs = tailPressures.toMutableList()

        repeat(2) {
            val nextPts = curPts.map { PointF(it.x, it.y) }.toMutableList()
            val nextPrs = curPrs.toMutableList()
            for (i in 1 until curPts.size - 1) {
                if (i == 0 || i == curPts.lastIndex) continue
                val p0 = curPts[i - 1]
                val p1 = curPts[i]
                val p2 = curPts[i + 1]

                val ax = p1.x - p0.x
                val ay = p1.y - p0.y
                val bx = p2.x - p1.x
                val by = p2.y - p1.y
                val la2 = ax * ax + ay * ay
                val lb2 = bx * bx + by * by
                if (la2 < eps || lb2 < eps) continue
                if (kotlin.math.max(la2, lb2) > maxSeg2) continue

                val cos = ((ax * bx + ay * by) / kotlin.math.sqrt(la2 * lb2)).coerceIn(-1f, 1f)
                if (cos < cosThreshold) continue

                nextPts[i] = PointF(
                    (p0.x + 2f * p1.x + p2.x) * 0.25f,
                    (p0.y + 2f * p1.y + p2.y) * 0.25f
                )
                val pr0 = curPrs[i - 1]
                val pr1 = curPrs[i]
                val pr2 = curPrs[i + 1]
                nextPrs[i] = (pr0 + 2f * pr1 + pr2) * 0.25f
            }
            curPts = nextPts
            curPrs = nextPrs
        }

        curPts[0].x = tailWorld[0].x
        curPts[0].y = tailWorld[0].y
        curPrs[0] = tailPressures[0]
        curPts[curPts.lastIndex].x = tailWorld.last().x
        curPts[curPts.lastIndex].y = tailWorld.last().y
        curPrs[curPrs.lastIndex] = tailPressures.last()

        val minDistWorld = (0.8f / scale).coerceAtLeast(1e-6f)
        val minDist2 = minDistWorld * minDistWorld
        val outPts = ArrayList<PointF>(curPts.size)
        val outPrs = ArrayList<Float>(curPrs.size)
        outPts.add(curPts[0])
        outPrs.add(curPrs[0])
        for (i in 1 until curPts.size) {
            val last = outPts.last()
            val p = curPts[i]
            val dx = p.x - last.x
            val dy = p.y - last.y
            if (dx * dx + dy * dy >= minDist2) {
                outPts.add(p)
                outPrs.add(curPrs[i])
            } else if (i == curPts.lastIndex) {
                last.x = p.x
                last.y = p.y
                outPrs[outPrs.lastIndex] = curPrs[i]
            }
        }
        return Pair(outPts, outPrs)
    }

    /**
     * 根据锚点长度估算“固定像素步长”对应的 world 步长。
     * - 目标点数 targetPoints 用于把长笔划压到大致 1000 点量级，避免过密
     * - minStepPx/maxStepPx 用于限制步长范围，避免太密或太稀
     * - maxPointsCap 用于实时预览时的上限控制，避免超过缓冲区导致末端跳变
     */
    private fun computeDesiredStepWorld(
        anchors: List<PointF>,
        scale: Float,
        targetPoints: Int,
        maxPointsCap: Int?
    ): Float {
        if (anchors.size < 2) return (1.0f / scale).coerceAtLeast(1e-6f)
        var lengthWorld = 0f
        for (i in 1 until anchors.size) {
            val prevAnchor = anchors[i - 1]
            val curAnchor = anchors[i]
            val dx = curAnchor.x - prevAnchor.x
            val dy = curAnchor.y - prevAnchor.y
            lengthWorld += kotlin.math.sqrt(dx * dx + dy * dy)
        }
        val lengthScreen = lengthWorld * scale
        val minStepPx = 0.8f
        val maxStepPx = 12f
        var stepScreen = (lengthScreen / targetPoints.coerceAtLeast(1)).coerceIn(minStepPx, maxStepPx)
        if (maxPointsCap != null && lengthScreen > 1e-6f) {
            val estimated = (lengthScreen / stepScreen).toInt() + 2
            if (estimated > maxPointsCap) {
                stepScreen = kotlin.math.max(stepScreen, lengthScreen / (maxPointsCap - 1).toFloat())
            }
        }
        return (stepScreen / scale).coerceAtLeast(1e-6f)
    }

    private data class QuadSeg(
        val p0: PointF,
        val p1: PointF,
        val p2: PointF,
        val pr0: Float,
        val pr1: Float,
        val pr2: Float
    )

    /**
     * 构建“贴点”的分段二次曲线：
     *
     * 目标：
     * - 尽量“贴着用户报点走”，避免全局拟合带来的外扩/超调（overshoot）
     * - 让曲线连续且观感自然，同时保留拐角与节奏
     *
     * 构建方式（Quadratic Bezier，二次贝塞尔）：
     * - 对于内点 i（1..n-2）：
     *   - 取相邻锚点的中点 m(i-1)=mid(anchor(i-1), anchor(i))
     *   - 取相邻锚点的中点 m(i)=mid(anchor(i), anchor(i+1))
     *   - 构造一段二次曲线：p0=m(i-1), p1=anchor(i), p2=m(i)
     *   - 这段曲线通过 anchor(i)（作为控制点）“牵引”形状，但端点落在中点上，
     *     能把每个锚点的影响限制在局部，避免远处点改变整体形状。
     *
     * 端点处理（端点切线控制点）：
     * - 端点没有完整的前后邻域，直接套用中点公式容易出现端点“发散/扁折”
     * - 因此首段/尾段采用“末端切线方向”构造控制点：
     *   - 首段：p0=anchor(0), p2=mid(0,1)，p1 沿 (anchor(1)-anchor(0)) 方向推进一小段
     *   - 尾段：p0=mid(n-2,n-1), p2=anchor(n-1)，p1 沿 (anchor(n-1)-anchor(n-2)) 方向回退一小段
     * - handle 长度做了夹紧，避免控制点过远导致端段曲率过大。
     *
     * sanitized：
     * - 若某段控制点过近导致退化（导数趋近 0）：
     *   - 会导致采样时 |B'(t)| 很小，从而 dt 过大，出现跨段直线/跳点
     *   - 因此把控制点回退为弦中点（退化为更稳定的“对称二次段”）
     */
    private fun buildQuadSplineSegments(anchors: List<PointF>, prs: List<Float>): List<QuadSeg> {
        val n = anchors.size
        if (n < 2) return emptyList()
        if (n == 2) {
            val startAnchor = anchors[0]
            val endAnchor = anchors[1]
            val midPoint = PointF((startAnchor.x + endAnchor.x) * 0.5f, (startAnchor.y + endAnchor.y) * 0.5f)
            val midPressure = (prs[0] + prs[1]) * 0.5f
            return listOf(QuadSeg(startAnchor, midPoint, endAnchor, prs[0], midPressure, prs[1]))
        }

        val mids = ArrayList<PointF>(n - 1)
        val midPrs = ArrayList<Float>(n - 1)
        for (i in 0 until n - 1) {
            val leftAnchor = anchors[i]
            val rightAnchor = anchors[i + 1]
            mids.add(PointF((leftAnchor.x + rightAnchor.x) * 0.5f, (leftAnchor.y + rightAnchor.y) * 0.5f))
            midPrs.add((prs[i] + prs[i + 1]) * 0.5f)
        }

        fun safeNormalize(dx: Float, dy: Float): Pair<Float, Float> {
            val len = kotlin.math.sqrt(dx * dx + dy * dy)
            if (len < 1e-6f) return Pair(0f, 0f)
            return Pair(dx / len, dy / len)
        }

        val out = ArrayList<QuadSeg>(n)

        run {
            val startAnchor = anchors[0]
            val secondAnchor = anchors[1]
            val startTangentDir = safeNormalize(secondAnchor.x - startAnchor.x, secondAnchor.y - startAnchor.y)
            val startHandleLen = kotlin.math.min(
                distance(startAnchor, secondAnchor) * 0.25f,
                distance(startAnchor, mids[0]) * 0.9f
            )
            // 首段端点切线控制点：
            // - 以 (second-start) 方向作为端点切线，构造控制点
            // - handle 做夹紧：既避免控制点过远导致端点“甩尾”，也避免过近导致退化
            val startControl = PointF(
                startAnchor.x + startTangentDir.first * startHandleLen,
                startAnchor.y + startTangentDir.second * startHandleLen
            )
            out.add(QuadSeg(startAnchor, startControl, mids[0], prs[0], prs[0], midPrs[0]))
        }

        for (i in 1 until n - 1) {
            // 中间段贴点二次曲线：
            // - p0/p2 使用相邻中点，能保证段与段之间在中点处连续衔接
            // - p1 直接使用真实锚点，让轨迹尽量贴近用户输入
            out.add(QuadSeg(mids[i - 1], anchors[i], mids[i], midPrs[i - 1], prs[i], midPrs[i]))
        }

        run {
            val preEndAnchor = anchors[n - 2]
            val endAnchor = anchors[n - 1]
            val endTangentDir = safeNormalize(endAnchor.x - preEndAnchor.x, endAnchor.y - preEndAnchor.y)
            val endHandleLen = kotlin.math.min(
                distance(preEndAnchor, endAnchor) * 0.25f,
                distance(mids[n - 2], endAnchor) * 0.9f
            )
            // 尾段端点切线控制点：
            // - 以 (end-preEnd) 方向作为端点切线，构造控制点（从 end 向回退）
            // - 这样尾端在实时阶段即便缺少“下一个点”，也能保持更自然的末端走向
            val endControl = PointF(
                endAnchor.x - endTangentDir.first * endHandleLen,
                endAnchor.y - endTangentDir.second * endHandleLen
            )
            out.add(QuadSeg(mids[n - 2], endControl, endAnchor, midPrs[n - 2], prs[n - 1], prs[n - 1]))
        }

        val sanitized = ArrayList<QuadSeg>(out.size)
        for (seg in out) {
            val chord = distance(seg.p0, seg.p2)
            val d01 = distance(seg.p0, seg.p1)
            // 退化检测：
            // - 若控制点 seg.p1 极接近 seg.p0（相对弦长过小），该段导数在起点附近可能趋近 0
            // - 采样时 dt ≈ step/|B'(t)| 会被放大，导致跨过大量参数区间，出现“直线拉过去”的伪影
            if (chord > 1e-3f && d01 < chord * 0.02f) {
                val midControl = PointF(
                    seg.p0.x + (seg.p2.x - seg.p0.x) * 0.5f,
                    seg.p0.y + (seg.p2.y - seg.p0.y) * 0.5f
                )
                // 退化修正：
                // - 将控制点回退到弦中点，使段形状更稳定，避免导数退化
                sanitized.add(QuadSeg(seg.p0, midControl, seg.p2, seg.pr0, seg.pr1, seg.pr2))
            } else {
                sanitized.add(seg)
            }
        }
        return sanitized
    }

    /**
     * 将二次曲线按近似固定 world 步长重采样到固定数组缓冲中（用于实时预览）。
     *
     * 固定步长采样要解决的问题：
     * - 直接用固定 dt（均匀参数采样）会导致：曲率大处点很密、曲率小处点很稀，线条粗细与速度观感不稳定
     * - 我们希望“近似固定弧长采样”：相邻输出点在屏幕上间距更均匀
     *
     * 导数自适应 dt（近似弧长参数化）：
     * - 二次贝塞尔 B(t) 的导数 B'(t) 描述了参数变化对应的“速度”
     * - 用 dt ≈ stepWorld / |B'(t)| 让每步前进的距离更接近 stepWorld
     *
     * 导数退化兜底（弦长估算）：
     * - 当 |B'(t)| 很小（例如控制点退化、段极短）时，上式会产生极大的 dt
     * - 为避免跨段直线/跳点，改用弦长 chord≈|p0-p2| 估算 dt，并把 dt 夹紧到合理范围
     *
     * 末点强制对齐：
     * - 无论采样过程如何，最后一个点必须落在 anchors.last()
     * - 这是“实时预览必须画到真实末端报点”的关键约束
     */
    private fun resampleQuadSplineIntoBuffers(
        anchors: List<PointF>,
        anchorPressures: List<Float>,
        stepWorld: Float,
        outPoints: FloatArray,
        outPressures: FloatArray,
        maxOutPoints: Int
    ): Int {
        val segs = buildQuadSplineSegments(anchors, anchorPressures)
        if (segs.isEmpty()) {
            outPoints[0] = anchors[0].x
            outPoints[1] = anchors[0].y
            outPressures[0] = anchorPressures[0]
            return 1
        }

        var outCount = 0
        fun push(x: Float, y: Float, pr: Float) {
            if (outCount >= maxOutPoints) return
            if (outCount > 0) {
                val lx = outPoints[(outCount - 1) * 2]
                val ly = outPoints[(outCount - 1) * 2 + 1]
                if (lx == x && ly == y) {
                    outPressures[outCount - 1] = pr
                    return
                }
            }
            val base = outCount * 2
            outPoints[base] = x
            outPoints[base + 1] = y
            outPressures[outCount] = pr
            outCount++
        }

        push(anchors[0].x, anchors[0].y, anchorPressures[0])

        fun bezier(t: Float, p0: PointF, p1: PointF, p2: PointF): PointF {
            val u = 1f - t
            val tt = t * t
            val uu = u * u
            return PointF(
                uu * p0.x + 2f * u * t * p1.x + tt * p2.x,
                uu * p0.y + 2f * u * t * p1.y + tt * p2.y
            )
        }

        fun bezierDeriv(t: Float, p0: PointF, p1: PointF, p2: PointF): Pair<Float, Float> {
            val u = 1f - t
            val dx = 2f * u * (p1.x - p0.x) + 2f * t * (p2.x - p1.x)
            val dy = 2f * u * (p1.y - p0.y) + 2f * t * (p2.y - p1.y)
            return Pair(dx, dy)
        }

        fun prBlend(t: Float, pr0: Float, pr1: Float, pr2: Float): Float {
            val u = 1f - t
            val tt = t * t
            val uu = u * u
            return uu * pr0 + 2f * u * t * pr1 + tt * pr2
        }

        val eps = 1e-6f
        for (seg in segs) {
            var t = 0f
            while (t < 1f && outCount < maxOutPoints) {
                val d = bezierDeriv(t, seg.p0, seg.p1, seg.p2)
                val len = kotlin.math.hypot(d.first.toDouble(), d.second.toDouble()).toFloat()
                val dt = if (len < eps) {
                    // 导数退化兜底：
                    // - 用弦长估算参数推进幅度，并夹紧到 [0.02, 0.25]
                    // - 目的：避免 dt 过大直接跨过整段造成“直线拉过去”
                    val chord = distance(seg.p0, seg.p2).coerceAtLeast(1e-6f)
                    (stepWorld / chord).coerceIn(0.02f, 0.25f)
                } else {
                    // 导数自适应 dt：
                    // - 近似保证每次前进的距离接近 stepWorld
                    // - dt 做上限 0.5，避免极端情况下 t 一步跳到末端
                    (stepWorld / len).coerceAtMost(0.5f)
                }
                val tt = (t + dt).coerceAtMost(1f)
                val p = bezier(tt, seg.p0, seg.p1, seg.p2)
                val pr = prBlend(tt, seg.pr0, seg.pr1, seg.pr2)
                push(p.x, p.y, pr)
                t = tt
            }
        }

        val end = anchors.last()
        val endPr = anchorPressures.last()
        // 末点强制对齐真实锚点末端：
        // - 确保实时预览/最终提交“落到用户最后报点”，避免末端悬空导致的时延感
        // - 若缓冲已满，则覆盖最后一个点，保证末端一定正确
        if (outCount == 0) {
            push(end.x, end.y, endPr)
        } else {
            val lx = outPoints[(outCount - 1) * 2]
            val ly = outPoints[(outCount - 1) * 2 + 1]
            if (lx != end.x || ly != end.y) {
                if (outCount < maxOutPoints) {
                    push(end.x, end.y, endPr)
                } else {
                    outPoints[(outCount - 1) * 2] = end.x
                    outPoints[(outCount - 1) * 2 + 1] = end.y
                    outPressures[outCount - 1] = endPr
                }
            } else {
                outPressures[outCount - 1] = endPr
            }
        }
        return outCount.coerceAtLeast(1)
    }

    private fun submitResampledQuadSplineAsStrokes(
        anchors: List<PointF>,
        anchorPressures: List<Float>,
        stepWorld: Float,
        maxStrokePoints: Int
    ) {
        val segs = buildQuadSplineSegments(anchors, anchorPressures)
        if (segs.isEmpty()) return

        val pts = ArrayList<Float>(maxStrokePoints * 2)
        val prs = ArrayList<Float>(maxStrokePoints)

        fun flush() {
            if (prs.size < 2) return
            jniSubmit(pts.toFloatArray(), prs.toFloatArray(), currentColor, currentType)
            val keepX = pts[pts.size - 2]
            val keepY = pts[pts.size - 1]
            val keepP = prs[prs.size - 1]
            pts.clear()
            prs.clear()
            pts.add(keepX)
            pts.add(keepY)
            prs.add(keepP)
        }

        fun push(x: Float, y: Float, pr: Float) {
            if (prs.size >= maxStrokePoints) flush()
            pts.add(x)
            pts.add(y)
            prs.add(pr)
        }

        push(anchors[0].x, anchors[0].y, anchorPressures[0])

        fun bezier(t: Float, p0: PointF, p1: PointF, p2: PointF): PointF {
            val u = 1f - t
            val tt = t * t
            val uu = u * u
            return PointF(
                uu * p0.x + 2f * u * t * p1.x + tt * p2.x,
                uu * p0.y + 2f * u * t * p1.y + tt * p2.y
            )
        }

        fun bezierDeriv(t: Float, p0: PointF, p1: PointF, p2: PointF): Pair<Float, Float> {
            val u = 1f - t
            val dx = 2f * u * (p1.x - p0.x) + 2f * t * (p2.x - p1.x)
            val dy = 2f * u * (p1.y - p0.y) + 2f * t * (p2.y - p1.y)
            return Pair(dx, dy)
        }

        fun prBlend(t: Float, pr0: Float, pr1: Float, pr2: Float): Float {
            val u = 1f - t
            val tt = t * t
            val uu = u * u
            return uu * pr0 + 2f * u * t * pr1 + tt * pr2
        }

        val eps = 1e-6f
        for (seg in segs) {
            var t = 0f
            while (t < 1f) {
                val d = bezierDeriv(t, seg.p0, seg.p1, seg.p2)
                val len = kotlin.math.hypot(d.first.toDouble(), d.second.toDouble()).toFloat()
                val dt = if (len < eps) {
                    val chord = distance(seg.p0, seg.p2).coerceAtLeast(1e-6f)
                    (stepWorld / chord).coerceIn(0.02f, 0.25f)
                } else {
                    (stepWorld / len).coerceAtMost(0.5f)
                }
                val tt = (t + dt).coerceAtMost(1f)
                val p = bezier(tt, seg.p0, seg.p1, seg.p2)
                val pr = prBlend(tt, seg.pr0, seg.pr1, seg.pr2)
                push(p.x, p.y, pr)
                t = tt
            }
        }

        val end = anchors.last()
        val endPr = anchorPressures.last()
        if (prs.isNotEmpty()) {
            val lx = pts[pts.size - 2]
            val ly = pts[pts.size - 1]
            if (lx != end.x || ly != end.y) {
                push(end.x, end.y, endPr)
            } else {
                prs[prs.lastIndex] = endPr
            }
        }
        if (prs.size >= 2) {
            jniSubmit(pts.toFloatArray(), prs.toFloatArray(), currentColor, currentType)
        }
    }

    /**
     * 备用：直接对 rawPoints 做 Catmull-Rom -> 三次贝塞尔拟合并重采样。
     * - 当前实时主链路优先使用“贴点分段二次曲线”，以减少拟合超调与细节损失
     * - 该函数保留用于对比/调参/回归验证
     */
    private fun buildStrokePointsBezier(): Pair<FloatArray, FloatArray> {
        val n = rawPoints.size
        if (n < 2) return Pair(FloatArray(0), FloatArray(0))

        val pts = rawPoints
        val prs = rawPressures
        val scale = scaleProvider().coerceAtLeast(1e-4f)
        val stepPx = 1.0f
        val outPts = ArrayList<Float>(n * 2)
        val outP = ArrayList<Float>(n)

        fun clampIndex(i: Int): Int = when {
            i < 0 -> 0
            i >= n -> n - 1
            else -> i
        }

        fun bezierPoint(t: Float, p0: PointF, p1: PointF, p2: PointF, p3: PointF): PointF {
            val u = 1f - t
            val tt = t * t
            val uu = u * u
            val uuu = uu * u
            val ttt = tt * t
            val x = uuu * p0.x + 3f * uu * t * p1.x + 3f * u * tt * p2.x + ttt * p3.x
            val y = uuu * p0.y + 3f * uu * t * p1.y + 3f * u * tt * p2.y + ttt * p3.y
            return PointF(x, y)
        }

        fun bezierTangent(t: Float, p0: PointF, p1: PointF, p2: PointF, p3: PointF): PointF {
            val u = 1f - t
            val tt = t * t
            val uu = u * u
            val x = -3f * uu * p0.x + 3f * (uu - 2f * u * t) * p1.x + 3f * (2f * u * t - tt) * p2.x + 3f * tt * p3.x
            val y = -3f * uu * p0.y + 3f * (uu - 2f * u * t) * p1.y + 3f * (2f * u * t - tt) * p2.y + 3f * tt * p3.y
            return PointF(x, y)
        }



        for (i in 0 until n - 1) {
            val p0 = pts[clampIndex(i - 1)]
            val p1 = pts[i]
            val p2 = pts[i + 1]
            val p3 = pts[clampIndex(i + 2)]
            // Catmull-Rom 转贝塞尔控制点（张力=0.5近似，因子1/6）
            val b0 = p1
            val b1 = PointF(
                p1.x + (p2.x - p0.x) / 6f,
                p1.y + (p2.y - p0.y) / 6f
            )
            val b2 = PointF(
                p2.x - (p3.x - p1.x) / 6f,
                p2.y - (p3.y - p1.y) / 6f
            )
            val b3 = p2

            var t = 0f
            val pA = prs[i]
            val pB = prs[i + 1]
            // 以导数长度近似像素步长：dt ≈ stepPx / |B'(t)|
            while (t < 1f) {
                val tan = bezierTangent(t, b0, b1, b2, b3)
                val len = kotlin.math.hypot(tan.x.toDouble(), tan.y.toDouble()).toFloat()
                val dt = if (len < 1e-3f) 0.25f else (stepPx / len).coerceAtMost(0.5f)
                val tt = (t + dt).coerceAtMost(1f)
                val p = bezierPoint(tt, b0, b1, b2, b3)
                val pr = pA + (pB - pA) * tt
                outPts.add(p.x)
                outPts.add(p.y)
                outP.add(pr)
                if (outP.size >= 1024) break
                t = tt
            }
            if (outP.size >= 1024) break
        }

        if (outPts.isEmpty()) {
            // 后备：若拟合失败则直接使用原始末点
            val last = pts.last()
            outPts.add(last.x)
            outPts.add(last.y)
            outP.add(prs.last())
        }

        return Pair(outPts.toFloatArray(), outP.toFloatArray())
    }

    /**
     * 固定像素步长的贝塞尔重采样：目标每条线约 targetPoints 个点。
     * 算法：
     * 1) 使用 Catmull-Rom 拟合每四点形成的一段三次贝塞尔；
     * 2) 通过增量累加长度，按固定像素步长采样（近似弧长参数化）；
     * 3) 压力按参数 t 线性插值；
     */
    private fun buildStrokeSegmentsBezierFixedStep(
        targetPoints: Int = 1000,
        minStepPx: Float = 0.8f,
        maxStepPx: Float = 12f,
        useLengthBasedStep: Boolean = true
    ): List<Pair<FloatArray, FloatArray>> {
        if (rawPoints.size < 2) return emptyList()

        val scale = scaleProvider().coerceAtLeast(1e-4f)

        var length = 0f
        for (i in 1 until rawPoints.size) {
            val prevPoint = rawPoints[i - 1]
            val curPoint = rawPoints[i]
            val dx = curPoint.x - prevPoint.x
            val dy = curPoint.y - prevPoint.y
            length += kotlin.math.sqrt(dx * dx + dy * dy)
        }
        val lengthScreen = length * scale
        var desiredStepScreen = if (useLengthBasedStep) {
            (lengthScreen / targetPoints.coerceAtLeast(1)).coerceIn(minStepPx, maxStepPx)
        } else {
            2.0f.coerceIn(minStepPx, maxStepPx)
        }
        val maxSegments = 8
        val maxTotalPoints = maxPoints * maxSegments
        if (lengthScreen > 1e-6f) {
            val estimated = (lengthScreen / desiredStepScreen).toInt() + 2
            if (estimated > maxTotalPoints) {
                desiredStepScreen = maxOf(desiredStepScreen, lengthScreen / (maxTotalPoints - 1).toFloat())
            }
        }
        val desiredStep = (desiredStepScreen / scale).coerceAtLeast(1e-6f)

        val segments = ArrayList<Pair<FloatArray, FloatArray>>(2)
        val outPts = ArrayList<Float>(maxPoints * 2)
        val outPrs = ArrayList<Float>(maxPoints)

        // 为了边界稳定，复制端点以构造 Catmull-Rom 端段
        val pts = ArrayList<PointF>()
        val prs = ArrayList<Float>()
        pts.add(rawPoints.first())
        prs.add(rawPressures.first())
        pts.addAll(rawPoints)
        prs.addAll(rawPressures)
        pts.add(rawPoints.last())
        prs.add(rawPressures.last())

        var lastX = pts[1].x
        var lastY = pts[1].y
        var lastP = prs[1]
        outPts.add(lastX)
        outPts.add(lastY)
        outPrs.add(lastP)

        fun flushSegment() {
            if (outPrs.size < 2) return
            segments.add(Pair(outPts.toFloatArray(), outPrs.toFloatArray()))
            val keepX = outPts[outPts.size - 2]
            val keepY = outPts[outPts.size - 1]
            val keepP = outPrs[outPrs.size - 1]
            outPts.clear()
            outPrs.clear()
            outPts.add(keepX)
            outPts.add(keepY)
            outPrs.add(keepP)
        }

        var accLen = 0f
        for (i in 0 until pts.size - 3) {
            val p0 = pts[i]
            val p1 = pts[i + 1]
            val p2 = pts[i + 2]
            val p3 = pts[i + 3]
            val pr1 = prs[i + 1]
            val pr2 = prs[i + 2]

            // Catmull-Rom -> Bezier 控制点映射
            val b0 = p1
            val b1 = PointF(p1.x + (p2.x - p0.x) / 6f, p1.y + (p2.y - p0.y) / 6f)
            val b2 = PointF(p2.x - (p3.x - p1.x) / 6f, p2.y - (p3.y - p1.y) / 6f)
            val b3 = p2

            var t = 0f
            val dt = 1f / 64f // 参数步长用于近似长度积分
            while (t <= 1f) {
                val u = 1f - t
                val tt = t * t
                val uu = u * u
                val pX = uu * u * b0.x + 3f * uu * t * b1.x + 3f * u * tt * b2.x + tt * t * b3.x
                val pY = uu * u * b0.y + 3f * uu * t * b1.y + 3f * u * tt * b2.y + tt * t * b3.y

                val dx = pX - lastX
                val dy = pY - lastY
                val dl = kotlin.math.sqrt(dx * dx + dy * dy)
                accLen += dl

                if (accLen >= desiredStep) {
                    val pInterp = pr1 + (pr2 - pr1) * t

                    outPts.add(pX)
                    outPts.add(pY)
                    outPrs.add(pInterp)
                    if (outPrs.size >= maxPoints) flushSegment()

                    lastX = pX
                    lastY = pY
                    lastP = pInterp
                    accLen = 0f
                }

                t += dt
            }
        }

        val end = rawPoints.last()
        val endP = rawPressures.last()
        if (outPrs.isNotEmpty() && (outPts[outPts.size - 2] != end.x || outPts[outPts.size - 1] != end.y)) {
            if (outPrs.size >= maxPoints) flushSegment()
            outPts.add(end.x)
            outPts.add(end.y)
            outPrs.add(endP)
        }
        if (outPrs.size >= 2) {
            segments.add(Pair(outPts.toFloatArray(), outPrs.toFloatArray()))
        }
        return segments
    }

    private fun distance(a: PointF, b: PointF): Float {
        val dx = a.x - b.x
        val dy = a.y - b.y
        return kotlin.math.sqrt(dx * dx + dy * dy)
    }
}
