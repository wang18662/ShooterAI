# -*- coding: utf-8 -*-
"""
诊断 + 修复：玩家血量 & 分离参数。

解决两件事：
  1. 把玩家血量改成极高（不死，方便长时间测试）
  2. 打印每个敌人的 Pursue Steering 组件实际参数，
     查清为什么「最近同伴」显示 -1

为什么用候选名逐个试：dir() 列不出蓝图/C++ 的 UPROPERTY，
    只能直接 get_editor_property("名字") 试。

用法：打开 Lvl_AI_Full → Tools → Execute Python Script → 选本文件
      把 [DIAG] 开头的日志发我
"""

import unreal

_eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

HP_CANDIDATES = ["MaxHP", "MaxHealth", "Health", "MaxHp", "MaxHitPoints", "MaxHealthPoints"]
SEP_CANDIDATES = [
    "SeparationRadius", "SeparationStrength", "MaxSeparationOffset",
    "bUseSeparation", "NearestAllyDistance",
    "StopDistance", "bAutoPilot", "bCheckLineOfSight",
    "bUseMemory", "MemoryDuration", "ScanDuration",
]


def log(m):
    unreal.log("[DIAG] " + str(m))


def warn(m):
    unreal.log_warning("[DIAG] " + str(m))


# ============================================================
# 1. 打印关卡里所有 Pawn 的关键参数（同时用于定位分离问题）
# ============================================================

log("=" * 60)
log("关卡里的 Pawn：")

pawns = []
for a in _eas.get_all_level_actors():
    try:
        if isinstance(a, unreal.Pawn):
            pawns.append(a)
    except Exception:
        pass

log("共 %d 个 Pawn" % len(pawns))

for p in pawns:
    log("-" * 50)
    log("  %s  class=%s" % (p.get_actor_label(), p.get_class().get_name()))

    # 找 Pursue Steering 组件
    comps = []
    try:
        comps = p.get_components_by_class(unreal.ActorComponent)
    except Exception:
        pass

    sep_comp = None
    for c in comps:
        try:
            if "Pursue" in c.get_class().get_name():
                sep_comp = c
                break
        except Exception:
            pass

    if sep_comp:
        log("    [Pursue Steering] 找到")
        for name in SEP_CANDIDATES:
            try:
                v = sep_comp.get_editor_property(name)
                log("        %-22s = %s" % (name, v))
            except Exception:
                pass
    else:
        log("    （没有 Pursue Steering 组件）")

    # 血量
    for name in HP_CANDIDATES:
        try:
            v = p.get_editor_property(name)
            log("    HP: %-16s = %s" % (name, v))
            break
        except Exception:
            pass


# ============================================================
# 2. 把玩家血量改成极高
# ============================================================

log("=" * 60)
log("尝试提高玩家血量：")

BIG_HP = 999999.0

# 方式 A：改蓝图类默认值（下次 Play 生效，需要保存蓝图）
bp_class = unreal.load_object(None,
    "/Game/Variant_Shooter/Blueprints/BP_ShooterCharacter.BP_ShooterCharacter_C")

if bp_class:
    try:
        cdo = unreal.get_default_object(bp_class)
        done = False
        for name in HP_CANDIDATES:
            try:
                old = cdo.get_editor_property(name)
                cdo.set_editor_property(name, BIG_HP)
                new = cdo.get_editor_property(name)
                log("    蓝图默认值 %s: %s -> %s" % (name, old, new))
                done = True
                break
            except Exception:
                continue

        if done:
            # 改完必须编译 + 保存，否则重启编辑器就丢了
            bp_asset = unreal.load_asset(
                "/Game/Variant_Shooter/Blueprints/BP_ShooterCharacter")
            if bp_asset:
                try:
                    unreal.BlueprintEditorLibrary.compile_blueprint(bp_asset)
                    log("    蓝图已编译")
                except Exception as e:
                    warn("    编译蓝图失败: %s" % e)
                try:
                    unreal.EditorAssetLibrary.save_loaded_asset(bp_asset)
                    log("    蓝图已保存")
                except Exception as e:
                    warn("    保存蓝图失败（请手动 Ctrl+S）: %s" % e)
        else:
            warn("    蓝图默认值里没找到血量候选名")
            warn("    请手动打开 BP_ShooterCharacter，在细节面板搜 Health")
    except Exception as e:
        warn("    改蓝图默认值失败: %s" % e)
else:
    warn("    加载 BP_ShooterCharacter 失败")

# 方式 B：改关卡里已有的玩家实例（如果 Play 中可能不适用，但编辑器里有实例就改）
for p in pawns:
    try:
        if "ShooterCharacter" in p.get_class().get_name():
            for name in HP_CANDIDATES:
                try:
                    p.set_editor_property(name, BIG_HP)
                    log("    关卡实例 %s 的 %s 设为 %g"
                        % (p.get_actor_label(), name, BIG_HP))
                    break
                except Exception:
                    continue
    except Exception:
        pass

log("=" * 60)
log("完成。注意：蓝图默认值的修改需要在蓝图编辑器里 Compile + Save 才持久化")
log("更省事的做法见下方说明（控制台命令 / 改 MaxHP）")
log("=" * 60)
