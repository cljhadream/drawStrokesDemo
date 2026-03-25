# 实施任务（Implementation Tasks）

- [ ] 1. 接入实时书写入口与交互
  - [ ] 1.1 在现有 `StrokeGLSurfaceView` 中开启单指书写（DOWN/MOVE/UP）
  - [ ] 1.2 统一触摸采集坐标系（screen->world），确保缩放/平移下正确采样

- [ ] 2. 实现贴点分段二次曲线的实时几何生成
  - [ ] 2.1 提取并复用“距离阈值过滤 + 角度门控弱平滑”的去噪逻辑（仅渲染序列）
  - [ ] 2.2 实现“两段曲线”：Committed 固定段 + Tail(K点)回滚段
  - [ ] 2.3 Tail 末端控制点按末端切线构造，末点严格对齐最新报点
  - [ ] 2.4 实现二次段的固定屏幕步长采样，并保证段终点/末端对齐
  - [ ] 2.5 为 Tail 增加稳定化平滑，并在转正时冻结为稳定锚点

- [ ] 3. 统一实时预览与最终落笔的提交策略
  - [ ] 3.1 MOVE：生成 render 点并通过 live update 更新到 native
  - [ ] 3.2 UP：用同一算法生成最终点并一次性 addStroke 提交，随后关闭 live

- [ ] 4. 修正 native live stroke 为“固定 slot 原地更新”
  - [ ] 4.1 为 live stroke 分配固定 strokeId（inactive 时 count=0）
  - [ ] 4.2 updateLiveStrokeWithCount 改为更新该 strokeId 的 SSBO/纹理路径数据
  - [ ] 4.3 统计接口排除 live slot（strokeCount/blueStrokeCount）

- [ ] 5. 验证与回归
  - [ ] 5.1 用回放数据验证端点一致性与角点保留
  - [ ] 5.2 在真机上验证实时书写末端对齐与落笔一致性
