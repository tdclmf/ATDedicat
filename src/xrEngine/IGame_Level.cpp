#include "stdafx.h"
#include "igame_level.h"
#include "igame_persistent.h"

#include "x_ray.h"
#include "std_classes.h"
#include "customHUD.h"
#include "render.h"
#include "gamefont.h"
#include "xrLevel.h"
#include "CameraManager.h"
#include "xr_object.h"
#include "feel_sound.h"

//#include "securom_api.h"

ENGINE_API IGame_Level* g_pGameLevel = NULL;
extern BOOL g_bLoaded;

IGame_Level::IGame_Level()
{
	m_pCameras = xr_new<CCameraManager>(true);
	g_pGameLevel = this;
	pLevel = NULL;
	bReady = false;
	pCurrentEntity = NULL;
	pCurrentViewEntity = NULL;
	Device.DumpResourcesMemoryUsage();
}

//#include "resourcemanager.h"

IGame_Level::~IGame_Level()
{
	if (strstr(Core.Params, "-nes_texture_storing"))
		//Device.Resources->StoreNecessaryTextures();
		Device.m_pRender->ResourcesStoreNecessaryTextures();
	xr_delete(pLevel);

	// Render-level unload
	Render->level_Unload();
	xr_delete(m_pCameras);
	// Unregister
	Device.seqParallel.clear_not_free();
	Device.seqParallelPB.clear_not_free();
	Device.seqRender.Remove(this);
	Device.seqFrame.Remove(this);
	CCameraManager::ResetPP();
	///////////////////////////////////////////
	Sound->set_geometry_occ(NULL);
	Sound->set_handler(NULL);
	Device.DumpResourcesMemoryUsage();

	u32 m_base = 0, c_base = 0, m_lmaps = 0, c_lmaps = 0;
	if (Device.m_pRender)
		Device.m_pRender->ResourcesGetMemoryUsage(m_base, c_base, m_lmaps, c_lmaps);

	Msg("* [ D3D ]: textures[%d K]", (m_base + m_lmaps) / 1024);
}

void IGame_Level::net_Stop()
{
	for (int i = 0; i < 6; i++)
		Objects.Update(false);
	// Destroy all objects
	Objects.Unload();
	IR_Release();

	bReady = false;
}

//-------------------------------------------------------------------------------------------
//extern CStatTimer tscreate;
void __stdcall _sound_event(ref_sound_data_ptr S, float range)
{
	if (g_pGameLevel && S && S->feedback) g_pGameLevel->SoundEvent_Register(S, range);
}

static void __stdcall build_callback(Fvector* V, int Vcnt, CDB::TRI* T, int Tcnt, void* params)
{
	g_pGameLevel->Load_GameSpecific_CFORM(T, Tcnt);
}

bool IGame_Level::Load(u32 dwNum)
{
	if (dwNum == -1) return false;

	// Initialize level data
	pApp->Level_Set(dwNum);
	string_path temp;
	if (!FS.exist(temp, "$level$", "level.ltx"))
		Debug.fatal(DEBUG_INFO, "Can't find level configuration file '%s'.", temp);
	pLevel = xr_new<CInifile>(temp);

	// Open
	// g_pGamePersistent->LoadTitle ("st_opening_stream");
	g_pGamePersistent->LoadTitle();
	IReader* LL_Stream = FS.r_open("$level$", "level");
	IReader& fs = *LL_Stream;

	// Header
	hdrLEVEL H;
	fs.r_chunk_safe(fsL_HEADER, &H, sizeof(H));
	R_ASSERT2(XRCL_PRODUCTION_VERSION == H.XRLC_version, "Incompatible level version.");

	// CForms
	// g_pGamePersistent->LoadTitle ("st_loading_cform");
	g_pGamePersistent->LoadTitle();
	ObjectSpace.Load(build_callback);
	//Sound->set_geometry_occ ( &Static );
	Sound->set_geometry_occ(ObjectSpace.GetStaticModel());
	Sound->set_handler(_sound_event);

	pApp->LoadSwitch();


	// HUD + Environment
	if (!g_hud)
		g_hud = (CCustomHUD*)NEW_INSTANCE(CLSID_HUDMANAGER);

	// Render-level Load
	Render->level_Load(LL_Stream);
	// tscreate.FrameEnd ();
	// Msg ("* S-CREATE: %f ms, %d times",tscreate.result,tscreate.count);

	// Objects
	g_pGamePersistent->Environment().mods_load();
	R_ASSERT(Load_GameSpecific_Before());
	Objects.Load();
	//. ANDY R_ASSERT (Load_GameSpecific_After ());

	// Done
	FS.r_close(LL_Stream);
	bReady = true;
	if (!g_dedicated_server) {
		IR_Capture();
		Device.seqRender.Add(this);
	}

	Device.seqFrame.Add(this);

	//SECUROM_MARKER_PERFORMANCE_OFF(10)

	return true;
}

int psNET_DedicatedSleep = 5;

void IGame_Level::OnRender()
{
	if (!g_dedicated_server) {
		// if (_abs(Device.fTimeDelta)<EPS_S) return;

#ifdef _GPA_ENABLED
		TAL_ID rtID = TAL_MakeID(1, Core.dwFrame, 0);
		TAL_CreateID(rtID);
		TAL_BeginNamedVirtualTaskWithID("GameRenderFrame", rtID);
		TAL_Parami("Frame#", Device.dwFrame);
		TAL_EndVirtualTask();
#endif // _GPA_ENABLED

		// Level render, only when no client output required
		if (!g_dedicated_server)
		{
			Render->Calculate();
			Render->Render();
		}
		else
		{
			Sleep(psNET_DedicatedSleep);
		}

#ifdef _GPA_ENABLED
		TAL_RetireID(rtID);
#endif // _GPA_ENABLED

		// Font
		// pApp->pFontSystem->SetSizeI(0.023f);
		pApp->pFontSystem->OnRender();
	}
}
#include "../../Include/xrRender/Kinematics.h"
void IGame_Level::OnFrame()
{
	PROF_EVENT("IGame_Level::OnFrame");
	// Log ("- level:on-frame: ",u32(Device.dwFrame));
	// if (_abs(Device.fTimeDelta)<EPS_S) return;

	// Update all objects
	VERIFY(bReady);
	Objects.Update(false);

	static DWORD this_thread_id = GetCurrentThreadId();
	Device.parallel_render_tasks.run([]()
	{
		if (this_thread_id != GetCurrentThreadId()) { PROF_THREAD("X-Ray PPL Thread") }
		Core.InitializeCOM();
		PROF_EVENT("CalculateBones");

		if (!g_SpatialSpace || !g_pGamePersistent)
			return;

		CFrustum ViewBase;
		ViewBase.CreateFromMatrix(Device.mFullTransform_saved, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
		const Fvector cam_pos = Device.vCameraPosition_saved;
		const auto* current_env = g_pGamePersistent->Environment().CurrentEnv;
		const float fog_distance = current_env ? current_env->fog_distance : 200.f;
		xr_vector<ISpatial*> spatials;
		g_SpatialSpace->q_sphere(spatials, ISpatial_DB::O_ORDERED, STYPE_RENDERABLE + STYPE_LIGHTSOURCE, cam_pos, fog_distance);
		spatials.erase(std::remove_if(spatials.begin(), spatials.end(), [&ViewBase, cam_pos](ISpatial* spatial)
		{
			if (!spatial) return true;
			if (!ViewBase.testSphere_dirty(spatial->spatial.sphere.P, spatial->spatial.sphere.R))
			{
				if (cam_pos.distance_to_sqr(spatial->spatial.sphere.P) > 62500.f)//250 m
					return true;
			}

			spatial->spatial_updatesector();

			if (spatial->spatial.type & STYPE_LIGHTSOURCE)
				return true;

			if (IRenderable* renderable = spatial->dcast_Renderable())
			{
				if(!renderable->renderable.visual || renderable->renderable.visual->dcast_ParticleCustom() || !renderable->renderable.visual->dcast_PKinematics())
					return true;
			}

			return false;
		}), spatials.end());

		TRY std::sort(spatials.begin(), spatials.end(), [cam_pos](ISpatial*& _1, ISpatial*& _2)
		{
			return _1->spatial.sphere.P.distance_to_sqr(cam_pos) < _2->spatial.sphere.P.distance_to_sqr(cam_pos);
		}); CATCH

		for (ISpatial* spatial : spatials)
		{
			TRY
			if (!spatial) continue;
			CObject* obj = spatial->dcast_CObject();
			if (!obj || !obj->Visual()) continue;
			IKinematics* ptr = obj->Visual()->dcast_PKinematics();
			if (!ptr) continue;
			ptr->CalculateBones(TRUE);
			CATCH
		}
	});

	// Ambience
	if (Sounds_Random.size() && (Device.dwTimeGlobal > Sounds_Random_dwNextTime))
	{
		Sounds_Random_dwNextTime = Device.dwTimeGlobal + ::Random.randI(10000, 20000);
		Fvector pos;
		pos.random_dir().normalize().mul(::Random.randF(30, 100)).add(Device.vCameraPosition);
		int id = ::Random.randI(Sounds_Random.size());
		if (Sounds_Random_Enabled)
		{
			Sounds_Random[id].play_at_pos(0, pos, 0);
			Sounds_Random[id].set_volume(1.f);
			Sounds_Random[id].set_range(10, 200);
		}
	}
}

void IGame_Level::d3dtext_renderer(Fvector pos, LPCSTR str, u32 color)
{
	Fvector4		v_res;
	Device.mFullTransform_saved.transform(v_res, pos);

	float x = (1.f + v_res.x) / 2.f * (Device.dwWidth);
	float y = (1.f - v_res.y) / 2.f * (Device.dwHeight);

	if (v_res.z < 0 || v_res.w < 0)
		return;

	if (v_res.x < -1.f || v_res.x > 1.f || v_res.y < -1.f || v_res.y>1.f)
		return;
	pApp->pFontSystem->SetAligment(CGameFont::alCenter);
	pApp->pFontSystem->SetColor(color);

	pApp->pFontSystem->Out(x, y, str ? str : "+");
}

void IGame_Level::d2dtext_renderer(Ivector2 pos, LPCSTR str, u32 color)
{
	pApp->pFontSystem->SetAligment(CGameFont::alCenter);
	pApp->pFontSystem->SetColor(color);

	pApp->pFontSystem->Out(pos.x, pos.y, str ? str : "+");
}

// ==================================================================================================

void CServerInfo::AddItem(LPCSTR name_, LPCSTR value_, u32 color_)
{
	shared_str s_name(name_);
	AddItem(s_name, value_, color_);
}

void CServerInfo::AddItem(shared_str& name_, LPCSTR value_, u32 color_)
{
	SItem_ServerInfo it;
	// shared_str s_name = CStringTable().translate( name_ );

	// xr_strcpy( it.name, s_name.c_str() );
	xr_strcpy(it.name, name_.c_str());
	xr_strcat(it.name, " = ");
	xr_strcat(it.name, value_);
	it.color = color_;

	if (data.size() < max_item)
	{
		data.push_back(it);
	}
}

void IGame_Level::SetEntity(CObject* O)
{
	if (pCurrentEntity)
		pCurrentEntity->On_LostEntity();

	if (O)
		O->On_SetEntity();

	pCurrentEntity = pCurrentViewEntity = O;
}

void IGame_Level::SetViewEntity(CObject* O)
{
	if (pCurrentViewEntity)
		pCurrentViewEntity->On_LostEntity();

	if (O)
		O->On_SetEntity();

	pCurrentViewEntity = O;
}
xrCriticalSection s_event_cs;
void IGame_Level::SoundEvent_Register(ref_sound_data_ptr S, float range)
{
	if (!g_bLoaded) return;
	if (!S) return;
	if (S->g_object && S->g_object->getDestroy())
	{
		S->g_object = 0;
		return;
	}
	if (0 == S->feedback) return;
	PROF_EVENT("IGame_Level::SoundEvent_Register");
	clamp(range, 0.1f, 500.f);

	const CSound_params* p = S->feedback->get_params();
	Fvector snd_position = p->position;
	if (S->feedback->is_2D())
	{
		snd_position.add(Sound->listener_position());
	}

	VERIFY(p && _valid(range));
	range = _min(range, p->max_ai_distance);
	VERIFY(_valid(snd_position));
	VERIFY(_valid(p->max_ai_distance));
	VERIFY(_valid(p->volume));

	// Query objects
	Fvector bb_size = {range, range, range};
	g_SpatialSpace->q_box(snd_ER, 0, STYPE_REACTTOSOUND, snd_position, bb_size);

	// Iterate
	xr_vector<ISpatial*>::iterator it = snd_ER.begin();
	xr_vector<ISpatial*>::iterator end = snd_ER.end();
	for (; it != end; it++)
	{
		Feel::Sound* L = (*it)->dcast_FeelSound();
		if (0 == L) continue;
		CObject* CO = (*it)->dcast_CObject();
		VERIFY(CO);
		if (CO->getDestroy()) continue;

		// Energy and signal
		VERIFY(_valid((*it)->spatial.sphere.P));
		float dist = snd_position.distance_to((*it)->spatial.sphere.P);
		if (dist > p->max_ai_distance) continue;
		VERIFY(_valid(dist));
		VERIFY2(!fis_zero(p->max_ai_distance), S->handle->file_name());
		float Power = (1.f - dist / p->max_ai_distance) * p->volume;
		VERIFY(_valid(Power));
		if (Power > EPS_S)
		{
			float occ = Sound->get_occlusion_to((*it)->spatial.sphere.P, snd_position);
			VERIFY(_valid(occ));
			Power *= occ;
			if (Power > EPS_S)
			{
				_esound_delegate D = {L, S, Power};
				xrCriticalSection::raii guard(&s_event_cs);
				snd_Events.push_back(D);
			}
		}
	}
	snd_ER.clear_not_free();
}

void IGame_Level::SoundEvent_Dispatch()
{
	xrCriticalSection::raii guard(&s_event_cs);
	while (!snd_Events.empty())
	{
		_esound_delegate& D = snd_Events.back();
		if (D.dest && D.source && D.source->feedback)
		{
			D.dest->feel_sound_new(
				D.source->g_object,
				D.source->g_type,
				D.source->g_userdata,

				D.source->feedback->is_2D() ? Device.vCameraPosition : D.source->feedback->get_params()->position,
				D.power
			);
		}
		snd_Events.pop_back();
	}
}

// Lain: added
void IGame_Level::SoundEvent_OnDestDestroy(Feel::Sound* obj)
{
	xrCriticalSection::raii guard(&s_event_cs);
	struct rem_pred
	{
		rem_pred(Feel::Sound* obj) : m_obj(obj)
		{
		}

		bool operator ()(const _esound_delegate& d)
		{
			return d.dest == m_obj;
		}

	private:
		Feel::Sound* m_obj;
	};

	snd_Events.erase(std::remove_if(snd_Events.begin(), snd_Events.end(), rem_pred(obj)),
	                 snd_Events.end());
}
