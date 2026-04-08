// EngineAPI.cpp: implementation of the CEngineAPI class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "EngineAPI.h"
#include "../xrcdb/xrXRC.h"

//#include "securom_api.h"

extern xr_token* vid_quality_token;

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

void __cdecl dummy(void)
{
};


#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "vfw32.lib")
#pragma comment(lib, "nvapi.lib")

#pragma comment(lib, "xrRender_R1.lib")
#pragma comment(lib, "xrRender_R2.lib")
#pragma comment(lib, "xrRender_R3.lib")
#pragma comment(lib, "xrRender_R4.lib")
// DR3D9
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d10.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dx11.lib")
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dx10.lib")
#pragma comment(lib, "dxgi.lib")

CEngineAPI::CEngineAPI()
{
	//hGame = 0;
	//hRender = 0;
	hTuner = 0;
	pCreate = 0;
	pDestroy = 0;
	tune_pause = dummy;
	tune_resume = dummy;
}

CEngineAPI::~CEngineAPI()
{
	// destroy quality token here
	if (vid_quality_token)
	{
		xr_free(vid_quality_token);
		vid_quality_token = NULL;
	}
}

ENGINE_API u32 g_current_renderer = 0;
ENGINE_API bool is_enough_address_space_available() { return sizeof(void*) == 8; }

extern BOOL DllMainXrRenderR1(HANDLE hModule, DWORD ul_reason_for_call, LPVOID lpReserved);
extern BOOL DllMainXrRenderR2(HANDLE hModule, DWORD ul_reason_for_call, LPVOID lpReserved);
extern BOOL DllMainXrRenderR3(HANDLE hModule, DWORD ul_reason_for_call, LPVOID lpReserved);
extern BOOL DllMainXrRenderR4(HANDLE hModule, DWORD ul_reason_for_call, LPVOID lpReserved);

void CEngineAPI::InitializeRenders()
{
	if (psDeviceFlags.test(rsR1)) {
		psDeviceFlags.set(rsR2, FALSE);
		psDeviceFlags.set(rsR3, FALSE);
		psDeviceFlags.set(rsR4, FALSE);
		Log("Loading render 1");
		DllMainXrRenderR1(NULL, DLL_PROCESS_ATTACH, NULL);
	} else if (psDeviceFlags.test(rsR2)) {
		psDeviceFlags.set(rsR1, FALSE);
		psDeviceFlags.set(rsR3, FALSE);
		psDeviceFlags.set(rsR4, FALSE);
		Log("Loading render 2");
		DllMainXrRenderR2(NULL, DLL_PROCESS_ATTACH, NULL);
	} else if (psDeviceFlags.test(rsR3)) {
		psDeviceFlags.set(rsR1, FALSE);
		psDeviceFlags.set(rsR2, FALSE);
		psDeviceFlags.set(rsR4, FALSE);
		Log("Loading render 3");
		DllMainXrRenderR3(NULL, DLL_PROCESS_ATTACH, NULL);
	}
	else if (psDeviceFlags.test(rsR4))
	{
		psDeviceFlags.set(rsR1, FALSE);
		psDeviceFlags.set(rsR2, FALSE);
		psDeviceFlags.set(rsR3, FALSE);
		Log("Loading render 4");
		DllMainXrRenderR4(NULL, DLL_PROCESS_ATTACH, NULL);
	}
}

extern BOOL DllMainXrGame(HANDLE hModule, u32 ul_reason_for_call, LPVOID lpReserved);

extern "C" DLL_Pure* __cdecl xrFactory_Create(CLASS_ID clsid);
extern "C" void __cdecl xrFactory_Destroy(DLL_Pure* O);
extern ENGINE_API bool g_dedicated_server;

void CEngineAPI::Initialize(void)
{
	PROF_EVENT("CEngineAPI::Initialize");
	//////////////////////////////////////////////////////////////////////////
	// render

	InitializeRenders();
	Device.ConnectToRender();

	Log("Loading xrGame...");
	DllMainXrGame(NULL, DLL_PROCESS_ATTACH, NULL);
	pCreate = xrFactory_Create;
	R_ASSERT(pCreate);
	pDestroy = xrFactory_Destroy;
	R_ASSERT(pDestroy);

	//////////////////////////////////////////////////////////////////////////
	// vTune
	tune_enabled = FALSE;
	if (strstr(Core.Params, "-tune"))
	{
		LPCSTR g_name = "vTuneAPI.dll";
		Log("Loading DLL:", g_name);
		hTuner = LoadLibrary(g_name);
		if (0 == hTuner)
			R_CHK(GetLastError());
		R_ASSERT2(hTuner, "Intel vTune is not installed");
		tune_enabled = TRUE;
		tune_pause = (VTPause*)GetProcAddress(hTuner, "VTPause");
		R_ASSERT(tune_pause);
		tune_resume = (VTResume*)GetProcAddress(hTuner, "VTResume");
		R_ASSERT(tune_resume);
	}
}

void CEngineAPI::Destroy(void)
{
	DllMainXrGame(NULL, DLL_PROCESS_DETACH, NULL);
	if (psDeviceFlags.test(rsR1))
		DllMainXrRenderR1(NULL, DLL_PROCESS_DETACH, NULL);
	else if (psDeviceFlags.test(rsR2))
		DllMainXrRenderR2(NULL, DLL_PROCESS_DETACH, NULL);
	else if (psDeviceFlags.test(rsR3))
		DllMainXrRenderR3(NULL, DLL_PROCESS_DETACH, NULL);
	else if (psDeviceFlags.test(rsR4))
		DllMainXrRenderR4(NULL, DLL_PROCESS_DETACH, NULL);
	pCreate = 0;
	pDestroy = 0;
	Engine.Event._destroy();
	XRC.r_clear_compact();
}

extern "C" {
typedef bool __cdecl SupportsAdvancedRenderingREF(void);
typedef bool /*_declspec(dllexport)*/ SupportsDX10RenderingREF();
typedef bool /*_declspec(dllexport)*/ SupportsDX11RenderingREF();
};

extern "C" {
#ifdef STATIC_RENDERER_R2
	bool /*_declspec(dllexport)*/ SupportsAdvancedRendering();
#endif
#ifdef STATIC_RENDERER_R3
bool /*_declspec(dllexport)*/ SupportsDX10Rendering();

#endif
#ifdef STATIC_RENDERER_R4
	bool /*_declspec(dllexport)*/ SupportsDX11Rendering();
#endif
};

void CEngineAPI::CreateRendererList()
{
	PROF_EVENT("CEngineAPI::CreateRendererList");
	if (g_dedicated_server) {

		vid_quality_token = xr_alloc<xr_token>(2);

		vid_quality_token[0].id = 0;
		vid_quality_token[0].name = xr_strdup("renderer_r1");

		vid_quality_token[1].id = -1;
		vid_quality_token[1].name = NULL;

	}
	else {
		// TODO: ask renderers if they are supported!
		if (vid_quality_token != NULL) return;
		bool bSupports_r2 = false;
		bool bSupports_r2_5 = false;
		bool bSupports_r3 = false;
		bool bSupports_r4 = false;

		LPCSTR r2_name = "xrRender_R2.dll";
		LPCSTR r3_name = "xrRender_R3.dll";
		LPCSTR r4_name = "xrRender_R4.dll";

		if (strstr(Core.Params, "-perfhud_hack"))
		{
			bSupports_r2 = true;
			bSupports_r2_5 = true;
			bSupports_r3 = true;
			bSupports_r4 = true;
		}
		else
		{
#ifdef STATIC_RENDERER_R2
			// try to initialize R2
			Log("Loading DLL:", r2_name);
			//hRender = LoadLibrary(r2_name);
			DllMainXrRenderR2(NULL, DLL_PROCESS_ATTACH, NULL);
			//if (hRender)
			{
				bSupports_r2 = true;
				//SupportsAdvancedRenderingREF* test_rendering = (SupportsAdvancedRenderingREF*)GetProcAddress(hRender, "SupportsAdvancedRendering");
				SupportsAdvancedRenderingREF* test_rendering = SupportsAdvancedRendering;
				R_ASSERT(test_rendering);
				bSupports_r2_5 = test_rendering();
				//FreeLibrary(hRender);
			}
#endif

#ifdef STATIC_RENDERER_R3
			// try to initialize R3
			Log("Loading DLL:", r3_name);
			// Hide "d3d10.dll not found" message box for XP
			SetErrorMode(SEM_FAILCRITICALERRORS);
			//hRender = LoadLibrary(r3_name);
			DllMainXrRenderR3(NULL, DLL_PROCESS_ATTACH, NULL);
			// Restore error handling
			SetErrorMode(0);
			//if (hRender)
			{
				//SupportsDX10RenderingREF* test_dx10_rendering = (SupportsDX10RenderingREF*)GetProcAddress(hRender, "SupportsDX10Rendering");
				SupportsDX10RenderingREF* test_dx10_rendering = SupportsDX10Rendering;
				R_ASSERT(test_dx10_rendering);
				bSupports_r3 = test_dx10_rendering();
				//FreeLibrary(hRender);
			}
#endif

#ifdef STATIC_RENDERER_R4
			// try to initialize R4
			Log("Loading DLL:", r4_name);
			// Hide "d3d10.dll not found" message box for XP
			SetErrorMode(SEM_FAILCRITICALERRORS);
			//hRender = LoadLibrary(r4_name);
			DllMainXrRenderR4(NULL, DLL_PROCESS_ATTACH, NULL);
			// Restore error handling
			SetErrorMode(0);
			//if (hRender)
			{
				//SupportsDX11RenderingREF* test_dx11_rendering = (SupportsDX11RenderingREF*)GetProcAddress(hRender, "SupportsDX11Rendering");
				SupportsDX11RenderingREF* test_dx11_rendering = SupportsDX11Rendering;
				R_ASSERT(test_dx11_rendering);
				bSupports_r4 = test_dx11_rendering();
				//FreeLibrary(hRender);
			}
#endif
		}

		//hRender = 0;
		bool proceed = true;
		xr_vector<LPCSTR> _tmp;
#ifdef STATIC_RENDERER_R1
		_tmp.push_back("renderer_r1");
#endif
#ifdef STATIC_RENDERER_R2
		if (proceed &= bSupports_r2, proceed)
		{
			_tmp.push_back("renderer_r2a");
			_tmp.push_back("renderer_r2");
		}
		if (proceed &= bSupports_r2_5, proceed)
			_tmp.push_back("renderer_r2.5");
#endif
#ifdef STATIC_RENDERER_R3
		if (proceed &= bSupports_r3, proceed)
			_tmp.push_back("renderer_r3");
#endif
#ifdef STATIC_RENDERER_R4
		if (proceed &= bSupports_r4, proceed)
			_tmp.push_back("renderer_r4");
#endif

		R_ASSERT2(_tmp.size() != 0, "No valid renderer found, please use a render system that's supported by your PC");

		u32 _cnt = _tmp.size() + 1;
		vid_quality_token = xr_alloc<xr_token>(_cnt);

		vid_quality_token[_cnt - 1].id = -1;
		vid_quality_token[_cnt - 1].name = NULL;

		//#ifdef DEBUG
		Msg("Available render modes[%d]:", _tmp.size());
		//#endif // DEBUG
		for (u32 i = 0; i < _tmp.size(); ++i)
		{
			vid_quality_token[i].id = i;
			vid_quality_token[i].name = _tmp[i];
			//#ifdef DEBUG
			Msg("[%s]", _tmp[i]);
			//#endif // DEBUG
		}
	}
}
