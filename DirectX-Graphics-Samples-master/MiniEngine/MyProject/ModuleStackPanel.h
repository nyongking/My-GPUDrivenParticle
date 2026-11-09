#pragma once

class EmitterRuntime;

// 현재 ImGui 창 안에 백엔드 선택, 모듈 순서, 입력값 편집 UI 를 그린다
void DrawModuleStackPanel(EmitterRuntime& runtime);
