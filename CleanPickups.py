# -*- coding: utf-8 -*-
"""
删掉关卡里所有武器拾取物。

为什么：拾取物的武器行设不上，只会产生白块和
       "Accessed None ... Current Weapon" 的刷屏报错，
       干扰 AI 测试。AI 验证不需要玩家开枪。

用法：打开 Lvl_AI_Full → Tools → Execute Python Script → 选本文件
"""

import unreal

_eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def log(m):
    unreal.log("[CleanPickups] " + str(m))


removed = []
kept = []

for a in list(_eas.get_all_level_actors()):
    try:
        cn = a.get_class().get_name()
        label = a.get_actor_label()
        # 只删拾取物，别的（包括刷怪点 Spawner）一个都不碰
        if "ShooterPickup" in cn or label.startswith("Pickup_"):
            removed.append("%s (%s)" % (label, cn))
            _eas.destroy_actor(a)
    except Exception:
        pass

log("=" * 56)
log("已删除 %d 个拾取物" % len(removed))
for r in removed:
    log("    - " + r)
log("=" * 56)
log("刷怪点（Spawn_ 开头）没有动，敌人照常生成")
log("现在 Play，玩家空手，但 AI 行为完全可测")
log("=" * 56)
