package com.example.myapplication

import android.graphics.Canvas
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Path
import android.os.Bundle
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.View
import android.view.ViewGroup
import android.widget.CheckBox
import android.widget.FrameLayout
import androidx.activity.ComponentActivity
import org.json.JSONArray
import java.io.File
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.sqrt

class BezierReplayActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = FrameLayout(this)
        root.layoutParams = ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        )

        val replayView = BezierReplayView(this)
        root.addView(
            replayView,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT
            )
        )

        val showPoints = CheckBox(this).apply {
            setTextColor(0xFFFFFFFF.toInt())
            setBackgroundColor(0x80000000.toInt())
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            val pad = dpToPx(8f)
            setPadding(pad, pad, pad, pad)
            text = "显示原始报点"
            isChecked = true
            setOnCheckedChangeListener { _, checked ->
                replayView.setShowRawPoints(checked)
            }
        }
        root.addView(
            showPoints,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT
            ).apply {
                gravity = Gravity.START or Gravity.TOP
                val m = dpToPx(12f)
                leftMargin = m
                topMargin = m
            }
        )

        setContentView(root)
    }

    private fun dpToPx(dp: Float): Int {
        return TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP,
            dp,
            resources.displayMetrics
        ).roundToInt()
    }
}

private class BezierReplayView(context: android.content.Context) : View(context) {
    private var showRawPoints: Boolean = true

    private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFFE91E63.toInt()
        style = Paint.Style.STROKE
        strokeWidth = dpToPx(1f).toFloat()
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }

    private val rawPointPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFF00E5FF.toInt()
        style = Paint.Style.FILL
    }

    private val baseStrokeWidthPx: Float = strokePaint.strokeWidth
    private val baseRawPointRadiusPx: Float = dpToPx(1f).toFloat()
    private var currentScale = 1.0f
    private var translateX = 0.0f
    private var translateY = 0.0f
    private var lastPanX = 0.0f
    private var lastPanY = 0.0f
    private val viewMatrix = Matrix()
    private val scaleDetector =
        ScaleGestureDetector(context, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(detector: ScaleGestureDetector): Boolean {
                val oldScale = currentScale
                val newScale = (oldScale * detector.scaleFactor).coerceIn(0.3f, 15.0f)
                if (newScale != oldScale) {
                    val focusX = detector.focusX
                    val focusY = detector.focusY
                    val focusWorldX = (focusX - translateX) / oldScale
                    val focusWorldY = (focusY - translateY) / oldScale
                    translateX = focusX - focusWorldX * newScale
                    translateY = focusY - focusWorldY * newScale
                    currentScale = newScale
                    invalidate()
                }
                return true
            }
        })

    private val sampleBase1000: List<ReplayInputPoint> = loadSamplePoints()

    private var rawStrokePointsScreen: List<List<ReplayVec2>> = emptyList()
    private var smoothedStrokePointsScreen: List<List<ReplayVec2>> = emptyList()
    private var finalSegmentsByStroke: List<List<ReplayQuadSegment>> = emptyList()

    init {
        isClickable = true
        isFocusable = true
        isFocusableInTouchMode = true
    }

    fun setShowRawPoints(show: Boolean) {
        showRawPoints = show
        invalidate()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        if (w <= 0) return

        val strokes = BezierReplayCore.splitStrokes(sampleBase1000)
        val scaleToScreen = w / 1000f

        rawStrokePointsScreen = strokes.map { stroke ->
            stroke.map { p ->
                ReplayVec2(p.x * scaleToScreen, p.y * scaleToScreen)
            }
        }

        smoothedStrokePointsScreen = rawStrokePointsScreen.map { stroke ->
            denoiseStroke(stroke)
        }

        finalSegmentsByStroke = smoothedStrokePointsScreen.map { stroke ->
            buildFinalSegments(stroke, w.toFloat())
        }

        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)

        val scale = currentScale.coerceAtLeast(1e-4f)
        strokePaint.strokeWidth = baseStrokeWidthPx / scale
        val r = baseRawPointRadiusPx / scale

        viewMatrix.reset()
        viewMatrix.setScale(scale, scale)
        viewMatrix.postTranslate(translateX, translateY)
        canvas.save()
        canvas.concat(viewMatrix)

        for (strokeIdx in finalSegmentsByStroke.indices) {
            val segments = finalSegmentsByStroke[strokeIdx]
            if (segments.isEmpty()) continue

            val path = Path()
            path.moveTo(segments[0].p0.x, segments[0].p0.y)
            for (seg in segments) {
                path.quadTo(seg.p1.x, seg.p1.y, seg.p2.x, seg.p2.y)
            }
            canvas.drawPath(path, strokePaint)
        }

        if (showRawPoints) {
            for (stroke in rawStrokePointsScreen) {
                for (p in stroke) {
                    canvas.drawCircle(p.x, p.y, r, rawPointPaint)
                }
            }
        }

        canvas.restore()
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        scaleDetector.onTouchEvent(event)
        if (scaleDetector.isInProgress) return true

        if (event.pointerCount > 1) return true

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                lastPanX = event.x
                lastPanY = event.y
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                if (currentScale > 1.0f) {
                    val dx = event.x - lastPanX
                    val dy = event.y - lastPanY
                    translateX += dx
                    translateY += dy
                    lastPanX = event.x
                    lastPanY = event.y
                    invalidate()
                }
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> return true
        }

        return super.onTouchEvent(event)
    }

    private fun buildFinalSegments(rawScreen: List<ReplayVec2>, canvasWidth: Float): List<ReplayQuadSegment> {
        if (rawScreen.size < 2) return emptyList()
        if (rawScreen.size == 2) {
            val a = rawScreen[0]
            val b = rawScreen[1]
            val c = ReplayVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f)
            return listOf(ReplayQuadSegment(p0 = a, p1 = c, p2 = b))
        }

        val n = rawScreen.size
        val mids = ArrayList<ReplayVec2>(n - 1)
        for (i in 0 until n - 1) {
            val a = rawScreen[i]
            val b = rawScreen[i + 1]
            mids.add(ReplayVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f))
        }

        val out = ArrayList<ReplayQuadSegment>(n)
        out.add(ReplayQuadSegment(p0 = rawScreen[0], p1 = rawScreen[0], p2 = mids[0]))
        for (i in 1 until n - 1) {
            val p0 = mids[i - 1]
            val p1 = rawScreen[i]
            val p2 = mids[i]
            out.add(ReplayQuadSegment(p0 = p0, p1 = p1, p2 = p2))
        }
        out.add(ReplayQuadSegment(p0 = mids[n - 2], p1 = rawScreen[n - 1], p2 = rawScreen[n - 1]))
        return out
    }

    private fun denoiseStroke(points: List<ReplayVec2>): List<ReplayVec2> {
        if (points.size < 3) return points

        val minDistPx = 0.8f
        val minDist2 = minDistPx * minDistPx
        val filtered = ArrayList<ReplayVec2>(points.size)
        filtered.add(points[0])
        for (i in 1 until points.size) {
            val last = filtered.last()
            val p = points[i]
            val dx = p.x - last.x
            val dy = p.y - last.y
            if (dx * dx + dy * dy >= minDist2) {
                filtered.add(p)
            }
        }
        if (filtered.size < 3) return filtered

        val cosThreshold = 0.95f
        val maxSegPx = 8.0f
        val maxSeg2 = maxSegPx * maxSegPx
        val eps = 1e-6f

        var cur = filtered.toMutableList()
        repeat(2) {
            val next = cur.toMutableList()
            for (i in 1 until cur.size - 1) {
                val p0 = cur[i - 1]
                val p1 = cur[i]
                val p2 = cur[i + 1]

                val ax = p1.x - p0.x
                val ay = p1.y - p0.y
                val bx = p2.x - p1.x
                val by = p2.y - p1.y
                val la2 = ax * ax + ay * ay
                val lb2 = bx * bx + by * by
                if (la2 < eps || lb2 < eps) continue
                if (max(la2, lb2) > maxSeg2) continue

                val cos = ((ax * bx + ay * by) / sqrt(la2 * lb2)).coerceIn(-1f, 1f)
                if (cos < cosThreshold) continue

                next[i] = ReplayVec2(
                    x = (p0.x + 2f * p1.x + p2.x) * 0.25f,
                    y = (p0.y + 2f * p1.y + p2.y) * 0.25f
                )
            }
            cur = next
        }

        val out = ArrayList<ReplayVec2>(cur.size)
        out.add(cur[0])
        for (i in 1 until cur.size) {
            val last = out.last()
            val p = cur[i]
            val dx = p.x - last.x
            val dy = p.y - last.y
            if (dx * dx + dy * dy >= minDist2) out.add(p)
        }
        return out
    }

    private fun parseFixedJson(json: String): List<ReplayInputPoint> {
        val arr = JSONArray(json)
        val out = ArrayList<ReplayInputPoint>(arr.length())
        for (i in 0 until arr.length()) {
            val obj = arr.getJSONObject(i)
            val x = obj.getDouble("x").toFloat()
            val y = obj.getDouble("y").toFloat()
            val timestamp = obj.getLong("timestamp")
            val eventType = when (obj.getString("eventType").uppercase()) {
                "DOWN" -> ReplayEventType.DOWN
                "MOVE" -> ReplayEventType.MOVE
                "UP" -> ReplayEventType.UP
                "CANCEL" -> ReplayEventType.CANCEL
                else -> ReplayEventType.MOVE
            }
            out.add(ReplayInputPoint(x, y, timestamp, eventType))
        }
        return out
    }

    private fun loadSamplePoints(): List<ReplayInputPoint> {
        val logText = readSdkJniLogTextOrNull() ?: return parseFixedJson(FIXED_POINTS_JSON)
        val parsed = parseSdkJniLogToReplayPoints(logText)
        if (parsed.isEmpty()) return parseFixedJson(FIXED_POINTS_JSON)
        return normalizeToBase1000(parsed)
    }

    private fun readSdkJniLogTextOrNull(): String? {
        val externalDir = context.getExternalFilesDir(null)
        val candidates = buildList {
            if (externalDir != null) add(File(externalDir, "sdk-jni-2026_03_24.log"))
            add(File(context.filesDir, "sdk-jni-2026_03_24.log"))
        }

        for (f in candidates) {
            if (!f.exists() || !f.isFile) continue
            return runCatching { f.readText() }.getOrNull()
        }
        return readSdkJniLogFromAssetsOrNull()
    }

    private fun readSdkJniLogFromAssetsOrNull(): String? {
        val assetNames = listOf(
            "sdk-jni-2026_03_24.log",
            "sdk-jni.log"
        )
        for (name in assetNames) {
            val text = runCatching {
                context.assets.open(name).bufferedReader().use { it.readText() }
            }.getOrNull()
            if (!text.isNullOrEmpty()) return text
        }
        return null
    }

    private fun parseSdkJniLogToReplayPoints(logText: String): List<ReplayInputPoint> {
        val downRegex = Regex("""onDown end.*?X:([-\d.]+),Y:([-\d.]+),Time:(\d+)""")
        val upRegex = Regex("""onUp end.*?X:([-\d.]+),Y:([-\d.]+),Time:(\d+)""")
        val moveRegex =
            Regex("""Touch onMove touchPoints.*?isPredict:(\d+).*?x:([-\d.]+),\s*y:([-\d.]+),p:([-\d.]+),time:(\d+)""")

        val out = ArrayList<ReplayInputPoint>(4096)
        var inStroke = false

        fun appendPoint(x: Float, y: Float, time: Long, eventType: ReplayEventType) {
            if (out.isNotEmpty()) {
                val last = out.last()
                if (last.x == x && last.y == y && last.eventType == eventType) return
            }
            out.add(ReplayInputPoint(x = x, y = y, timestamp = time, eventType = eventType))
        }

        for (line in logText.lineSequence()) {
            val down = downRegex.find(line)
            if (down != null) {
                val x = down.groupValues[1].toFloat()
                val y = down.groupValues[2].toFloat()
                val t = down.groupValues[3].toLong()
                if (inStroke && out.isNotEmpty()) {
                    val last = out.last()
                    appendPoint(last.x, last.y, t, ReplayEventType.CANCEL)
                }
                inStroke = true
                appendPoint(x, y, t, ReplayEventType.DOWN)
                continue
            }

            val move = moveRegex.find(line)
            if (move != null) {
                if (!inStroke) continue
                val isPredict = move.groupValues[1].toInt()
                if (isPredict != 0) continue
                val x = move.groupValues[2].toFloat()
                val y = move.groupValues[3].toFloat()
                val t = move.groupValues[5].toLong()
                appendPoint(x, y, t, ReplayEventType.MOVE)
                continue
            }

            val up = upRegex.find(line)
            if (up != null) {
                if (!inStroke) continue
                val x = up.groupValues[1].toFloat()
                val y = up.groupValues[2].toFloat()
                val t = up.groupValues[3].toLong()
                appendPoint(x, y, t, ReplayEventType.UP)
                inStroke = false
            }
        }

        if (out.isEmpty()) return out

        val t0 = out.minOf { it.timestamp }
        if (t0 != 0L) {
            for (i in out.indices) {
                val p = out[i]
                out[i] = p.copy(timestamp = p.timestamp - t0)
            }
        }

        return out
    }

    private fun normalizeToBase1000(points: List<ReplayInputPoint>): List<ReplayInputPoint> {
        if (points.isEmpty()) return points

        var minX = Float.POSITIVE_INFINITY
        var minY = Float.POSITIVE_INFINITY
        var maxX = Float.NEGATIVE_INFINITY
        var maxY = Float.NEGATIVE_INFINITY
        for (p in points) {
            minX = min(minX, p.x)
            minY = min(minY, p.y)
            maxX = max(maxX, p.x)
            maxY = max(maxY, p.y)
        }

        val spanX = (maxX - minX).coerceAtLeast(0.001f)
        val spanY = (maxY - minY).coerceAtLeast(0.001f)
        val span = max(spanX, spanY)
        val scale = 900f / span
        val scaledW = spanX * scale
        val scaledH = spanY * scale
        val offsetX = (1000f - scaledW) * 0.5f - minX * scale
        val offsetY = (1000f - scaledH) * 0.5f - minY * scale

        val out = ArrayList<ReplayInputPoint>(points.size)
        for (p in points) {
            out.add(
                p.copy(
                    x = p.x * scale + offsetX,
                    y = p.y * scale + offsetY
                )
            )
        }
        return out
    }

    private fun dpToPx(dp: Float): Int {
        return TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP,
            dp,
            resources.displayMetrics
        ).roundToInt()
    }

    private companion object {
        private val FIXED_POINTS_JSON = """
            [
              {"x":100,"y":180,"timestamp":0,"eventType":"DOWN"},
              {"x":160,"y":182,"timestamp":10,"eventType":"MOVE"},
              {"x":240,"y":185,"timestamp":20,"eventType":"MOVE"},
              {"x":330,"y":188,"timestamp":30,"eventType":"MOVE"},
              {"x":420,"y":190,"timestamp":40,"eventType":"MOVE"},

              {"x":500,"y":190,"timestamp":50,"eventType":"MOVE"},
              {"x":520,"y":210,"timestamp":60,"eventType":"MOVE"},
              {"x":530,"y":260,"timestamp":70,"eventType":"MOVE"},
              {"x":535,"y":340,"timestamp":80,"eventType":"MOVE"},
              {"x":540,"y":420,"timestamp":90,"eventType":"MOVE"},

              {"x":545,"y":520,"timestamp":100,"eventType":"MOVE"},
              {"x":548,"y":620,"timestamp":110,"eventType":"MOVE"},
              {"x":550,"y":720,"timestamp":120,"eventType":"UP"}
            ]
        """.trimIndent()
    }
}
