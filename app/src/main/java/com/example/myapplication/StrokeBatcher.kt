package com.example.myapplication

/**
 * 笔划批量提交聚合器：将多条笔划聚合为一次 JNI addStrokeBatch 调用。
 * - 使用短延迟（~16ms）窗口聚合，降低JNI往返与原生侧处理开销。
 */
class StrokeBatcher(
    private val maxBatchCount: Int = 8
) {
    private val pointsList = ArrayList<FloatArray>()
    private val pressuresList = ArrayList<FloatArray>()
    private val colorsList = ArrayList<FloatArray>()

    fun enqueue(points: FloatArray, pressures: FloatArray, color: FloatArray) {
        pointsList.add(points)
        pressuresList.add(pressures)
        colorsList.add(color)
        if (pointsList.size >= maxBatchCount) {
            flush()
        } else {
            // 立即提交单个笔划，避免延迟
            flush()
        }
    }

    /** 请求在短延迟后执行一次批量提交，聚合快速到达的多条笔划。 */
    fun flushSoon() {
        flush()
    }

    fun flush() {
        if (pointsList.isEmpty()) return
        // 展平为批量提交参数
        var totalPoints = 0
        for (p in pointsList) totalPoints += p.size / 2
        val pointsFlat = FloatArray(totalPoints * 2)
        val pressuresFlat = FloatArray(totalPoints)
        val counts = IntArray(pointsList.size)
        val colorsFlat = FloatArray(colorsList.size * 4)

        var pi = 0
        var pri = 0
        for (i in pointsList.indices) {
            val pts = pointsList[i]
            val prs = pressuresList[i]
            val cnt = pts.size / 2
            counts[i] = cnt
            System.arraycopy(pts, 0, pointsFlat, pi, pts.size)
            System.arraycopy(prs, 0, pressuresFlat, pri, prs.size)
            pi += pts.size
            pri += prs.size
        }
        for (i in colorsList.indices) {
            val c = colorsList[i]
            colorsFlat[i * 4 + 0] = c[0]
            colorsFlat[i * 4 + 1] = c[1]
            colorsFlat[i * 4 + 2] = c[2]
            colorsFlat[i * 4 + 3] = c[3]
        }

        // 通过批量接口提交
        NativeBridge.addStrokeBatch(pointsFlat, pressuresFlat, counts, colorsFlat)

        pointsList.clear()
        pressuresList.clear()
        colorsList.clear()
    }
}
