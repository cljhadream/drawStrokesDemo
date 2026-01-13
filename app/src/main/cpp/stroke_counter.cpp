// 临时文件：用于获取当前笔划数量
#include <jni.h>
#include <android/log.h>

// 声明外部变量
extern std::vector<struct StrokeMetaCPU> gMetas;

extern "C" JNIEXPORT jint JNICALL
Java_com_example_myapplication_NativeBridge_getStrokeCount(JNIEnv *env, jobject /* this */) {
    return (jint)gMetas.size();
}

extern "C" JNIEXPORT jint JNICALL
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