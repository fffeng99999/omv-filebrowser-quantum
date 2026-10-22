# openmediavault-filebrowser-quantum

OMV 8.x 原生插件（deb 包），把自托管网页文件管理器 **FileBrowser Quantum** 做成
WebUI 托管：`服务 → FileBrowser Quantum` 页配置参数、按钮部署/启停/升级、实时看容器状态与版本。

> 插件只做 **OMV 侧集成**（Compose 栈注册 + 容器生命周期 + 状态展示），**不改动 FileBrowser Quantum 自身功能**；
> 源、用户、分享链接等业务全部在 FileBrowser Quantum 原生网页界面管理。
>
> - 上游项目：https://github.com/gtsteffaniak/filebrowser ｜ 文档：https://filebrowserquantum.com
> - 本插件定位：**FileBrowser Quantum v2 系列**（`config.yaml` + SQLite）。v1.5.x 使用不同的配置格式，**不受支持**（保存时会被拒绝）。
> - 插件版本 8.0.1，OMV 大版本 8.x（`Depends: openmediavault (>= 8.0), openmediavault-compose (>= 8.2.0)`）。

## 与 omv-extras 的 `openmediavault-filebrowser` 的区别

omv-extras 里另有一个 `openmediavault-filebrowser` 插件，封装的是**已归档停维**的原版 FileBrowser。
本仓库封装的是其活跃分支 **FileBrowser Quantum**，包名不同（`openmediavault-filebrowser-quantum`），两者可并存。

## 集成模式

按工作区 `.dsh/skills/omv-plugin-dev/references/integration-modes.md` 的三轨决策树，
FileBrowser Quantum 上游**只发 Docker 镜像** → 走 **轨 1（Compose 轨）**，参照 `omv-immich`：

- 保存设置时插件调用 Compose RPC（`Compose.setFile`）把栈注册进 `conf.service.compose.file`，
  由 Compose 插件渲染 `<compose 共享文件夹>/filebrowserquantum/filebrowserquantum.yml` 与 `.env`；
  因此打开 `Services → Compose → Files / Services` 就能看到、编辑、启停该栈。**插件不自己写这些文件**（单一写入者）。
- 「应用」时 Salt state（`srv/salt/omv/deploy/filebrowserquantum/`）只做生命周期：`enable` 分支
  `omv-compose-run filebrowserquantum up -d`、关闭分支 `down --remove-orphans`（**绝不带 -v、绝不删数据**）；
  Compose 插件未配置共享文件夹或存储未选择时整段跳过，**绝不导致 Apply 失败**。

## 数据模型（`conf.service.filebrowserquantum`）

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `enable` | boolean | false | 总开关（默认关）。应用时按此建/删容器 |
| `version` | string | `2.0.10-beta` | 镜像标签（不含前导 `v`）。固定具体版本，禁止浮动标签 |
| `port` | integer | 8085 | 宿主 Web 端口（容器内部固定监听 80） |
| `imageVariant` | enum | `full` | `full`（含 FFmpeg+文档预览）/ `slim`（仅核心服务）；映射为标签后缀 `-slim` |
| `dataRef` | sharedfolder ref | "" | 配置 + SQLite 数据库 + 缓存（挂载 `/home/filebrowser/data`） |
| `sourceRef` | sharedfolder ref | "" | 被浏览的文件源（读写挂载 `/srv`） |

两个目录类字段都是 **OMV 共享文件夹引用**（`sharedFolderSelect` 下拉），数据天然落在阵列上；
渲染进 compose 时用 Compose 插件占位符 `${{ sf:"<共享文件夹名>" }}`，路径变化由 Compose 在部署时解析。

## 生成的 Compose 栈（单容器）

```yaml
name: filebrowserquantum

services:
  filebrowserquantum:
    container_name: filebrowserquantum
    image: ghcr.io/gtsteffaniak/filebrowser:2.0.10-beta   # 固定 tag
    user: "0:0"                                            # root，使容器可读写任意属主的源目录
    volumes:
      - ${{ sf:"<data 共享文件夹>" }}:/home/filebrowser/data
      - ${{ sf:"<source 共享文件夹>" }}:/srv
    ports:
      - '8085:80'
    restart: unless-stopped
```

> 镜像仓库取上游 v2 文档 Docker 示例里的 `ghcr.io/gtsteffaniak/filebrowser`（上游同时在 Docker Hub 发布
> `gtstef/filebrowser`，两者等价）。若某网络环境拉不动 ghcr.io，把 `IMAGE_REPO`
> （`usr/share/openmediavault/engined/rpc/filebrowserquantum.inc`）改为 `gtstef/filebrowser` 即可。

## config.yaml

首次「应用」**或**在插件页点「启动」时，ctl（`omv-filebrowserquantum-ctl up`）会在 **data 共享文件夹**里
创建最小 `config.yaml`（**已存在则永不覆盖**，用户改过的不受影响）。Salt 的启用/关闭分支都经由同一个 ctl，
因此「应用」与「启动」按钮走同一条代码路径：

```yaml
server:
  cacheDir: /home/filebrowser/data/tmp
  sources:
    - path: /srv
      config:
        defaultEnabled: true
```

源在容器内固定挂载到 `/srv`，因此**移动 source 共享文件夹时 config.yaml 无需改动**。
需要多源时，直接编辑 data 共享文件夹里的 `config.yaml`（插件不覆盖它）。

## 首次登录

FileBrowser Quantum v2 在首次启动时若管理员密码为默认值，会**生成随机密码并只写进容器日志一次**。
部署后点「查看日志」获取，打开网页界面后立即修改密码。

## 目录结构

```
omv-filebrowser-quantum/
├── debian/                                          # deb 打包控制文件
├── srv/salt/omv/deploy/filebrowserquantum/          # Salt：生命周期（经 ctl）
├── usr/sbin/omv-filebrowserquantum-ctl              # 生命周期/状态/升级/日志 ctl + config.yaml 首建
└── usr/share/openmediavault/
    ├── confdb/create.d/                             # 配置数据库初始化
    ├── datamodels/                                  # 配置模型
    ├── engined/{rpc,module}/                         # RPC 服务 + 引擎模块
    ├── locale/{zh_CN,zh_TW}/                        # 中英翻译
    └── workbench/{navigation.d,route.d,component.d}/ # 导航/路由/表单页
```

## RPC

服务名 `FileBrowserQuantum`：`get` / `set` / `start` / `stop` / `restart` / `upgrade` / `getLog` / `getShowConfig`。
`start/stop/restart/upgrade/getLog/getShowConfig` 均以后台任务执行（`omv-compose-run` 的实时输出）。

## 构建与发布

push tag `v*` → GitHub Actions（`.github/workflows/build.yml`）：lint（PHP/JSON/YAML/shell/PO）+ 构建 `_all.deb` + 附到 Release。
`${GITHUB_USER}` 占位符由 `render-vars.sh` 按 `debian/variables.env` 在 `dpkg-buildpackage` 前渲染。

本地构建：

```sh
sh ./render-vars.sh
dpkg-buildpackage -us -uc -b
```

安装：`apt-get install -y ./openmediavault-filebrowser-quantum_8.0.1_all.deb`（postinst 自动 `update-workbench` + `restart-engined`，**不启动服务**）。
卸载：`apt purge openmediavault-filebrowser-quantum`（数据/源共享文件夹一律保留）。
