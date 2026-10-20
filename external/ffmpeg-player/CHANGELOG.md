# FFmpegPlayer 更新日志

## v1.0.3

完整变更：[v1.0.2...v1.0.3](https://github.com/Haikure/ffmpeg-player/compare/v1.0.2...v1.0.3)

### 改进

- 放大模式启用硬件双线性采样，保留原始纹理尺寸并关闭额外多点平均，减轻方块感和锯齿；普通模式维持原有降采样行为。
- 将 `VideoPlayer.smooth` 与 `downsamplingEnabled` 解耦，支持在关闭降采样时独立选择线性或最近邻过滤，覆盖 I420、NV12、External OES 和 CPU 回退路径。
