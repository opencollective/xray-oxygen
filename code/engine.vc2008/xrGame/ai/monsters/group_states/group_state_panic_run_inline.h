#pragma once

#define TEMPLATE_SPECIALIZATION template <\
	typename _Object\
>

#define CStateGroupPanicRunAbstract CStateGroupPanicRun<_Object>


namespace GroupStatePanicRunDetails
{
	constexpr float MinUnseenTime = 15000.0f;
	constexpr float MinDistToEnemy = 15.0f;
}

TEMPLATE_SPECIALIZATION
void CStateGroupPanicRunAbstract::initialize()
{
	inherited::initialize();
	
	object->path().prepare_builder		();	
}

TEMPLATE_SPECIALIZATION
void CStateGroupPanicRunAbstract::execute()
{
	object->set_action							(ACT_RUN);
	object->set_state_sound						(MonsterSound::eMonsterSoundPanic);
	object->anim().accel_activate			(eAT_Aggressive);
	object->anim().accel_set_braking			(false);

	Fvector enemy2home = object->Home->get_home_point();
	enemy2home.sub(object->EnemyMan.get_enemy_position());
	enemy2home.normalize_safe();

	object->path().set_target_point(object->Home->get_place_in_max_home_to_direction(enemy2home));

	object->path().set_generic_parameters	();
}
TEMPLATE_SPECIALIZATION
bool CStateGroupPanicRunAbstract::check_completion()
{
	float dist_to_enemy = object->Position().distance_to(object->EnemyMan.get_enemy_position());
	u32 time_delta	= Device.dwTimeGlobal - object->EnemyMan.get_enemy_time_last_seen();

	if (dist_to_enemy < GroupStatePanicRunDetails::MinDistToEnemy)  return false;
	if (time_delta	  < GroupStatePanicRunDetails::MinUnseenTime)	return false;

	return true;
}

#undef DIST_TO_PATH_END
#undef MIN_DIST_TO_ENEMY
#undef TEMPLATE_SPECIALIZATION
#undef CStateGroupPanicRunAbstract
