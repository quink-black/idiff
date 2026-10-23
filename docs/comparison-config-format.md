# 对比配置 JSON 格式指引

本文面向需要在自己的项目里生成 idiff 可加载 JSON 的开发者。它说明
idiff 接受哪些 JSON 形状、推荐的写法，以及 idiff 如何把 JSON 里的
URL 落到本地文件——尤其是"同一份 JSON 在有网机器和离线机器上都能
用"该如何组织目录。

解析实现见 `src/core/comparison_config.cpp`，本地查找与下载见
`src/core/url_cache.cpp`，两者由 `src/domain/comparison_config_service.cpp`
串起来。本文描述的是这三处当前的行为。

## 1. 加载入口

| 入口 | 用法 |
|------|------|
| 图形界面 | `File > Open Comparison Config...`，或把 JSON 拖进窗口 |
| JSON-RPC | `comparison_config.load` `{"path": "/data/batch/compare.json"}` |
| 命令行 | `idiffctl.py call comparison_config.load '{"path": "/data/batch/compare.json"}'` |
| MCP | 工具 `load_comparison_config` |

加载后 idiff 立即显示第 0 组；组之间用
`comparison_config.switch_group` `{"group_index": N}` 切换。

## 2. 推荐格式

外部项目按这个形状生成即可，它是所有宽容解析分支的交集：

```json
{
  "groups": [
    {
      "name": "case-0001",
      "description": "kid-pisa x4",
      "items": [
        { "url": "https://cos.example.com/sr/case-0001/gt.png",  "title": "原图" },
        { "url": "https://cos.example.com/sr/case-0001/a.png",   "title": "model-a" },
        { "url": "https://cos.example.com/sr/case-0001/b.png",   "title": "model-b",
          "description": "step 300k" }
      ]
    },
    {
      "name": "case-0002",
      "items": [
        { "url": "https://cos.example.com/sr/case-0002/gt.png",  "title": "原图" },
        { "url": "https://cos.example.com/sr/case-0002/a.png",   "title": "model-a" }
      ]
    }
  ]
}
```

字段含义：

| 层级 | 字段 | 说明 |
|------|------|------|
| 组 | `name` | 组名，显示在分组列表里；缺失时 idiff 自动命名为 `Group N` |
| 组 | `description` | 可选，鼠标悬停提示 |
| 组 | `items` | 该组要一起对比的图片，按顺序加载 |
| 项 | `url` | 唯一必需字段。推荐写 http(s) 绝对 URL，见第 4 节 |
| 项 | `title` | 图片显示名，覆盖默认的文件名标签。名为 `原图` 的项会被当作该组的基准图 |
| 项 | `description` | 可选 |

键名不区分大小写，`items` 也可以写成 `images`，`title` 也可以写成
`name` 等，见第 3 节。写了 `title` 的项按原样显示，idiff 不会给它加
任何后缀：同名 `title` 会原样并存，在图列表里分不出来。idiff 只对没
写 `title` 的项做区分——这类项的标签由文件路径推导，重名时自动补上
更长的路径前缀。因此生成端必须让一组内的 `title` 互不相同。

## 3. idiff 还接受哪些形状

解析器刻意宽松，以下写法都能加载。外部项目不必迁就这些变体，只是
遇到别处生成的 JSON 时不至于失败。

- 顶层是数组：元素若带 `images/items/...` 键或本身是数组，就当作组
  列表；否则所有元素合成一个隐式组。
- 顶层对象没有组列表键：整个对象当成一个组。若它也没有 `items` 键，
  idiff 会扫描对象里所有以 `http://`、`https://`、`file://` 开头的字符串
  值，用它的键作为 `title`。手工写的
  `{"orig": "https://.../a.png", "srA": "https://.../b.png"}` 因此可用。
- 组列表的键可以是 `comparisonGroups`、`comparison_groups`、`groups`、
  `cases`、`sets`、`collections` 之一。
- 组内图片列表的键可以是 `images`、`items`、`pictures`、`files`、
  `urls`、`assets`、`photos` 之一；组对象本身是数组时，其元素即图片项。
- 图片项的 URL 可以写在 `url`、`src`、`href`、`link`、`path`、`image`
  下；标题可以写在 `title`、`name`、`label`、`caption`、`tag` 下；描述
  可以写在 `description`、`desc`、`detail`、`note`、`comment` 下。
- 图片项可以就是一个字符串，此时它整体作为 URL。
- 无法解析出任何 URL 的项会被静默跳过；一个组都没解析出来时加载失败，
  错误文字只进状态栏和日志。通过 JSON-RPC 或 idiffctl 加载时，这个失败
  不会变成 RPC 错误：调用照常成功，`idiffctl.py` 退出码为 0，返回的
  `entries`/`groups` 仍是上一次会话的值（进程刚启动时为 0）。所以
  `groups` 等于 0 只在刚启动、还没成功加载过配置的实例上能作为失败
  判据；复用一个已经加载过配置的实例时，失败的加载同样返回大于 0 的
  `groups`，只看这个值的脚本会把旧会话当成新结果继续比对。可靠的做法
  是加载后再用 `state.get` 核对当前条目的 `label`/`path` 是否等于新生成
  JSON 第 0 组的内容——生成端知道预期值，只看 `groups` 不够。

## 4. URL 如何落到本地文件

这是跨机器复用同一份 JSON 的关键，也是生成端需要配合的部分。

### 4.1 解析顺序

对 JSON 里的每一个 URL，idiff 按下面的顺序处理：

1. **本地优先。** 去掉 scheme、host、查询串和 fragment，把剩下的路径
   按 `/` 分段，依次拼到"本地基目录"下；若得到的文件是存在的普通文
   件，就直接用它，既不联网也不写缓存目录。本地基目录是 JSON 文件
   所在目录，以及它向上最多 3 层父目录（走到文件系统根就停止）；按由
   近及远的顺序取第一个命中。
2. **否则下载。** 用系统的 `curl` 下载到缓存目录。缓存根目录就是
   JSON 文件所在的目录（不是 `~/Downloads`，`~/.idiff.config` 里的
   `cache_root` 对配置加载不生效）。缓存内的路径是
   `<缓存根>/<host>/<URL 路径>`，但所有 URL 共有的 host 与最长公共
   路径前缀会被裁掉，让目录尽量浅；裁剪不会让两个 URL 撞到同一个
   文件上。
3. 下载失败的图片从该组剔除，状态栏显示 `N download failures: <最后一
   条错误>`。RPC 的 `comparison_config.switch_group` 只返回 `entries` 与
   `current_group`，不包含失败数或错误文字；自动化脚本要用 `entries`
   小于该组图片数来判断有图片被丢弃。

举例：URL 为 `https://cos.example.com/sr/src/g1/a.png`，路径分段为
`sr/src/g1/a.png`。若 JSON 位于 `/data/batch/sr/compare.json`，基目录
依次尝试 `/data/batch/sr`、`/data/batch`、`/data`、`/`；其中
`/data/batch/sr/src/g1/a.png` 若存在即命中。JSON 距文件系统根不足 3 层
时，最后一层基目录就是 `/`，此时 URL 路径会被拼成一个绝对路径。

### 4.2 跨机器复用：一份 JSON，两种取图方式

生成端只要遵守两条约定，同一份 JSON 就能在两种环境里都工作：

1. JSON 里写 **http(s) 绝对 URL**，作为图片的权威来源；
2. 需要离线使用时，把图片按 **URL 的路径结构**展开在某个目录树下，
   这个目录树要能从 JSON 所在目录（或其不超过 3 层的祖先）开始。

于是：有网的机器按 URL 下载，文件落在 JSON 所在的目录下；另一台机器
上如果已经有一份图片，按同样的路径结构摆放即可零网络加载。JSON 里不
需要出现任何本机绝对路径，绝对路径不一致的问题也就不存在。

有一点必须分清：**下载产生的缓存目录不能当作本地镜像复用。** 缓存里
的路径会裁掉所有 URL 共有的 host 与最长公共路径前缀，而本地查找用的
是完整的 URL 路径，两者不是同一棵树；裁剪量还随整个 JSON 的 URL 集合
变化，增删一组就会改变落点。以第 7 节里的两条 URL 为例，下载落到
`<JSON 目录>/a.png`，而本地镜像落在 `<JSON 目录>/src/g1/a.png`：第 7 节
的 JSON 在 `/tmp/batch/sr`，完整 URL 路径 `sr/src/g1/a.png` 是拼在它的
父目录 `/tmp/batch` 上的。把同一条路径拼在 JSON 目录本身同样命中（那时
多出一层 `sr/`），铺在哪一层取决于把哪一层选作基目录。镜像目录必须由
生成端按完整 URL 路径铺设，或者由打包脚本按同一规则展开。

推荐布局一——JSON 放在镜像树的根：

```
/data/batch/                       <- 镜像根，URL 路径 /sr/... 从这里开始
├── compare.json
└── sr/
    └── src/
        ├── g1/a.png               <- https://host/sr/src/g1/a.png
        └── g2/a.png
```

此时基目录 `/data/batch` 加上 URL 路径 `sr/src/g1/a.png` 即命中。

推荐布局二——JSON 放在镜像树内部（多一层更常见）：

```
/data/batch/
└── sr/
    ├── compare.json               <- JSON 在这里
    └── src/
        ├── g1/a.png               <- 基目录上溯到 /data/batch 后命中
        └── g2/a.png
```

判定条件可以写成一句：存在基目录 `B`（JSON 目录、父目录、祖父目录、
曾祖父目录之一），使得 `B` 拼接 URL 的路径等于图片的真实路径。

因为本地匹配只看路径、不看 host，换 CDN 域名或内网域名不会影响已经
摆好的本地镜像；host 只影响下载时缓存目录的第一层。

### 4.3 注意事项

- **查询串被丢弃。** `?sign=...` 之类的签名参数不影响本地查找，也不
  进入文件名；反过来，两个仅查询串不同的 URL 会映射到同一个本地文
  件，不要用查询串区分不同的图片。
- **文件名会被规整。** URL 路径按 `/` 分段后，每段先做百分号解码，再
  把 `<>:"\|?*` 和控制字符替换成 `_`（`.`、`-`、`_` 保留）；整段恰好
  是 `.` 或 `..` 时整段换成 `_`。替换只认整段，所以段内嵌的 `..` 不受
  约束：`..%2Fx` 解码成 `../x`，整段不等于 `..`，原样拼进路径就能读到
  基目录之外的文件。生成端应使用简单的 ASCII 文件名，并保留真实扩展
  名，扩展名参与图片格式的识别；不要使用百分号编码的斜杠（`%2F`）。
- **缓存命中只看文件存在且非空。** 远端图片内容变了但 URL 没变时，
  idiff 不会重新下载；需要更新就删掉对应的缓存文件。
- **不要依赖 `file://` 或本地绝对路径。** 它们会走同一套解析流程，但
  只有在图片恰好能通过"基目录拼接完整路径"还原时才命中——通常要求
  JSON 就在这棵目录树里，换一台机器基本失效。需要离线加载就用
  http(s) URL 加本地镜像。

## 5. 加载后的行为

- 同一时刻只有当前组的图片驻留内存，切换组会释放上一组。
- 切换组时，前后各 3 组的图片会在后台预取，切过去时通常已经就绪。
- 首次加载成功后视口进入 Overlay 模式，并自动选中前两张。
- 组内 `title` 为 `原图` 的项被当作该组的基准图，切换组时占住一个对
  比槽位，适合放 ground truth。

## 6. 生成端检查清单

- [ ] 顶层用 `groups`，组内用 `items`，每项至少带 `url`。
- [ ] `url` 是 http(s) 绝对 URL，路径结构稳定（它是本地镜像的依据）。
- [ ] 每个组有唯一的 `name`；组内 `title` 简短且互不相同，基准图用
      `原图`。
- [ ] 需要离线使用时，图片按 URL 路径展开在 JSON 目录或其不超过 3 层
      的祖先目录下。
- [ ] 文件名使用 ASCII 字符并保留真实扩展名。
- [ ] 不用查询串区分图片，不写本机绝对路径。
- [ ] 通过 JSON-RPC 或 idiffctl 加载后核对结果：加载失败不会产生 RPC
      错误，返回值可能仍来自上一次会话。刚启动的实例可以只看返回的
      `groups` 不为 0；复用实例时还要用 `state.get` 核对当前条目的
      `label`/`path` 是否等于新生成 JSON 第 0 组的内容。

## 7. 最小示例

`compare.json`：

```json
{
  "groups": [
    {
      "name": "g1",
      "items": [
        { "url": "https://host.example.com/sr/src/g1/a.png", "title": "原图" },
        { "url": "https://host.example.com/sr/src/g1/b.png", "title": "model-a" }
      ]
    }
  ]
}
```

对应的本地镜像（离线可用；由生成端或打包脚本按完整 URL 路径铺设，不是
idiff 自己下载出来的目录）：

```
/tmp/batch/sr/compare.json
/tmp/batch/sr/src/g1/a.png
/tmp/batch/sr/src/g1/b.png
```

加载：

```bash
idiffctl.py call comparison_config.load '{"path": "/tmp/batch/sr/compare.json"}'
```
