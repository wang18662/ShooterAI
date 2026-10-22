"""
给玩家一把枪（修正版 v2）

v1 的错误：
    我写了 pickup.set_editor_property("DataTable", ...) 和 ("DataKey", ...)。
    但 BP_ShooterPickup 上根本没有这两个属性。
    真实属性是【WeaponType】，类型 FDataTableRowHandle（结构体，里面才是 DataTable + RowName）。
    所以 v1 两次写入都失败 → WeaponType 为空 → 白块 + 拾取无效。

本版做法：
    1. 找出关卡里 WeaponType 为空的拾取物（就是那些白块）并删除
    2. 在 PlayerStart 前方生成新拾取物
    3. 写入 WeaponType（DataTable + RowName 一起写）
    4. 保存 + 重载关卡 —— 重载会让 OnConstruction 重新执行，
       网格才会从"白块"换成真实的枪模型
       （OnConstruction 只在 Actor 构建时跑一次，光设属性不会触发）

用法：Tools → Execute Python Script... → 选这个文件
"""

import unreal

# ============================================================
# 配置
# ============================================================

LEVEL_PATH = "/Game/Variant_Shooter/Lvl_AI_Full"
PICKUP_PATH = "/Game/Variant_Shooter/Blueprints/Pickups/BP_ShooterPickup"
DT_PATH = "/Game/Variant_Shooter/Blueprints/Pickups/DT_WeaponList"

WEAPON_ROW = "Rifle"       # 可选 Pistol / Rifle / GrenadeLauncher
FORWARD_DIST = 150.0       # PlayerStart 前方多远（cm）
Z_OFFSET = -90.0           # PlayerStart 在胶囊中心，往下放到地面


def log(msg):
    unreal.log("[GiveWeapon] " + str(msg))


def warn(msg):
    unreal.log_warning("[GiveWeapon] " + str(msg))


# ============================================================
# 1. 切换关卡
# ============================================================

level_sub = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
current = str(level_sub.get_current_level())
log("当前关卡 = " + current)

if "Lvl_AI_Full" not in current:
    log("切换到 " + LEVEL_PATH)
    level_sub.load_level(LEVEL_PATH)
    log("切换后 = " + str(level_sub.get_current_level()))


# ============================================================
# 2. 加载资产并校验数据表行
# ============================================================

dt = unreal.EditorAssetLibrary.load_asset(DT_PATH)
pickup_bp = unreal.EditorAssetLibrary.load_asset(PICKUP_PATH)

if dt is None or pickup_bp is None:
    warn("资产加载失败 dt=" + str(dt) + " bp=" + str(pickup_bp))
    raise RuntimeError("asset load failed")

rows = [str(n) for n in unreal.DataTableFunctionLibrary.get_data_table_row_names(dt)]
log("数据表已有行 = " + str(rows))

if WEAPON_ROW not in rows:
    warn("行名 '" + WEAPON_ROW + "' 不存在！可用：" + str(rows))
    raise RuntimeError("row not found")


# ============================================================
# 3. 清理无效拾取物（WeaponType 为空的那些白块）
# ============================================================

actor_sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
all_actors = actor_sub.get_all_level_actors()

pickups = [a for a in all_actors if "ShooterPickup" in a.get_class().get_name()]
log("关卡内拾取物数量 = " + str(len(pickups)))

removed = 0
for p in pickups:
    try:
        wt = p.get_editor_property("weapon_type")
        row_name = str(wt.get_editor_property("row_name"))
        tbl = wt.get_editor_property("data_table")
        valid = bool(tbl) and row_name not in ("None", "")
        log("  拾取物 " + p.get_name() + " → 行名='" + row_name + "' 表=" + str(tbl) + " 有效=" + str(valid))
        if not valid:
            actor_sub.destroy_actor(p)
            removed += 1
            log("    ↳ 已删除（WeaponType 为空）")
    except Exception as e:
        warn("  读取 " + p.get_name() + " 失败：" + str(e))

log("删除无效拾取物 = " + str(removed))


# ============================================================
# 4. 找 PlayerStart
# ============================================================

starts = [a for a in all_actors if "PlayerStart" in a.get_class().get_name()]
if not starts:
    warn("没有 PlayerStart")
    raise RuntimeError("no PlayerStart")

start = starts[0]
loc = start.get_actor_location()
rot = start.get_actor_rotation()
log("PlayerStart = " + str(loc) + "  Yaw=" + str(rot.yaw))

fwd = unreal.Vector(1.0, 0.0, 0.0).rotate_angle_axis(rot.yaw, unreal.Vector(0.0, 0.0, 1.0))
spawn_loc = unreal.Vector(loc.x + fwd.x * FORWARD_DIST,
                          loc.y + fwd.y * FORWARD_DIST,
                          loc.z + Z_OFFSET)
log("生成位置 = " + str(spawn_loc))


# ============================================================
# 5. 生成并写入 WeaponType
# ============================================================

cls = pickup_bp.generated_class()
spawned = actor_sub.spawn_actor_from_class(cls, spawn_loc, unreal.Rotator(0, 0, 0))
if spawned is None:
    raise RuntimeError("spawn failed")

log("已生成 = " + spawned.get_name())

handle = unreal.DataTableRowHandle()
handle.set_editor_property("data_table", dt)
handle.set_editor_property("row_name", WEAPON_ROW)

try:
    spawned.set_editor_property("weapon_type", handle)
    check = spawned.get_editor_property("weapon_type")
    log("写入后读回：data_table=" + str(check.get_editor_property("data_table"))
        + "  row_name=" + str(check.get_editor_property("row_name")))
except Exception as e:
    warn("写入 WeaponType 失败：" + str(e))


# ============================================================
# 6. 保存 + 重载关卡（重载才会让 OnConstruction 重新跑，网格才能更新）
# ============================================================

level_sub.save_current_level()
log("已保存，准备重载关卡以刷新网格...")

level_sub.load_level(LEVEL_PATH)
log("重载完成")

log("=" * 50)
log("完成。验收：")
log("  1. PlayerStart 前方应出现一把枪（不再是白块）")
log("  2. 选中它 → 细节面板 Pickup → Weapon Type 里 Data Table=DT_WeaponList, Row Name=" + WEAPON_ROW)
log("  3. Play → 往前走一步自动拾取 → 右下角出现弹药 HUD → 能开火")
log("=" * 50)
