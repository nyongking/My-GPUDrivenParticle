// 빌보드 방향 계산 (ParticleSpriteVS / ParticleGatherCS 공용)
//   Facing    = 사각형 앞면이 어디를 보는가
//   Alignment = 사각형 위쪽 축을 어디에 맞추는가
//   결과 = 사각형 중심 + 가로 / 세로 반 축 (corner (+-1, +-1) 을 곱해서 꼭짓점을 만든다)
//
//   C++ SpriteFacingMode / SpriteAlignment 와 같은 번호
#ifndef SPRITE_FACING_HLSLI
#define SPRITE_FACING_HLSLI

#define FACING_CAMERA_PLANE     0   // 카메라 평면과 나란히 (모든 파티클이 같은 방향, 기존 방식)
#define FACING_CAMERA_POSITION  1   // 카메라 위치를 바라봄 (화면 가장자리에서도 정면, 카메라가 돌아도 기울지 않음)
#define FACING_CUSTOM           2   // SpriteFacing 벡터를 바라봄 (예: (0,1,0) = 바닥에 눕힌 데칼)
#define FACING_DISTANCE_BLEND   3   // 가까우면 CameraPosition, 멀면 CameraPlane 으로 섞음

#define ALIGN_UNALIGNED         0   // 위쪽 = 카메라 위쪽
#define ALIGN_VELOCITY          1   // 위쪽 = 속도 방향 (+ 속도만큼 늘이기)
#define ALIGN_CUSTOM            2   // 위쪽 = SpriteAlignment 벡터 (예: (0,1,0) = 세로로 선 빌보드)

struct SpriteCamera
{
    float3 Position;
    float3 Right;
    float3 Up;
    float3 Forward;
};

// 이미터 단위 설정 (cbuffer 값)
struct SpriteOptions
{
    uint Facing;
    uint Alignment;
    float2 Pivot; // uv 기준 (0.5, 0.5 = 가운데, 0.5, 1 = 아래 가운데가 파티클 위치)
    float CameraOffset; // 카메라 쪽으로 당기는 거리 (m, 벽 / 바닥에 묻히는 것 방지)
    float VelocityStretch; // ALIGN_VELOCITY: 세로 길이 += 속력 * 이 값
    float BlendNear; // FACING_DISTANCE_BLEND: 이 거리까지 CameraPosition
    float BlendFar; // 이 거리부터 CameraPlane
};

// 파티클 단위 값
struct SpriteShape
{
    float3 Position;
    float2 Size; // 가로 / 세로 전체 길이 (m)
    float Rotation; // 도 (앞면 기준 반시계)
    float3 Velocity;
    float3 Facing; // FACING_CUSTOM
    float3 Alignment; // ALIGN_CUSTOM
};

// v 를 normal 에 수직인 평면으로 투영 + 정규화. 너무 짧으면 fallback
float3 ProjectOnPlane(float3 v, float3 normal, float3 fallback)
{
    const float3 p = v - normal * dot(v, normal);
    const float len2 = dot(p, p);
    return (len2 > 1e-8f) ? p * rsqrt(len2) : fallback;
}

void ComputeSpriteAxes(SpriteShape s, SpriteOptions o, SpriteCamera cam,
    out float3 center, out float3 axisX, out float3 axisY)
{
    const float3 toCam = cam.Position - s.Position;
    const float dist = length(toCam);
    const float3 dirToCam = (dist > 1e-5f) ? toCam / dist : -cam.Forward;

    // ---- 1. 앞면 방향 N (카메라 쪽을 향함) ----
    float3 N = -cam.Forward;
    
    if (o.Facing == FACING_CAMERA_POSITION)
    {
        N = dirToCam;
    }
    else if (o.Facing == FACING_CUSTOM)
    {
        const float len2 = dot(s.Facing, s.Facing);
        N = (len2 > 1e-8f) ? s.Facing * rsqrt(len2) : -cam.Forward;
    }
    else if (o.Facing == FACING_DISTANCE_BLEND)
    {
        const float t = saturate((dist - o.BlendNear) / max(o.BlendFar - o.BlendNear, 1e-4f));
        N = normalize(lerp(dirToCam, -cam.Forward, t) + 1e-6f * cam.Up);
    }

    // ---- 2. 위쪽 / 오른쪽 축 ----
    float3 up, right;
    const bool planeUnaligned = (o.Facing == FACING_CAMERA_PLANE) && (o.Alignment == ALIGN_UNALIGNED);
    if (planeUnaligned)
    {
        // 기존 방식 그대로 (계산 없음)
        right = cam.Right;
        up = cam.Up;
    }
    else
    {
        // 좌표계 손잡이 (cross 결과가 카메라 오른쪽과 같은 쪽이 되도록). 텍스처가 좌우로 뒤집히지 않게
        const float handed = (dot(cross(cam.Up, -cam.Forward), cam.Right) >= 0.f) ? 1.f : -1.f;

        // 기준 위쪽: 정렬 벡터. 없으면 카메라 위쪽
        float3 align = cam.Up;
        bool aligned = false;
        if (o.Alignment == ALIGN_VELOCITY)
        {
            const float len2 = dot(s.Velocity, s.Velocity);
            aligned = len2 > 1e-8f;
            if (aligned)
                align = s.Velocity * rsqrt(len2);
            else
                align = cam.Up;
        }
        else if (o.Alignment == ALIGN_CUSTOM)
        {
            const float len2 = dot(s.Alignment, s.Alignment);
            aligned = len2 > 1e-8f;
            if (aligned)
                align = s.Alignment * rsqrt(len2);
            else
                align = cam.Up;
        }

        if (aligned)
        {
            // 위쪽은 정렬 벡터 그대로, 그 축을 중심으로 돌려서 N 을 최대한 바라보게
            //   (정렬 벡터가 카메라 쪽을 곧바로 향하면 cross 가 0 -> 카메라 오른쪽을 투영해서 씀)
            //
            //up = align;
            //const float fdot = dot(up, N);
            //right = normalize(cross(up, N - up * fdot));           
            
            up = align;
            right = ProjectOnPlane(cross(up, N) * handed, up, ProjectOnPlane(cam.Right, up, cam.Right));
        }
        else
        {
            // 위쪽 = 카메라 위쪽을 앞면 평면에 투영 (카메라 롤이 반영됨). 앞면이 위/아래를 보면 카메라 앞쪽으로 대신
            //const float fdot = dot(cam.Up, N);
            //up = normalize(cam.Up - N * fdot);
            //right = normalize(cross(up, N));
            
            up = ProjectOnPlane(cam.Up, N, ProjectOnPlane(cam.Forward, N, cam.Right));
            right = normalize(cross(up, N)) * handed;
        }
    }

    // ---- 3. 회전 (앞면 평면 안에서) ----
    if (s.Rotation != 0.f)
    {
        float sn, cs;
        sincos(radians(s.Rotation), sn, cs);
        const float3 r = right * cs + up * sn;
        const float3 u = up * cs - right * sn;
        right = r;
        up = u;
    }

    // ---- 4. 크기 (+ 속도 늘이기) ----
    float2 size = s.Size;
    if (o.Alignment == ALIGN_VELOCITY)
        size.y += length(s.Velocity) * o.VelocityStretch;

    axisX = right * (size.x * 0.5f);
    axisY = up * (size.y * 0.5f);

    // ---- 5. 피벗: uv(Pivot) 자리가 파티클 위치에 오도록 중심을 옮긴다 ----
    const float2 pivotCorner = float2(o.Pivot.x * 2.f - 1.f, 1.f - o.Pivot.y * 2.f);
    center = s.Position - pivotCorner.x * axisX - pivotCorner.y * axisY;

    // ---- 6. 카메라 쪽으로 당기기 ----
    center += dirToCam * o.CameraOffset;
}

#endif // SPRITE_FACING_HLSLI
