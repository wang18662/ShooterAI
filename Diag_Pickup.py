"""
诊断 v2：一次把 BP_ShooterPickup 的所有事实摆出来。

比 v1 强的地方：
    v1 只试了 13 个猜的名字。这次直接遍历类的【全部属性】，
    连我没猜到的也能列出来，一次拿全，不用再来一轮。

四个部分：
    1. 类继承链（确定 BP_ShooterPickup 到底继承谁）
    2. 类的全部属性清单（含类型），从中筛出"像武器配置"的
    3. 关卡内拾取物实例的实际值
    4. 数据表内容

只读，不做任何修改。

用法：Tools → Execute Python Script...
"""

import unreal

LEVEL_PATH = "/Game/Variant_Shooter/Lvl_AI_Full"
PICKUP_PATH = "/Game/Variant_Shooter/Blueprints/Pickups/BP_ShooterPickup"
DT_PATH = "/Game/Variant_Shooter/Blueprints/Pickups/DT_WeaponList"


def log(m):
    unreal.log("[Diag] " + str(m))


def warn(m):
    unreal.log_warning("[Diag] " + str(m))


log("=" * 64)

bp = unreal.EditorAssetLibrary.load_asset(PICKUP_PATH)
if bp is None:
    warn("BP 加载失败")
    raise RuntimeError("bp load failed")

cls = bp.generated_class()
log("BP 生成的类 = " + str(cls))

# ---------- 1. 继承链 ----------
log("-" * 64)
log("【1】继承链")
c = cls
depth = 0
while c is not None and depth < 15:
    log("  [" + str(depth) + "] " + str(c.get_name()))
    try:
        nxt = c.get_super_class()
    except Exception as e:
        warn("  取父类失败: " + str(e))
        break
    if nxt is None or nxt == c:
        break
    c = nxt
    depth += 1

# ---------- 2. 全部属性 ----------
log("-" * 64)
log("【2】类的全部属性（Python 可见的）")

cdo = unreal.get_default_object(cls)
all_props = []

# dir() 只给 Python 属性，所以用反射拿 UPROPERTY
try:
    names = unreal.SystemLibrary.get_class_display_name(cls)
    log("  类显示名 = " + str(names))
except Exception:
    pass

# 方法 A：遍历 dir，再逐个 get_editor_property（覆盖面最广）
candidates = set()
for attr in dir(cdo):
    if attr.startswith("_"):
        continue
    # 跳过明显是函数/内置的
    if attr in ("get_name", "get_class", "get_outer", "get_path_name",
                "get_editor_property", "set_editor_property",
                "get_full_name", "get_fname", "get_package", "get_world"):
        continue
    candidates.add(attr)

log("  dir() 得到 " + str(len(candidates)) + " 个候选，逐个尝试读取：")

readable = {}
for name in sorted(candidates):
    try:
        v = cdo.get_editor_property(name)
        readable[name] = v
    except Exception:
        pass

log("  ✔ 可读属性共 " + str(len(readable)) + " 个：")
for name in sorted(readable.keys()):
    v = readable[name]
    log("      " + name + "  =  " + str(v) + "   (" + type(v).__name__ + ")")

# 关键字过滤，突出武器相关
log("  --- 疑似武器/数据相关 ---")
KEY = ("weapon", "data", "row", "table", "mesh", "class", "respawn", "pickup")
for name in sorted(readable.keys()):
    if any(k in name.lower() for k in KEY):
        log("      ★ " + name + "  =  " + str(readable[name]))

# ---------- 3. 关卡实例 ----------
log("-" * 64)
log("【3】关卡里的拾取物实例")

level_sub = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
cur = str(level_sub.get_current_level())
log("  当前关卡 = " + cur)
if "Lvl_AI_Full" not in cur:
    log("  切换到 " + LEVEL_PATH)
    level_sub.load_level(LEVEL_PATH)

actor_sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
pickups = [a for a in actor_sub.get_all_level_actors()
           if "ShooterPickup" in a.get_class().get_name()]

log("  拾取物数量 = " + str(len(pickups)))

# 只打印武器相关的，避免刷屏
weapon_props = [n for n in readable.keys()
                if any(k in n.lower() for k in ("weapon", "data", "row", "table"))]

for p in pickups:
    log("  --- " + p.get_name() + "  (" + p.get_class().get_name() + ")")
    for name in sorted(weapon_props):
        try:
            log("      " + name + " = " + str(p.get_editor_property(name)))
        except Exception as e:
            log("      " + name + " 读取失败: " + str(e))

# ---------- 4. 数据表 ----------
log("-" * 64)
log("【4】DT_WeaponList")
dt = unreal.EditorAssetLibrary.load_asset(DT_PATH)
if dt is None:
    warn("  数据表加载失败")
else:
    rows = [str(n) for n in unreal.DataTableFunctionLibrary.get_data_table_row_names(dt)]
    log("  行 = " + str(rows))

log("=" * 64)
log("把 [Diag] 开头的行全部贴出来")
