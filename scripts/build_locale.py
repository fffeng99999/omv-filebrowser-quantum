#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Builder for openmediavault-filebrowser-quantum gettext catalogs.

Emits:
  usr/share/openmediavault/locale/openmediavault-filebrowser-quantum.pot
  usr/share/openmediavault/locale/zh_CN/openmediavault-filebrowser-quantum.po
  usr/share/openmediavault/locale/zh_TW/openmediavault-filebrowser-quantum.po

Keeps .pot and Chinese PO catalogs 100% in sync with Workbench manifests.
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
WB = ROOT / "usr" / "share" / "openmediavault" / "workbench"
LOC = ROOT / "usr" / "share" / "openmediavault" / "locale"
PKG = "openmediavault-filebrowser-quantum"
VER = "8.1.5"
DATE = "2026-11-01 19:30+0800"

GET = re.compile(r'_\(\s*(["\'])(.*?)\1\s*\)', re.DOTALL)

# Runtime statusText and maskPreview values returned by RPC
STATUS = ["Running", "Stopped", "Not installed"]
RUNTIME = STATUS + ["No paths masked", "Healthy", "Unhealthy", "Starting", "No health check", "Unknown"]

# Translation mapping: key -> (zh_CN, zh_TW)
T: dict[str, tuple[str, str]] = {
    "Running": ("运行中", "執行中"),
    "Stopped": ("已停止", "已停止"),
    "Not installed": ("未安装", "未安裝"),
    "No paths masked": ("无需遮蔽", "無需遮蔽"),
    "Healthy": ("健康", "健康"),
    "Unhealthy": ("不健康", "不健康"),
    "Starting": ("启动中", "啟動中"),
    "No health check": ("无健康检查", "無健康檢查"),
    "Unknown": ("未知", "未知"),
    "FileBrowser Quantum is a modern, high-performance web file manager. The container is created in the Compose plugin (Services -> Compose -> Files) where it can also be inspected; configure the Compose plugin shared folder first (Services -> Compose -> Settings). Select the data shared folder, configure storage sources on the Sources tab, save, then click Apply and use the buttons below to deploy and manage the container.": ("FileBrowser Quantum 是现代、高性能网络文件管理器。容器在 Compose 插件中创建（服务 → Compose → 文件），也可在那里查看；请先配置 Compose 插件的共享文件夹（服务 → Compose → 设置）。选择数据共享文件夹，在「存储源」标签页配置存储源，保存并点「应用」，然后用下方按钮部署和管理容器。", "FileBrowser Quantum 是現代、高效能網路檔案管理員。容器在 Compose 外掛中建立（服務 → Compose → 檔案），也可在那裡檢視；請先設定 Compose 外掛的共享資料夾（服務 → Compose → 設定）。選取資料共享資料夾，在「儲存來源」分頁設定儲存來源，儲存並點「套用」，再用下方按鈕部署與管理容器。"),
    "This plugin manages the FileBrowser Quantum v2 line (config.yaml and a SQLite database in the data folder); the v1.5.x line uses a different configuration format and is not supported. On the first start a random admin password is generated and written to the container log - open the web interface and change it immediately.": ("本插件管理 FileBrowser Quantum v2 产品线（数据文件夹中的 config.yaml 和 SQLite 数据库）；v1.5.x 产品线采用不同配置格式且不受支持。初次启动时会生成随机管理员口令并写入容器日志——请打开 Web 界面并立即修改。", "本外掛管理 FileBrowser Quantum v2 產品線（資料資料夾中的 config.yaml 與 SQLite 資料庫）；v1.5.x 產品線採用不同設定格式且不受支援。首次啟動時會產生隨機管理員密碼並寫入容器日誌——請開啟 Web 介面並立即修改。"),
    "The buttons act on the saved configuration. After a task finishes, click 'Refresh status' to reload the service state, the running version and the mask preview; a deployment that Docker reports as unhealthy makes Apply changes fail.": ("按钮作用于已保存的配置。任务结束后请点「刷新状态」重新读取服务状态、运行版本与遮蔽预览；Docker 报告容器不健康时，「应用」会失败。", "按鈕作用於已儲存的設定。任務結束後請點「重新整理狀態」重新讀取服務狀態、執行版本與遮蔽預覽；Docker 回報容器不健康時，「套用」會失敗。"),
    "Status": ("状态", "狀態"),
    "Service status": ("服务状态", "服務狀態"),
    "Container health": ("容器健康状态", "容器健康狀態"),
    "Health reported by Docker for the running container. 'No health check' means the image does not define one.": ("由 Docker 报告的运行中容器健康状态。「无健康检查」表示镜像未定义健康检查。", "由 Docker 回報的執行中容器健康狀態。「無健康檢查」表示映像未定義健康檢查。"),
    "Running version": ("运行版本", "執行版本"),
    "Latest version": ("最新版本", "最新版本"),
    "Update available": ("有可用更新", "有可用更新"),
    "Configured sources": ("已配置存储源数", "已設定儲存來源數"),
    "Number of storage sources configured on the Sources tab.": ("在「存储源」标签页配置的存储源数量。", "在「儲存來源」分頁設定的儲存來源數量。"),
    "Service": ("服务", "服務"),
    "Enabled": ("启用", "啟用"),
    "Automatically manage the container from OMV. On Apply the container is (re)created; disabling the service removes the container on Apply (the data folder and storage sources are kept). The data folder and at least one storage source must be configured first.": ("由 OMV 自动管理容器。应用时（重新）创建容器；禁用服务会在应用时移除容器（数据文件夹和存储源保留）。必须先配置数据文件夹和至少一个存储源。", "由 OMV 自動管理容器。套用時（重新）建立容器；停用服務會在套用時移除容器（資料資料夾與儲存來源保留）。必須先設定資料資料夾與至少一個儲存來源。"),
    "Web port": ("Web 端口", "Web 連接埠"),
    "Port of the FileBrowser Quantum web interface on the host (the container listens on port 80 internally).": ("FileBrowser Quantum Web 界面在宿主机上的端口（容器内监听端口 80）。", "FileBrowser Quantum Web 介面在宿主機上的連接埠（容器內部監聽連接埠 80）。"),
    "Image tag": ("镜像标签", "映像標籤"),
    "Docker image tag of the ghcr.io/gtsteffaniak/filebrowser image, without a leading 'v' (e.g. 2.0.10-beta). Use a v2 tag; a specific version (not 'beta' or 'stable') keeps the deployment reproducible. The Upgrade button advances a pinned tag to the latest upstream release before it pulls the image.": ("ghcr.io/gtsteffaniak/filebrowser 镜像的 Docker 标签，不含开头的 'v'（例如 2.0.10-beta）。请使用 v2 标签；使用具体版本号（而非 'beta' 或 'stable'）可确保部署可复现。「升级」按钮会先把固定标签推进到上游最新发布版本，再拉取镜像。", "ghcr.io/gtsteffaniak/filebrowser 映像的 Docker 標籤，不含開頭的 'v'（例如 2.0.10-beta）。請使用 v2 標籤；使用具體版本號（而非 'beta' 或 'stable'）可確保部署可重現。「升級」按鈕會先把固定標籤推進到上游最新發布版本，再拉取映像。"),
    "FileBrowser v1.x tags are not supported. Use a v2 tag (e.g. 2.0.10-beta).": ("不支持 FileBrowser v1.x 标签。请使用 v2 标签（例如 2.0.10-beta）。", "不支援 FileBrowser v1.x 標籤。請使用 v2 標籤（例如 2.0.10-beta）。"),
    "Image variant": ("镜像变体", "映像變體"),
    "'Full' includes FFmpeg and document previews (larger image); 'Slim' is the core service only (much smaller, no media/document previews).": ("'Full' 包含 FFmpeg 和文档预览（镜像体积较大）；'Slim' 仅包含核心服务（体积小得多，无媒体/文档预览）。", "'Full' 包含 FFmpeg 與文件預覽（映像體積較大）；'Slim' 僅包含核心服務（體積小得多，無媒體/文件預覽）。"),
    "Full (media and document previews)": ("Full（含多媒体与文档预览）", "Full（含多媒體與文件預覽）"),
    "Slim (core service only)": ("Slim（仅核心服务）", "Slim（僅核心服務）"),
    "Storage": ("存储", "儲存"),
    "Stack file (Compose plugin)": ("栈文件（Compose 插件）", "堆疊檔案（Compose 外掛）"),
    "Stack file location. The stack files are rendered by the Compose plugin into its shared folder and are managed there, so this path cannot be changed here. Set the folder in Services -> Compose -> Settings.": ("栈文件位置。栈文件由 Compose 插件渲染在其共享文件夹中并在那里管理，因此无法在此修改。请在 服务 → Compose → 设置 中配置该文件夹。", "堆疊檔案位置。堆疊檔案由 Compose 外掛在其共享資料夾中管理，因此無法在此修改。請在 服務 → Compose → 設定 中設定該資料夾。"),
    "Data shared folder": ("数据共享文件夹", "資料共享資料夾"),
    "Shared folder for the FileBrowser Quantum configuration, its SQLite database and the cache (mounted at /home/filebrowser/data). Put it on the data array. Changing it later starts from an empty configuration in the new folder - the previous data is not deleted.": ("用于 FileBrowser Quantum 配置、SQLite 数据库和缓存的共享文件夹（挂载在 /home/filebrowser/data）。请置于数据阵列上。后续若更换文件夹将从新文件夹中的空白配置开始——旧数据不会被删除。", "用於 FileBrowser Quantum 設定、SQLite 資料庫與快取的共享資料夾（掛載在 /home/filebrowser/data）。請置於資料陣列上。後續若更換資料夾將從新資料夾中的空白設定開始——舊資料不會被刪除。"),
    "User ID (PUID)": ("用户 ID (PUID)", "使用者 ID (PUID)"),
    "User ID for the container process and files written to shared folders. PUID 0 runs the container as root, which is the only option for folders that are owned by root and not readable for others (e.g. a root:root 700 volume root); PUID/PGID 1000:100 is the default for shared folders created by OMV. POSIX ACLs can grant access as well - the permission check below shows what the configured ID can read right now.": ("容器进程及写入共享文件夹文件所使用的用户 ID。PUID 0 表示以 root 运行容器——当文件夹属于 root 且其他人不可读时（例如 root:root 700 的卷根目录）这是唯一可行方案；OMV 创建的共享文件夹默认使用 PUID/PGID 1000:100。POSIX ACL 也可授予访问权——下方权限检查会显示当前配置的 ID 现在能读到什么。", "容器程序及寫入共享資料夾檔案所使用的使用者 ID。PUID 0 表示以 root 執行容器——當資料夾屬於 root 且其他人不可讀時（例如 root:root 700 的卷根目錄）這是唯一可行方案；OMV 建立的共享資料夾預設使用 PUID/PGID 1000:100。POSIX ACL 也可授予存取權——下方權限檢查會顯示目前設定的 ID 現在能讀到什麼。"),
    "Group ID (PGID)": ("组 ID (PGID)", "群組 ID (PGID)"),
    "Group ID for the container process and files written to shared folders. Use 100 (the OMV 'users' group) together with PUID 1000, or 0 together with PUID 0 for root semantics.": ("容器进程及写入共享文件夹文件所使用的组 ID。与 PUID 1000 搭配时使用 100（OMV 的 users 组），需要 root 语义时与 PUID 0 搭配使用 0。", "容器程序及寫入共享資料夾檔案所使用的群組 ID。與 PUID 1000 搭配時使用 100（OMV 的 users 群組），需要 root 語意時與 PUID 0 搭配使用 0。"),
    "Permission check": ("权限检查", "權限檢查"),
    "Read access of the configured PUID/PGID to the data folder and the storage sources, evaluated when the page loads. Advisory only - POSIX ACLs also grant access and files created later can have different owners, so a 'NOT readable' entry is not a hard error. Without read access FileBrowser Quantum returns HTTP 403 in the web interface.": ("页面加载时评估所配置 PUID/PGID 对数据文件夹和存储源的读权限。仅为提示 - POSIX ACL 同样可授予访问权，且后续新建的文件可能属于其他用户，因此出现「不可读」条目并非硬性错误。若无读权限，FileBrowser Quantum 会在 Web 界面返回 HTTP 403。", "頁面載入時評估所設定 PUID/PGID 對資料資料夾與儲存來源的讀取權限。僅為提示 - POSIX ACL 同樣可授予存取權，且後續新建的檔案可能屬於其他使用者，因此出現「不可讀」項目並非硬性錯誤。若無讀取權限，FileBrowser Quantum 會在 Web 介面回傳 HTTP 403。"),
    "Network": ("网络", "網路"),
    "Base URL": ("基准 URL (Base URL)", "基準 URL (Base URL)"),
    "Optional URL path prefix when running behind a reverse proxy subpath (e.g. /files), written to http.baseURL of the upstream config.yaml. Leave empty to serve at the root - clearing the field removes a previously configured prefix.": ("在反向代理子路径（例如 /files）后运行时可选的 URL 路径前缀，写入上游 config.yaml 的 http.baseURL。根路径访问请留空——清空该字段会移除之前配置的前缀。", "在反向代理子路徑（例如 /files）後執行時可選的 URL 路徑前綴，寫入上游 config.yaml 的 http.baseURL。根路徑存取請留空——清空該欄位會移除先前設定的前綴。"),
    "Application URL": ("应用 URL", "應用 URL"),
    "Optional public URL used for external links (e.g. https://files.example.com), written to http.externalUrl of the upstream config.yaml. Upstream appends the base URL on its own, so a sub path must not be repeated here.": ("用于外部链接的可选公网 URL（例如 https://files.example.com），写入上游 config.yaml 的 http.externalUrl。上游会自行追加基准 URL，因此此处不要重复子路径。", "用於外部連結的可選公網 URL（例如 https://files.example.com），寫入上游 config.yaml 的 http.externalUrl。上游會自行附加基準 URL，因此此處不要重複子路徑。"),
    "Must be empty or an absolute http(s) URL without whitespace (e.g. https://files.example.com).": ("必须留空或为不含空白的绝对 http(s) URL（例如 https://files.example.com）。", "必須留空或為不含空白的絕對 http(s) URL（例如 https://files.example.com）。"),
    "Trust proxy headers": ("信任代理标头", "信任代理標頭"),
    "Trust X-Forwarded-* headers from reverse proxies for client IP and protocol detection (http.trustProxyHeaders).": ("信任来自反向代理的 X-Forwarded-* 标头以进行客户端 IP 和协议探测（http.trustProxyHeaders）。", "信任來自反向代理的 X-Forwarded-* 標頭以進行用戶端 IP 與通訊協定探測（http.trustProxyHeaders）。"),
    "Authentication method": ("身份验证方式", "身分驗證方式"),
    "Authentication method written to auth.methods of the upstream config.yaml - password (internal user database), noauth (disable authentication completely) or proxy (HTTP header authentication). Only these keys are read by upstream 2.x - no environment variable is involved.": ("身份验证方式，写入上游 config.yaml 的 auth.methods - password（标准内置用户数据库）、noauth（完全禁用身份验证）或 proxy（HTTP 标头代理认证）。上游 2.x 只读取这些配置键，不涉及任何环境变量。", "身分驗證方式，寫入上游 config.yaml 的 auth.methods - password（標準內建使用者資料庫）、noauth（完全停用身分驗證）或 proxy（HTTP 標頭代理認證）。上游 2.x 只讀取這些設定鍵，不涉及任何環境變數。"),
    "Password (internal user database)": ("密码验证（内置用户数据库）", "密碼驗證（內建使用者資料庫）"),
    "No authentication (public access)": ("无需验证（公开访问）", "無需驗證（公開存取）"),
    "Proxy header authentication": ("代理标头验证", "代理標頭驗證"),
    "Proxy authentication header": ("代理认证标头", "代理認證標頭"),
    "Header the reverse proxy sets to the user name (auth.methods.proxy.header). Only used with proxy authentication. Make sure the service is reachable exclusively through the proxy, otherwise anybody can set the header himself.": ("反向代理用于传递用户名的标头（auth.methods.proxy.header），仅在 proxy 方式下使用。请确保服务只能经代理访问，否则任何人都能自行设置该标头。", "反向代理用於傳遞使用者名稱的標頭（auth.methods.proxy.header），僅在 proxy 方式下使用。請確保服務只能經代理存取，否則任何人都能自行設定該標頭。"),
    "Must be a valid HTTP header name (letters, digits, '-', '_').": ("必须是合法的 HTTP 标头名称（字母、数字、'-'、'_'）。", "必須是合法的 HTTP 標頭名稱（字母、數字、'-'、'_'）。"),
    "Security and advanced": ("安全与高级选项", "安全與進階選項"),
    "Auto-mask protected directories": ("自动遮蔽敏感保护目录", "自動遮蔽敏感保護目錄"),
    "Mount an empty, read-only 1 MiB tmpfs over the RustFS object store, the Compose shared folder and the Docker data root whenever they fall inside a mounted storage source, preventing recursion and credential leaks. When disabled, storage sources that contain one of these directories are rejected instead of being exposed.": ("当 RustFS 对象存储、Compose 共享文件夹和 Docker data-root 位于挂载的存储源内部时，自动挂载空的只读 1 MiB tmpfs 进行遮蔽，防止循环递归和凭据泄露。若关闭该选项，包含上述目录的存储源将被拒绝，而不会被暴露。", "當 RustFS 物件儲存、Compose 共享資料夾與 Docker data-root 位於掛載的儲存來源內部時，自動掛載空的唯讀 1 MiB tmpfs 進行遮蔽，防止循環遞迴與憑證洩漏。若關閉該選項，包含上述目錄的儲存來源將被拒絕，而不會被公開。"),
    "Masked path count": ("遮蔽路径数", "遮蔽路徑數"),
    "Number of tmpfs masks currently configured in the rendered stack file.": ("当前渲染的栈文件中已配置的 tmpfs 遮蔽项数量。", "目前渲染的堆疊檔案中已設定的 tmpfs 遮蔽項數量。"),
    "Masked container paths": ("遮蔽的容器路径", "遮蔽的容器路徑"),
    "The container paths currently masked in the rendered stack file. Empty when nothing needs masking.": ("当前在渲染栈文件中遮蔽的容器路径。无需遮蔽时为空。", "目前在渲染堆疊檔案中遮蔽的容器路徑。無需遮蔽時為空。"),
    "Extra environment options": ("额外环境变量选项", "額外環境變數選項"),
    "Additional environment variables passed to the container, one per line in KEY=VALUE format. Lines starting with # are ignored.": ("传递给容器的附加环境变量，每行一个 KEY=VALUE 格式。以 # 开头的行将被忽略。", "傳遞給容器的附加環境變數，每行一個 KEY=VALUE 格式。以 # 開頭的行將被忽略。"),
    "Refresh status": ("刷新状态", "重新整理狀態"),
    "Start": ("启动", "啟動"),
    "FileBrowser Quantum deploy/start output": ("FileBrowser Quantum 部署/启动输出", "FileBrowser Quantum 部署/啟動輸出"),
    "Stop": ("停止", "停止"),
    "Do you really want to stop the FileBrowser Quantum container? The container is removed; the data and source folders and the settings are kept.": ("确定要停止 FileBrowser Quantum 容器吗？容器会被移除；数据和存储源文件夹以及设置都会保留。", "確定要停止 FileBrowser Quantum 容器嗎？容器會被移除；資料與儲存來源資料夾以及設定都會保留。"),
    "FileBrowser Quantum stop output": ("FileBrowser Quantum 停止输出", "FileBrowser Quantum 停止輸出"),
    "Restart": ("重启", "重啟"),
    "Do you really want to restart the FileBrowser Quantum container?": ("确定要重启 FileBrowser Quantum 容器吗？", "確定要重啟 FileBrowser Quantum 容器嗎？"),
    "FileBrowser Quantum restart output": ("FileBrowser Quantum 重启输出", "FileBrowser Quantum 重啟輸出"),
    "Upgrade": ("升级", "升級"),
    "Do you really want to upgrade FileBrowser Quantum? A pinned image tag is advanced to the latest upstream release, the stack is re-rendered, the new image is pulled and the container is recreated. Read the upstream release notes before a major upgrade.": ("确定要升级 FileBrowser Quantum 吗？固定的镜像标签会先推进到上游最新发布版本，重新渲染栈文件，拉取新镜像并重建容器。大版本升级前请阅读上游发布说明。", "確定要升級 FileBrowser Quantum 嗎？固定的映像標籤會先推進到上游最新發布版本，重新渲染堆疊檔案，拉取新映像並重建容器。大版本升級前請閱讀上游發布說明。"),
    "FileBrowser Quantum upgrade output": ("FileBrowser Quantum 升级输出", "FileBrowser Quantum 升級輸出"),
    "Check for latest version": ("检查最新版本", "檢查最新版本"),
    "Queried the latest upstream version. Save and apply, or use the Upgrade button.": ("已查询上游最新版本。保存并应用，或使用「升级」按钮。", "已查詢上游最新版本。儲存並套用，或使用「升級」按鈕。"),
    "View logs": ("查看日志", "檢視日誌"),
    "FileBrowser Quantum logs": ("FileBrowser Quantum 日志", "FileBrowser Quantum 日誌"),
    "View compose file": ("查看 compose 文件", "檢視 compose 檔案"),
    "Rendered docker-compose.yml": ("渲染后的 docker-compose.yml", "渲染後的 docker-compose.yml"),
    "Open FileBrowser Quantum": ("打开 FileBrowser Quantum", "開啟 FileBrowser Quantum"),
    "Shared folder": ("共享文件夹", "共享資料夾"),
    "Container path": ("容器路径", "容器路徑"),
    "Display name": ("显示名称", "顯示名稱"),
    "Read-only": ("只读", "唯讀"),
    "Default enabled": ("默认启用", "預設啟用"),
    "A storage source mounts a shared folder into the FileBrowser Quantum container so its files can be browsed and managed in the web interface. You can expose multiple shared folders into the container.": ("存储源将共享文件夹挂载到 FileBrowser Quantum 容器中，以便在 Web 界面中浏览和管理其文件。你可以向容器中暴露多个共享文件夹。", "儲存來源將共享資料夾掛載到 FileBrowser Quantum 容器中，以便在 Web 介面中瀏覽與管理其檔案。你可以向容器中公開多個共享資料夾。"),
    "Do not select the Compose plugin shared folder itself, a system directory, the RustFS object store or the Docker data root. Such folders are rejected or masked automatically so FileBrowser Quantum cannot read their credentials or its own state.": ("请勿选择 Compose 插件共享文件夹本身、系统目录、RustFS 对象存储或 Docker data-root。此类文件夹会被拒绝或自动遮蔽，使 FileBrowser Quantum 无法读取其凭据或自身状态。", "請勿選取 Compose 外掛共享資料夾本身、系統目錄、RustFS 物件儲存或 Docker data-root。此類資料夾會被拒絕或自動遮蔽，使 FileBrowser Quantum 無法讀取其憑證或自身狀態。"),
    "Shared folder exposed to FileBrowser Quantum. The container path below is filled in from it and stays editable.": ("暴露给 FileBrowser Quantum 的共享文件夹。下方的容器路径会据此自动填充，且仍可手动修改。", "公開給 FileBrowser Quantum 的共享資料夾。下方的容器路徑會據此自動填入，且仍可手動修改。"),
    "Absolute path inside the container. It is filled in from the selected shared folder as '/srv/<shared folder name>' and can still be changed; leave it empty to have it generated again when saving.": ("容器内的绝对路径。选定共享文件夹后会自动填充为 '/srv/<共享文件夹名>'，之后仍可手动修改；留空则在保存时重新自动生成。", "容器內的絕對路徑。選定共享資料夾後會自動填入 '/srv/<共享資料夾名>'，之後仍可手動修改；留空則在儲存時重新自動產生。"),
    "Must be an absolute path without a colon or whitespace (e.g. /srv/photos).": ("必须是不含冒号或空白的绝对路径（例如 /srv/photos）。", "必須是不含冒號或空白的絕對路徑（例如 /srv/photos）。"),
    "Optional display name for this storage source. Leave empty to use the shared folder name.": ("该存储源的可选显示名称。留空则使用共享文件夹名称。", "該儲存來源的可選顯示名稱。留空則使用共享資料夾名稱。"),
    "Mount the folder read-only (':ro'), preventing changes from within FileBrowser Quantum.": ("以只读方式挂载文件夹 (':ro')，防止在 FileBrowser Quantum 中进行更改。", "以唯讀方式掛載資料夾 (':ro')，防止在 FileBrowser Quantum 中進行變更。"),
    "Automatically enable this storage source for newly created users.": ("自动为新创建的用户启用此存储源。", "自動為新建立的使用者啟用此儲存來源。"),
    "Settings": ("设置", "設定"),
    "Sources": ("存储源", "儲存來源"),
    "FileBrowser Quantum": ("FileBrowser Quantum", "FileBrowser Quantum"),
    "Updated FileBrowser Quantum settings.": ("FileBrowser Quantum 设置已更新。", "FileBrowser Quantum 設定已更新。"),
    "Add source": ("添加存储源", "新增儲存來源"),
    "Added source.": ("已添加存储源。", "已新增儲存來源。"),
    "Edit source": ("编辑存储源", "編輯儲存來源"),
    "Updated source.": ("存储源已更新。", "儲存來源已更新。"),
}


def yaml_msgids_in_order() -> list[str]:
    ids, seen = [], set()
    for p in sorted(WB.rglob("*.yaml")):
        for m in GET.finditer(p.read_text(encoding="utf-8")):
            s = m.group(2)
            if s not in seen:
                seen.add(s)
                ids.append(s)
    return ids


def esc(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def entry(msgid: str, msgstr: str = "") -> str:
    return f'msgid "{esc(msgid)}"\nmsgstr "{esc(msgstr)}"\n'


def header(lang_team: str, lang: str | None) -> str:
    h = (
        f"# Translation template for {PKG}.\n"
        if lang is None
        else f"# Copyright (C) 2026 ${{GITHUB_USER}}\n"
        f"# This file is distributed under the same license as the {PKG} package.\n#\n"
    )
    h += 'msgid ""\nmsgstr ""\n'
    rows = [
        f"Project-Id-Version: {PKG} {VER}\\n",
        "Report-Msgid-Bugs-To: \\n",
        f"POT-Creation-Date: {DATE}\\n",
        f"PO-Revision-Date: {DATE}\\n",
        "Last-Translator: ${GITHUB_USER}\\n",
        f"Language-Team: {lang_team}\\n" if lang_team else "Language-Team: \\n",
        f"Language: {lang}\\n" if lang else "Language: \\n",
        "MIME-Version: 1.0\\n",
        "Content-Type: text/plain; charset=UTF-8\\n",
        "Content-Transfer-Encoding: 8bit\\n",
    ]
    return h + "".join(f'"{r}"\n' for r in rows) + "\n"


def main() -> int:
    manifest_ids = yaml_msgids_in_order()

    all_ids: list[str] = []
    seen = set()
    for mid in RUNTIME + manifest_ids:
        if mid not in seen:
            seen.add(mid)
            all_ids.append(mid)

    for mid in sorted(T.keys()):
        if mid not in seen:
            seen.add(mid)
            all_ids.append(mid)

    # Check for missing translations
    missing = [m for m in all_ids if m not in T]
    if missing:
        print("Missing translations for:", file=sys.stderr)
        for m in missing:
            print(f"  {m!r}", file=sys.stderr)
        return 1

    # Write POT
    pot_content = header("", None) + "\n".join(entry(mid) for mid in all_ids) + "\n"
    (LOC / f"{PKG}.pot").write_bytes(pot_content.encode("utf-8"))

    # Write zh_CN PO
    zh_cn_content = (
        header("Chinese (Simplified) <openmediavault-plugins@googlegroups.com>", "zh_CN")
        + "\n".join(entry(mid, T[mid][0]) for mid in all_ids)
        + "\n"
    )
    zh_cn_path = LOC / "zh_CN" / f"{PKG}.po"
    zh_cn_path.parent.mkdir(parents=True, exist_ok=True)
    zh_cn_path.write_bytes(zh_cn_content.encode("utf-8"))

    # Write zh_TW PO
    zh_tw_content = (
        header("Chinese (Traditional) <openmediavault-plugins@googlegroups.com>", "zh_TW")
        + "\n".join(entry(mid, T[mid][1]) for mid in all_ids)
        + "\n"
    )
    zh_tw_path = LOC / "zh_TW" / f"{PKG}.po"
    zh_tw_path.parent.mkdir(parents=True, exist_ok=True)
    zh_tw_path.write_bytes(zh_tw_content.encode("utf-8"))

    print(
        f"Built locale catalogs for {PKG} {VER}: {len(all_ids)} messages translated (zh_CN, zh_TW)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
