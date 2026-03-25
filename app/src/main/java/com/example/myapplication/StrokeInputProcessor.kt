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
    private val jniSubmit: (points: FloatArray, pressures: FloatArray, color: FloatArray, type: Int) -> Unit,
    private val liveBegin: (color: FloatArray, type: Int) -> Unit,
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

    var tailRollbackK: Int = 12

    private val committedAnchorWorld = ArrayList<PointF>(2048)
    private val committedAnchorPressures = ArrayList<Float>(2048)
    private val tailRawWorld = ArrayList<PointF>(64)
    private val tailRawPressures = ArrayList<Float>(64)

    // 当前笔划颜色（RGBA），可在外部动态修改
    var currentColor: FloatArray = floatArrayOf(0.1f, 0.4f, 1.0f, 0.85f)
    var currentType: Int = 0

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

    private fun rebuildLivePreview(force: Boolean) {
        val now = SystemClock.uptimeMillis()
        if (!force && now - lastLiveUpdateMs < 16L) return
        lastLiveUpdateMs = now

        val scale = scaleProvider().coerceAtLeast(1e-4f)
        val anchors = ArrayList<PointF>(committedAnchorWorld.size + tailRawWorld.size + 2)
        val pressures = ArrayList<Float>(committedAnchorPressures.size + tailRawPressures.size + 2)

        anchors.addAll(committedAnchorWorld)
        pressures.addAll(committedAnchorPressures)
        val stabilizedTail = stabilizeTail(tailRawWorld, tailRawPressures, scale)
        anchors.addAll(stabilizedTail.first)
        pressures.addAll(stabilizedTail.second)

        if (anchors.isEmpty()) return
        val stepWorld = computeDesiredStepWorld(
            anchors = anchors,
            scale = scale,
            targetPoints = 1000,
            maxPointsCap = (maxPoints - 2).coerceAtLeast(8)
        )
        liveCount = resampleQuadSplineIntoBuffers(
            anchors = anchors,
            anchorPressures = pressures,
            stepWorld = stepWorld,
            outPoints = livePointsBuf,
            outPressures = livePressuresBuf,
            maxOutPoints = maxPoints
        )
        if (liveCount > 0) {
            liveUpdate(livePointsBuf, livePressuresBuf, liveCount)
        }
    }

    private fun submitFinalStroke() {
        val scale = scaleProvider().coerceAtLeast(1e-4f)
        val anchors = ArrayList<PointF>(committedAnchorWorld.size + tailRawWorld.size + 2)
        val pressures = ArrayList<Float>(committedAnchorPressures.size + tailRawPressures.size + 2)

        anchors.addAll(committedAnchorWorld)
        pressures.addAll(committedAnchorPressures)
        val stabilizedTail = stabilizeTail(tailRawWorld, tailRawPressures, scale)
        anchors.addAll(stabilizedTail.first)
        pressures.addAll(stabilizedTail.second)

        if (anchors.size < 2) return
        val stepWorld = computeDesiredStepWorld(
            anchors = anchors,
            scale = scale,
            targetPoints = 1000,
            maxPointsCap = null
        )
        submitResampledQuadSplineAsStrokes(
            anchors = anchors,
            anchorPressures = pressures,
            stepWorld = stepWorld,
            maxStrokePoints = maxPoints
        )
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

    private fun computeDesiredStepWorld(
        anchors: List<PointF>,
        scale: Float,
        targetPoints: Int,
        maxPointsCap: Int?
    ): Float {
        if (anchors.size < 2) return (1.0f / scale).coerceAtLeast(1e-6f)
        var lengthWorld = 0f
        for (i in 1 until anchors.size) {
            val a = anchors[i - 1]
            val b = anchors[i]
            val dx = b.x - a.x
            val dy = b.y - a.y
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

    private fun buildQuadSplineSegments(anchors: List<PointF>, prs: List<Float>): List<QuadSeg> {
        val n = anchors.size
        if (n < 2) return emptyList()
        if (n == 2) {
            val a = anchors[0]
            val b = anchors[1]
            val c = PointF((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f)
            val pr = (prs[0] + prs[1]) * 0.5f
            return listOf(QuadSeg(a, c, b, prs[0], pr, prs[1]))
        }

        val mids = ArrayList<PointF>(n - 1)
        val midPrs = ArrayList<Float>(n - 1)
        for (i in 0 until n - 1) {
            val a = anchors[i]
            val b = anchors[i + 1]
            mids.add(PointF((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f))
            midPrs.add((prs[i] + prs[i + 1]) * 0.5f)
        }

        fun safeNormalize(dx: Float, dy: Float): Pair<Float, Float> {
            val len = kotlin.math.sqrt(dx * dx + dy * dy)
            if (len < 1e-6f) return Pair(0f, 0f)
            return Pair(dx / len, dy / len)
        }

        val out = ArrayList<QuadSeg>(n)

        run {
            val a0 = anchors[0]
            val a1 = anchors[1]
            val dir = safeNormalize(a1.x - a0.x, a1.y - a0.y)
            val handle = kotlin.math.min(distance(a0, a1) * 0.25f, distance(a0, mids[0]) * 0.9f)
            val c0 = PointF(a0.x + dir.first * handle, a0.y + dir.second * handle)
            out.add(QuadSeg(a0, c0, mids[0], prs[0], prs[0], midPrs[0]))
        }

        for (i in 1 until n - 1) {
            out.add(QuadSeg(mids[i - 1], anchors[i], mids[i], midPrs[i - 1], prs[i], midPrs[i]))
        }

        run {
            val prev = anchors[n - 2]
            val end = anchors[n - 1]
            val dir = safeNormalize(end.x - prev.x, end.y - prev.y)
            val handle = kotlin.math.min(distance(prev, end) * 0.25f, distance(mids[n - 2], end) * 0.9f)
            val c1 = PointF(end.x - dir.first * handle, end.y - dir.second * handle)
            out.add(QuadSeg(mids[n - 2], c1, end, midPrs[n - 2], prs[n - 1], prs[n - 1]))
        }

        val sanitized = ArrayList<QuadSeg>(out.size)
        for (seg in out) {
            val chord = distance(seg.p0, seg.p2)
            val d01 = distance(seg.p0, seg.p1)
            if (chord > 1e-3f && d01 < chord * 0.02f) {
                val c = PointF(
                    seg.p0.x + (seg.p2.x - seg.p0.x) * 0.5f,
                    seg.p0.y + (seg.p2.y - seg.p0.y) * 0.5f
                )
                sanitized.add(QuadSeg(seg.p0, c, seg.p2, seg.pr0, seg.pr1, seg.pr2))
            } else {
                sanitized.add(seg)
            }
        }
        return sanitized
    }

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
