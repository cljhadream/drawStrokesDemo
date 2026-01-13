package com.example.myapplication

import android.graphics.PointF
import android.os.SystemClock
import android.view.MotionEvent

/**
 * 触摸输入采集与笔划点生成：
 * - 采集单指触控的(x, y, pressure)
 * - 使用简化的平滑与重采样生成绘制点
 * - 在抬起时通过JNI传递到原生层
 */
class StrokeInputProcessor(
    private val screenToWorld: (x: Float, y: Float) -> FloatArray,
    private val scaleProvider: () -> Float,
    private val jniSubmit: (points: FloatArray, pressures: FloatArray, color: FloatArray) -> Unit,
    private val liveBegin: (color: FloatArray) -> Unit,
    private val liveUpdate: (points: FloatArray, pressures: FloatArray, count: Int) -> Unit,
    private val liveEnd: () -> Unit,
) {
    private val maxPoints = 1024
    private val rawPoints = mutableListOf<PointF>()
    private val rawPressures = mutableListOf<Float>()
    private var lastPressure = 0.5f
    private var lastLiveUpdateMs = 0L
    private val livePointsBuf = FloatArray(maxPoints * 2)
    private val livePressuresBuf = FloatArray(maxPoints)
    private var liveCount = 0
    private var lastSampleX = 0f
    private var lastSampleY = 0f
    private var lastSampleP = 0f
    private var smoothX = 0f
    private var smoothY = 0f
    private var hasSmooth = false
    private var committedDuringGesture = false

    // 当前笔划颜色（RGBA），可在外部动态修改
    var currentColor: FloatArray = floatArrayOf(0.1f, 0.4f, 1.0f, 0.85f)

    fun cancelStroke() {
        rawPoints.clear()
        rawPressures.clear()
        lastLiveUpdateMs = 0L
        liveCount = 0
        hasSmooth = false
        committedDuringGesture = false
        liveEnd()
    }

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
                committedDuringGesture = false
                rawPoints.add(PointF(x, y))
                rawPressures.add(p)
                liveBegin(currentColor)
                beginLiveBuffers(x, y, p)
                liveUpdate(livePointsBuf, livePressuresBuf, liveCount)
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                rawPoints.add(PointF(x, y))
                rawPressures.add(p)
                updateLivePreview()
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                rawPoints.add(PointF(x, y))
                rawPressures.add(p)
                appendLiveSample(x, y, p, forceEndPoint = true)
                if (!committedDuringGesture) {
                    val segments = buildStrokeSegmentsBezierFixedStep(targetPoints = 1000)
                    if (segments.isNotEmpty()) {
                        for (seg in segments) {
                            jniSubmit(seg.first, seg.second, currentColor)
                        }
                    } else {
                        submitLiveSegmentIfNeeded(force = true)
                    }
                } else {
                    submitLiveSegmentIfNeeded(force = true)
                }
                liveEnd()
                rawPoints.clear()
                rawPressures.clear()
                lastLiveUpdateMs = 0L
                liveCount = 0
                hasSmooth = false
                committedDuringGesture = false
                return true
            }
            MotionEvent.ACTION_POINTER_DOWN, MotionEvent.ACTION_POINTER_UP -> {
                return true
            }
        }
        return true
    }

    private fun beginLiveBuffers(x: Float, y: Float, p: Float) {
        liveCount = 1
        livePointsBuf[0] = x
        livePointsBuf[1] = y
        livePressuresBuf[0] = p
        lastSampleX = x
        lastSampleY = y
        lastSampleP = p
        smoothX = x
        smoothY = y
        hasSmooth = true
    }

    private fun updateLivePreview() {
        val now = SystemClock.uptimeMillis()
        if (now - lastLiveUpdateMs < 16L) return
        lastLiveUpdateMs = now

        if (rawPoints.isEmpty() || rawPressures.isEmpty()) return
        val pt = rawPoints.last()
        val pr = rawPressures.last()
        appendLiveSample(pt.x, pt.y, pr, forceEndPoint = false)
        submitLiveSegmentIfNeeded(force = false)
        if (liveCount > 0) {
            liveUpdate(livePointsBuf, livePressuresBuf, liveCount)
        }
    }

    private fun appendLiveSample(x: Float, y: Float, p: Float, forceEndPoint: Boolean) {
        if (!hasSmooth) {
            beginLiveBuffers(x, y, p)
            return
        }

        val k = 0.35f
        smoothX += (x - smoothX) * k
        smoothY += (y - smoothY) * k

        val scale = scaleProvider().coerceAtLeast(1e-4f)
        val stepScreen = 1.0f
        val stepWorld = (stepScreen / scale).coerceAtLeast(1e-4f)

        var tx = smoothX
        var ty = smoothY
        var tp = p

        var dx = tx - lastSampleX
        var dy = ty - lastSampleY
        var dist = kotlin.math.sqrt(dx * dx + dy * dy)

        var safety = 0
        while (dist >= stepWorld && safety < 64) {
            val t = stepWorld / dist
            val nx = lastSampleX + dx * t
            val ny = lastSampleY + dy * t
            val np = lastSampleP + (tp - lastSampleP) * t
            pushLivePoint(nx, ny, np)
            lastSampleX = nx
            lastSampleY = ny
            lastSampleP = np
            dx = tx - lastSampleX
            dy = ty - lastSampleY
            dist = kotlin.math.sqrt(dx * dx + dy * dy)
            safety++
        }

        if (forceEndPoint) {
            if (liveCount == 0 || livePointsBuf[(liveCount - 1) * 2] != x || livePointsBuf[(liveCount - 1) * 2 + 1] != y) {
                pushLivePoint(x, y, p)
                lastSampleX = x
                lastSampleY = y
                lastSampleP = p
                smoothX = x
                smoothY = y
            }
        }
    }

    private fun pushLivePoint(x: Float, y: Float, p: Float) {
        if (liveCount >= maxPoints) {
            submitLiveSegmentIfNeeded(force = true)
        }
        if (liveCount >= maxPoints) return
        val base = liveCount * 2
        livePointsBuf[base] = x
        livePointsBuf[base + 1] = y
        livePressuresBuf[liveCount] = p
        liveCount++
    }

    private fun submitLiveSegmentIfNeeded(force: Boolean) {
        if (liveCount < 2) return
        if (!force && liveCount < maxPoints) return

        val pts = FloatArray(liveCount * 2)
        val prs = FloatArray(liveCount)
        System.arraycopy(livePointsBuf, 0, pts, 0, liveCount * 2)
        System.arraycopy(livePressuresBuf, 0, prs, 0, liveCount)
        jniSubmit(pts, prs, currentColor)
        committedDuringGesture = true

        val keepX = livePointsBuf[(liveCount - 1) * 2]
        val keepY = livePointsBuf[(liveCount - 1) * 2 + 1]
        val keepP = livePressuresBuf[liveCount - 1]
        liveCount = 1
        livePointsBuf[0] = keepX
        livePointsBuf[1] = keepY
        livePressuresBuf[0] = keepP
        lastSampleX = keepX
        lastSampleY = keepY
        lastSampleP = keepP
        smoothX = keepX
        smoothY = keepY
        hasSmooth = true
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
     * 将原始点平滑并按固定间隔重采样，得到绘制点与对应压力。
     * 简化策略：
     * - 三点滑动平均平滑
     * - 固定步长（像素）重采样
     */
    /**
     * 贝塞尔拟合与固定像素步长采样：
     * - 使用Catmull-Rom转换为分段三次贝塞尔，生成平滑路径
     * - 使用导数长度自适应的参数步长，近似固定像素间隔采样
     * - 压力按相邻顶点线性插值
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
            val a = rawPoints[i - 1]
            val b = rawPoints[i]
            val dx = b.x - a.x
            val dy = b.y - a.y
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
