// #29 粒子视觉：封面 ↔ 滚筒 逐粒子 morph（fragment shader，配合默认 vertex shader）
// 架构：域变换（domain warping）。粒子永远在"粒子空间"的规则网格上（cellCenter），
//   每个像素把自己的屏幕 uv 变换到粒子空间 wuv，在 wuv 里找最近格子 → 必有粒子在附近。
//   cover 形态：warp=恒等 → 网格=点彩封面
//   tunnel 形态：warp=极坐标（屏幕→角度×半径）→ 网格变同心环=滚筒
//   morph：插值 warp，粒子始终可见（不会因移动而消失）
//
// 约定（Qt 6 ShaderEffect）：
//   - 只提供 fragment shader，内置 vertex shader 传入 qt_TexCoord0（location 0），值域 0..1
//   - uniform block binding 0 / std140，qt_Matrix offset 0、qt_Opacity offset 64，自定义成员排其后
//   - sampler 用 layout(binding=1) uniform sampler2D
//   - 输出 premultiplied
#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float uTime;
    float uMorphT;
    float uCell;
    float uCamRot;
    float uCamZoom;
    float uDriftX;
    float uDriftY;
    float uHasCover;
    vec2 uSize;
    vec2 uCoverSize;
    vec3 uFallback;
};

layout(binding = 1) uniform sampler2D cover;

float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

void main() {
    vec2 uv = qt_TexCoord0;                  // 0..1 屏幕空间
    vec2 grid = max(vec2(1.0), floor(uSize / uCell));

    // ---- 镜头缓动（整体旋转/缩放/漂移，绕画面中心，作用在屏幕 uv 上） ----
    vec2 cuv = uv - vec2(0.5);
    float ca = uCamRot;
    cuv = vec2(cuv.x * cos(ca) - cuv.y * sin(ca), cuv.x * sin(ca) + cuv.y * cos(ca));
    cuv = cuv * uCamZoom + vec2(0.5) + vec2(uDriftX, uDriftY);

    // ---- 域变换：cover(warp=恒等) ↔ tunnel(极坐标) ----
    vec2 centered = cuv - vec2(0.5);
    float r = length(centered);
    float ang = atan(centered.y, centered.x);            // -PI..PI
    // tunnel 粒子空间：x=角度（周向），y=半径+时间流动（径向，向内流）
    float tunnelY = clamp(r * 2.6 + uTime * 0.12, 0.0, 3.0);
    vec2 tunnelWarp = vec2(ang / 6.2831853 + 0.5, tunnelY);
    vec2 wuv = mix(cuv, tunnelWarp, uMorphT);

    // ---- 在粒子空间找最近格子的粒子 ----
    vec2 cell = floor(wuv * grid);
    vec2 cellCenter = (cell + 0.5) / grid;
    vec2 jitter = (vec2(hash(cell + 1.7), hash(cell + 9.2)) - 0.5) / grid * 0.6;
    vec2 particlePos = cellCenter + jitter;
    vec2 d = (wuv - particlePos) * grid;
    float dist = length(d);

    // ---- 封面色：在屏幕空间采样封面（点彩封面），fit 等比 ----
    float sa = uSize.x / max(uSize.y, 1.0);
    float ca2 = uCoverSize.x / max(uCoverSize.y, 1.0);
    vec2 fitted = (ca2 > sa) ? vec2(1.0, sa / ca2) : vec2(ca2 / sa, 1.0);
    vec2 coverUV2 = (cuv - vec2(0.5)) / fitted + vec2(0.5);
    bool inC = (coverUV2.x >= 0.0 && coverUV2.x <= 1.0 && coverUV2.y >= 0.0 && coverUV2.y <= 1.0);
    vec3 coverCol = uFallback;
    if (uHasCover > 0.5 && inC) coverCol = texture(cover, coverUV2).rgb;
    float area = inC ? 1.0 : 0.0;
    coverCol = mix(vec3(0.06, 0.08, 0.12), coverCol, area);

    // ---- 滚筒色：淡蓝白渐变，近中心（远端）更亮 ----
    float tunnelBright = 1.0 - smoothstep(0.0, 0.5, r);   // 中心亮、边缘暗
    vec3 tunnelCol = mix(vec3(0.35, 0.5, 0.75), vec3(0.85, 0.92, 1.0), tunnelBright);

    // ---- 颜色过渡 ----
    vec3 col = mix(coverCol, tunnelCol, uMorphT);
    col *= 1.0 - 0.12 * uMorphT;

    // ---- 粒子圆点 ----
    float h = hash(cell + 5.7);
    // tunnel 形态近大远小（中心粒子大）
    float sizeScale = mix(1.0, 0.7 + 0.6 * tunnelBright, uMorphT);
    float pr = 0.38 * (0.8 + 0.35 * h) * sizeScale;       // 半径（格单位）
    float a = 1.0 - smoothstep(pr * 0.4, pr, dist);
    // 呼吸闪烁
    a *= 0.72 + 0.28 * sin(uTime * 1.6 + h * 6.2831);
    // 封面区外粒子更暗（cover 形态）
    a *= mix(mix(0.35, 1.0, area), 1.0, uMorphT);

    fragColor = vec4(col * a, a) * qt_Opacity;
}
