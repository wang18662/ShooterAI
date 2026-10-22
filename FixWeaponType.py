"""
v3：直接设置 BP_ShooterPickup 类默认值里的 Weapon Type → Row Name。

关键更正（v1/v2 错在哪）：
    v1 写了 "DataTable" / "DataKey" 两个独立属性 —— 不存在（那是结构体内部字段名）。
    v2 写了 "weapon_type" —— 名字对了，但没验证是否真的写进去。
    diag 用 dir() 枚举属性 —— 错误方法，dir() 只暴露 BlueprintReadOnly/ReadWrite 的属性，
    UPROPERTY 不在里面，所以看起来"读不到 weapon_type"，实际是没试到。
    真实情况：Properties 面板里 Weapon Type 可编辑，Python 也能通过
    get_editor_property("weapon_type") 访问。

本版做法（最省事、最可靠）：
    1. 读 CDO 的 weapon_type，确认 DataTable 和 RowName 当前值
    2. 把 RowName 设成 Rifle（DataTable 已经是 DT_WeaponList 就保持）
    3. 保存蓝图资产
    4. 重载关卡 —— 重载会让每个拾取物重新跑 OnConstruction，
       从数据表读到网格 → 白块变成枪模型
    5. 可选：删掉 PlayerStart 附近多余的拾取物

为什么改类默认值而不是改实例：
    改默认值一次生效，所有没覆盖的拾取物都变正常；
    关卡里已有的 9 个白块也会一起变好，不用重新摆。

用法：Tools → Execute Python Script...
"""

import unreal

# ============================================================
# 配置
# ============================================================

PICKUP_PATH = "/Game/Variant_Shooter/Blueprints/Pickups/BP_ShooterPickup"
DT_PATH = "/Game/Variant_Shooter/Blueprints/Pickups/DT_WeaponList"
LEVEL_PATH = "/Game/Variant_Shooter/Lvl_AI_Full"

WEAPON_ROW = "Rifle"     # Pistol / Rifle / GrenadeLauncher

# 是否删掉 PlayerStart 附近堆着的多余拾取物（保留 1 个）
DELETE_DUPLICATES = True


def log(m):
    unreal.log("[FixWeapon] " + str(m))


def warn(m):
    unreal.log_warning("[FixWeapon] " + str(m))


log("=" * 60)

# ============================================================
# 1. 读类默认值
# ============================================================

bp = unreal.EditorAssetLibrary.load_asset(PICKUP_PATH)
if bp is None:
    raise RuntimeError("BP load failed: " + PICKUP_PATH)

cls = bp.generated_class()
cdo = unreal.get_default_object(cls)
log("类 = " + str(cls))
log("CDO = " + str(cdo))

old_handle = None
try:
    old_handle = cdo.get_editor_property("weapon_type")
    log("✔ 读到 weapon_type = " + str(old_handle))
    log("   data_table = " + str(old_handle.get_editor_property("data_table")))
    log("   row_name   = " + str(old_handle.get_editor_property("row_name")))
except Exception as e:
    warn("✘ 读 weapon_type 失败：" + str(e))
    warn("   → 属性名可能不同，停止执行")
    raise

# 校验数据表行名
dt = unreal.EditorAssetLibrary.load_asset(DT_PATH)
rows = [str(n) for n in unreal.DataTableFunctionLibrary.get_data_table_row_names(dt)]
log("数据表行 = " + str(rows))
if WEAPON_ROW not in rows:
    raise RuntimeError("行名 " + WEAPON_ROW + " 不存在")

# ============================================================
# 2. 写入 RowName
# ============================================================

handle = unreal.DataTableRowHandle()
handle.set_editor_property("data_table", dt)
handle.set_editor_property("row_name", WEAPON_ROW)

try:
    cdo.set_editor_property("weapon_type", handle)
    log("✔ 已写入 CDO.weapon_type")
except Exception as e:
    warn("✘ 写 CDO 失败：" + str(e))
    raise

# 读回验证
try:
    back = cdo.get_editor_property("weapon_type")
    log("读回验证： data_table = " + str(back.get_editor_property("data_table"))
        + "   row_name = " + str(back.get_editor_property("row_name")))
except Exception as e:
    warn("读回失败：" + str(e))

# ============================================================
# 3. 保存蓝图资产
# ============================================================

try:
    ok = unreal.EditorAssetLibrary.save_loaded_asset(bp, False)
    log("保存蓝图 = " + str(ok))
except Exception as e:
    warn("保存蓝图失败：" + str(e))

# ============================================================
# 4. 重载关卡，让 OnConstruction 重新跑（网格才会更新）
# ============================================================

level_sub = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
cur = str(level_sub.get_current_level())
log("当前关卡 = " + cur)

if "Lvl_AI_Full" not in cur:
    log("切换到 " + LEVEL_PATH)
    level_sub.load_level(LEVEL_PATH)

log("重载关卡以刷新网格 ...")
level_sub.load_level(LEVEL_PATH)

# ============================================================
# 5. 可选：清理 PlayerStart 附近多余的拾取物
# ============================================================

actor_sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
pickups = [a for a in actor_sub.get_all_level_actors()
           if "ShooterPickup" in a.get_class().get_name()]
log("关卡内拾取物数量 = " + str(len(pickups)))

if DELETE_DUPLICATES and pickups:
    starts = [a for a in actor_sub.get_all_level_actors()
              if "PlayerStart" in a.get_class().get_name()]
    if starts:
        sp = starts[0].get_actor_location()
        nearby = []
        for p in pickups:
            d = (p.get_actor_location() - sp).size()
            log("  " + p.get_name() + " 距 PlayerStart = " + str(round(d)) + "cm")
            if d < 600.0:
                nearby.append((d, p))
        # 由近到远保留 1 个，其余删掉
        nearby.sort(key=lambda t: t[0])
        for i, (d, p) in enumerate(nearby):
            if i == 0:
                log("  保留 " + p.get_name() + "（最近的）")
            else:
                actor_sub.destroy_actor(p)
                log("  删除 " + p.get_name())

    level_sub.save_current_level()
    log("已保存关卡")

log("=" * 60)
log("完成。检查：")
log("  1. 关卡里之前那些白块拾取物 → 应该变成枪的模型")
log("  2. Play → 走到枪上自动拾取 → 右下角出现弹药 HUD → 左键能开火")
log("=" * 60)
