# -*- coding: utf-8 -*-
"""
拾取物修复（第三版）：直接按已知属性名写入，不再自动探测。

为什么前两版失败：
    1. 用 C++ 版的属性名 "weapon_type" —— 这是蓝图版项目，属性名不同，写入直接抛异常
    2. 用 dir() 自动探测 —— dir() 只返回 Python 属性，
       蓝图新增的变量（UPROPERTY）不在里面，所以永远找不到

本版做法：直接按候选名单逐个试，哪个能读出来且是 DataTableRowHandle 就用哪个。
         候选名单来自资产二进制里实际出现的标识符。

用法：打开 Lvl_AI_Full → Tools → Execute Python Script → 选本文件
      把 [FIX3] 开头的日志发我
"""

import unreal

BP_PATH = "/Game/Variant_Shooter/Blueprints/Pickups/BP_ShooterPickup.BP_ShooterPickup_C"
DT_PATH = "/Game/Variant_Shooter/Blueprints/Pickups/DT_WeaponList.DT_WeaponList"

# 候选属性名（按优先级）。第一个来自资产实测。
ROW_CANDIDATES = ["Weapon", "WeaponType", "WeaponRow", "WeaponData", "weapon"]
MESH_CANDIDATES = ["StaticMesh", "Mesh", "PickupMesh"]

SPOTS = [
    ("A_South", -9000, 5200, "rifle"),
    ("A_SE",    -7000, 4500, "rifle"),
    ("H_North",     0, 6800, "pistol"),
    ("B_NE",    -5500, 8500, "rifle"),
    ("F_Mid",   -7000, 1000, "pistol"),
    ("C_West",   2200, 5800, "rifle"),
]

MESH = {
    "rifle": "/Game/Weapons/Rifle/Meshes/SM_Rifle.SM_Rifle",
    "pistol": "/Game/Weapons/Pistol/Meshes/SM_Pistol.SM_Pistol",
}

_eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def log(m):
    unreal.log("[FIX3] " + str(m))


def warn(m):
    unreal.log_warning("[FIX3] " + str(m))


pickup_class = unreal.load_class(None, BP_PATH)
dt = unreal.load_asset(DT_PATH)
if not pickup_class or not dt:
    warn("资源加载失败")
    raise SystemExit

# ---------- 1. 在临时 Actor 上找出真实属性名 ----------

probe = _eas.spawn_actor_from_class(pickup_class, unreal.Vector(0, 0, -5000),
                                    unreal.Rotator(0, 0, 0))
if not probe:
    warn("无法生成探测用 Actor")
    raise SystemExit

row_prop = None
mesh_prop = None

log("-" * 52)
log("探测属性名：")
for cand in ROW_CANDIDATES:
    try:
        val = probe.get_editor_property(cand)
        log("    %-14s 存在, 类型=%s" % (cand, type(val).__name__))
        if isinstance(val, unreal.DataTableRowHandle) and row_prop is None:
            row_prop = cand
    except Exception as e:
        log("    %-14s 不存在 (%s)" % (cand, type(e).__name__))

for cand in MESH_CANDIDATES:
    try:
        val = probe.get_editor_property(cand)
        log("    %-14s 存在, 类型=%s" % (cand, type(val).__name__))
        if type(val).__name__ == "StaticMesh" and mesh_prop is None:
            mesh_prop = cand
    except Exception as e:
        log("    %-14s 不存在 (%s)" % (cand, type(e).__name__))

log("-" * 52)
log("结论：武器行属性 = %s   模型属性 = %s" % (row_prop, mesh_prop))

_eas.destroy_actor(probe)

if not row_prop:
    warn("所有候选名都试过了，都不对。")
    warn("请打开 BP_ShooterPickup，在左侧【变量】列表里看武器类型那个变量叫什么，告诉我")
    raise SystemExit

# ---------- 2. 行名 ----------

row_names = [str(r) for r in unreal.DataTableFunctionLibrary.get_data_table_row_names(dt)]
log("数据表行名: %s" % ", ".join(row_names))


def pick(kw):
    for rn in row_names:
        if kw.lower() in rn.lower():
            return rn
    return row_names[0]


row_map = {"rifle": pick("rifle"), "pistol": pick("pistol")}

# ---------- 3. 清旧 ----------

removed = 0
for a in list(_eas.get_all_level_actors()):
    try:
        if "Pickup" in a.get_class().get_name():
            _eas.destroy_actor(a)
            removed += 1
    except Exception:
        pass
log("清掉旧拾取物 %d 个" % removed)

# ---------- 4. 重建 ----------

made = 0
for label, x, y, kw in SPOTS:
    row = row_map[kw]
    try:
        a = _eas.spawn_actor_from_class(pickup_class, unreal.Vector(x, y, 0),
                                        unreal.Rotator(0, 0, 0))
        a.set_actor_label("Pickup_%s" % label)

        handle = unreal.DataTableRowHandle()
        handle.set_editor_property("data_table", dt)
        handle.set_editor_property("row_name", unreal.Name(row))
        a.set_editor_property(row_prop, handle)

        # 回读
        got = a.get_editor_property(row_prop)
        gdt = got.get_editor_property("data_table")
        grow = str(got.get_editor_property("row_name"))
        ok = (gdt is not None) and (grow == row)

        if mesh_prop:
            m = unreal.load_asset(MESH[kw])
            if m:
                try:
                    a.set_editor_property(mesh_prop, m)
                except Exception:
                    pass

        if ok:
            made += 1
            log("%-8s %s='%s' 回读OK" % (label, row_prop, grow))
        else:
            warn("%-8s 回读不匹配 dt=%s row='%s'" % (label, gdt, grow))

    except Exception as e:
        warn("%s 失败: %s" % (label, e))

log("=" * 52)
log("完成 %d / %d" % (made, len(SPOTS)))
log("=" * 52)
