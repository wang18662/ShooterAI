# -*- coding: utf-8 -*-
"""
修复武器拾取物（第二版）：多策略写入 + 回读验证。

上一版症状：走到拾取物上拿不到枪，报 CurrentWeapon is None。
原因推断：FDataTableRowHandle 是结构体，Python 里赋值容易静默失败——
          看起来设了，实际存的是空句柄，于是 BeginPlay 取不到行 → WeaponClass 为空。

本版做法：
  1. 从数据表读出真实行名（不写死）
  2. 三种写法依次尝试，每次立刻回读验证，取第一个成功的
  3. 拾取点挪开 4 米，避开出生点同位置的初始化时序问题
  4. 全部结果打到 Output Log

用法：打开 Lvl_AI_Full → Tools → Execute Python Script → 选本文件
"""

import unreal

DT_PATH = "/Game/Variant_Shooter/Blueprints/Pickups/DT_WeaponList.DT_WeaponList"
PICKUP_BP = "/Game/Variant_Shooter/Blueprints/Pickups/BP_ShooterPickup.BP_ShooterPickup_C"

MESH = {
    "rifle": "/Game/Weapons/Rifle/Meshes/SM_Rifle.SM_Rifle",
    "pistol": "/Game/Weapons/Pistol/Meshes/SM_Pistol.SM_Pistol",
    "grenade": "/Game/Variant_Shooter/Blueprints/Pickups/Meshes/SM_GrenadeLauncher.SM_GrenadeLauncher",
}

# 拾取点（都离出生点 >4m，避免出生瞬间就触发重叠）
SPOTS = [
    ("A_Mid", -9000, 5200, "rifle"),
    ("A_SE",  -7000, 4500, "rifle"),
    ("H_N",       0, 6800, "pistol"),
    ("B_NE",  -5500, 8500, "rifle"),
    ("F_Mid", -7000, 1000, "pistol"),
    ("C_W",    2200, 5800, "rifle"),
]

_eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def log(m):
    unreal.log("[FixPickups] " + str(m))


def warn(m):
    unreal.log_warning("[FixPickups] " + str(m))


dt = unreal.load_asset(DT_PATH)
pickup_class = unreal.load_class(None, PICKUP_BP)
if not dt or not pickup_class:
    warn("资源加载失败 dt=%s bp=%s" % (dt, pickup_class))
    raise SystemExit

# ---------- 1. 读真实行名 ----------
row_names = []
try:
    row_names = [str(r) for r in unreal.DataTableFunctionLibrary.get_data_table_row_names(dt)]
except Exception as e:
    warn("读行名失败: %s" % e)

log("数据表行名（共 %d）：%s" % (len(row_names), ", ".join(row_names)))
if not row_names:
    raise SystemExit


def pick(keyword):
    for rn in row_names:
        if keyword.lower() in rn.lower():
            return rn
    return None


rows = {"rifle": pick("rifle"), "pistol": pick("pistol"), "grenade": pick("grenade")}
log("匹配: rifle=%s pistol=%s" % (rows["rifle"], rows["pistol"]))


# ---------- 2. 清旧 ----------
removed = 0
for a in list(_eas.get_all_level_actors()):
    try:
        if "ShooterPickup" in a.get_class().get_name():
            _eas.destroy_actor(a)
            removed += 1
    except Exception:
        pass
log("清掉旧拾取物 %d 个" % removed)


# ---------- 3. 三种写入策略 ----------
def try_set_handle(actor, row):
    """依次尝试三种写法，回读验证，返回成功的策略名或 None。"""

    strategies = []

    # A: 空结构体 + 两个字段分别 set
    def strat_a():
        h = unreal.DataTableRowHandle()
        h.set_editor_property("data_table", dt)
        h.set_editor_property("row_name", unreal.Name(row))
        actor.set_editor_property("weapon_type", h)

    # B: 构造时传关键字
    def strat_b():
        h = unreal.DataTableRowHandle(data_table=dt, row_name=unreal.Name(row))
        actor.set_editor_property("weapon_type", h)

    # C: 直接塞 dict（部分版本支持）
    def strat_c():
        actor.set_editor_property("weapon_type", {"data_table": dt, "row_name": row})

    strategies = [("A_逐字段", strat_a), ("B_构造参数", strat_b), ("C_dict", strat_c)]

    for name, fn in strategies:
        try:
            fn()
        except Exception as e:
            log("    策略 %s 抛异常: %s" % (name, e))
            continue

        # 回读
        try:
            wt = actor.get_editor_property("weapon_type")
            got_dt = wt.get_editor_property("data_table")
            got_row = str(wt.get_editor_property("row_name"))
            if got_dt is not None and got_row == row:
                return name, got_row
            log("    策略 %s 回读不匹配: dt=%s row='%s'"
                % (name, got_dt.get_name() if got_dt else "None", got_row))
        except Exception as e:
            log("    策略 %s 回读异常: %s" % (name, e))

    return None, None


# ---------- 4. 生成 ----------
made = 0
for label, x, y, kw in SPOTS:
    row = rows.get(kw) or row_names[0]
    try:
        a = _eas.spawn_actor_from_class(pickup_class, unreal.Vector(x, y, 0),
                                        unreal.Rotator(0, 0, 0))
        a.set_actor_label("Pickup_%s" % label)

        ok_strategy, got = try_set_handle(a, row)

        # 可见模型
        mp = MESH.get(kw)
        mesh_ok = False
        if mp:
            mesh = unreal.load_asset(mp)
            if mesh:
                comps = a.get_components_by_class(unreal.StaticMeshComponent)
                if comps:
                    comps[0].set_static_mesh(mesh)
                    mesh_ok = True

        if ok_strategy:
            made += 1
            log("%-8s row='%s' 写入成功[%s] 回读='%s' 模型=%s"
                % (label, row, ok_strategy, got, "OK" if mesh_ok else "无"))
        else:
            warn("%-8s row='%s' 三种策略全部失败，需要手动设置" % (label, row))

    except Exception as e:
        warn("%s 生成失败: %s" % (label, e))

log("=" * 56)
log("完成：%d / %d 个拾取物写入成功" % (made, len(SPOTS)))
log("请把上面这些 [FixPickups] 行发给我")
log("=" * 56)
