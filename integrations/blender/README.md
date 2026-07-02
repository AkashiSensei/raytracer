# Blender 渲染器接入说明

本目录提供 Blender RenderEngine 插件，让 Blender 可以直接使用本项目的光线追踪器渲染当前场景。

这个入口不会替代或破坏原有命令行渲染路径；它只是新增了一条 Blender 路径。Blender 插件会读取当前场景的 evaluated mesh、活动摄像机、灯光和基础材质，然后交给本项目的本地 bridge 或远程 HTTP server 渲染。

## 版本

当前按 Blender 5.1.2 进行适配和验证。其他 5.x 版本大概率可用，但界面菜单名称可能略有差异。

## 构建

在项目根目录执行：

```bash
./build.sh
```

构建完成后，根目录会生成：

```text
./raytracer
./raytracer_blender_bridge
./raytracer_server
```

其中 `raytracer` 是原有命令行渲染器，`raytracer_blender_bridge` 是 Blender 本地模式使用的桥接程序，`raytracer_server` 是远程模式使用的 HTTP 渲染服务。

## 安装插件

在 Blender 中文界面中：

1. 打开 `编辑 > 偏好设置`。
2. 进入 `插件`。
3. 点击右上角的下拉菜单，选择 `从磁盘安装...`。
4. 选择 `integrations/blender/raytracer_addon.py`。
5. 启用 `Raytracer Renderer`。

启用后，在 `渲染属性` 中把 `渲染引擎` 切换为 `Raytracer`。

## 本地模式渲染

在 `渲染属性 > Raytracer` 面板中确认：

- `后端` 选择 `本地`。
- `Bridge` 指向项目根目录里的 `raytracer_blender_bridge`。
- 根据需要调整 `采样数`、`最大深度`、`线程数`、`仅直接光照`、`灯光强度倍率`、`背景` 和 `环境光`。

然后按 `F12` 或使用 Blender 的渲染菜单即可渲染当前场景。

如果插件是直接从仓库中的 `integrations/blender/raytracer_addon.py` 安装的，通常会自动找到根目录的 bridge。若找不到，请手动把 `Bridge` 设置为绝对路径，例如：

```text
/Users/your-name/develop/raytracer/raytracer_blender_bridge
```

## 远程模式渲染

远程模式需要先在渲染机器上启动 `raytracer_server`。启动参数 `--default-threads` 表示默认线程数：只有当客户端请求没有指定线程数时，server 才会使用这个值。`--max-request-threads` 表示 server 接受的单次请求最大线程数；如果 Blender 请求更高的线程数，server 会按这个上限渲染。例如在服务器上执行：

```bash
./raytracer_server --host 0.0.0.0 --port 8080 --default-threads 16 --max-request-threads 16
```

如果只在同一台机器上测试，也可以监听本地地址：

```bash
./raytracer_server --host 127.0.0.1 --port 8080 --default-threads 8 --max-request-threads 8
```

然后在 Blender 的 `渲染属性 > Raytracer` 面板中：

- `后端` 选择 `远程`。
- `远程地址` 填写服务器地址，例如 `http://192.168.1.10:8080`。
- 根据需要调整 `采样数`、`最大深度`、`线程数` 和 `仅直接光照`。这里的 `线程数` 是本次渲染请求使用的线程数；如果客户端不传，server 才会使用启动时的 `--default-threads`；如果超过 server 的 `--max-request-threads`，server 会按最大值执行。

插件会把当前 Blender 场景导出为内部 JSON 包，通过 `POST /jobs` 创建远程渲染任务；随后轮询任务进度和渐进预览，渲染完成后下载 RGBA32F 图像并显示。如果在 Blender 中取消渲染，插件会向 server 发送取消请求，server 会尽快停止对应任务。

也可以用命令行快速检查服务是否可用：

```bash
curl http://127.0.0.1:8080/health
```

远程渲染接口：

```text
POST /jobs?threads=8&direct_only=0&render_schedule=sample_passes&partial_update_interval=1
Content-Type: application/json

<Blender 插件导出的内部场景 JSON>
```

成功时返回：

```json
{"job_id":"..."}
```

随后可查询和控制任务：

```text
GET  /jobs/<job_id>/progress
GET  /jobs/<job_id>/partial
GET  /jobs/<job_id>/result
POST /jobs/<job_id>/cancel
```

`render_schedule` 可选 `rows` 或 `sample_passes`。`sample_passes` 的采样批次由后端固定选择：本地 bridge 为 `16`，远程 server 为 `64`。`partial_update_interval` 在 `rows` 下表示预览行批次间隔，在 `sample_passes` 下表示 sample 间隔；旧的 `partial_update_rows` 仍作为兼容别名。`/progress` 会返回任务状态、归一化进度、`partial_seq`、当前调度的 samples/rows/pixels 计数，以及 elapsed/remaining 秒数。当 `partial_seq` 变化时，`/partial` 返回当前最新的 `application/octet-stream` 预览图，内容同样是本项目的 `RTRGBAF1` RGBA32F 二进制图像；还没有预览时返回 `202 Accepted`。`/result` 在任务完成后返回最终图像。`POST /render` 作为同步调试接口仍保留，但 Blender 插件默认使用任务式接口。

## 参数说明

| 设置 | 说明 |
|------|------|
| `后端` | `本地` 使用本机 bridge 子进程；`远程` 通过 HTTP 连接 `raytracer_server` |
| `Bridge` | 本地桥接程序 `raytracer_blender_bridge` 的路径 |
| `远程地址` | 远程 `raytracer_server` 的 HTTP 地址 |
| `远程超时` | 等待远程渲染完成的超时时间，单位为秒 |
| `采样数` | 每像素采样次数。完整路径追踪下越高越干净，但渲染越慢 |
| `最大深度` | 光线递归反弹深度上限 |
| `线程数` | 本次渲染请求让 C++ 渲染端使用的 CPU 线程数；远程模式下会随请求发给 server |
| `仅直接光照` | 只计算相机射线、直接光照、阴影和环境光，不做递归随机反弹；适合快速预览 |
| `采样调度` | 控制渲染任务顺序。`按行完成` 会先完成一批像素行，`全图累积` 会每轮给整张图增加采样，更接近 Cycles 的渐进预览 |
| `渐进预览` | 渲染期间刷新 Blender Render Result；本地 bridge 和远程 server 都支持，最终图像不受影响 |
| `灯光强度倍率` | Blender 光照导出为 raytracer 内部光照强度前应用的倍率，默认 `0.03` |
| `背景` | Blender 路径显式导出的背景。默认 `黑色`，也可选 `World 表面`、`自定义颜色` 或 raytracer 内置 `天空` |
| `环境光` | 简单全局补光开关。默认关闭；开启后按 `环境光颜色 * 环境光强度 * 灯光强度倍率` 导出，环境光强度默认 `5.0` |
| `面光源相机可见` | 控制 Blender Area Light 是否能被相机直接看到。默认关闭；面光源默认仍会在玻璃、金属等 specular/glossy 路径中可见 |
| `调试缓存` | 可选目录。填写后每次渲染会在该目录下创建一个 `raytracer_...` 子目录，保存中间 JSON、日志和结果文件 |

Blender 路径不会触发 CLI 的默认天空、默认灯光或默认环境光。插件会显式导出：

- `background`：由 `背景` 设置决定，默认黑色。
- `lighting.ambient`：由 `环境光` 设置决定，默认 `[0,0,0]`。
- `lights`：来自 Blender 场景中未禁用渲染的灯光；没有灯光时导出空数组。

因此关闭所有灯光并保持默认 `黑色` 背景、关闭 `环境光` 时，场景不会被额外补光。路径追踪模式下，如果场景中有发光材质，仍会贡献光照；这是对象材质本身的结果，不是插件额外添加。

插件会把少量 Blender 原生属性面板标记为兼容 `Raytracer`，用于编辑插件实际读取的数据或通用渲染辅助设置。例如材质 `表(曲)面`、材质 `视图显示`、World `表(曲)面`、基础 Light/Camera 面板、输出尺寸和色彩管理等。材质和 World 都会优先使用 Blender 原生 Surface 面板族；若当前 Blender 版本同时提供多个同名 Surface 变体，插件只挂载其中一个，避免重复。材质 Surface 面板族会完整保留，因为其中包含基础色、金属度、糙度、Alpha、Transmission、IOR 和自发光等插件会读取的参数。Line Art、Freestyle、体积、阴影、采样、Film、Passes 等当前不会导出的整块面板会被隐藏，避免看起来像 raytracer 已支持这些功能。

## Blender 支持清单

Blender 插件只承诺导出渲染核心已经支持或可以合理近似的内容。没有核心对应模型的 Blender 高级功能会明确忽略，而不是偷偷用错误效果代替。

### 渲染流程

| Blender 内容 | 导出内容 | 核心处理 | 备注 |
|--------------|----------|----------|------|
| `F12` 最终渲染 | 当前 evaluated scene 的内部 JSON | bridge/server 调用共享 `render_scene()` | 不接管实时 viewport path tracing |
| 本地后端 | `raytracer_blender_bridge` 子进程 | 读取 JSON，输出 `RTRGBAF1` RGBA32F | 插件把结果写回 Blender `Combined` pass |
| 远程后端 | HTTP `POST /jobs` | server 后台渲染、轮询进度/预览、下载结果 | 当前是 CPU 核心；CUDA/GPU 尚未实现 |
| 取消渲染 | Blender `test_break()` | 本地终止子进程；远程发送 `/cancel` | server 会尽快停止对应任务 |
| 进度条 | bridge stderr `PROGRESS` 或远程 progress API | Blender `update_progress()` | 不是逐采样进度，是按行/任务状态汇报 |
| 渲染状态 | bridge stderr `STATUS` 或远程 progress API | Blender `update_stats()` | `按行完成` 显示 Pixels，`全图累积` 显示 Samples，并附带 elapsed/remaining |
| 渐进预览 | bridge stderr `PARTIAL` 或远程 `/partial` | Blender `begin_result()/end_result()` 刷新 `Combined` pass | `按行完成` 为行级填充；`全图累积` 按采样批次进行全画面降噪 |

### 相机与输出

| Blender 内容 | 导出内容 | 核心处理 | 备注 |
|--------------|----------|----------|------|
| 当前活动摄像机 | `camera.lookfrom/lookat/vup/vfov/focus_dist/aperture/frame` | 生成 primary ray | 必须有 active camera；优先使用 Blender `view_frame()` 导出的显式取景平面，使视角/缩放更接近 Cycles |
| 分辨率与百分比 | `image.width/height` | 输出对应尺寸 RGBA32F | 使用 `render.resolution_percentage` 后的尺寸 |
| 采样数 | `image.samples` | 每像素 Monte Carlo 采样 | `仅直接光照` 且 1 sample 时使用像素中心采样 |
| 最大深度 | `image.max_depth` | 路径递归深度上限 | 对 `仅直接光照` 影响很小 |
| 景深 | `aperture` / `focus_dist` | 相机薄透镜模型 | 当前近似读取 Blender DOF 设置 |

### 几何与对象

| Blender 内容 | 导出内容 | 核心处理 | 备注 |
|--------------|----------|----------|------|
| Mesh 对象 | `objects[].type = "triangles"` | `TriangleMesh` + 内部 BVH 求交 | 直接导出三角形顶点，不依赖 OBJ/GLB 文件 |
| Curve / Surface / Font / Meta | evaluated mesh 后的三角形 | 同 mesh | 由 Blender evaluated depsgraph 转网格 |
| 修改器、骨骼、动画姿态 | evaluated mesh 当前帧结果 | 当作静态三角网格渲染 | 不导出骨骼或动画，只导出当前帧几何 |
| 对象世界变换 | 已烘焙到顶点坐标 | 核心收到世界坐标三角形 | 不另存 Blender object transform |
| 多材质槽 | 按材质索引拆成多个 triangle object | 每组绑定一个核心材质 | 调试 JSON 中可看到拆分后的对象 |
| UV | `uvs` | 用于核心贴图/材质采样 | 有 active UV 时导出 active UV；没有 UV 时会为 generated 坐标路径生成包围盒平面坐标 |
| Loop normal | `normals` | 命中后插值法线 | 更接近 Blender 平滑/平面着色结果 |
| 渲染禁用 | 跳过 `hide_render` 对象 | 不进入核心场景 | 对几何和灯光都生效 |

不支持：粒子系统专用属性、实例集合的高级语义、运动模糊、几何节点的非 evaluated 过程信息、置换/细分参数本身。若它们已经体现在 evaluated mesh 中，则会作为普通三角形被导出。

### 世界、背景与环境光

| Blender/Raytracer 设置 | 导出内容 | 核心处理 | 备注 |
|------------------------|----------|----------|------|
| `背景 = 黑色` | `background: {"type":"solid","color":[0,0,0]}` | miss ray 返回黑色 | Blender 路径默认值 |
| `背景 = World 表面` | `background` solid color | miss ray 返回 World Surface 的 Background 颜色乘强度 | 支持未连接贴图的 Background 节点；复杂 World 节点回退到显示颜色 |
| `背景 = 自定义颜色` | `background` solid color | miss ray 返回自定义颜色 | 插件面板颜色 |
| `背景 = 天空` | `background: {"type":"sky"}` | 核心内置蓝白天空渐变 | 这是 raytracer 内置天空，不是 Blender Sky Texture |
| `环境光` 开关 | `lighting.ambient` | 直接光照中的常量补光项 | 默认关闭，开启后为 `颜色 * 环境光强度 * 灯光强度倍率` |
| Blender World 复杂节点 | 不完整导出 | 无核心对应 | HDRI、Sky Texture、程序节点暂不采样；无法解析时回退到 World 显示颜色 |

Blender 路径会显式导出 `background`、`lighting.ambient` 和 `lights`，因此不会触发 CLI 的默认天空、默认灯光或默认弱环境光。

### 灯光

| Blender 灯光 | 导出内容 | 核心处理 | 备注 |
|--------------|----------|----------|------|
| Point | `lights[].type = "point"`，位置、颜色、`energy * 灯光强度倍率` | 点光源，平方距离衰减，发阴影射线 | 位置会影响照明 |
| Sun | `lights[].type = "directional"`，方向、颜色、`energy * 灯光强度倍率` | 方向光，无距离衰减，发阴影射线 | 只使用旋转方向，移动位置无影响 |
| Spot | 近似导出为 point，强度为 `energy * 灯光强度倍率` | 当点光源处理 | 当前不支持锥角、半影 |
| Area Square / Rectangle | `lights[].type = "rect"`，中心、方向、`u/v` 尺寸向量、颜色、强度、可见性标记 | 解析矩形面光源，支持面积采样、软阴影和 specular 命中 | 默认相机不可见、specular 可见；`面光源相机可见` 可让相机直接看到灯面 |
| Area Disk / Ellipse | `lights[].type = "disk"`，中心、方向、半径、颜色、强度、可见性标记 | 解析圆盘面光源，支持面积采样、软阴影和 specular 命中 | Ellipse 当前按等面积近似为圆盘 |
| 禁用渲染的灯 | 不导出 | 无贡献 | 读取 `hide_render` |
| 发光材质 | `emissive` 材质 | 作为自发光表面，可参与路径追踪 | 这是材质贡献，不是 Blender Light 对象 |

核心直接光照会遍历点光/方向光/解析面光源并发阴影射线；被遮挡的光源不会给当前命中点贡献直接光照。Area Light 会按面积采样形成软阴影，也可以被 specular/glossy 反射或折射路径命中。体积光、IES、点/聚光灯半径、spot cone 暂不支持。

`灯光强度倍率` 是 Blender 接入层的单位标定。渲染核心仍使用自己的内部光照单位；插件负责把 Blender 的灯光 `energy` 和插件环境光强度映射到该内部单位。默认 `0.03` 是为了让 Blender 常见点光能量在当前简化光照模型里更接近可调试范围。若希望更接近某个 EEVEE/Cycles 参考图，可以直接调这个倍率，并在 `调试缓存/scene.rt.json` 中查看最终导出的 `lights[].intensity` 和 `lighting.ambient`。

### 材质

Blender 插件会把当前材质近似转换成本项目渲染核心支持的材质类型：

| Blender Principled BSDF | 导出内容 | 核心处理 | 备注 |
|-------------------------|----------|----------|------|
| 无材质 | `pbr` 默认灰色 | PBR 材质 | `albedo=[0.8,0.8,0.8]` |
| `基础色` | `pbr.albedo` 或贴图对象 | PBR 基础色 | 支持颜色值，也支持下方列出的常见贴图节点链 |
| `金属度` | `pbr.metallic` 或贴图对象 | PBR metallic | 标量或标量贴图；最终范围夹到 `[0,1]` |
| `糙度` | `pbr.roughness` 或贴图对象 | PBR roughness | 标量或标量贴图；最终范围夹到 `[0.001,1]` |
| `自发光颜色 * 自发光强度` | `emissive.emission` | 自发光材质，可作为面光源 | 强度大于 0 时优先导出为 emissive |
| `Alpha < 0.35` | `dielectric` | 折射/反射介质 | 作为透明介质近似 |
| `透射 / Transmission Weight > 0.5` | `dielectric` | 折射/反射介质 | 作为玻璃/水等近似 |
| `折射率 (IOR)` | `dielectric.ior` | Schlick Fresnel + 折射方向 | 仅导出为 dielectric 时生效 |
| `基础色` + dielectric | `dielectric.albedo` 或贴图对象 | 有色玻璃近似 | 核心会用颜色衰减透射/反射 |

#### 贴图节点与映射关系

插件会从 Principled BSDF 的 `基础色`、`金属度`、`糙度` 输入沿连接向前解析一组常见节点，并导出为核心可采样的 texture JSON。当前支持：

| Blender 节点/连接 | 导出内容 | 核心处理 | 备注 |
|-------------------|----------|----------|------|
| Checker Texture | `{type:"checker", color1, color2, scale, coord, uv_scale, uv_offset, uv_rotation}` | 程序棋盘纹理 | 支持 Color 输出；连接 Fac 输出时按黑白系数导出 |
| Image Texture | `{type:"image", path, interpolation, extension, color_space, coord, uv_*}` | 图片贴图采样 | 支持 Repeat / Extend / Clip / Mirror，Nearest / Linear，sRGB 解码 |
| Noise Texture | `{type:"noise", scale, detail, roughness, distortion, coord, uv_*}` | 程序噪声近似 | 用确定性灰度噪声近似 Blender/Cycles 噪声，不保证完全一致 |
| ColorRamp | `{type:"color_ramp", source, stops, interpolation}` | 颜色渐变映射 | 支持 Linear / Constant / Ease |
| Math | `{type:"math", operation, a, b, clamp}` | 标量运算 | 支持常见二元运算，例如 add/subtract/multiply/divide/min/max/power/compare |
| Mix / MixRGB | `{type:"mix", factor, color1, color2}` | 线性插值 | `MixRGB` 目前只支持 Mix 混合模式 |
| Invert | `{type:"invert", factor, color}` | 颜色反相 | 可用于基础色路径 |
| Map Range | `{type:"map_range", value, from_min, from_max, to_min, to_max, clamp}` | 标量重映射 | 非 Linear 插值会记录为 unsupported 诊断 |
| RGB / Value | `{type:"solid", color}` | 常量颜色或常量标量 | Value 会导出为灰度 solid |
| Reroute | 不单独导出 | 继续追踪输入来源 | 用于整理节点图时不影响导出 |

Mapping 节点会被折叠进被连接的贴图对象：

| Blender Mapping 输入 | 导出字段 | 核心处理 |
|----------------------|----------|----------|
| `Scale X/Y` | `uv_scale` | 采样前缩放 UV |
| `Location X/Y` | `uv_offset` | 缩放/旋转后平移 UV |
| `Rotation Z` | `uv_rotation`，单位为度 | 在 UV 平面内旋转 |

Texture Coordinate 节点当前识别 `UV`、`Generated` 和 `Object`/`Generated` 类路径。`UV` 会使用 mesh 的 active UV；`Generated`/`Object` 会使用导出时按对象局部包围盒生成的近似坐标。若同一个材质组同时混用 UV 和 Generated 坐标，插件会保留 active UV，并在材质 JSON 上写入 `mixed_texture_coords` 诊断。

核心材质大致处理方式：

- `pbr`：Cook-Torrance / GGX 风格的金属-粗糙度材质；直接光照路径会使用基础色参与漫反射近似。
- `dielectric`：玻璃/水等透明介质，使用 IOR、反射/折射和 Schlick Fresnel。
- `emissive`：直接返回发光颜色，并登记为可采样发光表面。

目前不会导出 Blender 的次表面散射、涂层、边缘光泽、薄膜、体积、背面剔除、阴影模式和完整节点图；这些没有直接对应的核心材质参数。贴图节点如果没有被插件显式解析，会在材质 JSON 的 `unsupported_textures` 中记录诊断，并回退到对应输入的默认值或常量值。

### 调试缓存

| 内容 | 文件 | 用途 |
|------|------|------|
| 导出的内部场景 | `scene.rt.json` | 查看最终传给核心的相机、几何、灯光、材质 |
| 本地 bridge 命令 | `command.txt` | 直接复现本次渲染 |
| 本地日志 | `bridge.stderr.log` / `bridge.stdout.log` | 查看 primitive、lights、ambient、background 和错误 |
| 远程请求 | `remote_request.txt` | 查看 server endpoint 和线程参数 |
| 远程进度 | `remote_progress.log` | 查看 job 状态 |
| 渲染结果 | `result.rgba32f` | Blender 收到的线性 RGBA float 图像 |

仓库提供了一个小工具用于检查最新调试缓存中的贴图导出情况：

```bash
python3 scripts/inspect_texture_export.py debug --fail-on-unsupported
```

传入调试缓存根目录时，脚本会自动选择最新的 `raytracer_.../scene.rt.json`。如果希望确认场景中确实导出了 Checker Texture，可以额外加 `--expect-checker`。

## 当前限制

- 不支持完整 Blender 节点材质图；当前只解析上面列出的常见贴图节点链。
- 不支持体光源、World HDRI/Sky Texture。
- 不支持交互式 viewport path tracing、AOV/pass、多视图层差异化输出。
- 不支持运动模糊、体积散射、粒子/毛发专用渲染属性。
- 远程模式当前使用轮询查询进度，不是 WebSocket/SSE 流式推送。
- 远程任务当前存储在 server 内存中，没有持久化任务队列或鉴权。
- `raytracer_server` 当前仍调用 CPU 渲染核心；CUDA/GPU 后端尚未实现。

## 降噪建议

如果画面噪点很多：

- 预览时可以开启 `仅直接光照`，画面会更干净，但不会有完整全局光照。
- 最终渲染可以提高 `采样数`，例如 128、256 或更高。
- 当前渲染器还没有 denoiser、MIS 和显式光源重要性采样，所以 8 或 32 samples 在完整路径追踪下通常仍会明显有噪点。

## 故障排查

如果 Blender 提示找不到 bridge：

1. 确认已经在项目根目录执行过 `./build.sh`。
2. 确认根目录存在 `raytracer_blender_bridge`。
3. 在 `渲染属性 > Raytracer > Bridge` 中手动选择该可执行文件。

如果 Blender 内置渲染器正常，但 Raytracer 输出纯黑：

1. 先确认 `Bridge` 指向刚刚构建的项目根目录 `raytracer_blender_bridge`；修改 C++ 后需要重新执行 `./build.sh`。
2. 临时开启 `环境光`，把 `环境光颜色` 设为白色、`环境光强度` 设为 `5.0`。如果仍然纯黑，通常说明相机没有命中导出的几何体，或导出的材质基色接近黑色。
3. 在 `调试缓存` 填写一个目录，例如 `/tmp/raytracer-debug`，重新渲染后进入最新的 `raytracer_...` 子目录，用下面命令直接复现：

```bash
./raytracer_blender_bridge --scene scene.rt.json --out-float replay.rgba32f --threads 4 --direct-only
```

bridge 日志会打印图像尺寸、primitive 数量、灯光数量、环境光和背景类型。如果 `primitives=0` 或 `lights=0`，优先检查对象/灯光是否在渲染中被禁用。

本地模式的调试缓存通常包含：

- `scene.rt.json`：Blender 导出的内部场景。
- `command.txt`：插件调用 bridge 的命令。
- `bridge.stderr.log` / `bridge.stdout.log`：bridge 输出日志。
- `result.rgba32f`：bridge 返回给 Blender 的线性 RGBA float 图像。

远程模式的调试缓存通常包含：

- `scene.rt.json`：发送给 server 的内部场景。
- `remote_request.txt`：任务创建请求。
- `remote_create_response.json`：server 返回的 job id。
- `remote_progress.log`：轮询到的进度状态。
- `result.rgba32f`：server 返回给 Blender 的线性 RGBA float 图像。

如果切到 `远程` 后端后渲染失败：

1. 确认服务器上已经启动 `raytracer_server`。
2. 用 `curl http://server:port/health` 检查网络是否可达。
3. 确认 Blender 中的 `远程地址` 包含协议和端口，例如 `http://192.168.1.10:8080`。
4. 如果场景很大，可以适当调大 `远程超时` 和 server 的 `--max-body-mb`。
