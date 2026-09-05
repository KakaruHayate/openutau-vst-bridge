# OpenUtau Bridge 预发布说明

**版本 0.2.0（Alpha）· 协议 v1.1 · 2026-09-05**

这是 OpenUtau DAW 桥接插件的第二个 alpha 预发布版。写给准备分发和测试这份包的人，也写给想快速了解「这一版和上一版差在哪」的人。动手装之前，请先通读一遍；完整的安装与使用步骤在《使用说明书》（`MANUAL.zh-CN.md`），逐项测试清单在《测试指南》（`TESTING.zh-CN.md`）。

---

## 这是什么

一个把 OpenUtau 渲染好的声音直接摆到 DAW 时间轴上的插件。你在 **OpenUtau 里编辑**（写音符、换歌手、调渲染），声音实时出现在 **DAW 的乐器轨上**，位置和 OpenUtau 里一致——不用导出 wav 再拖。

它**不是**把 OpenUtau 塞进 DAW 窗口的那种（Synthesizer V / ARA 的做法）。OpenUtau 始终是独立窗口；DAW 里的插件只负责「连接、接收音频、按位置摆放」。所以它没有编辑器界面——这一版起有一个只读的**信息小窗**（仅 Windows），双击插件可以看到工程状态。

## 包内容

每个平台的包里是同一套东西：

| 平台 | 文件 |
|---|---|
| Windows 64 位 | `OpenUtau Bridge.vst3`（文件夹）+ `OpenUtau Bridge.clap`（文件） |
| macOS（Apple 芯片 / Intel） | 同上两种格式 |
| Linux 64 位 | 同上两种格式 |

> **必须与配套的 OpenUtau 测试版成对使用**（`OpenUtau-win-x64.zip` 等，带 DAW Integration 功能的那份）。插件是协议 v1.1，旧版 OpenUtau 主程序连不上或功能不完整。

## 这版有什么

在上一版（0.1.0，协议 v1.0）的基础上：

**v1.1 新增**

- **信息小窗**（Windows）：显示工程名与保存状态、连接状态与端口、BPM、播放状态、音轨列表（当前实例负责的轨道带 ▸ 标记）。只读，无控件。
- **播放头同步**：DAW 的播放头单向驱动 OpenUtau——OpenUtau 窗口里的播放位置跟着 DAW 走。播放状态变化立即同步；播放中约 100ms 一次；停止时拖动超过 50ms 才同步。
- **BPM 不匹配提醒**：DAW 工程速度和 OpenUtau 工程速度不一致（差 0.5 BPM 以上）时，OpenUtau 会对每种不一致提醒一次。不做小节换算。
- **协议升级到 v1.1**：`updateProjectInfo` / `playhead` / `bpm` 三种消息；协议主版本不变，新旧混用时未知消息会被安全忽略。

**v1.0 基础（上一版已发布）**

- 实时同步：OpenUtau 里的编辑（音符、歌词、渲染）自动更新到 DAW。
- 多实例多轨：每条 DAW 轨一个实例，用 `OpenUtau Track` 参数各管 OpenUtau 的一条音轨。
- Pre-fader 输出：OpenUtau 的音量/声像/静音不影响桥接信号，混音完全交给 DAW（注意：和更早的版本相比整体响约 3 dB，这是设计如此）。
- 导出/Bounce 支持：离线渲染会等待缺失的音频，不再导出成空白。
- 采样率自适应：线路上固定 44.1kHz，插件自动转换到 DAW 的采样率。

## 已知问题与限制

- **信息小窗仅 Windows。** macOS / Linux 上双击插件暂时无界面，功能不受影响。
- **OpenUtau 播放头被 DAW 驱动。** 播放期间在 OpenUtau 里手动拖播放头会被立刻覆盖，属预期行为。
- **两边按秒对齐，不按小节。** BPM 不一致时只有提醒，没有换算。
- **未保存的 OpenUtau 工程连不上。** 会收到「请先保存工程」的提示，这是刻意的保护（未保存工程无法生成可靠的音频路径）。
- **DAW 工程不保存 OpenUtau 内容。** 重开 DAW 工程后要重新连一次 OpenUtau。
- **不接受 MIDI 输入。** 音符只能在 OpenUtau 里写。
- macOS 的 Logic Pro / GarageBand 大概率扫不到插件（沙箱限制）。

## 发布前验证状态

- 插件自带测试 122/122 通过（协议、并发、混音、resample 全链路，真实 socket）。
- GitHub CI 三平台（Windows x64 / macOS arm64 / Linux x64）构建 + 测试 + Steinberg VST3 validator 全绿。
- 已知残留（不在本版修）：个别宿主对**嵌入式**插件界面的兼容性未经广泛验证，遇到信息窗显示异常请按测试指南反馈。

## 反馈

按《测试指南》（`TESTING.zh-CN.md`）里的「出问题了怎么报告」一节来：**必带** `%TEMP%\OpenUtau\` 下的 `bridge-*.log` 日志、DAW 名称与版本、系统版本、VST3 还是 CLAP、复现步骤。

## 许可

插件本体 MPL-2.0，依赖均为宽松许可（详见 `README.md`）。测试分发的 OpenUtau 主程序遵循其自身许可。
