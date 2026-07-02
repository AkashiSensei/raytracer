# Raytracer

一个从零开始的 C++ 光线追踪渲染器，输出 PPM/PNG 图片文件。
场景通过 JSON 文件配置，支持命令行参数覆盖。

## 快速开始

### 编译

方式一：项目脚本（推荐）

```bash
./build.sh
```

脚本默认使用 `g++`，也可以通过环境变量覆盖编译器和参数：

```bash
CXX=clang++ CXXFLAGS="-std=c++17 -O2 -Wall -Wextra" ./build.sh
```

方式二：CMake

```bash
cmake -S . -B build
cmake --build build
```

方式三：直接 g++

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -o raytracer src/main.cpp
```

### 测试

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake 测试包含 C++ 回归测试和 deterministic golden image 回归；golden 覆盖基础球、镜面/玻璃/水、Khronos GLB PBR/透明材质、WaterBottle、OBJ MTL 导入、glass_emissive、bunny/mark OBJ、no-normal OBJ、扩展 MTL、studio preset 以及 Khronos CompareRoughness / TransmissionRoughnessTest。更新 golden 图片时先确认渲染变化符合预期，再运行：

```bash
for name in \
    golden_default \
    golden_mirror_glass_water \
    golden_khronos_compare_metallic \
    golden_khronos_attenuation_test \
    golden_khronos_water_bottle \
    golden_khronos_compare_roughness \
    golden_khronos_transmission_roughness_test \
    golden_obj_mtl_test \
    golden_mtl_extended \
    golden_glass_emissive \
    golden_bunny_test \
    golden_no_normal_obj \
    golden_studio_materials
do
    ./build/raytracer --scene "tests/golden/scenes/${name}.json" --threads 1 --out "/tmp/${name}.png"
    python3 tests/golden/compare_images.py --actual "/tmp/${name}.png" --expected "tests/golden/images/${name}.png" --update
done
```

### 性能基准

```bash
./build.sh
./scripts/benchmark.sh
```

`scripts/benchmark.sh` 会用单线程和硬件线程数分别渲染 `default`、`pbr_test`、`mirror_glass_water` 和 `mark` 场景，并通过 `--stats` 打印加载耗时、渲染耗时、总耗时和发光体采样面积。

### 结构化性能报告

```bash
python3 scripts/benchmark_report.py --samples 16 --threads 0 --out reports/benchmark_run
```

`scripts/benchmark_report.py` 会批量渲染全部默认场景（透明 / 网格重 / 面光三类），通过 `--stats-format json` 收集每场景的 load/render time 与 primitive 数，产出 `benchmark_run.csv`、`benchmark_run.json` 和 `benchmark_run.md`（按 render_ms 排序的慢场景表 + 分类均值），便于定位透明 / 网格重场景的瓶颈。

### 渲染质量自动报告

```bash
python3 scripts/render_report.py --samples 64 --threads 0 --out reports/render_run
```

`scripts/render_report.py` 会批量渲染场景 → 校验 PNG → 生成 contact sheet 拼图 → 输出 `report.md`（含每场景 mean_luma / sat_pct / 渲染耗时 + Pass/Needs-work 验收标记）和 `metrics.json`。每轮 PR 都可用它附视觉验收结果。需要 Pillow 才能生成 contact sheet 与 luma 指标。

`--threads 0` 表示使用全部 CPU 硬件线程（默认值，多线程并行渲染）；指定 `--threads 1` 则单线程渲染，便于确定性复现或性能对比。

### HTML 优美验收报告（含高清原图）

```bash
# 一键生成：自动渲染缩略图（64 spp）+ 高清原图（512 spp）+ 自包含 HTML 报告
python3 scripts/html_report.py \
    --render-dir reports/render_html \
    --benchmark reports/benchmark_20260701.json \
    --out reports/render_html/report.html \
    --samples 64 --hq-samples 512 --threads 0

# 只出缩略图报告，关闭 HQ
python3 scripts/html_report.py \
    --render-dir reports/render_quick \
    --out reports/render_quick/report.html \
    --samples 32 --hq-samples 0 --threads 0

# 在浏览器打开
open reports/render_html/report.html
```

`scripts/html_report.py` 生成单文件自包含 HTML 报告（深色主题卡片布局 + 分类筛选 + 性能对比表 + 验收徽章）。每张场景卡片底部有 "View high-res (512 spp, WxH)" 按钮，点击打开全屏 modal 灯箱查看该场景的 512 采样高清原图（支持 Esc / X 关闭、点击图片在新标签打开完整分辨率）。所有图片以 base64 内嵌，HTML 可直接邮件/PR 附件传送。

关键参数：

| 参数 | 含义 | 默认值 |
|------|------|--------|
| `--samples <n>` | 缩略图采样数（卡片用，加载快） | 64 |
| `--hq-samples <n>` | 高清原图采样数（modal 链接用）；设为 0 关闭 HQ | 512 |
| `--threads <n>` | 渲染线程数；`0` = 全部硬件线程（多线程并行） | 0 |
| `--scenes <s1 s2 ...>` | 可选子集场景 | 全部 17 个默认场景 |
| `--force-rerender` | 即使已有渲染图也重新渲染 | 关闭 |

如果 `--render-dir` 里缺少 `metrics.json`、缩略图或高清图，脚本会**自动调用** `render_report.py` 先渲染再生成 HTML，所以只需要跑这一条命令即可。`--benchmark` 可选，提供后会额外展示 1 线程 vs 多线程的加速比表。

### 运行

```bash
# 用默认场景渲染
./run.sh

# 指定场景文件
./run.sh --scene scenes/three_balls.json

# 覆盖输出路径和采样数；.png 会直接写 PNG
./run.sh --scene scenes/three_balls.json --out my.png --samples 64

# 高采样渲染时可指定线程数；不指定时默认使用 CPU 硬件线程数
./run.sh --model models/glb/toyota_mark_ii_jzx100.glb --samples 64 --threads 8 --out car_64.ppm

# 渲染 OBJ 示例场景
./run.sh --scene scenes/mark.json

# 直接指定 OBJ/GLB 模型，未指定 --scene 时默认使用 scenes/obj.json 作为通用模板
./run.sh --model models/obj/mark.obj --out mark.ppm
./run.sh --model models/glb/toyota_mark_ii_jzx100.glb --out car.ppm

# 快速清晰预览：关闭随机递归反弹，只保留直接光照和阴影
./run.sh --preview --model models/glb/toyota_mark_ii_jzx100.glb --out car_preview.ppm

# 也可以保留指定采样数，只关闭递归随机反弹
./run.sh --direct-only --model models/glb/toyota_mark_ii_jzx100.glb --samples 8 --out car_direct.ppm
```

### Blender 5.1 渲染器入口

项目额外提供了一个 Blender RenderEngine 插件，可以直接渲染 Blender 当前场景，不需要手写 `scenes/*.json`，也不会影响原有 CLI 渲染路径。插件支持本地 bridge 渲染，也支持通过 HTTP 连接远程 `raytracer_server`。

Blender 插件的安装、构建、参数说明、完整支持清单和当前限制见 [integrations/blender/README.md](integrations/blender/README.md)。支持清单会明确说明 Blender 中的相机、模型、世界、灯光、材质和调试缓存如何导出，以及渲染核心会如何近似处理。

渲染核心的一条光线如何经过 `ray_color()`、直接光照、发光物体采样、BSDF 反弹和 MIS，见 [docs/render-flow.md](docs/render-flow.md)。

### 命令行参数

| 参数 | 含义 | 默认值 |
|------|------|--------|
| `--scene <path>` | 场景 JSON 文件 | `scenes/default.json` |
| `--model <path>` | OBJ/GLB 模型文件，覆盖场景中 `mesh` 的 `obj/path` | 未指定时使用场景配置 |
| `--obj <path>` | `--model` 的兼容别名 | 未指定时使用场景配置 |
| `--out <path>` | 输出图像路径，支持 `.ppm` / `.png`（覆盖场景配置） | 场景中的 `output` |
| `--samples <n>` | 每像素采样数（覆盖场景配置） | 场景中的 `samples` |
| `--threads <n>` | 渲染线程数，用于高采样加速 | CPU 硬件线程数 |
| `--exposure <n>` | 显示曝光倍率（覆盖场景配置） | 场景中的 `exposure` 或 1.0 |
| `--tone-map <mode>` | 色调映射：`aces`、`reinhard`、`none` | 场景中的 `tone_map` 或 `aces` |
| `--seed <n>` | 固定随机种子，便于复现渲染结果 | 场景中的 `seed` 或随机种子 |
| `--firefly-clamp <n>` | 按样本限制最大 radiance 通道，降低透明/高光 fireflies | 场景中的 `firefly_clamp` 或关闭 |
| `--stats` | 输出 load/render/total 耗时和场景统计 | 关闭 |
| `--stats-format <m>` | stats 输出格式：`text` 或 `json`（便于 `scripts/benchmark_report.py` 解析） | `text` |
| `--direct-only` | 只计算直接光照、阴影和环境光，关闭递归随机反弹以减少噪声 | 关闭 |
| `--preview` | 快速预览模式：启用 `--direct-only`，且未指定 `--samples` 时使用 1 sample | 关闭 |
| `--help` | 显示帮助 | — |

### 预览

输出路径以 `.png` 结尾时会直接写 PNG；其他扩展名保持旧行为写 PPM。PPM 可被多数看图软件直接打开。macOS 下转 PNG：

```bash
sips -s format png out.ppm --out out.png
open out.png
```

也可以使用项目自带脚本自动转换：

```bash
# 默认把 out.ppm 转成 out.png
./scripts/ppm_to_png.sh

# 指定输入文件，输出名默认同名 .png
./scripts/ppm_to_png.sh three_balls.ppm

# 同时指定输入和输出
./scripts/ppm_to_png.sh three_balls.ppm preview.png
```

## 场景文件格式

场景用 JSON 描述，包含 `image`/`render`、`scene`、`environment`、`camera`、`lighting`、`lights`、`ground`、`objects` 几部分。`render` 是 `image` 的新别名；两者同时存在时，`render` 会覆盖同名渲染设置。见 `scenes/` 目录示例。

```json
{
    "image": {
        "width": 800,
        "height": 400,
        "samples": 16,
        "max_depth": 32,
        "output": "out.png",
        "tone_map": "aces",
        "exposure": 1.0,
        "seed": 12345,
        "firefly_clamp": 12.0
    },
    "scene": {
        "preset": "studio_softbox"
    },
    "environment": {
        "type": "gradient",
        "top": [0.5, 0.7, 1.0],
        "bottom": [1.0, 1.0, 1.0],
        "intensity": 1.0
    },
    "camera": {
        "lookfrom": [0, 0, 0],
        "lookat": [0, 0, -1],
        "vup": [0, 1, 0],
        "vfov": 60,
        "aperture": 0.0,
        "focus_dist": 1.0
    },
    "lighting": {
        "ambient": [0.05, 0.05, 0.05]
    },
    "lights": [
        {
            "type": "directional",
            "direction": [-1, -1.5, -1],
            "color": [1.0, 0.96, 0.9],
            "intensity": 0.9
        },
        {
            "type": "point",
            "position": [3, 4, 5],
            "color": [0.8, 0.9, 1.0],
            "intensity": 5.0
        }
    ],
    "ground": {
        "enabled": true,
        "material": {
            "type": "lambertian",
            "albedo": [0.62, 0.60, 0.55]
        }
    },
    "objects": [
        {
            "type": "sphere",
            "center": [0, 0, -1],
            "radius": 0.5,
            "material": {
                "type": "lambertian",
                "albedo": [0.1, 0.2, 0.5]
            }
        }
    ]
}
```

### 字段说明

**image / render**（均可选，有默认值；`render` 是兼容旧 `image` 的新命名）

| 字段　　　　| 类型　 | 默认值　　　| 说明　　　　　　　　　　　　　 |
| -------------| --------| -------------| --------------------------------|
| `width`　　 | int　　| 800　　　　 | 图像宽度　　　　　　　　　　　 |
| `height`　　| int　　| 400　　　　 | 图像高度　　　　　　　　　　　 |
| `samples`　 | int　　| 16　　　　　| 每像素采样数，越大越细腻、越慢 |
| `max_depth` | int　　| 32　　　　　| 光线递归深度上限　　　　　　　 |
| `output`　　| string | `"out.ppm"` | 输出文件路径，`.png` 直接写 PNG，其他扩展名写 PPM |
| `tone_map`  | string | `"aces"`     | 显示色调映射：`aces`、`reinhard`、`none` |
| `exposure`  | float  | 1.0          | 显示曝光倍率 |
| `seed`      | int    | 随机种子     | 固定随机序列，常用于回归测试 |
| `firefly_clamp` | float | 关闭 | 按样本整体缩放过亮 radiance，保留色相，用于压低玻璃/水/高光 fireflies |

**scene / environment**（均可选）

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `scene.preset` | string | 无 | 当前支持 `"studio_softbox"` / `"studio"` / `"material_studio"`，提供材质测试棚软箱灯 |
| `environment.type` | string | `"gradient"` | 支持 `"gradient"`、`"solid"`、`"texture"` |
| `environment.top` / `bottom` | [r,g,b] | 天空蓝 / 白色 | 渐变背景上下颜色 |
| `environment.color` | [r,g,b] | 白色 | solid 背景颜色 |
| `environment.texture` / `path` / `file` | string | 无 | equirectangular 背景纹理 |
| `environment.intensity` | float | 1.0 | 背景亮度倍率 |

**camera**（均可选，有默认值）

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `lookfrom` | [x,y,z] | [0,0,0] | 相机位置 |
| `lookat` | [x,y,z] | [0,0,-1] | 相机看向的点 |
| `vup` | [x,y,z] | [0,1,0] | 相机上方向 |
| `vfov` | float | 60 | 垂直视野角（度） |
| `aperture` | float | 0.0 | 光圈孔径，0 = 无景深 |
| `focus_dist` | float | 1.0 | 对焦距离 |
| `auto` | bool | false | 为 true 时根据模型包围盒自动放置相机，可搭配 `vfov` 使用 |
| `focal_length` + `sensor_height` / `sensor_width` | float | 无 | 按摄影机焦距和传感器尺寸换算垂直 FOV |
| `orbit.target` / `yaw` / `pitch` / `distance` | object | 无 | 用目标点、角度和距离生成环绕相机 |
| `view` | string | 默认正面略高 | 自动相机视角：`front`、`back`、`left`、`right`、`top`、`bottom`、`three_quarter` |
| `margin` | float | 1.35 | 自动 framing 留白倍率 |
| `target_offset` | [x,y,z] | [0,0,0] | 自动 framing 的目标点偏移 |

**background / lighting / lights**（均可选；CLI 会为未显式配置的场景提供展示友好的默认背景和光照）

| 字段 | 类型 | 说明 |
|------|------|------|
| `background` | object 或 [r,g,b] | 兼容旧字段，等价于 `environment`；数组表示纯色背景，object 支持 `type: "solid"` / `"gradient"` / `"sky"` / `"texture"` |
| `lighting.ambient` | [r,g,b] | 环境光颜色，影响阴影内的最低亮度 |
| `lights[].type` | string | 支持 `"directional"`、`"point"`、`"spot"`、`"sphere"`、`"rect"`、`"disk"` |
| `lights[].direction` | [x,y,z] | 方向光方向，表示光线照射方向 |
| `lights[].position` | [x,y,z] | 点光、聚光、球形光、矩形光的位置 |
| `lights[].radius` | float | 球形光/盘形光半径 |
| `lights[].angle` / `soft_angle` | float | 聚光灯外锥角和柔边角度 |
| `lights[].u` / `v` | [x,y,z] | 矩形光两条边向量；也可用 `width` / `height` + `direction` 生成 |
| `lights[].color` | [r,g,b] | 光源颜色 |
| `lights[].intensity` | float | 光源强度；局部光会随距离平方衰减，面积光会按面积采样 |

渲染器会对方向光、点光、聚光、球形光、矩形光、盘形光发射阴影射线，被遮挡的光源不会给当前命中点贡献直接光照。

CLI 会保留这些默认项，这是命令行场景格式的易用性设计：用更少的参数也能快速得到适合展示和调试的基础光照效果。需要完全控制场景时，可以显式关闭对应默认项：

- 未写 `environment` / `background` 时使用蓝白天空渐变；显式写 `{"type":"solid","color":[0,0,0]}` 可关闭天空。
- 未写 `lighting.ambient` 时使用弱环境光 `[0.04,0.04,0.04]`；显式写 `[0,0,0]` 可关闭环境光。
- 未写 `lights` 时使用默认方向光和点光源；显式写空数组 `[]` 可关闭默认灯光。

**ground**（可选；用于自动生成接收阴影的地面）

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `enabled` | bool | true | 是否生成地面；只有写了 `ground` 块才会启用 |
| `size` | float | 根据主体包围盒自动计算 | 地面边长 |
| `y` | float | 主体包围盒最低点略下方 | 地面高度 |
| `material` | object | 灰米色漫反射材质 | 地面材质，格式同普通 `material` |

自动地面会加入 BVH 参与求交，因此模型可以把点光源/方向光阴影投到地面上，形成接触阴影。自动相机仍按模型主体包围盒取景，不会被大地面拉远。

**objects**（数组，每个元素是一个物体）

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 目前支持 `"sphere"`、`"mesh"` |
| `center` | [x,y,z] | 球心坐标 |
| `radius` | float | 半径 |
| `obj` / `path` | string | `mesh` 使用的 OBJ/GLB 文件路径 |
| `auto_fit` | bool | `mesh` 是否自动居中和缩放，未写 `scale/translate` 时默认开启 |
| `fit_size` | float | 自动缩放后的最长轴尺寸，默认 3.0 |
| `fit_center` | [x,y,z] | 自动居中后的目标中心，默认 [0,0,0] |
| `scale` | float | 手动缩放，或在 `auto_fit` 下作为缩放倍率 |
| `translate` | [x,y,z] | 手动平移，或在 `auto_fit` 下作为额外偏移 |
| `material` | object | 模型没有内嵌材质时使用的 fallback 材质，见下 |
| `override_material` | bool | 为 true 时强制用 `material` 覆盖 GLB 内嵌材质 |

**material.type** 支持五种：

| 类型 | 额外字段 | 说明 |
|------|----------|------|
| `"lambertian"` | `albedo`: [r,g,b], `texture`: string/object | 漫反射（哑光） |
| `"metal"` | `albedo`: [r,g,b], `texture`: string/object, `fuzz`: 0~1 | 金属反射，fuzz=0 镜面，越大越粗糙 |
| `"dielectric"` | `ior`: float, `albedo`: [r,g,b]（可选）, `roughness`: 0~1（可选）, `attenuation_color`: [r,g,b]（可选）, `attenuation_distance`: float（可选） | 玻璃/水等透明介质，ior=1.5 玻璃，1.33 水；`roughness` 表现粗糙透射，`attenuation_*` 用 Beer-Lambert 近似表现厚玻璃/水体吸收 |
| `"pbr"` | `albedo`/`metallic`/`roughness` + 各 `_map` + `normal_map` | Cook-Torrance 金属-粗糙度材质 |
| `"emissive"` | `emission`: [r,g,b] | 自发光材质，可作为面光源 |

颜色 `albedo` 各分量范围 [0, 1]。`texture` 可以直接写图片路径，也可以写成 `{ "path": "..." }`。当前支持 PPM/PNM，PNG/JPG 通过 `stb_image` 解码。

### 示例场景

| 文件 | 内容 |
|------|------|
| `scenes/default.json` | 漫反射球 + 地面（基础验证） |
| `scenes/three_balls.json` | 漫反射 + 玻璃 + 金属 三球场景 |
| `scenes/glass_bottle.json` | GLB 玻璃瓶（自动识别 BLEND 为透明介质） |
| `scenes/glass_volume.json` | 玻璃瓶 + 体积吸收（Beer-Lambert 淡绿色玻璃） |
| `scenes/glass_emissive.json` | 玻璃瓶 + Emissive 面光源（MIS 验证） |
| `scenes/cornell_box.json` | Cornell Box（经典 MIS 验证：面光源 + 漫反射墙 + color bleeding） |
| `scenes/glass.json` | 玻璃材质验证 |
| `scenes/obj.json` | 通用 OBJ/GLB 模型模板，配合 `--model` 使用 |
| `scenes/mark.json` | 加载 `models/obj/mark.obj` 的网格场景 |
| `scenes/obj_mtl_test.json` | OBJ `.mtl` 的基础颜色/贴图材质验收场景 |
| `scenes/triangle_test.json` | 三角形 + 三角网格（验证 UV 插值） |
| `scenes/bunny_test.json` | OBJ 网格 + 变换验证 |
| `scenes/no_normal_obj.json` | 无法线 OBJ + 平滑法线 |
| `scenes/pbr_test.json` | PBR 金属-粗糙度材质验证 |
| `scenes/texture_test.json` | 贴图采样验证 |
| `scenes/textured_quad.json` | 带 UV 和棋盘纹理的最小贴图验证场景 |
| `scenes/blender_room_texture_export.json` | Blender 导出的房间场景 fixture（相机 frame + rect 面光源 + image 贴图 Mapping） |
| `scenes/disk_light_test.json` | disk 面积光软阴影验证（漫反射球 + 玻璃 + 金属） |
| `scenes/mirror_glass_water.json` | 镜面金属 + 玻璃 + 水面 + PBR + 自发光面积灯验收场景 |
| `scenes/studio_materials.json` | 使用 studio preset、环境背景、自动 framing、镜面/粗糙玻璃/水面的材质测试棚 |
| `scenes/khronos_water_bottle.json` | Khronos WaterBottle 官方 GLB 验收场景 |
| `scenes/khronos_transmission_test.json` | Khronos TransmissionTest 官方 GLB 验收场景 |
| `scenes/khronos_compare_metallic.json` | Khronos CompareMetallic 官方 GLB，验证 metallic 梯度和镜面响应 |
| `scenes/khronos_compare_roughness.json` | Khronos CompareRoughness 官方 GLB，验证 roughness 梯度和高光扩散 |
| `scenes/khronos_attenuation_test.json` | Khronos AttenuationTest 官方 GLB，验证透明体积衰减 |
| `scenes/khronos_transmission_roughness_test.json` | Khronos TransmissionRoughnessTest 官方 GLB，验证粗糙透射 |
| `scenes/khronos_glass_vase_flowers.json` | Khronos GlassVaseFlowers 官方 GLB，对比 alpha blend 与 transmission/volume 玻璃 |
| `scenes/khronos_glass_broken_window.json` | Khronos GlassBrokenWindow 官方 GLB，验证 transmission + alpha mask 破洞玻璃 |
| `scenes/khronos_glass_hurricane_candle_holder.json` | Khronos GlassHurricaneCandleHolder 官方 GLB，验证彩色厚玻璃体积吸收 |

### 官方测试资产

Khronos 官方 glTF Sample Assets 通过 `scripts/fetch_khronos_assets.sh` 下载到 `models/khronos/`。资产来源、license 和 credit 记录在 `models/khronos/LICENSES.md`。

## OBJ / GLB 网格支持

当前版本已支持在场景 JSON 中通过 `mesh` 对象加载 Wavefront OBJ 或 GLB 模型，并输出渲染图片。

- 支持 OBJ `v`、`vn`、`vt`、`f`
- 支持 OBJ `vt`，命中三角形时会插值 UV 并采样材质纹理
- 支持 OBJ `.mtl` 的 `Kd` / `map_Kd`、`Ke` / `map_Ke`、`d` / `Tr` / `map_d`、`Ni`、`Ks` / `map_Ks`、`Ns` / `map_Ns`，可绑定基础颜色、贴图、自发光、透明度、折射率和高光粗糙度近似
- 支持三角形、四边形和一般多边形面，内部会自动扇形拆分为三角形
- 支持 `v` / `v//vn` / `v/vt/vn` 等常见面索引格式
- OBJ 缺法线时会按面积加权生成平滑法线；normal/UV 混合缺失时会补齐安全默认值
- 支持 GLB 里的 `POSITION`、`NORMAL`、`TEXCOORD_0`、`indices` 和节点矩阵/TRS
- 支持 GLB PBR 基础材质：`baseColorFactor`、`baseColorTexture`、`metallicFactor`、`roughnessFactor`、`metallicRoughnessTexture`、`normalTexture`、`emissiveFactor`、`emissiveTexture`
- `metallicRoughnessTexture` 按 glTF 约定读取：G 通道驱动 roughness，B 通道驱动 metallic
- GLB PBR 材质会保持表面 BRDF；`emissiveFactor` / `emissiveTexture` 作为附加发光项，不会把整个表面降级为纯面积光
- 支持 GLB 贴图读取，可读取内嵌 bufferView 图片或外部图片路径
- 支持 GLB `alphaMode: "BLEND"` 自动识别为透明介质（Dielectric）；`alphaMode: "MASK"` + `alphaCutoff` 会真正生效：命中点 alpha 低于 cutoff 时丢弃命中并让主光线/阴影光线继续穿过
- 支持 GLB KHR 扩展：`KHR_materials_transmission`（透射因子与 transmissionTexture）、`KHR_materials_ior`（折射率）、`KHR_materials_volume`（透明介质衰减、thicknessFactor、thicknessTexture）、`KHR_texture_transform`（UV scale/offset）
- GLB `doubleSided: true` 会标记材质为双面，Dielectric 不再剔除背面
- GLB 材质路由优先级：KHR transmission / `alphaMode: "BLEND"` > PBR > metallic > lambertian；非 PBR 的纯自发光导入材质仍会映射为 `Emissive`
- 未支持的 GLB 扩展会被忽略并使用 fallback 材质行为
- GLB 材质会按 primitive 自动绑定到三角形；`material` 字段只作为 fallback，除非设置 `override_material: true`
- 支持根据 OBJ/GLB 包围盒自动居中、缩放模型
- 支持没有显式 `camera` 时根据模型包围盒自动放置相机
- 支持 `transform` 块中的平移、欧拉旋转、轴角旋转、统一/非统一缩放
- 支持顶层 `scale` 和 `translate`；在 `auto_fit` 下它们会作为自动适配后的额外调整
- 网格会合并为 `TriangleMesh`，内部构建三角形级 BVH 加速求交
- OBJ `.mtl` 的高光参数会保守映射到现有 `Metal` / `PBR` 近似；完整 Phong/SpecGloss 工作流仍不是目标

### 变换

`transform` 块包含可选的 `translate`/`rotate`/`rotate_axis`/`scale`，按 SRT 顺序（先缩放，再旋转，最后平移）烘焙到顶点：

```json
{
    "type": "mesh",
    "file": "../models/obj/mark.obj",
    "transform": {
        "translate": [-0.5, 0, 0],
        "rotate": [0, 180, 0],
        "rotate_axis": { "axis": [0, 1, 0], "angle": 45 },
        "scale": 0.2
    },
    "material": { "type": "lambertian", "albedo": [0.8, 0.6, 0.2] }
}
```

- `translate`: `[x, y, z]` 平移
- `rotate`: `[rx, ry, rz]` 欧拉角（度），XYZ 顺序
- `rotate_axis`: `{axis: [x,y,z], angle: 度数}` 轴角旋转（在 `rotate` 之前应用）
- `scale`: 数值（统一缩放）或 `[sx, sy, sz]`（非统一缩放）

### 向后兼容

旧字段 `obj` + 顶层 `scale` + 顶层 `translate` 仍然可用（仅支持缩放和平移，无旋转）。示例：

```json
{
    "type": "mesh",
    "obj": "../models/obj/mark.obj",
    "auto_fit": true,
    "fit_size": 3.0,
    "fit_center": [0, 0, 0],
    "override_material": false,
    "material": {
        "type": "lambertian",
        "texture": "../textures/checker.pnm",
        "albedo": [0.75, 0.2, 0.2]
    }
}
```

## BVH 加速结构

场景图元超过 1 个时自动构建线性 BVH 加速射线求交。

- **线性节点**：`LinearBVHNode` 为 POD 结构（AABB + 左右子索引 + 叶子图元起止），可直接 `memcpy` 到 GPU
- **构建**：SAH（Surface Area Heuristic）分桶构建——对每个轴将图元按质心分入 12 个桶，评估每个桶边界的 SAH 代价 `C = (SA_left·N_left + SA_right·N_right) / SA_parent`，选择代价最低的轴与分割点；质心重合等退化情况回退到中点划分。三角网格内部 BVH 同样使用 SAH（叶子粒度 4 三角形）
- **性能**：SAH 相比中点划分在网格重场景下显著提速——mark.obj ~52%、Khronos WaterBottle ~41%、glass_bottle ~23%、Khronos TransmissionTest ~23%
- **遍历**：栈数组迭代（深度 64），与未来 GPU 栈遍历逻辑一致
- **结果一致性**：因 `hit()` 取最近命中，遍历顺序变化不影响渲染结果

后续阶段（12/14）将复用此线性结构与栈遍历逻辑实现 GPU BVH。

## PBR 材质与纹理（阶段 11）

支持物理合理的金属-粗糙度着色模型。

### 材质类型

| 类型 | 字段 | 说明 |
|------|------|------|
| `lambertian` | `albedo` | 漫反射（保留） |
| `metal` | `albedo`, `fuzz` | 金属反射（保留） |
| `dielectric` | `ior`, `albedo`（可选） | 玻璃/水（保留） |
| `pbr` | `albedo`/`metallic`/`roughness` + 各 `_map` + `normal_map` | Cook-Torrance BRDF + NEE/MIS |
| `emissive` | `emission` | 自发光（面光源） |

### PBR 示例

```json
{
    "type": "pbr",
    "albedo": [0.8, 0.2, 0.2],
    "albedo_map": "../textures/checkerboard.png",
    "metallic": 1.0,
    "roughness": 0.05,
    "normal_map": "../textures/normal.png"
}
```

- 标量值（`albedo`/`metallic`/`roughness`）为默认，`_map` 字段加载贴图覆盖
- `normal_map` 通过切线空间（TBN）扰动法线
- BRDF：Cook-Torrance（GGX 法线分布 + Smith 几何项 + Fresnel-Schlick）
- 采样：GGX 重要性采样，提高收敛速度

### 自发光

```json
{ "type": "emissive", "emission": [8, 8, 8] }
```

`emission` 值可 >1，作为面光源参与全局光照。

## 项目结构

```
raytracer/
├── CMakeLists.txt                构建配置
├── build.sh                      构建脚本（g++ 直接编译）
├── run.sh                        运行脚本（编译后直接执行）
├── include/raytracer/            头文件库（header-only，按模块分目录）
│   ├── math/                     模块 A：数学库
│   │   ├── util.h                常量 (pi/infinity)、随机数、角度换算
│   │   ├── vec3.h                三维向量（兼作 Point3 / Color）、reflect/refract
│   │   ├── vec2.h                二维向量（纹理坐标）
│   │   ├── mat4.h                4x4 变换矩阵（平移/旋转/缩放/法线变换）
│   │   └── ray.h                 射线：origin + direction，at(t)
│   ├── geometry/                 模块 B：几何求交
│   │   ├── hittable.h            可命中物体抽象基类 + HitRecord（含 tangent/UV）
│   │   ├── sphere.h              球体：射线-球求交
│   │   ├── hittable_list.h       物体列表：遍历找最近交点
│   │   ├── triangle.h            单三角形（Möller–Trumbore + UV 插值）
│   │   ├── triangle_mesh.h       三角网格（SoA 布局 + 内部 BVH）
│   │   ├── aabb.h                轴对齐包围盒
│   │   └── bvh.h                 线性 BVH 加速结构（扁平节点 + 栈遍历）
│   ├── material/                 模块 C：材质
│   │   ├── material.h            Lambertian / Metal / Dielectric / PBR / Emissive
│   │   └── texture.h             纹理基类 + SolidColor / Image / Tinted / Checker
│   ├── render/                   模块 D：渲染
│   │   ├── camera.h              相机：FOV、lookat、景深
│   │   ├── image.h               PPM/PNG 写入 + tone mapping + 伽马校正
│   │   └── texture.h             渲染端纹理工具
│   └── scene/                    模块 D：场景
│       ├── json.h                最小 JSON 解析器（零依赖）
│       ├── obj.h                 OBJ 加载器（手写 + tinyobjloader）
│       ├── glb.h                 GLB 加载器（glTF 2.0 + KHR 扩展）
│       └── scene.h               场景加载器：JSON → Camera + HittableList + Lights
├── src/main.cpp                  命令行解析 + 渲染主循环 + ray_color 递归
├── third_party/                  第三方 header-only 库
│   ├── stb_image.h               PNG/JPG 解码
│   ├── stb_image_write.h         PNG 写入
│   └── tiny_obj_loader.h         OBJ 解析
├── scenes/                       场景文件目录
├── models/                       测试模型（OBJ/GLB）
├── textures/                     测试纹理
├── scripts/                      辅助脚本（ppm_to_png.sh）
├── tests/                        回归测试
└── docs/                         设计文档（specs + plans）
```

头文件 include 路径统一为 `#include "raytracer/<模块>/<文件>.h"`，例如 `#include "raytracer/math/vec3.h"`。


## 当前实现状态

- ✅ 阶段 0：骨架 + PPM 渐变图
- ✅ 阶段 1：射线-球求交
- ✅ 阶段 2：法线 / 天空背景
- ✅ 阶段 3：多物体（球 + 地面）
- ✅ 阶段 4：Lambert 漫反射 + 柔和阴影
- ✅ 阶段 5：抗锯齿（多次采样平均）
- ✅ 阶段 6：材质系统（Lambert / Metal / Dielectric 全部完成）
- ✅ 阶段 7：场景文件化（JSON 场景 + 命令行参数）
- ✅ 阶段 8：三角形与三角网格求交（Möller–Trumbore + UV 插值 + SoA 网格）
- ✅ 阶段 9：OBJ 模型加载与变换（tinyobjloader + 4x4 矩阵 + 平滑法线）
- ✅ 阶段 10：BVH 线性化加速（扁平节点 + 栈遍历，GPU 友好；SAH 分桶构建）
- ✅ 阶段 11：PBR 材质 + 纹理 + 自发光（Cook-Torrance + GGX 重要性采样）
- ✅ 真实 Beer-Lambert 体积吸收：Ray 携带当前介质（attenuation_color + attenuation_distance），折射进入/退出时切换介质，TIR/Fresnel 反射保持介质，累积路径吸收
- ✅ 光源类型：directional / point / spot / sphere 面积光 / rect 面积光 / disk 面积光（圆形面积光，软阴影 + NEE）
- ✅ GLB 玻璃材质支持：`alphaMode: BLEND` 自动识别 + `KHR_materials_transmission` / `KHR_materials_ior` 扩展 + Dielectric albedo/纹理
- ✅ 透明材质降噪：Russian roulette 路径终止 + partial transmission（`KHR_materials_transmission` 因子）+ rough transmission 重要性采样 + `firefly_clamp`
- ✅ glTF 透明材质完整接入：`KHR_materials_volume` thicknessFactor/thicknessTexture + `KHR_materials_transmission` transmissionTexture 真正参与着色
- ✅ GLB/OBJ 导入兼容：`alphaMode: MASK` + `alphaCutoff` 真正生效并穿透主光线/阴影光线、`doubleSided` 背面不剔除、`KHR_texture_transform` UV scale/offset、MTL `map_Ks` / `map_Ns` / `map_d` 贴图
- ✅ 输出色彩控制：ACES / Reinhard / None 色调映射 + 可调 exposure
- ✅ PBR 走 NEE（Cook-Torrance 的 f/pdf 实现 + MIS 权重）
- ✅ Blender RenderEngine 接入：本地 bridge / 远程 server 渲染、evaluated mesh 导出、调试缓存、活动相机 `view_frame()` 显式取景平面导出，使视角/缩放更接近 Cycles
- ✅ Blender 解析光源导出：Point / Sun / Spot，以及 Area Light 的 rect / disk 面光源采样；Area 默认相机不可见、specular/glossy 可见，并提供“面光源相机可见”插件选项
- ✅ Blender 材质贴图导出：Principled BSDF 的基础色 / 金属度 / 糙度支持 Checker、Image、Noise、ColorRamp、Math、Mix、Invert、Map Range、RGB/Value 等常见节点，并导出 Mapping 的 scale / offset / rotation
- ✅ Blender 贴图坐标与诊断：导出 active UV，支持 Generated/Object 近似坐标，记录 unsupported texture 节点和混合坐标诊断，并提供 `scripts/inspect_texture_export.py` 检查调试缓存
- ✅ Blender 渐进预览与采样调度：本地 bridge 通过 `PARTIAL` RGBA32F 快照、远程 server 通过 `/jobs/<id>/partial` 和 `partial_seq` 逐步刷新 Render Result；支持按行完成和全图 sample batch 累积两种顺序，本地默认 batch 16、远程默认 batch 64，最终图像路径不变
- ✅ 性能 profiling：`--stats-format json` + `scripts/benchmark_report.py`（CSV/JSON/MD，分类定位透明/网格重瓶颈）
- ✅ 渲染质量自动报告：`scripts/render_report.py`（批量渲染 → PNG 校验 → contact sheet → markdown 验收报告）+ `scripts/html_report.py`（自包含 HTML 报告 + 高清原图 modal 灯箱）
- ✅ Golden 回归覆盖扩展：default、mirror_glass_water、Khronos WaterBottle/CompareMetallic/Attenuation/CompareRoughness/TransmissionRoughness、OBJ MTL、扩展 MTL、glass_emissive、bunny、no-normal OBJ、studio_materials
- ⬜ 后续：CUDA 移植

## 渲染原理速览

1. 相机对每个像素发射一条主射线
2. `ray_color` 求射线与场景最近交点
3. 命中 → 计算直接光照（`direct_delta_lights`：遍历光源 + 阴影射线 + Lambert/PBR BRDF）
4. 调材质 `scatter()` 生成间接射线，颜色按 attenuation 衰减后递归
5. 透明材质（Dielectric）走 specular 路径：Fresnel + partial transmission 因子决定反射/折射概率；rough transmission 用 rough² 扰动出射方向
6. specular / transparent 路径在深度 ≥4 后启用 Russian roulette：按 luminance 决定续跑概率，幸存路径按 1/p 放大，保持无偏
7. `alphaMode: MASK` 命中点 alpha < cutoff 时丢弃命中，光线继续穿过
8. 未命中 → 返回环境背景（gradient/solid/texture）
9. 递归到 `max_depth` 或射线不散射则停止
10. 每像素多次采样平均，`firefly_clamp` 限制单样本峰值，消除锯齿与高光飞点
11. 输出前做 exposure、色调映射和 gamma 2.2 校正

## 开发环境要求

- C++17 编译器（g++ / clang++ / MSVC 均可）
- CMake ≥ 3.10（可选）
- 第三方 header-only 库（已包含在 `third_party/`）：`stb_image.h`（图片解码）、`stb_image_write.h`（PNG 写入）、`tiny_obj_loader.h`（OBJ 解析）
