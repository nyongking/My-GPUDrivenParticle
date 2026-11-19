#include "pch.h"
#include "ShaderCompiler.h"

#include <dxcapi.h>
#include <d3d12shader.h>

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

#pragma comment(lib, "dxcompiler.lib")

namespace
{

	std::wstring Widen(const std::string& s)
	{
		return std::wstring(s.begin(), s.end());
	}

	std::string HResultText(HRESULT hr)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
		return buf;
	}
}


bool ShaderCompiler::Initialize(const std::wstring& includeDir)
{
	Shutdown();

	HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&m_Utils));
	if (FAILED(hr))
	{
		m_Log = "[ShaderCompiler] DxcUtils 생성 실패 " + HResultText(hr) + " (dxcompiler.dll 이 exe 옆에 있는지 확인)\n";
		return false;
	}

	hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_Compiler));
	if (FAILED(hr))
	{
		m_Log = "[ShaderCompiler] DxcCompiler 생성 실패 " + HResultText(hr) + "\n";
		Shutdown();
		return false;
	}
	hr = m_Utils->CreateDefaultIncludeHandler(&m_IncludeHandler);
	if (FAILED(hr))
	{
		m_Log = "[ShaderCompiler] include 처리기 생성 실패" + HResultText(hr) + "\n";
		Shutdown();
		return false;
	}

	m_IncludeDir = includeDir;
	if (m_IncludeDir.empty())
		m_Log += "[ShaderCompiler] 경고: include 폴더가 비어있음, ModuleCommon.hlsli 못 찾을 수 있음";

	ComPtr<IDxcVersionInfo> version;
	if (SUCCEEDED(m_Compiler->QueryInterface(IID_PPV_ARGS(&version))))
	{
		UINT32 major = 0, minor = 0;
		version->GetVersion(&major, &minor);
		m_Log = "[ShaderCompiler] DXC " + std::to_string(major) + "." + std::to_string(minor) + " 준비됨\n";
	}
	return true;
}

void ShaderCompiler::Shutdown()
{
	if (m_IncludeHandler)
	{
		m_IncludeHandler->Release();
		m_IncludeHandler = nullptr;
	}
	if (m_Compiler)
	{
		m_Compiler->Release();
		m_Compiler = nullptr;
	}
	if (m_Utils)
	{
		m_Utils->Release();
		m_Utils = nullptr;
	}

}

ShaderCompileResult ShaderCompiler::Compile(const std::string& source, const std::string& fileName, const std::string& entry, const std::string& target) const
{
	ShaderCompileResult out;
	if (!Ready())
	{
		out.messages = "[ShaderCompiler] Initialize 를 먼저 호출해야 함\n";
		return out;
	}

	// ---------- 인자 (Build.props 와 같은 설정) ----------
	std::vector<std::wstring> args =
	{
		Widen(fileName),          // 소스 이름 (에러 메시지, include 기준 폴더)
		L"-E", Widen(entry),
		L"-T", Widen(target),
		L"-HV", L"2021",
	};

	if (!m_IncludeDir.empty())
	{
		args.push_back(L"-I");
		args.push_back(m_IncludeDir);
	}

#if defined(_DEBUG)
	args.push_back(L"-Zi");            // 디버그 정보
	args.push_back(L"-Qembed_debug");  // PIX 에서 소스가 보이게 바이트코드에 넣기
	args.push_back(L"-Od");
#else
	args.push_back(L"-O3");
#endif

	std::vector<LPCWSTR> argsPtrs;
	for (const std::wstring& a : args)
		argsPtrs.push_back(a.c_str());

	DxcBuffer src;
	src.Ptr = source.data();
	src.Size = source.size();
	src.Encoding = DXC_CP_UTF8;

	// -------- 컴파일 -------
	ComPtr<IDxcResult> result;
	HRESULT hr = m_Compiler->Compile(&src, argsPtrs.data(), 
		static_cast<UINT32>(argsPtrs.size()),
		m_IncludeHandler, IID_PPV_ARGS(&result));
	if (FAILED(hr) || !result)
	{
		out.messages = "[ShaderCompiler] Compile 호출 실패 " + HResultText(hr) + "\n";
		return out;
	}

	// ------- 에러 + 경고 텍스트 -------
	ComPtr<IDxcBlobUtf8> errors;
	if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr))
		&& errors && errors->GetStringLength() > 0)
	{
		out.messages.assign(errors->GetStringPointer(), errors->GetStringLength());
	}

	HRESULT status = E_FAIL;
	result->GetStatus(&status);
	if (FAILED(status))
	{
		if (out.messages.empty())
			out.messages = "[ShaderCompiler] 컴파일 실패 " + HResultText(status) + " (메시지 없음)\n";
		return out;
	}

	// ------- 바이트코드 --------
	ComPtr<IDxcBlob> object;
	if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr))
		|| !object || object->GetBufferSize() == 0)
	{
		out.messages += "[ShaderCompiler] 바이트코드가 비어 있음\n";
		return out;
	}

	const uint8_t* p = static_cast<const uint8_t*>(object->GetBufferPointer());
	out.bytecode.assign(p, p + object->GetBufferSize());
	// 반사 데이터 (상수 버퍼 크기 확인용)
	if (result->HasOutput(DXC_OUT_REFLECTION))
	{
		ComPtr<IDxcBlob> refl;
		if (SUCCEEDED(result->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&refl), nullptr)) && refl)
		{
			const uint8_t* r = static_cast<const uint8_t*>(refl->GetBufferPointer());
			out.reflection.assign(r, r + refl->GetBufferSize());
		}
	}

	out.ok = true;
	return out;

}

bool ShaderCompiler::ReflectConstantBuffer(const ShaderCompileResult& result, 
	const std::string& cbufferName, 
	ConstantBufferInfo& out) const
{
	out = ConstantBufferInfo();
	if (!Ready() || result.reflection.empty())
		return false;

	DxcBuffer buf;
	buf.Ptr = result.reflection.data();
	buf.Size = result.reflection.size();
	buf.Encoding = 0;

	ComPtr<ID3D12ShaderReflection> refl;
	if (FAILED(m_Utils->CreateReflection(&buf, IID_PPV_ARGS(&refl))))
		return false;

	ID3D12ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByName(cbufferName.c_str());
	D3D12_SHADER_BUFFER_DESC cbDesc;
	if (!cb || FAILED(cb->GetDesc(&cbDesc)))
		return false;

	out.size = cbDesc.Size;
	for (UINT i = 0; i < cbDesc.Variables; ++i)
	{
		D3D12_SHADER_VARIABLE_DESC v;
		if (FAILED(cb->GetVariableByIndex(i)->GetDesc(&v)))
			continue;
		out.variables.push_back({ v.Name ? v.Name : "", v.StartOffset, v.Size });
	}
	return true;
}

bool ShaderCompiler::ReflectConstantBuffer(const void* bytecode, size_t size, const std::string& cbufferName, ConstantBufferInfo& out) const
{
	out = ConstantBufferInfo();
	if (!Ready() || bytecode == nullptr || size == 0)
		return false;

	DxcBuffer buf;
	buf.Ptr = bytecode;
	buf.Size = size;
	buf.Encoding = 0;

	ComPtr<ID3D12ShaderReflection> refl;
	if (FAILED(m_Utils->CreateReflection(&buf, IID_PPV_ARGS(&refl))))
		return false;

	ID3D12ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByName(cbufferName.c_str());
	D3D12_SHADER_BUFFER_DESC cbDesc;
	if (!cb || FAILED(cb->GetDesc(&cbDesc)))
		return false;

	out.size = cbDesc.Size;
	for (UINT i = 0; i < cbDesc.Variables; ++i)
	{
		D3D12_SHADER_VARIABLE_DESC v;
		if (FAILED(cb->GetVariableByIndex(i)->GetDesc(&v)))
			continue;
		out.variables.push_back({ v.Name ? v.Name : "", v.StartOffset, v.Size });
	}
	return true;


}

std::wstring ShaderCompiler::FindIncludeDir(const std::wstring& fileToFind)
{
#if defined(_WIN32)
	auto exists = [](const std::wstring& path)
		{
			const DWORD attr = GetFileAttributesW(path.c_str());
			return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
		};

	// 1) 작업 폴더\Shaders
	wchar_t cwd[MAX_PATH] = {};
	if (GetCurrentDirectoryW(MAX_PATH, cwd))
	{
		const std::wstring dir = std::wstring(cwd) + L"\\Shaders";
		if (exists(dir + L"\\" + fileToFind)) return dir;
	}

	// 2) exe 폴더\Shaders
	wchar_t exe[MAX_PATH] = {};
	if (GetModuleFileNameW(nullptr, exe, MAX_PATH))
	{
		std::wstring dir(exe);
		dir = dir.substr(0, dir.find_last_of(L"\\/")) + L"\\Shaders";
		if (exists(dir + L"\\" + fileToFind)) return dir;
	}
	return L"";
#else
	(void)fileToFind;
	return L"Shaders";
#endif
}