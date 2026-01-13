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
    private var currentScale = 1.0f
    private var translateX = 0.0f
    private var translateY = 0.0f
    private var suppressDrawUntilNextDown = false
    private val input = StrokeInputProcessor(
        screenToWorld = { x, y ->
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
            val newScale = (oldScale * factor).coerceIn(0.2f, 8.0f)
            if (newScale != oldScale) {
                val focusWorldX = (focusX - translateX) / oldScale
                val focusWorldY = (focusY - translateY) / oldScale
                translateX = focusX - focusWorldX * newScale
                translateY = focusY - focusWorldY * newScale
                currentScale = newScale

                NativeBridge.setViewTransform(currentScale, translateX, translateY)
            }
            return true
        }
        override fun onScaleBegin(detector: ScaleGestureDetector): Boolean {
            return true
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
