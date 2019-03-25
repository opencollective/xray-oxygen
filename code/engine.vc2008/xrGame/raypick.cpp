#include "StdAfx.h"
#include "raypick.h"
#include "level.h"
#include "script_game_object.h"

CRayPick::CRayPick() 
{
	start_position.set(0,0,0); 
	direction.set(0,0,0); 
	range = 0; 
	flags = collide::rq_target::rqtNone; 
	ignore = nullptr;
};

CRayPick::CRayPick(Fvector& P, Fvector& D, float R, collide::rq_target F, CScriptGameObject* I)
{
	start_position.set(P); 
	direction.set(D);
	range = R; 
	flags = F; 
	ignore = nullptr;
	if (I)
		ignore = smart_cast<CObject*>(&(I->object()));
};

void CRayPick::set_ignore_object(CScriptGameObject* I)
{
    if (I)
    {
        ignore = smart_cast<CObject*>(&(I->object()));
    }
}

bool	CRayPick::query		()
{
	collide::rq_result		R;
	if (Level().ObjectSpace.RayPick		(start_position, direction, range, flags, R, ignore))
	{
		result.set(R);
		return true;
	} 
	else 
		return false;
}