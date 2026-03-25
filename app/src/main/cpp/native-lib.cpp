// Copyright-free. 第二阶段：JNI数据管理、缓冲与GLSL着色器实现。
#include <jni.h>
#include <android/log.h>
#include <GLES3/gl31.h>
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <unistd.h>

#define LOG_TAG "NativeLib@20260123_2"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static int g_Width = 0;
static int g_Height = 0;

// 配置：每条笔划最大点数（用于实例化绘制）
static const int kMaxPointsPerStroke = 1024;
static const int kVertsPerStroke = kMaxPointsPerStroke * 2 + 8;

// GL对象与状态
static GLuint gProgram = 0;
static GLuint gDebugProgram = 0;
static GLuint gImageProgram = 0;
static GLuint gTexProgram = 0;
static GLuint gVAO = 0;
static GLuint gEmptyVAO = 0; // 用于 SSBO 渲染路径的空 VAO
static GLuint gBypassVBO = 0;
static GLuint gPointsBuffer = 0;    // VBO: half(x,y,pressure)
static GLuint gPositionsSSBO = 0;   // SSBO(binding=1): float32 vec2 positions
static GLuint gStrokeMetaSSBO = 0;  // SSBO(binding=0): stroke metadata
static GLuint gPressuresSSBO = 0;  // SSBO(binding=2): float32 pressures
static GLuint gVisibleIndexSSBO = 0; // SSBO(binding=3): visible stroke id list
static GLuint gImageTex = 0;
static GLuint gImageVAO = 0;
static GLuint gImageVBO = 0;
static GLint uImageTexLoc = -1;
// Fallback-数据纹理（AHardwareBuffer/EGLImage 或普通纹理）
static GLuint gDataTex = 0;
static GLuint gMetaBWCTex = 0;
static GLuint gMetaColorTex = 0;
static GLint uDataTexLoc = -1;
static GLint uMetaBWCLoc = -1;
static GLint uMetaColorLoc = -1;
static AHardwareBuffer* gDataAHB = nullptr;
static AHardwareBuffer* gMetaBWCAHB = nullptr;
static AHardwareBuffer* gMetaColorAHB = nullptr;
static EGLImageKHR gDataImage = EGL_NO_IMAGE_KHR;
static EGLImageKHR gMetaBWCImage = EGL_NO_IMAGE_KHR;
static EGLImageKHR gMetaColorImage = EGL_NO_IMAGE_KHR;
static GLint uResolutionLoc = -1;
static GLint uViewScaleLoc = -1;
static GLint uViewTranslateLoc = -1;
static GLint uStrokeCountLoc = -1;
static GLint uBaseInstanceLoc = -1;
static GLint uMaxPointSizeLoc = -1;
static GLint uPassLoc = -1;
static GLint uRenderMaxPointsLoc = -1;
static float gViewScale = 1.0f;
static float gViewTranslateX = 0.0f;
static float gViewTranslateY = 0.0f;
static float gMaxPointSize = 64.0f;
static std::atomic<int> gRenderMaxPoints{1024};
static GLint uColorLoc = -1;     // 回退路径使用的颜色uniform
static bool gUseSSBO = true;     // 根据GL版本决定是否使用SSBO路径
static bool gHasVertexHalfFloat = false; // 是否支持顶点属性使用half浮点
static int gGlErrorLogBudget = 16;
static std::atomic<int> gStrokeUploadLogBudget{8};
static std::atomic<int> gBatchUploadLogBudget{8};
static std::atomic<int> gQueueLogBudget{8};
static std::atomic<int> gViewTransformLogBudget{64};
static std::atomic<int> gFirstFrameLogOnce{1};
static std::atomic<int> gFallbackFirstFrameLogOnce{1};
static bool gUseFramebufferFetch = false;
static bool gUseFramebufferFetchEXT = false;
static int gGestureStartStrokeId = -1;
static int gDarkenStrokeCount = 0;
static int gVisibleIndexCapacity = 0;
static int gVisibleCount = 0;
static std::atomic<int> gVisibleDirty{1};
static std::atomic<int> gIsInteracting{0};
static std::atomic<int64_t> gLastInteractionMs{0};
static std::atomic<int> gProgressCount{0};
static std::atomic<int> gFallbackStrokeCount{0};
static bool gGlReady = false;
static int gFallbackCapacityStrokes = 0;
static std::atomic<int> gFallbackAllocLogBudget{8};
static std::atomic<int> gFallbackWriteLogBudget{12};
static std::atomic<int> gFallbackFenceLogBudget{12};

static PFNEGLCREATEIMAGEKHRPROC gEglCreateImageKHR = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC gEglDestroyImageKHR = nullptr;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gGlEGLImageTargetTexture2DOES = nullptr;
static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC gEglGetNativeClientBufferANDROID = nullptr;
static PFNEGLCREATESYNCKHRPROC gEglCreateSyncKHR = nullptr;
static PFNEGLDESTROYSYNCKHRPROC gEglDestroySyncKHR = nullptr;
static PFNEGLWAITSYNCKHRPROC gEglWaitSyncKHR = nullptr;
static PFNEGLCLIENTWAITSYNCKHRPROC gEglClientWaitSyncKHR = nullptr;
static int gDataWriteFenceFd = -1;
static int gMetaBWCWriteFenceFd = -1;
static int gMetaColorWriteFenceFd = -1;
static GLsync gFallbackGpuSync = nullptr;

// 纹理路径：采样器uniform位置
static GLint uTexResolutionLoc = -1;
static GLint uTexViewScaleLoc = -1;
static GLint uTexViewTranslateLoc = -1;
static GLint uTexStrokeCountLoc = -1;
static GLint uTexBaseInstanceLoc = -1;
static GLint uTexPassLoc = -1;
static GLint uTexRenderMaxPointsLoc = -1;
static GLint uTexDataSamplerLoc = -1;
static GLint uTexMetaBWCSamplerLoc = -1;
static GLint uTexMetaColorSamplerLoc = -1;

// CPU侧元数据
struct StrokeMetaCPU {
    int start;
    int count;
    float baseWidth;
    float pad;
    float color[4];
    float type;
    float reserved0;
    float reserved1;
    float reserved2;
};
static std::vector<StrokeMetaCPU> gMetas;
struct StrokeBoundsCPU {
    float minX;
    float minY;
    float maxX;
    float maxY;
};
static std::vector<StrokeBoundsCPU> gBounds;
static std::vector<uint32_t> gVisiblePackedCPU;
static int gAllocatedStrokes = 0;
static bool gLiveActive = false;
static StrokeMetaCPU gLiveMeta;
static StrokeBoundsCPU gLiveBounds{0.0f, 0.0f, 0.0f, 0.0f};
static bool gHasLiveBounds = false;
static float gLiveColor[4] = {0.1f, 0.4f, 1.0f, 0.85f};

// 当GL程序尚未就绪时暂存的笔划，待初始化完成后统一上传
struct PendingStroke {
    std::vector<float> points;   // 2*N
    std::vector<float> pressures;// N
    std::vector<float> color;    // 4
    int type = 0;
};
static std::vector<PendingStroke> gPendingStrokes;

// 计算需要的点容量（按笔划数与每条最大点数）
static size_t pointsCapacityByStrokes(size_t strokes) {
    return strokes * (size_t)kMaxPointsPerStroke;
}

static inline size_t packedPressureCount(size_t pointCount) {
    return (pointCount + 1u) / 2u;
}

static inline uint16_t floatToUnorm16(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 65535;
    return (uint16_t)(v * 65535.0f + 0.5f);
}

static inline void setPackedPressure(std::vector<uint32_t>& packed, size_t pointIndex, uint16_t p16) {
    size_t wordIndex = pointIndex >> 1;
    uint32_t cur = packed[wordIndex];
    if ((pointIndex & 1u) == 0u) {
        packed[wordIndex] = (cur & 0xFFFF0000u) | (uint32_t)p16;
    } else {
        packed[wordIndex] = (cur & 0x0000FFFFu) | ((uint32_t)p16 << 16);
    }
}

static bool ensureFallbackStorageCapacity(int requiredStrokes);

// 统一缓冲扩容：新建更大缓冲并复制旧数据，避免丢失
static GLuint resizeBufferCopy(GLenum target, GLuint oldBuf, GLsizeiptr oldSize, GLsizeiptr newSize) {
    GLuint newBuf = 0;
    glGenBuffers(1, &newBuf);
    glBindBuffer(target, newBuf);
    glBufferData(target, newSize, nullptr, GL_DYNAMIC_DRAW);
    // 复制旧数据
    glBindBuffer(GL_COPY_READ_BUFFER, oldBuf);
    glBindBuffer(GL_COPY_WRITE_BUFFER, newBuf);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, oldSize);
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    // 释放旧缓冲
    glDeleteBuffers(1, &oldBuf);
    return newBuf;
}

// 确保缓冲容量足够容纳所需笔划数；按倍增策略扩容并复制内容
static void ensureCapacityForStrokes(size_t requiredStrokes) {
    if (requiredStrokes <= (size_t)gAllocatedStrokes) return;
    size_t newAlloc = (size_t)gAllocatedStrokes;
    while (newAlloc < requiredStrokes) {
        newAlloc = newAlloc < 16384 ? newAlloc * 2 : (size_t)(newAlloc * 1.5);
    }

    if (!gUseSSBO) {
        if (ensureFallbackStorageCapacity((int)newAlloc)) {
            gAllocatedStrokes = (int)newAlloc;
        }
        return;
    }

    size_t oldPointsCap = pointsCapacityByStrokes(gAllocatedStrokes);
    size_t newPointsCap = pointsCapacityByStrokes(newAlloc);

    // 扩容 Positions SSBO
    if (gUseSSBO && gPositionsSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPositionsSSBO);
        gPositionsSSBO = resizeBufferCopy(GL_SHADER_STORAGE_BUFFER,
                                          gPositionsSSBO,
                                          (GLsizeiptr)(oldPointsCap * sizeof(float) * 2),
                                          (GLsizeiptr)(newPointsCap * sizeof(float) * 2));
    }
    // 扩容 Pressures SSBO
    if (gUseSSBO && gPressuresSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPressuresSSBO);
        size_t oldWords = packedPressureCount(oldPointsCap);
        size_t newWords = packedPressureCount(newPointsCap);
        gPressuresSSBO = resizeBufferCopy(GL_SHADER_STORAGE_BUFFER,
                                          gPressuresSSBO,
                                          (GLsizeiptr)(oldWords * sizeof(uint32_t)),
                                          (GLsizeiptr)(newWords * sizeof(uint32_t)));
    }
    // 扩容 VBO: half 或 float 三分量
    if (gPointsBuffer) {
        glBindBuffer(GL_ARRAY_BUFFER, gPointsBuffer);
        size_t oldSize = oldPointsCap * (gHasVertexHalfFloat ? sizeof(uint16_t) * 3 : sizeof(float) * 3);
        size_t newSize = newPointsCap * (gHasVertexHalfFloat ? sizeof(uint16_t) * 3 : sizeof(float) * 3);
        gPointsBuffer = resizeBufferCopy(GL_ARRAY_BUFFER, gPointsBuffer, (GLsizeiptr)oldSize, (GLsizeiptr)newSize);
        // 重新声明顶点属性（新缓冲绑定后保持一致）
        glBindVertexArray(gVAO);
        glBindBuffer(GL_ARRAY_BUFFER, gPointsBuffer);
        glEnableVertexAttribArray(0);
        if (gHasVertexHalfFloat) {
            glVertexAttribPointer(0, 3, GL_HALF_FLOAT, GL_FALSE, sizeof(uint16_t) * 3, (const void*)0);
        } else {
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, (const void*)0);
        }
    }
    // 扩容 StrokeMeta SSBO（按 CPU 侧结构体大小）
    if (gUseSSBO && gStrokeMetaSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gStrokeMetaSSBO);
        gStrokeMetaSSBO = resizeBufferCopy(GL_SHADER_STORAGE_BUFFER,
                                           gStrokeMetaSSBO,
                                           (GLsizeiptr)((size_t)gAllocatedStrokes * sizeof(StrokeMetaCPU)),
                                           (GLsizeiptr)(newAlloc * sizeof(StrokeMetaCPU)));
    }
    if (gUseSSBO && gVisibleIndexSSBO) {
        int oldCap = gVisibleIndexCapacity;
        int newCap = (int)newAlloc + 1;
        if (oldCap < 1) oldCap = gAllocatedStrokes + 1;
        if (newCap < 1) newCap = 1;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gVisibleIndexSSBO);
        gVisibleIndexSSBO = resizeBufferCopy(GL_SHADER_STORAGE_BUFFER,
                                             gVisibleIndexSSBO,
                                             (GLsizeiptr)((size_t)oldCap * sizeof(uint32_t) * 2u),
                                             (GLsizeiptr)((size_t)newCap * sizeof(uint32_t) * 2u));
        gVisibleIndexCapacity = newCap;
    }

    gAllocatedStrokes = (int)newAlloc;
    LOGI("Buffers grown: strokes=%d pointsCap=%zu", gAllocatedStrokes, newPointsCap);
}

// 浮点转半浮点（简化版本，仅用于顶点数据）
static inline uint16_t floatToHalf(float f) {
    union { float f; uint32_t u; } v{f};
    uint32_t x = v.u;
    uint32_t sign = (x >> 16) & 0x8000;
    uint32_t mantissa = x & 0x007FFFFF;
    int exp = ((x >> 23) & 0xFF) - 127 + 15;
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        mantissa = (mantissa | 0x00800000) >> (1 - exp);
        return (uint16_t)(sign | (mantissa >> 13));
    } else if (exp >= 31) {
        return (uint16_t)(sign | 0x7C00);
    }
    return (uint16_t)(sign | (exp << 10) | (mantissa >> 13));
}

static bool loadEglImageProcsIfNeeded() {
    if (gEglCreateImageKHR && gEglDestroyImageKHR && gGlEGLImageTargetTexture2DOES && gEglGetNativeClientBufferANDROID) return true;
    gEglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    gEglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    gGlEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    gEglGetNativeClientBufferANDROID = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    return gEglCreateImageKHR && gEglDestroyImageKHR && gGlEGLImageTargetTexture2DOES && gEglGetNativeClientBufferANDROID;
}

static bool loadEglSyncProcsIfNeeded() {
    if (gEglCreateSyncKHR && gEglDestroySyncKHR && (gEglWaitSyncKHR || gEglClientWaitSyncKHR)) return true;
    gEglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    gEglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    gEglWaitSyncKHR = (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
    gEglClientWaitSyncKHR = (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
    return gEglCreateSyncKHR && gEglDestroySyncKHR && (gEglWaitSyncKHR || gEglClientWaitSyncKHR);
}

static void closeFenceFd(int* fd) {
    if (!fd) return;
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void waitForFallbackGpuIdleIfNeeded() {
    if (!gFallbackGpuSync) return;
    GLenum r = glClientWaitSync(gFallbackGpuSync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
    if (r == GL_TIMEOUT_EXPIRED) {
        glWaitSync(gFallbackGpuSync, 0, GL_TIMEOUT_IGNORED);
    }
    glDeleteSync(gFallbackGpuSync);
    gFallbackGpuSync = nullptr;
}

static void insertGpuWaitOnNativeFenceFd(int* fd) {
    if (!fd || *fd < 0) return;
    EGLDisplay dpy = eglGetCurrentDisplay();
    if (dpy == EGL_NO_DISPLAY) {
        closeFenceFd(fd);
        return;
    }
    if (!loadEglSyncProcsIfNeeded()) {
        closeFenceFd(fd);
        return;
    }
    const EGLint attrs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, *fd, EGL_NONE};
    EGLSyncKHR sync = gEglCreateSyncKHR(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
    closeFenceFd(fd);
    if (sync == EGL_NO_SYNC_KHR) {
        if (gFallbackFenceLogBudget.fetch_sub(1) > 0) {
            LOGE("FallbackFence: eglCreateSyncKHR failed err=0x%x", eglGetError());
        }
        return;
    }
    if (gEglWaitSyncKHR) {
        gEglWaitSyncKHR(dpy, sync, 0);
    } else if (gEglClientWaitSyncKHR) {
        gEglClientWaitSyncKHR(dpy, sync, 0, EGL_FOREVER_KHR);
    }
    gEglDestroySyncKHR(dpy, sync);
    if (gFallbackFenceLogBudget.fetch_sub(1) > 0) {
        LOGI("FallbackFence: inserted GPU wait");
    }
}

static void destroyFallbackStorage() {
    EGLDisplay dpy = eglGetCurrentDisplay();
    if (dpy != EGL_NO_DISPLAY && gEglDestroyImageKHR) {
        if (gDataImage != EGL_NO_IMAGE_KHR) {
            gEglDestroyImageKHR(dpy, gDataImage);
            gDataImage = EGL_NO_IMAGE_KHR;
        }
        if (gMetaBWCImage != EGL_NO_IMAGE_KHR) {
            gEglDestroyImageKHR(dpy, gMetaBWCImage);
            gMetaBWCImage = EGL_NO_IMAGE_KHR;
        }
        if (gMetaColorImage != EGL_NO_IMAGE_KHR) {
            gEglDestroyImageKHR(dpy, gMetaColorImage);
            gMetaColorImage = EGL_NO_IMAGE_KHR;
        }
    } else {
        gDataImage = EGL_NO_IMAGE_KHR;
        gMetaBWCImage = EGL_NO_IMAGE_KHR;
        gMetaColorImage = EGL_NO_IMAGE_KHR;
    }

    if (gDataTex) {
        glDeleteTextures(1, &gDataTex);
        gDataTex = 0;
    }
    if (gMetaBWCTex) {
        glDeleteTextures(1, &gMetaBWCTex);
        gMetaBWCTex = 0;
    }
    if (gMetaColorTex) {
        glDeleteTextures(1, &gMetaColorTex);
        gMetaColorTex = 0;
    }

    if (gDataAHB) {
        AHardwareBuffer_release(gDataAHB);
        gDataAHB = nullptr;
    }
    if (gMetaBWCAHB) {
        AHardwareBuffer_release(gMetaBWCAHB);
        gMetaBWCAHB = nullptr;
    }
    if (gMetaColorAHB) {
        AHardwareBuffer_release(gMetaColorAHB);
        gMetaColorAHB = nullptr;
    }

    closeFenceFd(&gDataWriteFenceFd);
    closeFenceFd(&gMetaBWCWriteFenceFd);
    closeFenceFd(&gMetaColorWriteFenceFd);
    if (gFallbackGpuSync) {
        glDeleteSync(gFallbackGpuSync);
        gFallbackGpuSync = nullptr;
    }

    gFallbackCapacityStrokes = 0;
}

static bool allocateAndBindOneBuffer2D(AHardwareBuffer** outAhb,
                                      EGLImageKHR* outImage,
                                      GLuint* outTex,
                                      int width,
                                      int height,
                                      uint32_t format) {
    if (!outAhb || !outImage || !outTex) return false;
    *outAhb = nullptr;
    *outImage = EGL_NO_IMAGE_KHR;
    *outTex = 0;

    AHardwareBuffer_Desc desc{};
    desc.width = (uint32_t)width;
    desc.height = (uint32_t)height;
    desc.layers = 1;
    desc.format = format;
    desc.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

    int res = AHardwareBuffer_allocate(&desc, outAhb);
    if (res != 0 || !(*outAhb)) {
        if (gFallbackAllocLogBudget.fetch_sub(1) > 0) {
            LOGE("FallbackAlloc: AHardwareBuffer_allocate failed res=%d w=%d h=%d fmt=%u", res, width, height, format);
        }
        return false;
    }

    EGLDisplay dpy = eglGetCurrentDisplay();
    if (dpy == EGL_NO_DISPLAY) {
        if (gFallbackAllocLogBudget.fetch_sub(1) > 0) {
            LOGE("FallbackAlloc: eglGetCurrentDisplay failed");
        }
        return false;
    }
    if (!loadEglImageProcsIfNeeded()) {
        if (gFallbackAllocLogBudget.fetch_sub(1) > 0) {
            LOGE("FallbackAlloc: loadEglImageProcsIfNeeded failed (create=%p destroy=%p target=%p nativeBuf=%p)",
                 (void*)gEglCreateImageKHR, (void*)gEglDestroyImageKHR, (void*)gGlEGLImageTargetTexture2DOES, (void*)gEglGetNativeClientBufferANDROID);
        }
        return false;
    }

    EGLClientBuffer clientBuf = gEglGetNativeClientBufferANDROID(*outAhb);
    const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    *outImage = gEglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, clientBuf, attrs);
    if (*outImage == EGL_NO_IMAGE_KHR) {
        if (gFallbackAllocLogBudget.fetch_sub(1) > 0) {
            LOGE("FallbackAlloc: eglCreateImageKHR failed err=0x%x", eglGetError());
        }
        return false;
    }

    glGenTextures(1, outTex);
    glBindTexture(GL_TEXTURE_2D, *outTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gGlEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)(*outImage));
    if (gFallbackAllocLogBudget.fetch_sub(1) > 0) {
        GLenum ge = glGetError();
        AHardwareBuffer_Desc outDesc{};
        AHardwareBuffer_describe(*outAhb, &outDesc);
        LOGW("FallbackAlloc: tex=%u w=%u h=%u stride=%u fmt=%u usage=0x%llx glErr=0x%x",
             (unsigned)(*outTex),
             outDesc.width, outDesc.height, outDesc.stride, outDesc.format, (unsigned long long)outDesc.usage,
             (unsigned)ge);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

static bool ensureFallbackStorageCapacity(int requiredStrokes) {
    if (requiredStrokes <= gFallbackCapacityStrokes && gDataAHB && gMetaBWCAHB && gMetaColorAHB && gDataTex && gMetaBWCTex && gMetaColorTex) {
        return true;
    }

    if (gFallbackAllocLogBudget.fetch_sub(1) > 0) {
        LOGW("FallbackAlloc: ensure capacity from %d to %d", gFallbackCapacityStrokes, requiredStrokes);
    }
    if (requiredStrokes < 1) requiredStrokes = 1;

    int oldCap = gFallbackCapacityStrokes;
    int newCap = oldCap > 0 ? oldCap : 1;
    while (newCap < requiredStrokes) {
        newCap = newCap < 16384 ? (newCap * 2) : (int)(newCap * 1.5f);
    }

    AHardwareBuffer* newDataAHB = nullptr;
    AHardwareBuffer* newMetaBWCAHB = nullptr;
    AHardwareBuffer* newMetaColorAHB = nullptr;
    EGLImageKHR newDataImage = EGL_NO_IMAGE_KHR;
    EGLImageKHR newMetaBWCImage = EGL_NO_IMAGE_KHR;
    EGLImageKHR newMetaColorImage = EGL_NO_IMAGE_KHR;
    GLuint newDataTex = 0;
    GLuint newMetaBWCTex = 0;
    GLuint newMetaColorTex = 0;

    bool ok0 = allocateAndBindOneBuffer2D(&newDataAHB, &newDataImage, &newDataTex, kMaxPointsPerStroke, newCap, AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT);
    bool ok1 = allocateAndBindOneBuffer2D(&newMetaBWCAHB, &newMetaBWCImage, &newMetaBWCTex, 2, newCap, AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT);
    bool ok2 = allocateAndBindOneBuffer2D(&newMetaColorAHB, &newMetaColorImage, &newMetaColorTex, 1, newCap, AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT);

    if (!ok0 || !ok1 || !ok2) {
        EGLDisplay dpy = eglGetCurrentDisplay();
        if (dpy != EGL_NO_DISPLAY && gEglDestroyImageKHR) {
            if (newDataImage != EGL_NO_IMAGE_KHR) gEglDestroyImageKHR(dpy, newDataImage);
            if (newMetaBWCImage != EGL_NO_IMAGE_KHR) gEglDestroyImageKHR(dpy, newMetaBWCImage);
            if (newMetaColorImage != EGL_NO_IMAGE_KHR) gEglDestroyImageKHR(dpy, newMetaColorImage);
        }
        if (newDataTex) glDeleteTextures(1, &newDataTex);
        if (newMetaBWCTex) glDeleteTextures(1, &newMetaBWCTex);
        if (newMetaColorTex) glDeleteTextures(1, &newMetaColorTex);
        if (newDataAHB) AHardwareBuffer_release(newDataAHB);
        if (newMetaBWCAHB) AHardwareBuffer_release(newMetaBWCAHB);
        if (newMetaColorAHB) AHardwareBuffer_release(newMetaColorAHB);
        return false;
    }

    int copyStrokes = 0;
    if (oldCap > 0 && gDataAHB && gMetaBWCAHB && gMetaColorAHB) {
        waitForFallbackGpuIdleIfNeeded();
        copyStrokes = std::min(oldCap, newCap);

        void* srcData = nullptr;
        void* dstData = nullptr;
        if (AHardwareBuffer_lock(gDataAHB, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &srcData) == 0 && srcData &&
            AHardwareBuffer_lock(newDataAHB, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &dstData) == 0 && dstData) {
            AHardwareBuffer_Desc srcDesc{};
            AHardwareBuffer_Desc dstDesc{};
            AHardwareBuffer_describe(gDataAHB, &srcDesc);
            AHardwareBuffer_describe(newDataAHB, &dstDesc);
            uint32_t srcStride = srcDesc.stride > 0 ? srcDesc.stride : (uint32_t)kMaxPointsPerStroke;
            uint32_t dstStride = dstDesc.stride > 0 ? dstDesc.stride : (uint32_t)kMaxPointsPerStroke;
            uint16_t* src = (uint16_t*)srcData;
            uint16_t* dst = (uint16_t*)dstData;
            size_t rowElems = (size_t)kMaxPointsPerStroke * 4u;
            size_t rowBytes = rowElems * sizeof(uint16_t);
            for (int s = 0; s < copyStrokes; ++s) {
                uint16_t* srcRow = src + (size_t)s * (size_t)srcStride * 4u;
                uint16_t* dstRow = dst + (size_t)s * (size_t)dstStride * 4u;
                memcpy(dstRow, srcRow, rowBytes);
            }
        }
        AHardwareBuffer_unlock(gDataAHB, nullptr);
        AHardwareBuffer_unlock(newDataAHB, nullptr);

        void* srcBWC = nullptr;
        void* dstBWC = nullptr;
        if (AHardwareBuffer_lock(gMetaBWCAHB, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &srcBWC) == 0 && srcBWC &&
            AHardwareBuffer_lock(newMetaBWCAHB, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &dstBWC) == 0 && dstBWC) {
            AHardwareBuffer_Desc srcDesc{};
            AHardwareBuffer_Desc dstDesc{};
            AHardwareBuffer_describe(gMetaBWCAHB, &srcDesc);
            AHardwareBuffer_describe(newMetaBWCAHB, &dstDesc);
            uint32_t srcStride = srcDesc.stride > 0 ? srcDesc.stride : 2u;
            uint32_t dstStride = dstDesc.stride > 0 ? dstDesc.stride : 2u;
            uint16_t* src = (uint16_t*)srcBWC;
            uint16_t* dst = (uint16_t*)dstBWC;
            for (int s = 0; s < copyStrokes; ++s) {
                uint16_t* srcPx = src + (size_t)s * (size_t)srcStride * 4u;
                uint16_t* dstPx = dst + (size_t)s * (size_t)dstStride * 4u;
                memcpy(dstPx, srcPx, sizeof(uint16_t) * 8u);
            }
        }
        AHardwareBuffer_unlock(gMetaBWCAHB, nullptr);
        AHardwareBuffer_unlock(newMetaBWCAHB, nullptr);

        void* srcColor = nullptr;
        void* dstColor = nullptr;
        if (AHardwareBuffer_lock(gMetaColorAHB, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &srcColor) == 0 && srcColor &&
            AHardwareBuffer_lock(newMetaColorAHB, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &dstColor) == 0 && dstColor) {
            AHardwareBuffer_Desc srcDesc{};
            AHardwareBuffer_Desc dstDesc{};
            AHardwareBuffer_describe(gMetaColorAHB, &srcDesc);
            AHardwareBuffer_describe(newMetaColorAHB, &dstDesc);
            uint32_t srcStride = srcDesc.stride > 0 ? srcDesc.stride : 1u;
            uint32_t dstStride = dstDesc.stride > 0 ? dstDesc.stride : 1u;
            uint16_t* src = (uint16_t*)srcColor;
            uint16_t* dst = (uint16_t*)dstColor;
            for (int s = 0; s < copyStrokes; ++s) {
                uint16_t* srcPx = src + (size_t)s * (size_t)srcStride * 4u;
                uint16_t* dstPx = dst + (size_t)s * (size_t)dstStride * 4u;
                dstPx[0] = srcPx[0];
                dstPx[1] = srcPx[1];
                dstPx[2] = srcPx[2];
                dstPx[3] = srcPx[3];
            }
        }
        AHardwareBuffer_unlock(gMetaColorAHB, nullptr);
        AHardwareBuffer_unlock(newMetaColorAHB, nullptr);
    }

    destroyFallbackStorage();
    gDataAHB = newDataAHB;
    gMetaBWCAHB = newMetaBWCAHB;
    gMetaColorAHB = newMetaColorAHB;
    gDataImage = newDataImage;
    gMetaBWCImage = newMetaBWCImage;
    gMetaColorImage = newMetaColorImage;
    gDataTex = newDataTex;
    gMetaBWCTex = newMetaBWCTex;
    gMetaColorTex = newMetaColorTex;
    gFallbackCapacityStrokes = newCap;
    if (copyStrokes > 0 && gFallbackAllocLogBudget.fetch_sub(1) > 0) {
        LOGW("FallbackAlloc: grown to %d, preserved=%d", newCap, copyStrokes);
    }
    if (gFallbackAllocLogBudget.fetch_sub(1) > 0) {
        LOGW("FallbackAlloc: capacity=%d dataTex=%u metaBWCTex=%u metaColorTex=%u",
             gFallbackCapacityStrokes, (unsigned)gDataTex, (unsigned)gMetaBWCTex, (unsigned)gMetaColorTex);
    }
    return true;
}

static bool writeFallbackMeta(int strokeId,
                              int count,
                              float baseWidth,
                              float effect,
                              float type,
                              const float color[4],
                              float boundsMinX,
                              float boundsMinY,
                              float boundsSpanX,
                              float boundsSpanY) {
    if (!gMetaBWCAHB || !gMetaColorAHB) return false;
    if (strokeId < 0) return false;
    if (strokeId >= gFallbackCapacityStrokes) return false;

    waitForFallbackGpuIdleIfNeeded();

    void* mbwcPtr = nullptr;
    if (AHardwareBuffer_lock(gMetaBWCAHB, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &mbwcPtr) != 0 || !mbwcPtr) {
        return false;
    }
    AHardwareBuffer_Desc mbwcDesc{};
    AHardwareBuffer_describe(gMetaBWCAHB, &mbwcDesc);
    uint16_t* mbwcBase = (uint16_t*)mbwcPtr;
    uint32_t stride = mbwcDesc.stride > 0 ? mbwcDesc.stride : 2u;
    uint16_t* px = mbwcBase + (size_t)strokeId * (size_t)stride * 4u;
    px[0] = floatToHalf((float)count);
    px[1] = floatToHalf(baseWidth);
    px[2] = floatToHalf(effect);
    px[3] = floatToHalf(type);
    px[4] = floatToHalf(boundsMinX);
    px[5] = floatToHalf(boundsMinY);
    px[6] = floatToHalf(boundsSpanX);
    px[7] = floatToHalf(boundsSpanY);
    int fence0 = -1;
    AHardwareBuffer_unlock(gMetaBWCAHB, &fence0);
    closeFenceFd(&gMetaBWCWriteFenceFd);
    gMetaBWCWriteFenceFd = fence0;

    void* mcPtr = nullptr;
    if (AHardwareBuffer_lock(gMetaColorAHB, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &mcPtr) != 0 || !mcPtr) {
        return false;
    }
    AHardwareBuffer_Desc mcDesc{};
    AHardwareBuffer_describe(gMetaColorAHB, &mcDesc);
    uint16_t* mcBase = (uint16_t*)mcPtr;
    uint32_t mcStride = mcDesc.stride > 0 ? mcDesc.stride : 1u;
    uint16_t* pc = mcBase + (size_t)strokeId * (size_t)mcStride * 4u;
    pc[0] = floatToHalf(color[0]);
    pc[1] = floatToHalf(color[1]);
    pc[2] = floatToHalf(color[2]);
    pc[3] = floatToHalf(color[3]);
    int fence1 = -1;
    AHardwareBuffer_unlock(gMetaColorAHB, &fence1);
    closeFenceFd(&gMetaColorWriteFenceFd);
    gMetaColorWriteFenceFd = fence1;
    if (gFallbackWriteLogBudget.fetch_sub(1) > 0) {
        LOGI("FallbackWriteMeta: id=%d count=%d baseWidth=%.3f effect=%.3f color=(%.3f,%.3f,%.3f,%.3f) fenceBWC=%d fenceColor=%d",
             strokeId, count, baseWidth, effect, color[0], color[1], color[2], color[3], gMetaBWCWriteFenceFd, gMetaColorWriteFenceFd);
    }
    return true;
}

static bool writeFallbackPoints(int strokeId,
                                const float* pointsXY,
                                const float* pressures,
                                int count,
                                float boundsMinX,
                                float boundsMinY,
                                float boundsSpanX,
                                float boundsSpanY) {
    if (!gDataAHB) return false;
    if (!pointsXY || !pressures) return false;
    if (strokeId < 0) return false;
    if (strokeId >= gFallbackCapacityStrokes) return false;
    int N = count;
    if (N < 0) N = 0;
    if (N > kMaxPointsPerStroke) N = kMaxPointsPerStroke;

    waitForFallbackGpuIdleIfNeeded();

    void* dataPtr = nullptr;
    if (AHardwareBuffer_lock(gDataAHB, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &dataPtr) != 0 || !dataPtr) {
        return false;
    }
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(gDataAHB, &desc);
    uint32_t stride = desc.stride > 0 ? desc.stride : (uint32_t)kMaxPointsPerStroke;
    uint16_t* base = (uint16_t*)dataPtr;
    uint16_t* row = base + (size_t)strokeId * (size_t)stride * 4u;
    float invSpanX = (boundsSpanX > 0.0f) ? (1.0f / boundsSpanX) : 0.0f;
    float invSpanY = (boundsSpanY > 0.0f) ? (1.0f / boundsSpanY) : 0.0f;
    for (int i = 0; i < N; ++i) {
        float x = pointsXY[(size_t)i * 2u + 0u];
        float y = pointsXY[(size_t)i * 2u + 1u];
        float p = pressures[(size_t)i];
        uint16_t* pix = row + (size_t)i * 4u;
        float xn = (x - boundsMinX) * invSpanX;
        float yn = (y - boundsMinY) * invSpanY;
        if (!(xn >= 0.0f)) xn = 0.0f;
        if (!(yn >= 0.0f)) yn = 0.0f;
        if (xn > 1.0f) xn = 1.0f;
        if (yn > 1.0f) yn = 1.0f;
        pix[0] = floatToHalf(xn);
        pix[1] = floatToHalf(yn);
        pix[2] = floatToHalf(p);
        pix[3] = floatToHalf(0.0f);
    }
    int fence = -1;
    AHardwareBuffer_unlock(gDataAHB, &fence);
    closeFenceFd(&gDataWriteFenceFd);
    gDataWriteFenceFd = fence;
    if (gFallbackWriteLogBudget.fetch_sub(1) > 0) {
        float fx = (N > 0) ? pointsXY[0] : 0.0f;
        float fy = (N > 0) ? pointsXY[1] : 0.0f;
        float lx = (N > 0) ? pointsXY[(size_t)(N - 1) * 2u + 0u] : 0.0f;
        float ly = (N > 0) ? pointsXY[(size_t)(N - 1) * 2u + 1u] : 0.0f;
        LOGI("FallbackWritePoints: id=%d count=%d stride=%u first=(%.1f,%.1f) last=(%.1f,%.1f) fenceData=%d",
             strokeId, N, (unsigned)stride, fx, fy, lx, ly, gDataWriteFenceFd);
    }
    return true;
}

// 着色器工具
static GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string log(len, '\0');
        glGetShaderInfoLog(s, len, nullptr, log.data());
        LOGE("Shader compile error: %s", log.c_str());
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint linkProgram2(GLuint vs, GLuint fs) {
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::string log(len, '\0');
        glGetProgramInfoLog(p, len, nullptr, log.data());
        LOGE("Program link error: %s", log.c_str());
    }
    glDetachShader(p, vs);
    glDetachShader(p, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static StrokeBoundsCPU computeBoundsFromPoints(const float* pts, int n) {
    StrokeBoundsCPU b{0.0f, 0.0f, 0.0f, 0.0f};
    if (!pts || n <= 0) return b;
    float minX = pts[0];
    float maxX = pts[0];
    float minY = pts[1];
    float maxY = pts[1];
    for (int i = 1; i < n; ++i) {
        float x = pts[i * 2 + 0];
        float y = pts[i * 2 + 1];
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
    }
    b.minX = minX;
    b.minY = minY;
    b.maxX = maxX;
    b.maxY = maxY;
    return b;
}

static int computeLodPointsFromScreenExtent(float extentPixels, int count) {
    if (count <= 0) return 0;
    int c = std::min(count, 1024);
    if (c <= 16) return c;
    float stepPx = 2.0f;
    int lod = (int)std::ceil(extentPixels / stepPx) + 2;
    if (lod < 16) lod = 16;
    if (lod > c) lod = c;
    return lod;
}

static void ensureVisibleIndexCapacity(int required) {
    if (!gUseSSBO || !gVisibleIndexSSBO) return;
    if (required <= 0) return;
    if (required <= gVisibleIndexCapacity) return;
    int newCap = gVisibleIndexCapacity > 0 ? gVisibleIndexCapacity : 1;
    while (newCap < required) {
        newCap = newCap < 16384 ? newCap * 2 : (int)(newCap * 1.5f);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, gVisibleIndexSSBO);
    if (gVisibleIndexCapacity <= 0) {
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)((size_t)newCap * sizeof(uint32_t) * 2u), nullptr, GL_DYNAMIC_DRAW);
    } else {
        gVisibleIndexSSBO = resizeBufferCopy(GL_SHADER_STORAGE_BUFFER,
                                             gVisibleIndexSSBO,
                                             (GLsizeiptr)((size_t)gVisibleIndexCapacity * sizeof(uint32_t) * 2u),
                                             (GLsizeiptr)((size_t)newCap * sizeof(uint32_t) * 2u));
    }
    gVisibleIndexCapacity = newCap;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, gVisibleIndexSSBO);
}

static int computeBaseProgressBudget() {
    return gIsInteracting.load() != 0 ? 800 : 2000;
}

static void resetProgress() {
    gProgressCount.store(computeBaseProgressBudget());
}

static void updateVisibleListIfNeeded() {
    if (!gUseSSBO || !gVisibleIndexSSBO) return;
    if (gVisibleDirty.load() == 0) return;

    int committed = (int)gMetas.size();
    int total = committed + (gLiveActive ? 1 : 0);
    if (total <= 0) {
        gVisiblePackedCPU.clear();
        gVisibleCount = 0;
        gVisibleDirty.store(0);
        return;
    }

    ensureVisibleIndexCapacity(total);
    struct VisibleItem {
        uint32_t strokeId;
        uint32_t lod;
        float score;
    };
    std::vector<VisibleItem> items;
    items.reserve((size_t)total);

    float w = (float)g_Width;
    float h = (float)g_Height;
    float pad = 24.0f;
    if (w <= 0.0f || h <= 0.0f) {
        int globalMax = std::clamp(gRenderMaxPoints.load(), 1, 1024);
        for (int i = 0; i < committed; ++i) {
            int count = gMetas[(size_t)i].count;
            if (count <= 0) continue;
            uint32_t lod = (uint32_t)std::min(std::min(count, 1024), globalMax);
            items.push_back(VisibleItem{(uint32_t)i, lod, 0.0f});
        }
        if (gLiveActive && gLiveMeta.count > 0) {
            uint32_t lod = (uint32_t)std::min(std::min(gLiveMeta.count, 1024), globalMax);
            items.push_back(VisibleItem{(uint32_t)committed, lod, 0.0f});
        }
    } else {
        int boundsN = (int)gBounds.size();
        int n = std::min(committed, boundsN);
        for (int i = 0; i < n; ++i) {
            if (gMetas[(size_t)i].count <= 0) continue;
            const StrokeBoundsCPU& b = gBounds[(size_t)i];
            float minX = b.minX * gViewScale + gViewTranslateX;
            float maxX = b.maxX * gViewScale + gViewTranslateX;
            float minY = b.minY * gViewScale + gViewTranslateY;
            float maxY = b.maxY * gViewScale + gViewTranslateY;
            if (maxX + pad < 0.0f) continue;
            if (minX - pad > w) continue;
            if (maxY + pad < 0.0f) continue;
            if (minY - pad > h) continue;
            float dx = std::max(0.0f, maxX - minX);
            float dy = std::max(0.0f, maxY - minY);
            float extent = std::sqrt(dx * dx + dy * dy);
            int lodI = computeLodPointsFromScreenExtent(extent, gMetas[(size_t)i].count);
            if (lodI <= 0) continue;
            float cx = (minX + maxX) * 0.5f;
            float cy = (minY + maxY) * 0.5f;
            float dcx = cx - w * 0.5f;
            float dcy = cy - h * 0.5f;
            float dist = std::sqrt(dcx * dcx + dcy * dcy);
            float score = extent / (dist + 1.0f);
            items.push_back(VisibleItem{(uint32_t)i, (uint32_t)lodI, score});
        }
        for (int i = n; i < committed; ++i) {
            if (gMetas[(size_t)i].count <= 0) continue;
            int lodI = std::min(gMetas[(size_t)i].count, 1024);
            items.push_back(VisibleItem{(uint32_t)i, (uint32_t)lodI, 0.0f});
        }
        if (gLiveActive) {
            bool vis = true;
            int lodLive = std::min(gLiveMeta.count, 1024);
            float score = 0.0f;
            if (gHasLiveBounds) {
                const StrokeBoundsCPU& b = gLiveBounds;
                float minX = b.minX * gViewScale + gViewTranslateX;
                float maxX = b.maxX * gViewScale + gViewTranslateX;
                float minY = b.minY * gViewScale + gViewTranslateY;
                float maxY = b.maxY * gViewScale + gViewTranslateY;
                if (maxX + pad < 0.0f) vis = false;
                if (minX - pad > w) vis = false;
                if (maxY + pad < 0.0f) vis = false;
                if (minY - pad > h) vis = false;
                float dx = std::max(0.0f, maxX - minX);
                float dy = std::max(0.0f, maxY - minY);
                float extent = std::sqrt(dx * dx + dy * dy);
                lodLive = computeLodPointsFromScreenExtent(extent, gLiveMeta.count);
                float cx = (minX + maxX) * 0.5f;
                float cy = (minY + maxY) * 0.5f;
                float dcx = cx - w * 0.5f;
                float dcy = cy - h * 0.5f;
                float dist = std::sqrt(dcx * dcx + dcy * dcy);
                score = extent / (dist + 1.0f);
            }
            if (vis && lodLive > 0) {
                items.push_back(VisibleItem{(uint32_t)committed, (uint32_t)lodLive, score});
            }
        }
    }

    std::sort(items.begin(), items.end(), [](const VisibleItem& a, const VisibleItem& b) {
        return a.strokeId < b.strokeId;
    });

    gVisiblePackedCPU.clear();
    gVisiblePackedCPU.reserve(items.size() * 2u);
    for (const auto& it : items) {
        gVisiblePackedCPU.push_back(it.strokeId);
        gVisiblePackedCPU.push_back(it.lod);
    }

    gVisibleCount = (int)items.size();
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, gVisibleIndexSSBO);
    if (gVisibleCount > 0) {
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)((size_t)gVisibleCount * sizeof(uint32_t) * 2u), gVisiblePackedCPU.data());
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, gVisibleIndexSSBO);
    gVisibleDirty.store(0);
    resetProgress();
}

// 将一条笔划上传到GPU缓冲，并更新CPU侧元数据
static void uploadStroke(const std::vector<float>& pts,
                         const std::vector<float>& prs,
                         const std::vector<float>& col,
                         int type) {
    if (pts.empty() || prs.empty() || col.size() < 4) return;
    int N = (int)prs.size();
    if ((int)pts.size() < N * 2) return;
    if (N > kMaxPointsPerStroke) N = kMaxPointsPerStroke;

    if (!gUseSSBO) {
        int strokeId = gFallbackStrokeCount.fetch_add(1);
        if (strokeId < 0) strokeId = 0;
        if (!ensureFallbackStorageCapacity(strokeId + 1)) {
            LOGE("Fallback: ensure storage failed, strokeId=%d", strokeId);
            return;
        }
        StrokeBoundsCPU b = computeBoundsFromPoints(pts.data(), N);
        float spanX = b.maxX - b.minX;
        float spanY = b.maxY - b.minY;
        float c[4] = {col[0], col[1], col[2], col[3]};
        if (!writeFallbackPoints(strokeId, pts.data(), prs.data(), N, b.minX, b.minY, spanX, spanY)) {
            LOGE("Fallback: write points failed, strokeId=%d", strokeId);
            return;
        }
        if (!writeFallbackMeta(strokeId, N, 16.0f, 0.0f, (float)type, c, b.minX, b.minY, spanX, spanY)) {
            LOGE("Fallback: write meta failed, strokeId=%d", strokeId);
            return;
        }
        return;
    }

    int strokeId = (int)gMetas.size();
    int neededStrokes = strokeId + 1;
    if (neededStrokes > gAllocatedStrokes) {
        int newCap = gAllocatedStrokes;
        while (newCap < neededStrokes) newCap *= 2;
        size_t pointsCapacity = (size_t)newCap * kMaxPointsPerStroke;

        if (gUseSSBO) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPositionsSSBO);
            glBufferData(GL_SHADER_STORAGE_BUFFER, pointsCapacity * sizeof(float) * 2, nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gPositionsSSBO);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, gStrokeMetaSSBO);
            glBufferData(GL_SHADER_STORAGE_BUFFER, newCap * sizeof(StrokeMetaCPU), nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, gStrokeMetaSSBO);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPressuresSSBO);
            glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(packedPressureCount(pointsCapacity) * sizeof(uint32_t)), nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, gPressuresSSBO);
        }
        gAllocatedStrokes = newCap;
    }

    int start = strokeId * kMaxPointsPerStroke;
    std::vector<float> posWrite(N * 2);
    for (int i = 0; i < N; ++i) {
        float x = pts[i * 2 + 0];
        float y = pts[i * 2 + 1];
        posWrite[i * 2 + 0] = x;
        posWrite[i * 2 + 1] = y;
    }

    if (gUseSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPositionsSSBO);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, (GLintptr)(start * sizeof(float) * 2), (GLsizeiptr)(posWrite.size() * sizeof(float)), posWrite.data());
        }
    if (gUseSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPressuresSSBO);
        size_t startWord = (size_t)start >> 1;
        std::vector<uint32_t> packed(packedPressureCount((size_t)N), 0u);
        for (int i = 0; i < N; ++i) {
            setPackedPressure(packed, (size_t)i, floatToUnorm16(prs[(size_t)i]));
        }
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(startWord * sizeof(uint32_t)),
                        (GLsizeiptr)(packed.size() * sizeof(uint32_t)),
                        packed.data());
    }

    StrokeMetaCPU meta;
    meta.start = start;
    meta.count = N;
    meta.baseWidth = 16.0f;
    meta.pad = 0.0f;
    meta.color[0] = col[0]; meta.color[1] = col[1]; meta.color[2] = col[2]; meta.color[3] = col[3];
    meta.type = (float)type;
    meta.reserved0 = 0.0f;
    meta.reserved1 = 0.0f;
    meta.reserved2 = 0.0f;
    gMetas.push_back(meta);
    StrokeBoundsCPU bounds = computeBoundsFromPoints(pts.data(), N);
    if ((int)gBounds.size() < strokeId) gBounds.resize((size_t)strokeId);
    gBounds.push_back(bounds);
    if (gUseSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gStrokeMetaSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, (GLintptr)(strokeId * sizeof(StrokeMetaCPU)), (GLsizeiptr)sizeof(StrokeMetaCPU), &meta);
    }
    float firstX = posWrite.size() >= 2 ? posWrite[0] : 0.0f;
    float firstY = posWrite.size() >= 2 ? posWrite[1] : 0.0f;
    float lastX = (N >= 1) ? posWrite[(N - 1) * 2] : 0.0f;
    float lastY = (N >= 1) ? posWrite[(N - 1) * 2 + 1] : 0.0f;
    if (gStrokeUploadLogBudget.fetch_sub(1) > 0) {
        LOGI("addStroke(uploaded): id=%d, count=%d type=%.0f width=%.1f color=(%.2f,%.2f,%.2f,%.2f) first=(%.1f,%.1f) last=(%.1f,%.1f)",
             strokeId, N, meta.type, meta.baseWidth, col[0], col[1], col[2], col[3], firstX, firstY, lastX, lastY);
    }
    gVisibleDirty.store(1);
}

static const char* kVS = R"(#version 310 es
// 顶点着色器（ES 3.1+ / SSBO路径）
// 目标：在一次 glDrawArraysInstanced 调用中绘制所有笔划。
//
// 绘制模型：
// - 每个实例（gl_InstanceID）代表一条笔划（stroke）。
// - 每个实例内部用一个 TRIANGLE_STRIP 生成「起始端帽 + 笔身 + 结束端帽」的三角带。
// - gl_VertexID 代表该实例内的顶点编号，用于决定当前顶点属于端帽还是笔身，以及笔身对应的采样点与左右侧。
//
// 数据来源（SSBO）：
// - binding=0: metas[]        每条笔划的元数据（起始索引、点数、基础宽度、颜色、效果标记）。
// - binding=1: positions[]    所有笔划的点坐标（像素坐标，按笔划连续存储）。
// - binding=2: pressures[]    所有点的压力（与positions[]一一对应）。
//
// 视图变换：
// - 坐标：screen = positions * uViewScale + uViewTranslate
// - 宽度：跟随视图缩放，实现缩放时笔迹粗细等比变化：
//         radius = baseWidth * pressure * 0.5 * uViewScale
//
// LOD（缩放手势期间降采样）：
// - uRenderMaxPoints 控制每条笔划参与绘制的最大采样点数。
// - 当真实点数 count 很大时，按均匀采样将 [0..count-1] 映射到 [0..uRenderMaxPoints-1]，
//   显著降低顶点数量，提升缩放期间的交互流畅度。
layout(location=0) in vec3 aStrictCheckBypass;

struct StrokeMeta {
    int start;
    int count;
    float baseWidth;
    float pad;
    vec4 color;
    vec4 extra;
};

layout(std430, binding=0) buffer StrokeMetaBuf {
    StrokeMeta metas[];
};

layout(std430, binding=1) buffer PositionsBuf { vec2 positions[]; };
layout(std430, binding=2) buffer PressuresBuf { uint pressuresPacked[]; };
layout(std430, binding=3) buffer VisibleIndexBuf { uint visiblePacked[]; };

uniform vec2 uResolution;
uniform float uViewScale;
uniform vec2 uViewTranslate;
uniform float uMaxPointSize;
uniform float uStrokeCount;
uniform float uBaseInstance;
uniform int uPass;
uniform int uRenderMaxPoints;

out highp vec4 vColor;
out highp float vEffect;
out highp float vMode;
out highp float vEdgeSigned;
out highp float vHalfWidth;
out highp vec2 vCapLocal;
out highp float vCapRadius;
out highp float vCapSign;
out highp float vType;
out highp float vSeed;

highp vec2 safeNormalize(highp vec2 v) {
    highp float l = length(v);
    if (l < 1e-4) return vec2(1.0, 0.0);
    return v / l;
}

highp float loadPressure(int globalPointIndex) {
    uint idx = uint(globalPointIndex);
    uint w = pressuresPacked[idx >> 1];
    uint v = ((idx & 1u) == 0u) ? (w & 65535u) : (w >> 16);
    return float(v) * (1.0 / 65535.0);
}

void setOffscreen() {
    gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
    vColor = vec4(0.0);
    vMode = 0.0;
    vEdgeSigned = 0.0;
    vHalfWidth = 0.0;
    vCapLocal = vec2(0.0);
    vCapRadius = 0.0;
    vCapSign = 0.0;
    vType = 0.0;
    vSeed = 0.0;
}

void setDegenerateAtScreen(vec2 anchorScreen, float zNdc) {
    vec2 ndc;
    ndc.x = (anchorScreen.x / uResolution.x) * 2.0 - 1.0;
    ndc.y = 1.0 - (anchorScreen.y / uResolution.y) * 2.0;
    gl_Position = vec4(ndc, zNdc, 1.0);
    vColor = vec4(0.0);
    vEffect = 0.0;
    vMode = 0.0;
    vEdgeSigned = 0.0;
    vHalfWidth = 0.0;
    vCapLocal = vec2(0.0);
    vCapRadius = 0.0;
    vCapSign = 0.0;
    vType = 0.0;
    vSeed = 0.0;
}

void main() {
    vec3 dummy = aStrictCheckBypass * 0.000001;
    
    int visibleIndex = gl_InstanceID + int(uBaseInstance);
    int base = visibleIndex * 2;
    int strokeId = int(visiblePacked[base + 0]);
    int lodPoints = int(visiblePacked[base + 1]);
    float strokeDenom = max(uStrokeCount, 1.0);
    float strokeNorm = (float(strokeId) + 0.5) / strokeDenom;
    float zNdc = 1.0 - 2.0 * strokeNorm;

    int start = metas[strokeId].start;
    int count = metas[strokeId].count;
    float effect = metas[strokeId].pad;
    vEffect = effect;
    vType = metas[strokeId].extra.x;
    vSeed = fract(sin(float(strokeId) * 12.9898 + 78.233) * 43758.5453);
    // 分两次渲染时，用 effect 作为笔划分组标记：
    // - uPass==0：绘制 effect<=0.5 的笔划
    // - uPass==1：绘制 effect>0.5 的笔划
    // - uPass==2：不分组，直接绘制全部
    if ((uPass == 0 && effect > 0.5) || (uPass == 1 && effect <= 0.5) || count <= 0) {
        setOffscreen();
        return;
    }

    // 基于uRenderMaxPoints动态决定本实例输出的三角带顶点数：
    // - 笔身：每个采样点输出左右两个边缘点 -> 2 * maxPoints
    // - 端帽：起点与终点各输出4个点，形成半圆/方形过渡
    int maxPoints = clamp(min(lodPoints, uRenderMaxPoints), 1, 1024);
    int kBodyVerts = maxPoints * 2;
    const int kStartCapVerts = 4;
    const int kEndCapVerts = 4;
    int kBodyStart = kStartCapVerts; // 4
    int kBodyEnd = kBodyStart + kBodyVerts; // 4 + 2048
    int kEndCapStart = kBodyEnd;
    int kTotalVerts = kBodyVerts + kStartCapVerts + kEndCapVerts; // 2048 + 8

    int lastPointIdx = max(count - 1, 0);
    vec2 p0Screen = positions[start] * uViewScale + uViewTranslate;
    vec2 p1Screen = positions[start + min(1, lastPointIdx)] * uViewScale + uViewTranslate;
    vec2 pNScreen = positions[start + lastPointIdx] * uViewScale + uViewTranslate;
    vec2 pN1Screen = positions[start + max(lastPointIdx - 1, 0)] * uViewScale + uViewTranslate;
    vec2 dirStart = safeNormalize(p1Screen - p0Screen);
    vec2 dirEnd = safeNormalize(pNScreen - pN1Screen);
    vec2 nStart = vec2(-dirStart.y, dirStart.x);
    vec2 nEnd = vec2(-dirEnd.y, dirEnd.x);
    float r0 = metas[strokeId].baseWidth * loadPressure(start) * 0.5 * uViewScale;
    float rN = metas[strokeId].baseWidth * loadPressure(start + lastPointIdx) * 0.5 * uViewScale;

    int vid = gl_VertexID;
    bool degenerateTail = false;
    if (vid < 0) {
        setOffscreen();
        return;
    }
    if (vid >= kTotalVerts) {
        vid = kTotalVerts - 1;
        degenerateTail = true;
    }

    vec2 posScreen = vec2(0.0);
    vMode = 0.0;
    vEdgeSigned = 0.0;
    vHalfWidth = 0.0;
    vCapLocal = vec2(0.0);
    vCapRadius = 0.0;
    vCapSign = 0.0;
    vColor = metas[strokeId].color;

    if (vid < kStartCapVerts) {
        // 起始端帽：以起点为中心，在笔迹方向的反向生成端部几何
        vec2 center = p0Screen;
        float r = r0;
        vec2 dir = dirStart;
        vec2 n = nStart;
        float sign = -1.0;
        float x0 = -r;
        float x1 = 0.0;
        if (count <= 1) {
            dir = vec2(1.0, 0.0);
            n = vec2(0.0, 1.0);
            sign = 0.0;
            x1 = r;
        }

        float x = (vid == 0 || vid == 1) ? x0 : x1;
        float y = (vid == 0 || vid == 2) ? -r : r;
        posScreen = center + dir * x + n * y;

        vMode = 1.0;
        vCapLocal = vec2(x, y);
        vCapRadius = r;
        vCapSign = sign;
    } else if (vid < kBodyEnd) {
        // 笔身：用采样点的法线偏移构造左右边界，转角处用miter限制避免尖刺过长
        if (count <= 1) {
            setDegenerateAtScreen(p0Screen, zNdc);
            return;
        }
        int bodyVid = vid - kBodyStart;
        int pointIdx = bodyVid >> 1;
        int side = (bodyVid & 1) == 0 ? -1 : 1;

        // 均匀采样：将 [0..maxPoints-1] 映射到 [0..lastPointIdx]
        int denom = max(maxPoints - 1, 1);
        int clampedPoint = min((pointIdx * lastPointIdx) / denom, lastPointIdx);
        int idx = start + clampedPoint;
        vec2 pCurScreen = positions[idx] * uViewScale + uViewTranslate;
        float pressure = loadPressure(idx);
        // 修复：笔身宽度也需要随视图缩放，否则会变成细线
        float radius = metas[strokeId].baseWidth * pressure * 0.5 * uViewScale;

        int prevSampleIdx = max(pointIdx - 1, 0);
        int nextSampleIdx = min(pointIdx + 1, maxPoints - 1);
        int prevPointIdx = min((prevSampleIdx * lastPointIdx) / denom, lastPointIdx);
        int nextPointIdx = min((nextSampleIdx * lastPointIdx) / denom, lastPointIdx);
        vec2 pPrevScreen = positions[start + prevPointIdx] * uViewScale + uViewTranslate;
        vec2 pNextScreen = positions[start + nextPointIdx] * uViewScale + uViewTranslate;

        vec2 dirPrev = safeNormalize(pCurScreen - pPrevScreen);
        vec2 dirNext = safeNormalize(pNextScreen - pCurScreen);

        // 修复：LOD模式下起点和终点的邻居重合导致切线计算错误
        if (pointIdx == 0) dirPrev = dirNext;
        if (pointIdx == maxPoints - 1) dirNext = dirPrev;

        vec2 nPrev = vec2(-dirPrev.y, dirPrev.x);
        vec2 nNext = vec2(-dirNext.y, dirNext.x);

        float dp = dot(dirPrev, dirNext);
        vec2 sumN = nPrev + nNext;
        float sumNL = length(sumN);
        float turn = dirPrev.x * dirNext.y - dirPrev.y * dirNext.x;

        vec2 miterN = nPrev;
        float miterLen = 1.0;
        if (sumNL > 1e-3 && dp > -0.95) {
            miterN = sumN / sumNL;
            float denom = dot(miterN, nPrev);
            miterLen = 1.0 / max(abs(denom), 1e-3);
            miterLen = min(miterLen, 4.0);
        }

        float sideSign = float(side);
        bool isOuter = (turn >= 0.0) ? (sideSign > 0.0) : (sideSign < 0.0);
        vec2 edgeN = nPrev;
        float edgeLen = 1.0;
        if (clampedPoint == 0) {
            edgeN = nNext;
            edgeLen = 1.0;
        } else if (clampedPoint == lastPointIdx) {
            edgeN = nPrev;
            edgeLen = 1.0;
        } else if (dp < -0.95 || sumNL < 1e-3) {
            edgeN = nPrev;
            edgeLen = 1.0;
        } else if (isOuter) {
            edgeN = miterN;
            edgeLen = miterLen;
        } else {
            edgeN = nPrev;
            edgeLen = 1.0;
        }

        float halfWidth = radius * edgeLen;
        vec2 offset = edgeN * (halfWidth * sideSign);
        posScreen = pCurScreen + offset;
        // 向片元着色器输出“有符号边缘距离”，用于抗锯齿过渡（笔身）
        vEdgeSigned = sideSign * halfWidth;
        vHalfWidth = halfWidth;
        vMode = 0.0;
    } else {
        // 结束端帽：以终点为中心，沿笔迹方向生成端部几何
        if (count <= 1) {
            setDegenerateAtScreen(p0Screen, zNdc);
            return;
        }
        int capVid = vid - kEndCapStart;
        vec2 center = pNScreen;
        float r = rN;
        vec2 dir = dirEnd;
        vec2 n = nEnd;
        float sign = 1.0;
        float x0 = 0.0;
        float x1 = r;

        float x = (capVid == 0 || capVid == 1) ? x0 : x1;
        float y = (capVid == 0 || capVid == 2) ? -r : r;
        posScreen = center + dir * x + n * y;

        vMode = 1.0;
        vCapLocal = vec2(x, y);
        vCapRadius = r;
        vCapSign = sign;
    }

    vec2 ndc;
    ndc.x = (posScreen.x / uResolution.x) * 2.0 - 1.0;
    ndc.y = 1.0 - (posScreen.y / uResolution.y) * 2.0;
    gl_Position = vec4(ndc, zNdc, 1.0);
    if (degenerateTail) {
        vColor = vec4(0.0);
        vEffect = 0.0;
        vMode = 0.0;
        vEdgeSigned = 0.0;
        vHalfWidth = 0.0;
        vCapLocal = vec2(0.0);
        vCapRadius = 0.0;
        vCapSign = 0.0;
        vType = 0.0;
        vSeed = 0.0;
    }
}
)";

static const char* kFS = R"(#version 310 es
precision highp float;

// 片元着色器（ES 3.1+ / SSBO路径）
// - vMode==0：笔身，使用vEdgeSigned与vHalfWidth做抗锯齿边缘过渡
// - vMode==1：端帽，使用SDF圆/半平面裁剪做抗锯齿过渡
// 输出为预乘alpha：rgb已乘以alpha，便于与GL_ONE/GL_ONE_MINUS_SRC_ALPHA配合

in highp vec4 vColor;
in highp float vEffect;
in highp float vMode;
in highp float vEdgeSigned;
in highp float vHalfWidth;
in highp vec2 vCapLocal;
in highp float vCapRadius;
in highp float vCapSign;
in highp float vType;
in highp float vSeed;

out vec4 fragColor;

highp float hash21(highp vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

highp float noise2(highp vec2 p) {
    highp vec2 i = floor(p);
    highp vec2 f = fract(p);
    highp float a = hash21(i);
    highp float b = hash21(i + vec2(1.0, 0.0));
    highp float c = hash21(i + vec2(0.0, 1.0));
    highp float d = hash21(i + vec2(1.0, 1.0));
    highp vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

highp float fbm(highp vec2 p) {
    highp float sum = 0.0;
    highp float amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        sum += amp * noise2(p);
        p = p * 2.03 + vec2(17.0, 29.0);
        amp *= 0.5;
    }
    return sum;
}

void main() {
    if (vColor.a <= 0.0) discard;
    // 笔身抗锯齿：把“到边缘的距离”映射成平滑alpha
    float aaBody = max(fwidth(vEdgeSigned) * 1.5, 1.0);
    float hw = max(vHalfWidth, 0.0);
    float alphaBody = 1.0 - smoothstep(hw - 0.5 * aaBody, hw + 0.5 * aaBody, abs(vEdgeSigned));

    // 端帽抗锯齿：SDF圆 + 可选半平面裁剪（控制半圆/方形端帽的形状）
    float sdfCircle = length(vCapLocal) - vCapRadius;
    float sdf = sdfCircle;
    if (abs(vCapSign) > 0.5) {
        float sdfPlane = -vCapLocal.x * vCapSign;
        sdf = max(sdfCircle, sdfPlane);
    }
    float aaCap = max(fwidth(sdf) * 1.5, 1.0);
    float alphaCap = 1.0 - smoothstep(-0.5 * aaCap, 0.5 * aaCap, sdf);

    float useCap = clamp(vMode, 0.0, 1.0);
    float alpha = mix(alphaBody, alphaCap, useCap);
    vec3 rgb = vColor.rgb;
    float outA = vColor.a * alpha;
    if (vType > 0.5) {
        float luma = dot(rgb, vec3(0.299, 0.587, 0.114));
        rgb = mix(rgb, vec3(luma), 0.55);
        float angle = vSeed * 6.2831853;
        mat2 R = mat2(cos(angle), -sin(angle), sin(angle), cos(angle));
        vec2 p = R * (gl_FragCoord.xy + vec2(vSeed * 97.0, vSeed * 193.0));
        float g = fbm(p * 0.085 + vec2(vSeed * 13.0, vSeed * 31.0));
        float g2 = fbm(p.yx * 0.16 + vec2(vSeed * 53.0, vSeed * 71.0));
        float coverage = clamp(0.82 + 0.18 * g, 0.78, 1.0);
        float shade = 1.0 - 0.10 * (1.0 - g2);
        rgb *= shade;
        outA *= coverage;
    }
    fragColor = vec4(rgb * outA, outA);
}
)";

static const char* kFS_fetch_EXT = R"(#version 310 es
#extension GL_EXT_shader_framebuffer_fetch : require
precision highp float;

// 帧缓冲读取版本（EXT）：在片元中读取目标颜色并做自定义混合
// 目的：避免启用固定管线混合，在大量笔划叠加时减少带宽与状态切换开销

in highp vec4 vColor;
in highp float vEffect;
in highp float vMode;
in highp float vEdgeSigned;
in highp float vHalfWidth;
in highp vec2 vCapLocal;
in highp float vCapRadius;
in highp float vCapSign;
in highp float vType;
in highp float vSeed;

layout(location = 0) inout vec4 fragColor;

highp float hash21(highp vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

highp float noise2(highp vec2 p) {
    highp vec2 i = floor(p);
    highp vec2 f = fract(p);
    highp float a = hash21(i);
    highp float b = hash21(i + vec2(1.0, 0.0));
    highp float c = hash21(i + vec2(0.0, 1.0));
    highp float d = hash21(i + vec2(1.0, 1.0));
    highp vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

highp float fbm(highp vec2 p) {
    highp float sum = 0.0;
    highp float amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        sum += amp * noise2(p);
        p = p * 2.03 + vec2(17.0, 29.0);
        amp *= 0.5;
    }
    return sum;
}

void main() {
    if (vColor.a <= 0.0) discard;
    float aaBody = max(fwidth(vEdgeSigned) * 1.5, 1.0);
    float hw = max(vHalfWidth, 0.0);
    float alphaBody = 1.0 - smoothstep(hw - 0.5 * aaBody, hw + 0.5 * aaBody, abs(vEdgeSigned));

    float sdfCircle = length(vCapLocal) - vCapRadius;
    float sdf = sdfCircle;
    if (abs(vCapSign) > 0.5) {
        float sdfPlane = -vCapLocal.x * vCapSign;
        sdf = max(sdfCircle, sdfPlane);
    }
    float aaCap = max(fwidth(sdf) * 1.5, 1.0);
    float alphaCap = 1.0 - smoothstep(-0.5 * aaCap, 0.5 * aaCap, sdf);

    float useCap = clamp(vMode, 0.0, 1.0);
    float alpha = mix(alphaBody, alphaCap, useCap);

    // 源颜色：预乘alpha
    float Sa = vColor.a * alpha;
    vec3 S = vColor.rgb;
    if (vType > 0.5) {
        float luma = dot(S, vec3(0.299, 0.587, 0.114));
        S = mix(S, vec3(luma), 0.55);
        float angle = vSeed * 6.2831853;
        mat2 R = mat2(cos(angle), -sin(angle), sin(angle), cos(angle));
        vec2 p = R * (gl_FragCoord.xy + vec2(vSeed * 97.0, vSeed * 193.0));
        float g = fbm(p * 0.085 + vec2(vSeed * 13.0, vSeed * 31.0));
        float g2 = fbm(p.yx * 0.16 + vec2(vSeed * 53.0, vSeed * 71.0));
        float coverage = clamp(0.82 + 0.18 * g, 0.78, 1.0);
        float shade = 1.0 - 0.10 * (1.0 - g2);
        S *= shade;
        Sa *= coverage;
    }
    vec3 Sp = S * Sa;

    // 目标颜色：预乘alpha（由framebuffer fetch提供）
    vec4 dst = fragColor;
    float Da = dst.a;
    vec3 Dp = dst.rgb;

    // 默认混合：out = S + D*(1-Sa)
    vec3 outRGB = Sp + Dp * (1.0 - Sa);
    float outA = Sa + Da * (1.0 - Sa);

    if (vEffect > 0.5) {
        // “加深/暗化”效果：对非预乘的目标色做min混合，再回写为预乘
        vec3 Du = Dp / max(Da, 1e-6);
        vec3 B = min(Du, S);
        outRGB = Dp * (1.0 - Sa) + Sp * (1.0 - Da) + (Sa * Da) * B;
        outA = Sa + Da - Sa * Da;
    }
    fragColor = vec4(outRGB, outA);
}
)";

static const char* kFS_fetch_ARM = R"(#version 310 es
#extension GL_ARM_shader_framebuffer_fetch : require
precision highp float;

// 帧缓冲读取版本（ARM）：逻辑与EXT版本一致，但读取接口不同

in highp vec4 vColor;
in highp float vEffect;
in highp float vMode;
in highp float vEdgeSigned;
in highp float vHalfWidth;
in highp vec2 vCapLocal;
in highp float vCapRadius;
in highp float vCapSign;
in highp float vType;
in highp float vSeed;

out vec4 fragColor;

highp float hash21(highp vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

highp float noise2(highp vec2 p) {
    highp vec2 i = floor(p);
    highp vec2 f = fract(p);
    highp float a = hash21(i);
    highp float b = hash21(i + vec2(1.0, 0.0));
    highp float c = hash21(i + vec2(0.0, 1.0));
    highp float d = hash21(i + vec2(1.0, 1.0));
    highp vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

highp float fbm(highp vec2 p) {
    highp float sum = 0.0;
    highp float amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        sum += amp * noise2(p);
        p = p * 2.03 + vec2(17.0, 29.0);
        amp *= 0.5;
    }
    return sum;
}

void main() {
    if (vColor.a <= 0.0) discard;
    float aaBody = max(fwidth(vEdgeSigned) * 1.5, 1.0);
    float hw = max(vHalfWidth, 0.0);
    float alphaBody = 1.0 - smoothstep(hw - 0.5 * aaBody, hw + 0.5 * aaBody, abs(vEdgeSigned));

    float sdfCircle = length(vCapLocal) - vCapRadius;
    float sdf = sdfCircle;
    if (abs(vCapSign) > 0.5) {
        float sdfPlane = -vCapLocal.x * vCapSign;
        sdf = max(sdfCircle, sdfPlane);
    }
    float aaCap = max(fwidth(sdf) * 1.5, 1.0);
    float alphaCap = 1.0 - smoothstep(-0.5 * aaCap, 0.5 * aaCap, sdf);

    float useCap = clamp(vMode, 0.0, 1.0);
    float alpha = mix(alphaBody, alphaCap, useCap);

    float Sa = vColor.a * alpha;
    vec3 S = vColor.rgb;
    if (vType > 0.5) {
        float luma = dot(S, vec3(0.299, 0.587, 0.114));
        S = mix(S, vec3(luma), 0.55);
        float angle = vSeed * 6.2831853;
        mat2 R = mat2(cos(angle), -sin(angle), sin(angle), cos(angle));
        vec2 p = R * (gl_FragCoord.xy + vec2(vSeed * 97.0, vSeed * 193.0));
        float g = fbm(p * 0.085 + vec2(vSeed * 13.0, vSeed * 31.0));
        float g2 = fbm(p.yx * 0.16 + vec2(vSeed * 53.0, vSeed * 71.0));
        float coverage = clamp(0.82 + 0.18 * g, 0.78, 1.0);
        float shade = 1.0 - 0.10 * (1.0 - g2);
        S *= shade;
        Sa *= coverage;
    }
    vec3 Sp = S * Sa;

    vec4 dst = gl_LastFragColorARM;
    float Da = dst.a;
    vec3 Dp = dst.rgb;

    vec3 outRGB = Sp + Dp * (1.0 - Sa);
    float outA = Sa + Da * (1.0 - Sa);

    if (vEffect > 0.5) {
        // “加深/暗化”效果：对非预乘的目标色做min混合，再回写为预乘
        vec3 Du = Dp / max(Da, 1e-6);
        vec3 B = min(Du, S);
        outRGB = Dp * (1.0 - Sa) + Sp * (1.0 - Da) + (Sa * Da) * B;
        outA = Sa + Da - Sa * Da;
    }
    fragColor = vec4(outRGB, outA);
}
)";

static const char* kVS_debug = R"(#version 300 es
layout(location=0) in vec3 aStrictCheckBypass;
void main(){
    vec2 p;
    if (gl_VertexID == 0) p = vec2(0.0, 0.5);
    else if (gl_VertexID == 1) p = vec2(-0.5, -0.5);
    else p = vec2(0.5, -0.5);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

static const char* kFS_debug = R"(#version 300 es
precision mediump float;
out vec4 fragColor;
void main(){ fragColor = vec4(1.0, 0.0, 0.0, 1.0); }
)";

// 简化回退着色器（ES 3.0）：不使用SSBO，仅按VBO中的x,y绘制线段用于可见性诊断
static const char* kVS_simple = R"(#version 300 es
layout(location=0) in vec3 aData; // half(x,y,pressure) -> 以x,y为主
uniform vec2 uResolution;
uniform float uViewScale;
uniform vec2 uViewTranslate;
void main(){
    vec2 p = aData.xy; // 屏幕像素坐标
    p = p * uViewScale + uViewTranslate;
    vec2 ndc;
    ndc.x = (p.x / uResolution.x) * 2.0 - 1.0;
    ndc.y = 1.0 - (p.y / uResolution.y) * 2.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)";

static const char* kFS_simple = R"(#version 300 es
precision mediump float;
uniform vec4 uColor;
out vec4 fragColor;
void main(){ fragColor = uColor; }
)";

static const char* kVS_image = R"(#version 300 es
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main(){
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

static const char* kFS_image = R"(#version 300 es
precision mediump float;
in vec2 vUV;
uniform sampler2D uTex;
out vec4 fragColor;
void main(){
    fragColor = texture(uTex, vUV);
}
)";

static const char* kVS_tex = R"(#version 300 es
precision highp float;
precision highp sampler2D;
// 顶点着色器（ES 3.0+ / 纹理取数路径）
// 目标：在一次 glDrawArraysInstanced 调用中绘制所有笔划，并与SSBO路径保持相同的几何生成逻辑。
//
// 数据来源（纹理）：
// - uDataTex：每个像素存一个点 (x,y,pressure,unused)，纹理坐标为 (pointIdx, strokeId)
// - uMetaBWCTex：每条笔划一个像素 (count, baseWidth, effect, unused)，纹理坐标为 (0, strokeId)
// - uMetaColorTex：每条笔划一个像素 (r,g,b,a)，纹理坐标为 (0, strokeId)
layout(location=0) in vec3 aStrictCheckBypass;

uniform vec2 uResolution;
uniform float uViewScale;
uniform vec2 uViewTranslate;
uniform float uStrokeCount;
uniform float uBaseInstance;
uniform int uPass;
uniform int uRenderMaxPoints;

uniform sampler2D uDataTex;
uniform sampler2D uMetaBWCTex;
uniform sampler2D uMetaColorTex;

out highp vec4 vColor;
out highp float vEffect;
out highp float vMode;
out highp float vEdgeSigned;
out highp float vHalfWidth;
out highp vec2 vCapLocal;
out highp float vCapRadius;
out highp float vCapSign;
out highp float vType;
out highp float vSeed;

highp vec2 safeNormalize(highp vec2 v) {
    highp float l = length(v);
    if (l < 1e-4) return vec2(1.0, 0.0);
    return v / l;
}

highp vec4 readMetaBWC(int strokeId) {
    return texelFetch(uMetaBWCTex, ivec2(0, strokeId), 0);
}

highp vec4 readMetaBounds(int strokeId) {
    return texelFetch(uMetaBWCTex, ivec2(1, strokeId), 0);
}

highp vec3 readPoint(int strokeId, int pointIdx, highp vec4 bounds) {
    vec4 t = texelFetch(uDataTex, ivec2(pointIdx, strokeId), 0);
    vec2 posW = bounds.xy + t.xy * bounds.zw;
    return vec3(posW, t.z);
}

highp vec4 readMetaColor(int strokeId) {
    return texelFetch(uMetaColorTex, ivec2(0, strokeId), 0);
}

void setOffscreen() {
    gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
    vColor = vec4(0.0);
    vEffect = 0.0;
    vMode = 0.0;
    vEdgeSigned = 0.0;
    vHalfWidth = 0.0;
    vCapLocal = vec2(0.0);
    vCapRadius = 0.0;
    vCapSign = 0.0;
    vType = 0.0;
    vSeed = 0.0;
}

void setDegenerateAtScreen(vec2 anchorScreen, float zNdc) {
    vec2 ndc;
    ndc.x = (anchorScreen.x / uResolution.x) * 2.0 - 1.0;
    ndc.y = 1.0 - (anchorScreen.y / uResolution.y) * 2.0;
    gl_Position = vec4(ndc, zNdc, 1.0);
    vColor = vec4(0.0);
    vEffect = 0.0;
    vMode = 0.0;
    vEdgeSigned = 0.0;
    vHalfWidth = 0.0;
    vCapLocal = vec2(0.0);
    vCapRadius = 0.0;
    vCapSign = 0.0;
    vType = 0.0;
    vSeed = 0.0;
}

void main() {
    vec3 dummy = aStrictCheckBypass * 0.000001;

    int strokeId = gl_InstanceID + int(uBaseInstance);
    vec4 mbwc = readMetaBWC(strokeId);
    vec4 mbounds = readMetaBounds(strokeId);
    int count = int(mbwc.x + 0.5);
    float baseWidth = mbwc.y;
    float effect = mbwc.z;
    vEffect = effect;
    vType = mbwc.w;
    vSeed = fract(sin(float(strokeId) * 12.9898 + 78.233) * 43758.5453);

    // 分组渲染逻辑与SSBO路径保持一致
    if ((uPass == 0 && effect > 0.5) || (uPass == 1 && effect <= 0.5) || count <= 0) {
        setOffscreen();
        return;
    }

    float zNdc = 0.0;

    int maxPoints = clamp(min(count, uRenderMaxPoints), 1, 1024);
    int kBodyVerts = maxPoints * 2;
    const int kStartCapVerts = 4;
    const int kEndCapVerts = 4;
    int kBodyStart = kStartCapVerts;
    int kBodyEnd = kBodyStart + kBodyVerts;
    int kEndCapStart = kBodyEnd;
    int kTotalVerts = kBodyVerts + kStartCapVerts + kEndCapVerts;

    int lastPointIdx = max(count - 1, 0);
    vec3 p0w = readPoint(strokeId, 0, mbounds);
    vec3 p1w = readPoint(strokeId, min(1, lastPointIdx), mbounds);
    vec3 pNw = readPoint(strokeId, lastPointIdx, mbounds);
    vec3 pN1w = readPoint(strokeId, max(lastPointIdx - 1, 0), mbounds);

    vec2 p0Screen = p0w.xy * uViewScale + uViewTranslate;
    vec2 p1Screen = p1w.xy * uViewScale + uViewTranslate;
    vec2 pNScreen = pNw.xy * uViewScale + uViewTranslate;
    vec2 pN1Screen = pN1w.xy * uViewScale + uViewTranslate;

    vec2 dirStart = safeNormalize(p1Screen - p0Screen);
    vec2 dirEnd = safeNormalize(pNScreen - pN1Screen);
    vec2 nStart = vec2(-dirStart.y, dirStart.x);
    vec2 nEnd = vec2(-dirEnd.y, dirEnd.x);
    float r0 = baseWidth * p0w.z * 0.5 * uViewScale;
    float rN = baseWidth * pNw.z * 0.5 * uViewScale;

    int vid = gl_VertexID;
    bool degenerateTail = false;
    if (vid < 0) {
        setOffscreen();
        return;
    }
    if (vid >= kTotalVerts) {
        vid = kTotalVerts - 1;
        degenerateTail = true;
    }

    vec2 posScreen = vec2(0.0);
    vMode = 0.0;
    vEdgeSigned = 0.0;
    vHalfWidth = 0.0;
    vCapLocal = vec2(0.0);
    vCapRadius = 0.0;
    vCapSign = 0.0;
    vColor = readMetaColor(strokeId);

    if (vid < kStartCapVerts) {
        vec2 center = p0Screen;
        float r = r0;
        vec2 dir = dirStart;
        vec2 n = nStart;
        float sign = -1.0;
        float x0 = -r;
        float x1 = 0.0;
        if (count <= 1) {
            dir = vec2(1.0, 0.0);
            n = vec2(0.0, 1.0);
            sign = 0.0;
            x1 = r;
        }
        float x = (vid == 0 || vid == 1) ? x0 : x1;
        float y = (vid == 0 || vid == 2) ? -r : r;
        posScreen = center + dir * x + n * y;
        vMode = 1.0;
        vCapLocal = vec2(x, y);
        vCapRadius = r;
        vCapSign = sign;
    } else if (vid < kBodyEnd) {
        if (count <= 1) {
            setDegenerateAtScreen(p0Screen, zNdc);
            return;
        }
        int bodyVid = vid - kBodyStart;
        int pointIdx = bodyVid >> 1;
        int side = (bodyVid & 1) == 0 ? -1 : 1;

        int denom = max(maxPoints - 1, 1);
        int clampedPoint = min((pointIdx * lastPointIdx) / denom, lastPointIdx);
        vec3 pCurW = readPoint(strokeId, clampedPoint, mbounds);
        vec2 pCurScreen = pCurW.xy * uViewScale + uViewTranslate;
        float pressure = pCurW.z;
        float radius = baseWidth * pressure * 0.5 * uViewScale;

        int prevSampleIdx = max(pointIdx - 1, 0);
        int nextSampleIdx = min(pointIdx + 1, maxPoints - 1);
        int prevPointIdx = min((prevSampleIdx * lastPointIdx) / denom, lastPointIdx);
        int nextPointIdx = min((nextSampleIdx * lastPointIdx) / denom, lastPointIdx);

        vec2 pPrevScreen = readPoint(strokeId, prevPointIdx, mbounds).xy * uViewScale + uViewTranslate;
        vec2 pNextScreen = readPoint(strokeId, nextPointIdx, mbounds).xy * uViewScale + uViewTranslate;

        vec2 dirPrev = safeNormalize(pCurScreen - pPrevScreen);
        vec2 dirNext = safeNormalize(pNextScreen - pCurScreen);
        if (pointIdx == 0) dirPrev = dirNext;
        if (pointIdx == maxPoints - 1) dirNext = dirPrev;

        vec2 nPrev = vec2(-dirPrev.y, dirPrev.x);
        vec2 nNext = vec2(-dirNext.y, dirNext.x);

        float dp = dot(dirPrev, dirNext);
        vec2 sumN = nPrev + nNext;
        float sumNL = length(sumN);
        float turn = dirPrev.x * dirNext.y - dirPrev.y * dirNext.x;

        vec2 miterN = nPrev;
        float miterLen = 1.0;
        if (sumNL > 1e-3 && dp > -0.95) {
            miterN = sumN / sumNL;
            float denom2 = dot(miterN, nPrev);
            miterLen = 1.0 / max(abs(denom2), 1e-3);
            miterLen = min(miterLen, 4.0);
        }

        float sideSign = float(side);
        bool isOuter = (turn >= 0.0) ? (sideSign > 0.0) : (sideSign < 0.0);
        vec2 edgeN = nPrev;
        float edgeLen = 1.0;
        if (clampedPoint == 0) {
            edgeN = nNext;
            edgeLen = 1.0;
        } else if (clampedPoint == lastPointIdx) {
            edgeN = nPrev;
            edgeLen = 1.0;
        } else if (dp < -0.95 || sumNL < 1e-3) {
            edgeN = nPrev;
            edgeLen = 1.0;
        } else if (isOuter) {
            edgeN = miterN;
            edgeLen = miterLen;
        } else {
            edgeN = nPrev;
            edgeLen = 1.0;
        }

        float halfWidth = radius * edgeLen;
        vec2 offset = edgeN * (halfWidth * sideSign);
        posScreen = pCurScreen + offset;
        vEdgeSigned = sideSign * halfWidth;
        vHalfWidth = halfWidth;
        vMode = 0.0;
    } else {
        if (count <= 1) {
            setDegenerateAtScreen(p0Screen, zNdc);
            return;
        }
        int capVid = vid - kEndCapStart;
        vec2 center = pNScreen;
        float r = rN;
        vec2 dir = dirEnd;
        vec2 n = nEnd;
        float sign = 1.0;
        float x0 = 0.0;
        float x1 = r;
        float x = (capVid == 0 || capVid == 1) ? x0 : x1;
        float y = (capVid == 0 || capVid == 2) ? -r : r;
        posScreen = center + dir * x + n * y;
        vMode = 1.0;
        vCapLocal = vec2(x, y);
        vCapRadius = r;
        vCapSign = sign;
    }

    vec2 ndc;
    ndc.x = (posScreen.x / uResolution.x) * 2.0 - 1.0;
    ndc.y = 1.0 - (posScreen.y / uResolution.y) * 2.0;
    gl_Position = vec4(ndc, zNdc, 1.0);

    if (degenerateTail) {
        vColor = vec4(0.0);
        vEffect = 0.0;
        vMode = 0.0;
        vEdgeSigned = 0.0;
        vHalfWidth = 0.0;
        vCapLocal = vec2(0.0);
        vCapRadius = 0.0;
        vCapSign = 0.0;
        vType = 0.0;
        vSeed = 0.0;
    }
}
)";

static const char* kFS_tex = R"(#version 300 es
precision highp float;
// 片元着色器（ES 3.0+ / 通用）
// 与SSBO路径相同：输出预乘alpha，便于使用 GL_ONE / GL_ONE_MINUS_SRC_ALPHA 混合。
in highp vec4 vColor;
in highp float vEffect;
in highp float vMode;
in highp float vEdgeSigned;
in highp float vHalfWidth;
in highp vec2 vCapLocal;
in highp float vCapRadius;
in highp float vCapSign;
in highp float vType;
in highp float vSeed;
out vec4 fragColor;

highp float hash21(highp vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

highp float noise2(highp vec2 p) {
    highp vec2 i = floor(p);
    highp vec2 f = fract(p);
    highp float a = hash21(i);
    highp float b = hash21(i + vec2(1.0, 0.0));
    highp float c = hash21(i + vec2(0.0, 1.0));
    highp float d = hash21(i + vec2(1.0, 1.0));
    highp vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

highp float fbm(highp vec2 p) {
    highp float sum = 0.0;
    highp float amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        sum += amp * noise2(p);
        p = p * 2.03 + vec2(17.0, 29.0);
        amp *= 0.5;
    }
    return sum;
}

void main() {
    if (vColor.a <= 0.0) discard;
    float aaBody = max(fwidth(vEdgeSigned) * 1.5, 1.0);
    float hw = max(vHalfWidth, 0.0);
    float alphaBody = 1.0 - smoothstep(hw - 0.5 * aaBody, hw + 0.5 * aaBody, abs(vEdgeSigned));

    float sdfCircle = length(vCapLocal) - vCapRadius;
    float sdf = sdfCircle;
    if (abs(vCapSign) > 0.5) {
        float sdfPlane = -vCapLocal.x * vCapSign;
        sdf = max(sdfCircle, sdfPlane);
    }
    float aaCap = max(fwidth(sdf) * 1.5, 1.0);
    float alphaCap = 1.0 - smoothstep(-0.5 * aaCap, 0.5 * aaCap, sdf);

    float useCap = clamp(vMode, 0.0, 1.0);
    float alpha = mix(alphaBody, alphaCap, useCap);
    vec3 rgb = vColor.rgb;
    float outA = vColor.a * alpha;
    if (vType > 0.5) {
        float luma = dot(rgb, vec3(0.299, 0.587, 0.114));
        rgb = mix(rgb, vec3(luma), 0.55);
        float angle = vSeed * 6.2831853;
        mat2 R = mat2(cos(angle), -sin(angle), sin(angle), cos(angle));
        vec2 p = R * (gl_FragCoord.xy + vec2(vSeed * 97.0, vSeed * 193.0));
        float g = fbm(p * 0.085 + vec2(vSeed * 13.0, vSeed * 31.0));
        float g2 = fbm(p.yx * 0.16 + vec2(vSeed * 53.0, vSeed * 71.0));
        float coverage = clamp(0.82 + 0.18 * g, 0.78, 1.0);
        float shade = 1.0 - 0.10 * (1.0 - g2);
        rgb *= shade;
        outA *= coverage;
    }
    fragColor = vec4(rgb * outA, outA);
}
)";

extern "C" {

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_onNativeSurfaceCreated(JNIEnv* env, jobject /*thiz*/) {
    gGlReady = false;
    if (gProgram) {
        glDeleteProgram(gProgram);
        gProgram = 0;
    }
    if (gTexProgram) {
        glDeleteProgram(gTexProgram);
        gTexProgram = 0;
    }
    // 初始化基本状态
    LOGW("GL Renderer: %s", glGetString(GL_RENDERER));
    LOGW("GL Vendor: %s", glGetString(GL_VENDOR));
    LOGW("GL Version: %s", glGetString(GL_VERSION));
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glClearDepthf(1.0f);
    glEnable(GL_BLEND);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);

    // 判断GL版本是否>= ES 3.1，低于则走简化回退路径
    const char* verStr = (const char*)glGetString(GL_VERSION);
    gUseSSBO = false;
    if (verStr && (strstr(verStr, "OpenGL ES 3.2") || strstr(verStr, "OpenGL ES 3.1"))) {
        GLint maxVertexSsbo = 0;
        GLint maxSsboBindings = 0;
        glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &maxVertexSsbo);
        glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &maxSsboBindings);

        if (maxVertexSsbo < 4 || maxSsboBindings < 4) {
            LOGW("Fallback: SSBO unsupported, skip linking SSBO program (vertexBlocks=%d bindings=%d)", maxVertexSsbo, maxSsboBindings);
        } else {
            GLuint vs = compileShader(GL_VERTEX_SHADER, kVS);
            GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFS);
            gProgram = linkProgram2(vs, fs);
            if (!gProgram) {
                LOGW("Fallback: SSBO shader compile/link failed (vertexBlocks=%d bindings=%d)", maxVertexSsbo, maxSsboBindings);
            } else {
                gUseSSBO = true;
                LOGW("SSBO path enabled (vertexBlocks=%d bindings=%d)", maxVertexSsbo, maxSsboBindings);
            }
        }
    } else {
        LOGW("Fallback: using ES3.0 simple shader path");
    }
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    GLint depthBits = 0;
    glGetIntegerv(GL_DEPTH_BITS, &depthBits);
    GLfloat pointRange[2] = {0.0f, 0.0f};
    glGetFloatv(GL_ALIASED_POINT_SIZE_RANGE, pointRange);
    if (pointRange[1] > 0.0f) gMaxPointSize = pointRange[1];
    LOGW("EGL depth bits: %d, useSSBO: %s", depthBits, gUseSSBO ? "yes" : "no");
    LOGW("Point size range: %.1f .. %.1f", pointRange[0], pointRange[1]);

    if (!gDebugProgram) {
        GLuint dvs = compileShader(GL_VERTEX_SHADER, kVS_debug);
        GLuint dfs = compileShader(GL_FRAGMENT_SHADER, kFS_debug);
        gDebugProgram = linkProgram2(dvs, dfs);
    }
    // 检测是否支持OES_vertex_half_float，用于决定回退路径的顶点格式
    GLint numExt = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &numExt);
    for (GLint i = 0; i < numExt; ++i) {
        const char* ext = (const char*)glGetStringi(GL_EXTENSIONS, i);
        if (ext && (strcmp(ext, "GL_OES_vertex_half_float") == 0 || strcmp(ext, "OES_vertex_half_float") == 0)) {
            gHasVertexHalfFloat = true;
            break;
        }
    }
    bool hasFetchEXT = false;
    bool hasFetchARM = false;
    for (GLint i = 0; i < numExt; ++i) {
        const char* ext = (const char*)glGetStringi(GL_EXTENSIONS, i);
        if (!ext) continue;
        if (strcmp(ext, "GL_EXT_shader_framebuffer_fetch") == 0) hasFetchEXT = true;
        if (strcmp(ext, "GL_ARM_shader_framebuffer_fetch") == 0) hasFetchARM = true;
    }
    LOGW("Vertex half-float supported: %s", gHasVertexHalfFloat ? "yes" : "no");

    if (gUseSSBO && gProgram && (hasFetchEXT || hasFetchARM)) {
        GLuint vs = compileShader(GL_VERTEX_SHADER, kVS);
        if (vs) {
            const char* fsSrc = hasFetchEXT ? kFS_fetch_EXT : kFS_fetch_ARM;
            GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
            if (fs) {
                GLuint p = linkProgram2(vs, fs);
                if (p) {
                    glDeleteProgram(gProgram);
                    gProgram = p;
                    gUseFramebufferFetch = true;
                    gUseFramebufferFetchEXT = hasFetchEXT;
                }
            } else {
                glDeleteShader(vs);
            }
        }
    }
    if (gUseSSBO) {
        glUseProgram(gProgram);
        uResolutionLoc = glGetUniformLocation(gProgram, "uResolution");
        uViewScaleLoc = glGetUniformLocation(gProgram, "uViewScale");
        uViewTranslateLoc = glGetUniformLocation(gProgram, "uViewTranslate");
        uStrokeCountLoc = glGetUniformLocation(gProgram, "uStrokeCount");
        uBaseInstanceLoc = glGetUniformLocation(gProgram, "uBaseInstance");
        uMaxPointSizeLoc = glGetUniformLocation(gProgram, "uMaxPointSize");
        uPassLoc = glGetUniformLocation(gProgram, "uPass");
        uRenderMaxPointsLoc = glGetUniformLocation(gProgram, "uRenderMaxPoints");
        uColorLoc = glGetUniformLocation(gProgram, "uColor");

        // VAO与缓冲
        glGenVertexArrays(1, &gVAO);
        glGenVertexArrays(1, &gEmptyVAO);

        gAllocatedStrokes = 4096;
        size_t pointsCapacity = (size_t)gAllocatedStrokes * kMaxPointsPerStroke;

        glBindVertexArray(gEmptyVAO);
        if (!gBypassVBO) glGenBuffers(1, &gBypassVBO);
        glBindBuffer(GL_ARRAY_BUFFER, gBypassVBO);
        const int bypassVerts = kVertsPerStroke;
        std::vector<float> bypassData((size_t)bypassVerts * 3u, 0.0f);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(bypassData.size() * sizeof(float)), bypassData.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, (const void*)0);
        glVertexAttribDivisor(0, 0);

        // Positions SSBO
        glGenBuffers(1, &gPositionsSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPositionsSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, pointsCapacity * sizeof(float) * 2, nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gPositionsSSBO);

        // Meta SSBO
        glGenBuffers(1, &gStrokeMetaSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gStrokeMetaSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, gAllocatedStrokes * sizeof(StrokeMetaCPU), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, gStrokeMetaSSBO);

        // Pressures SSBO
        glGenBuffers(1, &gPressuresSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPressuresSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(packedPressureCount(pointsCapacity) * sizeof(uint32_t)), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, gPressuresSSBO);

        glGenBuffers(1, &gVisibleIndexSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gVisibleIndexSSBO);
        gVisibleIndexCapacity = gAllocatedStrokes + 1;
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)((size_t)gVisibleIndexCapacity * sizeof(uint32_t) * 2u), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, gVisibleIndexSSBO);
        gVisibleDirty.store(1);

        LOGI("Allocated buffers: strokes=%d, maxPointsPerStroke=%d, positions=%zu bytes, pressures=%zu bytes",
             gAllocatedStrokes, kMaxPointsPerStroke,
             (size_t)(pointsCapacity * sizeof(float) * 2),
             (size_t)(packedPressureCount(pointsCapacity) * sizeof(uint32_t)));

        // 如有暂存笔划，初始化完成后立即刷新到GPU
        if (!gPendingStrokes.empty()) {
            size_t pending = gPendingStrokes.size();
            for (const auto& ps : gPendingStrokes) {
                uploadStroke(ps.points, ps.pressures, ps.color, ps.type);
            }
            gPendingStrokes.clear();
            LOGI("Flushed pending strokes: %zu", pending);
        }
    } else {
        GLuint vs = compileShader(GL_VERTEX_SHADER, kVS_tex);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFS_tex);
        gTexProgram = linkProgram2(vs, fs);
        if (gTexProgram) {
            glUseProgram(gTexProgram);
            uTexResolutionLoc = glGetUniformLocation(gTexProgram, "uResolution");
            uTexViewScaleLoc = glGetUniformLocation(gTexProgram, "uViewScale");
            uTexViewTranslateLoc = glGetUniformLocation(gTexProgram, "uViewTranslate");
            uTexStrokeCountLoc = glGetUniformLocation(gTexProgram, "uStrokeCount");
            uTexBaseInstanceLoc = glGetUniformLocation(gTexProgram, "uBaseInstance");
            uTexPassLoc = glGetUniformLocation(gTexProgram, "uPass");
            uTexRenderMaxPointsLoc = glGetUniformLocation(gTexProgram, "uRenderMaxPoints");
            uTexDataSamplerLoc = glGetUniformLocation(gTexProgram, "uDataTex");
            uTexMetaBWCSamplerLoc = glGetUniformLocation(gTexProgram, "uMetaBWCTex");
            uTexMetaColorSamplerLoc = glGetUniformLocation(gTexProgram, "uMetaColorTex");
            LOGW("FallbackProgram: texProgram=%u uResolution=%d uViewScale=%d uViewTranslate=%d uStrokeCount=%d uBaseInstance=%d uPass=%d uRenderMaxPoints=%d uDataTex=%d uMetaBWCTex=%d uMetaColorTex=%d",
                 (unsigned)gTexProgram,
                 uTexResolutionLoc, uTexViewScaleLoc, uTexViewTranslateLoc, uTexStrokeCountLoc, uTexBaseInstanceLoc, uTexPassLoc, uTexRenderMaxPointsLoc,
                 uTexDataSamplerLoc, uTexMetaBWCSamplerLoc, uTexMetaColorSamplerLoc);
        } else {
            LOGE("FallbackProgram: link failed, texProgram=0");
        }

        glGenVertexArrays(1, &gEmptyVAO);
        glBindVertexArray(gEmptyVAO);
        if (!gBypassVBO) glGenBuffers(1, &gBypassVBO);
        glBindBuffer(GL_ARRAY_BUFFER, gBypassVBO);
        const int bypassVerts = kVertsPerStroke;
        std::vector<float> bypassData((size_t)bypassVerts * 3u, 0.0f);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(bypassData.size() * sizeof(float)), bypassData.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, (const void*)0);
        glVertexAttribDivisor(0, 0);
        glBindVertexArray(0);

        size_t pending = gPendingStrokes.size();
        int initialCap = (int)std::max<size_t>(1u, pending);
        if (!ensureFallbackStorageCapacity(initialCap)) {
            LOGE("Fallback: failed to allocate initial AHardwareBuffer textures");
        }
        gFallbackStrokeCount.store(0);
        if (!gPendingStrokes.empty()) {
            for (const auto& ps : gPendingStrokes) {
                uploadStroke(ps.points, ps.pressures, ps.color, ps.type);
            }
            gPendingStrokes.clear();
            LOGI("Flushed pending strokes: %zu", pending);
        }
    }
    gGlReady = true;
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_onNativeSurfaceChanged(JNIEnv* env, jobject /*thiz*/, jint width, jint height) {
    g_Width = width; g_Height = height;
    glViewport(0, 0, g_Width, g_Height);
    LOGW("Surface changed: %dx%d", g_Width, g_Height);
    if (gUseSSBO && gProgram) {
        glUseProgram(gProgram);
        glUniform2f(uResolutionLoc, (float)g_Width, (float)g_Height);
        glUniform1f(uViewScaleLoc, 1.0f);
        gViewScale = 1.0f;
        gViewTranslateX = 0.0f;
        gViewTranslateY = 0.0f;
        if (uViewTranslateLoc >= 0) glUniform2f(uViewTranslateLoc, gViewTranslateX, gViewTranslateY);
    } else if (!gUseSSBO && gTexProgram) {
        glUseProgram(gTexProgram);
        if (uTexResolutionLoc >= 0) glUniform2f(uTexResolutionLoc, (float)g_Width, (float)g_Height);
        if (uTexViewScaleLoc >= 0) glUniform1f(uTexViewScaleLoc, 1.0f);
        gViewScale = 1.0f;
        gViewTranslateX = 0.0f;
        gViewTranslateY = 0.0f;
        if (uTexViewTranslateLoc >= 0) glUniform2f(uTexViewTranslateLoc, gViewTranslateX, gViewTranslateY);
    }
    gVisibleDirty.store(1);
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_onNativeDrawFrame(JNIEnv* env, jobject /*thiz*/) {
    while (glGetError() != GL_NO_ERROR) {}
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    
    if (!gGlReady) return;

    if (!gUseSSBO) {
        if (!gTexProgram || !gEmptyVAO || !gDataTex || !gMetaBWCTex || !gMetaColorTex) return;
        int committedStrokes = gFallbackStrokeCount.load();
        int totalStrokes = committedStrokes + (gLiveActive ? 1 : 0);
        if (gFallbackFirstFrameLogOnce.fetch_sub(1) > 0) {
            LOGW("FallbackFirstFrame: texProgram=%u dataTex=%u metaBWCTex=%u metaColorTex=%u capacity=%d committed=%d live=%s",
                 (unsigned)gTexProgram,
                 (unsigned)gDataTex,
                 (unsigned)gMetaBWCTex,
                 (unsigned)gMetaColorTex,
                 gFallbackCapacityStrokes,
                 committedStrokes,
                 gLiveActive ? "yes" : "no");
        }
        if (totalStrokes <= 0) {
            if (gDebugProgram) {
                glUseProgram(gDebugProgram);
                glBindVertexArray(gEmptyVAO);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
            return;
        }

        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        glUseProgram(gTexProgram);
        if (uTexResolutionLoc >= 0) glUniform2f(uTexResolutionLoc, (float)g_Width, (float)g_Height);
        if (uTexViewScaleLoc >= 0) glUniform1f(uTexViewScaleLoc, gViewScale);
        if (uTexViewTranslateLoc >= 0) glUniform2f(uTexViewTranslateLoc, gViewTranslateX, gViewTranslateY);
        if (uTexStrokeCountLoc >= 0) glUniform1f(uTexStrokeCountLoc, (float)std::max(totalStrokes, 1));
        if (uTexBaseInstanceLoc >= 0) glUniform1f(uTexBaseInstanceLoc, 0.0f);
        if (uTexPassLoc >= 0) glUniform1i(uTexPassLoc, 2);
        if (uTexRenderMaxPointsLoc >= 0) glUniform1i(uTexRenderMaxPointsLoc, std::clamp(gRenderMaxPoints.load(), 1, 1024));

        insertGpuWaitOnNativeFenceFd(&gDataWriteFenceFd);
        insertGpuWaitOnNativeFenceFd(&gMetaBWCWriteFenceFd);
        insertGpuWaitOnNativeFenceFd(&gMetaColorWriteFenceFd);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gDataTex);
        if (uTexDataSamplerLoc >= 0) glUniform1i(uTexDataSamplerLoc, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, gMetaBWCTex);
        if (uTexMetaBWCSamplerLoc >= 0) glUniform1i(uTexMetaBWCSamplerLoc, 1);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, gMetaColorTex);
        if (uTexMetaColorSamplerLoc >= 0) glUniform1i(uTexMetaColorSamplerLoc, 2);

        glBindVertexArray(gEmptyVAO);
        const int vertsPerStroke = std::clamp(gRenderMaxPoints.load(), 1, 1024) * 2 + 8;
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, vertsPerStroke, totalStrokes);
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            LOGE("Fallback glDraw error=0x%x", err);
        }
        glBindVertexArray(0);
        if (gFallbackGpuSync) {
            glDeleteSync(gFallbackGpuSync);
            gFallbackGpuSync = nullptr;
        }
        gFallbackGpuSync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        return;
    }

    if (!gProgram) return;
    glUseProgram(gProgram);
    if (gGlErrorLogBudget > 0) {
        GLenum e = glGetError();
        if (e != GL_NO_ERROR) {
            --gGlErrorLogBudget;
            LOGE("GL error at glUseProgram: 0x%x", e);
        }
    }
    if (uResolutionLoc >= 0) {
        glUniform2f(uResolutionLoc, (float)g_Width, (float)g_Height);
    }
    if (uViewScaleLoc >= 0) {
        glUniform1f(uViewScaleLoc, gViewScale);
    }
    if (uViewTranslateLoc >= 0) {
        glUniform2f(uViewTranslateLoc, gViewTranslateX, gViewTranslateY);
    }
    if (uMaxPointSizeLoc >= 0) {
        glUniform1f(uMaxPointSizeLoc, gMaxPointSize);
    }
    if (uRenderMaxPointsLoc >= 0) {
        glUniform1i(uRenderMaxPointsLoc, std::clamp(gRenderMaxPoints.load(), 1, 1024));
    }
    if (gFirstFrameLogOnce.fetch_sub(1) > 0) {
        LOGW("FirstFrame: useSSBO=%s framebufferFetch=%s scale=%.3f translate=(%.1f,%.1f) renderMaxPoints=%d committed=%d live=%s",
             gUseSSBO ? "yes" : "no",
             gUseFramebufferFetch ? (gUseFramebufferFetchEXT ? "ext" : "arm") : "no",
             gViewScale,
             gViewTranslateX, gViewTranslateY,
             std::clamp(gRenderMaxPoints.load(), 1, 1024),
             (int)gMetas.size(),
             gLiveActive ? "yes" : "no");
    }
    int committedStrokes = (int)gMetas.size();
    int totalStrokes = committedStrokes + (gLiveActive ? 1 : 0);

    if (gUseSSBO) {
        updateVisibleListIfNeeded();
        glBindVertexArray(gEmptyVAO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, gStrokeMetaSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gPositionsSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, gPressuresSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, gVisibleIndexSSBO);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        if (gGlErrorLogBudget > 0) {
            GLenum e = glGetError();
            if (e != GL_NO_ERROR) {
                --gGlErrorLogBudget;
                LOGE("GL error at bind SSBO/VAO: 0x%x", e);
            }
        }
    } else {
        glBindVertexArray(gVAO);
        // 回退路径仅用VBO绘制线段
        glBindBuffer(GL_ARRAY_BUFFER, gPointsBuffer);
        glEnableVertexAttribArray(0);
        if (gHasVertexHalfFloat) {
            glVertexAttribPointer(0, 3, GL_HALF_FLOAT, GL_FALSE, sizeof(uint16_t) * 3, (const void*)0);
        } else {
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, (const void*)0);
        }
    }
    //disable this log, too much
    //LOGI("drawFrame: strokes=%d res=%dx%d scale=%f", totalStrokes, g_Width, g_Height, gViewScale);
    // 额外诊断：打印前几条笔划的元数据，确认GPU可读数据与期望一致
    for (int s = 0; s < committedStrokes && s < 3; ++s) {
        const StrokeMetaCPU &m = gMetas[s];
        //disable this log, too much
        /*
        LOGI("stroke[%d]: start=%d count=%d width=%.1f color=(%.2f,%.2f,%.2f,%.2f)",
             s, m.start, m.count, m.baseWidth, m.color[0], m.color[1], m.color[2], m.color[3]);
             */
    }
    // 添加简单的测试渲染：绘制一个红色三角形
    if (totalStrokes == 0) {
        if (gDebugProgram) {
            glUseProgram(gDebugProgram);
            glBindVertexArray(gUseSSBO ? gEmptyVAO : gVAO);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        return;
    }

    if (!gProgram) return;

    if (uPassLoc >= 0) glUniform1i(uPassLoc, 2);
    if (gUseFramebufferFetch) {
        glDisable(GL_BLEND);
    } else {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    int drawCount = (gVisibleIndexSSBO ? gVisibleCount : (gLiveActive ? totalStrokes : committedStrokes));
    if (gVisibleIndexSSBO) {
        drawCount = gVisibleCount;
    }
    if (drawCount > 0) {
        if (uStrokeCountLoc >= 0) glUniform1f(uStrokeCountLoc, (float)std::max(totalStrokes, 1));
        if (uBaseInstanceLoc >= 0) glUniform1f(uBaseInstanceLoc, 0.0f);
        const int vertsPerStroke = std::clamp(gRenderMaxPoints.load(), 1, 1024) * 2 + 8;
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, vertsPerStroke, drawCount);
    }
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        LOGE("glDraw error=0x%x", err);
    }
}

JNIEXPORT jboolean JNICALL
Java_com_example_myapplication_NativeBridge_isUsingSSBO(JNIEnv* env, jobject /*thiz*/) {
    (void)env;
    return gUseSSBO ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_updateFallbackImage(JNIEnv* env, jobject /*thiz*/, jbyteArray rgbaBytes, jint width, jint height) {
    if (!env || !rgbaBytes) return;
    if (width <= 0 || height <= 0) return;
    if (!gGlReady || gUseSSBO) return;
    if (!gImageTex) return;

    jsize len = env->GetArrayLength(rgbaBytes);
    const jsize expected = (jsize)((int64_t)width * (int64_t)height * 4);
    if (len < expected) return;

    std::vector<uint8_t> rgba((size_t)expected);
    env->GetByteArrayRegion(rgbaBytes, 0, expected, reinterpret_cast<jbyte*>(rgba.data()));

    glBindTexture(GL_TEXTURE_2D, gImageTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_setViewScale(JNIEnv* env, jobject /*thiz*/, jfloat scale) {
    // 防止除零或过小值导致视觉异常
    gViewScale = (scale < 1e-4f) ? 1e-4f : scale;
    if (gViewTransformLogBudget.fetch_sub(1) > 0) {
        LOGI("setViewScale: %.6f", gViewScale);
    }
    gVisibleDirty.store(1);
    resetProgress();
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_setViewTransform(JNIEnv* env, jobject /*thiz*/, jfloat scale, jfloat cx, jfloat cy) {
    gViewScale = (scale < 1e-4f) ? 1e-4f : scale;
    gViewTranslateX = cx;
    gViewTranslateY = cy;
    if (gViewTransformLogBudget.fetch_sub(1) > 0) {
        LOGI("setViewTransform: scale=%.6f translate=(%.2f,%.2f)", gViewScale, gViewTranslateX, gViewTranslateY);
    }
    gVisibleDirty.store(1);
    resetProgress();
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_setInteractionState(JNIEnv* env, jobject /*thiz*/, jboolean isInteracting, jlong timestampMs) {
    (void)env;
    gIsInteracting.store(isInteracting ? 1 : 0);
    gLastInteractionMs.store((int64_t)timestampMs);
    resetProgress();
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_setRenderMaxPoints(JNIEnv* env, jobject /*thiz*/, jint maxPoints) {
    gRenderMaxPoints.store(std::clamp((int)maxPoints, 1, 1024));
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_beginLiveStroke(JNIEnv* env, jobject /*thiz*/, jfloatArray color, jint type) {
    if (env && color && env->GetArrayLength(color) >= 4) {
        env->GetFloatArrayRegion(color, 0, 4, gLiveColor);
    }
    gLiveActive = true;
    gGestureStartStrokeId = gUseSSBO ? (int)gMetas.size() : -1;
    gLiveMeta.start = 0;
    gLiveMeta.count = 0;
    gLiveMeta.baseWidth = 16.0f;
    gLiveMeta.pad = 0.0f;
    gLiveMeta.color[0] = gLiveColor[0];
    gLiveMeta.color[1] = gLiveColor[1];
    gLiveMeta.color[2] = gLiveColor[2];
    gLiveMeta.color[3] = gLiveColor[3];
    gLiveMeta.type = (float)type;
    gLiveMeta.reserved0 = 0.0f;
    gLiveMeta.reserved1 = 0.0f;
    gLiveMeta.reserved2 = 0.0f;
    gHasLiveBounds = false;
    gVisibleDirty.store(1);

    if (!gUseSSBO) {
        int liveId = gFallbackStrokeCount.load();
        if (liveId < 0) liveId = 0;
        if (!ensureFallbackStorageCapacity(liveId + 1)) return;
        writeFallbackMeta(liveId, 0, 16.0f, 0.0f, (float)type, gLiveColor, 0.0f, 0.0f, 0.0f, 0.0f);
    }
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_updateLiveStroke(JNIEnv* env, jobject /*thiz*/, jfloatArray points, jfloatArray pressures) {
    if (!gLiveActive) return;
    if (!env || !points || !pressures) return;

    jsize pLen = env->GetArrayLength(points);
    jsize prLen = env->GetArrayLength(pressures);
    if (pLen < 4 || prLen < 2) return;

    int N = (int)prLen;
    if (N > kMaxPointsPerStroke) N = kMaxPointsPerStroke;
    if (pLen < (jsize)(N * 2)) return;

    std::vector<float> pts((size_t)N * 2u);
    std::vector<float> prs((size_t)N);
    env->GetFloatArrayRegion(points, 0, N * 2, pts.data());
    env->GetFloatArrayRegion(pressures, 0, N, prs.data());
    gLiveBounds = computeBoundsFromPoints(pts.data(), N);
    gHasLiveBounds = true;

    if (!gUseSSBO) {
        int liveId = gFallbackStrokeCount.load();
        if (liveId < 0) liveId = 0;
        if (!ensureFallbackStorageCapacity(liveId + 1)) return;
        StrokeBoundsCPU b = gLiveBounds;
        float spanX = b.maxX - b.minX;
        float spanY = b.maxY - b.minY;
        writeFallbackPoints(liveId, pts.data(), prs.data(), N, b.minX, b.minY, spanX, spanY);
        writeFallbackMeta(liveId, N, 16.0f, 0.0f, gLiveMeta.type, gLiveColor, b.minX, b.minY, spanX, spanY);
        return;
    }

    int strokeId = (int)gMetas.size();
    ensureCapacityForStrokes((size_t)strokeId + 1u);
    int start = strokeId * kMaxPointsPerStroke;

    std::vector<float> posWrite((size_t)N * 2u);
    for (int i = 0; i < N; ++i) {
        float x = pts[(size_t)i * 2u + 0u];
        float y = pts[(size_t)i * 2u + 1u];
        posWrite[(size_t)i * 2u + 0u] = x;
        posWrite[(size_t)i * 2u + 1u] = y;
    }

    if (gUseSSBO && gPositionsSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPositionsSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(start * sizeof(float) * 2),
                        (GLsizeiptr)(posWrite.size() * sizeof(float)),
                        posWrite.data());
    }
    if (gUseSSBO && gPressuresSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPressuresSSBO);
        size_t startWord = (size_t)start >> 1;
        std::vector<uint32_t> packed(packedPressureCount((size_t)N), 0u);
        for (int i = 0; i < N; ++i) {
            setPackedPressure(packed, (size_t)i, floatToUnorm16(prs[(size_t)i]));
        }
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(startWord * sizeof(uint32_t)),
                        (GLsizeiptr)(packed.size() * sizeof(uint32_t)),
                        packed.data());
    }

    StrokeMetaCPU meta;
    meta.start = start;
    meta.count = N;
    meta.baseWidth = 16.0f;
    meta.pad = 0.0f;
    meta.color[0] = gLiveColor[0];
    meta.color[1] = gLiveColor[1];
    meta.color[2] = gLiveColor[2];
    meta.color[3] = gLiveColor[3];
    meta.type = gLiveMeta.type;
    meta.reserved0 = 0.0f;
    meta.reserved1 = 0.0f;
    meta.reserved2 = 0.0f;
    gLiveMeta = meta;
    if (gUseSSBO && gStrokeMetaSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gStrokeMetaSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(strokeId * sizeof(StrokeMetaCPU)),
                        (GLsizeiptr)sizeof(StrokeMetaCPU),
                        &meta);
    }
    gVisibleDirty.store(1);
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_updateLiveStrokeWithCount(JNIEnv* env, jobject /*thiz*/, jfloatArray points, jfloatArray pressures, jint count) {
    if (!gLiveActive) return;
    if (!env || !points || !pressures) return;
    if (count <= 0) return;

    jsize pLen = env->GetArrayLength(points);
    jsize prLen = env->GetArrayLength(pressures);
    int N = (int)count;
    if (N > kMaxPointsPerStroke) N = kMaxPointsPerStroke;
    if (pLen < (jsize)(N * 2) || prLen < (jsize)N) return;

    std::vector<float> pts((size_t)N * 2u);
    std::vector<float> prs((size_t)N);
    env->GetFloatArrayRegion(points, 0, N * 2, pts.data());
    env->GetFloatArrayRegion(pressures, 0, N, prs.data());
    gLiveBounds = computeBoundsFromPoints(pts.data(), N);
    gHasLiveBounds = true;

    if (!gUseSSBO) {
        int liveId = gFallbackStrokeCount.load();
        if (liveId < 0) liveId = 0;
        if (!ensureFallbackStorageCapacity(liveId + 1)) return;
        StrokeBoundsCPU b = gLiveBounds;
        float spanX = b.maxX - b.minX;
        float spanY = b.maxY - b.minY;
        writeFallbackPoints(liveId, pts.data(), prs.data(), N, b.minX, b.minY, spanX, spanY);
        writeFallbackMeta(liveId, N, 16.0f, 0.0f, gLiveMeta.type, gLiveColor, b.minX, b.minY, spanX, spanY);
        return;
    }

    int strokeId = (int)gMetas.size();
    ensureCapacityForStrokes((size_t)strokeId + 1u);
    int start = strokeId * kMaxPointsPerStroke;

    std::vector<float> posWrite((size_t)N * 2u);
    for (int i = 0; i < N; ++i) {
        float x = pts[(size_t)i * 2u + 0u];
        float y = pts[(size_t)i * 2u + 1u];
        posWrite[(size_t)i * 2u + 0u] = x;
        posWrite[(size_t)i * 2u + 1u] = y;
    }

    if (gUseSSBO && gPositionsSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPositionsSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(start * sizeof(float) * 2),
                        (GLsizeiptr)(posWrite.size() * sizeof(float)),
                        posWrite.data());
    }
    if (gUseSSBO && gPressuresSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPressuresSSBO);
        size_t startWord = (size_t)start >> 1;
        std::vector<uint32_t> packed(packedPressureCount((size_t)N), 0u);
        for (int i = 0; i < N; ++i) {
            setPackedPressure(packed, (size_t)i, floatToUnorm16(prs[(size_t)i]));
        }
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(startWord * sizeof(uint32_t)),
                        (GLsizeiptr)(packed.size() * sizeof(uint32_t)),
                        packed.data());
    }

    StrokeMetaCPU meta;
    meta.start = start;
    meta.count = N;
    meta.baseWidth = 16.0f;
    meta.pad = 0.0f;
    meta.color[0] = gLiveColor[0];
    meta.color[1] = gLiveColor[1];
    meta.color[2] = gLiveColor[2];
    meta.color[3] = gLiveColor[3];
    meta.type = gLiveMeta.type;
    meta.reserved0 = 0.0f;
    meta.reserved1 = 0.0f;
    meta.reserved2 = 0.0f;
    gLiveMeta = meta;
    if (gUseSSBO && gStrokeMetaSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gStrokeMetaSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(strokeId * sizeof(StrokeMetaCPU)),
                        (GLsizeiptr)sizeof(StrokeMetaCPU),
                        &meta);
    }
    gVisibleDirty.store(1);
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_endLiveStroke(JNIEnv* env, jobject /*thiz*/) {
    (void)env;
    if (!gUseSSBO) {
        gLiveActive = false;
        gLiveMeta.count = 0;
        gHasLiveBounds = false;
        gVisibleDirty.store(1);
        return;
    }
    int startId = gGestureStartStrokeId;
    int endId = (int)gMetas.size();
    if (startId < 0) startId = endId;
    if (startId < endId) {
        int changed = 0;
        for (int i = startId; i < endId; ++i) {
            if (gMetas[i].pad <= 0.5f) {
                gMetas[i].pad = 1.0f;
                changed++;
            }
        }
        if (changed > 0) {
            gDarkenStrokeCount += changed;
            if (gUseSSBO && gStrokeMetaSSBO) {
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, gStrokeMetaSSBO);
                glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                                (GLintptr)(startId * sizeof(StrokeMetaCPU)),
                                (GLsizeiptr)((endId - startId) * (int)sizeof(StrokeMetaCPU)),
                                &gMetas[(size_t)startId]);
            }
        }
    }
    gGestureStartStrokeId = -1;
    gLiveActive = false;
    gLiveMeta.count = 0;
    gHasLiveBounds = false;
    gVisibleDirty.store(1);
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_addStroke(JNIEnv* env, jobject /*thiz*/,
                                                      jfloatArray points,
                                                      jfloatArray pressures,
                                                      jfloatArray color,
                                                      jint type) {
    jsize pLen = env->GetArrayLength(points);    // 2*N
    jsize prLen = env->GetArrayLength(pressures);// N
    jsize cLen = env->GetArrayLength(color);     // 4
    if (pLen <= 0 || prLen <= 0 || cLen < 4) return;

    int N = (int)prLen;
    if (pLen < N * 2) return;
    if (N > kMaxPointsPerStroke) N = kMaxPointsPerStroke;

    std::vector<float> pts(pLen);
    std::vector<float> prs(prLen);
    std::vector<float> col(4);
    env->GetFloatArrayRegion(points, 0, pLen, pts.data());
    env->GetFloatArrayRegion(pressures, 0, prLen, prs.data());
    env->GetFloatArrayRegion(color, 0, 4, col.data());
    bool ready = gGlReady && (gUseSSBO ? (gProgram != 0) : (gTexProgram != 0));
    if (!ready) {
        PendingStroke ps;
        ps.points = std::move(pts);
        ps.pressures = std::move(prs);
        ps.color = std::move(col);
        ps.type = (int)type;
        gPendingStrokes.push_back(std::move(ps));
        if (gQueueLogBudget.fetch_sub(1) > 0) {
            LOGW("addStroke queued (GL not ready): count=%d", N);
        }
        return;
    }

    uploadStroke(pts, prs, col, (int)type);
    gVisibleDirty.store(1);
}

JNIEXPORT jint JNICALL
Java_com_example_myapplication_NativeBridge_getStrokeCount(JNIEnv *env, jobject /* this */) {
    (void)env;
    return gUseSSBO ? (jint)gMetas.size() : (jint)gFallbackStrokeCount.load();
}

JNIEXPORT jint JNICALL
Java_com_example_myapplication_NativeBridge_getBlueStrokeCount(JNIEnv *env, jobject /* this */) {
    (void)env;
    if (!gUseSSBO) return 0;
    int blueCount = 0;
    for (const auto& meta : gMetas) {
        // 检查是否为蓝色笔划 (0.1f, 0.4f, 1.0f, 0.85f)
        if (meta.color[0] >= 0.05f && meta.color[0] <= 0.15f &&
            meta.color[1] >= 0.35f && meta.color[1] <= 0.45f &&
            meta.color[2] >= 0.95f && meta.color[2] <= 1.05f &&
            meta.color[3] >= 0.80f && meta.color[3] <= 0.90f) {
            blueCount++;
        }
    }
    return (jint)blueCount;
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_addStrokeBatch(JNIEnv* env, jobject /*thiz*/,
                                                           jfloatArray points,
                                                           jfloatArray pressures,
                                                           jintArray counts,
                                                           jfloatArray colors,
                                                           jintArray types) {
    jsize pLen = env->GetArrayLength(points);
    jsize prLen = env->GetArrayLength(pressures);
    jsize cLen = env->GetArrayLength(colors);
    jsize cntLen = env->GetArrayLength(counts);
    jsize tLen = types ? env->GetArrayLength(types) : 0;
    if (cntLen <= 0 || pLen <= 0 || prLen <= 0 || cLen < cntLen * 4 || tLen < cntLen) return;

    bool ready = gGlReady && (gUseSSBO ? (gProgram != 0) : (gTexProgram != 0));
    if (!ready) {
        std::vector<float> ptsFlat(pLen);
        std::vector<float> prsFlat(prLen);
        std::vector<int> cnts(cntLen);
        std::vector<float> colsFlat(cLen);
        std::vector<int> typesFlat((size_t)cntLen);
        env->GetFloatArrayRegion(points, 0, pLen, ptsFlat.data());
        env->GetFloatArrayRegion(pressures, 0, prLen, prsFlat.data());
        env->GetIntArrayRegion(counts, 0, cntLen, cnts.data());
        env->GetFloatArrayRegion(colors, 0, cLen, colsFlat.data());
        env->GetIntArrayRegion(types, 0, cntLen, typesFlat.data());

        int pi = 0, pri = 0;
        for (int s = 0; s < cntLen; ++s) {
            int nOrig = cnts[s];
            nOrig = nOrig < 0 ? 0 : nOrig;
            int n = nOrig > kMaxPointsPerStroke ? kMaxPointsPerStroke : nOrig;
            std::vector<float> pts((size_t)n * 2u);
            std::vector<float> prs((size_t)n);
            for (int i = 0; i < n; ++i) {
                pts[(size_t)i * 2u + 0u] = ptsFlat[(size_t)pi + (size_t)i * 2u + 0u];
                pts[(size_t)i * 2u + 1u] = ptsFlat[(size_t)pi + (size_t)i * 2u + 1u];
                prs[(size_t)i] = prsFlat[(size_t)pri + (size_t)i];
            }
            pi += nOrig * 2;
            pri += nOrig;
            PendingStroke ps;
            ps.points = std::move(pts);
            ps.pressures = std::move(prs);
            ps.color = { colsFlat[(size_t)s * 4u + 0u], colsFlat[(size_t)s * 4u + 1u], colsFlat[(size_t)s * 4u + 2u], colsFlat[(size_t)s * 4u + 3u] };
            ps.type = typesFlat[(size_t)s];
            gPendingStrokes.push_back(std::move(ps));
        }
        if (gQueueLogBudget.fetch_sub(1) > 0) {
            int t0 = cntLen > 0 ? typesFlat[0] : -1;
            float c0r = cLen >= 4 ? colsFlat[0] : -1.0f;
            float c0g = cLen >= 4 ? colsFlat[1] : -1.0f;
            float c0b = cLen >= 4 ? colsFlat[2] : -1.0f;
            float c0a = cLen >= 4 ? colsFlat[3] : -1.0f;
            LOGW("addStrokeBatch queued (GL not ready): strokes=%d t0=%d c0=(%.2f,%.2f,%.2f,%.2f)",
                 (int)cntLen, t0, c0r, c0g, c0b, c0a);
        }
        return;
    }

    if (!gUseSSBO) {
        jboolean copyPts = JNI_FALSE;
        jboolean copyPrs = JNI_FALSE;
        jboolean copyCols = JNI_FALSE;
        jboolean copyCnts = JNI_FALSE;
        jboolean copyTypes = JNI_FALSE;
        const float* ptsPtr = env->GetFloatArrayElements(points, &copyPts);
        const float* prsPtr = env->GetFloatArrayElements(pressures, &copyPrs);
        const float* colsPtr = env->GetFloatArrayElements(colors, &copyCols);
        const jint* cntPtr = env->GetIntArrayElements(counts, &copyCnts);
        const jint* typePtr = env->GetIntArrayElements(types, &copyTypes);
        if (!ptsPtr || !prsPtr || !colsPtr || !cntPtr || !typePtr) {
            if (ptsPtr) env->ReleaseFloatArrayElements(points, const_cast<jfloat*>(ptsPtr), JNI_ABORT);
            if (prsPtr) env->ReleaseFloatArrayElements(pressures, const_cast<jfloat*>(prsPtr), JNI_ABORT);
            if (colsPtr) env->ReleaseFloatArrayElements(colors, const_cast<jfloat*>(colsPtr), JNI_ABORT);
            if (cntPtr) env->ReleaseIntArrayElements(counts, const_cast<jint*>(cntPtr), JNI_ABORT);
            if (typePtr) env->ReleaseIntArrayElements(types, const_cast<jint*>(typePtr), JNI_ABORT);
            return;
        }

        int startId = gFallbackStrokeCount.load();
        int needed = startId + (int)cntLen;
        if (!ensureFallbackStorageCapacity(needed)) {
            env->ReleaseFloatArrayElements(points, const_cast<jfloat*>(ptsPtr), JNI_ABORT);
            env->ReleaseFloatArrayElements(pressures, const_cast<jfloat*>(prsPtr), JNI_ABORT);
            env->ReleaseFloatArrayElements(colors, const_cast<jfloat*>(colsPtr), JNI_ABORT);
            env->ReleaseIntArrayElements(counts, const_cast<jint*>(cntPtr), JNI_ABORT);
            env->ReleaseIntArrayElements(types, const_cast<jint*>(typePtr), JNI_ABORT);
            LOGE("Fallback: ensure storage failed for batch, needed=%d", needed);
            return;
        }

        int pi = 0;
        int pri = 0;
        for (int s = 0; s < (int)cntLen; ++s) {
            int nOrig = (int)cntPtr[s];
            int nSafe = nOrig < 0 ? 0 : nOrig;
            int n = nSafe > kMaxPointsPerStroke ? kMaxPointsPerStroke : nSafe;
            int strokeId = startId + s;

            float c[4] = {
                colsPtr[s * 4 + 0],
                colsPtr[s * 4 + 1],
                colsPtr[s * 4 + 2],
                colsPtr[s * 4 + 3]
            };
            float t = (float)typePtr[s];
            if (n > 0) {
                const float* pxy = ptsPtr + (size_t)pi;
                StrokeBoundsCPU b = computeBoundsFromPoints(pxy, n);
                float spanX = b.maxX - b.minX;
                float spanY = b.maxY - b.minY;
                writeFallbackPoints(strokeId, pxy, prsPtr + (size_t)pri, n, b.minX, b.minY, spanX, spanY);
                writeFallbackMeta(strokeId, n, 16.0f, 0.0f, t, c, b.minX, b.minY, spanX, spanY);
            } else {
                writeFallbackMeta(strokeId, n, 16.0f, 0.0f, t, c, 0.0f, 0.0f, 0.0f, 0.0f);
            }
            pi += nSafe * 2;
            pri += nSafe;
        }

        gFallbackStrokeCount.store(needed);
        env->ReleaseFloatArrayElements(points, const_cast<jfloat*>(ptsPtr), JNI_ABORT);
        env->ReleaseFloatArrayElements(pressures, const_cast<jfloat*>(prsPtr), JNI_ABORT);
        env->ReleaseFloatArrayElements(colors, const_cast<jfloat*>(colsPtr), JNI_ABORT);
        env->ReleaseIntArrayElements(counts, const_cast<jint*>(cntPtr), JNI_ABORT);
        env->ReleaseIntArrayElements(types, const_cast<jint*>(typePtr), JNI_ABORT);
        return;
    }

    std::vector<float> ptsFlat(pLen);
    std::vector<float> prsFlat(prLen);
    std::vector<int> cnts(cntLen);
    std::vector<float> colsFlat(cLen);
    std::vector<int> typesFlat((size_t)cntLen);
    env->GetFloatArrayRegion(points, 0, pLen, ptsFlat.data());
    env->GetFloatArrayRegion(pressures, 0, prLen, prsFlat.data());
    env->GetIntArrayRegion(counts, 0, cntLen, cnts.data());
    env->GetFloatArrayRegion(colors, 0, cLen, colsFlat.data());
    env->GetIntArrayRegion(types, 0, cntLen, typesFlat.data());

    if (!gGlReady || !gProgram) {
        int pi = 0, pri = 0;
        for (int s = 0; s < cntLen; ++s) {
            int n = cnts[s];
            n = n < 0 ? 0 : n;
            n = n > kMaxPointsPerStroke ? kMaxPointsPerStroke : n;
            std::vector<float> pts(n * 2);
            std::vector<float> prs(n);
            for (int i = 0; i < n; ++i) {
                pts[i * 2 + 0] = ptsFlat[pi + i * 2 + 0];
                pts[i * 2 + 1] = ptsFlat[pi + i * 2 + 1];
                prs[i] = prsFlat[pri + i];
            }
            pi += n * 2; pri += n;
            PendingStroke ps;
            ps.points = std::move(pts);
            ps.pressures = std::move(prs);
            ps.color = { colsFlat[s * 4 + 0], colsFlat[s * 4 + 1], colsFlat[s * 4 + 2], colsFlat[s * 4 + 3] };
            ps.type = typesFlat[(size_t)s];
            gPendingStrokes.push_back(std::move(ps));
        }
        if (gQueueLogBudget.fetch_sub(1) > 0) {
            LOGW("addStrokeBatch queued (GL not ready): strokes=%d", (int)cntLen);
        }
        return;
    }

    int startId = (int)gMetas.size();
    int S = (int)cntLen;
    int neededStrokes = startId + S;
    if (neededStrokes > gAllocatedStrokes) {
        int newCap = gAllocatedStrokes;
        while (newCap < neededStrokes) newCap *= 2;
        size_t pointsCapacity = (size_t)newCap * kMaxPointsPerStroke;

        if (gUseSSBO) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPositionsSSBO);
            glBufferData(GL_SHADER_STORAGE_BUFFER, pointsCapacity * sizeof(float) * 2, nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gPositionsSSBO);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, gStrokeMetaSSBO);
            glBufferData(GL_SHADER_STORAGE_BUFFER, newCap * sizeof(StrokeMetaCPU), nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, gStrokeMetaSSBO);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPressuresSSBO);
            glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(packedPressureCount(pointsCapacity) * sizeof(uint32_t)), nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, gPressuresSSBO);
        }
        gAllocatedStrokes = newCap;
    }

    size_t blockPts = (size_t)S * kMaxPointsPerStroke;
    std::vector<float> positionsBatch(blockPts * 2, 0.0f);
    std::vector<uint32_t> packedPressuresBatch(packedPressureCount(blockPts), 0u);
    std::vector<StrokeMetaCPU> metasBatch; metasBatch.reserve(S);
    if ((int)gBounds.size() < startId) gBounds.resize((size_t)startId);

    int pi = 0, pri = 0;
    int totalPoints = 0;
    for (int s = 0; s < S; ++s) {
        int nOrig = cnts[s];
        int n = nOrig < 0 ? 0 : (nOrig > kMaxPointsPerStroke ? kMaxPointsPerStroke : nOrig);
        totalPoints += n;
        size_t base = (size_t)s * kMaxPointsPerStroke;
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
        if (n > 0) {
            minX = maxX = ptsFlat[pi + 0];
            minY = maxY = ptsFlat[pi + 1];
        }
        for (int i = 0; i < n; ++i) {
            float x = ptsFlat[pi + i * 2 + 0];
            float y = ptsFlat[pi + i * 2 + 1];
            float pr = prsFlat[pri + i];
            if (i > 0) {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
            positionsBatch[(base + i) * 2 + 0] = x;
            positionsBatch[(base + i) * 2 + 1] = y;
            setPackedPressure(packedPressuresBatch, base + (size_t)i, floatToUnorm16(pr));
        }
        pi += n * 2; pri += n;

        StrokeMetaCPU m;
        m.start = (startId + s) * kMaxPointsPerStroke;
        m.count = n;
        m.baseWidth = 16.0f;
        m.pad = 0.0f;
        m.color[0] = colsFlat[s * 4 + 0];
        m.color[1] = colsFlat[s * 4 + 1];
        m.color[2] = colsFlat[s * 4 + 2];
        m.color[3] = colsFlat[s * 4 + 3];
        m.type = (float)typesFlat[(size_t)s];
        m.reserved0 = 0.0f;
        m.reserved1 = 0.0f;
        m.reserved2 = 0.0f;
        metasBatch.push_back(m);
        gMetas.push_back(m);

        StrokeBoundsCPU b{minX, minY, maxX, maxY};
        gBounds.push_back(b);
    }

    // 确保容量足够（可能增长并复制旧数据）
    ensureCapacityForStrokes((size_t)gMetas.size());
    size_t globalStart = (size_t)startId * kMaxPointsPerStroke;
    if (gUseSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPositionsSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(globalStart * sizeof(float) * 2),
                        (GLsizeiptr)(positionsBatch.size() * sizeof(float)),
                        positionsBatch.data());
        // 提交压力 SSBO
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPressuresSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)((globalStart >> 1) * sizeof(uint32_t)),
                        (GLsizeiptr)(packedPressuresBatch.size() * sizeof(uint32_t)),
                        packedPressuresBatch.data());
        // 提交元数据 SSBO（按条上传）
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gStrokeMetaSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(startId * sizeof(StrokeMetaCPU)),
                        (GLsizeiptr)(metasBatch.size() * sizeof(StrokeMetaCPU)),
                        metasBatch.data());
    }

    if (gBatchUploadLogBudget.fetch_sub(1) > 0) {
        LOGI("addStrokeBatch(uploaded): strokes=%d totalPoints=%d startId=%d", S, totalPoints, startId);
    }
    gVisibleDirty.store(1);
}

}
