// Copyright-free. 第二阶段：JNI数据管理、缓冲与GLSL着色器实现。
#include <jni.h>
#include <android/log.h>
#include <GLES3/gl31.h>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <cstdint>

#define LOG_TAG "NativeLib"
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
static GLuint gVAO = 0;
static GLuint gEmptyVAO = 0; // 用于 SSBO 渲染路径的空 VAO
static GLuint gBypassVBO = 0;
static GLuint gPointsBuffer = 0;    // VBO: half(x,y,pressure)
static GLuint gPositionsSSBO = 0;   // SSBO(binding=1): float32 vec2 positions
static GLuint gStrokeMetaSSBO = 0;  // SSBO(binding=0): stroke metadata
static GLuint gPressuresSSBO = 0;  // SSBO(binding=2): float32 pressures
static GLuint gVisibleIndexSSBO = 0; // SSBO(binding=3): visible stroke id list
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
static std::atomic<int> gFirstFrameLogOnce{1};
static bool gUseFramebufferFetch = false;
static bool gUseFramebufferFetchEXT = false;
static int gGestureStartStrokeId = -1;
static int gDarkenStrokeCount = 0;
static int gVisibleIndexCapacity = 0;
static int gVisibleCount = 0;
static std::atomic<int> gVisibleDirty{1};

// CPU侧元数据
struct StrokeMetaCPU {
    int start;
    int count;
    float baseWidth;
    float pad;
    float color[4];
};
static std::vector<StrokeMetaCPU> gMetas;
struct StrokeBoundsCPU {
    float minX;
    float minY;
    float maxX;
    float maxY;
};
static std::vector<StrokeBoundsCPU> gBounds;
static std::vector<uint32_t> gVisibleIdsCPU;
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
};
static std::vector<PendingStroke> gPendingStrokes;

// 计算需要的点容量（按笔划数与每条最大点数）
static size_t pointsCapacityByStrokes(size_t strokes) {
    return strokes * (size_t)kMaxPointsPerStroke;
}

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
        gPressuresSSBO = resizeBufferCopy(GL_SHADER_STORAGE_BUFFER,
                                          gPressuresSSBO,
                                          (GLsizeiptr)(oldPointsCap * sizeof(float)),
                                          (GLsizeiptr)(newPointsCap * sizeof(float)));
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
                                             (GLsizeiptr)((size_t)oldCap * sizeof(uint32_t)),
                                             (GLsizeiptr)((size_t)newCap * sizeof(uint32_t)));
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
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)((size_t)newCap * sizeof(uint32_t)), nullptr, GL_DYNAMIC_DRAW);
    } else {
        gVisibleIndexSSBO = resizeBufferCopy(GL_SHADER_STORAGE_BUFFER,
                                             gVisibleIndexSSBO,
                                             (GLsizeiptr)((size_t)gVisibleIndexCapacity * sizeof(uint32_t)),
                                             (GLsizeiptr)((size_t)newCap * sizeof(uint32_t)));
    }
    gVisibleIndexCapacity = newCap;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, gVisibleIndexSSBO);
}

static void updateVisibleListIfNeeded() {
    if (!gUseSSBO || !gVisibleIndexSSBO) return;
    if (gVisibleDirty.load() == 0) return;

    int committed = (int)gMetas.size();
    int total = committed + (gLiveActive ? 1 : 0);
    if (total <= 0) {
        gVisibleIdsCPU.clear();
        gVisibleCount = 0;
        gVisibleDirty.store(0);
        return;
    }

    ensureVisibleIndexCapacity(total);
    gVisibleIdsCPU.clear();
    gVisibleIdsCPU.reserve((size_t)total);

    float w = (float)g_Width;
    float h = (float)g_Height;
    float pad = 16.0f * gViewScale;
    if (w <= 0.0f || h <= 0.0f) {
        for (int i = 0; i < total; ++i) gVisibleIdsCPU.push_back((uint32_t)i);
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
            gVisibleIdsCPU.push_back((uint32_t)i);
        }
        for (int i = n; i < committed; ++i) {
            if (gMetas[(size_t)i].count <= 0) continue;
            gVisibleIdsCPU.push_back((uint32_t)i);
        }
        if (gLiveActive) {
            bool vis = true;
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
            }
            if (vis) gVisibleIdsCPU.push_back((uint32_t)committed);
        }
    }

    gVisibleCount = (int)gVisibleIdsCPU.size();
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, gVisibleIndexSSBO);
    if (gVisibleCount > 0) {
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)((size_t)gVisibleCount * sizeof(uint32_t)), gVisibleIdsCPU.data());
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, gVisibleIndexSSBO);
    gVisibleDirty.store(0);
}

// 将一条笔划上传到GPU缓冲，并更新CPU侧元数据
static void uploadStroke(const std::vector<float>& pts,
                         const std::vector<float>& prs,
                         const std::vector<float>& col) {
    if (pts.empty() || prs.empty() || col.size() < 4) return;
    int N = (int)prs.size();
    if ((int)pts.size() < N * 2) return;
    if (N > kMaxPointsPerStroke) N = kMaxPointsPerStroke;

    int strokeId = (int)gMetas.size();
    int neededStrokes = strokeId + 1;
    if (neededStrokes > gAllocatedStrokes) {
        int newCap = gAllocatedStrokes;
        while (newCap < neededStrokes) newCap *= 2;
        size_t pointsCapacity = (size_t)newCap * kMaxPointsPerStroke;

        glBindBuffer(GL_ARRAY_BUFFER, gPointsBuffer);
        size_t elemSizeVBO2 = gHasVertexHalfFloat ? (sizeof(uint16_t) * 3) : (sizeof(float) * 3);
        glBufferData(GL_ARRAY_BUFFER, pointsCapacity * elemSizeVBO2, nullptr, GL_DYNAMIC_DRAW);
        if (gUseSSBO) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPositionsSSBO);
            glBufferData(GL_SHADER_STORAGE_BUFFER, pointsCapacity * sizeof(float) * 2, nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gPositionsSSBO);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, gStrokeMetaSSBO);
            glBufferData(GL_SHADER_STORAGE_BUFFER, newCap * sizeof(StrokeMetaCPU), nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, gStrokeMetaSSBO);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPressuresSSBO);
            glBufferData(GL_SHADER_STORAGE_BUFFER, pointsCapacity * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, gPressuresSSBO);
        }
        gAllocatedStrokes = newCap;
    }

    int start = strokeId * kMaxPointsPerStroke;
    std::vector<float> posWrite(N * 2);
    std::vector<uint16_t> vboWrite16;
    std::vector<float> vboWriteF;
    if (gHasVertexHalfFloat) vboWrite16.resize(N * 3); else vboWriteF.resize(N * 3);
    for (int i = 0; i < N; ++i) {
        float x = pts[i * 2 + 0];
        float y = pts[i * 2 + 1];
        float pr = prs[i];
        posWrite[i * 2 + 0] = x;
        posWrite[i * 2 + 1] = y;
        if (gHasVertexHalfFloat) {
            vboWrite16[i * 3 + 0] = floatToHalf(x);
            vboWrite16[i * 3 + 1] = floatToHalf(y);
            vboWrite16[i * 3 + 2] = floatToHalf(pr);
        } else {
            vboWriteF[i * 3 + 0] = x;
            vboWriteF[i * 3 + 1] = y;
            vboWriteF[i * 3 + 2] = pr;
        }
    }

    if (gUseSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPositionsSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, (GLintptr)(start * sizeof(float) * 2), (GLsizeiptr)(posWrite.size() * sizeof(float)), posWrite.data());
    }
    glBindBuffer(GL_ARRAY_BUFFER, gPointsBuffer);
    if (gHasVertexHalfFloat) {
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(start * sizeof(uint16_t) * 3), (GLsizeiptr)(vboWrite16.size() * sizeof(uint16_t)), vboWrite16.data());
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(start * sizeof(float) * 3), (GLsizeiptr)(vboWriteF.size() * sizeof(float)), vboWriteF.data());
    }
    if (gUseSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPressuresSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, (GLintptr)(start * sizeof(float)), (GLsizeiptr)(prs.size() * sizeof(float)), prs.data());
    }

    StrokeMetaCPU meta;
    meta.start = start;
    meta.count = N;
    meta.baseWidth = 16.0f;
    meta.pad = 0.0f;
    meta.color[0] = col[0]; meta.color[1] = col[1]; meta.color[2] = col[2]; meta.color[3] = col[3];
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
        LOGI("addStroke(uploaded): id=%d, count=%d width=%.1f color=(%.2f,%.2f,%.2f,%.2f) first=(%.1f,%.1f) last=(%.1f,%.1f)",
             strokeId, N, meta.baseWidth, col[0], col[1], col[2], col[3], firstX, firstY, lastX, lastY);
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
// - 宽度：为保持“物理粗细”不随缩放变化，宽度不跟随uViewScale缩放：
//         radius = baseWidth * pressure * 0.5
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
};

layout(std430, binding=0) buffer StrokeMetaBuf {
    StrokeMeta metas[];
};

layout(std430, binding=1) buffer PositionsBuf { vec2 positions[]; };
layout(std430, binding=2) buffer PressuresBuf { float pressures[]; };
layout(std430, binding=3) buffer VisibleIndexBuf { uint visibleIds[]; };

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

highp vec2 safeNormalize(highp vec2 v) {
    highp float l = length(v);
    if (l < 1e-4) return vec2(1.0, 0.0);
    return v / l;
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
}

void main() {
    vec3 dummy = aStrictCheckBypass * 0.000001;
    
    int visibleIndex = gl_InstanceID + int(uBaseInstance);
    int strokeId = int(visibleIds[visibleIndex]);

    int start = metas[strokeId].start;
    int count = metas[strokeId].count;
    float effect = metas[strokeId].pad;
    vEffect = effect;
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
    int maxPoints = clamp(uRenderMaxPoints, 1, 1024);
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
    // 线宽以屏幕像素为准，不随缩放变粗/变细
    float r0 = metas[strokeId].baseWidth * pressures[start] * uViewScale * 0.5;
    float rN = metas[strokeId].baseWidth * pressures[start + lastPointIdx] * uViewScale * 0.5;

    int vid = gl_VertexID;
    if (vid < 0 || vid >= kTotalVerts) {
        setOffscreen();
        return;
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
            setOffscreen();
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
        float pressure = pressures[idx];
        // 修复：笔身宽度也需要随视图缩放，否则会变成细线
        float radius = metas[strokeId].baseWidth * pressure * uViewScale * 0.5;

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
            setOffscreen();
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
    // 使用strokeId生成稳定的深度顺序，尽量避免大量笔划相互覆盖时的闪烁
    float t = (float(strokeId) + 0.5) / max(uStrokeCount, 1.0);
    float depth01 = 1.0 - t;
    float zNdc = depth01 * 2.0 - 1.0;
    gl_Position = vec4(ndc, zNdc, 1.0);
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

out vec4 fragColor;

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

    float useCap = step(0.5, vMode);
    float alpha = mix(alphaBody, alphaCap, useCap);
    float outA = vColor.a * alpha;
    fragColor = vec4(vColor.rgb * outA, outA);
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

layout(location = 0) inout vec4 fragColor;

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

    float useCap = step(0.5, vMode);
    float alpha = mix(alphaBody, alphaCap, useCap);

    // 源颜色：预乘alpha
    float Sa = vColor.a * alpha;
    vec3 S = vColor.rgb;
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

out vec4 fragColor;

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

    float useCap = step(0.5, vMode);
    float alpha = mix(alphaBody, alphaCap, useCap);

    float Sa = vColor.a * alpha;
    vec3 S = vColor.rgb;
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

extern "C" {

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_onNativeSurfaceCreated(JNIEnv* env, jobject /*thiz*/) {
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
    if (verStr && (strstr(verStr, "OpenGL ES 3.2") || strstr(verStr, "OpenGL ES 3.1"))) {
        GLint maxVertexSsbo = 0;
        GLint maxSsboBindings = 0;
        glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &maxVertexSsbo);
        glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &maxSsboBindings);

        // 一些设备对上面两个上限的返回值可能偏保守；这里改为“先尝试编译SSBO版本”，
        // 只有在编译/链接失败时才回退到ES3.0的简化线段绘制路径。
        gUseSSBO = true;
        GLuint vs = compileShader(GL_VERTEX_SHADER, kVS);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFS);
        gProgram = linkProgram2(vs, fs);
        if (!gProgram) {
            gUseSSBO = false;
            GLuint fvs = compileShader(GL_VERTEX_SHADER, kVS_simple);
            GLuint ffs = compileShader(GL_FRAGMENT_SHADER, kFS_simple);
            gProgram = linkProgram2(fvs, ffs);
            LOGW("Fallback: SSBO shader compile/link failed (vertexBlocks=%d bindings=%d)", maxVertexSsbo, maxSsboBindings);
        } else {
            LOGW("SSBO path enabled (vertexBlocks=%d bindings=%d)", maxVertexSsbo, maxSsboBindings);
        }
    } else {
        gUseSSBO = false;
        GLuint vs = compileShader(GL_VERTEX_SHADER, kVS_simple);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFS_simple);
        gProgram = linkProgram2(vs, fs);
        LOGW("Fallback: using ES3.0 simple shader path");
    }
    if (gUseSSBO) {
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
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

    // 提前扩大容量，减少后续频繁重分配（诊断阶段可根据设备调整）
    gAllocatedStrokes = 4096;
    size_t pointsCapacity = (size_t)gAllocatedStrokes * kMaxPointsPerStroke;

    // VBO: 根据扩展选择half或float格式 (x,y,pressure)
    glBindVertexArray(gVAO);
    glGenBuffers(1, &gPointsBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, gPointsBuffer);
    size_t elemSizeVBO = gHasVertexHalfFloat ? (sizeof(uint16_t) * 3) : (sizeof(float) * 3);
    glBufferData(GL_ARRAY_BUFFER, pointsCapacity * elemSizeVBO, nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    if (gHasVertexHalfFloat) {
        glVertexAttribPointer(0, 3, GL_HALF_FLOAT, GL_FALSE, sizeof(uint16_t) * 3, (const void*)0);
    } else {
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, (const void*)0);
    }

    glBindVertexArray(gEmptyVAO);
    if (!gBypassVBO) glGenBuffers(1, &gBypassVBO);
    glBindBuffer(GL_ARRAY_BUFFER, gBypassVBO);
    const int bypassVerts = kVertsPerStroke;
    std::vector<float> bypassData((size_t)bypassVerts * 3u, 0.0f);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(bypassData.size() * sizeof(float)), bypassData.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, (const void*)0);
    glVertexAttribDivisor(0, 0);

    if (gUseSSBO) {
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
        glBufferData(GL_SHADER_STORAGE_BUFFER, pointsCapacity * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, gPressuresSSBO);

        glGenBuffers(1, &gVisibleIndexSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gVisibleIndexSSBO);
        gVisibleIndexCapacity = gAllocatedStrokes + 1;
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)((size_t)gVisibleIndexCapacity * sizeof(uint32_t)), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, gVisibleIndexSSBO);
        gVisibleDirty.store(1);
    }
    LOGI("Allocated buffers: strokes=%d, maxPointsPerStroke=%d, positions=%zu bytes, pressures=%zu bytes, vbo=%zu bytes",
         gAllocatedStrokes, kMaxPointsPerStroke,
         (size_t)(pointsCapacity * sizeof(float) * 2),
         (size_t)(pointsCapacity * sizeof(float)),
         (size_t)(pointsCapacity * (gHasVertexHalfFloat ? sizeof(uint16_t) * 3 : sizeof(float) * 3)));

    // 如有暂存笔划，初始化完成后立即刷新到GPU
    if (!gPendingStrokes.empty()) {
        size_t pending = gPendingStrokes.size();
        for (const auto& ps : gPendingStrokes) {
            uploadStroke(ps.points, ps.pressures, ps.color);
        }
        gPendingStrokes.clear();
        LOGI("Flushed pending strokes: %zu", pending);
    }
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_onNativeSurfaceChanged(JNIEnv* env, jobject /*thiz*/, jint width, jint height) {
    g_Width = width; g_Height = height;
    glViewport(0, 0, g_Width, g_Height);
    LOGW("Surface changed: %dx%d", g_Width, g_Height);
    if (gProgram) {
        glUseProgram(gProgram);
        glUniform2f(uResolutionLoc, (float)g_Width, (float)g_Height);
        glUniform1f(uViewScaleLoc, 1.0f);
        gViewScale = 1.0f;
        gViewTranslateX = 0.0f;
        gViewTranslateY = 0.0f;
        if (uViewTranslateLoc >= 0) glUniform2f(uViewTranslateLoc, gViewTranslateX, gViewTranslateY);
    }
    gVisibleDirty.store(1);
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_onNativeDrawFrame(JNIEnv* env, jobject /*thiz*/) {
    while (glGetError() != GL_NO_ERROR) {}
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    
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
             gUseFramebufferFetch ? "yes" : "no",
             gViewScale,
             gViewTranslateX, gViewTranslateY,
             std::clamp(gRenderMaxPoints.load(), 1, 1024),
             (int)gMetas.size(),
             gLiveActive ? "yes" : "no");
        if (!gUseSSBO) {
            LOGW("FallbackActive: ES3.0 simple path draws GL_LINE_STRIP (likely 1px width)");
        }
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

    if (gUseSSBO) {
        if (uPassLoc >= 0) glUniform1i(uPassLoc, 2);
        if (gUseFramebufferFetch) {
            glDisable(GL_BLEND);
        } else {
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        int drawCount = (gVisibleIndexSSBO ? gVisibleCount : (gLiveActive ? totalStrokes : committedStrokes));
        if (drawCount > 0) {
            if (uStrokeCountLoc >= 0) glUniform1f(uStrokeCountLoc, (float)std::max(drawCount, 1));
            if (uBaseInstanceLoc >= 0) glUniform1f(uBaseInstanceLoc, 0.0f);
            const int vertsPerStroke = std::clamp(gRenderMaxPoints.load(), 1, 1024) * 2 + 8;
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, vertsPerStroke, drawCount);
        }
    } else {
        glEnable(GL_BLEND);
        for (int s = 0; s < committedStrokes; ++s) {
            const StrokeMetaCPU &m = gMetas[s];
            if (m.pad > 0.5f) {
                glBlendEquationSeparate(GL_MIN, GL_FUNC_ADD);
                glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            } else {
                glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            }
            if (uColorLoc >= 0) {
                glUniform4f(uColorLoc, m.color[0], m.color[1], m.color[2], m.color[3]);
            }
            glDrawArrays(GL_LINE_STRIP, m.start, m.count);
        }
        if (gLiveActive && gLiveMeta.count > 0) {
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            if (uColorLoc >= 0) {
                glUniform4f(uColorLoc, gLiveMeta.color[0], gLiveMeta.color[1], gLiveMeta.color[2], gLiveMeta.color[3]);
            }
            glDrawArrays(GL_LINE_STRIP, gLiveMeta.start, gLiveMeta.count);
        }
    }
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        LOGE("glDraw error=0x%x", err);
    }
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_setViewScale(JNIEnv* env, jobject /*thiz*/, jfloat scale) {
    // 防止除零或过小值导致视觉异常
    gViewScale = (scale < 1e-4f) ? 1e-4f : scale;
    gVisibleDirty.store(1);
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_setViewTransform(JNIEnv* env, jobject /*thiz*/, jfloat scale, jfloat cx, jfloat cy) {
    gViewScale = (scale < 1e-4f) ? 1e-4f : scale;
    gViewTranslateX = cx;
    gViewTranslateY = cy;
    gVisibleDirty.store(1);
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_setRenderMaxPoints(JNIEnv* env, jobject /*thiz*/, jint maxPoints) {
    gRenderMaxPoints.store(std::clamp((int)maxPoints, 1, 1024));
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_beginLiveStroke(JNIEnv* env, jobject /*thiz*/, jfloatArray color) {
    if (env && color && env->GetArrayLength(color) >= 4) {
        env->GetFloatArrayRegion(color, 0, 4, gLiveColor);
    }
    gLiveActive = true;
    gGestureStartStrokeId = (int)gMetas.size();
    gLiveMeta.start = 0;
    gLiveMeta.count = 0;
    gLiveMeta.baseWidth = 16.0f;
    gLiveMeta.pad = 0.0f;
    gLiveMeta.color[0] = gLiveColor[0];
    gLiveMeta.color[1] = gLiveColor[1];
    gLiveMeta.color[2] = gLiveColor[2];
    gLiveMeta.color[3] = gLiveColor[3];
    gHasLiveBounds = false;
    gVisibleDirty.store(1);
}

JNIEXPORT void JNICALL
Java_com_example_myapplication_NativeBridge_updateLiveStroke(JNIEnv* env, jobject /*thiz*/, jfloatArray points, jfloatArray pressures) {
    if (!gLiveActive) return;
    if (!env || !points || !pressures) return;
    if (!gPointsBuffer) return;

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

    int strokeId = (int)gMetas.size();
    ensureCapacityForStrokes((size_t)strokeId + 1u);
    int start = strokeId * kMaxPointsPerStroke;

    std::vector<float> posWrite((size_t)N * 2u);
    std::vector<uint16_t> vboWrite16;
    std::vector<float> vboWriteF;
    if (gHasVertexHalfFloat) vboWrite16.resize((size_t)N * 3u); else vboWriteF.resize((size_t)N * 3u);
    for (int i = 0; i < N; ++i) {
        float x = pts[(size_t)i * 2u + 0u];
        float y = pts[(size_t)i * 2u + 1u];
        float pr = prs[(size_t)i];
        posWrite[(size_t)i * 2u + 0u] = x;
        posWrite[(size_t)i * 2u + 1u] = y;
        if (gHasVertexHalfFloat) {
            vboWrite16[(size_t)i * 3u + 0u] = floatToHalf(x);
            vboWrite16[(size_t)i * 3u + 1u] = floatToHalf(y);
            vboWrite16[(size_t)i * 3u + 2u] = floatToHalf(pr);
        } else {
            vboWriteF[(size_t)i * 3u + 0u] = x;
            vboWriteF[(size_t)i * 3u + 1u] = y;
            vboWriteF[(size_t)i * 3u + 2u] = pr;
        }
    }

    if (gUseSSBO && gPositionsSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPositionsSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(start * sizeof(float) * 2),
                        (GLsizeiptr)(posWrite.size() * sizeof(float)),
                        posWrite.data());
    }
    glBindBuffer(GL_ARRAY_BUFFER, gPointsBuffer);
    if (gHasVertexHalfFloat) {
        glBufferSubData(GL_ARRAY_BUFFER,
                        (GLintptr)(start * sizeof(uint16_t) * 3),
                        (GLsizeiptr)(vboWrite16.size() * sizeof(uint16_t)),
                        vboWrite16.data());
    } else {
        glBufferSubData(GL_ARRAY_BUFFER,
                        (GLintptr)(start * sizeof(float) * 3),
                        (GLsizeiptr)(vboWriteF.size() * sizeof(float)),
                        vboWriteF.data());
    }
    if (gUseSSBO && gPressuresSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPressuresSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(start * sizeof(float)),
                        (GLsizeiptr)(prs.size() * sizeof(float)),
                        prs.data());
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
    if (!gPointsBuffer) return;
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

    int strokeId = (int)gMetas.size();
    ensureCapacityForStrokes((size_t)strokeId + 1u);
    int start = strokeId * kMaxPointsPerStroke;

    std::vector<float> posWrite((size_t)N * 2u);
    std::vector<uint16_t> vboWrite16;
    std::vector<float> vboWriteF;
    if (gHasVertexHalfFloat) vboWrite16.resize((size_t)N * 3u); else vboWriteF.resize((size_t)N * 3u);
    for (int i = 0; i < N; ++i) {
        float x = pts[(size_t)i * 2u + 0u];
        float y = pts[(size_t)i * 2u + 1u];
        float pr = prs[(size_t)i];
        posWrite[(size_t)i * 2u + 0u] = x;
        posWrite[(size_t)i * 2u + 1u] = y;
        if (gHasVertexHalfFloat) {
            vboWrite16[(size_t)i * 3u + 0u] = floatToHalf(x);
            vboWrite16[(size_t)i * 3u + 1u] = floatToHalf(y);
            vboWrite16[(size_t)i * 3u + 2u] = floatToHalf(pr);
        } else {
            vboWriteF[(size_t)i * 3u + 0u] = x;
            vboWriteF[(size_t)i * 3u + 1u] = y;
            vboWriteF[(size_t)i * 3u + 2u] = pr;
        }
    }

    if (gUseSSBO && gPositionsSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPositionsSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(start * sizeof(float) * 2),
                        (GLsizeiptr)(posWrite.size() * sizeof(float)),
                        posWrite.data());
    }
    glBindBuffer(GL_ARRAY_BUFFER, gPointsBuffer);
    if (gHasVertexHalfFloat) {
        glBufferSubData(GL_ARRAY_BUFFER,
                        (GLintptr)(start * sizeof(uint16_t) * 3),
                        (GLsizeiptr)(vboWrite16.size() * sizeof(uint16_t)),
                        vboWrite16.data());
    } else {
        glBufferSubData(GL_ARRAY_BUFFER,
                        (GLintptr)(start * sizeof(float) * 3),
                        (GLsizeiptr)(vboWriteF.size() * sizeof(float)),
                        vboWriteF.data());
    }
    if (gUseSSBO && gPressuresSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPressuresSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(start * sizeof(float)),
                        (GLsizeiptr)(prs.size() * sizeof(float)),
                        prs.data());
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
                                                      jfloatArray color) {
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
    if (!gProgram) {
        PendingStroke ps;
        ps.points = std::move(pts);
        ps.pressures = std::move(prs);
        ps.color = std::move(col);
        gPendingStrokes.push_back(std::move(ps));
        if (gQueueLogBudget.fetch_sub(1) > 0) {
            LOGW("addStroke queued (GL not ready): count=%d", N);
        }
        return;
    }

    uploadStroke(pts, prs, col);
    gVisibleDirty.store(1);
}

JNIEXPORT jint JNICALL
Java_com_example_myapplication_NativeBridge_getStrokeCount(JNIEnv *env, jobject /* this */) {
    return (jint)gMetas.size();
}

JNIEXPORT jint JNICALL
Java_com_example_myapplication_NativeBridge_getBlueStrokeCount(JNIEnv *env, jobject /* this */) {
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
                                                           jfloatArray colors) {
    jsize pLen = env->GetArrayLength(points);
    jsize prLen = env->GetArrayLength(pressures);
    jsize cLen = env->GetArrayLength(colors);
    jsize cntLen = env->GetArrayLength(counts);
    if (cntLen <= 0 || pLen <= 0 || prLen <= 0 || cLen < cntLen * 4) return;

    std::vector<float> ptsFlat(pLen);
    std::vector<float> prsFlat(prLen);
    std::vector<int> cnts(cntLen);
    std::vector<float> colsFlat(cLen);
    env->GetFloatArrayRegion(points, 0, pLen, ptsFlat.data());
    env->GetFloatArrayRegion(pressures, 0, prLen, prsFlat.data());
    env->GetIntArrayRegion(counts, 0, cntLen, cnts.data());
    env->GetFloatArrayRegion(colors, 0, cLen, colsFlat.data());

    if (!gProgram) {
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

        glBindBuffer(GL_ARRAY_BUFFER, gPointsBuffer);
        size_t elemSizeVBO2 = gHasVertexHalfFloat ? (sizeof(uint16_t) * 3) : (sizeof(float) * 3);
        glBufferData(GL_ARRAY_BUFFER, pointsCapacity * elemSizeVBO2, nullptr, GL_DYNAMIC_DRAW);
        if (gUseSSBO) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPositionsSSBO);
            glBufferData(GL_SHADER_STORAGE_BUFFER, pointsCapacity * sizeof(float) * 2, nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gPositionsSSBO);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, gStrokeMetaSSBO);
            glBufferData(GL_SHADER_STORAGE_BUFFER, newCap * sizeof(StrokeMetaCPU), nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, gStrokeMetaSSBO);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPressuresSSBO);
            glBufferData(GL_SHADER_STORAGE_BUFFER, pointsCapacity * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, gPressuresSSBO);
        }
        gAllocatedStrokes = newCap;
    }

    size_t blockPts = (size_t)S * kMaxPointsPerStroke;
    std::vector<float> positionsBatch(blockPts * 2, 0.0f);
    std::vector<float> pressuresBatch(blockPts, 0.0f);
    std::vector<uint16_t> vboBatch16;
    std::vector<float> vboBatchF;
    if (gHasVertexHalfFloat) vboBatch16.resize(blockPts * 3, 0);
    else vboBatchF.resize(blockPts * 3, 0.0f);
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
            pressuresBatch[base + i] = pr;
            if (gHasVertexHalfFloat) {
                vboBatch16[(base + i) * 3 + 0] = floatToHalf(x);
                vboBatch16[(base + i) * 3 + 1] = floatToHalf(y);
                vboBatch16[(base + i) * 3 + 2] = floatToHalf(pr);
            } else {
                vboBatchF[(base + i) * 3 + 0] = x;
                vboBatchF[(base + i) * 3 + 1] = y;
                vboBatchF[(base + i) * 3 + 2] = pr;
            }
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
                        (GLintptr)(globalStart * sizeof(float)),
                        (GLsizeiptr)(pressuresBatch.size() * sizeof(float)),
                        pressuresBatch.data());
        // 提交元数据 SSBO（按条上传）
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gStrokeMetaSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(startId * sizeof(StrokeMetaCPU)),
                        (GLsizeiptr)(metasBatch.size() * sizeof(StrokeMetaCPU)),
                        metasBatch.data());
    }
    glBindBuffer(GL_ARRAY_BUFFER, gPointsBuffer);
    if (gHasVertexHalfFloat) {
        glBufferSubData(GL_ARRAY_BUFFER,
                        (GLintptr)(globalStart * sizeof(uint16_t) * 3),
                        (GLsizeiptr)(vboBatch16.size() * sizeof(uint16_t)),
                        vboBatch16.data());
    } else {
        glBufferSubData(GL_ARRAY_BUFFER,
                        (GLintptr)(globalStart * sizeof(float) * 3),
                        (GLsizeiptr)(vboBatchF.size() * sizeof(float)),
                        vboBatchF.data());
    }
    if (gUseSSBO) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gPressuresSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(globalStart * sizeof(float)),
                        (GLsizeiptr)(pressuresBatch.size() * sizeof(float)),
                        pressuresBatch.data());
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
