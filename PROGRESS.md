# DBAdapters 进度记录

> 按项目治理规则维护：每次会话结束更新本文件，下次会话开始先读取。

## ✅ 已完成

- **中英文 README**：新建 `README.md`（中文）与 `README.en.md`（英文），结构参考 Spark 仓库，涵盖项目概述、核心功能模块（DBInterface / 四种 Wrapper / AsyncDBWriter）、目录结构、环境依赖、快速构建、使用示例（TypedTable CRUD、DuckDB 向量化读取、异步写库）、集成测试、许可证。
- **环境准备文档修正**：将从 Spark 拷贝的 `docs/environment-setup.md` / `docs/environment-setup.en.md` 改为 DBAdapters 专属版本，替换 GTest / test/unittest / Spark 路径等错误内容，补充 Spark + duckdb 预编译依赖说明。

## 🔄 进行中

- 无

## ❓ 待讨论 / 待决策

- README 落款沿用 Spark 的 "Created by [Fireseeker]"（两仓库 LICENSE 同为 xunmeng2002）——如需调整署名请告知。
- 构建依赖 Spark 与 duckdb 为**预编译库**（`../Libs/Spark/<triplet>`、`../Libs/duckdb/<triplet>`），`vcpkg.json` 未声明这两者；后续若考虑可复现构建，可讨论是否将 duckdb 纳入 vcpkg 管理。
- `test/TestDB` 中 MySQL / MariaDB 测试默认注释关闭（需本地服务），如要常开可配置 CI。
