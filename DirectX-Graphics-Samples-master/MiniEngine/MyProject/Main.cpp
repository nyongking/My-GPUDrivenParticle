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
#include "LightManager.h"
#include "DebugDraw.h"
#include "TexturedQuad.h"
#include "ImGuiLayer.h"

#include "ParticleSystem.h"
#include "ParticleUtility.h"

#include "ProfileRecorder.h"

#include "ModuleLibrary.h"
#include "EmitterRuntime.h"

#include "EmitterTestScene.h"

#include <cmath>
#include "ModuleStackPanel.h"

using namespace GameCore;
using namespace Graphics;

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

    static constexpr const wchar_t* kSimTimerName = L"Particle Simulation";   // 모든 이미터 시뮬레이션의 GPU 타이머

    EmitterTestScene            m_TestScene;
    EmitterTestScene::Settings  m_TestDraft;          // UI 가 편집, Build 를 눌러야 적용
    bool                        m_UseTestScene = false;
    bool                        m_FixedDt = true;     // 측정 재현성을 위해 1/60 고정

    ProfileRecorder             m_Recorder;
    char                        m_RecordTag[64] = "Generated";
    int                         m_RecordFrames = 600;
    bool                        m_LastSaveOk = true;
    GpuSimFrameStats            m_LastSimStats;       // 지난 프레임 값 (UI 표시 / CSV)

    // -------- 평면 설정
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

    Renderer::Initialize();
    DebugDraw::Initialize();
    QuadRenderer::Initialize();

    // Camera Setting
    m_Camera.SetZRange(0.1f, 1000.f);
    m_Camera.SetEyeAtUp(Vector3(0.f, 5.f, -15.f), Vector3(0.f, 0.f, 0.f), Vector3(Math::kYUnitVector));

    auto NewCam = new FlyingFPSCamera(m_Camera, Vector3(Math::kYUnitVector));
    NewCam->SetMoveSpeed(10.f);
    NewCam->SetStrafeSpeed(10.f);
    m_CameraController.reset(NewCam);
    m_FlyCam = NewCam;

    //// Setup your data
    //m_Draft.maxParticles = 1'000'000;
    //m_Draft.steadyRatio = 0.5f;
    //m_Particles.Initialize(m_Draft, Vector3(0.f, 0.f, 0.f));

    m_Recorder.SetTimers({ kSimTimerName });

    InitializeEmitter();

    m_Floor.SetFloor(Vector3(0.f, -1.f, 0.f), 20.f);
    m_Floor.SetTint(Color(0.35f, 0.35f, 0.38f));
}

void MyProject::Cleanup(void)
{
    // Free up resources in an orderly fashion
    m_TestScene.Shutdown();
    m_Runtime.Shutdown();

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


    // Update something
    m_CameraController->Update(deltaT);

    m_Runtime.ApplyPending();     // UI 가 지난 프레임에 남긴 변경을 Tick 전에 적용

    const float simDt = m_FixedDt ? (1.f / 60.f) : deltaT;

    // 소유자: Y 축 회전 + (선택) 위아래 이동
    m_OwnerTime += deltaT;
    m_OwnerAngle += m_RotateSpeedDeg * (3.14159265f / 180.f) * deltaT;
    const float bob = m_MoveOwner ? std::sin(m_OwnerTime * 2.f) : 0.f;   // 위아래 1m

    OwnerTransform owner;
    owner.position = { m_EmitterPos.x, m_EmitterPos.y + bob, m_EmitterPos.z };
    owner.rotation = QuatFromAxisAngle(float3{ 0.f, 1.f, 0.f }, m_OwnerAngle);

    {
        GpuEmitterInstance::BeginFrameStats();
        ComputeContext& simCtx = ComputeContext::Begin(kSimTimerName);

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
    m_Floor.Draw(gfxContext);
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

    if (!m_Runtime.Initialize(modules, 100'000, EmitterBackend::Gpu))
        Utility::Print(m_Runtime.LastError().c_str());
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

    ImGui::SeparatorText("Owner");
    ImGui::SliderFloat("Rotate (deg/s)", &m_RotateSpeedDeg, 0.f, 360.f);
    ImGui::Checkbox("Move up/down (velocity)", &m_MoveOwner);

    DrawModuleStackPanel(m_Runtime);

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
    ImGui::SeparatorText("Test Scene (week 5)");

    ImGui::BeginDisabled(m_Recorder.IsBusy());      // 기록 중에는 조건 변경 금지

    if (ImGui::Checkbox("Use test scene", &m_UseTestScene) && m_UseTestScene && m_TestScene.Empty())
        m_TestScene.Build(m_TestDraft);
    ImGui::Checkbox("Fixed dt (1/60)", &m_FixedDt);

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
    ImGui::SliderFloat("Spawn rate / emitter", &m_TestDraft.spawnRate, 1.f, 2000.f, "%.0f");
    ImGui::SliderFloat("Spacing", &m_TestDraft.spacing, 0.5f, 5.f, "%.1f");

    for (uint32_t i = 0; i < uint32_t(EmitterPreset::Count); ++i)
    {
        if (i > 0) ImGui::SameLine();
        bool on = (m_TestDraft.presetMask & (1u << i)) != 0;
        if (ImGui::Checkbox(PresetName(EmitterPreset(i)), &on))
            m_TestDraft.presetMask = on ? (m_TestDraft.presetMask | (1u << i)) : (m_TestDraft.presetMask & ~(1u << i));
    }

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
    ImGui::Text("Compute PSOs  : %u  (%u programs x %u + Finalize)",
        ps.livePrograms * EmitterGpuProgram::kNumKernels + 1, ps.livePrograms, uint32_t(EmitterGpuProgram::kNumKernels));
    ImGui::Text("PSO changes   : %u / frame", m_LastSimStats.psoChanges);
    ImGui::Text("Dispatches    : %u / frame", m_LastSimStats.dispatches);
    ImGui::Text("Sim GPU time  : %.3f ms", EngineProfiling::GetGpuTimeLast(kSimTimerName));
    ImGui::Text("Setup         : %.1f ms (shader build %.1f ms, %u builds)", b.setupMs, b.shaderBuildMs, b.shaderBuilds);
    if (!m_TestScene.LastError().empty())
        ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "Some emitters failed (see Output window)");

    // ---------- 기록 ----------
    ImGui::InputText("Tag", m_RecordTag, sizeof(m_RecordTag));
    ImGui::InputInt("Frames", &m_RecordFrames);
    if (m_Recorder.IsBusy())
        ImGui::ProgressBar(m_Recorder.GetProgress(), ImVec2(-1.f, 0.f), m_Recorder.GetStateText());
    else if (ImGui::Button("Record"))
        m_Recorder.Start(120, uint32_t(m_RecordFrames > 0 ? m_RecordFrames : 1));
    if (!m_LastSaveOk)
        ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "CSV save failed (open in Excel?)");
}

void MyProject::TickRecorder()
{
    const uint32_t alive = m_UseTestScene ? m_TestScene.AliveTotal() : m_Runtime.AliveCount();
    if (!m_Recorder.Tick(alive))
        return;

    // 기록이 끝난 프레임: 조건과 함께 CSV 한 줄
    const EmitterGpuProgram::Stats& ps = EmitterGpuProgram::GetStats();
    const uint32_t emitters = m_UseTestScene ? m_TestScene.Count() : 1u;
    const uint32_t capacity = m_UseTestScene ? m_TestScene.CapacityTotal() : m_Runtime.Capacity();
    const uint32_t psoCount = ps.livePrograms * EmitterGpuProgram::kNumKernels + 1;   // +1 = 공용 Finalize
    const EmitterTestScene::BuildResult& b = m_TestScene.LastBuild();

    const std::string header = "emitters,capPerEmitter,spawnRate,fixedDt,psoCount,psoChanges,dispatches,setupMs,shaderBuildMs";
    char values[256];
    sprintf_s(values, "%u,%u,%.1f,%d,%u,%u,%u,%.2f,%.2f",
        emitters,
        m_UseTestScene ? m_TestScene.AppliedSettings().capacityPerEmitter : m_Runtime.Capacity(),
        m_UseTestScene ? m_TestScene.AppliedSettings().spawnRate : 0.f,
        m_FixedDt ? 1 : 0,
        psoCount, m_LastSimStats.psoChanges, m_LastSimStats.dispatches,
        m_UseTestScene ? b.setupMs : 0.0, m_UseTestScene ? b.shaderBuildMs : 0.0);

    m_LastSaveOk = m_Recorder.AppendCSV("ParticleProfile_week5.csv", header, values, m_RecordTag, capacity);
}

