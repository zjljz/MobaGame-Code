# 第三方与版权边界

本仓库根 LICENSE 的 MIT 许可只适用于作者拥有授权权利的自有代码，不覆盖第三方代码、Unreal Engine 或未包含的资产/插件，也不替换文件中已有的第三方版权和许可声明。

## 保留的 Epic Games 声明

以下文件在源快照中带有 “Copyright Epic Games, Inc. All Rights Reserved.”，本次原样保留：
- Source/MobaGame.Target.cs
- Source/MobaGameEditor.Target.cs
- Source/MobaGame/MobaGame.Build.cs
- Source/MobaGame/MobaGame.cpp
- Source/MobaGame/MobaGame.h

它们是工程启动、构建规则或目标定义文件。不能仅凭文件名称或模板形态推断所有权，也不能用本仓库 MIT 为其中 Epic 享有权利的部分重新授权。使用者应遵守适用的 Unreal Engine 条款。此处不表示已完成第三方版权法律审查。

## Unreal Engine

引擎源码和二进制未包含在本仓库。引擎模块依赖不意味着本仓库对其提供许可。请从 Epic 官方渠道获取引擎并遵守对应条款。

## GenericMessagePlugin / GMP

外部依赖：[wangjieest/GenericMessagePlugin](https://github.com/wangjieest/GenericMessagePlugin)
固定参考提交：85283fbec0edba1a04c1cff8181455c5d97ba60a。
已直接核实该提交的根 [LICENSE](https://github.com/wangjieest/GenericMessagePlugin/blob/85283fbec0edba1a04c1cff8181455c5d97ba60a/LICENSE) 为 Apache-2.0。

本仓库不分发 GMP 源码或兼容补丁。上游子组件可能有独立许可，根 Apache-2.0 不应被视为整个插件所有文件的唯一许可；恢复或再分发插件时保留其 LICENSE、NOTICE、版权头及组件声明，并自行核实实际版本。

## 其他外部组件

Win64 代码使用操作系统 BCrypt/DPAPI，并引用引擎提供的 OpenSSL 接口。相关库没有随本仓库分发，其许可、平台可用性和打包义务应分别核实。
RiderLink、UnrealAgentLink 不包含在本仓库，也不通过本仓库授予任何许可。

