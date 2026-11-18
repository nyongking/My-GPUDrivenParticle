# 멀티 텍스처링 (스프라이트 텍스처 레이어) 보관본

`UberShader_Opt` 브랜치에서 뺀 **멀티 텍스처링 기능**을 다시 넣을 수 있게 보관한 폴더입니다.
이 폴더는 빌드에 포함되지 않습니다 (`MyProject.vcxproj` 는 파일을 이름으로만 등록하고, 이 폴더는 프로젝트 밖에 있습니다).

- 제거 커밋: `cc8bf2d` (멀티 텍스처링(텍스처 레이어) 제거 -> ref/MultiTexture 로 보관)
- 제거 직전 상태: `3f65f7d`
- 원래 만든 커밋: `135733d` (디졸브) -> `bf54a46` (범용 텍스처 레이어) + `b76d4ce` 의 샘플러 수 수정

## 들어 있는 것

| 경로 | 내용 |
|---|---|
| `MultiTexture.patch` | **복구용.** 제거 커밋(`cc8bf2d`) 기준으로 코드 21 개 파일을 멀티 텍스처링 상태로 되돌리는 패치 (새 파일 3 개 포함, 샘플러 수 `Reset(kNumRootParams, 2)` 수정 포함) |
| `files/` | **읽기용.** 멀티 텍스처링이 들어 있던 때의 전체 파일 (원래 인코딩 그대로: CP949 / UTF-8 BOM, CRLF) |

`files/` 에는 `Main.cpp`, `MyProject.vcxproj`, `.filters` 를 넣지 않았습니다. 이 파일들은 다른 작업으로 계속 바뀌어서
통째로 보관하면 금방 맞지 않게 됩니다. 이 파일들의 변경 (시간 전달 2 줄, 새 파일 등록) 은 패치에만 있습니다.

**빠진 것 (일부러 보관하지 않음):** 그때 만든 파티클 데이터 `Fire.emitter.json`, `Smoke.emitter.json`,
Erode 용 노이즈 `T_ErodeNoise.png` (+ `.dds`). 필요하면 커밋 `571306a` 에서 꺼낼 수 있습니다.

```bash
git show 571306a:DirectX-Graphics-Samples-master/MiniEngine/MyProject/Assets/Textures/T_ErodeNoise.png > T_ErodeNoise.png
```

## 다시 넣는 방법

저장소 루트에서 실행합니다. 두 방법 모두 결과는 같습니다 (패치는 적용 후 `3f65f7d` 와 내용이 같은 것을 확인함).

**방법 1. 패치 적용 (코드만 복구, 권장)**

```bash
git apply --check ref/MultiTexture/MultiTexture.patch
```

```bash
git apply ref/MultiTexture/MultiTexture.patch
```

그 사이에 같은 파일을 고쳐서 `--check` 가 실패하면 3-way 병합으로 적용하고 충돌만 손으로 고칩니다.

```bash
git apply -3 ref/MultiTexture/MultiTexture.patch
```

**방법 2. 제거 커밋 되돌리기 (코드 + 불 / 연기 데이터까지 복구)**

```bash
git revert cc8bf2d
```

## 다시 넣은 뒤 확인할 것

1. 새 파일 3 개가 프로젝트에 등록됐는지: `ParticleMaterial.h/.cpp`, `Shaders/SpriteMaterial.hlsli` (패치가 `.vcxproj` / `.filters` 에 같이 넣음)
2. `SpriteRenderer.cpp` 의 `s_RootSig.Reset(kNumRootParams, 2)` (wrap 샘플러 `s1` 때문에 2 여야 함. 1 이면 `InitStaticSampler` ASSERT)
3. Debug / Release 빌드, 레이어를 모두 끈 화면이 지금과 같은지

## 참고

- 지금 (제거된 상태) 에서 `Fire_New.emitter.json`, `Test.emitter.json` 을 불러오면 `MaterialParam0 / 1` 바인딩 칸이 없어서
  "unknown binding slot ... -> ignored" 경고가 2 개 나옵니다. 동작에는 문제가 없고, 멀티 텍스처링을 다시 넣으면 사라집니다.
  (`"layers": []` 항목은 경고 없이 무시됩니다)
- `T_FireDistNoise.dds` 는 레이어(선형) 로 쓰일 때 변환된 파일입니다. 이 텍스처를 베이스 색 텍스처로 쓰면 색이 연하게 보일 수 있으니,
  그때는 `.dds` 를 지워서 sRGB 로 다시 변환되게 하세요.

## 이해할 때 읽는 순서 (요약)

1. `files/ParticleMaterial.h` : 레이어 하나의 설정 (`SpriteLayer`), 연산 종류
2. `files/Shaders/SpriteMaterial.hlsli` : 실제 계산 (`LayerParam`, `LayerUV`, `ApplyLayer`, `ErodeLayer`)
3. `files/Shaders/ParticleSpritePS.hlsl` `main` : 1) Distort -> 2) 베이스 -> 3) 나머지 레이어
4. `files/SpriteRenderer.cpp` `BindForDraw` / `PackLayers` : CPU 에서 켜진 레이어만 모아 넘김
5. `files/ParticleRenderList.cpp` `Add` + `files/Shaders/ParticleListPS.hlsl` : 전체 정렬 경로 (이미터 번호 x 4 로 레이어를 찾음)
