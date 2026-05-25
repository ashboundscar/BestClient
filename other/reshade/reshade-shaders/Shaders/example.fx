/*
    example.fx
    ----------
    Purpose:
    This file is a compact, working ReShade reference shader plus a capability map
    for the local shader library. It is meant to be given to another AI so it can
    infer what kinds of shaders this pack supports and how new shaders should be structured.

    Local pack snapshot used for this reference:
    - 491 *.fx files
    - 224 *.fxh files
    - 715 shader files total

    Repository-wide capabilities observed while building this file:
    - Includes/helpers:
      ReShade.fxh, ReShadeUI.fxh, Blending.fxh, DrawText.fxh, many project-specific *.fxh files
    - Global resources:
      ReShade::BackBufferTex, ReShade::DepthBufferTex, PostProcessVS(), GetLinearizedDepth()
    - UI annotations:
      ui_type = "slider" / "drag" / "combo" / "color" / "radio" / "input" / "list"
      ui_label, ui_tooltip, ui_text, ui_units, ui_category, ui_category_closed, ui_category_toggle, hidden
    - Source-driven uniforms:
      source = "timer", "frametime", "framecount", "mousepoint", "mousedelta",
      "mousebutton", "random", "pingpong", "key", plus overlay/depth-ready and matrix/vector sources in some packs
    - Textures and samplers:
      texture, texture2D, sampler, sampler2D, pooled render targets, mip chains, sRGB textures, external source textures
    - Techniques and passes:
      multipass post-process chains, hidden helper techniques, enabled/timeout annotations,
      blending, clear state, multi-render-target output, stencil state, render target masks
    - Compute/storage:
      ComputeShader, DispatchSizeX/Y, groupshared memory, storage / storage1D / storage2D / storage3D

    Keep the active code below reasonably simple and compile-friendly.
    Rarer features that would make the file heavier or renderer-dependent are documented as templates near the end.
*/

#include "ReShade.fxh"
#include "ReShadeUI.fxh"
#include "Blending.fxh"

namespace ExampleReference
{
	static const float PI = 3.14159265359;
	static const float3 LUMA_COEFF = float3(0.2126, 0.7152, 0.0722);

	// -------------------------------------------------------------------------
	// Source bindings
	// -------------------------------------------------------------------------

	uniform float Timer <
		source = "timer";
	>;

	uniform float FrameTime <
		source = "frametime";
	>;

	uniform int FrameCount <
		source = "framecount";
	>;

	uniform float2 MousePoint <
		source = "mousepoint";
	>;

	uniform float3 MouseDelta <
		source = "mousedelta";
	>;

	uniform int RandomValue <
		source = "random";
		min = 0;
		max = 32767;
	>;

	uniform float2 Phase <
		source = "pingpong";
		min = 0.0;
		max = 2.0;
		step = float2(0.125, 0.25);
		smoothing = 0.0;
	>;

	uniform bool ToggleOverlay <
		source = "key";
		keycode = 0x74;
		toggle = true;
		mode = "toggle";
	>;

	uniform bool LeftMouseDown <
		source = "mousebutton";
		keycode = 0;
		toggle = false;
	>;

	// -------------------------------------------------------------------------
	// UI examples
	// -------------------------------------------------------------------------

	uniform int ReadmeLine <
		ui_category = "example.fx";
		ui_category_closed = false;
		ui_text = "Working reference shader + capability map for the local ReShade pack.";
		hidden = true;
	> = 0;

	uniform bool EffectEnabled <
		ui_category = "example.fx";
		ui_category_toggle = true;
		ui_label = "Enable reference effect";
		ui_tooltip = "Master switch for the visible example pipeline.";
	> = true;

	BLENDING_COMBO(BlendMode, "Blend Mode", "Blend helper from Blending.fxh.", "example.fx", false, 0, 0)

	uniform int DebugView <
		ui_category = "example.fx";
		ui_label = "Debug View";
		ui_type = "combo";
		ui_items = "Final\0Original\0Blur\0Depth\0Depth Mask\0Luma\0Noise\0History\0";
		ui_tooltip = "Typical debug pattern used across many local shaders.";
	> = 0;

	uniform int ToneMapMode <
		__UNIFORM_LIST_INT1
		ui_category = "example.fx";
		ui_label = "Tone Map";
		ui_items = "Off\0Reinhard\0ACES-like\0";
		ui_tooltip = "List widget example via ReShadeUI.fxh.";
	> = 1;

	uniform int SplitCompare <
		__UNIFORM_RADIO_INT1
		ui_category = "example.fx";
		ui_label = "Split Compare";
		ui_items = "Off\0Vertical\0Horizontal\0";
		ui_tooltip = "Radio widget example via ReShadeUI.fxh.";
	> = 0;

	uniform float Strength <
		__UNIFORM_SLIDER_FLOAT1
		ui_category = "example.fx";
		ui_label = "Strength";
		ui_min = 0.0;
		ui_max = 1.0;
		ui_step = 0.01;
	> = 0.75;

	uniform float BlurRadius <
		__UNIFORM_SLIDER_FLOAT1
		ui_category = "example.fx";
		ui_label = "Blur Radius";
		ui_min = 0.5;
		ui_max = 3.0;
		ui_step = 0.05;
		ui_units = " px";
	> = 1.5;

	uniform float BloomAmount <
		__UNIFORM_SLIDER_FLOAT1
		ui_category = "example.fx";
		ui_label = "Bloom Amount";
		ui_min = 0.0;
		ui_max = 2.0;
		ui_step = 0.01;
	> = 0.6;

	uniform float Saturation <
		__UNIFORM_SLIDER_FLOAT1
		ui_category = "example.fx";
		ui_label = "Saturation";
		ui_min = 0.0;
		ui_max = 2.0;
		ui_step = 0.01;
	> = 1.1;

	uniform float TintAmount <
		__UNIFORM_SLIDER_FLOAT1
		ui_category = "example.fx";
		ui_label = "Tint Amount";
		ui_min = 0.0;
		ui_max = 1.0;
		ui_step = 0.01;
	> = 0.2;

	uniform float3 TintColor <
		__UNIFORM_COLOR_FLOAT3
		ui_category = "example.fx";
		ui_label = "Tint Color";
	> = float3(1.00, 0.96, 0.90);

	uniform float2 SampleOffsetPx <
		ui_category = "example.fx";
		ui_label = "Sample Offset";
		ui_type = "drag";
		ui_min = -8.0;
		ui_max = 8.0;
		ui_step = 0.05;
		ui_units = " px";
	> = float2(0.0, 0.0);

	uniform int ExtraSamples <
		ui_category = "example.fx";
		ui_label = "Extra Samples";
		ui_type = "input";
		ui_min = 0;
		ui_max = 8;
		ui_tooltip = "Input widget example. Kept small to avoid heavy loops.";
	> = 2;

	uniform bool UseDepthMask <
		ui_category = "example.fx";
		ui_label = "Use Depth Mask";
		ui_tooltip = "Demonstrates depth-aware compositing using ReShade::GetLinearizedDepth.";
	> = false;

	uniform bool InvertDepthMask <
		ui_category = "example.fx";
		ui_label = "Invert Depth Mask";
	> = false;

	uniform float DepthNear <
		__UNIFORM_SLIDER_FLOAT1
		ui_category = "example.fx";
		ui_label = "Depth Near";
		ui_min = 0.0;
		ui_max = 1.0;
		ui_step = 0.001;
	> = 0.15;

	uniform float DepthFar <
		__UNIFORM_SLIDER_FLOAT1
		ui_category = "example.fx";
		ui_label = "Depth Far";
		ui_min = 0.0;
		ui_max = 1.0;
		ui_step = 0.001;
	> = 0.85;

	uniform float NoiseAmount <
		__UNIFORM_SLIDER_FLOAT1
		ui_category = "example.fx";
		ui_label = "Noise Amount";
		ui_min = 0.0;
		ui_max = 0.25;
		ui_step = 0.001;
	> = 0.03;

	// -------------------------------------------------------------------------
	// Local render targets and samplers
	// -------------------------------------------------------------------------

	texture DownsampleTex < pooled = true; >
	{
		Width = BUFFER_WIDTH / 2;
		Height = BUFFER_HEIGHT / 2;
		Format = RGBA8;
	};

	texture BlurTex < pooled = true; >
	{
		Width = BUFFER_WIDTH / 2;
		Height = BUFFER_HEIGHT / 2;
		Format = RGBA8;
	};

	texture HistoryTex < pooled = true; >
	{
		Width = BUFFER_WIDTH;
		Height = BUFFER_HEIGHT;
		Format = RGBA8;
	};

	texture MipChainTex < pooled = true; >
	{
		Width = BUFFER_WIDTH / 2;
		Height = BUFFER_HEIGHT / 2;
		Format = RGBA8;
		MipLevels = 4;
	};

	sampler2D sDownsample
	{
		Texture = DownsampleTex;
		AddressU = CLAMP;
		AddressV = CLAMP;
		MinFilter = LINEAR;
		MagFilter = LINEAR;
		MipFilter = LINEAR;
	};

	sampler2D sBlur
	{
		Texture = BlurTex;
		AddressU = CLAMP;
		AddressV = CLAMP;
		MinFilter = LINEAR;
		MagFilter = LINEAR;
		MipFilter = LINEAR;
	};

	sampler2D sHistory
	{
		Texture = HistoryTex;
		AddressU = CLAMP;
		AddressV = CLAMP;
		MinFilter = LINEAR;
		MagFilter = LINEAR;
		MipFilter = LINEAR;
	};

	sampler2D sMipChain
	{
		Texture = MipChainTex;
		AddressU = CLAMP;
		AddressV = CLAMP;
		MinFilter = LINEAR;
		MagFilter = LINEAR;
		MipFilter = LINEAR;
	};

	// -------------------------------------------------------------------------
	// Helpers
	// -------------------------------------------------------------------------

	float Luma(float3 color)
	{
		return dot(color, LUMA_COEFF);
	}

	float3 ApplySaturation(float3 color, float amount)
	{
		return lerp(Luma(color).xxx, color, amount);
	}

	float3 ApplyTint(float3 color, float3 tint, float amount)
	{
		return lerp(color, color * tint, amount);
	}

	float3 ToneMap(float3 color, int mode)
	{
		if(mode == 1)
		{
			return color / (1.0 + color);
		}

		if(mode == 2)
		{
			return saturate((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14));
		}

		return color;
	}

	float Hash12(float2 p)
	{
		p = frac(p * float2(0.1031, 0.1030));
		p += dot(p, p.yx + 33.33);
		return frac((p.x + p.y) * p.x);
	}

	float FilmNoise(float2 uv)
	{
		float2 seed = uv * BUFFER_SCREEN_SIZE + float2((float)FrameCount, (float)RandomValue * 0.01);
		return Hash12(seed + Phase) * 2.0 - 1.0;
	}

	float Depth01(float2 uv)
	{
		return saturate(ReShade::GetLinearizedDepth(uv));
	}

	float DepthFade(float2 uv)
	{
		if(!UseDepthMask)
		{
			return 1.0;
		}

		float depth = Depth01(uv);
		float mask = smoothstep(DepthNear, DepthFar, depth);
		return InvertDepthMask ? mask : 1.0 - mask;
	}

	float EdgeFromDepth(float2 uv)
	{
		float2 px = BUFFER_PIXEL_SIZE;
		float c = Depth01(uv);
		float r = Depth01(uv + float2(px.x, 0.0));
		float l = Depth01(uv - float2(px.x, 0.0));
		float u = Depth01(uv + float2(0.0, px.y));
		float d = Depth01(uv - float2(0.0, px.y));
		return saturate(abs(c - r) + abs(c - l) + abs(c - u) + abs(c - d));
	}

	float Vignette(float2 uv)
	{
		float2 pos = uv * 2.0 - 1.0;
		return saturate(1.0 - dot(pos, pos) * 0.35);
	}

	float4 Blur5(sampler2D samplerTex, float2 uv, float2 direction, float radius)
	{
		float2 px = BUFFER_PIXEL_SIZE * radius * 2.0;
		float4 color = tex2D(samplerTex, uv) * 0.2941176471;
		color += tex2D(samplerTex, uv + direction * px * 1.3333333333) * 0.3529411765;
		color += tex2D(samplerTex, uv - direction * px * 1.3333333333) * 0.3529411765;
		return color;
	}

	bool ShowOriginalAt(float2 uv)
	{
		if(SplitCompare == 1)
		{
			return uv.x < 0.5;
		}

		if(SplitCompare == 2)
		{
			return uv.y < 0.5;
		}

		return false;
	}

	float3 OverlayPattern(float2 uv)
	{
		float2 mouseUv = MousePoint / BUFFER_SCREEN_SIZE;
		float2 delta = uv - mouseUv;
		float ring = smoothstep(0.09, 0.08, abs(length(delta) - (0.06 + 0.02 * Phase.x)));
		float cross = smoothstep(0.006, 0.0, min(abs(delta.x), abs(delta.y)));
		float pulse = 0.5 + 0.5 * sin(Timer * 0.005 + Phase.y * PI);
		float clickBoost = LeftMouseDown ? 1.0 : 0.35;
		return float3(1.0, 0.55 + 0.35 * pulse, 0.10) * max(ring, cross) * clickBoost;
	}

	// -------------------------------------------------------------------------
	// Pixel shaders
	// -------------------------------------------------------------------------

	float4 PS_CopyBackBuffer(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
	{
		return tex2D(ReShade::BackBuffer, texcoord);
	}

	float4 PS_Downsample(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
	{
		float2 offset = SampleOffsetPx * BUFFER_PIXEL_SIZE;
		float2 px = BUFFER_PIXEL_SIZE * 2.0;

		float3 color = tex2D(ReShade::BackBuffer, texcoord + offset).rgb * 0.40;
		color += tex2D(ReShade::BackBuffer, texcoord + offset + float2(px.x, 0.0)).rgb * 0.15;
		color += tex2D(ReShade::BackBuffer, texcoord + offset - float2(px.x, 0.0)).rgb * 0.15;
		color += tex2D(ReShade::BackBuffer, texcoord + offset + float2(0.0, px.y)).rgb * 0.15;
		color += tex2D(ReShade::BackBuffer, texcoord + offset - float2(0.0, px.y)).rgb * 0.15;

		float bloomMask = saturate((Luma(color) - 0.35) * 2.0);
		return float4(color * bloomMask, 1.0);
	}

	float4 PS_BlurHorizontal(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
	{
		return Blur5(sDownsample, texcoord, float2(1.0, 0.0), BlurRadius + ExtraSamples * 0.15);
	}

	float4 PS_BlurVertical(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
	{
		return Blur5(sBlur, texcoord, float2(0.0, 1.0), BlurRadius + ExtraSamples * 0.15);
	}

	float4 PS_Composite(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
	{
		float4 backBuffer = tex2D(ReShade::BackBuffer, texcoord);
		float4 history = tex2D(sHistory, texcoord);
		float4 blurred = tex2D(sDownsample, texcoord);
		float depth = Depth01(texcoord);
		float depthMask = DepthFade(texcoord);
		float noise = FilmNoise(texcoord) * NoiseAmount;

		float3 graded = backBuffer.rgb;
		graded = ApplySaturation(graded, Saturation);
		graded = ApplyTint(graded, TintColor, TintAmount);
		graded = ToneMap(graded, ToneMapMode);

		float3 bloom = blurred.rgb * BloomAmount * depthMask;
		float3 overlay = ToggleOverlay ? OverlayPattern(texcoord) : 0.0;
		float3 finalColor = ComHeaders::Blending::Blend(BlendMode, backBuffer.rgb, graded + bloom, Strength);
		finalColor += overlay;
		finalColor += noise.xxx;
		finalColor *= Vignette(texcoord);

		if(ShowOriginalAt(texcoord))
		{
			finalColor = backBuffer.rgb;
		}

		if(DebugView == 1)
		{
			finalColor = backBuffer.rgb;
		}
		else if(DebugView == 2)
		{
			finalColor = blurred.rgb;
		}
		else if(DebugView == 3)
		{
			finalColor = depth.xxx;
		}
		else if(DebugView == 4)
		{
			finalColor = depthMask.xxx;
		}
		else if(DebugView == 5)
		{
			finalColor = Luma(backBuffer.rgb).xxx;
		}
		else if(DebugView == 6)
		{
			finalColor = (noise * 0.5 + 0.5).xxx;
		}
		else if(DebugView == 7)
		{
			finalColor = history.rgb;
		}

		float edge = EdgeFromDepth(texcoord);
		finalColor = lerp(finalColor, finalColor + edge.xxx * 0.15, UseDepthMask ? 1.0 : 0.0);

		return float4(saturate(finalColor), backBuffer.a);
	}

	float4 PS_AdditiveOverlay(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
	{
		float3 overlay = OverlayPattern(texcoord);
		return float4(overlay, ToggleOverlay ? 1.0 : 0.0);
	}
}

// Hidden helper technique pattern observed in multiple local shaders.
// timeout = 1 makes it refresh periodically while staying out of the normal UI.
technique ExampleReference_History <
	hidden = true;
	enabled = true;
	timeout = 1;
>
{
	pass CaptureBackBuffer
	{
		VertexShader = PostProcessVS;
		PixelShader = ExampleReference::PS_CopyBackBuffer;
		RenderTarget = ExampleReference::HistoryTex;
		ClearRenderTargets = false;
	}
}

technique ExampleReference_Main <
	ui_label = "example.fx";
	ui_tooltip = "Reference multipass effect built from patterns found across the local ReShade library.";
>
{
	pass Downsample
	{
		VertexShader = PostProcessVS;
		PixelShader = ExampleReference::PS_Downsample;
		RenderTarget = ExampleReference::DownsampleTex;
		ClearRenderTargets = true;
	}

	pass BlurHorizontal
	{
		VertexShader = PostProcessVS;
		PixelShader = ExampleReference::PS_BlurHorizontal;
		RenderTarget = ExampleReference::BlurTex;
	}

	pass BlurVertical
	{
		VertexShader = PostProcessVS;
		PixelShader = ExampleReference::PS_BlurVertical;
		RenderTarget = ExampleReference::DownsampleTex;
	}

	pass Composite
	{
		VertexShader = PostProcessVS;
		PixelShader = ExampleReference::PS_Composite;
		SRGBWriteEnable = false;
	}

	pass AdditiveOverlay
	{
		VertexShader = PostProcessVS;
		PixelShader = ExampleReference::PS_AdditiveOverlay;
		BlendEnable = true;
		BlendOp = ADD;
		SrcBlend = ONE;
		DestBlend = ONE;
		RenderTargetWriteMask = 0x7;
	}
}

/*
    -------------------------------------------------------------------------
    Additional reference templates gathered from the local pack
    -------------------------------------------------------------------------

    1. External textures / lookup textures / overlays

        texture2D LUTTex < source = "Textures/MyLUT.png"; >
        {
            Width = 64;
            Height = 64;
            Format = RGBA8;
        };

        sampler2D sLUT
        {
            Texture = LUTTex;
            AddressU = CLAMP;
            AddressV = CLAMP;
            MinFilter = LINEAR;
            MagFilter = LINEAR;
            MipFilter = LINEAR;
            SRGBTexture = true;
        };

    2. Multiple render targets

        struct PSOut
        {
            float4 Color0 : SV_Target0;
            float4 Color1 : SV_Target1;
        };

        PSOut PS_MRT(float4 position : SV_Position, float2 texcoord : TEXCOORD)
        {
            PSOut output;
            float3 color = tex2D(ReShade::BackBuffer, texcoord).rgb;
            output.Color0 = float4(color, 1.0);
            output.Color1 = float4(Luma(color).xxx, 1.0);
            return output;
        }

        technique ExampleMRT
        {
            pass
            {
                VertexShader = PostProcessVS;
                PixelShader = PS_MRT;
                RenderTarget0 = MRT0Tex;
                RenderTarget1 = MRT1Tex;
            }
        }

    3. Stencil and pass states observed in the library

        pass
        {
            VertexShader = PostProcessVS;
            PixelShader = SomePS;
            StencilEnable = true;
            StencilFunc = ALWAYS;
            StencilPass = REPLACE;
            StencilRef = 1;
            ClearRenderTargets = false;
            RenderTargetWriteMask = 0xF;
        }

    4. Compute / storage pattern
       Observed in local files such as GaussianBlurCS.fx, SharpContrast.fx,
       RealLongExposure.fx, SMAA compute variants, HDR analysis tools and others.

        #if (((__RENDERER__ >= 0xb000 && __RENDERER__ < 0x10000) || (__RENDERER__ >= 0x14300)) && __RESHADE__ >= 40800)
            #define EXAMPLE_COMPUTE 1
        #else
            #define EXAMPLE_COMPUTE 0
        #endif

        #if EXAMPLE_COMPUTE
        texture ComputeTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
        sampler2D sComputeTex { Texture = ComputeTex; };

        storage rwUntyped { Texture = ComputeTex; };
        storage1D<float> rwHistogram { Texture = HistogramTex; };
        storage2D<float4> rwComputeTex { Texture = ComputeTex; };
        storage3D<uint> rwVolumeTex { Texture = VolumeTex; };

        groupshared float4 sharedBlock[64];

        [numthreads(8, 8, 1)]
        void CSMain(uint3 id : SV_DispatchThreadID)
        {
            if(any(id.xy >= uint2(BUFFER_WIDTH, BUFFER_HEIGHT)))
                return;

            tex2Dstore(rwComputeTex, id.xy, float4(1.0, 0.0, 0.0, 1.0));
        }

        technique ExampleCompute
        {
            pass
            {
                ComputeShader = CSMain;
                DispatchSizeX = 8;
                DispatchSizeY = 8;
            }
        }
        #endif

    5. Other source bindings observed in the local pack

        source = "overlay_open"
        source = "overlay_active"
        source = "overlay_hovered"
        source = "screenshot"
        source = "bufready_depth"

    6. Matrix / vector style sources seen in specialized packs

        mat_ViewProj
        mat_InvViewProj
        mat_Projection
        mat_InvProjection
        vec_CameraPosition
        vec_CameraViewDir

    7. Common design patterns seen repeatedly in the pack

        - Hidden setup techniques that write helper textures
        - Half/quarter resolution blur chains
        - Depth-aware masks and fades
        - Temporal accumulation via history textures
        - Fullscreen debug views and visualization modes
        - Procedural overlays driven by mouse/time/frame counters
        - Blend helpers for color grading, sharpening, bloom, CRT/VHS/posterization, analysis overlays
*/
