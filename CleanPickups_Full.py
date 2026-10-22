"""
清理 Lvl_AI_Full 里的无效拾取物（白块）。

背景：
    BP_ShooterPickup 的 WeaponType 是 protected UPROPERTY，
    Python 反射读不到也写不进（已诊断确认），所以脚本生成的拾取物
    全部是空配置 → 显示白块、拾取无效。

    这些废弃实例需要清掉，改用「从 Lvl_ArenaShooter 手动复制」的方式。

本脚本安全点：
    - 只删 BP_ShooterPickup 类的 actor，不碰其他任何东西
    - 删除前逐个打印名字、类、世界坐标
    - 删完打印剩余数量
    - 不保存关卡（让你先看一眼，确认没问题再自己 Ctrl+S）

用法：Tools → Execute Python Script...
"""

import unreal

LEVEL_PATH = "/Game/Variant_Shooter/Lvl_AI_Full"


def log(m):
    unreal.log("[CleanPickup] " + str(m))


level_sub = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
cur = str(level_sub.get_current_level())
log("当前关卡 = " + cur)

if "Lvl_AI_Full" not in cur:
    log("切换到 " + LEVEL_PATH)
    level_sub.load_level(LEVEL_PATH)

actor_sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
all_actors = actor_sub.get_all_level_actors()

pickups = [a for a in all_actors if "ShooterPickup" in a.get_class().get_name()]
log("找到拾取物 " + str(len(pickups)) + " 个：")

for i, p in enumerate(pickups):
    loc = p.get_actor_location()
    log("  [" + str(i) + "] " + p.get_name()
        + "  (" + p.get_class().get_name() + ")"
        + "  @ (" + str(round(loc.x)) + ", " + str(round(loc.y)) + ", " + str(round(loc.z)) + ")")

deleted = 0
for p in pickups:
    try:
        name = p.get_name()
        actor_sub.destroy_actor(p)
        deleted += 1
        log("  ✘ 已删除 " + name)
    except Exception as e:
        log("  删除失败 " + p.get_name() + " : " + str(e))

log("共删除 " + str(deleted) + " 个")

# 复查
remain = [a for a in actor_sub.get_all_level_actors()
          if "ShooterPickup" in a.get_class().get_name()]
log("剩余拾取物 = " + str(len(remain)))

log("=" * 50)
log("清理完成。下一步（手动操作，1 分钟）：")
log("  1. 打开 /Game/Variant_Shooter/Lvl_ArenaShooter")
log("  2. 在世界大纲里展开该关卡，找到 BP_ShooterPickup 实例")
log("  3. 选中一个，Ctrl+C")
log("  4. 切回 Lvl_AI_Full，Ctrl+V")
log("  5. 拖到 PlayerStart 前方地面，Ctrl+S 保存")
log("     （复制会带上 WeaponType 配置，白块会直接变成枪模型）")
log("=" * 50)
