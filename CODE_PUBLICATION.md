# 代码发布边界

本次来源：远端 main 已提交快照 462e2ba249690b4048188a46b4e64a86ee484a24。
本公开仓库应独立初始化，不能推送原仓库的历史、分支、标签或 LFS 对象。

## 初始允许范围

- Source/ 下本次已审查的 196 个源码、构建/Target 和测试辅助文件。
- Config/DefaultEditor.ini、DefaultEngine.ini、DefaultGame.ini、DefaultInput.ini、DefaultGMPMeta.ini。
- Plugins/GameFeatures/MobaClassic/MobaClassic.uplugin。
- MobaGame.uproject：仅移除 UnrealAgentLink 的启用项，其余依赖关系保持。
- 新建 README.md、LICENSE、THIRD_PARTY_NOTICES.md、CODE_PUBLICATION.md、.gitignore。

## 排除与后续维护

所有 Content、资产及大二进制、引擎源码、外置插件、生成物、缓存、日志、私有工具、文档同步映射、原工程过程记录均排除。本次不直接复制原工程 Docs 或 GMP 兼容补丁。

.gitignore 只能降低误加入风险，不能从已有提交或历史删除文件。后续同步应使用显式允许列表并检查暂存文件，不能用复制全部文件后只删除根 Content 的方式。
任何层级的新 Content、资产后缀或外部依赖都应默认排除；自有新代码则按内容审查，不仅凭插件名称判断所有权。
新增依赖、素材、截图、文档或测试数据前检查来源、许可、敏感信息和公开权限，不把项目 MIT 自动扩展到第三方内容。

## 验证范围

对本次远端 Source 和 Config 完成文本读取及常见凭据模式检查；未发现这些模式不构成无秘密信息的保证。发布前还需独立复核最终允许文件集、版权声明及实际写入内容。
没有运行构建或测试；原工程历史验证记录不等于本公开导出通过验证。

