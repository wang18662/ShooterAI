# -*- coding: utf-8 -*-
"""
诊断：打印拾取物身上真实的变量名和值。

为什么需要：BP_ShooterPickup 是纯蓝图资产，
    武器类型那个变量到底叫什么名字，只能从运行时读出来，猜不出来。

用法：打开 Lvl_AI_Full（或任意有关卡的关卡）
      Tools -> Execute Python Script -> 选本文件
      把 Output Log 里 [Diag] 开头的行全部发给我
"""

import unreal

_eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def log(m):
    unreal.log("[Diag] " + str(m))


log("=" * 60)

# ---------- 1. 当前关卡里所有拾取物 ----------
pickups = []
for a in _eas.get_all_level_actors():
    try:
        cn = a.get_class().get_name()
        if "Pickup" in cn:
            pickups.append(a)
    except Exception:
        pass

log("当前关卡拾取物数量: %d" % len(pickups))

for i, a in enumerate(pickups):
    log("-" * 50)
    log("[%d] label=%s  class=%s" % (i, a.get_actor_label(), a.get_class().get_name()))

    # 列出所有和 weapon / table / row / mesh 相关的属性
    keys = []
    for name in dir(a):
        if name.startswith("_"):
            continue
        low = name.lower()
        if any(k in low for k in ("weapon", "table", "row", "mesh", "pickup", "respawn")):
            keys.append(name)

    if not keys:
        log("    （没有匹配到 weapon/table/row 相关属性名）")
    for name in sorted(keys):
        try:
            val = a.get_editor_property(name)
            log("    %-24s = %s" % (name, val))
        except Exception as e:
            log("    %-24s <读不到: %s>" % (name, e))


# ---------- 2. 数据表真实行名 ----------
log("-" * 50)
dt = unreal.load_asset("/Game/Variant_Shooter/Blueprints/Pickups/DT_WeaponList.DT_WeaponList")
if dt:
    try:
        rows = unreal.DataTableFunctionLibrary.get_data_table_row_names(dt)
        log("DT_WeaponList 行名: %s" % ", ".join([str(r) for r in rows]))
    except Exception as e:
        log("读行名失败: %s" % e)
else:
    log("DT_WeaponList 加载失败")


# ---------- 3. 拾取物蓝图的所有蓝图变量名 ----------
log("-" * 50)
try:
    cd = unreal.load_object(None, "/Game/Variant_Shooter/Blueprints/Pickups/BP_ShooterPickup.BP_ShooterPickup_C")
    cdo = unreal.get_default_object(cd) if cd else None
    if cdo:
        names = []
        for name in dir(cdo):
            if name.startswith("_"):
                continue
            low = name.lower()
            if any(k in low for k in ("weapon", "table", "row", "respawn", "mesh")):
                names.append(name)
        log("BP_ShooterPickup 类默认值里的相关属性名: %s" % ", ".join(sorted(names)))
    else:
        log("取不到类默认值对象")
except Exception as e:
    log("枚举类默认值失败: %s" % e)

log("=" * 60)
log("把以上 [Diag] 行发给我")
log("=" * 60)
