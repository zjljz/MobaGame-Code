# MobaGame-Code

MobaGame 的代码展示仓库，面向 Unreal Engine C++ 多人 Gameplay 架构、实现与测试代码阅读。

## 快照与范围

本次导出基于原工程远端 main 已提交快照 `462e2ba249690b4048188a46b4e64a86ee484a24`（2026-10-20），不代表作者电脑上的未提交工作或完整项目现状。公开仓库使用独立提交历史，不镜像原工程历史。

包含：
- MobaGame：Experience / GameFeature 协调、Hero / GAS、战斗与技能目标选择、物品、比赛、会话与重连等实现。
- MobaGameUI：客户端表现、状态面板、目标预览及前端 UI 的 C++ 部分。
- MobaGameTests 及模块内测试源码：原生逻辑、网络、资产与 UI 等测试实现，具体可执行性受模块注册、运行参数、依赖及资产条件限制。
- 三个 Target 文件、构建规则、基础 Config 和项目描述文件。
- MobaClassic GameFeature 描述文件，作为原结构记录；当前快照的该插件没有 C++ Source。

这不是完整游戏发行版，也不是开箱即用的可运行示例。

## 明确不包含

所有层级的 Content 目录，包括根 Content 和 Plugins/GameFeatures/MobaClassic/Content；Unreal 资产、地图、蓝图、纹理和其他二进制内容及其 LFS 指针均不发布。
同时不包含 Unreal Engine 源码、GMP 等外置插件源码、编译产物、缓存、日志、开发代理工具、私有文档同步配置和原工程历史。

当前不发布资产是代码仓库的范围选择，不表示这些资产来自第三方。后续添加素材或插件时，需要分别确认其许可和再分发条件。

## 环境与外部依赖

- 项目描述文件关联 Unreal Engine 5.8；Target 采用 Unreal5_8 include order 和 BuildSettingsVersion.V7。原工程文档记录过 5.8.3 验证，但本公开导出没有重新编译，也未验证其他版本。
- Unreal Engine 及 GameFeatures、GameplayAbilities、EnhancedInput、TargetingSystem 等引擎模块/插件需通过合法授权的引擎安装获得，本仓库不分发它们。
- GMP：上游 [GenericMessagePlugin](https://github.com/wangjieest/GenericMessagePlugin)，原工程固定提交为 `85283fbec0edba1a04c1cff8181455c5d97ba60a`。按上游目录结构将 Plugins/GMP 放入工程 Plugins/GMP；不要复制其他机器的 Binaries/Intermediate。本仓库不 vendor 该依赖。
- 原工程曾为 GMP 在 Source Engine Editor 构建下的 UE verify 宏冲突使用兼容补丁。本次代码导出不包含该补丁，不能承诺仅恢复原始 GMP 后即可通过该构建组合。需针对实际引擎和插件版本复核。
- UnrealAgentLink 是开发工具，已从公开项目的启用声明中移除；RiderLink 不是必要前提。
- 重连凭证实现包含 Win64 平台路径。未验证跨平台运行，不将配置中的平台节视为跨平台支持声明。

可在准备好兼容的 Unreal Engine 和外部依赖后尝试生成工程文件及构建 MobaGameEditor。Game、Editor、Server 目标的可用性取决于所选引擎发行方式与构建环境；本仓库没有提供已验证的构建产物或 CI 通过承诺。

## 为什么不能直接运行

Config 仍保留原工程的资产引用，例如前端地图、GameConfiguration 和客户端 UI 配置。这些对应资产被有意排除。
MobaClassic 缺少 GameFeatureData、Experience、输入配置、GameplayEffect、地图等内容，不能仅凭 uplugin 描述文件恢复玩法。
C++ 控件实现也不替代 WBP 布局和绑定。资产校验、真实地图、网络场景和 UI 测试可能需要原工程内容或重新制作的等价 fixture。
本次只审查导出范围与文本，没有运行编译、自动化测试、Editor、打包或联机验证。保留测试代码不表示所有测试已注册、可发现或通过。

## 阅读入口

- Source/MobaGame/Public 与 Private：业务定义与实现。
- Source/MobaGame/Private/Tests：模块内原生测试。
- Source/MobaGameTests/Private/Tests：独立测试实现；公开快照不补改原来的注册接线。
- Source/MobaGameUI：客户端表现和 UI 测试。

## 许可

作者拥有权利的自有代码采用 MIT，见 [LICENSE](LICENSE)。
已有第三方/Epic 版权声明保留，不因根 MIT 而改授许可。外部引擎、插件及其组件遵循各自许可；见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

