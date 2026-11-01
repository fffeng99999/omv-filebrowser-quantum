# openmediavault-filebrowser-quantum 系统架构重构与功能演进详细设计说明书

- **文档标识**：`OMV-FBQ-ARCH-SPEC-20261031`
- **对应插件**：`omv-filebrowser-quantum` / `openmediavault-filebrowser-quantum`
- **目标发布版本**：**8.1.0**（tag `v8.1.0`，从 8.0.1 PoC 原型全面演进）
- **架构师**：`lead_architect` (OMV 插件系统架构师)
- **文档状态**：**架构定稿，落地实施规范（下游研发、UI 与 QA 实施的唯一输入）**
- **上游基线**：FileBrowser Quantum v2 (<https://github.com/gtsteffaniak/filebrowser>，官方镜像 `ghcr.io/gtsteffaniak/filebrowser`)
- **规范依据**：
  - `project_rules_dev.md`「插件开发红线」
  - `.dsh/skills/omv-plugin-dev/references/integration-modes.md`（三轨规范：轨 1 Compose 轨）
  - `omv-reference/omv-plugin-dev-reference/`（03 后端 datamodel/RPC、04 Salt 部署、05·06 前端 Workbench、09 坑清单）
  - 依赖任务输入：Task t1 审计报告 (`docs/audit/2026-10-31-code-audit-report.md`) 与 Task t2 需求分析矩阵

---

## 0. 架构重构背景与一页纸决策矩阵

### 0.1 重构背景与核心痛点归结

在 Task t1（代码审计）与 Task t2（功能调研）中，团队对现有 `openmediavault-filebrowser-quantum` (8.0.1) 进行了全面穿透分析。当前插件虽走通了“基于 `openmediavault-compose` 注册单容器栈”的最小可用流程，但本质仍属于初级概念验证（PoC）阶段，存在以下阻碍生产化落地的严重隐患：

1. **RPC 同步阻塞风暴（P0）**：`OMVRpcServiceFileBrowserQuantum::get()` 在主线程中同步串行执行 4 次子进程，且直接调用 ctl 脚本通过外网 curl 抓取 GitHub Atom Feed 与 REST API。在离线、DNS 缓慢或 GitHub API 403 限流场景下，页面加载必卡 6~14 秒甚至触发 504 Gateway Timeout。
2. **Shell 胶水层脆弱解析（P0）**：`omv-filebrowserquantum-ctl` 中的 `conf_value()` 使用极其脆弱的贪婪匹配 `sed` 解析 JSON，遇同名字段、复杂字符串或包含逗号/花括号的值直接截断或覆盖。
3. **存储安全与防递归缺位（P0）**：未对共享文件夹进行拓扑冲突检查，允许 `dataRef == sourceRef`，导致 FileBrowser Quantum 的数据库与运行配置直接暴露并引发索引引擎无限递归；生产禁读卷（UUID `6dab12c5-4b76-461a-9c3a-7f610aca2f74`）无任何硬隔离保护。
4. **单体巨石表单与单源锁死（P0/P1）**：单页平铺状态、配置与 7 个动作按钮，交互层级混乱；数据模型硬编码单一 `sourceRef`，锁死了 FileBrowser Quantum v2 原生支持的 `server.sources` 多源存储挂载核心能力。
5. **配置静态脱节与漂移（P1）**：`prepare_config` 采用静态一次性写入策略，生成后绝不更新（`[ -f config.yaml ] && return 0`），导致 OMV WebUI 中的参数变更与上游运行配置彻底脱节。
6. **文件属主污染隐患（P1）**：强制以 `0:0` (root) 运行容器，在宿主机共享文件夹内创建的文件属主均为 root，导致普通 Samba 用户（UID 1000）丧失写权限。

### 0.2 一页纸架构决策全景矩阵

| 架构维度 | 8.0.1 PoC 现状 | 8.1.0 重构落地设计 | 决策理由与规范依据 |
|---|---|---|---|
| **集成轨道** | 轨 1 (Compose 轨) | **轨 1 (Compose 轨) 规范化深耕** | 上游只发布 Docker 镜像，严格遵循三轨决策树；单一写入者原则，由 `openmediavault-compose` 托管渲染 |
| **数据模型架构** | 单一平铺模型 `conf.service.filebrowserquantum` | **父单例 + 子可迭代 (Master-Detail) 双模型** | 父模型存全局配置，子模型 `*.source` 管理多存储源映射；支持任意多个共享文件夹纳管 |
| **存储挂载拓扑** | 单一 `sourceRef` 硬编码挂载到 `/srv` | **动态多挂载源（`server.sources`）** | 支持按共享文件夹名称自动生成容器路径 `/srv/<name>`，支持独立只读与默认启用属性 |
| **防递归安全机制** | **无**（允许相同路径、允许包含） | **双闸门防递归 + tmpfs 物理遮蔽引擎** | RPC 静态校验硬拦截（R-1~R-7）+ ctl 启动前动态注入 1MiB 只读 tmpfs 遮蔽敏感目录 |
| **配置文件管理** | 静态生成，一旦存在绝不更新 | **声明式分层同步（Declarative Sync）** | OMV 数据库作为 sources 的唯一事实源，ctl 启动时声明式同步更新 `config.yaml`，保留用户自定义参数 |
| **RPC 性能与解耦** | `get()` 同步串行执行 4 进程 + 外网 curl | **网络 I/O 剥离 + 独立异步 RPC `getLatestVersion`** | `get()` 仅读取本地状态（耗时 < 50ms）；外网检测按需异步执行，结果本地缓存（TTL 12h） |
| **脚本胶水解析** | 脆弱的 `sed` 正则提取 JSON 键值 | **`jq` 优先 + 内置 `python3` 兜底解析** | 严格杜绝贪婪匹配与逗号截断崩溃，确保多字段、嵌套结构解析 100% 健壮 |
| **容器用户权限** | 强制固定 `user: "0:0"` (root) | **可配 PUID/PGID + 默认宿主安全映射** | 增加 `puid`/`pgid` 配置（默认 `1000:100` 或当前配置），避免容器污染 NAS Samba 用户文件属主 |
| **前端交互体系** | 单体巨石表单（状态+配置+按钮全平铺） | **Navigation Container + Settings Form + Sources DataTable** | 对齐 `omv-filestash` / `omv-immich` 标杆，拆分为导航容器、设置表单、存储列表与新增抽屉 |
| **外链跳转机制** | 硬编码 `http://<host>:<port>` | **协议自适应 + `baseURL` / `appUrl` 适配** | 适配 HTTPS、端口转发与 NPM/Traefik 反向代理，支持自定义应用外部访问 URL，避免 Mixed Content |
| **工程质量工具** | 无自动化测试脚本 | **标准化测试工具链（validate/test/locale）** | 引入 `validate-workbench.py`、`test_anti_recursion.py`、`build_locale.py` 作为发布硬闸门 |

---

## 1. 数据模型扩展与持久化迁移架构（双模型体系）

### 1.1 父单例模型：`conf.service.filebrowserquantum.json`

父模型负责承载全局服务状态、网络参数、运行时镜像版本、运行身份、反代 URL 以及子容器节点。

- **Model ID**: `conf.service.filebrowserquantum`
- **XPath**: `.//services/filebrowserquantum`
- **Iterable**: `false`

```json
{
  "type": "config",
  "id": "conf.service.filebrowserquantum",
  "title": "FileBrowser Quantum",
  "queryinfo": {
    "xpath": ".//services/filebrowserquantum",
    "iterable": false
  },
  "properties": {
    "enable": {
      "type": "boolean",
      "default": false
    },
    "version": {
      "type": "string",
      "default": "2.0.10-beta"
    },
    "port": {
      "type": "integer",
      "minimum": 1,
      "maximum": 65535,
      "default": 8085
    },
    "imageVariant": {
      "type": "string",
      "enum": [
        "full",
        "slim"
      ],
      "default": "full"
    },
    "dataRef": {
      "type": "string",
      "oneOf": [
        {
          "type": "string",
          "format": "uuidv4"
        },
        {
          "type": "string",
          "maxLength": 0
        }
      ],
      "default": ""
    },
    "sourceRef": {
      "type": "string",
      "oneOf": [
        {
          "type": "string",
          "format": "uuidv4"
        },
        {
          "type": "string",
          "maxLength": 0
        }
      ],
      "default": "",
      "description": "Deprecated in 8.1.0; kept for smooth backward-compatibility migration."
    },
    "baseURL": {
      "type": "string",
      "default": ""
    },
    "appUrl": {
      "type": "string",
      "default": ""
    },
    "trustProxyHeaders": {
      "type": "boolean",
      "default": false
    },
    "authMethod": {
      "type": "string",
      "enum": [
        "password",
        "noauth",
        "proxy"
      ],
      "default": "password"
    },
    "puid": {
      "type": "integer",
      "minimum": 0,
      "maximum": 65535,
      "default": 1000
    },
    "pgid": {
      "type": "integer",
      "minimum": 0,
      "maximum": 65535,
      "default": 100
    },
    "maskProtected": {
      "type": "boolean",
      "default": true
    },
    "extraOptions": {
      "type": "string",
      "default": ""
    },
    "sources": {
      "type": "object",
      "properties": {
        "source": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "uuid": {
                "type": "string",
                "format": "uuidv4"
              },
              "sharedfolderref": {
                "type": "string",
                "oneOf": [
                  {
                    "type": "string",
                    "format": "uuidv4"
                  },
                  {
                    "type": "string",
                    "maxLength": 0
                  }
                ],
                "default": ""
              },
              "path": {
                "type": "string",
                "default": "/srv"
              },
              "name": {
                "type": "string",
                "default": ""
              },
              "readonly": {
                "type": "boolean",
                "default": false
              },
              "defaultEnabled": {
                "type": "boolean",
                "default": true
              }
            }
          }
        }
      }
    }
  }
}
```

### 1.2 子可迭代模型：`conf.service.filebrowserquantum.source.json`

子模型专门管理挂载到 FileBrowser Quantum 中的存储源列表，与上游 `server.sources` 数组映射。

- **Model ID**: `conf.service.filebrowserquantum.source`
- **XPath**: `.//services/filebrowserquantum/sources/source`
- **Iterable**: `true`
- **ID Property**: `uuid`

```json
{
  "type": "config",
  "id": "conf.service.filebrowserquantum.source",
  "title": "FileBrowser Quantum source",
  "queryinfo": {
    "xpath": ".//services/filebrowserquantum/sources/source",
    "iterable": true,
    "idproperty": "uuid"
  },
  "properties": {
    "uuid": {
      "type": "string",
      "format": "uuidv4"
    },
    "sharedfolderref": {
      "type": "string",
      "oneOf": [
        {
          "type": "string",
          "format": "uuidv4"
        },
        {
          "type": "string",
          "maxLength": 0
        }
      ],
      "default": ""
    },
    "path": {
      "type": "string",
      "default": "/srv"
    },
    "name": {
      "type": "string",
      "default": ""
    },
    "readonly": {
      "type": "boolean",
      "default": false
    },
    "defaultEnabled": {
      "type": "boolean",
      "default": true
    }
  }
}
```

### 1.3 `config.xml` 数据库节点拓扑

在 OMV 系统的 `/etc/openmediavault/config.xml` 中，数据结构呈现为：

```xml
<config>
  <services>
    <filebrowserquantum>
      <enable>0</enable>
      <version>2.0.10-beta</version>
      <port>8085</port>
      <imageVariant>full</imageVariant>
      <dataRef>e2a1b3c4-1234-5678-9abc-def012345678</dataRef>
      <sourceRef></sourceRef> <!-- 废弃，保留用于向后兼容 -->
      <baseURL></baseURL>
      <appUrl></appUrl>
      <trustProxyHeaders>0</trustProxyHeaders>
      <authMethod>password</authMethod>
      <puid>1000</puid>
      <pgid>100</pgid>
      <maskProtected>1</maskProtected>
      <extraOptions></extraOptions>
      <sources>
        <source>
          <uuid>f4a2b1c0-aaaa-bbbb-cccc-111122223333</uuid>
          <sharedfolderref>a1b2c3d4-e5f6-7a8b-9c0d-1e2f3a4b5c6d</sharedfolderref>
          <path>/srv/media</path>
          <name>Media Library</name>
          <readonly>1</readonly>
          <defaultEnabled>1</defaultEnabled>
        </source>
        <source>
          <uuid>b5c3d2e1-xxxx-yyyy-zzzz-444455556666</uuid>
          <sharedfolderref>c7d8e9f0-1a2b-3c4d-5e6f-7a8b9c0d1e2f</sharedfolderref>
          <path>/srv/documents</path>
          <name>Work Documents</name>
          <readonly>0</readonly>
          <defaultEnabled>1</defaultEnabled>
        </source>
      </sources>
    </filebrowserquantum>
  </services>
</config>
```

### 1.4 ConfDB 初始化脚本 (`usr/share/openmediavault/confdb/create.d/conf.service.filebrowserquantum.sh`)

```dash
#!/usr/bin/env dash
set -e
. /usr/share/openmediavault/scripts/helper-functions

if ! omv_config_exists "/config/services/filebrowserquantum"; then
    omv_config_add_node "/config/services" "filebrowserquantum"
    omv_config_add_key "/config/services/filebrowserquantum" "enable" "0"
    omv_config_add_key "/config/services/filebrowserquantum" "version" "2.0.10-beta"
    omv_config_add_key "/config/services/filebrowserquantum" "port" "8085"
    omv_config_add_key "/config/services/filebrowserquantum" "imageVariant" "full"
    omv_config_add_key "/config/services/filebrowserquantum" "dataRef" ""
    omv_config_add_key "/config/services/filebrowserquantum" "sourceRef" ""
    omv_config_add_key "/config/services/filebrowserquantum" "baseURL" ""
    omv_config_add_key "/config/services/filebrowserquantum" "appUrl" ""
    omv_config_add_key "/config/services/filebrowserquantum" "trustProxyHeaders" "0"
    omv_config_add_key "/config/services/filebrowserquantum" "authMethod" "password"
    omv_config_add_key "/config/services/filebrowserquantum" "puid" "1000"
    omv_config_add_key "/config/services/filebrowserquantum" "pgid" "100"
    omv_config_add_key "/config/services/filebrowserquantum" "maskProtected" "1"
    omv_config_add_key "/config/services/filebrowserquantum" "extraOptions" ""
fi

# Ensure child container node 'sources' exists
if omv_config_exists "/config/services/filebrowserquantum" && \
        ! omv_config_exists "/config/services/filebrowserquantum/sources"; then
    omv_config_add_node "/config/services/filebrowserquantum" "sources"
fi

exit 0
```

### 1.5 数据库无损平滑迁移方案 (`confdb/migrations.d/conf.service.filebrowserquantum_8.1.0.sh`)

为保证从 8.0.1 升级到 8.1.0 时用户历史配置不丢失，必须提供标准的 confdb 迁移脚本：

```dash
#!/usr/bin/env dash
set -e
. /usr/share/openmediavault/scripts/helper-functions

XPATH="/config/services/filebrowserquantum"

if omv_config_exists "$XPATH"; then
    # 1. 补充新增字段
    omv_config_add_key_if_missing "$XPATH" "baseURL" ""
    omv_config_add_key_if_missing "$XPATH" "appUrl" ""
    omv_config_add_key_if_missing "$XPATH" "trustProxyHeaders" "0"
    omv_config_add_key_if_missing "$XPATH" "authMethod" "password"
    omv_config_add_key_if_missing "$XPATH" "puid" "1000"
    omv_config_add_key_if_missing "$XPATH" "pgid" "100"
    omv_config_add_key_if_missing "$XPATH" "maskProtected" "1"
    omv_config_add_key_if_missing "$XPATH" "extraOptions" ""

    # 2. 补全新建 sources 容器节点
    if ! omv_config_exists "$XPATH/sources"; then
        omv_config_add_node "$XPATH" "sources"
    fi

    # 3. 数据平滑迁移：将旧的 sourceRef 转为 sources 的第一条子记录
    OLD_SOURCE_REF="$(omv_config_get "$XPATH/sourceRef" 2>/dev/null || true)"
    if [ -n "$OLD_SOURCE_REF" ]; then
        EXISTING_SOURCES_COUNT="$(omv_config_get_count "$XPATH/sources/source")"
        if [ "$EXISTING_SOURCES_COUNT" -eq 0 ]; then
            NEW_UUID="$(python3 -c 'import uuid; print(str(uuid.uuid4()))')"
            omv_config_add_node "$XPATH/sources" "source"
            omv_config_add_key "$XPATH/sources/source[last()]" "uuid" "$NEW_UUID"
            omv_config_add_key "$XPATH/sources/source[last()]" "sharedfolderref" "$OLD_SOURCE_REF"
            omv_config_add_key "$XPATH/sources/source[last()]" "path" "/srv"
            omv_config_add_key "$XPATH/sources/source[last()]" "name" "Default Storage"
            omv_config_add_key "$XPATH/sources/source[last()]" "readonly" "0"
            omv_config_add_key "$XPATH/sources/source[last()]" "defaultEnabled" "1"
            echo "Successfully migrated legacy sourceRef to sources child record (uuid: $NEW_UUID)"
        fi
    fi
fi

exit 0
```

---

## 2. 安全边界防护与物理防递归架构（Security & Anti-recursion）

FileBrowser Quantum 具备强大的后台索引引擎（Crawler + Indexer），并且其运行配置 `config.yaml`、缓存目录 `tmp/` 和 SQLite 数据库 `filebrowser.sqlite` 均存放在 `dataRef` 共享文件夹中。若防护不严，极易引发致命的**数据自吞噬爆炸**或**凭据泄露**。

### 2.1 生产 NAS 禁读卷硬拦截机制

- **红线依据**：`AGENTS.md` / `project_rules_ops.md` 生产红线 #9
- **受保护对象**：生产 NAS 上 `/dev/md127`（LVM2 逻辑卷 UUID `6dab12c5-4b76-461a-9c3a-7f610aca2f74`，对应挂载路径前缀 `/srv/dev-disk-by-uuid-6dab12c5-4b76-461a-9c3a-7f610aca2f74`）。
- **硬拦截 —— 已移除（8.1.3，2026-11-01）**：红线 #9 的「禁读卷」**只约束 AI agent**，不代表该卷未被用户使用；用户自有服务（Immich 照片库 `immich-photos-r6`、FileBrowser `/volume1`）正常挂载该卷是操作者的选择（见 `project_memory.md` 2026-10-07 裁定）。此前把它实现成 `assertNotRestrictedVolume()` 硬拒绝，导致 `volume1` 无法作为存储源（500「points to a restricted volume」），现已删除 `RESTRICTED_VOLUME_PREFIX` 常量与该方法；R-1~R-7 的防递归与凭据遮蔽逻辑不受影响。

### 2.2 存储拓扑防递归冲突校验规则体系（R-1 至 R-7 硬拒绝闸门）

在配置生效前（PHP RPC 渲染闸门与 ctl 部署闸门），必须对所有存储源与系统目录进行拓扑关系静态计算，违背以下任一规则立即拒绝保存或启动：

| 规则编号 | 规则名称 | 校验逻辑 | 防护目的与灾难推演 |
|---|---|---|---|
| **R-1** | 必填性与引用合法性 | `dataRef` 非空且必须在 OMV 共享文件夹库中有效解析；每个 `source` 的 `sharedfolderref` 必须可解析 | 杜绝悬空引用导致挂载失败 |
| **R-2** | 根文件系统阻断 | 任何引用的宿主真实路径 `realpath($path)` 不得为系统根目录 `/` | 避免误将 Linux 根文件系统整体暴露 |
| **R-3** | 敏感保护根目录拦截 | 任何 `source` 的宿主路径不得与以下目录**完全相同**：<br>1) Compose 共享文件夹 (`conf.service.compose.sharedfolderref`)；<br>2) Docker Data-root (`/var/lib/docker`)；<br>3) RustFS 数据目录 | 保护 Compose 栈的 `.env` 凭据、Docker 容器内部状态不被直接完全暴露 |
| **R-4** | 数据与源冲突硬阻断 | 1) `dataRef == sourceRef` **绝对拒绝**；<br>2) 任何 `source` 宿主路径**不得是 `dataRef` 的严格子目录**（`isStrictDescendant(source, data)`）；<br>3) `dataRef` **不得是任何 `source` 的严格子目录**，除非启用 tmpfs 自动遮蔽 | 彻底阻断 FileBrowser Quantum 将自身运行配置和数据库作为用户文件暴露，杜绝误删数据库与无限递归扫描 |
| **R-5** | 源相互嵌套校验 | 任意两个 `source` 的宿主路径不得完全相同，且容器内映射路径 `path` 不得重名 | 杜绝挂载点覆盖与同一文件多重扫描索引 |
| **R-6** | 容器内保留目录阻断 | 容器内路径 `path` 严禁侵占系统保留前缀：`/app`, `/etc`, `/proc`, `/sys`, `/dev`, `/var`, `/usr`, `/bin`, `/sbin`, `/lib`, `/tmp`, `/home` | 保证容器操作系统基础运行环境完好 |
| **R-7** | 最大遮蔽数量限制 | 计算得出的 tmpfs 遮蔽项数量不得超过 32 项（`count($masks) <= 32`） | 保证 Docker Compose 启动性能与内核挂载结构稳定 |

### 2.3 tmpfs 物理防递归遮蔽引擎（Physical tmpfs Anti-recursion Engine）

当用户将一个大型数据盘（例如包含 Docker 目录或 Compose 共享文件夹的上级目录）添加为 `source` 时，系统自动在容器挂载层针对敏感子目录注入空只读 `tmpfs`，实现内核级物理不可见。

#### 遮蔽集合计算规范与流程

1. **识别受保护根（Protected Roots）**：
   - `data` 目录（`dataRef` 对应宿主路径，包含 `config.yaml` 与 `filebrowser.sqlite`）；
   - `compose` 共享文件夹（存储所有栈的配置与密码）；
   - `docker` 数据根目录（Docker data-root）；
   - `rustfs` 数据目录（若安装了 RustFS）。
2. **计算子路径与注入遮蔽点**：
   若受保护根 $P$ 是某个源 $S$ 的严格子路径（$P \subset S$），则计算其在容器内的相对挂载点：
   $$Target = S_{containerPath} + "/" + RelativePath(P, S_{host})$$
   对该 $Target$ 在 Compose 的 `volumes` 中注入：
   ```yaml
   - type: tmpfs
     target: %%TARGET%%
     tmpfs:
       size: 1048576 # 1 MiB 空 tmpfs
       mode: 0500    # 只读
   ```
3. **效果证明**：容器内任何进程、上游爬虫遍历到该目录时，均只看到一个空的、只读的 1MB 临时内存文件系统，物理上无法触碰宿主机底层真实数据。

### 2.4 用户身份与权限解耦（PUID/PGID 运行治理）

- **痛点**：8.0.1 强制 `user: "0:0"`，创建的文件属主为 root，破坏 NAS Samba 局域网权限。
- **重构设计**：
  - 数据模型引入 `puid`（默认 1000）与 `pgid`（默认 100）。
  - 在 Compose 模板中，若配置为非 root，动态注入环境变量：
    ```yaml
    environment:
      - PUID=%%PUID%%
      - PGID=%%PGID%%
      - UMASK=002
    ```
  - 对于容器运行身份，支持通过配置灵活切换。当用户选择使用特定 UID/GID 运行时，ctl 脚本在 `up` 阶段自动对 `dataRef` 目录进行安全的属主引导校验，确保服务平稳启动。

---

## 3. 后端 PHP RPC 模块深度重构设计 (`engined/rpc/filebrowserquantum.inc`)

### 3.1 架构解耦：消除 `get()` 网络阻塞与进程风暴

彻底废除 `get()` 中的 `latest` 外网 curl 调用与多子进程串行，将基础配置读取与耗时网络检测完全解耦：

```php
/**
 * Settings object enriched with local status information (Zero Network I/O).
 * Execution time drops from 6~14s to < 50ms.
 */
public function get($params, $context)
{
    $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
    $object = $this->getSettingsObject($context);

    // 1. 本地状态（直接读取，不阻塞）
    list(, $statusOut) = $this->runCtl(["status"], false);
    $state = trim(implode("\n", $statusOut));
    if (!in_array($state, ["running", "stopped", "notinstalled"])) {
        $state = "stopped";
    }
    $object["running"] = ("running" === $state);
    $object["statusText"] = [
        "running" => "Running",
        "stopped" => "Stopped",
        "notinstalled" => "Not installed"
    ][$state];

    // 2. 获取当前运行镜像 tag
    list(, $versionOut) = $this->runCtl(["version"], false);
    $object["runningVersion"] = trim(implode("\n", $versionOut));

    // 3. 从本地缓存读取上游版本（绝不在主线程发起实时外网请求）
    $cachedLatest = $this->readCachedLatestVersion();
    $object["latestVersion"] = $cachedLatest["version"] ?? "";
    $object["updateAvailable"] = $this->computeUpdateAvailable(
        $object["runningVersion"],
        $object["latestVersion"]
    );

    // 4. 解析 compose 文件位置与遮蔽数量
    list(, $pathOut) = $this->runCtl(["path"], false);
    $dir = trim(implode("\n", $pathOut));
    $object["composeFile"] = ("" !== $dir) ? $dir."/".self::STACK_NAME.".yml" : "";

    // 5. 计算当前拓扑遮蔽预览
    $masks = $this->computeMasksForCurrentConfig($context, $object);
    $object["maskCount"] = count($masks);
    $object["maskPreview"] = empty($masks) ? "No paths masked" : implode("\n", $masks);

    return $object;
}
```

新增独立异步查询方法 `getLatestVersion`：
```php
/**
 * Asynchronously query latest upstream version from GitHub (Triggered on demand).
 */
public function getLatestVersion($params, $context)
{
    $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
    $object = $this->getSettingsObject($context);
    $channel = (false !== strpos((string)($object["version"] ?? ""), "-beta")) ? "beta" : "stable";

    list($exitCode, $output) = $this->runCtl(["latest", $channel], false);
    $tag = trim(implode("\n", $output));
    if (0 === $exitCode && !empty($tag)) {
        $this->writeCachedLatestVersion($tag);
        return [
            "latestVersion" => $tag,
            "success" => true
        ];
    }
    return [
        "latestVersion" => "",
        "success" => false
    ];
}
```

### 3.2 多存储源 CRUD 接口规范

为驱动前端 DataTable，RPC 服务实现标准的四个数据管理方法：

#### 1. `getSources($params, $context)`
- **功能**：获取全部配置的存储源，并补充 OMV 共享文件夹名称、实际宿主路径与可读性信息。
- **返回结构**：
  ```json
  [
    {
      "uuid": "f4a2b1c0-aaaa-bbbb-cccc-111122223333",
      "sharedfolderref": "a1b2c3d4-e5f6-7a8b-9c0d-1e2f3a4b5c6d",
      "sharedFolderName": "Media",
      "path": "/srv/media",
      "name": "Media Library",
      "readonly": true,
      "defaultEnabled": true
    }
  ]
  ```

#### 2. `getSource($params, $context)`
- **参数**：`{"uuid": "..."}`
- **功能**：根据 UUID 获取单条存储源配置对象，用于编辑抽屉回显。

#### 3. `setSource($params, $context)`
- **参数**：包含 `uuid`（空则为新增，非空为修改）、`sharedfolderref`、`path`、`name`、`readonly`、`defaultEnabled`。
- **校验逻辑**：
  - 校验 `sharedfolderref` 非空并进行 R-1 ~ R-7 拓扑防递归计算；
  - 校验容器内 `path` 格式（必须以 `/` 开头，不含冒号和特殊字符，不占用系统保留路径）；
  - 更新或追加到 `conf.service.filebrowserquantum.source` 数据库；
  - 自动触发 `syncComposeStack()` 与 `syncConfigFile()`。

#### 4. `deleteSource($params, $context)`
- **参数**：`{"uuid": "..."}`
- **功能**：从数据库中删除指定源，并重新同步 Compose 栈与 `config.yaml`。

### 3.3 声明式 Compose 与 `config.yaml` 动态编排架构

解决“8.0.1 静态一次性生成 config.yaml 导致配置脱节”的核心架构设计：

#### Compose 栈声明式构建逻辑 (`buildComposeBody`)

```php
private function buildComposeBody(
    string $dataName,
    array $resolvedSources,
    array $masks,
    string $image,
    int $port,
    int $puid,
    int $pgid,
    string $baseURL,
    bool $trustProxy
): string {
    $volumes = [];
    // 1. Data 持久化挂载
    $volumes[] = "      - \${{ sf:\"{$dataName}\" }}:/home/filebrowser/data";

    // 2. 动态多源挂载
    foreach ($resolvedSources as $s) {
        $sfName = $s["sharedFolderName"];
        $cPath = $s["path"];
        $ro = $s["readonly"] ? ":ro" : "";
        $volumes[] = "      - \${{ sf:\"{$sfName}\" }}:{$cPath}{$ro}";
    }

    // 3. tmpfs 物理防递归遮蔽注入
    foreach ($masks as $m) {
        $volumes[] = "      - type: tmpfs\n        target: {$m}\n        tmpfs:\n          size: 1048576\n          mode: 0500";
    }

    $volumesYaml = implode("\n", $volumes);

    $body = <<<YAML
name: filebrowserquantum

services:
  filebrowserquantum:
    container_name: filebrowserquantum
    image: {$image}
    user: "{$puid}:{$pgid}"
    environment:
      - PUID={$puid}
      - PGID={$pgid}
      - UMASK=002
    volumes:
{$volumesYaml}
    ports:
      - '{$port}:80'
    restart: unless-stopped
YAML;

    return $body . "\n";
}
```

#### `config.yaml` 动态同步机制

在 `omv-filebrowserquantum-ctl` 中实现 `prepare_config` 的全自动声明式更新机制：
1. 每次 ctl 接收到 `up` 或 RPC 触发 `set` 时，ctl 读取 OMV 数据库中最新的 sources 列表；
2. 若 `config.yaml` 已经存在，保留用户自行在 WebUI 或文件中配置的高级属性（如认证插件、自定义主题、高级搜索等），仅**增量更新或重建 `server.sources` 数组**；
3. 若 `config.yaml` 不存在，根据 OMV 数据库生成完整的标准化初始 `config.yaml`：
   ```yaml
   server:
     cacheDir: /home/filebrowser/data/tmp
     baseURL: "%%BASE_URL%%"
     trustProxyHeaders: %%TRUST_PROXY%%
     sources:
       - path: /srv/media
         name: "Media Library"
         config:
           defaultEnabled: true
       - path: /srv/documents
         name: "Documents"
         config:
           defaultEnabled: true
   ```
4. 该机制彻底解决了 OMV 变更配置无法作用于上游运行时的问题。

---

## 4. 控制胶水脚本重构规范 (`usr/sbin/omv-filebrowserquantum-ctl`)

### 4.1 彻底废弃脆弱的 sed 正则，采用健壮解析

重写 `conf_value()` 函数，优先使用 `jq`，回退使用 `python3`，杜绝贪婪匹配和字符截断：

```sh
conf_value() {
    _id="$1"
    _key="$2"
    if command -v jq >/dev/null 2>&1; then
        omv-confdbadm read "$_id" 2>/dev/null | jq -r ".${_key} // empty"
    else
        omv-confdbadm read "$_id" 2>/dev/null | python3 -c "
import sys, json
try:
    data = json.load(sys.stdin)
    val = data.get('${_key}', '')
    if val is not None:
        print(val)
except Exception:
    pass
" 2>/dev/null
    fi
}
```

### 4.2 严谨的容器运行态状态机判定

针对 `status` 状态判定的缺陷，彻底避免 `wc -l` 统计到 docker compose 的 stderr 弃用警告：

```sh
status)
    _dir="$(stack_dir)"
    if [ -z "$_dir" ] || [ ! -f "$_dir/$STACK.yml" ]; then
        echo "notinstalled"
        exit 0
    fi
    # 使用 docker inspect 精准获取真实运行状态，不依赖脆弱的文本行数
    if command -v docker >/dev/null 2>&1; then
        _state="$(docker inspect --format '{{.State.Status}}' "$CONTAINER" 2>/dev/null || true)"
        if [ "$_state" = "running" ]; then
            echo "running"
            exit 0
        fi
    fi
    echo "stopped"
;;
```

### 4.3 部署前防护：`maskcheck` 命令规范

在 `ctl up` 与 `ctl restart` 真正拉起容器前，强制增加 `maskcheck` 校验步骤：
1. 检查当前已渲染的 `$STACK.yml`；
2. 校验 Compose 声明中所有 `volumes` 挂载点是否符合防递归要求；
3. 校验 `dataRef` 宿主目录权限，若发现不可写则尝试按照设定的 PUID/PGID 执行安全授权；
4. 校验通过后再调用 `dc up -d --remove-orphans`。

---

## 5. Salt 部署状态机与生命周期设计 (`srv/salt/omv/deploy/filebrowserquantum/`)

### 5.1 状态文件重构：`srv/salt/omv/deploy/filebrowserquantum/default.sls`

```yaml
{% set config = salt['omv_conf.get']('conf.service.filebrowserquantum') %}
{% set compose = salt['omv_conf.get']('conf.service.compose') %}
{% set stackfile = salt['cmd.run']('/usr/sbin/omv-filebrowserquantum-ctl path', python_shell=False) ~ '/filebrowserquantum.yml' %}

# 前置条件校验：必须已配置 Compose 共享文件夹与 Data 共享文件夹
{% if compose.sharedfolderref | length > 0 and config.dataRef | length > 0 %}

{% if config.enable | to_bool %}
filebrowserquantum_stack_up:
  cmd.run:
    - name: /usr/sbin/omv-filebrowserquantum-ctl up
    - onlyif: test -f '{{ stackfile }}'

filebrowserquantum_health_check:
  cmd.run:
    - name: /usr/sbin/omv-filebrowserquantum-ctl status
    - require:
      - cmd: filebrowserquantum_stack_up
{% else %}
filebrowserquantum_stack_down:
  cmd.run:
    - name: /usr/sbin/omv-filebrowserquantum-ctl down
    - onlyif: test -f '{{ stackfile }}'
{% endif %}

{% else %}
# 降级分支：前置存储尚未就绪或被解绑
filebrowserquantum_not_configured:
  test.configurable_test_state:
    - name: "FileBrowser Quantum skipped: Compose shared folder or Data folder not configured."
    - result: True
    - changes: False

# 存储解绑时安全清理残留运行容器
filebrowserquantum_cleanup_orphans:
  cmd.run:
    - name: /usr/sbin/omv-filebrowserquantum-ctl down
    - onlyif: docker inspect --format '{{.State.Status}}' filebrowserquantum 2>/dev/null | grep -q running
{% endif %}
```

**关键机制分析**：
1. **降级保护**：当 Compose 未设置或 Data 文件夹未选择时，走 `test.configurable_test_state`，OMV WebUI 点击 Apply 永远不会报红报错。
2. **孤儿容器清理**：增加 `filebrowserquantum_cleanup_orphans`，彻底修复 8.0.1 中当用户清空文件夹并应用时旧容器在后台继续占端口的缺陷。

---

## 6. Workbench 前端交互与页面解耦规范

彻底颠覆单体巨石表单，对齐 `omv-filestash` 标杆，构建 Navigation Container 树状体系。

### 6.1 前端组件与路由架构图

```
/services/filebrowserquantum (Navigation Container: 顶级导航容器)
 ├── Tab 1: /services/filebrowserquantum/settings (Settings Form: 设置与控制台)
 │    ├── 状态指标区 (Service Status, Running Version, Latest Version, Update Badge, Pending Apply)
 │    ├── 核心配置区 (Enable, Web Port, Image Tag, Image Variant)
 │    ├── 数据存储区 (Data Shared Folder, Stack File Info, PUID, PGID)
 │    ├── 网络反代区 (Base URL, App URL, Trust Proxy Headers)
 │    ├── 高级安全区 (Auto-mask Protected Dirs, Mask Preview, Extra Options)
 │    └── 动作按钮条 (Start, Stop, Restart, Upgrade, Check Update, View Logs, View Compose, Open WebUI)
 │
 └── Tab 2: /services/filebrowserquantum/sources (Sources DataTable: 多存储源管理)
      ├── 数据表格 (Shared Folder, Container Path, Label/Name, Read-only, Default Enabled)
      ├── 顶部动作 (Add Source Button)
      ├── 行级动作 (Edit Source, Delete Source)
      └── 抽屉模态框: /services/filebrowserquantum/sources/create & /edit/:uuid
           ├── Shared Folder Select (带新建按钮)
           ├── Container Path (自动补全 /srv/<name>)
           ├── Display Name (别名)
           ├── Read-only Switch (只读开关)
           └── Default Enabled Switch (默认对新用户启用)
```

### 6.2 路由清单与清单文件规划

#### 1. 导航入口：`usr/share/openmediavault/workbench/navigation.d/services.filebrowserquantum.yaml`
```yaml
version: "1.0"
type: navigation-item
data:
  path: "services.filebrowserquantum"
  text: _("FileBrowser Quantum")
  icon: "mdi:folder-network-outline"
  url: "/services/filebrowserquantum"
  permissions:
    role:
      - admin
```

#### 2. 子导航项：`navigation.d/services.filebrowserquantum.settings.yaml` 与 `sources.yaml`
- `settings`: position: 10, text: `_("Settings")`, icon: `"tune"`, url: `"/services/filebrowserquantum/settings"`
- `sources`: position: 20, text: `_("Sources")`, icon: `"mdi:folder-multiple"`, url: `"/services/filebrowserquantum/sources"`

#### 3. 路由定义：`usr/share/openmediavault/workbench/route.d/`
- `services.filebrowserquantum.yaml`: 指向 `omv-services-filebrowserquantum-navigation-page`
- `services.filebrowserquantum.settings.yaml`: url `"/services/filebrowserquantum/settings"`, editing: true, 指向 `omv-services-filebrowserquantum-settings-form-page`
- `services.filebrowserquantum.sources.yaml`: url `"/services/filebrowserquantum/sources"`, 指向 `omv-services-filebrowserquantum-sources-datatable-page`
- `services.filebrowserquantum.sources.create.yaml`: url `"/services/filebrowserquantum/sources/create"`, 指向 `omv-services-filebrowserquantum-source-form-page`
- `services.filebrowserquantum.sources.edit.yaml`: url `"/services/filebrowserquantum/sources/edit/:uuid"`, editing: true, 指向 `omv-services-filebrowserquantum-source-form-page`

### 6.3 协议自适应与外部访问跳转规范

解决硬编码 `http://` 导致的 Mixed Content 与反代跳转失败：

```yaml
- text: _("Open FileBrowser Quantum")
  execute:
    type: url
    url: >-
      /externalRedirect/{{
        (appUrl if appUrl else (
          (location() | get('protocol')) + '//' +
          (location() | get('hostname')) + ':' + port +
          (baseURL if baseURL else '')
        )) | encodeuricomponent
      }}
```
**设计优势**：
1. 若用户配置了 `appUrl`（如 `https://files.mydomain.com`），直接优先跳转自定义公网地址；
2. 否则，利用 Workbench 的 `location() | get('protocol')` 自动继承当前 OMV 管理界面的 HTTP/HTTPS 协议，杜绝跨协议混合内容拦截；
3. 自动拼接 `baseURL` 根路径。

---

## 7. 国际化 (i18n) 与工程验证工具链规范

### 7.1 完整 gettext 国际化与翻译清单

所有 Workbench 表单、DataTable 列头、按钮提示以及 RPC 返回的动态状态，均必须纳入规范化 i18n 体系：

- **运行时动态映射**（通过 `request.get.transform` 转换）：
  - `"Running"` -> zh_CN: `"运行中"`, zh_TW: `"執行中"`
  - `"Stopped"` -> zh_CN: `"已停止"`, zh_TW: `"已停止"`
  - `"Not installed"` -> zh_CN: `"未安装"`, zh_TW: `"未安裝"`
  - `"No paths masked"` -> zh_CN: `"无需遮蔽"`, zh_TW: `"無需遮蔽"`
- **关键词汇对齐表**：
  - `"Sources"` -> zh_CN: `"存储源"`, zh_TW: `"儲存來源"`
  - `"Add source"` -> zh_CN: `"添加存储源"`, zh_TW: `"新增儲存來源"`
  - `"Container path"` -> zh_CN: `"容器挂载路径"`, zh_TW: `"容器掛載路徑"`
  - `"Auto-mask protected directories"` -> zh_CN: `"自动遮蔽敏感保护目录"`, zh_TW: `"自動遮蔽敏感保護目錄"`
  - `"Check for latest version"` -> zh_CN: `"检查最新版本"`, zh_TW: `"檢查最新版本"`

### 7.2 工程质量测试与验证工具链引入

在项目根目录构建标准工作区脚本：

1. **`scripts/validate-workbench.py`**：
   - 静态解析所有 `workbench/{component.d,route.d,navigation.d}/*.yaml`；
   - 验证组件名称唯一性与路由合法性；
   - 校验所有 `_("...")` 翻译字符串均完整存在于 `openmediavault-filebrowser-quantum.pot` 和 `zh_CN`/`zh_TW` 的 `.po` 文件中；
   - 强制校验所有提交源码文本为 **LF 行尾**。
2. **`scripts/test_anti_recursion.py`**：
   - 单元测试 R-1 至 R-7 规则的拦截表现；
   - 验证路径父子关系计算（`isStrictDescendant`、`getRelativePath`）；
   - 测试 tmpfs 遮蔽计算引擎对嵌套共享文件夹的生成结果。
3. **`scripts/build_locale.py`**：
   - 提取代码与前端 YAML 中的最新 msgid，保持 POT 与双语 PO 文件绝对同步。

---

## 8. 版本跃升、演进路线图与验收检查清单 (DoD)

### 8.1 版本命名与实施计划

- **版本跃升**：从 8.0.1 直接升至 **8.1.0**（体现多源架构与模型层重大演进）。
- **任务推进顺序**：
  - **阶段一 (后端与模型实施)**：实现 `datamodels/` 双模型、`confdb/create.d` 与 `migrations.d` 迁移脚本；重构 `engined/rpc/filebrowserquantum.inc`（多源 CRUD、防递归 R1-R7、异步版本检测）。
  - **阶段二 (胶水与部署实施)**：重构 `omv-filebrowserquantum-ctl`（`jq` 解析、状态机、动态 `prepare_config`、`maskcheck`）；优化 Salt 状态文件与孤儿容器清理。
  - **阶段三 (前端解耦实施)**：拆分单体表单为 Navigation Container、Settings Form、Sources DataTable、Source Form；适配反代协议自适应。
  - **阶段四 (质量与验证保障)**：运行 `validate-workbench.py`、`test_anti_recursion.py`，全量更新 POT/PO 翻译；在本地执行 `dpkg-buildpackage` 确认打包无报错。

### 8.2 验收交付标准（Definition of Done）清单

- [ ] **DOD-1 数据模型与迁移**：
  - `conf.service.filebrowserquantum.json` 与 `conf.service.filebrowserquantum.source.json` 通过 JSON 语法校验。
  - 8.0.1 遗留配置在安装 8.1.0 后，原有的 `sourceRef` 自动且无损地转换为子存储源第一条记录。
- [ ] **DOD-2 安全与防递归**：
  - 生产数据卷（UUID `6dab12c5-4b76-461a-9c3a-7f610aca2f74`）**允许**作为存储源（8.1.3 起；红线 #9 仅约束 agent，插件不再拦截）。
  - 设置 `dataRef == sourceRef` 或嵌套路径时，系统精准拒绝并提示。
  - 嵌套 Compose 目录时，Compose 栈中能够正确渲染注入只读 tmpfs 遮蔽项。
- [ ] **DOD-3 RPC 性能**：
  - 调用 `get()` RPC 在无外网环境下响应耗时 $\le 100\text{ms}$，绝无超时与卡顿。
  - 独立调用的 `getLatestVersion` 能够正确拉取 GitHub 最新 tag 并本地缓存。
- [ ] **DOD-4 控制脚本可靠性**：
  - `omv-filebrowserquantum-ctl` 内部无任何脆弱的 sed 正则提取 JSON。
  - `status` 命令输出稳定为 `running`、`stopped` 或 `notinstalled` 三态之一。
- [ ] **DOD-5 前端体验解耦**：
  - Workbench 呈现清晰的“设置”与“存储源”双 Tab 页面。
  - 支持在存储源页面进行新增、修改、删除多个共享文件夹。
  - “打开 Web 界面”按钮在 HTTPS 或反向代理环境下协议自适应正常。
- [ ] **DOD-6 工具链与规范**：
  - `scripts/validate-workbench.py` 运行通过，0 警告 0 报错。
  - `scripts/test_anti_recursion.py` 单元测试全部 passed。
  - 全部中英翻译完整覆盖，无任何缺失的 msgid。

---

## 9. 结论

本规范彻底解决了现有 `openmediavault-filebrowser-quantum` 中的 P0/P1 缺陷，全面对齐了工作区 `omv-filestash` 与 `omv-immich` 成熟标杆。通过引入双模型架构、多源动态同步、双闸门防递归与组件解耦，将原本受限的单源 PoC 原型升级为健壮、安全、可扩展的企业级 OMV 原生管理插件，为后续代码落地提供了完整、确定、可验证的技术指南。
