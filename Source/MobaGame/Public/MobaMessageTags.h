#pragma once

#include "Experience/ExperienceTypes.h"
#include "Hero/HeroLifeState.h"
#include "GMP/GMPUtils.h"
#include "GMP/GMPHubOpt.h"

// 对象作用域为 UExperienceSubsystem；唯一参数是本次提交后的 FExperienceSnapshot 值。
// 保留 MSGKEY 在调用点展开的类型检查及文件/行追踪，不长期保存临时 typed key。
#define MOBA_MESSAGE_EXPERIENCE_CHANGED MSGKEY("Moba.Experience.Changed")

// 对象作用域为 UExperienceFeatureSubsystem；参数为 URL 值和已提交的 EExperienceFeatureState。
#define MOBA_MESSAGE_EXPERIENCE_FEATURE_CHANGED MSGKEY("Moba.Experience.FeatureChanged")

// Hero 对象作用域；唯一参数为已提交的 FHeroLifeSnapshot 值，不驱动权威结算。
#define MOBA_MESSAGE_HERO_LIFE_CHANGED MSGKEY("Moba.Hero.LifeChanged")

// 属性组件对象作用域；唯一参数为已提交／复制的累计经验 int64，通知后只读重取。
#define MOBA_MESSAGE_HERO_PROGRESSION_CHANGED MSGKEY("Moba.Hero.ProgressionChanged")

// 目标ASC对象作用域；参数为来源Context与已提交的FCombatSettlementResult值，仅服务器本地事实通知。
#define MOBA_MESSAGE_COMBAT_SETTLED MSGKEY("Moba.Combat.Settled")

// GameInstance 的 UGameFlowSubsystem 对象作用域；参数为已提交的 FGameFlowSnapshot 值。
#define MOBA_MESSAGE_GAME_FLOW_CHANGED MSGKEY("Moba.Session.GameFlowChanged")

// 各自对象作用域；参数是提交或复制完成后的版本，只触发展示重读。
#define MOBA_MESSAGE_ABILITY_LOADOUT_CHANGED MSGKEY("Moba.Ability.LoadoutChanged")
#define MOBA_MESSAGE_ITEM_INVENTORY_CHANGED MSGKEY("Moba.Item.InventoryChanged")
#define MOBA_MESSAGE_PARTICIPANT_ECONOMY_CHANGED MSGKEY("Moba.Participant.EconomyChanged")
