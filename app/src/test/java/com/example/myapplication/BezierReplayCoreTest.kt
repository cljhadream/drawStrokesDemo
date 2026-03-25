package com.example.myapplication

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class BezierReplayCoreTest {
    @Test
    fun splitStrokes_splitsByDownUp() {
        val points = listOf(
            ReplayInputPoint(0f, 0f, 0L, ReplayEventType.DOWN),
            ReplayInputPoint(1f, 0f, 1L, ReplayEventType.MOVE),
            ReplayInputPoint(2f, 0f, 2L, ReplayEventType.UP),
            ReplayInputPoint(10f, 10f, 3L, ReplayEventType.DOWN),
            ReplayInputPoint(11f, 10f, 4L, ReplayEventType.MOVE),
            ReplayInputPoint(12f, 10f, 5L, ReplayEventType.CANCEL),
            ReplayInputPoint(20f, 20f, 6L, ReplayEventType.DOWN),
            ReplayInputPoint(21f, 20f, 7L, ReplayEventType.UP)
        )

        val strokes = BezierReplayCore.splitStrokes(points)
        assertEquals(3, strokes.size)
        assertEquals(3, strokes[0].size)
        assertEquals(2, strokes[1].size)
        assertEquals(2, strokes[2].size)
    }

    @Test
    fun buildAnchorIndices_detectsCorner() {
        val pts = listOf(
            ReplayVec2(0f, 0f),
            ReplayVec2(10f, 0f),
            ReplayVec2(10f, 10f)
        )

        val anchors = BezierReplayCore.buildAnchorIndices(pts, cornerAngleDeg = 60f).toList()
        assertTrue(anchors.contains(0))
        assertTrue(anchors.contains(1))
        assertTrue(anchors.contains(2))
    }

    @Test
    fun fitAndResample_preservesAnchorPointsInResampled() {
        val pts = listOf(
            ReplayVec2(0f, 0f),
            ReplayVec2(30f, 0f),
            ReplayVec2(30f, 40f),
            ReplayVec2(30f, 80f)
        )

        val anchors = BezierReplayCore.buildAnchorIndices(pts, cornerAngleDeg = 60f).toList()
        val (_, resampled) = BezierReplayCore.fitAndResample(
            points = pts,
            sampleStep = 5f,
            cornerAngleDeg = 60f
        )

        for (idx in anchors) {
            assertTrue(containsPoint(resampled, pts[idx], eps = 0.001f))
        }
    }

    @Test
    fun fitAndResample_endpointsMatchInput() {
        val pts = listOf(
            ReplayVec2(5f, 6f),
            ReplayVec2(20f, 10f),
            ReplayVec2(45f, 18f)
        )

        val (segments, resampled) = BezierReplayCore.fitAndResample(pts, sampleStep = 2f, cornerAngleDeg = 60f)
        assertTrue(segments.isNotEmpty())
        assertTrue(resampled.isNotEmpty())

        assertTrue(close(segments.first().p0, pts.first(), eps = 0.0001f))
        assertTrue(close(segments.last().p2, pts.last(), eps = 0.0001f))
    }

    private fun containsPoint(list: List<ReplayVec2>, target: ReplayVec2, eps: Float): Boolean {
        for (p in list) {
            if (close(p, target, eps)) return true
        }
        return false
    }

    private fun close(a: ReplayVec2, b: ReplayVec2, eps: Float): Boolean {
        return kotlin.math.abs(a.x - b.x) <= eps && kotlin.math.abs(a.y - b.y) <= eps
    }
}
