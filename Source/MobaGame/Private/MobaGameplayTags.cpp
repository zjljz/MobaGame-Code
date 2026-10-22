#include "MobaGameplayTags.h"

namespace MobaGameplayTags
{
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Input_Slot_Primary, "Input.Slot.Primary", "主要能力稳定输入槽；精确匹配");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Self, "Cooldown.Self", "无伤害 Self 示例能力的冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Input_Slot_UnitAttack, "Input.Slot.UnitAttack", "单位攻击稳定槽；精确匹配");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Input_Slot_Target, "Input.Slot.Target", "可配置目标投送输入槽");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_UnitAttack, "Event.UnitAttack", "服务器已验证的单位目标能力事件");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_UnitAttack, "Cooldown.UnitAttack", "单位攻击冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Damage, "Damage", "伤害类型根域，规则不将根标签解释为伤害");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Damage_True, "Damage.True", "真实伤害；测试规则精确匹配");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Life_Alive, "State.Life.Alive", "存活生命状态");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Life_Dying, "State.Life.Dying", "正在提交死亡");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Life_Dead, "State.Life.Dead", "死亡等待复活");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Life_Respawning, "State.Life.Respawning", "正在恢复本实体");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Untargetable, "State.Untargetable", "不能被单位目标能力选择");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Invulnerable, "State.Invulnerable", "免受伤害，不等同不可选中");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_CancelOnDeath, "Ability.CancelOnDeath", "死亡取消的能力策略");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Persistent, "Effect.Persistent", "跨死亡保留的示例持续效果");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_RemoveOnDeath, "Effect.RemoveOnDeath", "死亡时移除的效果策略");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Active, "Ability.Active", "可由控制中断的主动能力");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_BasicAttack, "Ability.BasicAttack", "普通攻击分类");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Spell, "Ability.Spell", "主动法术分类");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_ItemHeal, "Cooldown.Item.Heal", "HealingPotion定义共享五秒冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_ItemShield, "Cooldown.Item.Shield", "GuardOrb定义共享五秒冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Damage_Magical, "Damage.Magical", "魔法伤害");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Damage_Magical_Test, "Damage.Magical.Test", "显式映射测试子类");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Damage_Physical, "Damage.Physical", "物理伤害");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Damage_Physical_Test, "Damage.Physical.Test", "显式映射测试子类");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Damage_True_Test, "Damage.True.Test", "显式映射测试子类");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Damage_Unknown, "Damage.Unknown", "拒绝未知伤害测试");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Beneficial, "Effect.Beneficial", "正向效果");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Dispel_Basic, "Effect.Dispel.Basic", "基础驱散");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Dispel_Strong, "Effect.Dispel.Strong", "强驱散");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Dispel_Undispellable, "Effect.Dispel.Undispellable", "不可驱散");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Harmful, "Effect.Harmful", "负向效果");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Shield, "Effect.Shield", "唯一通用护盾池");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Input_Slot_Heal, "Input.Slot.Heal", "自身治疗槽");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Input_Slot_Shield, "Input.Slot.Shield", "自身护盾槽");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Root, "State.Root", "禁止自主移动");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Silence, "State.Silence", "禁止法术");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Slow, "State.Slow", "减速状态");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Stun, "State.Stun", "禁止移动及主动施法");
}
