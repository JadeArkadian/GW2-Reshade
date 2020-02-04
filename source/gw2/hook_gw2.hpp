#pragma once

#include <vector>
#include <array>
#include <map>

#include "../d3d9/d3d9.hpp"
#include "log.hpp"
#include "gw2_table.hpp"
#include "xxhash.h"

#define MAX_TOKENS	8192

typedef union {
	DWORD d;
	float f;
} DWFL;

struct MumbleContext {
	byte serverAddress[28];
	unsigned mapId;
	unsigned mapType;
	unsigned shardId;
	unsigned instance;
	unsigned buildId;
};

struct LinkedMem {
	UINT32	uiVersion;
	DWORD	uiTick;
	float	fAvatarPosition[3];
	float	fAvatarFront[3];
	float	fAvatarTop[3];
	wchar_t	name[256];
	float	fCameraPosition[3];
	float	fCameraFront[3];
	float	fCameraTop[3];
	wchar_t	identity[256];
	UINT32	context_len;
	MumbleContext context;
	wchar_t description[2048];
};

class hook_gw2 {
public:
	hook_gw2(Direct3DDevice9* device) :
		_device(device),
		rt(new gw2_table),
		_pShaderInjection_vs(NULL),
		_pShaderInjection_ps(NULL),
		_surface_current(NULL),
		_is_fx_done(false),
		_is_in_competitive_map(false),
		_pattern_InjectionStable{ 0x800c0001, 0xa0550006, 0x3000009, 0x80010002, 0x80e40001, 0xa0e40004, 0x3000009, 0x80020002, 0x80e40001, 0xa0e40005, 0x2000001, 0xe0030000, 0x80440002 },//l = 13
		_pattern_charScreen{ 0x80440002, 0x2000001, 0xe00c0000, 0x90440009, 0x2000001, 0xe0030001, 0x90440008 }, //l = 7
		_pattern_bloom{ 0x3000005, 0x80270000, 0x80e40000, 0xa0ff0000, 0x2000001, 0x802f0800, 0x80e40000, } //l = 7
	{ }

	void OnPresent();
	HRESULT CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader);
	HRESULT SetVertexShader(IDirect3DVertexShader9 *pShader);
	HRESULT CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader);
	HRESULT SetPixelShader(IDirect3DPixelShader9 *pShader);
	HRESULT SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget);

private:
	void replacePatternFog1(const DWORD *pFunction, int l, float _fog_amount);
	void replacePatternFog2(const DWORD *pFunction, int l, float _fog_amount);
	void replacePatternBloom(const DWORD *pFunction, int l);

	bool isEnd(DWORD token);
	int get_pattern(const DWORD *pFunction, int l);
	bool checkPattern(const DWORD *pFunction, int l, DWORD *pattern, int pl);
	int getFuncLenght(const DWORD *pFunction);

	void initMumble();

	DWORD _pFunction[MAX_TOKENS];
	void* _pShaderInjection_vs;
	void* _pShaderInjection_ps;

	DWORD _pattern_InjectionStable[13];//l = 13
	DWORD _pattern_charScreen[7]; //l = 7
	DWORD _pattern_bloom[7]; //l = 7

	bool _is_fx_done;
	bool _is_lm_resolved;
	bool _is_in_competitive_map;

	IDirect3DSurface9* _surface_current;
	Direct3DDevice9 *_device;
	std::map<LPDIRECT3DPIXELSHADER9, LPDIRECT3DPIXELSHADER9> shader_map;
	int edited_shader_this_frame = 0;

	LinkedMem *lm = NULL;
	gw2_table *rt;
};	

