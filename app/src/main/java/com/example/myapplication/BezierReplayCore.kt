package com.example.myapplication

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.acos
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.sqrt

internal data class ReplayVec2(val x: Float, val y: Float)

internal data class ReplayQuadSegment(
    val p0: ReplayVec2,
    val p1: ReplayVec2,
    val p2: ReplayVec2
)

internal data class ReplayInputPoint(
    val x: Float,
    val y: Float,
    val timestamp: Long,
    val eventType: ReplayEventType
)

internal enum class ReplayEventType {
    DOWN,
    MOVE,
    UP,
    CANCEL
}

internal object BezierReplayCore {
    fun splitStrokes(points: List<ReplayInputPoint>): List<List<ReplayInputPoint>> {
        val out = ArrayList<ArrayList<ReplayInputPoint>>()
        var cur: ArrayList<ReplayInputPoint>? = null

        for (p in points) {
            when (p.eventType) {
                ReplayEventType.DOWN -> {
                    cur = ArrayList()
                    cur.add(p)
                }
                ReplayEventType.MOVE -> {
                    cur?.add(p)
                }
                ReplayEventType.UP -> {
                    cur?.add(p)
                    if (cur != null && cur.size >= 2) out.add(cur)
                    cur = null
                }
                ReplayEventType.CANCEL -> {
                    if (cur != null && cur.size >= 2) out.add(cur)
                    cur = null
                }
            }
        }

        if (cur != null && cur.size >= 2) out.add(cur)
        return out
    }

    fun fitAndResample(
        points: List<ReplayVec2>,
        sampleStep: Float,
        cornerAngleDeg: Float = 60f
    ): Pair<List<ReplayQuadSegment>, List<ReplayVec2>> {
        if (points.size < 2) return emptyList<ReplayQuadSegment>() to emptyList()
        if (points.size == 2) {
            val a = points[0]
            val b = points[1]
            val c = ReplayVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f)
            val seg = ReplayQuadSegment(a, c, b)
            return listOf(seg) to resampleSegments(listOf(seg), sampleStep)
        }

        val anchorIndices = buildAnchorIndices(points, cornerAngleDeg)
        if (anchorIndices.size < 2) return emptyList<ReplayQuadSegment>() to points

        val segments = ArrayList<ReplayQuadSegment>(max(1, anchorIndices.size - 1))
        for (i in 0 until anchorIndices.size - 1) {
            val startIdx = anchorIndices[i]
            val endIdx = anchorIndices[i + 1]
            if (endIdx <= startIdx) continue

            val a = points[startIdx]
            val b = points[endIdx]
            val representative = chooseRepresentative(points, startIdx, endIdx, a, b)
            val c = ReplayVec2(
                2f * representative.x - (a.x + b.x) * 0.5f,
                2f * representative.y - (a.y + b.y) * 0.5f
            )
            segments.add(ReplayQuadSegment(a, c, b))
        }

        return segments to resampleSegments(segments, sampleStep)
    }

    fun buildAnchorIndices(points: List<ReplayVec2>, cornerAngleDeg: Float = 60f): IntArray {
        if (points.size < 2) return intArrayOf()

        val anchors = ArrayList<Int>(points.size)
        anchors.add(0)

        for (i in 1 until points.size - 1) {
            val p0 = points[i - 1]
            val p1 = points[i]
            val p2 = points[i + 1]

            val v1x = p1.x - p0.x
            val v1y = p1.y - p0.y
            val v2x = p2.x - p1.x
            val v2y = p2.y - p1.y

            val l1 = sqrt(v1x * v1x + v1y * v1y)
            val l2 = sqrt(v2x * v2x + v2y * v2y)
            if (l1 < 0.001f || l2 < 0.001f) continue

            val cos = ((v1x * v2x + v1y * v2y) / (l1 * l2)).coerceIn(-1f, 1f)
            val angle = (acos(cos) * 180f / PI.toFloat())
            if (angle >= cornerAngleDeg) {
                anchors.add(i)
            }
        }

        anchors.add(points.size - 1)

        val dedup = anchors.distinct()
        val out = IntArray(dedup.size)
        for (i in dedup.indices) out[i] = dedup[i]
        return out
    }

    fun resampleSegments(segments: List<ReplayQuadSegment>, sampleStep: Float): List<ReplayVec2> {
        if (segments.isEmpty()) return emptyList()

        val out = ArrayList<ReplayVec2>()
        out.add(segments[0].p0)

        for (seg in segments) {
            val approxLen = distance(seg.p0, seg.p2)
            val steps = max(2, ceil(approxLen / max(0.5f, sampleStep)).toInt())
            for (i in 1..steps) {
                val t = i.toFloat() / steps.toFloat()
                val p = evalQuad(seg, t)
                val last = out.last()
                if (abs(p.x - last.x) > 0.0001f || abs(p.y - last.y) > 0.0001f) {
                    out.add(p)
                }
            }
        }

        return out
    }

    fun evalQuad(seg: ReplayQuadSegment, t: Float): ReplayVec2 {
        val u = 1f - t
        val x = u * u * seg.p0.x + 2f * u * t * seg.p1.x + t * t * seg.p2.x
        val y = u * u * seg.p0.y + 2f * u * t * seg.p1.y + t * t * seg.p2.y
        return ReplayVec2(x, y)
    }

    private fun chooseRepresentative(
        points: List<ReplayVec2>,
        startIdx: Int,
        endIdx: Int,
        a: ReplayVec2,
        b: ReplayVec2
    ): ReplayVec2 {
        if (endIdx - startIdx <= 1) {
            return ReplayVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f)
        }

        var bestIdx = -1
        var bestDist = -1f
        for (i in startIdx + 1 until endIdx) {
            val p = points[i]
            val d = distancePointToSegment(p, a, b)
            if (d > bestDist) {
                bestDist = d
                bestIdx = i
            }
        }

        if (bestIdx == -1) return ReplayVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f)
        return points[bestIdx]
    }

    private fun distance(a: ReplayVec2, b: ReplayVec2): Float {
        val dx = a.x - b.x
        val dy = a.y - b.y
        return sqrt(dx * dx + dy * dy)
    }

    private fun distancePointToSegment(p: ReplayVec2, a: ReplayVec2, b: ReplayVec2): Float {
        val abx = b.x - a.x
        val aby = b.y - a.y
        val apx = p.x - a.x
        val apy = p.y - a.y

        val abLen2 = abx * abx + aby * aby
        if (abLen2 <= 0.000001f) return distance(p, a)

        val t = ((apx * abx + apy * aby) / abLen2).coerceIn(0f, 1f)
        val proj = ReplayVec2(a.x + t * abx, a.y + t * aby)
        return distance(p, proj)
    }
}

