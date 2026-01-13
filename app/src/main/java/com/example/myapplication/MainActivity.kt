package com.example.myapplication

import android.os.Bundle
import android.util.TypedValue
import android.util.Log
import android.view.Gravity
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.TextView
import androidx.activity.ComponentActivity
import kotlin.math.PI
import kotlin.math.roundToInt
import kotlin.math.sin

/**
 * 主界面，创建并显示GLSurfaceView用于OpenGL ES渲染。
 */
class MainActivity : ComponentActivity() {
    private lateinit var glView: StrokeGLSurfaceView
    private var demoDrawn: Boolean = false
    private lateinit var scaleLabel: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.d("MainActivity", "Creating StrokeGLSurfaceView...")
        glView = StrokeGLSurfaceView(this)
        Log.d("MainActivity", "Setting content view...")
        val root = FrameLayout(this)
        root.layoutParams = ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        )

        val glParams = FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT
        )
        root.addView(glView, glParams)

        scaleLabel = TextView(this).apply {
            setTextColor(0xFFFFFFFF.toInt())
            setBackgroundColor(0x80000000.toInt())
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            val pad = dpToPx(8f)
            setPadding(pad, pad, pad, pad)
            text = "100%"
        }
        val labelParams = FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT,
            FrameLayout.LayoutParams.WRAP_CONTENT
        ).apply {
            gravity = Gravity.START or Gravity.TOP
            val m = dpToPx(12f)
            leftMargin = m
            topMargin = m
        }
        root.addView(scaleLabel, labelParams)

        glView.setOnViewScaleChangedListener { scale ->
            scaleLabel.text = "${(scale * 100f).roundToInt()}%"
        }

        setContentView(root)
        Log.d("MainActivity", "Content view set")
    }

    override fun onResume() {
        super.onResume()
        glView.onResume()
        if (!demoDrawn) {
            drawDemoStroke()
            demoDrawn = true
        }
    }

    override fun onPause() {
        glView.onPause()
        super.onPause()
    }

    /**
     * 绘制1万条测试笔划，用于性能测试。
     * 生成不同颜色、位置和形状的线条来测试渲染性能。
     */
    private fun drawDemoStroke() {
        val dm = resources.displayMetrics
        val w = dm.widthPixels.toFloat()
        val h = dm.heightPixels.toFloat()
        
        // 生成10条线条，用于测试平滑效果
        val totalStrokeCount = 10
        val pointsPerStroke = 1024
        val batchSize = 5000 // 每批5000条线，避免内存溢出
        val totalBatches = (totalStrokeCount + batchSize - 1) / batchSize
        
        Log.i("MainActivity", "开始生成 $totalStrokeCount 条测试线条，分 $totalBatches 批处理...")
        
        // 分批处理，避免一次性分配大量内存
        for (batchIndex in 0 until totalBatches) {
            val startStroke = batchIndex * batchSize
            val endStroke = minOf(startStroke + batchSize, totalStrokeCount)
            val currentBatchSize = endStroke - startStroke
            
            Log.i("MainActivity", "处理第 ${batchIndex + 1}/$totalBatches 批，线条 $startStroke-${endStroke-1}")
            
            // 为当前批次预分配固定大小的数组，避免ArrayList动态扩容
            val batchPoints = FloatArray(currentBatchSize * pointsPerStroke * 2)
            val batchPressures = FloatArray(currentBatchSize * pointsPerStroke)
            val batchCounts = IntArray(currentBatchSize) { pointsPerStroke }
            val batchColors = FloatArray(currentBatchSize * 4)
            
            var pointIndex = 0
            var pressureIndex = 0
            
            for (strokeIndex in startStroke until endStroke) {
                val localStrokeIndex = strokeIndex - startStroke
                
                // 为每条线生成随机颜色
                val hue = (strokeIndex.toFloat() / totalStrokeCount) * 360f // 彩虹色谱
                val saturation = 0.7f + (strokeIndex % 3) * 0.1f // 0.7-0.9
                val brightness = 0.8f + (strokeIndex % 2) * 0.2f // 0.8-1.0
                val alpha = 0.6f + (strokeIndex % 5) * 0.08f // 0.6-0.92
                
                val rgb = hsvToRgb(hue, saturation, brightness)
                batchColors[localStrokeIndex * 4] = rgb[0]
                batchColors[localStrokeIndex * 4 + 1] = rgb[1] 
                batchColors[localStrokeIndex * 4 + 2] = rgb[2]
                batchColors[localStrokeIndex * 4 + 3] = alpha
            
                // 为每条线生成不同的位置和形状
                // 使用随机分布确保覆盖整个屏幕
                val random1 = (strokeIndex * 17 + 23) % 1000 / 1000f  // 伪随机数1
                val random2 = (strokeIndex * 31 + 47) % 1000 / 1000f  // 伪随机数2
                val random3 = (strokeIndex * 13 + 71) % 1000 / 1000f  // 伪随机数3
                val random4 = (strokeIndex * 41 + 89) % 1000 / 1000f  // 伪随机数4
                
                // 确保线条分布在整个屏幕范围内
                val margin = 50f // 边距
                val startX = margin + random1 * (w - 2 * margin)
                val startY = margin + random2 * (h - 2 * margin)
                
                // 增加线条长度，使其更明显
                val lengthVariation = 1.0f + 0.5f * random3 // 100%-150%的长度变化（最小为屏幕宽度）
                val angleVariation = random4 * 360f // 0-360度的随机角度
                val angle = Math.toRadians(angleVariation.toDouble()).toFloat()
                
                val lineLength = w * lengthVariation
                val endX = startX + lineLength * kotlin.math.cos(angle)
                val endY = startY + lineLength * kotlin.math.sin(angle)
                
                // 使用 Catmull-Rom 样条插值生成平滑且密集的点序列
                // 控制点：我们在屏幕上随机生成几个关键点
                val keyPointsX = FloatArray(6)
                val keyPointsY = FloatArray(6)
                
                // 生成关键控制点
                for (k in 0 until 6) {
                    val t = k / 5.0f
                    val bx = startX + (endX - startX) * t
                    val by = startY + (endY - startY) * t
                    
                    // 添加随机扰动
                    val waveType = strokeIndex % 4
                    val amplitude = h * 0.15f * (1 + strokeIndex % 2) // 增加振幅
                    val frequency = 1.5f + (strokeIndex % 3)
                    
                    val waveY = when (waveType) {
                        0 -> amplitude * sin(2f * PI.toFloat() * frequency * t) 
                        1 -> amplitude * sin(2f * PI.toFloat() * frequency * t) * t
                        2 -> amplitude * (if (sin(2f * PI.toFloat() * frequency * t) > 0) 1f else -1f)
                        else -> amplitude * (2f * t - 1f) * sin(2f * PI.toFloat() * frequency * t)
                    }
                    
                    keyPointsX[k] = bx
                    keyPointsY[k] = by + waveY
                }
                
                // 在关键点之间进行高密度插值
                // pointsPerStroke = 64 (现在我们充分利用这64个点来画一条平滑曲线)
                // 为了保证极致平滑，我们应该让点非常密集。
                // Catmull-Rom 需要4个点 P0, P1, P2, P3 来计算 P1-P2 之间的曲线
                
                var outIdx = 0
                // 我们有 6 个关键点，能形成 3 段完整的 Catmull-Rom 曲线 (1-2, 2-3, 3-4)
                // 为了简单，我们对整条线进行分段插值
                
                // 增加点数以获得更好的平滑度，但受限于 batchPoints 预分配的大小 (64)
                // 这里我们尽量均匀分布
                val segments = 5 // 5段间隔
                val pointsPerSegment = (pointsPerStroke - 4) / segments 
                
                for (i in 0 until pointsPerStroke) {
                    // 映射 i 到 t (0.0 - 5.0)
                    val tGlobal = i.toFloat() / (pointsPerStroke - 1) * (6 - 1)
                    val segmentIdx = tGlobal.toInt().coerceIn(0, 4)
                    val tLocal = tGlobal - segmentIdx
                    
                    // 获取4个控制点 (处理边界)
                    val p0Idx = (segmentIdx - 1).coerceAtLeast(0)
                    val p1Idx = segmentIdx
                    val p2Idx = (segmentIdx + 1).coerceAtMost(5)
                    val p3Idx = (segmentIdx + 2).coerceAtMost(5)
                    
                    val p0x = keyPointsX[p0Idx]; val p0y = keyPointsY[p0Idx]
                    val p1x = keyPointsX[p1Idx]; val p1y = keyPointsY[p1Idx]
                    val p2x = keyPointsX[p2Idx]; val p2y = keyPointsY[p2Idx]
                    val p3x = keyPointsX[p3Idx]; val p3y = keyPointsY[p3Idx]
                    
                    // Catmull-Rom 公式
                    val tt = tLocal * tLocal
                    val ttt = tt * tLocal
                    
                    val q0 = -0.5f * ttt + tLocal * tLocal - 0.5f * tLocal
                    val q1 =  1.5f * ttt - 2.5f * tLocal * tLocal + 1.0f
                    val q2 = -1.5f * ttt + 2.0f * tLocal * tLocal + 0.5f * tLocal
                    val q3 =  0.5f * ttt - 0.5f * tLocal * tLocal
                    
                    val px = 0.5f * (2f*p1x + (p2x-p0x)*tLocal + (2f*p0x - 5f*p1x + 4f*p2x - p3x)*tt + (-p0x + 3f*p1x - 3f*p2x + p3x)*ttt)
                    val py = 0.5f * (2f*p1y + (p2y-p0y)*tLocal + (2f*p0y - 5f*p1y + 4f*p2y - p3y)*tt + (-p0y + 3f*p1y - 3f*p2y + p3y)*ttt)
                    
                    batchPoints[pointIndex++] = px
                    batchPoints[pointIndex++] = py
                    
                    // 生成变化的压力值
                    val pressure = 0.4f + 0.6f * (0.5f + 0.5f * sin(PI.toFloat() * tGlobal + strokeIndex * 0.1f))
                    batchPressures[pressureIndex++] = pressure
                }
            }
            
            // 提交当前批次到GL线程
            glView.queueEvent {
                val startTime = System.currentTimeMillis()
                NativeBridge.addStrokeBatch(
                    batchPoints,
                    batchPressures, 
                    batchCounts,
                    batchColors
                )
                val endTime = System.currentTimeMillis()
                
                Log.i("MainActivity", "批次 ${batchIndex + 1} 提交完成，耗时: ${endTime - startTime}ms，" +
                        "线条数: $currentBatchSize，顶点数: ${currentBatchSize * pointsPerStroke}")
            }
            
            // 在批次之间添加短暂延迟，让系统有时间进行垃圾回收
            if (batchIndex < totalBatches - 1) {
                Thread.sleep(10)
            }
        }
        
        Log.i("MainActivity", "所有 $totalStrokeCount 条线条生成完成！")
    }
    
    /**
     * HSV转RGB颜色空间转换
     */
    private fun hsvToRgb(h: Float, s: Float, v: Float): FloatArray {
        val c = v * s
        val x = c * (1 - kotlin.math.abs((h / 60f) % 2 - 1))
        val m = v - c
        
        val (r1, g1, b1) = when {
            h < 60 -> Triple(c, x, 0f)
            h < 120 -> Triple(x, c, 0f)
            h < 180 -> Triple(0f, c, x)
            h < 240 -> Triple(0f, x, c)
            h < 300 -> Triple(x, 0f, c)
            else -> Triple(c, 0f, x)
        }
        
        return floatArrayOf(r1 + m, g1 + m, b1 + m)
    }

    private fun dpToPx(dp: Float): Int {
        return TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP,
            dp,
            resources.displayMetrics
        ).roundToInt()
    }
}
