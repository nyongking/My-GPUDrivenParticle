#pragma once
// ShaderGenerator로 만든 문자열을 컴파일 진행

struct IDxcUtils;
struct IDxcCompiler3;
struct IDxcIncludeHandler;

struct ShaderCompileResult
{
	bool					ok = false; 
	std::vector<uint8_t>	bytecode;	// PSO에 넣을 내용
	std::vector<uint8_t>	reflection;	// ReflectConstantBuffer에 넘길 데이터
	std::string				messages;	// 에러 + 경고
};

struct ConstantBufferInfo
{
	struct Variable { std::string name; uint32_t offset; uint32_t size; };

	uint32_t              size = 0; // 16배수 크기인 DXC가 잡은 크기
	std::vector<Variable> variables;
};

class ShaderCompiler
{
public:
	ShaderCompiler() = default;
	~ShaderCompiler()
	{
		Shutdown();
	}

	ShaderCompiler(const ShaderCompiler&) = delete;
	ShaderCompiler& operator=(const ShaderCompiler&) = delete;

	bool Initialize(const std::wstring& includeDir);
	void Shutdown();
	bool Ready() const { return m_Compiler != nullptr; }
	const std::string& Log() const { return m_Log; }

	// source : HLSL 전체
	// fileName : '에러 메시지'가 나올 이름
	// entry : main 함수 이름
	// target : version, "cs_6_0"과 같이 표현
	ShaderCompileResult Compile(const std::string& source, const std::string& fileName,
		const std::string& entry, const std::string& target = "cs_6_2") const;
 
	bool ReflectConstantBuffer(const ShaderCompileResult& result, const std::string& cbufferName,
		ConstantBufferInfo& out) const;

	bool ReflectConstantBuffer(const void* bytecode, size_t size, const std::string& cbufferName,
		ConstantBufferInfo& out) const;

	static std::wstring FindIncludeDir(const std::wstring& fileToFind);

private:
	struct IDxcUtils*			m_Utils = nullptr;
	struct IDxcCompiler3*		m_Compiler = nullptr;
	struct IDxcIncludeHandler*	m_IncludeHandler = nullptr;
	std::wstring				m_IncludeDir;
	std::string					m_Log;
};

