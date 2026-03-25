package com.example.myapplication

import android.graphics.Bitmap

/**
 * Kotlin到C++的JNI桥，提供渲染相关的本地方法接口。
 */
object NativeBridge {
    init {
        System.loadLibrary("native-lib")
    }

    private var scratchPixels: IntArray? = null
    private var scratchRgba: ByteArray? = null

    external fun onNativeSurfaceCreated()
    external fun onNativeSurfaceChanged(width: Int, height: Int)
    external fun onNativeDrawFrame()
    external fun setViewScale(scale: Float)
    external fun setViewTransform(scale: Float, cx: Float, cy: Float)
    /**
     * 设置渲染时每条笔划使用的最大点数（LOD）。
     * - 用于缩放手势进行中降低顶点负载，提升交互流畅度
     * - 取值范围：1..1024
     */
    external fun setRenderMaxPoints(maxPoints: Int)
    external fun setInteractionState(isInteracting: Boolean, timestampMs: Long)
    external fun beginLiveStroke(color: FloatArray)
    external fun updateLiveStroke(points: FloatArray, pressures: FloatArray)
    external fun updateLiveStrokeWithCount(points: FloatArray, pressures: FloatArray, count: Int)
    external fun endLiveStroke()

    // 传递笔划数据到原生层
    external fun addStroke(points: FloatArray, pressures: FloatArray, color: FloatArray)

    // 获取当前笔划总数
    external fun getStrokeCount(): Int

    // 获取蓝色笔划数量
    external fun getBlueStrokeCount(): Int

    /**
     * 批量提交多条笔划：
     * - points：按笔划拼接的[x,y]数组，长度为 sum(counts) * 2
     * - pressures：按笔划拼接的压力数组，长度为 sum(counts)
     * - counts：每条笔划的点数
     * - colors：每条笔划的RGBA颜色，长度为 counts.size * 4
     */
    external fun addStrokeBatch(points: FloatArray, pressures: FloatArray, counts: IntArray, colors: FloatArray)

    external fun isUsingSSBO(): Boolean

    external fun updateFallbackImage(rgba: ByteArray, width: Int, height: Int)

    fun updateFallbackBitmap(bitmap: Bitmap) {
        val w = bitmap.width
        val h = bitmap.height
        if (w <= 0 || h <= 0) return

        val pixelCount = w * h
        val pixels = scratchPixels?.takeIf { it.size >= pixelCount } ?: IntArray(pixelCount).also { scratchPixels = it }
        bitmap.getPixels(pixels, 0, w, 0, 0, w, h)

        val rgbaSize = pixelCount * 4
        val rgba = scratchRgba?.takeIf { it.size >= rgbaSize } ?: ByteArray(rgbaSize).also { scratchRgba = it }
        var o = 0
        for (i in 0 until pixelCount) {
            val argb = pixels[i]
            rgba[o + 0] = ((argb shr 16) and 0xFF).toByte()
            rgba[o + 1] = ((argb shr 8) and 0xFF).toByte()
            rgba[o + 2] = (argb and 0xFF).toByte()
            rgba[o + 3] = ((argb ushr 24) and 0xFF).toByte()
            o += 4
        }
        updateFallbackImage(rgba, w, h)
    }
}
