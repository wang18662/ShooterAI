# -*- coding: utf-8 -*-
"""
最终版：自动探测真实属性名，不再猜。

思路：
    1. 读 BP_ShooterPickup 的类默认值，找出【类型是 DataTableRowHandle】的那个属性
       —— 不管它叫 Weapon / WeaponType / WeaponRow，都能找到
    2. 顺便找出【类型是 StaticMesh】的属性，设成枪的模型（避免只剩白底板）
    3. 用探测到的属性名生成拾取物，回读验证
    4. 全程打日志，失败会说清为什么

用法：打开 Lvl_AI_Full → Tools → Execute Python Script → 选本文件
      然后把 [FIX] 开头的日志发我
"""

import unreal

BP_PATH = "/Game/Variant_Shooter/Blueprints/Pickups/BP_ShooterPickup.BP_ShooterPickup_C"
DT_PATH = "/Game/Variant_Shooter/Blueprints/Pickups/DT_WeaponList.DT_WeaponList"

MESH = {
    "rifle": "/Game/Weapons/Rifle/Meshes/SM_Rifle.SM_Rifle",
    "pistol": "/Game/Weapons/Pistol/Meshes/SM_Pistol.SM_Pistol",
}

SPOTS = [
    ("A_South", -9000, 5200, "rifle"),
    ("A_SE",    -7000, 4500, "rifle"),
    ("H_North",     0, 6800, "pistol"),
    ("B_NE",    -5500, 8500, "rifle"),
    ("F_Mid",   -7000, 1000, "pistol"),
    ("C_West",   2200, 5800, "rifle"),
]

_eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def log(m):
    unreal.log("[FIX] " + str(m))


def warn(m):
    unreal.log_warning("[FIX] " + str(m))


# ============================================================
# 1. 探测属性名
# ============================================================

bp_class = unreal.load_object(None, BP_PATH)
if not bp_class:
    warn("加载 BP_ShooterPickup 失败: " + BP_PATH)
    raise SystemExit

cdo = None
try:
    cdo = unreal.get_default_object(bp_class)
except Exception as e:
    warn("取类默认值失败: %s" % e)

if not cdo:
    raise SystemExit

row_prop = None        # 存 DataTableRowHandle 的属性名
mesh_prop = None       # 存 StaticMesh 的属性名
all_props = []

for name in dir(cdo):
    if name.startswith("_"):
        continue
    try:
        val = cdo.get_editor_property(name)
    except Exception:
        continue

    type_name = type(val).__name__
    all_props.append("%s:%s" % (name, type_name))

    if isinstance(val, unreal.DataTableRowHandle) and row_prop is None:
        row_prop = name
    if type_name == "StaticMesh" and mesh_prop is None:
        mesh_prop = name

log("=" * 56)
log("探测结果：")
log("    武器行属性名 = %s" % (row_prop if row_prop else "!!! 没找到"))
log("    模型属性名   = %s" % (mesh_prop if mesh_prop else "（没有，跳过改模型）"))
log("-" * 56)
log("全部属性（供参考）：")
for p in sorted(all_props):
    log("    " + p)
log("=" * 56)

if not row_prop:
    warn("没找到 DataTableRowHandle 类型的属性，无法自动修复。请把上面『全部属性』发我")
    raise SystemExit


# ============================================================
# 2. 读行名
# ============================================================

dt = unreal.load_asset(DT_PATH)
row_names = []
try:
    row_names = [str(r) for r in unreal.DataTableFunctionLibrary.get_data_table_row_names(dt)]
except Exception as e:
    warn("读行名失败: %s" % e)

log("DT_WeaponList 行名: %s" % ", ".join(row_names))
if not row_names:
    raise SystemExit


def pick(kw):
    for rn in row_names:
        if kw.lower() in rn.lower():
            return rn
    return row_names[0]


row_map = {"rifle": pick("rifle"), "pistol": pick("pistol")}


# ============================================================
# 3. 清旧
# ============================================================

removed = 0
for a in list(_eas.get_all_level_actors()):
    try:
        cn = a.get_class().get_name()
        if "Pickup" in cn:
            _eas.destroy_actor(a)
            removed += 1
    except Exception:
        pass
log("清掉旧拾取物 %d 个" % removed)


# ============================================================
# 4. 重建并验证
# ============================================================

made = 0
for label, x, y, kw in SPOTS:
    row = row_map[kw]
    try:
        a = _eas.spawn_actor_from_class(unreal.load_class(None, BP_PATH),
                                        unreal.Vector(x, y, 0),
                                        unreal.Rotator(0, 0, 0))
        a.set_actor_label("Pickup_%s" % label)

        # 用探测到的属性名写入
        handle = unreal.DataTableRowHandle()
        handle.set_editor_property("data_table", dt)
        handle.set_editor_property("row_name", unreal.Name(row))
        a.set_editor_property(row_prop, handle)

        # 回读验证
        got = a.get_editor_property(row_prop)
        got_dt = got.get_editor_property("data_table")
        got_row = str(got.get_editor_property("row_name"))
        ok_row = (got_dt is not None) and (got_row == row)

        # 设模型
        ok_mesh = False
        if mesh_prop:
            mesh = unreal.load_asset(MESH[kw])
            if mesh:
                try:
                    a.set_editor_property(mesh_prop, mesh)
                    ok_mesh = True
                except Exception as e:
                    log("    设模型失败: %s" % e)

        if ok_row:
            made += 1
            log("%-8s %s='%s' 回读OK  模型=%s"
                % (label, row_prop, got_row, "OK" if ok_mesh else "未设"))
        else:
            warn("%-8s 写入后回读不匹配: dt=%s row='%s'"
                 % (label, got_dt.get_name() if got_dt else "None", got_row))

    except Exception as e:
        warn("%s 失败: %s" % (label, e))

log("=" * 56)
log("完成：%d / %d 成功" % (made, len(SPOTS)))
log("Play 后走到 B 区的枪上试试（A 区那把在出生点南边 13m）")
log("=" * 56)
