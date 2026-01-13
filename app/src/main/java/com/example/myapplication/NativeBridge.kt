package com.example.myapplication

/**
 * Kotlin到C++的JNI桥，提供渲染相关的本地方法接口。
 */
object NativeBridge {
    init {
        System.loadLibrary("native-lib")
    }

    external fun onNativeSurfaceCreated()
    external fun onNativeSurfaceChanged(width: Int, height: Int)
    external fun onNativeDrawFrame()
    external fun setViewScale(scale: Float)
    external fun setViewTransform(scale: Float, cx: Float, cy: Float)
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
}
