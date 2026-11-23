#include "pch.h"
#include "GameCore.h"
#include "GraphicsCore.h"
#include "SystemTime.h"
#include "TextRenderer.h"
#include "GameInput.h"
#include "CommandContext.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "BufferManager.h"
#include "EngineProfiling.h"

#include "PostEffects.h"
#include "Renderer.h"
#include "Camera.h"
#include "CameraController.h"
#include "GlobalData.h"
#include "LightManager.h"
#include "DebugDraw.h"
#include "TexturedQuad.h"
#include "ImGuiLayer.h"


#include "ParticleSystem.h"
#include "ParticleUtility.h"

#include "ProfileRecorder.h"

#include "ModuleLibrary.h"
#include "CurveLibrary.h"
#include "EmitterRuntime.h"
#include "ParticleMaterial.h"

#include "EmitterTestScene.h"
#include "UberGpuProgram.h"

#include "ModuleStackPanel.h"
#include "MaterialGraphEditor.h"
#include "EmitterAsset.h"
#include <algorithm>
#include <cmath>

using namespace GameCore;
using namespace Graphics;

GlobalData sGlobal;

class MyProject : public GameCore::IGameApp
{
public:
    MyProject()
    {
    }
    virtual ~MyProject()
    {

    }

    virtual void Startup(void) override;
    virtual void Cleanup(void) override;

    virtual void Update(float deltaT) override;
    virtual void RenderScene(void) override;

    virtual bool RequiresImGui() const override { return true; }

private:
    void InitializeEmitter();
    void UpdateImGui(float deltaT);
    void UpdateMiddleDragLook();
    void UpdateTestSceneImGui();
    void TickRecorder();
    void StartBenchmark();
    void TickBenchmark();
    void DrawBenchmarkImGui();


    bool m_ShowImGuiDemo = false;

    Camera m_Camera;
    std::unique_ptr<CameraController> m_CameraController;

    // -----------
    EmitterRuntime  m_Runtime;
    float3          m_EmitterPos = { 0.f, 0.f, 0.f };

    // ----------- 소유자 변환 (엔진 값 LocalToWorld / Velocity 확인)
    float           m_OwnerAngle = 0.f;         // 라디안
    float           m_RotateSpeedDeg = 90.f;    // 초당 회전 (Box Location 방향이 돌아감)
    bool            m_MoveOwner = false;        // 위아래로 움직이기 (Owner.Velocity 확인)
    float           m_OwnerTime = 0.f;

    bool            m_TimeStop = false;         // true = 시뮬레이션 정지 (카메라 / UI 는 계속)
    bool            m_StepOnce = false;         // 정지 중 한 프레임만 진행

    static constexpr const wchar_t* kSimTimerName = L"Particle Simulation";   // 모든 이미터 시뮬레이션의 GPU 타이머
    static constexpr const char*    kBenchCsvPath = "ParticleProfile_week5_bench_v3.csv";

    EmitterTestScene            m_TestScene;
    EmitterTestScene::Settings  m_TestDraft;          // UI 가 편집, Build 를 눌러야 적용
    std::vector<std::string>    m_TestAssetList;      // Assets/Emitters 목록 (Refresh 를 눌러야 다시 읽음)
    bool                        m_TestAssetListRead = false;
    bool                        m_UseTestScene = false;
    bool                        m_FixedDt = true;     // 측정 재현성을 위해 1/60 고정

    ProfileRecorder             m_Recorder;
    char                        m_RecordTag[64] = "Generated";
    int                         m_RecordFrames = 600;
    GpuSimFrameStats            m_LastSimStats;       // 지난 프레임 값 (UI 표시 / CSV)

    // CSV 한 줄. 저장이 실패하면 (OneDrive 동기화 / 다른 프로그램이 파일을 잡고 있음) 들고 있다가 다시 시도한다
    //   다음 Record 를 시작하기 전까지 기록기의 샘플이 그대로라 같은 줄을 다시 쓸 수 있다
    struct CsvRow { std::string path, header, values, tag; uint32_t capacity = 0; };
    void                        SaveCsvRow(const CsvRow& row);
    CsvRow                      m_PendingCsv;
    bool                        m_CsvPending = false;
    uint32_t                    m_CsvRetryWait = 0;   // 남은 프레임 (0 이 되면 다시 시도)
    std::string                 m_CsvStatus;          // 마지막 저장 결과 (전체 경로 포함)

    struct BenchStep { uint32_t count; GpuSimMode mode; bool stageMajor; };
    static constexpr uint32_t   kBenchCountOptions = 6;
    static constexpr uint32_t   kBenchCounts[kBenchCountOptions] = { 1, 10, 50, 100, 200, 400 };
    bool                        m_BenchUseCount[kBenchCountOptions] = { true, true, true, true, true, false };
    int                         m_BenchWarmup = 120;              // 장면을 새로 만든 뒤 버릴 프레임 (생성/전환 직후의 튐 제외)
    std::vector<BenchStep>      m_BenchSteps;
    size_t                      m_BenchIndex = 0;
    bool                        m_BenchRunning = false;
    bool                        m_BenchRecording = false;         // 지금 기록 중인 것이 벤치마크 한 칸인가 (CSV 파일 구분)

    bool                        m_BenchEmitterOrder = true;
    bool                        m_BenchStageOrder = true;

    // -------- 평면 설정 --------
    TexturedQuad m_Floor;

    FlyingFPSCamera* m_FlyCam = nullptr;   // m_CameraController 가 소유, 여기선 참조만
    bool             m_MiddleDragging = false;

};

CREATE_APPLICATION(MyProject)

void MyProject::Startup(void)
{
    // Rendering Setting
    //g_SceneColorBuffer.SetClearColor(Color(0.5f, 0.5f, 0.4f));
    PostEffects::EnableAdaptation = false;
    PostEffects::BloomEnable = false;

    Renderer::Initialize();
    DebugDraw::Initialize();
    QuadRenderer::Initialize();

    // Camera Setting
    m_Camera.SetZRange(0.1f, 1000.f);
    m_Camera.SetEyeAtUp(Vector3(0.f, 3.f, -5.f), Vector3(0.f, 0.f, 0.f), Vector3(Math::kYUnitVector));

    auto NewCam = new FlyingFPSCamera(m_Camera, Vector3(Math::kYUnitVector));
    NewCam->SetMoveSpeed(10.f);
    NewCam->SetStrafeSpeed(10.f);
    m_CameraController.reset(NewCam);
    m_FlyCam = NewCam;



    m_Recorder.SetTimers({ kSimTimerName });

    CurveLibrary::Initialize();     // 전역 커브 버퍼. 이미터가 묶어 쓰므로 이미터보다 먼저

    InitializeEmitter();

    m_Floor.SetFloor(Vector3(0.f, -3.f, 0.f), 10.f);
    m_Floor.SetTint(Color(0.35f, 0.35f, 0.38f));
}

void MyProject::Cleanup(void)
{
    MaterialGraphEditor::Shutdown();
    // Free up resources in an orderly fashion
    m_TestScene.Shutdown();
    m_Runtime.Shutdown();
    CurveLibrary::Shutdown();       // 이미터가 다 내려간 뒤 (Shutdown 이 GPU 를 기다린다)
    ParticleTextures::Shutdown();

    m_Floor.Release();

    m_CameraController.reset();
    QuadRenderer::Shutdown();
    DebugDraw::Shutdown();
    Lighting::Shutdown(); // temp
    Renderer::Shutdown();
}

void MyProject::Update(float deltaT)
{
    ScopedTimer _prof(L"Update State");
    ParticleTextures::CollectRetired();

    // Update something
    m_CameraController->Update(deltaT);

    m_Runtime.ApplyPending();     // UI 가 지난 프레임에 남긴 변경을 Tick 전에 적용

    // 타임 스톱: 시뮬레이션 / 소유자 시간을 0 으로 (Tick 은 그대로 돌려 펜스 / 버퍼 흐름 유지). Step 은 한 프레임만 진행
    const bool  advance = !m_TimeStop || m_StepOnce;
    m_StepOnce = false;
    const float frameDt = m_FixedDt ? (1.f / 60.f) : deltaT;
    const float simDt = advance ? frameDt : 0.f;
    const float ownerDt = advance ? (m_TimeStop ? frameDt : deltaT) : 0.f;

    // 소유자: Y 축 회전 + (선택) 위아래 이동
    m_OwnerTime += ownerDt;
    m_OwnerAngle += m_RotateSpeedDeg * (3.14159265f / 180.f) * ownerDt;
    const float bob = m_MoveOwner ? std::sin(m_OwnerTime * 2.f) : 0.f;   // 위아래 1m

    OwnerTransform owner;
    owner.position = { m_EmitterPos.x, m_EmitterPos.y + bob, m_EmitterPos.z };
    owner.rotation = QuatFromAxisAngle(float3{ 0.f, 1.f, 0.f }, m_OwnerAngle);

    if (!m_TimeStop)
    {
        sGlobal.exeTime += frameDt;
    }

    {
        GpuEmitterInstance::BeginFrameStats();
        ComputeContext& simCtx = ComputeContext::Begin(kSimTimerName);
        CurveLibrary::Flush(simCtx);    // 편집된 커브를 올리고 버퍼를 읽기 상태로 (시뮬레이션이 t1 로 읽는다)

        if (m_UseTestScene)
            m_TestScene.Tick(simCtx, simDt);
        else
            m_Runtime.Tick(simCtx, simDt, owner);

        const uint64_t fence = simCtx.Finish();

        if (m_UseTestScene)
            m_TestScene.OnSubmitted(fence);
        else
            m_Runtime.OnSubmitted(fence);

        m_LastSimStats = GpuEmitterInstance::FrameStats();
    }

    TickRecorder();
    TickBenchmark();

    UpdateImGui(deltaT);
    UpdateMiddleDragLook();
}

void MyProject::RenderScene(void)
{
    GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Render");

    gfxContext.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
    gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);

    gfxContext.ClearColor(g_SceneColorBuffer);
    gfxContext.ClearDepth(g_SceneDepthBuffer);

    gfxContext.SetRenderTarget(g_SceneColorBuffer.GetRTV(), g_SceneDepthBuffer.GetDSV());
    gfxContext.SetViewportAndScissor(0, 0, g_SceneColorBuffer.GetWidth(), g_SceneColorBuffer.GetHeight());

    // Rendering something
    DebugDraw::Render(gfxContext, m_Camera.GetViewProjMatrix());
    QuadRenderer::BeginDraw(gfxContext, m_Camera.GetViewProjMatrix());
    //m_Floor.Draw(gfxContext);

    if (m_UseTestScene)
        m_TestScene.Render(gfxContext, m_Camera);
    else
        m_Runtime.Render(gfxContext, m_Camera);

    // Finish
    gfxContext.Finish();
}


void MyProject::InitializeEmitter()
{
    const std::vector<ModuleDesc> modules =
    {
        // Emitter Update
        SpawnRate(50.f),

        // Particle Initialize
        InitializeParticle(0.1f, 1.f, 1.f, RandomRange<float3>(float3{0.3f, 0.1f, 0.f}, float3{-0.3f, -0.1f, 0.f}, true), float4{1.f, 1.f, 1.f, 1.f}),
        SphereLocation(0.1f, true),
        //AddVelocityInCone(5.f, 45.f, float3{0.f, 1.f, 0.f}),

        // Particle Update
        ParticleState(true),
        //GravityForce(float3{0.f, -10.f, 0.f}),
        SolveForceAndVelocity(10.f, 100.f),
        SubUVAnimation(0.f, 63.f)      // 수명 동안 0~63 프레임 (T_Smoke_SubUV = 8x8)
    };

    if (!m_Runtime.Initialize(modules, 10'000, EmitterBackend::Gpu))
        Utility::Print(m_Runtime.LastError().c_str());

    Utility::Print(DescribeUber(m_Runtime.Uber()).c_str());
}

void MyProject::UpdateImGui(float deltaT)
{
    ImGui::SetNextWindowPos(ImVec2(10.f, 10.f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Debug");
    ImGui::Text("%.2f ms (%.1f FPS)", deltaT * 1000.f, deltaT > 0.f ? 1.f / deltaT : 0.f);
    ImGui::Text("Mode: %s  [F1 to toggle]", ImGuiLayer::IsCameraMode() ? "Camera" : "UI");
    ImGui::Checkbox("Show ImGui Demo", &m_ShowImGuiDemo);
    ImGui::End();

    ImGui::Begin("Particle");

    ImGui::SeparatorText("Time");
    ImGui::Checkbox("Time stop", &m_TimeStop);
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_TimeStop);
    if (ImGui::Button("Step"))
        m_StepOnce = true;
    ImGui::EndDisabled();

    ImGui::SeparatorText("Owner");
    ImGui::SliderFloat("Rotate (deg/s)", &m_RotateSpeedDeg, 0.f, 360.f);
    ImGui::Checkbox("Move up/down (velocity)", &m_MoveOwner);

    if (!m_UseTestScene)
        DrawModuleStackPanel(m_Runtime);

    UpdateTestSceneImGui();


    ImGui::End();


    if (m_ShowImGuiDemo)
        ImGui::ShowDemoWindow(&m_ShowImGuiDemo);
}

void MyProject::UpdateMiddleDragLook()
{
    // Camera 모드에서는 기존 FPS 방식이 담당
    if (ImGuiLayer::IsCameraMode() || !m_FlyCam)
    {
        m_MiddleDragging = false;
        return;
    }

    ImGuiIO& io = ImGui::GetIO();

    // ImGui 창 위에서 누른 건 무시하고, 빈 화면에서 눌렀을 때만 드래그 시작
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && !io.WantCaptureMouse)
        m_MiddleDragging = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle))
        m_MiddleDragging = false;

    if (!m_MiddleDragging || (io.MouseDelta.x == 0.f && io.MouseDelta.y == 0.f))
        return;

    // 현재 바라보는 방향에서 heading / pitch 를 다시 구한다 (FlyingFPSCamera 생성자와 같은 기준)
    const Vector3 f = m_Camera.GetForwardVec();
    const float fx = float(f.GetX()), fy = float(f.GetY()), fz = float(f.GetZ());

    float pitch = asinf(fy < -1.f ? -1.f : (fy > 1.f ? 1.f : fy));
    float heading = atan2f(-fx, -fz);

    const float kSensitivity = 0.005f;                 // 라디안 / 픽셀
    const float kPitchLimit = XM_PIDIV2 - 0.01f;      // 정확히 수직이 되면 방향이 뒤집혀서 살짝 덜 간다

    heading -= io.MouseDelta.x * kSensitivity;         // 오른쪽으로 끌면 오른쪽을 본다
    pitch -= io.MouseDelta.y * kSensitivity;         // 위로 끌면 위를 본다
    pitch = (std::max)(-kPitchLimit, (std::min)(kPitchLimit, pitch));

    m_FlyCam->SetHeadingPitchAndPosition(heading, pitch, m_Camera.GetPosition());
}


void MyProject::UpdateTestSceneImGui()
{
    ImGui::SeparatorText("Test Scene");

    ImGui::BeginDisabled(m_Recorder.IsBusy() || m_BenchRunning);      // 기록 중에는 조건 변경 금지

    if (ImGui::Checkbox("Use test scene", &m_UseTestScene) && m_UseTestScene && m_TestScene.Empty())
        m_TestScene.Build(m_TestDraft);
    ImGui::Checkbox("Fixed dt (1/60)", &m_FixedDt);

    {
        bool stageMajor = m_TestScene.StageMajor();
        if (ImGui::Checkbox("Stage-major order (all emitters per stage)", &stageMajor))
            m_TestScene.SetStageMajor(stageMajor);
    }

    {
        // 렌더: 이미터 사이까지 섞어서 정렬 (한 목록, Draw 1번) / 이미터마다 따로 정렬 (Draw N번)
        bool globalSort = m_TestScene.GlobalSort();
        if (ImGui::Checkbox("Global particle sort (across emitters)", &globalSort))
            m_TestScene.SetGlobalSort(globalSort);

        // 전체 정렬 경로에서만 의미가 있다
        ImGui::BeginDisabled(!globalSort);
        ImGui::Indent();
        bool culling = m_TestScene.GatherCulling();
        if (ImGui::Checkbox("Gather culling", &culling))
            m_TestScene.SetGatherCulling(culling);
        bool excludeAdditive = m_TestScene.ExcludeAdditiveFromSort();
        if (ImGui::Checkbox("Exclude additive from sort", &excludeAdditive))
            m_TestScene.SetExcludeAdditiveFromSort(excludeAdditive);
        ImGui::Unindent();
        ImGui::EndDisabled();
    }

    {
        int mode = int(UberGpuProgram::Mode());
        ImGui::TextUnformatted("Sim mode:");
        ImGui::SameLine();
        const bool a = ImGui::RadioButton("Generated", &mode, int(GpuSimMode::Generated));
        ImGui::SameLine();
        const bool c = ImGui::RadioButton("Uber", &mode, int(GpuSimMode::Uber));
        if (a || c)
        {
            UberGpuProgram::SetMode(GpuSimMode(mode));
            strcpy_s(m_RecordTag, UberGpuProgram::ModeName(GpuSimMode(mode)));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", UberGpuProgram::StatusText());
    }


    // N 바로 고르기 (누르면 바로 다시 만든다)
    static const uint32_t kCounts[] = { 1, 10, 100 };
    for (int i = 0; i < IM_ARRAYSIZE(kCounts); ++i)
    {
        if (i > 0) ImGui::SameLine();
        char label[16];
        sprintf_s(label, "N = %u", kCounts[i]);
        if (ImGui::Button(label))
        {
            m_TestDraft.count = kCounts[i];
            m_UseTestScene = true;
            m_TestScene.Build(m_TestDraft);
        }
    }

    int count = int(m_TestDraft.count);
    int capacity = int(m_TestDraft.capacityPerEmitter);
    if (ImGui::InputInt("Count", &count))                 m_TestDraft.count = uint32_t((std::max)(count, 1));
    if (ImGui::InputInt("Capacity / emitter", &capacity)) m_TestDraft.capacityPerEmitter = uint32_t((std::max)(capacity, 64));
    ImGui::SliderFloat("Spacing", &m_TestDraft.spacing, 0.5f, 5.f, "%.1f");

    // ---- 에셋 (고르면 프리셋 / 콤보 대신 사용) ----
    if (ImGui::TreeNode("Emitter assets (Assets/Emitters)"))
    {
        // 매 프레임 폴더를 읽지 않도록 처음 한 번 + Refresh 때만
        if (!m_TestAssetListRead || ImGui::SmallButton("Refresh"))
        {
            m_TestAssetList = ListEmitterAssets();
            m_TestAssetListRead = true;
        }
        if (m_TestAssetList.empty())
            ImGui::TextDisabled("(no assets - save one from the emitter panel)");

        std::vector<std::string>& picked = m_TestDraft.assets;
        for (const std::string& name : m_TestAssetList)
        {
            const auto it = std::find(picked.begin(), picked.end(), name);
            bool on = it != picked.end();
            if (ImGui::Checkbox(name.c_str(), &on))
            {
                if (on)
                    picked.push_back(name);
                else
                    picked.erase(it);
            }
        }
        // 목록에서 사라진 파일 (지웠거나 이름을 바꿈) 도 보여 줘야 끌 수 있다
        for (size_t i = 0; i < picked.size(); )
        {
            if (std::find(m_TestAssetList.begin(), m_TestAssetList.end(), picked[i]) != m_TestAssetList.end())
            {
                ++i;
                continue;
            }
            bool on = true;
            ImGui::Checkbox((picked[i] + " (missing)").c_str(), &on);
            if (!on)
                picked.erase(picked.begin() + ptrdiff_t(i));
            else
                ++i;
        }

        ImGui::Checkbox("Use asset capacity (else Capacity / emitter)", &m_TestDraft.assetCapacity);
        ImGui::TreePop();
    }

    const bool useAssets = !m_TestDraft.assets.empty();
    if (useAssets)
        ImGui::TextDisabled("Using %u asset(s): spawn rate / presets / combos are ignored", uint32_t(m_TestDraft.assets.size()));

    ImGui::BeginDisabled(useAssets);
    ImGui::SliderFloat("Spawn rate / emitter", &m_TestDraft.spawnRate, 1.f, 2000.f, "%.0f");

    for (uint32_t i = 0; i < uint32_t(EmitterPreset::Count); ++i)
    {
        if (i > 0) ImGui::SameLine();
        bool on = (m_TestDraft.presetMask & (1u << i)) != 0;
        if (ImGui::Checkbox(PresetName(EmitterPreset(i)), &on))
            m_TestDraft.presetMask = on ? (m_TestDraft.presetMask | (1u << i)) : (m_TestDraft.presetMask & ~(1u << i));
    }

    int combos = int(m_TestDraft.comboCount);
    if (ImGui::SliderInt("Module combos (0 = presets)", &combos, 0, int(kMaxModuleCombos)))
        m_TestDraft.comboCount = uint32_t(combos);
    if (m_TestDraft.comboCount > 0)
        ImGui::TextDisabled("first: %s", ComboName(0).c_str());
    ImGui::EndDisabled();

    if (ImGui::Button("Build"))
    {
        m_UseTestScene = true;
        m_TestScene.Build(m_TestDraft);
    }

    ImGui::EndDisabled();

    // ---------- 측정값 ----------
    const EmitterGpuProgram::Stats& ps = EmitterGpuProgram::GetStats();
    const EmitterTestScene::BuildResult& b = m_TestScene.LastBuild();

    ImGui::Text("Emitters      : %u / %u", m_TestScene.Count(), b.requested);
    ImGui::Text("Alive         : %u / %u", m_TestScene.AliveTotal(), m_TestScene.CapacityTotal());
    ImGui::Text("Generated PSOs: %u  (%u programs x %u + Finalize)",
        ps.livePrograms * EmitterGpuProgram::kNumKernels + 1, ps.livePrograms, uint32_t(EmitterGpuProgram::kNumKernels));
    ImGui::Text("Uber PSOs     : %u + Finalize", UberGpuProgram::kNumPSOs);
    ImGui::Text("Uber emitters : %u / %u", m_LastSimStats.uberEmitters, m_LastSimStats.emitters);
    if (m_LastSimStats.uberFallbacks > 0)
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "  %u emitters fell back to Generated (see Output window)", m_LastSimStats.uberFallbacks);
    ImGui::Text("PSO changes   : %u / frame", m_LastSimStats.psoChanges);
    ImGui::Text("Dispatches    : %u / frame", m_LastSimStats.dispatches);
    ImGui::Text("Sim GPU time  : %.3f ms", EngineProfiling::GetGpuTimeLast(kSimTimerName));
    ImGui::Text("Setup         : %.1f ms (shader build %.1f ms, %u builds)", b.setupMs, b.shaderBuildMs, b.shaderBuilds);
    if (!m_TestScene.LastError().empty())
        ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "Some emitters failed (see Output window)");
    
    DrawBenchmarkImGui();

    // ---------- 기록 ----------
    ImGui::InputText("Tag", m_RecordTag, sizeof(m_RecordTag));
    ImGui::InputInt("Frames", &m_RecordFrames);
    // Space (패드 Start) 는 엔진 프로파일러를 멈춘다. 멈춘 동안은 타이머가 갱신되지 않아 기록도 진행되지 않는다
    if (EngineProfiling::IsPaused())
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "Profiler paused (Space) - recording is on hold");

    if (m_Recorder.IsBusy())
        ImGui::ProgressBar(m_Recorder.GetProgress(), ImVec2(-1.f, 0.f), m_Recorder.GetStateText());
    else if (m_CsvPending)
    {
        // 새로 기록하면 아직 못 쓴 줄의 샘플이 지워지므로 먼저 처리하게 한다
        ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "%s", m_CsvStatus.c_str());
        if (ImGui::SmallButton("Retry now"))
            SaveCsvRow(m_PendingCsv);
        ImGui::SameLine();
        if (ImGui::SmallButton("Discard row"))
        {
            m_CsvPending = false;
            m_CsvStatus = "Row discarded";
        }
    }
    else
    {
        if (ImGui::Button("Record"))
            m_Recorder.Start(120, uint32_t(m_RecordFrames > 0 ? m_RecordFrames : 1));
        if (!m_CsvStatus.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", m_CsvStatus.c_str());
        }
    }
}

void MyProject::SaveCsvRow(const CsvRow& row)
{
    std::string full = row.path;
    char fullBuf[MAX_PATH] = {};
    if (GetFullPathNameA(row.path.c_str(), MAX_PATH, fullBuf, nullptr) > 0)
        full = fullBuf;

    if (m_Recorder.AppendCSV(row.path, row.header, row.values, row.tag, row.capacity))
    {
        m_CsvPending = false;
        m_CsvStatus = "Saved -> " + full;
        Utility::Print(("[Record] " + m_CsvStatus + "\n").c_str());
        return;
    }

    char reason[128] = {};
    strerror_s(reason, errno);
    m_PendingCsv = row;
    m_CsvPending = true;
    m_CsvRetryWait = 60;    // 약 1 초 뒤 다시
    m_CsvStatus = std::string("CSV save failed (") + reason + "), retrying: " + full;
    Utility::Print(("[Record] " + m_CsvStatus + "\n").c_str());
}

void MyProject::TickRecorder()
{
    if (m_CsvPending && m_CsvRetryWait > 0 && --m_CsvRetryWait == 0)
        SaveCsvRow(m_PendingCsv);

    const uint32_t alive = m_UseTestScene ? m_TestScene.AliveTotal() : m_Runtime.AliveCount();
    if (!m_Recorder.Tick(alive))
        return;

    // 기록이 끝난 프레임: 조건과 함께 CSV 한 줄
    const EmitterGpuProgram::Stats& ps = EmitterGpuProgram::GetStats();
    const uint32_t emitters = m_UseTestScene ? m_TestScene.Count() : 1u;
    const uint32_t capacity = m_UseTestScene ? m_TestScene.CapacityTotal() : m_Runtime.Capacity();
    const GpuSimFrameStats& fs = m_LastSimStats;
    const bool anyGenerated = fs.emitters > fs.uberEmitters;
    const uint32_t psoCount = (fs.uberEmitters > 0 ? UberGpuProgram::kNumPSOs : 0u)
        + (anyGenerated ? ps.livePrograms * EmitterGpuProgram::kNumKernels : 0u) + 1;   // +1 = 공용 Finalize
    const EmitterTestScene::BuildResult& b = m_TestScene.LastBuild();

    const std::string header = "emitters,capPerEmitter,spawnRate,fixedDt,psoCount,psoChanges,dispatches,setupMs,shaderBuildMs";
    // 에셋 capacity 를 쓰면 이미터마다 다를 수 있어서 평균을 남긴다
    const EmitterTestScene::Settings& applied = m_TestScene.AppliedSettings();
    const uint32_t capPerEmitter = !m_UseTestScene ? m_Runtime.Capacity()
        : (!applied.assets.empty() && applied.assetCapacity) ? m_TestScene.CapacityTotal() / (std::max)(m_TestScene.Count(), 1u)
        : applied.capacityPerEmitter;

    char values[256];
    sprintf_s(values, "%u,%u,%.1f,%d,%u,%u,%u,%.2f,%.2f",
        emitters,
        capPerEmitter,
        m_UseTestScene ? m_TestScene.AppliedSettings().spawnRate : 0.f,
        m_FixedDt ? 1 : 0,
        psoCount, m_LastSimStats.psoChanges, m_LastSimStats.dispatches,
        m_UseTestScene ? b.setupMs : 0.0, m_UseTestScene ? b.shaderBuildMs : 0.0);

    if (m_BenchRecording)
    {
        m_BenchRecording = false;
        const EmitterTestScene::Settings& ts = m_TestScene.AppliedSettings();
        uint32_t presetCount = 0;
        for (uint32_t i = 0; i < uint32_t(EmitterPreset::Count); ++i)
            presetCount += (ts.presetMask >> i) & 1u;
        const uint32_t variants = !ts.assets.empty() ? uint32_t(ts.assets.size())
            : ts.comboCount > 0 ? (std::min)(ts.comboCount, kMaxModuleCombos) : (std::max)(presetCount, 1u);

        char extra[128];
        sprintf_s(extra, ",%s,%u,%d,%u,%u",
            UberGpuProgram::ModeName(UberGpuProgram::Mode()), ps.livePrograms,
            EmitterRuntime::ProgramCacheEnabled() ? 1 : 0, fs.uberEmitters, variants);
        SaveCsvRow({ kBenchCsvPath, header + ",mode,generatedPrograms,cache,uberEmitters,shaderVariants",
            std::string(values) + extra, m_RecordTag, capacity });
        return;
    }

    SaveCsvRow({ "ParticleProfile_week5.csv", header, values, m_RecordTag, capacity });
}
void MyProject::StartBenchmark()
{
    m_BenchSteps.clear();
    for (uint32_t i = 0; i < kBenchCountOptions; ++i)
    {
        if (!m_BenchUseCount[i])
            continue;
        for (int order = 0; order < 2; ++order)
        {
            const bool stage = order == 1;
            if ((stage && !m_BenchStageOrder) || (!stage && !m_BenchEmitterOrder))
                continue;
            m_BenchSteps.push_back({ kBenchCounts[i], GpuSimMode::Generated, stage });
            m_BenchSteps.push_back({ kBenchCounts[i], GpuSimMode::Uber, stage });
        }
    }
    m_BenchIndex = 0;
    m_BenchRunning = !m_BenchSteps.empty();
    m_UseTestScene = true;
    m_FixedDt = true;
}

void MyProject::TickBenchmark()
{
    // 못 쓴 줄이 있으면 다음 칸으로 넘어가지 않는다 (다음 Start 가 샘플을 지움)
    if (!m_BenchRunning || m_Recorder.IsBusy() || m_CsvPending)
        return;

    if (m_BenchIndex >= m_BenchSteps.size())
    {
        m_BenchRunning = false;
        Utility::Printf("[Benchmark] done -> %s\n", kBenchCsvPath);
        return;
    }

    const BenchStep& s = m_BenchSteps[m_BenchIndex++];

    // 모드를 먼저 바꿔야 Build 가 필요한 셰이더만 만든다 (Uber 면 컴파일 없음)
    UberGpuProgram::SetMode(s.mode);
    m_TestScene.SetStageMajor(s.stageMajor);
    sprintf_s(m_RecordTag, "%s%s", UberGpuProgram::ModeName(s.mode), s.stageMajor ? "+Stage" : "");

    m_TestDraft.count = s.count;
    m_TestScene.Build(m_TestDraft);

    m_BenchRecording = true;
    m_Recorder.Start(uint32_t((std::max)(m_BenchWarmup, 1)), uint32_t((std::max)(m_RecordFrames, 1)));

    char msg[96];
    sprintf_s(msg, "[Benchmark] %u / %u : N = %u, %s\n", uint32_t(m_BenchIndex), uint32_t(m_BenchSteps.size()),
        s.count, UberGpuProgram::ModeName(s.mode));
    Utility::Print(msg);
}

void MyProject::DrawBenchmarkImGui()
{
    ImGui::SeparatorText("Benchmark: sim GPU time vs emitter count");

    // 공정한 비교: 생성 방식도 조합마다 한 번만 컴파일하고, 같은 조합끼리 프로그램을 같이 쓰게 한다
    bool cache = EmitterRuntime::ProgramCacheEnabled();
    ImGui::BeginDisabled(m_BenchRunning);
    if (ImGui::Checkbox("Share generated programs (cache)", &cache))
        EmitterRuntime::SetProgramCacheEnabled(cache);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("alive: %u, cache hits: %u", EmitterGpuProgram::GetStats().livePrograms, EmitterRuntime::ProgramCacheHits());

    ImGui::BeginDisabled(m_BenchRunning);
    ImGui::TextUnformatted("N:");
    for (uint32_t i = 0; i < kBenchCountOptions; ++i)
    {
        ImGui::SameLine();
        char label[16];
        sprintf_s(label, "%u##bench", kBenchCounts[i]);
        ImGui::Checkbox(label, &m_BenchUseCount[i]);
    }
    ImGui::InputInt("Warmup frames", &m_BenchWarmup);
    ImGui::Checkbox("Emitter order", &m_BenchEmitterOrder);
    ImGui::SameLine();
    ImGui::Checkbox("Stage order", &m_BenchStageOrder);
    ImGui::EndDisabled();
    ImGui::TextDisabled("record frames = 'Frames' below, capacity / spawn rate / presets = Test Scene settings");

    if (!m_BenchRunning)
    {
        if (ImGui::Button("Run benchmark"))
            StartBenchmark();
    }
    else
    {
        const BenchStep& s = m_BenchSteps[m_BenchIndex > 0 ? m_BenchIndex - 1 : 0];
        ImGui::Text("Running %u / %u : N = %u, %s", uint32_t(m_BenchIndex), uint32_t(m_BenchSteps.size()),
            s.count, UberGpuProgram::ModeName(s.mode));
        ImGui::SameLine();
        if (ImGui::Button("Stop"))
            m_BenchRunning = false;     // 진행 중인 칸은 끝까지 기록된다
    }
}
