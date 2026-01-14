package com.example.myapplication

import android.content.Context
import android.opengl.GLSurfaceView
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import javax.microedition.khronos.egl.EGL10
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.egl.EGLDisplay

/**
 * 配置OpenGL ES上下文版本为3，设置Renderer并启用连续渲染。
 */
class StrokeGLSurfaceView(context: Context) : GLSurfaceView(context) {
    private val renderer = NativeRenderer()
    private val batcher = StrokeBatcher()

    // 视图变换参数：
    // - currentScale：世界坐标到屏幕坐标的缩放因子（用于无损矢量缩放）
    // - translateX/translateY：屏幕空间平移（以像素为单位），实现以手势焦点为中心缩放
    private var currentScale = 1.0f
    private var translateX = 0.0f
    private var translateY = 0.0f
    private var suppressDrawUntilNextDown = false
    private var onViewScaleChanged: ((Float) -> Unit)? = null
    private val input = StrokeInputProcessor(
        screenToWorld = { x, y ->
            // 将屏幕坐标反变换到世界坐标，确保缩放/平移时仍能正确采集笔迹点
            val inv = 1.0f / currentScale.coerceAtLeast(1e-4f)
            floatArrayOf((x - translateX) * inv, (y - translateY) * inv)
        },
        scaleProvider = { currentScale },
        jniSubmit = { points, pressures, color ->
            queueEvent {
                batcher.enqueue(points, pressures, color)
            }
        },
        liveBegin = { color ->
            queueEvent {
                NativeBridge.beginLiveStroke(color)
            }
        },
        liveUpdate = { points, pressures, count ->
            queueEvent {
                NativeBridge.updateLiveStrokeWithCount(points, pressures, count)
            }
        },
        liveEnd = {
            queueEvent {
                NativeBridge.endLiveStroke()
            }
        }
    )
    // 视图缩放手势检测器
    private val scaleDetector = ScaleGestureDetector(context, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
        override fun onScale(detector: ScaleGestureDetector): Boolean {
            val factor = detector.scaleFactor
            val focusX = detector.focusX
            val focusY = detector.focusY

            val oldScale = currentScale
            // 缩放范围：30% - 1500%
            val newScale = (oldScale * factor).coerceIn(0.3f, 15.0f)
            if (newScale != oldScale) {
                // 保持缩放焦点不漂移：先把焦点从屏幕映射回旧世界坐标，再换算到新屏幕坐标求出平移量
                val focusWorldX = (focusX - translateX) / oldScale
                val focusWorldY = (focusY - translateY) / oldScale
                translateX = focusX - focusWorldX * newScale
                translateY = focusY - focusWorldY * newScale
                currentScale = newScale

                // 仅更新视图变换参数，避免在 Kotlin 层做任何重建/重采样
                NativeBridge.setViewTransform(currentScale, translateX, translateY)
                onViewScaleChanged?.invoke(currentScale)
            }
            return true
        }
        override fun onScaleBegin(detector: ScaleGestureDetector): Boolean {
            queueEvent {
                NativeBridge.setInteractionState(true, System.currentTimeMillis())
                // 缩放手势进行中启用低LOD：显著降低每条笔划参与渲染的点数，优先保证交互流畅
                NativeBridge.setRenderMaxPoints(256)
            }
            return true
        }

        override fun onScaleEnd(detector: ScaleGestureDetector) {
            queueEvent {
                NativeBridge.setInteractionState(false, System.currentTimeMillis())
                // 手势结束恢复全量渲染，保证最终静止画面质量
                NativeBridge.setRenderMaxPoints(1024)
            }
        }
    })

    init {
        // 要求至少OpenGL ES 3.x；几何着色器后续使用ES 3.2
        setEGLContextClientVersion(3)
        setEGLConfigChooser(MsaaConfigChooser())
        preserveEGLContextOnPause = true
        
        setRenderer(renderer)
        renderMode = RENDERMODE_CONTINUOUSLY
        
        // 强制请求渲染，确保连续渲染模式生效
        requestRender()

        // 确保视图可接收触摸事件
        isClickable = true
        isFocusable = true
        isFocusableInTouchMode = true
        requestFocus()
        
        // 强制设置视图可见性
        visibility = android.view.View.VISIBLE
    }

    fun setOnViewScaleChangedListener(listener: ((Float) -> Unit)?) {
        onViewScaleChanged = listener
        onViewScaleChanged?.invoke(currentScale)
    }

    private class MsaaConfigChooser : EGLConfigChooser {
        override fun chooseConfig(egl: EGL10, display: EGLDisplay): EGLConfig {
            val configs = arrayOfNulls<EGLConfig>(64)
            val numConfig = IntArray(1)

            val configSpecMsaa4 = intArrayOf(
                EGL10.EGL_RED_SIZE, 8,
                EGL10.EGL_GREEN_SIZE, 8,
                EGL10.EGL_BLUE_SIZE, 8,
                EGL10.EGL_ALPHA_SIZE, 8,
                EGL10.EGL_DEPTH_SIZE, 16,
                EGL10.EGL_RENDERABLE_TYPE, 4,
                EGL10.EGL_SAMPLE_BUFFERS, 1,
                EGL10.EGL_SAMPLES, 4,
                EGL10.EGL_NONE
            )

            if (egl.eglChooseConfig(display, configSpecMsaa4, configs, configs.size, numConfig) && numConfig[0] > 0) {
                return configs[0]!!
            }

            val configSpecNoMsaa = intArrayOf(
                EGL10.EGL_RED_SIZE, 8,
                EGL10.EGL_GREEN_SIZE, 8,
                EGL10.EGL_BLUE_SIZE, 8,
                EGL10.EGL_ALPHA_SIZE, 8,
                EGL10.EGL_DEPTH_SIZE, 16,
                EGL10.EGL_RENDERABLE_TYPE, 4,
                EGL10.EGL_NONE
            )
            if (egl.eglChooseConfig(display, configSpecNoMsaa, configs, configs.size, numConfig) && numConfig[0] > 0) {
                return configs[0]!!
            }

            throw RuntimeException("无法选择合适的 EGLConfig")
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        // 优先处理缩放手势
        if (event.actionMasked == MotionEvent.ACTION_POINTER_DOWN) {
            input.cancelStroke()
            suppressDrawUntilNextDown = true
        }
        scaleDetector.onTouchEvent(event)
        if (scaleDetector.isInProgress) {
            return true
        }
        if (event.pointerCount > 1 || event.actionMasked == MotionEvent.ACTION_POINTER_UP) {
            return true
        }
        if (event.actionMasked == MotionEvent.ACTION_DOWN) {
            suppressDrawUntilNextDown = false
        } else if (suppressDrawUntilNextDown) {
            return true
        }
        
        // 处理绘制输入
        val handled = input.onTouchEvent(event)
        if (handled && event.action == MotionEvent.ACTION_UP) {
            // 触摸结束时立即批量提交，确保及时性
            queueEvent {
                batcher.flushSoon()
            }
            // 强制请求渲染
            requestRender()
        }
        return handled || super.onTouchEvent(event)
    }
}
