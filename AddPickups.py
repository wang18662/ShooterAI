# -*- coding: utf-8 -*-
"""
给【当前打开的地图】补上武器拾取物（不重建关卡，不动已有 Actor）。

为什么需要：模板是「空手出生 + 地上捡枪」的设计。
BuildMap.py 生成的地图里没有拾取物，所以玩家手上没枪。

用法：
  1. 打开 Lvl_AI_Full
  2. Tools -> Execute Python Script... 选本文件
     （或 Output Log 切到 Python：
      exec(open(r"D:/UnrealProjects/ShooterAI/AddPickups.py", encoding="utf-8").read())）
"""

import unreal

# ============================================================
# 配置
# ============================================================

PICKUP_BP_PATH = "/Game/Variant_Shooter/Blueprints/Pickups/BP_ShooterPickup.BP_ShooterPickup_C"
DT_PATH = "/Game/Variant_Shooter/Blueprints/Pickups/DT_WeaponList.DT_WeaponList"

# 每个未指定武器的拾取点，按顺序循环取这些行
WEAPON_ROWS = ["Rifle", "Rifle", "Pistol", "Rifle", "Pistol"]

# 拾取物位置：(x, y, z)，z 填 0（碰撞球挂在 Z+84，正好在胸口高度）
PICKUP_SPOTS = [
    ("Pickup_A1", -9000, 6500),    # A 区正中（出生点旁边）
    ("Pickup_A2", -7000, 4500),    # A 区东南
    ("Pickup_H1", 0, 6800),        # H 枢纽北侧
    ("Pickup_B1", -5500, 8500),    # B 区东北
    ("Pickup_F1", -7000, 1000),    # F 长旷野正中
    ("Pickup_C1", 2200, 5800),     # C 区西侧通道
]

# 武器模型（拾取物的可见外观；不设也行，只是看不见东西在地上）
MESH_PATHS = {
    "Rifle": "/Game/Weapons/Rifle/Meshes/SM_Rifle.SM_Rifle",
    "Pistol": "/Game/Weapons/Pistol/Meshes/SM_Pistol.SM_Pistol",
    "GrenadeLauncher": "/Game/Variant_Shooter/Blueprints/Pickups/Meshes/SM_GrenadeLauncher.SM_GrenadeLauncher",
}

_eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def log(m):
    unreal.log("[AddPickups] " + str(m))


def warn(m):
    unreal.log_warning("[AddPickups] " + str(m))


# ============================================================
# 加载依赖
# ============================================================

pickup_class = unreal.load_class(None, PICKUP_BP_PATH)
if not pickup_class:
    warn("找不到 BP_ShooterPickup: " + PICKUP_BP_PATH)
    raise SystemExit

dt = unreal.load_asset(DT_PATH)
if not dt:
    warn("找不到武器数据表: " + DT_PATH)
    raise SystemExit

log("拾取物蓝图与武器表加载成功")


# ============================================================
# 生成
# ============================================================

made = 0

for i, (label, x, y) in enumerate(PICKUP_SPOTS):
    row = WEAPON_ROWS[i % len(WEAPON_ROWS)]

    try:
        actor = _eas.spawn_actor_from_class(
            pickup_class,
            unreal.Vector(float(x), float(y), 0.0),
            unreal.Rotator(0.0, 0.0, 0.0),
        )
        actor.set_actor_label("%s_%s" % (label, row))

        # 关键：武器类型是 FDataTableRowHandle（数据表 + 行名）
        # BeginPlay 会从这个行里读出 WeaponClass，走到上面就自动拿到枪
        handle = unreal.DataTableRowHandle()
        handle.set_editor_property("data_table", dt)
        handle.set_editor_property("row_name", unreal.Name(row))
        actor.set_editor_property("weapon_type", handle)

        # 顺手设一下可见模型（不给的话地上什么都没有，但捡起来照样有枪）
        mesh_path = MESH_PATHS.get(row)
        if mesh_path:
            mesh_asset = unreal.load_asset(mesh_path)
            if mesh_asset:
                for comp in actor.get_components_by_class(unreal.StaticMeshComponent):
                    comp.set_static_mesh(mesh_asset)
                    break

        made += 1
        log("生成 %s (%s) 于 (%d, %d)" % (label, row, x, y))

    except Exception as e:
        warn("生成 %s 失败: %s" % (label, e))

log("=" * 50)
log("完成：生成 %d / %d 个武器拾取物" % (made, len(PICKUP_SPOTS)))
log("Play 后走到拾取物上即可拿到枪")
log("=" * 50)
