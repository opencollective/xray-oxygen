#include "stdafx.h"
#include "../../xrEngine/igame_persistent.h"
#include "../../xrEngine/irenderable.h"
#include "../xrRender/FBasicVisual.h"
#include <ppl.h>
#include <concurrent_vector.h>

const	float	tweak_COP_initial_offs = 1200.f;
const	float	tweak_ortho_xform_initial_offs = 1000.f;	//. ?
const	float	tweak_guaranteed_range = 20.f;	//. ?

const	float	MAP_SIZE_START = 6.f;
const	float	MAP_GROW_FACTOR = 4.f;

//////////////////////////////////////////////////////////////////////////
// tables to calculate view-frustum bounds in world space
// note: D3D uses [0..1] range for Z
static Fvector3		corners[8] = {
	{ -1, -1,  0 },		{ -1, -1, +1},
	{ -1, +1, +1 },		{ -1, +1,  0},
	{ +1, +1, +1 },		{ +1, +1,  0},
	{ +1, -1, +1},		{ +1, -1,  0}
};
static int			facetable[6][4] = {
	{ 6, 7, 5, 4 },		{ 1, 0, 7, 6 },
	{ 1, 2, 3, 0 },		{ 3, 2, 4, 5 },
	// near and far planes
	{ 0, 3, 5, 7 },		{  1, 6, 4, 2 },
};
//////////////////////////////////////////////////////////////////////////
#include "../xrRender/Frustum.inl"
#define DW_AS_FLT(DW) (*(FLOAT*)&(DW))
#define FLT_SIGN(F) ((FLT_AS_DW(F) & 0x80000000L))
xr_vector<Fbox, xalloc<Fbox>> s_casters;

void CRender::render_sun()
{
	PIX_EVENT(render_sun);

	light* fuckingsun = (light*)Lights.sun._get();
	D3DXMATRIX m_LightViewProj;

	// calculate view-frustum bounds in world space
	Fmatrix	ex_project, ex_full, ex_full_inverse;
	{
		float _far_ = min(ps_r_sun_far, Environment().CurrentEnv->far_plane);
		ex_project.build_projection(deg2rad(Device.fFOV/* *Device.fASPECT*/), Device.fASPECT, VIEWPORT_NEAR, _far_);
		ex_full.mul(ex_project, Device.mView);
		D3DXMatrixInverse((D3DXMATRIX*)&ex_full_inverse, nullptr, (D3DXMATRIX*)&ex_full);
	}

	// Compute volume(s) - something like a frustum for infinite directional light
	// Also compute virtual light position and sector it is inside
	CFrustum					cull_frustum;
	xr_vector<Fplane> cull_planes;
	Fvector3 cull_COP;
	CSector* cull_sector;
	Fmatrix  cull_xform;
	{
		
			FPU::m64r();
			// Lets begin from base frustum
			Fmatrix fullxform_inv = ex_full_inverse;
			DumbConvexVolume<false> hull;
			{
				hull.points.reserve(8);
				for (int p = 0; p < 8; p++)
				{
					Fvector3 xf = wform(fullxform_inv, corners[p]);
					hull.points.push_back(xf);
				}
				for (int plane = 0; plane < 6; plane++)
				{
					hull.polys.emplace_back();
					for (int pt = 0; pt < 4; pt++)
						hull.polys.back().points.push_back(facetable[plane][pt]);
				}
			}
		hull.compute_caster_model(cull_planes, fuckingsun->direction);

		// Search for default sector - assume "default" or "outdoor" sector is the largest one
		//. hack: need to know real outdoor sector
		CSector*	largest_sector = nullptr;
		float		largest_sector_vol = 0;
		for (IRender_Sector* pSector : Sectors)
		{
			CSector* S = (CSector*)pSector;
			dxRender_Visual* V = S->root();
			float vol = V->vis.box.getvolume();

			if (vol > largest_sector_vol) 
			{
				largest_sector_vol = vol;
				largest_sector = S;
			}
		}
		cull_sector = largest_sector;

		// COP - 100 km away
		cull_COP.mad(Device.vCameraPosition, fuckingsun->direction, -tweak_COP_initial_offs);

		// Create frustum for query
		cull_frustum._clear();
		for (u32 p = 0; p < cull_planes.size(); p++)
			cull_frustum._add(cull_planes[p]);

		// Create approximate ortho-xform
		// view: auto find 'up' and 'right' vectors
		Fmatrix mdir_View, mdir_Project;
		Fvector L_dir, L_up, L_right, L_pos;
		L_pos.set(fuckingsun->position);
		L_dir.set(fuckingsun->direction).normalize();
		L_up.set(0, 1, 0);

		if (_abs(L_up.dotproduct(L_dir)) > .99f)
			L_up.set(0, 0, 1);

		L_right.crossproduct(L_up, L_dir).normalize();
		L_up.crossproduct(L_dir, L_right).normalize();
		mdir_View.build_camera_dir(L_pos, L_dir, L_up);

		// projection: box
		Fbox frustum_bb;
		frustum_bb.invalidate();
		for (u32 it = 0; it < 8; it++)
		{
			Fvector	xf = wform(mdir_View, hull.points[it]);
			frustum_bb.modify(xf);
		}

		Fbox&	bb = frustum_bb;
		bb.grow(EPS);
		D3DXMatrixOrthoOffCenterLH((D3DXMATRIX*)&mdir_Project, bb.min.x, bb.max.x, bb.min.y, bb.max.y, bb.min.z - tweak_ortho_xform_initial_offs, bb.max.z);
		// full-xform
		cull_xform.mul(mdir_Project, mdir_View);
		FPU::m24r();
	}

	// Begin SMAP-render
	{
		HOM.Disable();
		phase = PHASE_SMAP;
		r_pmask(true, RImplementation.o.Tshadows);
	}

	// Fill the database
	s_casters.reserve(main_coarse_structure.size());
	r_dsgraph_render_subspace(cull_sector, &cull_frustum, cull_xform, cull_COP, TRUE);

	// IGNORE PORTALS
	if (ps_r_flags.test(R_FLAG_SUN_IGNORE_PORTALS))
	{
		for (IRender_Sector* pSector : Sectors)
		{
			CSector* S = (CSector*)pSector;
			dxRender_Visual* root = S->root();

			set_Frustum(&cull_frustum);
			add_Geometry(root);
		}
	}

	//	Prepare to interact with D3DX code

	const D3DXMATRIX&	m_View = *((D3DXMATRIX*)(&Device.mView));
	const D3DXMATRIX&	m_Projection = *((D3DXMATRIX*)&ex_project);
	const D3DXVECTOR3	m_lightDir = -D3DXVECTOR3(fuckingsun->direction.x, fuckingsun->direction.y, fuckingsun->direction.z);

	//  these are the limits specified by the physical camera
	//  gamma is the "tilt angle" between the light and the view direction.
	float m_fCosGamma = m_lightDir.x * Device.mView._13 +
		m_lightDir.y * Device.mView._23 +
		m_lightDir.z * Device.mView._33;
	float m_fTSM_Delta = ps_r_sun_tsm_projection;

	// Compute REAL sheared xform based on receivers/casters information
	FPU::m64r();
	if (_abs(m_fCosGamma) < 0.99f && ps_r_flags.test(R_FLAG_SUN_TSM))
	{
		//  get the near and the far plane (points) in eye space.
		D3DXVECTOR3 frustumPnts[8];

		Frustum eyeFrustum(&m_Projection);  // autocomputes all the extrema points

		for (int i = 0; i < 4; i++)
		{
			frustumPnts[i] = eyeFrustum.pntList[(i << 1)];       // far plane
			frustumPnts[i + 4] = eyeFrustum.pntList[(i << 1) | 0x1]; // near plane
		}

		//   we need to transform the eye into the light's post-projective space.
		//   however, the sun is a directional light, so we first need to find an appropriate
		//   rotate/translate matrix, before constructing an ortho projection.
		//   this matrix is a variant of "light space" from LSPSMs, with the Y and Z axes permuted

		D3DXVECTOR3 leftVector, upVector, viewVector;
		const D3DXVECTOR3	eyeVector(0.f, 0.f, -1.f);  //  eye is always -Z in eye space

		//  code copied straight from BuildLSPSMProjectionMatrix
		D3DXVec3TransformNormal(&upVector, &m_lightDir, &m_View);  // lightDir is defined in eye space, so xform it
		D3DXVec3Cross(&leftVector, &upVector, &eyeVector);
		D3DXVec3Normalize(&leftVector, &leftVector);
		D3DXVec3Cross(&viewVector, &upVector, &leftVector);

		D3DXMATRIX lightSpaceBasis;
		{
			lightSpaceBasis._11 = leftVector.x; lightSpaceBasis._12 = viewVector.x; lightSpaceBasis._13 = -upVector.x; lightSpaceBasis._14 = 0.f;
			lightSpaceBasis._21 = leftVector.y; lightSpaceBasis._22 = viewVector.y; lightSpaceBasis._23 = -upVector.y; lightSpaceBasis._24 = 0.f;
			lightSpaceBasis._31 = leftVector.z; lightSpaceBasis._32 = viewVector.z; lightSpaceBasis._33 = -upVector.z; lightSpaceBasis._34 = 0.f;
			lightSpaceBasis._41 = 0.f;          lightSpaceBasis._42 = 0.f;          lightSpaceBasis._43 = 0.f;        lightSpaceBasis._44 = 1.f;
		}
		//  rotate the view frustum into light space
		D3DXVec3TransformCoordArray(frustumPnts, sizeof(D3DXVECTOR3), frustumPnts, sizeof(D3DXVECTOR3), &lightSpaceBasis, sizeof(frustumPnts) / sizeof(D3DXVECTOR3));

		
		//  build an off-center ortho projection that translates and scales the eye frustum's 3D AABB to the unit cube
		BoundingBox frustumBox(frustumPnts, sizeof(frustumPnts) / sizeof(D3DXVECTOR3));

		//  also - transform the shadow caster bounding boxes into light projective space.  we want to translate along the Z axis so that
		//  all shadow casters are in front of the near plane.
		D3DXVECTOR2 depthbounds = BuildTSMProjectionMatrix_caster_depth_bounds(lightSpaceBasis);

		float min_z = min(depthbounds.x, frustumBox.minPt.z);
		float max_z = max(depthbounds.y, frustumBox.maxPt.z);

		if (min_z <= 1.f)	//?
		{
			D3DXMATRIX lightSpaceTranslate;
			D3DXMatrixTranslation(&lightSpaceTranslate, 0.f, 0.f, -min_z + 1.f);
			max_z = -min_z + max_z + 1.f;
			min_z = 1.f;
			D3DXMatrixMultiply(&lightSpaceBasis, &lightSpaceBasis, &lightSpaceTranslate);
			D3DXVec3TransformCoordArray(frustumPnts, sizeof(D3DXVECTOR3), frustumPnts, sizeof(D3DXVECTOR3), &lightSpaceTranslate, sizeof(frustumPnts) / sizeof(D3DXVECTOR3));
			frustumBox = BoundingBox(frustumPnts, sizeof(frustumPnts) / sizeof(D3DXVECTOR3));
		}

		D3DXMATRIX lightSpaceOrtho;
		D3DXMatrixOrthoOffCenterLH(&lightSpaceOrtho,frustumBox.minPt.x, frustumBox.maxPt.x, frustumBox.minPt.y, frustumBox.maxPt.y, min_z, max_z);

		//  transform the view frustum by the new matrix
		D3DXVec3TransformCoordArray(frustumPnts, sizeof(D3DXVECTOR3), frustumPnts, sizeof(D3DXVECTOR3), &lightSpaceOrtho, sizeof(frustumPnts) / sizeof(D3DXVECTOR3));

		DirectX::XMFLOAT2 centerPts[2];
		//  near plane
		centerPts[0].x = 0.25f * (frustumPnts[4].x + frustumPnts[5].x + frustumPnts[6].x + frustumPnts[7].x);
		centerPts[0].y = 0.25f * (frustumPnts[4].y + frustumPnts[5].y + frustumPnts[6].y + frustumPnts[7].y);
		//  far plane
		centerPts[1].x = 0.25f * (frustumPnts[0].x + frustumPnts[1].x + frustumPnts[2].x + frustumPnts[3].x);
		centerPts[1].y = 0.25f * (frustumPnts[0].y + frustumPnts[1].y + frustumPnts[2].y + frustumPnts[3].y);

		DirectX::XMFLOAT2 centerOrig =
		{
			(centerPts[0].x + centerPts[1].x) * 0.5f,
			(centerPts[0].y + centerPts[1].y) * 0.5f
		};

		D3DXMATRIX trapezoid_space;

		D3DXMATRIX xlate_center(
			1.f, 0.f, 0.f, 0.f,
			0.f, 1.f, 0.f, 0.f,
			0.f, 0.f, 1.f, 0.f,
			-centerOrig.x, -centerOrig.y, 0.f, 1.f);

		DirectX::XMFLOAT2 center_dirl =
		{
			centerPts[0].x - centerPts[1].x,
			centerPts[0].y - centerPts[1].y
		};

		float half_center_len = XRay::Math::XMFloat2Len(center_dirl);
		float x_len = centerPts[1].x - centerOrig.x;
		float y_len = centerPts[1].y - centerOrig.y;

		float cos_theta = x_len / half_center_len;
		float sin_theta = y_len / half_center_len;

		D3DXMATRIX rot_center(cos_theta, -sin_theta, 0.f, 0.f,
			sin_theta, cos_theta, 0.f, 0.f,
			0.f, 0.f, 1.f, 0.f,
			0.f, 0.f, 0.f, 1.f);

		//  this matrix transforms the center line to y=0.
		//  since Top and Base are orthogonal to Center, we can skip computing the convex hull, and instead
		//  just find the view frustum X-axis extrema.  The most negative is Top, the most positive is Base
		//  Point Q (trapezoid projection point) will be a point on the y=0 line.
		D3DXMatrixMultiply(&trapezoid_space, &xlate_center, &rot_center);
		D3DXVec3TransformCoordArray(frustumPnts, sizeof(D3DXVECTOR3), frustumPnts, sizeof(D3DXVECTOR3), &trapezoid_space, sizeof(frustumPnts) / sizeof(D3DXVECTOR3));
		BoundingBox frustumAABB2D(frustumPnts, sizeof(frustumPnts) / sizeof(DirectX::XMFLOAT3));

		float x_scale = max(_abs(frustumAABB2D.maxPt.x), _abs(frustumAABB2D.minPt.x));
		float y_scale = max(_abs(frustumAABB2D.maxPt.y), _abs(frustumAABB2D.minPt.y));
		x_scale = 1.f / x_scale;
		y_scale = 1.f / y_scale;

		//  maximize the area occupied by the bounding box
		D3DXMATRIX scale_center(
			x_scale, 0.f, 0.f, 0.f,
			0.f, y_scale, 0.f, 0.f,
			0.f, 0.f, 1.f, 0.f,
			0.f, 0.f, 0.f, 1.f
		);

		D3DXMatrixMultiply(&trapezoid_space, &trapezoid_space, &scale_center);

		//  scale the frustum AABB up by these amounts (keep all values in the same space)
		frustumAABB2D.minPt.x *= x_scale;
		frustumAABB2D.maxPt.x *= x_scale;
		frustumAABB2D.minPt.y *= y_scale;
		frustumAABB2D.maxPt.y *= y_scale;

		//  compute eta.
		float lambda = frustumAABB2D.maxPt.x - frustumAABB2D.minPt.x;
		float delta_proj = m_fTSM_Delta * lambda;	//focusPt.x - frustumAABB2D.minPt.x;
		const float xi = -0.6f;					// - 0.6f;  // 80% line
		float		eta = (lambda*delta_proj*(1.f + xi)) / (lambda*(1.f - xi) - 2.f*delta_proj);

		//  compute the projection point a distance eta from the top line.  this point is on the center line, y=0
		DirectX::XMFLOAT2 projectionPtQ(frustumAABB2D.maxPt.x + eta, 0.f);

		//  find the maximum slope from the projection point to any point in the frustum.  this will be the
		//  projection field-of-view
		float max_slope = -1e32f;
		float min_slope = 1e32f;

		for (D3DXVECTOR3 & frustumPnt : frustumPnts)
		{
			DirectX::XMFLOAT2 tmp(frustumPnt.x*x_scale, frustumPnt.y*y_scale);
			float x_dist = tmp.x - projectionPtQ.x;
			if (!(ALMOST_ZERO(tmp.y) || ALMOST_ZERO(x_dist)))
			{
				max_slope = max(max_slope, tmp.y / x_dist);
				min_slope = min(min_slope, tmp.y / x_dist);
			}
		}

		float xn = eta;
		float xf = lambda + eta;

		D3DXMATRIX ptQ_xlate(-1.f, 0.f, 0.f, 0.f,
			0.f, 1.f, 0.f, 0.f,
			0.f, 0.f, 1.f, 0.f,
			projectionPtQ.x, 0.f, 0.f, 1.f);
		D3DXMatrixMultiply(&trapezoid_space, &trapezoid_space, &ptQ_xlate);

		//  this shear balances the "trapezoid" around the y=0 axis (no change to the projection pt position)
		//  since we are redistributing the trapezoid, this affects the projection field of view (shear_amt)
		float shear_amt = (max_slope + _abs(min_slope))*0.5f - max_slope;
		max_slope = max_slope + shear_amt;

		D3DXMATRIX trapezoid_shear
		(
			1.f, shear_amt, 0.f, 0.f,
			0.f, 1.f, 0.f, 0.f,
			0.f, 0.f, 1.f, 0.f,
			0.f, 0.f, 0.f, 1.f
		);

		D3DXMatrixMultiply(&trapezoid_space, &trapezoid_space, &trapezoid_shear);

		float z_aspect = (frustumBox.maxPt.z - frustumBox.minPt.z) / (frustumAABB2D.maxPt.y - frustumAABB2D.minPt.y);

		//  perform a 2DH projection to 'unsqueeze' the top line.
		D3DXMATRIX trapezoid_projection
		(
			xf / (xf - xn),			0.f,				0.f,							1.f,
			0.f,					1.f / max_slope,	0.f,							0.f,
			0.f,					0.f,				1.f / (z_aspect*max_slope),		0.f,
			-xn * xf / (xf - xn),	0.f,				0.f,							0.f
		);

		D3DXMatrixMultiply(&trapezoid_space, &trapezoid_space, &trapezoid_projection);

		//  the x axis is compressed to [0..1] as a result of the projection, so expand it to [-1,1]
		D3DXMATRIX  biasedScaleX
		(
			2.f, 0.f, 0.f, 0.f,
			0.f, 1.f, 0.f, 0.f,
			0.f, 0.f, 1.f, 0.f,
			-1.f, 0.f, 0.f, 1.f
		);

		D3DXMatrixMultiply(&trapezoid_space, &trapezoid_space, &biasedScaleX);

		D3DXMatrixMultiply(&m_LightViewProj, &m_View, &lightSpaceBasis);
		D3DXMatrixMultiply(&m_LightViewProj, &m_LightViewProj, &lightSpaceOrtho);
		D3DXMatrixMultiply(&m_LightViewProj, &m_LightViewProj, &trapezoid_space);
	}
	else
	{
		m_LightViewProj = *((D3DXMATRIX*)(&cull_xform));
	}
	FPU::m24r();

	// perform "refit" or "focusing" on relevant
	if (ps_r_flags.test(R_FLAG_SUN_FOCUS))
	{
		FPU::m64r();

		// create clipper
		DumbClipper	view_clipper;
		Fmatrix&	xform = *(Fmatrix*)(&m_LightViewProj);
		view_clipper.frustum.CreateFromMatrix(ex_full, FRUSTUM_P_ALL);

		for (int p = 0; p < view_clipper.frustum.p_count; p++)
		{
			Fplane& P = view_clipper.frustum.planes[p];
			view_clipper.planes.emplace_back(P.n.x, P.n.y, P.n.z, P.d);
		}

		// 
		Fbox3 b_casters, b_receivers;
		Fvector3 pt;

		// casters
		b_casters.invalidate();
		for (auto & s_caster : s_casters)
		{
			for (int e = 0; e < 8; e++)
			{
				s_caster.getpoint(e, pt);
				pt = wform(xform, pt);
				b_casters.modify(pt);
			}
		}

		// receivers
		b_receivers.invalidate();
		b_receivers = view_clipper.clipped_AABB(main_coarse_structure, xform);
		Fmatrix x_project, x_full, x_full_inverse;
		{
			x_project.build_projection(deg2rad(Device.fFOV/* *Device.fASPECT*/), Device.fASPECT, ps_r_sun_near, ps_r_sun_near + tweak_guaranteed_range);
			x_project.build_projection(deg2rad(Device.fFOV/* *Device.fASPECT*/), Device.fASPECT, VIEWPORT_NEAR, ps_r_sun_near + tweak_guaranteed_range);
			x_full.mul(x_project, Device.mView);
			D3DXMatrixInverse((D3DXMATRIX*)&x_full_inverse, nullptr, (D3DXMATRIX*)&x_full);
		}

		for (auto corner : corners)
		{
			pt = wform(x_full_inverse, corner);	// world space
			pt = wform(xform, pt);						// trapezoid space
			b_receivers.modify(pt);
		}

		// some tweaking
		b_casters.grow(EPS);
		b_receivers.grow(EPS);

		// because caster points are from coarse representation only allow to "shrink" box, not grow
		// that is the same as if we first clip casters by frustum
		if (b_receivers.min.x < -1)	b_receivers.min.x = -1;
		if (b_receivers.min.y < -1)	b_receivers.min.y = -1;
		if (b_casters.min.z < 0)		b_casters.min.z = 0;
		if (b_receivers.max.x > +1)	b_receivers.max.x = +1;
		if (b_receivers.max.y > +1)	b_receivers.max.y = +1;
		if (b_casters.max.z > +1)		b_casters.max.z = +1;

		float boxWidth = b_receivers.max.x - b_receivers.min.x;
		float boxHeight = b_receivers.max.y - b_receivers.min.y;
		//  the divide by two's cancel out in the translation, but included for clarity
		float boxX = (b_receivers.max.x + b_receivers.min.x) / 2.f;
		float boxY = (b_receivers.max.y + b_receivers.min.y) / 2.f;
		D3DXMATRIX trapezoidUnitCube(
			2.f / boxWidth, 0.f, 0.f, 0.f,
			0.f, 2.f / boxHeight, 0.f, 0.f,
			0.f, 0.f, 1.f, 0.f,
			-2.f*boxX / boxWidth, -2.f*boxY / boxHeight, 0.f, 1.f);

		D3DXMatrixMultiply(&m_LightViewProj, &m_LightViewProj, &trapezoidUnitCube);
		FPU::m24r();
	}

	// Finalize & Cleanup
	fuckingsun->X.D.combine = *(Fmatrix*)(&m_LightViewProj);
	main_coarse_structure.clear();
	s_casters.clear();

	// Render shadow-map
	//. !!! We should clip based on shrinked frustum (again)
	{
		bool	bNormal = !mapNormalPasses[0][0].empty() || !mapMatrixPasses[0][0].empty();
		bool	bSpecial = !mapNormalPasses[1][0].empty() || !mapMatrixPasses[1][0].empty() || !mapSorted.empty();
		if (bNormal || bSpecial) {
			Target->phase_smap_direct(fuckingsun, SE_SUN_FAR);
			RCache.set_xform_world(Fidentity);
			RCache.set_xform_view(Fidentity);
			RCache.set_xform_project(fuckingsun->X.D.combine);
			r_dsgraph_render_graph(0);
			fuckingsun->X.D.transluent = FALSE;
			if (bSpecial) {
				fuckingsun->X.D.transluent = TRUE;
				Target->phase_smap_direct_tsh(fuckingsun, SE_SUN_FAR);
				r_dsgraph_render_graph(1);			// normal level, secondary priority
				r_dsgraph_render_sorted();			// strict-sorted geoms
			}
		}
	}

	// End SMAP-render
	{
		r_pmask(true, false);
	}

	// Accumulate
	Target->phase_accumulator();

#ifdef USE_DX11
	if (Target->use_minmax_sm_this_frame())
	{
		PIX_EVENT(SE_SUN_FAR_MINMAX_GENERATE);
		Target->create_minmax_SM();
	}
#endif

	PIX_EVENT(SE_SUN_FAR);
	Target->accum_direct(SE_SUN_FAR);

	// Restore XForms
	RCache.set_xform_world(Fidentity);
	RCache.set_xform_view(Device.mView);
	RCache.set_xform_project(Device.mProject);
}

void CRender::render_sun_near()
{
	light* fuckingsun = (light*)Lights.sun._get();

	// calculate view-frustum bounds in world space
	Fmatrix	ex_project, ex_full, ex_full_inverse;
	{
		ex_project.build_projection(deg2rad(Device.fFOV/* *Device.fASPECT*/), Device.fASPECT, VIEWPORT_NEAR, ps_r_sun_near);
		ex_full.mul(ex_project, Device.mView);
		D3DXMatrixInverse((D3DXMATRIX*)&ex_full_inverse, nullptr, (D3DXMATRIX*)&ex_full); //ex_full_inverse.Inverse(nullptr, ex_full);
	}

	// Compute volume(s) - something like a frustum for infinite directional light
	// Also compute virtual light position and sector it is inside
	CFrustum							   cull_frustum;
	xr_vector<Fplane> cull_planes;
	Fvector3							   cull_COP;
	CSector*							   cull_sector;
	Fmatrix							   cull_xform;
	{
		FPU::m64r();
		// Lets begin from base frustum
		Fmatrix fullxform_inv = ex_full_inverse;
#ifdef	_DEBUG
		typedef		DumbConvexVolume<true>	t_volume;
#else
		using t_volume =		DumbConvexVolume<false>;
#endif
		t_volume					hull;
		{
			hull.points.reserve(9);
			for (Fvector3 corner : corners) {
				Fvector3				xf = wform(fullxform_inv, corner);
				hull.points.push_back(xf);
			}
			for (auto & plane : facetable) 
			{
				hull.polys.emplace_back();
				for (int pt : plane)
					hull.polys.back().points.push_back(pt);
			}
		}
		hull.compute_caster_model(cull_planes, fuckingsun->direction);
#ifdef	_DEBUG
		for (u32 it = 0; it < cull_planes.size(); it++)
			RImplementation.Target->dbg_addplane(cull_planes[it], 0xffffffff);
#endif

		// Search for default sector - assume "default" or "outdoor" sector is the largest one
		//. hack: need to know real outdoor sector
		CSector*	largest_sector = nullptr;
		float		largest_sector_vol = 0;
		for (IRender_Sector* pSector : Sectors)
		{
			CSector* S = (CSector*)pSector;
			dxRender_Visual* V = S->root();
			float vol = V->vis.box.getvolume();

			if (vol > largest_sector_vol)
			{
				largest_sector_vol = vol;
				largest_sector = S;
			}
		}
		cull_sector = largest_sector;

		// COP - 100 km away
		cull_COP.mad(Device.vCameraPosition, fuckingsun->direction, -tweak_COP_initial_offs);

		// Create frustum for query
		cull_frustum._clear();
		for (Fplane & cull_plane : cull_planes)
			cull_frustum._add(cull_plane);

		// Create approximate ortho-xform
		// view: auto find 'up' and 'right' vectors
		Fmatrix mdir_View, mdir_Project;
		Fvector L_dir, L_up, L_right, L_pos;
		L_pos.set(fuckingsun->position);
		L_dir.set(fuckingsun->direction).normalize();
		L_right.set(1, 0, 0);

		if (_abs(L_right.dotproduct(L_dir)) > .99f)
			L_right.set(0, 0, 1);

		L_up.crossproduct(L_dir, L_right).normalize();
		L_right.crossproduct(L_up, L_dir).normalize();
		mdir_View.build_camera_dir(L_pos, L_dir, L_up);

		//	Simple
		Fbox	frustum_bb;
		frustum_bb.invalidate();

		for (int it = 0; it < 8; it++)
		{
			Fvector	xf = wform(mdir_View, hull.points[it]);
			frustum_bb.modify(xf);
		}
		Fbox& bb = frustum_bb;
		bb.grow(EPS);

		D3DXMatrixOrthoOffCenterLH((D3DXMATRIX*)&mdir_Project, bb.min.x, bb.max.x, bb.min.y, bb.max.y, bb.min.z - tweak_ortho_xform_initial_offs, bb.max.z);
		// build viewport xform

		float	view_dim = float(RImplementation.o.smapsize);
		Fmatrix	m_viewport =
		{
			view_dim / 2.f,	0.0f,				0.0f,		0.0f,
			0.0f,			-view_dim / 2.f,		0.0f,		0.0f,
			0.0f,			0.0f,				1.0f,		0.0f,
			view_dim / 2.f,	view_dim / 2.f,		0.0f,		1.0f
		};
		Fmatrix m_viewport_inv;
		D3DXMatrixInverse((D3DXMATRIX*)&m_viewport_inv, nullptr, (D3DXMATRIX*)&m_viewport);


		// snap view-position to pixel
		cull_xform.mul(mdir_Project, mdir_View);
		Fvector cam_proj = wform(cull_xform, Device.vCameraPosition);
		Fvector	cam_pixel = wform(m_viewport, cam_proj);
		cam_pixel.x = floorf(cam_pixel.x);
		cam_pixel.y = floorf(cam_pixel.y);
		Fvector cam_snapped = wform(m_viewport_inv, cam_pixel);
		Fvector diff;		diff.sub(cam_snapped, cam_proj);
		Fmatrix adjust;		adjust.translate(diff);
		cull_xform.mul(adjust, cull_xform);

		// calculate scissor
		Fbox		scissor;	scissor.invalidate();
		Fmatrix		scissor_xf;
		scissor_xf.mul(m_viewport, cull_xform);
		for (int it = 0; it < 9; it++) {
			Fvector	xf = wform(scissor_xf, hull.points[it]);
			scissor.modify(xf);
		}
		s32		limit = RImplementation.o.smapsize - 1;
		fuckingsun->X.D.minX = clampr(iFloor(scissor.min.x), 0, limit);
		fuckingsun->X.D.maxX = clampr(iCeil(scissor.max.x), 0, limit);
		fuckingsun->X.D.minY = clampr(iFloor(scissor.min.y), 0, limit);
		fuckingsun->X.D.maxY = clampr(iCeil(scissor.max.y), 0, limit);

		// full-xform
		FPU::m24r();
	}

	// Begin SMAP-render
	{
		bool	bSpecialFull = !mapNormalPasses[1][0].empty() || !mapMatrixPasses[1][0].empty() || !mapSorted.empty();
		VERIFY(!bSpecialFull);
		HOM.Disable();
		phase = PHASE_SMAP;
		if (RImplementation.o.Tshadows)	r_pmask(true, true);
		else							r_pmask(true, false);
	}

	// Fill the database
	r_dsgraph_render_subspace(cull_sector, &cull_frustum, cull_xform, cull_COP, TRUE);

	// Finalize & Cleanup
	fuckingsun->X.D.combine = cull_xform;

	// Render shadow-map
	//. !!! We should clip based on shrinked frustum (again)
	{
		bool	bNormal = !mapNormalPasses[0][0].empty() || !mapMatrixPasses[0][0].empty();
		bool	bSpecial = !mapNormalPasses[1][0].empty() || !mapMatrixPasses[1][0].empty() || !mapSorted.empty();
		if (bNormal || bSpecial) {
			Target->phase_smap_direct(fuckingsun, SE_SUN_NEAR);
			RCache.set_xform_world(Fidentity);
			RCache.set_xform_view(Fidentity);
			RCache.set_xform_project(fuckingsun->X.D.combine);
			r_dsgraph_render_graph(0);
			if (ps_r_flags.test(R_FLAG_DETAIL_SHADOW))
				Details->Render();
			fuckingsun->X.D.transluent = FALSE;
			if (bSpecial) {
				fuckingsun->X.D.transluent = TRUE;
				Target->phase_smap_direct_tsh(fuckingsun, SE_SUN_NEAR);
				r_dsgraph_render_graph(1);			// normal level, secondary priority
				r_dsgraph_render_sorted();			// strict-sorted geoms
			}
		}
	}

	// End SMAP-render
	{
		r_pmask(true, false);
	}

	// Accumulate
	Target->phase_accumulator();

#ifdef USE_DX11
	if (Target->use_minmax_sm_this_frame())
	{
		PIX_EVENT(SE_SUN_FAR_MINMAX_GENERATE);
		Target->create_minmax_SM();
	}
#endif

	PIX_EVENT(SE_SUN_NEAR);
	Target->accum_direct(SE_SUN_NEAR);

	// Restore XForms
	RCache.set_xform_world(Fidentity);
	RCache.set_xform_view(Device.mView);
	RCache.set_xform_project(Device.mProject);
}

void CRender::render_sun_filtered()
{
#ifndef USE_DX11
	if (!RImplementation.o.sunfilter)	return;
	Target->phase_accumulator();
	PIX_EVENT(SE_SUN_LUMINANCE);
	Target->accum_direct(SE_SUN_LUMINANCE);
#endif
}

void CRender::init_cacades()
{
	u32 cascade_count = 3;
	m_sun_cascades.resize(cascade_count);

	float fBias = -0.0000025f;

	m_sun_cascades[0].reset_chain = true;
	m_sun_cascades[0].size = 9;
	m_sun_cascades[0].bias = m_sun_cascades[0].size*fBias;

	m_sun_cascades[1].size = 40;
	m_sun_cascades[1].bias = m_sun_cascades[1].size*fBias;

	m_sun_cascades[2].size = 160;
	m_sun_cascades[2].bias = m_sun_cascades[2].size*fBias;
}

void CRender::render_sun_cascades()
{
	bool b_need_to_render_sunshafts = RImplementation.Target->need_to_render_sunshafts();
	bool last_cascade_chain_mode = m_sun_cascades.back().reset_chain;
	if (b_need_to_render_sunshafts)
		m_sun_cascades[m_sun_cascades.size() - 1].reset_chain = true;

	for (u32 i = 0; i < m_sun_cascades.size(); ++i)
		render_sun_cascade(i);

	if (b_need_to_render_sunshafts)
		m_sun_cascades[m_sun_cascades.size() - 1].reset_chain = last_cascade_chain_mode;
}

void CRender::render_sun_cascade(u32 cascade_ind)
{
	light* fuckingsun = (light*)Lights.sun._get();

	// calculate view-frustum bounds in world space
	Fmatrix ex_project, ex_full, ex_full_inverse;
	{
		ex_project = Device.mProject;
		ex_full.mul(ex_project, Device.mView);
		D3DXMatrixInverse((D3DXMATRIX*)&ex_full_inverse, nullptr, (D3DXMATRIX*)&ex_full);
	}

	// Compute volume(s) - something like a frustum for infinite directional light
	// Also compute virtual light position and sector it is inside
	xr_vector<Fplane> cull_planes;
	CFrustum					cull_frustum;
	Fvector3					cull_COP;
	CSector*					cull_sector;
	Fmatrix					cull_xform;
	{
		FPU::m64r();
		// Lets begin from base frustum
		Fmatrix		fullxform_inv = ex_full_inverse;
#ifdef	_DEBUG
		typedef		DumbConvexVolume<true>	t_volume;
#else
		using t_volume =		DumbConvexVolume<false>;
#endif

		//******************************* Need to be placed after cuboid built **************************
		// Search for default sector - assume "default" or "outdoor" sector is the largest one
		//. hack: need to know real outdoor sector
		CSector*	largest_sector = nullptr;
		float		largest_sector_vol = 0;

		for (IRender_Sector* pSector : Sectors)
		{
			CSector* S = (CSector*)pSector;
			dxRender_Visual* V = S->root();
			float vol = V->vis.box.getvolume();

			if (vol > largest_sector_vol)
			{
				largest_sector_vol = vol;
				largest_sector = S;
			}
		}
		cull_sector = largest_sector;

		// COP - 100 km away
		cull_COP.mad(Device.vCameraPosition, fuckingsun->direction, -tweak_COP_initial_offs);

		// Create approximate ortho-xform
		// view: auto find 'up' and 'right' vectors
		Fmatrix						mdir_View, mdir_Project;
		Fvector						L_dir, L_up, L_right, L_pos;
		L_pos.set(fuckingsun->position);
		L_dir.set(fuckingsun->direction).normalize();
		L_right.set(1, 0, 0);					if (_abs(L_right.dotproduct(L_dir)) > .99f)	L_right.set(0, 0, 1);
		L_up.crossproduct(L_dir, L_right).normalize();
		L_right.crossproduct(L_up, L_dir).normalize();
		mdir_View.build_camera_dir(L_pos, L_dir, L_up);



		//////////////////////////////////////////////////////////////////////////
#ifdef	_DEBUG
		typedef		FixedConvexVolume<true>		t_cuboid;
#else
		using t_cuboid =		FixedConvexVolume<false>;
#endif

		t_cuboid light_cuboid;
		{
			// Initialize the first cascade rays, then each cascade will initialize rays for next one.
			if (cascade_ind == 0 || m_sun_cascades[cascade_ind].reset_chain)
			{
				Fvector3				near_p, edge_vec;
				for (int p = 0; p < 4; p++)
				{
					near_p = wform(fullxform_inv, corners[facetable[4][p]]);

					edge_vec = wform(fullxform_inv, corners[facetable[5][p]]);
					edge_vec.sub(near_p);
					edge_vec.normalize();

					light_cuboid.view_frustum_rays.push_back(sun::ray(near_p, edge_vec));
				}
			}
			else
				light_cuboid.view_frustum_rays = m_sun_cascades[cascade_ind].rays;

			light_cuboid.view_ray.P = Device.vCameraPosition;
			light_cuboid.view_ray.D = Device.vCameraDirection;
			light_cuboid.light_ray.P = L_pos;
			light_cuboid.light_ray.D = L_dir;
		}

		// THIS NEED TO BE A CONSTATNT
		Fplane light_top_plane;
		light_top_plane.build_unit_normal(L_pos, L_dir);
		float dist = light_top_plane.classify(Device.vCameraPosition);

		float map_size = m_sun_cascades[cascade_ind].size;
		

		D3DXMatrixOrthoOffCenterLH((D3DXMATRIX*)&mdir_Project, -map_size * 0.5f, map_size*0.5f, -map_size * 0.5f, map_size*0.5f, 0.1, dist + map_size);

		// build viewport xform
		float	view_dim = float(RImplementation.o.smapsize);
		Fmatrix	m_viewport = {
			view_dim / 2.f,	0.0f,				0.0f,		0.0f,
			0.0f,			-view_dim / 2.f,		0.0f,		0.0f,
			0.0f,			0.0f,				1.0f,		0.0f,
			view_dim / 2.f,	view_dim / 2.f,		0.0f,		1.0f
		};
		Fmatrix				m_viewport_inv;
		D3DXMatrixInverse((D3DXMATRIX*)&m_viewport_inv, nullptr, (D3DXMATRIX*)&m_viewport);

		// snap view-position to pixel
		cull_xform.mul(mdir_Project, mdir_View);
		Fmatrix	cull_xform_inv; cull_xform_inv.invert(cull_xform);

		for (int p = 0; p < 8; p++) {
			Fvector3				xf = wform(cull_xform_inv, corners[p]);
			light_cuboid.light_cuboid_points[p] = xf;
		}

		// only side planes
		for (int plane = 0; plane < 4; plane++)
			for (int pt = 0; pt < 4; pt++)
			{
				int asd = facetable[plane][pt];
				light_cuboid.light_cuboid_polys[plane].points[pt] = asd;
			}


		Fvector lightXZshift;
		light_cuboid.compute_caster_model_fixed(cull_planes, lightXZshift, m_sun_cascades[cascade_ind].size, m_sun_cascades[cascade_ind].reset_chain);
		Fvector proj_view = Device.vCameraDirection;
		proj_view.y = 0;
		proj_view.normalize();

		// Initialize rays for the next cascade
		if (cascade_ind < m_sun_cascades.size() - 1)
			m_sun_cascades[cascade_ind + 1].rays = light_cuboid.view_frustum_rays;

		static bool draw_debug = false;
		if (draw_debug && cascade_ind == 0)
			for (u32 it = 0; it < cull_planes.size(); it++)
				RImplementation.Target->dbg_addplane(cull_planes[it], it * 0xFFF);

		Fvector cam_shifted = L_pos;
		cam_shifted.add(lightXZshift);

		// rebuild the view transform with the shift.
		mdir_View.identity();
		mdir_View.build_camera_dir(cam_shifted, L_dir, L_up);
		cull_xform.identity();
		cull_xform.mul(mdir_Project, mdir_View);
		cull_xform_inv.invert(cull_xform);


		// Create frustum for query
		cull_frustum._clear();
		for (auto & cull_plane : cull_planes)
			cull_frustum._add(cull_plane);

		{
			Fvector cam_proj = Device.vCameraPosition;
			const float		align_aim_step_coef = 4.f;
			cam_proj.set(floorf(cam_proj.x / align_aim_step_coef) + align_aim_step_coef / 2, floorf(cam_proj.y / align_aim_step_coef) + align_aim_step_coef / 2, floorf(cam_proj.z / align_aim_step_coef) + align_aim_step_coef / 2);
			cam_proj.mul(align_aim_step_coef);
			Fvector	cam_pixel = wform(cull_xform, cam_proj);
			cam_pixel = wform(m_viewport, cam_pixel);
			Fvector shift_proj = lightXZshift;
			
		
			cull_xform.transform_dir(shift_proj);
			m_viewport.transform_dir(shift_proj);

			const float	align_granularity = 4.f;
			shift_proj.x = shift_proj.x > 0 ? align_granularity : -align_granularity;
			shift_proj.y = shift_proj.y > 0 ? align_granularity : -align_granularity;
			shift_proj.z = 0;

			cam_pixel.x = cam_pixel.x / align_granularity - floorf(cam_pixel.x / align_granularity);
			cam_pixel.y = cam_pixel.y / align_granularity - floorf(cam_pixel.y / align_granularity);
			cam_pixel.x *= align_granularity;
			cam_pixel.y *= align_granularity;
			cam_pixel.z = 0;

			cam_pixel.sub(shift_proj);

			m_viewport_inv.transform_dir(cam_pixel);
			cull_xform_inv.transform_dir(cam_pixel);

			Fvector diff = cam_pixel;
			static float sign_test = -1.f;
			diff.mul(sign_test);
			Fmatrix adjust;		adjust.translate(diff);
			cull_xform.mulB_44(adjust); 
		}

		m_sun_cascades[cascade_ind].xform = cull_xform;

		s32	limit = RImplementation.o.smapsize - 1;
		fuckingsun->X.D.minX = 0;
		fuckingsun->X.D.maxX = limit;
		fuckingsun->X.D.minY = 0;
		fuckingsun->X.D.maxY = limit;

		// full-xform
		FPU::m24r();
	}

	// Begin SMAP-render
	bool	bSpecialFull = !mapNormalPasses[1][0].empty() || !mapMatrixPasses[1][0].empty() || !mapSorted.empty();
	VERIFY(!bSpecialFull);
	HOM.Disable();
	phase = PHASE_SMAP;
	r_pmask(true, RImplementation.o.Tshadows);

	// Fill the database
	r_dsgraph_render_subspace(cull_sector, &cull_frustum, cull_xform, cull_COP, TRUE);

	// Finalize & Cleanup
	fuckingsun->X.D.combine = cull_xform;

	// Render shadow-map
	//. !!! We should clip based on shrinked frustum (again)
	bool bNormal = !mapNormalPasses[0][0].empty() || !mapMatrixPasses[0][0].empty();
	bool bSpecial = !mapNormalPasses[1][0].empty() || !mapMatrixPasses[1][0].empty() || !mapSorted.empty();
	if (bNormal || bSpecial)
	{
		Target->phase_smap_direct(fuckingsun, SE_SUN_FAR);
		RCache.set_xform_world(Fidentity);
		RCache.set_xform_view(Fidentity);
		RCache.set_xform_project(fuckingsun->X.D.combine);
		r_dsgraph_render_graph(0);
		if (ps_r_flags.test(R_FLAG_DETAIL_SHADOW))
			Details->Render();
		fuckingsun->X.D.transluent = FALSE;

		if (bSpecial)
		{
			fuckingsun->X.D.transluent = TRUE;
			Target->phase_smap_direct_tsh(fuckingsun, SE_SUN_FAR);
			r_dsgraph_render_graph(1);			// normal level, secondary priority
			r_dsgraph_render_sorted();			// strict-sorted geoms
		}
	}

	// End SMAP-render
	r_pmask(true, false);

	// Accumulate
	Target->phase_accumulator();

#ifdef USE_DX11
	if (Target->use_minmax_sm_this_frame())
	{
		PIX_EVENT(SE_SUN_NEAR_MINMAX_GENERATE);
		Target->create_minmax_SM();
	}
#endif

	PIX_EVENT(SE_SUN_NEAR);

	if (cascade_ind == 0)
		Target->accum_direct_cascade(SE_SUN_NEAR, m_sun_cascades[cascade_ind].xform, m_sun_cascades[cascade_ind].xform, m_sun_cascades[cascade_ind].bias);
	else
		if (cascade_ind < m_sun_cascades.size() - 1)
			Target->accum_direct_cascade(SE_SUN_MIDDLE, m_sun_cascades[cascade_ind].xform, m_sun_cascades[cascade_ind - 1].xform, m_sun_cascades[cascade_ind].bias);
		else
			Target->accum_direct_cascade(SE_SUN_FAR, m_sun_cascades[cascade_ind].xform, m_sun_cascades[cascade_ind - 1].xform, m_sun_cascades[cascade_ind].bias);

	// Restore XForms
	RCache.set_xform_world(Fidentity);
	RCache.set_xform_view(Device.mView);
	RCache.set_xform_project(Device.mProject);
}