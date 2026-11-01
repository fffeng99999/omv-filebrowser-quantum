# openmediavault-filebrowser-quantum

OMV 8.x 原生插件（deb 包），把自托管网页文件管理器 **FileBrowser Quantum** 做成 WebUI 托管：
`服务 → FileBrowser Quantum` 页配置参数、按钮部署/启停/升级、实时看容器状态、健康与版本。

> 插件只做 **OMV 侧集成**（Compose 栈注册 + 容器生命周期 + 状态展示 + 上游 `config.yaml` 的声明式同步），
> **不改动 FileBrowser Quantum 自身功能**；用户、分享链接、访问规则等业务全部在 FileBrowser Quantum 原生网页界面管理。
>
> - 上游项目：https://github.com/gtsteffaniak/filebrowser ｜ 文档：https://filebrowserquantum.com
> - 本插件定位：**FileBrowser Quantum v2 系列**（`config.yaml` + SQLite）。v1.5.x 使用不同的配置格式，**不受支持**（保存时会被拒绝）。
> - 当前版本 **8.1.5**，OMV 大版本 8.x（`Depends: openmediavault (>= 8.0), openmediavault-compose (>= 8.2.0)`）。

## 与 omv-extras 的 `openmediavault-filebrowser` 的区别

omv-extras 里另有一个 `openmediavault-filebrowser` 插件，封装的是**已归档停维**的原版 FileBrowser。
本仓库封装的是其活跃分支 **FileBrowser Quantum**，包名不同（`openmediavault-filebrowser-quantum`），两者可并存。

## 集成模式

按工作区 `.dsh/skills/omv-plugin-dev/references/integration-modes.md` 的三轨决策树，
FileBrowser Quantum 上游**只发 Docker 镜像** → 走 **轨 1（Compose 轨）**，参照 `omv-immich`：

- 保存设置时插件调用 Compose RPC（`Compose.setFile`）把栈注册进 `conf.service.compose.file`，
  由 Compose 插件渲染 `<compose 共享文件夹>/filebrowserquantum/filebrowserquantum.yml` 与 `.env`；
  因此打开 `Services → Compose → Files / Services` 就能看到、编辑、启停该栈。**插件不自己写这些文件**（单一写入者）。
- 「应用」时 Salt state（`srv/salt/omv/deploy/filebrowserquantum/`）只做生命周期，两个分支都经
  `/usr/sbin/omv-filebrowserquantum-ctl`：启用分支 `up`、关闭分支 `down`（**绝不带 -v、绝不删数据**）。
- **启动门控（fail-closed）**：`ctl up/restart/upgrade` 与 RPC `start/restart/upgrade` 先跑同一套检查
  （`fbq_tools.py gate`）——**必须已启用、数据目录已配置且存在、至少一个存储源可解析、Compose 共享文件夹已配置**，
  否则直接失败。RPC 另外要求没有未应用的脏配置（FileBrowser Quantum 或 sharedfolders 模块）。
  因此“数据目录未配置也能按旧栈启动”的路径已经不存在。
- **配置变不完整时撤栈**：删除最后一个存储源或清空数据目录后，已注册的栈会被注销（容器随之停止），
  而不是留着一个仍在暴露旧挂载的容器；首次配置（本来没有栈）与“已有栈变不完整”是两条不同路径。

## 数据模型（`conf.service.filebrowserquantum`）

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `enable` | boolean | false | 总开关（默认关）。应用时按此建/删容器；**未启用时 Start 直接失败** |
| `version` | string | `2.0.10-beta` | 镜像标签（不含前导 `v`）。固定标签 + Upgrade 会先推进版本 |
| `port` | integer | 8085 | 宿主 Web 端口（容器内部固定监听 80） |
| `imageVariant` | enum | `full` | `full` / `slim`；映射为标签后缀 `-slim` |
| `dataRef` | sharedfolder ref | "" | 配置 + SQLite + 缓存（挂载 `/home/filebrowser/data`） |
| `baseURL` | string | "" | 写入 `http.baseURL`；清空即真正清除 |
| `appUrl` | string | "" | 写入 `http.externalUrl`（只写 origin，上游自行追加 baseURL） |
| `trustProxyHeaders` | boolean | false | 写入 `http.trustProxyHeaders` |
| `authMethod` | enum | `password` | `password` / `noauth` / `proxy`，写入 `auth.methods` |
| `authProxyHeader` | string | `X-Forwarded-User` | `auth.methods.proxy.header`，仅 proxy 方式使用 |
| `puid` / `pgid` | integer | 1000 / 100 | 容器运行身份；`0:0` = root 语义（root 属主目录的唯一可行方案） |
| `maskProtected` | boolean | true | 反递归 tmpfs 遮蔽；关闭时含保护目录的存储源会被拒绝 |
| `extraOptions` | string | "" | 追加环境变量（KEY=VALUE，每行一条） |
| `sourceRef` | sharedfolder ref | "" | **8.1.0 起弃用**（迁移脚本会转成一条存储源），保留仅为兼容 |

存储源是独立 iterable 模型 `conf.service.filebrowserquantum.source`（Master-Detail，任意多条）：

| 字段 | 说明 |
|---|---|
| `uuid` | 源自身的 UUID |
| `sharedfolderref` | **身份**：OMV 共享文件夹 UUID（不是路径） |
| `path` | 容器内挂载点（`/srv/...`）；留空时保存按共享文件夹名派生 |
| `name` | 显示名（默认取共享文件夹名） |
| `readonly` | 以 `:ro` 挂载 |
| `defaultEnabled` | 上游 `config.defaultEnabled`，新用户默认可见 |

**共享文件夹身份 = UUID**。Workbench 下拉为了能「选中即自动填充容器路径」，提交的是
`/srv/<共享文件夹名>` 这个派生值；两条名字清洗后相撞（`My Photos` 与 `My_Photos` → 都是 `/srv/My_Photos`）
时，派生值会带确定性的 UUID 后缀（`/srv/My_Photos-857c45ee`），反查因此永远唯一；
反查命中多条时**报错**而不是取第一条。API 客户端也可直接提交 UUID。

## 生成的 Compose 栈（单容器）

```yaml
name: filebrowserquantum

services:
  filebrowserquantum:
    container_name: filebrowserquantum
    image: ghcr.io/gtsteffaniak/filebrowser:2.0.10-beta   # 固定 tag
    user: "0:0"                                            # 取自 puid/pgid
    restart: unless-stopped
    ports:
      - '8085:80'
    volumes:
      - ${{ sf:"<data 共享文件夹>" }}:/home/filebrowser/data
      - ${{ sf:"<存储源共享文件夹>" }}:/srv/pool:ro
    tmpfs:                                                 # 反递归遮蔽（见下）
      - /srv/pool/compose:size=1048576,mode=0500
    environment:
      - PUID=0
      - PGID=0
      - UMASK=002
    security_opt:
      - no-new-privileges:true
```

> 镜像仓库取上游 v2 文档 Docker 示例里的 `ghcr.io/gtsteffaniak/filebrowser`（上游同时在 Docker Hub 发布
> `gtstef/filebrowser`，两者等价）。若某网络环境拉不动 ghcr.io，把 `IMAGE_REPO`
> （`usr/share/openmediavault/engined/rpc/filebrowserquantum.inc`）改为 `gtstef/filebrowser` 即可。
>
> 环境变量只保留容器运行时的 `PUID`/`PGID`/`UMASK` 与用户自定义项；
> baseURL、externalUrl、认证方式**不再**通过 `FILEBROWSER_BASEURL` / `APPLICATION_URL` /
> `FILEBROWSER_NOAUTH` 传递——上游 2.x 不读这些变量。

## config.yaml（声明式同步）

`ctl up/restart/upgrade` 与「应用」都会先跑 `prepare_config`（`fbq_tools.py prepare-config`）：

- 只改插件拥有的键：`http.baseURL`、`http.externalUrl`、`http.trustProxyHeaders`、
  `auth.methods.noauth|password.enabled|proxy.enabled|proxy.header`、`server.cacheDir`、`server.sources`；
  **其他上游自定义项（branding、访问规则、其他认证方式…）原样保留**。
- **解析失败不覆盖**：`config.yaml` 无法解析（YAML 错误或不是映射）时中止启动并保留原文件，
  提示修复或删除后重试——不会再被当成空配置覆盖。
- **原子写入**：临时文件 + `fsync` + `os.replace`，替换前把旧文件备份为 `config.yaml.omv-bak`。
- 认证映射与上游一致：`noauth: true` 时同时关闭 password/proxy；`proxy` 时写 `enabled` + `header`。
- 由于上游会把 `http.baseURL` 追加到 `http.externalUrl`，写入的 externalUrl **只保留 origin**，
  「打开」按钮也用 origin + baseURL 拼接，因此不会出现 `/files/files`。

示例（反向代理子路径 + 代理认证）：

```yaml
http:
  baseURL: /files
  externalUrl: https://files.example.com
  trustProxyHeaders: true
server:
  cacheDir: /home/filebrowser/data/tmp
  sources:
    - path: /srv/pool
      name: pool
      config:
        defaultEnabled: true
auth:
  methods:
    noauth: false
    password:
      enabled: false
    proxy:
      enabled: true
      header: X-Forwarded-User
```

## 反递归与遮蔽

- 当 **Compose 共享文件夹 / Docker data-root / RustFS 数据目录 / 插件数据目录** 落在某个存储源内部时，
  自动在其容器路径处挂一个空的只读 1 MiB tmpfs（`mode=0500`），阻止递归与凭据泄露。
- `maskcheck`（启动前）核对 `yml` 与实时拓扑：M1 数据挂载可写、**M2 bind 集合双向一致**
  （栈里多出的挂载、配置里有但没渲染的源都报错，含 `:ro` 漂移）、M3/M4 保留路径、M5 遮蔽条目参数与数量、
  M6 遮蔽集合与当前布局一致（多/少都算脏，提示重新保存并应用）。
- 关闭 `maskProtected` **不再等于“暴露”**：包含上述敏感目录的存储源会被直接拒绝，必须选更窄的共享文件夹。

## 生命周期、按钮与反馈

- 按钮按真实状态启用：`Start`（已配置+已启用+未运行）、`Stop`（运行中）、`Restart`、`Upgrade`（已配置+已启用）、`Open`（运行中）。
- 任务对话框结束后页面**不会自动刷新**：用 **「刷新状态」** 按钮重新执行 `get`，状态/运行版本/遮蔽预览一并更新。
- **健康**：UI 展示 Docker 健康状态（`healthy`/`unhealthy`/`starting`/`无健康检查`）；
  Salt 应用结束时跑 `ctl verify`，容器未运行或 Docker 报 `unhealthy` 会让 **Apply changes 失败**，不再静默成功。
- **升级**：`Upgrade` 先解析上游最新正式版本 → 固定标签写入配置库 → 重渲染栈 → `docker compose pull` → `up -d`；
  `pull` 失败直接让任务失败（旧版 `--ignore-pull-failures || true` 会假成功）。浮动标签（`beta`/`stable`）不改写版本。
- **检查更新**：查询失败（NAS 到不了 github.com 等）直接抛错显示原因，不再固定弹“查询成功”。
- **权限预检**：设置页「Permission check」在加载时按当前 `PUID/PGID` 逐个检查数据目录与存储源的读权限，
  并提示 root 语义与 ACL 说明（仅供提示，不作为硬性拦截）。

## 首次登录

FileBrowser Quantum v2 在首次启动时若管理员密码为默认值，会**生成随机密码并只写进容器日志一次**。
部署后点「查看日志」获取，打开网页界面后**立即修改密码**，不要继续沿用默认/初始凭据。

## 目录结构

```
omv-filebrowser-quantum/
├── debian/                                             # deb 打包控制文件
├── srv/salt/omv/deploy/filebrowserquantum/             # Salt：生命周期 + 健康校验（经 ctl）
├── usr/sbin/omv-filebrowserquantum-ctl                 # 生命周期/状态/升级/日志 ctl（fail-closed）
├── usr/share/openmediavault/filebrowserquantum/
│   └── fbq_tools.py                                    # 门控 / config.yaml 同步 / maskcheck / verify（生产逻辑）
├── scripts/                                            # 测试与工具（跑生产代码）
└── usr/share/openmediavault/
    ├── confdb/{create.d,migrations.d}/                 # 配置数据库初始化与迁移
    ├── datamodels/                                     # 配置模型
    ├── engined/{rpc,module}/                           # RPC 服务 + 引擎模块
    ├── locale/{zh_CN,zh_TW}/                           # 中英翻译
    └── workbench/{navigation.d,route.d,component.d}/   # 导航/路由/表单页
```

## RPC

服务名 `FileBrowserQuantum`：`get` / `set` / `getLatestVersion` / `getSources` / `getSource` / `setSource` /
`deleteSource` / `getSharedFolderChoices` / `start` / `stop` / `restart` / `upgrade` / `getLog` / `getShowConfig`。
`start/stop/restart/upgrade/getLog/getShowConfig` 以后台任务执行（`omv-compose-run` 的实时输出）。

## 构建与发布

- CI（`.github/workflows/build.yml`）**只允许手动触发**（`workflow_dispatch`），**只编译不发版**：
  lint（PHP/JSON/YAML/shell/PO + `scripts/test_source_path.php` + `scripts/test_anti_recursion.py`）→ 打包 `_all.deb` → **workflow artifact**。
- `${GITHUB_USER}` 占位符由 `render-vars.sh` 按 `debian/variables.env` 在 `dpkg-buildpackage` 前渲染。
- **发版是独立人工动作**：用户明确要求后才 `gh release create v<版本号> --generate-notes dist/*.deb`。

本地构建：

```sh
sh ./render-vars.sh
dpkg-buildpackage -us -uc -b
```

安装：`apt-get install -y ./openmediavault-filebrowser-quantum_8.1.5_all.deb`（postinst 自动 `update-workbench` + `restart-engined`，**不启动服务**）。
卸载：`apt purge openmediavault-filebrowser-quantum`（数据/源共享文件夹一律保留）。

## 测试

```sh
make validate   # YAML/路由/i18n + Constraint 结构（含 requiredIf 写法）校验
make test       # 反递归/门控/config.yaml（执行 fbq_tools.py 生产代码）+ 有 php 时跑 scripts/test_source_path.php
make locale     # 重新生成 .pot 与 zh_CN/zh_TW
```

`scripts/test_anti_recursion.py` **直接 import `fbq_tools.py`**，跑的就是 ctl 会执行的算法；
`scripts/test_source_path.php` 覆盖共享文件夹派生/碰撞消歧、URL 规范化与往返、升级标签推进、栈注销。

## 故障排查

- **网页 403**：看设置页「Permission check」。共享文件夹是 `root:root 700` 时 PUID 1000 读不到，
  可改 `PUID/PGID=0`（root 语义）或用 ACL 授权。
- **Apply 失败且提示容器不健康**：`ctl logs` 看容器日志；Spring/启动失败时 Salt 会如实报错。
- **提示 mask 集合过期**：存储布局变了（新增/删除共享文件夹、挂载点变化），到设置页「保存」再「应用」。
- **config.yaml 解析失败**：插件不会覆盖它，修复 YAML 或删除该文件后重新应用（`config.yaml.omv-bak` 是上一份自动备份）。
