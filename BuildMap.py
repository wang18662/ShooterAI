# -*- coding: utf-8 -*-
"""
一键生成七区 AI 测试地图（A/B/C/D/E/F/H）
用途：在 UE 编辑器里执行本脚本，自动生成所有墙体、楼板、坡道、
      导航体、光照、玩家出生点、敌人刷怪点。

执行方式（二选一）：
  1. 菜单 Tools -> Execute Python Script...  选本文件
  2. Output Log 左下角切到 Python，输入：
     exec(open(r"D:/UnrealProjects/ShooterAI/BuildMap.py", encoding="utf-8").read())

注意：脚本会【新建并保存】关卡 /Game/Variant_Shooter/Lvl_AI_Full，
      不会改动你已有的 Lvl_AI_Test。
"""

import unreal

# ============================================================
# 配置
# ============================================================

LEVEL_PATH = "/Game/Variant_Shooter/Lvl_AI_Full"
CUBE_PATH = "/Engine/BasicShapes/Cube.Cube"
MAT_PATH = "/Engine/BasicShapes/BasicShapeMaterial"
SPAWNER_BP = "/Game/Variant_Shooter/Blueprints/AI/BP_ShooterNPCSpawner.BP_ShooterNPCSpawner_C"
GAMEMODE_BP = "/Game/Variant_Shooter/Blueprints/BP_ShooterGameMode.BP_ShooterGameMode_C"

WALL_T = 0.5     # 墙厚 0.5m
WALL_H = 4.0     # 墙高 4m
NAV_AGENT_RADIUS = 45.0

# 全图范围（cm）
X0, X1 = -12000, 5000
Y0, Y1 = -1000, 11500


def log(msg):
    unreal.log("[BuildMap] " + str(msg))


def warn(msg):
    unreal.log_warning("[BuildMap] " + str(msg))


# ============================================================
# 基础设施
# ============================================================

_cube = unreal.EditorAssetLibrary.load_asset(CUBE_PATH)
_mat = unreal.EditorAssetLibrary.load_asset(MAT_PATH)

_eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

_created = []


def prepare_level():
    """新建关卡；若已存在则加载并清空，保证可重复执行。"""
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)

    if unreal.EditorAssetLibrary.does_asset_exist(LEVEL_PATH):
        if not les.load_level(LEVEL_PATH):
            warn("加载已有关卡失败，继续在当前关卡生成")
            return
        # 清空：这个关卡是本脚本的产物，整体重建最干净
        old = list(_eas.get_all_level_actors())
        for a in old:
            _eas.destroy_actor(a)
        log("已加载并清空已有关卡（删除 %d 个 Actor）" % len(old))
    else:
        if les.new_level(LEVEL_PATH):
            log("已新建关卡 " + LEVEL_PATH)
        else:
            warn("新建关卡失败，继续在当前关卡生成")


def box(label, scale, loc, roll=0.0, mat=None):
    """生成一个方块。scale/loc 单位：米 / 厘米。"""
    try:
        actor = _eas.spawn_actor_from_class(
            unreal.StaticMeshActor,
            unreal.Vector(loc[0], loc[1], loc[2]),
            unreal.Rotator(0.0, 0.0, roll),
        )
        actor.set_actor_label(label)
        actor.set_actor_scale3d(unreal.Vector(scale[0], scale[1], scale[2]))

        smc = actor.static_mesh_component
        smc.set_static_mesh(_cube)
        use_mat = mat if mat else _mat
        if use_mat:
            smc.set_material(0, use_mat)

        _created.append(actor)
        return actor
    except Exception as e:
        warn("生成失败 %s: %s" % (label, e))
        return None


def floor(label, x0, x1, y0, y1, top_z=0.0, thickness=0.2):
    """按边界生成地板，顶面在 top_z。"""
    sx = (x1 - x0) / 100.0
    sy = (y1 - y0) / 100.0
    cx = (x0 + x1) / 2.0
    cy = (y0 + y1) / 2.0
    cz = top_z - thickness * 100.0 / 2.0
    return box(label, (sx, sy, thickness), (cx, cy, cz))


def wall_x(label, x0, x1, y, z_center=None, height=WALL_H, thickness=WALL_T):
    """沿 X 方向延伸的墙。"""
    if z_center is None:
        z_center = height * 50.0
    return box(
        label,
        ((x1 - x0) / 100.0, thickness, height),
        ((x0 + x1) / 2.0, y, z_center),
    )


def wall_y(label, y0, y1, x, z_center=None, height=WALL_H, thickness=WALL_T):
    """沿 Y 方向延伸的墙。"""
    if z_center is None:
        z_center = height * 50.0
    return box(
        label,
        (thickness, (y1 - y0) / 100.0, height),
        (x, (y0 + y1) / 2.0, z_center),
    )


# ============================================================
# 0. 新建/重建关卡 + 光照 + 大底板
# ============================================================

prepare_level()

# 天空光照，否则一片黑
try:
    _eas.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 3000),
                                unreal.Rotator(-45.0, -30.0, 0.0)).set_actor_label("Sun")
    _eas.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0, 0, 1000),
                                unreal.Rotator(0, 0, 0)).set_actor_label("SkyLight")
    _eas.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector(0, 0, 0),
                                unreal.Rotator(0, 0, 0)).set_actor_label("SkyAtmosphere")
    _eas.spawn_actor_from_class(unreal.ExponentialHeightFog, unreal.Vector(0, 0, 0),
                                unreal.Rotator(0, 0, 0)).set_actor_label("Fog")
    log("光照已生成")
except Exception as e:
    warn("光照生成失败（不影响玩法）: %s" % e)

# 一整块底板，避免区域交界处漏水
floor("Floor_Master", X0, X1, Y0, Y1, top_z=0.0)


# ============================================================
# A 区 开阔广场  X[-12000,-6000] Y[4000,9000]
# ============================================================

wall_y("A_Wall_W", 4000, 9000, -12000)
wall_x("A_Wall_S", -12000, -6000, 4000)
wall_x("A_Wall_N", -12000, -6000, 9000)
# 东侧由 B 西墙充当（带门洞）

for i, (px, py) in enumerate([(-10000, 7500), (-8000, 7500), (-10000, 5500), (-8000, 5500)], 1):
    box("A_Pillar_%d" % i, (2.5, 2.5, 3.0), (px, py, 150))


# ============================================================
# B 区 迷宫走廊  X[-6000,-1000] Y[3000,9000]
# ============================================================

# 西墙（兼 A 东墙），Y[6200,6800] 6m 门洞
wall_y("B_Wall_W_S", 3000, 6200, -6000)
wall_y("B_Wall_W_N", 6800, 9000, -6000)

# 东墙，Y[6300,6900] 6m 门洞通 H
wall_y("B_Wall_E_S", 3000, 6300, -1000)
wall_y("B_Wall_E_N", 6900, 9000, -1000)

# 南墙，X[-5000,-4400] 6m 门洞通 F
wall_x("B_Wall_S_W", -6000, -5000, 3000)
wall_x("B_Wall_S_E", -4400, -1000, 3000)

wall_x("B_Wall_N", -6000, -1000, 9000)

# 内部隔墙
wall_y("B_V1", 3000, 5500, -5000)
wall_y("B_V2", 4500, 9000, -3500)
wall_y("B_V3", 3000, 7000, -2000)
wall_x("B_H1", -6000, -5300, 5500)   # 死胡同顶盖


# ============================================================
# H 中央枢纽  X[-1000,2000] Y[4500,7500]
# ============================================================

# 东墙，Y[5700,6300] 6m 门洞通 C
wall_y("H_Wall_E_S", 4500, 5700, 2000)
wall_y("H_Wall_E_N", 6300, 7500, 2000)

# 北墙，X[200,800] 6m 门洞通 D
wall_x("H_Wall_N_W", -1000, 200, 7500)
wall_x("H_Wall_N_E", 800, 2000, 7500)

# 南墙，X[-200,200] 4m 门洞通 E
wall_x("H_Wall_S_W", -1000, -200, 4500)
wall_x("H_Wall_S_E", 200, 2000, 4500)

box("H_Pillar", (3.0, 3.0, 4.0), (500, 6000, 200))


# ============================================================
# C 区 三层建筑  X[2000,5000] Y[4500,7500]
# ============================================================

# 外围墙（西侧由 H 东墙充当）
wall_x("C_Wall_S", 2000, 5000, 4500)
wall_y("C_Wall_E", 4500, 7500, 5000)
wall_x("C_Wall_N", 4000, 5000, 7500)   # X[2000,4000] 由 D 南墙充当

# 建筑本体外墙：贯通 3 层，高 9m
BH = 9.0
box("Bldg_W", (WALL_T, 20.0, BH), (2500, 6000, BH * 50.0))
box("Bldg_E", (WALL_T, 20.0, BH), (4500, 6000, BH * 50.0))
box("Bldg_S", (20.0, WALL_T, BH), (3500, 5000, BH * 50.0))
box("Bldg_N", (20.0, WALL_T, BH), (3500, 7000, BH * 50.0))

# 2F / 3F 楼板：4 块拼，中间 4x4m 洞 X[3300,3700] Y[5800,6200]
for floor_name, z_center in (("2F", 290), ("3F", 590)):
    box("%s_Slab_N" % floor_name, (20.0, 8.0, 0.2), (3500, 6600, z_center))
    box("%s_Slab_S" % floor_name, (20.0, 8.0, 0.2), (3500, 5400, z_center))
    box("%s_Slab_W" % floor_name, (8.0, 4.0, 0.2), (2900, 6000, z_center))
    box("%s_Slab_E" % floor_name, (8.0, 4.0, 0.2), (4100, 6000, z_center))

# 两条坡道 30°，各升 3m
box("Ramp_1F_2F", (3.0, 6.0, 0.3), (2800, 5360, 150), roll=-30.0)
box("Ramp_2F_3F", (3.0, 6.0, 0.3), (4200, 6640, 450), roll=30.0)


# ============================================================
# D 区 掩体阵  X[-1000,4000] Y[7500,11500]
# ============================================================

wall_y("D_Wall_W", 7500, 11500, -1000)
wall_y("D_Wall_E", 7500, 11500, 4000)
wall_x("D_Wall_N", -1000, 4000, 11500)
wall_x("D_Wall_S", 2000, 4000, 7500)   # X[-1000,2000] 由 H 北墙充当

COVERS = [
    (0, 8200), (900, 8200), (1800, 8200), (2700, 8200), (3600, 8200),
    (-500, 9400), (400, 9400), (1300, 9400), (2200, 9400), (3100, 9400),
    (0, 10600), (900, 10600), (1800, 10600), (2700, 10600), (3600, 10600),
    (1500, 8800), (2400, 10100), (300, 9800),
]
for i, (cx, cy) in enumerate(COVERS, 1):
    box("D_Cover_%02d" % i, (2.0, 2.0, 1.2), (cx, cy, 60))


# ============================================================
# E 区 高台  X[-1000,1000] Y[2500,4500]
# ============================================================

box("E_Platform", (20.0, 10.0, 4.0), (0, 3000, 200))
box("E_Ramp", (4.0, 10.77, 0.3), (0, 4000, 200), roll=-21.8)


# ============================================================
# F 区 长旷野  X[-12000,-2000] Y[-1000,3000]
# ============================================================

wall_y("F_Wall_W", -1000, 3000, -12000)
wall_y("F_Wall_E", -1000, 3000, -2000)
wall_x("F_Wall_S", -12000, -2000, -1000)
wall_x("F_Wall_N", -12000, -6000, 3000)   # X[-6000,-2000] 由 B 南墙充当


# ============================================================
# 玩家出生点（必须有标签，否则死亡复活报 Accessed None）
# ============================================================

try:
    ps = _eas.spawn_actor_from_class(unreal.PlayerStart, unreal.Vector(-9000, 6500, 120),
                                     unreal.Rotator(0, 0, 0))
    ps.set_actor_label("PlayerStart_A")
    ps.set_editor_property("tags", [unreal.Name("Player0"), unreal.Name("red")])
    log("PlayerStart 已生成（tags: Player0 / red）")
except Exception as e:
    warn("PlayerStart 生成失败: %s" % e)


# ============================================================
# 敌人刷怪点（Z 踩在对应地面高度上）
# ============================================================

spawner_class = unreal.load_class(None, SPAWNER_BP)
if not spawner_class:
    warn("找不到 BP_ShooterNPCSpawner，跳过刷怪点")
    spawner_class = None

SPAWNERS = [
    ("Spawn_A1", -10500, 4500, 0),
    ("Spawn_A2", -7500, 4500, 0),
    ("Spawn_B1", -1500, 8300, 0),
    ("Spawn_C1", 3500, 5500, 0),
    ("Spawn_C2", 3500, 6600, 300),   # 2F 楼板上
    ("Spawn_D1", 3500, 11200, 0),
    ("Spawn_D2", -700, 11200, 0),
    ("Spawn_H1", -600, 5200, 0),
    ("Spawn_H2", 1600, 6800, 0),
    ("Spawn_F1", -11400, 200, 0),
    ("Spawn_F2", -9400, 200, 0),
    ("Spawn_F3", -7400, 200, 0),
    ("Spawn_F4", -5400, 200, 0),
    ("Spawn_F5", -3400, 200, 0),
    ("Spawn_F6", -2400, 200, 0),
]

if spawner_class:
    for name, sx, sy, sz in SPAWNERS:
        try:
            a = _eas.spawn_actor_from_class(spawner_class, unreal.Vector(sx, sy, sz),
                                            unreal.Rotator(0, 0, 0))
            a.set_actor_label(name)
            _created.append(a)
        except Exception as e:
            warn("刷怪点 %s 失败: %s" % (name, e))
    log("刷怪点已生成 %d 个" % len(SPAWNERS))


# ============================================================
# 武器拾取物（模板是「空手出生 + 地上捡枪」，没有这些玩家手上没枪）
# ============================================================

PICKUP_BP = "/Game/Variant_Shooter/Blueprints/Pickups/BP_ShooterPickup.BP_ShooterPickup_C"
DT_PATH = "/Game/Variant_Shooter/Blueprints/Pickups/DT_WeaponList.DT_WeaponList"

PICKUP_SPOTS = [
    (-9000, 5200, "Rifle"),    # A 区南部（离出生点 13m，避免出生瞬间触发重叠）
    (-7000, 4500, "Rifle"),    # A 区东南
    (0, 6800, "Pistol"),       # H 枢纽北侧
    (-5500, 8500, "Rifle"),    # B 区东北
    (-7000, 1000, "Pistol"),   # F 长旷野正中
    (2200, 5800, "Rifle"),     # C 区西侧通道
]

MESH_PATHS = {
    "Rifle": "/Game/Weapons/Rifle/Meshes/SM_Rifle.SM_Rifle",
    "Pistol": "/Game/Weapons/Pistol/Meshes/SM_Pistol.SM_Pistol",
}

pickup_class = unreal.load_class(None, PICKUP_BP)
weapon_dt = unreal.load_asset(DT_PATH) if DT_PATH else None

if pickup_class and weapon_dt:
    made = 0
    for px, py, row in PICKUP_SPOTS:
        try:
            a = _eas.spawn_actor_from_class(pickup_class, unreal.Vector(px, py, 0),
                                            unreal.Rotator(0, 0, 0))
            a.set_actor_label("Pickup_%s_%d_%d" % (row, px, py))

            # FDataTableRowHandle：BeginPlay 会从这行读出 WeaponClass
            handle = unreal.DataTableRowHandle()
            handle.set_editor_property("data_table", weapon_dt)
            handle.set_editor_property("row_name", unreal.Name(row))
            a.set_editor_property("weapon_type", handle)

            mesh_asset = unreal.load_asset(MESH_PATHS.get(row, ""))
            if mesh_asset:
                for comp in a.get_components_by_class(unreal.StaticMeshComponent):
                    comp.set_static_mesh(mesh_asset)
                    break

            _created.append(a)
            made += 1
        except Exception as e:
            warn("拾取物 %s@(%d,%d) 失败: %s" % (row, px, py, e))
    log("武器拾取物已生成 %d / %d" % (made, len(PICKUP_SPOTS)))
else:
    warn("找不到 BP_ShooterPickup 或 DT_WeaponList，跳过拾取物（玩家会空手）")


# ============================================================
# NavMesh 范围 + Agent 半径
# ============================================================

try:
    nav = _eas.spawn_actor_from_class(unreal.NavMeshBoundsVolume,
                                      unreal.Vector((X0 + X1) / 2.0, (Y0 + Y1) / 2.0, 200),
                                      unreal.Rotator(0, 0, 0))
    nav.set_actor_label("NavMeshBounds")
    # 默认体积 200x200x200，缩放到覆盖全图
    nav.set_actor_scale3d(unreal.Vector(
        (X1 - X0) / 200.0,
        (Y1 - Y0) / 200.0,
        5.0,
    ))
    log("NavMeshBoundsVolume 已生成")
except Exception as e:
    warn("NavMesh 体积生成失败: %s" % e)

# 设置 RecastNavMesh 的 Agent Radius
try:
    all_actors = _eas.get_all_level_actors()
    for a in all_actors:
        if isinstance(a, unreal.RecastNavMesh):
            a.set_editor_property("agent_radius", NAV_AGENT_RADIUS)
            log("RecastNavMesh AgentRadius 设为 %g" % NAV_AGENT_RADIUS)
except Exception as e:
    warn("设置 AgentRadius 失败（可在编辑器里手动改）: %s" % e)


# ============================================================
# GameMode（决定玩家用带枪的角色）
# ============================================================

try:
    gm_class = unreal.load_class(None, GAMEMODE_BP)
    if gm_class:
        ws = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world().get_world_settings()
        ws.set_editor_property("default_game_mode", gm_class)
        log("GameMode Override = BP_ShooterGameMode")
except Exception as e:
    warn("设置 GameMode 失败（可在 World Settings 手动设）: %s" % e)


# ============================================================
# 收尾：刷新 + 保存
# ============================================================

try:
    unreal.SystemLibrary.execute_console_command(None, "RebuildNavigation")
except Exception:
    pass

try:
    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()
    log("关卡已保存")
except Exception as e:
    warn("保存失败，请手动 Ctrl+S: %s" % e)

log("=" * 56)
log("完成：共生成 %d 个 Actor" % len(_created))
log("关卡: " + LEVEL_PATH)
log("下一步：按 P 检查 NavMesh 是否全绿，然后 Play 测试")
log("=" * 56)
