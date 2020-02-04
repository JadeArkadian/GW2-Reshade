#include "d3d9/d3d9_device.hpp"
#include "d3d9/d3d9_swapchain.hpp"
#include "ini_file.hpp"
#include "hook_gw2.hpp"

void hook_gw2::OnPresent() {
	auto runtime = _device->_implicit_swapchain->_runtime;
	if (lm == NULL) initMumble();

	if (!_is_fx_done) _device->_implicit_swapchain->_runtime->apply_effects(_surface_current);
	_is_fx_done = false;

	if (shader_map.size() > 2000) {
		for (auto const& x : shader_map){
			x.second->Release();
		}
		shader_map.clear();
		LOG(INFO) << "Cleared shader cache.";
	}
	edited_shader_this_frame = 0;
	
	if (_device->_implicit_swapchain->_runtime->_map_id != lm->context.mapId) {
		shader_map.clear();
		_device->_implicit_swapchain->_runtime->map_region = rt->get_region(lm->context.mapId);
		_device->_implicit_swapchain->_runtime->_map_id = lm->context.mapId;
		_is_in_competitive_map = rt->is_competitive(lm->context.mapId);

		if (_device->_implicit_swapchain->_runtime->_auto_preset == 0) {
			//Map changed
			bool preset_exist = false;

			//Check map
			for (int i = 0; i < runtime->_preset_files.size(); ++i) {
				std::string region;
				reshade::ini_file(runtime->_preset_files[i]).get("", "Zone", region);
				if (gw2_table::checkID(lm->context.mapId, region)) {
					runtime->_current_preset = i;
					runtime->load_preset(runtime->_preset_files[i]);
					preset_exist = true;
					if (runtime->_performance_mode) runtime->reload();
					break;
				}
			}
			if (!preset_exist) {
				//check region
				for (int i = 0; i < runtime->_preset_files.size(); ++i) {
					std::string region;
					reshade::ini_file(runtime->_preset_files[i]).get("", "Zone", region);
					if(region == rt->get_region(lm->context.mapId)) {
						runtime->_current_preset = i;
						runtime->load_preset(runtime->_preset_files[i]);
						preset_exist = true;
						if (runtime->_performance_mode) runtime->reload();
						break;
					}
				}
			}
			if (!preset_exist) {
				//global
				for (int i = 0; i < runtime->_preset_files.size(); ++i) {
					std::string region;
					reshade::ini_file(runtime->_preset_files[i]).get("", "Zone", region);
					if (region == "global") {
						runtime->_current_preset = i;
						runtime->load_preset(runtime->_preset_files[i]);
						preset_exist = true;
						if (runtime->_performance_mode) runtime->reload();
						break;
					}
				}
			}
		}

	}
}

HRESULT hook_gw2::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) {
	HRESULT hr = _device->_orig->SetRenderTarget(RenderTargetIndex, pRenderTarget);
	_surface_current = pRenderTarget;
	return hr;
}

HRESULT hook_gw2::CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) {
	HRESULT hr = _device->_orig->CreateVertexShader(pFunction, ppShader);
	
	if (_pShaderInjection_vs == NULL) {
		int l = getFuncLenght(pFunction);
		XXH64_hash_t hash = XXH64(pFunction, l, 0);
		if (hash == _device->_implicit_swapchain->_runtime->_inj_vs) {
			LOG(INFO) << "Stable injection point found.";
			_pShaderInjection_vs = *ppShader;
		}
	}

	return hr;
}

HRESULT hook_gw2::SetVertexShader(IDirect3DVertexShader9 *pShader) {
	if (pShader == NULL) return _device->_orig->SetVertexShader(pShader);

	if (_device->_implicit_swapchain->_runtime->_skip_ui == 0 && !_is_fx_done && pShader == _pShaderInjection_vs) {
		_is_fx_done = true;
		_device->_implicit_swapchain->_runtime->apply_effects(_surface_current);
	}

	return _device->_orig->SetVertexShader(pShader);
}

HRESULT hook_gw2::CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) {
	int l = getFuncLenght(pFunction);


	if (_pShaderInjection_ps == NULL) {
		HRESULT hr = _device->_orig->CreatePixelShader(pFunction, ppShader);
		XXH64_hash_t hash = XXH64(pFunction, l, 0);
		if (hash == _device->_implicit_swapchain->_runtime->_inj_ps) {
			LOG(INFO) << "Unstable injection point found.";
			_pShaderInjection_ps = *ppShader;
			return hr;
		}
	}
	
	int _pattern = get_pattern(pFunction, l);
	switch (_pattern) {
		case 1: //Detected pattern 1
			replacePatternFog1(pFunction, l, _device->_implicit_swapchain->_runtime->_fog_amount);
			return _device->_orig->CreatePixelShader(_pFunction, ppShader);
		case 2: //Detected pattern 2
			replacePatternFog2(pFunction, l, _device->_implicit_swapchain->_runtime->_fog_amount);
			return _device->_orig->CreatePixelShader(_pFunction, ppShader);
		case 3://Detected bloom
			if (_device->_implicit_swapchain->_runtime->_no_bloom == 0)
				return _device->_orig->CreatePixelShader(pFunction, ppShader);
			replacePatternBloom(pFunction, l);
			return _device->_orig->CreatePixelShader(_pFunction, ppShader);
		default: //No pattern detected
			return _device->_orig->CreatePixelShader(pFunction, ppShader);
	}
}

HRESULT hook_gw2::SetPixelShader(IDirect3DPixelShader9 *pShader) {
	if (pShader == NULL) return _device->_orig->SetPixelShader(pShader);
	
	if (_device->_implicit_swapchain->_runtime->_skip_ui == 0 && pShader == _pShaderInjection_ps && !_is_fx_done) {
		_is_fx_done = true;
		_device->_implicit_swapchain->_runtime->apply_effects(_surface_current);
	}

	HRESULT hr = _device->_orig->SetPixelShader(pShader);

	float fa = _is_in_competitive_map?1.0f:_device->_implicit_swapchain->_runtime->_fog_amount;
	float constant[] = { fa, fa, fa, fa};
	_device->SetPixelShaderConstantF(222, constant, 1);

	return hr;
}

int hook_gw2::get_pattern(const DWORD *pFunction, int l) {
	//pattern bugged { 0x91ff0000, 0x4000004, 0x80270800, 0xa0e40001, 0x80000000, 0x90e40000, 0x2000001, 0x80280800, 0xa0000000, }
	//If mad oC0 -> mov oC0 -> end
	if (pFunction[l - 4] == 0x2000001 && pFunction[l - 3] == 0x80280800 && pFunction[l - 9] == 0x4000004 && pFunction[l - 8] == 0x80270800 && pFunction[l - 7] == 0x80e40000) {
		//If last arg of move is one of the following
		if (pFunction[l - 2] == 0xa0000001 || pFunction[l - 2] == 0xa0000000 || pFunction[l - 2] == 0xa0000002 || pFunction[l - 2] == 0xa0ff0000 || pFunction[l - 2] == 0xa0ff0001) {
			//If op above are lerp and add, it's map ps so ignore
			if (pFunction[l - 13] == 0x3000002 && pFunction[l - 18] == 0x4000012) {
				return -1;
			}
			//Else it need to be edited with pattern 1
			return 1;
		}
		//If it's only mad oC0 -> end
	} else if (pFunction[l - 6] == 0x4000004 && pFunction[l - 5] == 0x80270800 && pFunction[l - 4] == 0x80e40000) {
		//If it's mad -> mad, it's UI ps
		if (pFunction[l - 11] == 0x4000004) return -1;
		//Else it need to be edited with pattern 2
		return 2;
	} else if (checkPattern(pFunction, l, _pattern_bloom, 7)) {
		return 3;
	}
	return -1;
}

void hook_gw2::replacePatternFog1(const DWORD * pFunction, int l, float _fog_amount) {
	DWORD constant[] = { 0x5000051, 0xa00f00df, 0, 0, 0, 0 };

	_pFunction[0] = pFunction[0];
	for (int i = 1; i < 7; ++i) _pFunction[i] = constant[i - 1];
	for (int i = 1; i < l; ++i) {
		//0x5000051
		_pFunction[i + 6] = pFunction[i];
	}
	l += 6;
	_pFunction[l + 4] = _pFunction[l - 1]; //END>>5
	_pFunction[l + 3] = _pFunction[l - 2];//c1>>4
	_pFunction[l + 2] = _pFunction[l - 3];//oC0.w>>4
	_pFunction[l + 1] = _pFunction[l - 4];//mov>>4

	_pFunction[l - 4] = 0x4000012; //lrp
	_pFunction[l - 3] = _pFunction[l - 8];//oC0.xyz
	_pFunction[l - 2] = 0xa00000de;//c223
	_pFunction[l - 1] = 0x80e40001;//r1
	_pFunction[l] = _pFunction[l - 7];//r0

	_pFunction[l - 8] = 0x800f0001;//oC0.xyz > r1
}

void hook_gw2::replacePatternFog2(const DWORD * pFunction, int l, float _fog_amount) {
	DWORD constant[] = { 0x5000051, 0xa00f00df, 0, 0, 0, 0 };

	for (int i = 1; i < 7; ++i) _pFunction[i] = constant[i - 1];
	for (int i = 1; i < l; ++i) {
		//0x5000051
		_pFunction[i + 6] = pFunction[i];
	}
	l += 6;

	_pFunction[l + 4] = _pFunction[l - 1]; //END>>5

	_pFunction[l - 1] = 0x4000012; //lrp
	_pFunction[l] = _pFunction[l - 5];//oC0.xyz
	_pFunction[l + 1] = 0xa00000de;//c223
	_pFunction[l + 2] = 0x80e40001;//r1
	_pFunction[l + 3] = _pFunction[l - 4];//r0

	_pFunction[l - 5] = 0x800f0001;//oC0.xyz > r1
}

void hook_gw2::replacePatternBloom(const DWORD * pFunction, int l) {
	LOG(INFO) << "Bloom shader edited.";
	DWFL hexFloat;
	hexFloat.f = 0;
	for (int i = 0; i < l; ++i) _pFunction[i] = pFunction[i];
	_pFunction[3] = hexFloat.d;
	_pFunction[4] = hexFloat.d;
	_pFunction[5] = hexFloat.d;
	_pFunction[6] = hexFloat.d;
}

bool hook_gw2::checkPattern(const DWORD *pFunction, int l, DWORD *pattern, int pl) {
	for (int i = 0; i < pl; ++i) {
		if (pFunction[l - 2 - i] != pattern[pl - 1 - i]) {
			return false;
		}
	}
	return true;
}

int hook_gw2::getFuncLenght(const DWORD *pFunction) {
	int op = 0, l = 1;
	while (!isEnd(pFunction[op++]))  l++;
	return l;
}

bool hook_gw2::isEnd(DWORD token) {
	return (token & D3DSI_OPCODE_MASK) == D3DSIO_END;
}

void hook_gw2::initMumble() {
	HANDLE hMapObject = OpenFileMappingW(PAGE_READONLY, FALSE, L"MumbleLink");
	if (hMapObject == NULL) {
		hMapObject = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LinkedMem), L"MumbleLink");
	}
	if (hMapObject != NULL) {
		lm = (LinkedMem *)MapViewOfFile(hMapObject, PAGE_READONLY, 0, 0, sizeof(LinkedMem));
		if (lm == NULL) {
			CloseHandle(hMapObject);
			hMapObject = NULL;
			return;
		}
	}
}
