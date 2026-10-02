# Modern Post Effects: Agent Implementation Specification

- Status: ready for implementation planning and execution.
- Research and repository inspection date: 2026-10-02.
- Target: QuakeRay's active NVRHI/Vulkan traced renderer.
- Documentation language: English.

## 1. Objective and visual direction

Implement a cohesive, restrained modern post-processing stack:

1. Soft, stable, multi-scale HDR bloom with a visual quality target comparable to UE5's standard bloom.
2. Reddish, edge-localized chromatic aberration during damage, plus subtle aberration while the camera is submerged in water, slime/acid, or lava. Ordinary dry gameplay must have zero aberration.
3. A short shader-based pickup pulse confined to the lower screen, replacing the existing broad bonus tint.
4. Subtle lens flares from the visible sun and exceptionally bright visible scene sources.
5. Adjustable contrast-adaptive sharpening.
6. A dedicated **Effects** menu in **Options**, immediately below **Graphics** and above **Lighting**. This specifies a sibling Options page, with the interpretation of “below Graphics” made explicit.

The look should preserve Quake's dark areas, colored lighting, texture detail, and combat readability. Bright emitters should have a compact bright core and a soft wide tail. Damage should be immediately recognizable without coloring or blurring the central aiming area. Lens flares should be noticed when looking toward a bright source, not dominate ordinary scenes.

All five effects must work in the normal game frame, respond to live settings, and have independently controllable strengths. The HUD, crosshair, console, menus, and editor panels are composited after the new effects.

## 2. Research findings and chosen techniques

These are methods verified against documentation available in 2026. Some foundational algorithms were published earlier; their age does not make them obsolete. The references describe different implementations, not a single universal “2026 algorithm.”

| Effect | Verified reference behavior | Required QuakeRay implementation |
| --- | --- | --- |
| Bloom | Epic distinguishes inexpensive multi-resolution standard bloom from more expensive FFT convolution. Unity 6.3 exposes threshold, scatter, clamp, filter quality, downscale, and mip iterations. | A normalized HDR pyramid with a stable 13-tap downsample and a 9-tap tent reconstruction. Soft knee, controlled scatter, firefly suppression in the first reduction, and pre-tonemap composition. |
| Chromatic aberration | Epic exposes intensity and a start offset from the image center. | A few RGB samples with a smooth edge mask. Gameplay envelopes control activation; red tint is an additional edge feedback layer, not an inherent property of aberration. |
| Pickup feedback | This is a gameplay-art requirement rather than a standardized optical effect. | An analytic bottom-only mask and bounded warm pulse, independent of the global tint effect. |
| Lens flare | Epic's flare is image-based. Froyok describes independent bright-pass extraction, ghosts, halo, glare, and floating-point pre-tonemap composition. | Independent high-threshold HDR extraction, 2–3 restrained ghosts and a faint halo, at reduced resolution. Visibility comes from rendered scene radiance. |
| Sharpen | AMD CAS adapts sharpening to local contrast after TAA. AMD explicitly cautions against stacking CAS with an upscaler's sharpening. | One standalone CAS sharpen-only pass after the actual upscaler and before gameplay feedback/UI. Keep FSR's internal sharpening disabled. |

The bloom target is **UE5-style visual quality**, not source-code parity with Unreal. The selected pyramid is a Jimenez-style real-time approximation; do not describe it as Epic's exact algorithm or FFT convolution. Normalized reconstruction prevents gain from changing with mip count, but additive bloom is not a physically energy-conserving lens simulation.

### Sources

- [Epic: Bloom](https://dev.epicgames.com/documentation/en-us/unreal-engine/bloom-in-unreal-engine): standard multi-scale bloom, thresholds, screen-relative size, and convolution tradeoffs.
- [Epic: Post Process Effects](https://dev.epicgames.com/documentation/en-us/unreal-engine/post-process-effects-in-unreal-engine): chromatic aberration start offset and image-based lens flare controls.
- [Unity 6.3 LTS: Bloom reference](https://docs.unity3d.com/6000.3/Documentation/Manual/urp/post-processing-bloom.html): current production controls and quality/performance choices. Unity's gamma-space threshold semantics are not adopted here; QuakeRay's proposed thresholds are explicitly linear.
- [Jorge Jimenez: Next Generation Post Processing](https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare): pyramidal bloom and temporal stability rationale.
- [LearnOpenGL: Physically Based Bloom](https://learnopengl.com/Guest-Articles/2022/Phys.-Based-Bloom): inspectable 13-tap/9-tap filters and first-level Karis filtering.
- [Froyok: Custom Lens-Flare Post-Process](https://www.froyok.fr/blog/2021-09-ue4-custom-lens-flare/): extraction, ghosts, halo, and pre-tonemap integration. This is a 2021 technical reference, not a claim about a new UE5 feature.
- [AMD: FidelityFX CAS](https://gpuopen.com/fidelityfx-cas/): contrast-adaptive sharpening and TAA integration.
- [AMD: FSR integration guide](https://gpuopen.com/learn/ue-fsr/): optional RCAS, sharpening slider, and avoiding double sharpening.
- [AMD: FSR Super Resolution manual](https://gpuopen.com/manuals/fsr_sdk/techniques/super-resolution-upscaler/): HDR/linear input contracts and placement in the frame.

## 3. Verified repository baseline

The implementation agent must recheck these entry points against its checkout. Line numbers refer to the inspected revision and can move.

| Area | Evidence | Consequence |
| --- | --- | --- |
| Active rendering path | `renderer/Source/VulkanDevice.cpp:1126–1147`, `RenderThroughRhi` | New effects must be wired into the RHI frame. Settings alone or an unused legacy class do not implement an effect. |
| Missing bloom/sharpen execution | `renderer/Source/RHI/RhiPostEffectPass.h:146–151`; `NvrhiFrameSkeleton.cpp:1134–1241` | The current RHI path omits the separate bloom and sharpening passes. Implement the replacement in the active RHI frame. |
| Old bloom input | `Quake/gl_vidsdl.c:2135–2139`; `renderer/Source/Shaders/HLSL/CmPrepareFinal.comp.hlsl:161–172,323–347` | Threshold is supplied as zero. Emission is converted into an exponential EV boost, with `rt_bloom_emis_mult` defaulting to 50. Replace this input model. |
| Tone mapping occurs early | `RhiRtComposePass.cpp:1744–1785`; `CmPrepareFinal.comp.hlsl:121–158,305–347` | HDR radiance is available before prepare-final. `FINAL` is clamped display-linear color afterward; later bloom cannot recover clipped highlights. |
| Constant aberration | `Quake/gl_vidsdl.c:291,2357–2362`; `EfChromaticAberration.comp.hlsl:26–74` | Default strength is 0.3 and activation depends only on the cvar. The existing shader uses up to eight spectral samples and has no gameplay mask contract. |
| Shared tint | `Quake/gl_vidsdl.c:2364–2418`; `EfColorTint.comp.hlsl:29–58` | Powerups, lava, damage, and pickups compete in one priority chain. Damage and pickups need independent state and composition. |
| Damage source | `Quake/view.c:270–331` | `V_ParseDamage` provides armor/blood feedback and a damage pulse. Handle repeated events rather than relying on a single render-frame flag. |
| Pickup source | `Quake/view.c:353–359`; `Quake/cl_parse.c:2016` | `bf`/bonus feedback is generic; it does not identify which item was picked up. |
| Liquids | `Quake/gl_rmain.c:687–715`; `qray.h:1187–1193` | Lava is currently sent as water plus `rt_lavaeffects`; acid has its own media value. The post-FX media classification must distinguish lava without changing refraction semantics. |
| Actual upscaler | `NvrhiFrameSkeleton.cpp:1182–1223` | FSR may fail and fall back to TAAU. Sharpen must follow the actual successful path. |
| FSR sharpening | `renderer/Source/FSR.cpp:388–391`; `RenderResolutionHelper.h:110–122` | FSR sharpening is disabled, while helper predicates assume CAS can be inside FSR and return hardcoded strengths. Correct these helpers for the chosen standalone strategy. |
| UI boundary | `NvrhiFrameSkeleton.cpp:1225–1325` | Existing pre-UI effects and post-UI CRT/wipe have distinct placements. Put new effects before UI. |
| Vertical orientation | `renderer/Source/Shaders/HLSL/RhiPresent.frag.hlsl:77–89` | Traced presentation flips Y. Bottom-screen masks must be defined in displayed screen coordinates. |
| Existing menu | `Quake/menu.c:2275–2419,2549–2669` | Move the bloom control out of Graphics and add an Effects page/state following existing keyboard, mouse, and controller patterns. |
| Shader toolchain | `build_shaders.ps1`; `renderer/Source/Shaders/GenerateShaders.py:31–45,106–136` | Add HLSL compute shaders compiled with DXC to SPIR-V; register/load their blob names and deploy them through the existing scripts. |

No active consumer of `v_blend` was found outside its calculation/declaration in this checkout. Recheck this during implementation; the verified broad pickup feedback is the shader tint branch, not a presumed additional legacy fullscreen draw.

## 4. Frame architecture and color contracts

### 4.1 Required execution order

```text
RT lighting, sky and clouds
  -> ASVGF and checkerboard resolve
  -> existing scene exposure analysis
  -> raster world/smoke overlay
  -> prepare scene-linear HDR color and optical source
  -> shared reduced-resolution HDR source
       -> bloom extraction/downsample/reconstruction
       -> independent flare extraction/ghosts/halo
  -> HDR optical composition + existing tone curve and scene fog
  -> existing FSR 3.1 OR actual TAAU fallback
  -> standalone CAS, if enabled
  -> existing scene-only powerup/warp effects
  -> gameplay edge aberration + damage edge tint + bottom pickup pulse
  -> HUD / crosshair / console / menus / editor panels
  -> existing post-UI CRT/wipe, when explicitly enabled
  -> present / screenshot
```

Bloom and flare operate at render resolution or below. CAS and gameplay feedback operate at output resolution. HUD pixels must never be bloom/flare sources. Gameplay feedback must not feed exposure analysis, bloom extraction, or temporal reconstruction history.

The existing tone-before-upscale order is a deliberate integration constraint for this task. Do not move tone mapping after FSR by changing a call order alone: its inputs, history, flags, and TAAU contract would also need migration. Preserve the current upscaler input representation and verify its declared format/flags; a floating-point image is not automatically scene HDR.

### 4.2 HDR preparation

- Refactor prepare-final into explicit HDR preparation and final display conversion when optical effects are active. Factor the existing emission blend and god-rays contribution so each is applied once.
- Use the visible, denoised scene HDR result, including visible emission, sun/sky, highlights, and raster overlay. Sample full-resolution resolved inputs, not checkerboard coordinates as if they were full screen.
- Keep `FINAL` scene-linear until the optical passes finish; then write its current display-linear result for FSR/TAAU. A separate HDR scratch image is allowed only if aliasing/read-write constraints require it.
- Remove the old `getBloomInput` exponential emission amplification from both enabled and disabled optical paths. An emission multiplier of 50 must not become a new default gain or turn into `exp2` overflow.
- Match visible fog/cloud attenuation in optical source eligibility. In particular, heavily fogged bright sources must not produce an unattenuated flare. Preserve the existing world tone/fog result when bloom and flare are disabled. Do not silently move the existing post-tone scene fog into HDR and recolor the entire baseline scene.
- A source-visibility/transmittance factor may attenuate the optical extraction while final composition retains the existing fog order. Apply the scene fog to the world once; document this optical approximation and verify it with fogged-emitter captures.
- Exposure analysis reads the existing scene-only source, not the bloom/flare-composited image. Avoid feedback where bloom increases measured luminance and makes exposure oscillate.
- Use `RGBA16_FLOAT` for writable optical buffers by default. Never quantize the pyramid to 8-bit color or clamp scene HDR to `[0,1]` before extraction.
- Sanitize non-finite/negative optical samples and bound extracted radiance below FP16 overflow. This bound is for the optical buffers, not an additional scene tone curve.

### 4.3 Extraction exposure

Define a shared, finite positive `E_extract` for brightness thresholds, derived from the renderer's existing adapted scene luminance and exposure bias. The exact mapping must be calibrated against the current tone curve and documented in the implementation report; do not introduce a second auto-exposure controller.

Extraction uses `C_exposed = C_scene * E_extract`. Threshold values in this specification are **exposed linear units**. Convert the extracted optical contribution back to scene-linear units before adding it to scene HDR. Apply the final tone mapping/exposure once. The old `ev100ToLuminance` helper computes luminance, not reciprocal exposure; do not mistake it for an exposure multiplier.

For bloom, a soft knee can use the following reference equations, with `L` equal to exposed linear luminance:

```text
K = max(threshold * kneeFraction, epsilon)
q = clamp(L - threshold + K, 0, 2*K)
soft = q*q / (4*K + epsilon)
weight = max(L - threshold, soft) / max(L, epsilon)
bright = C_exposed * weight
```

At zero knee use an explicitly supported hard-threshold branch. At threshold zero, zero-valued input must remain black. Keep RGB ratios when extracting luminance; do not create three independent colored thresholds.

## 5. Bloom requirements

### 5.1 Algorithm

1. Generate/reuse an anti-aliased raw half-resolution HDR source with bilinear sampling and a normalized 13-tap reduction. Extract bloom's first level from that source using its soft threshold; for Low quality, reduce it again to quarter resolution. Flare extracts independently from the raw source with its own threshold. Never threshold the shared source itself.
2. Apply luminance-weighted/Karis outlier suppression only in the initial full-to-half reduction. It must reduce isolated noisy fireflies without suppressing an entire legitimate bright sun disc. If the selected filter darkens uniform radiance, compensate its normalized weights rather than baking that loss into bloom intensity.
3. Build progressively smaller levels with the same normalized downsample. Stop before invalid dimensions; support 1-pixel axes and odd resolutions.
4. Reconstruct from coarse to fine with a normalized 9-tap tent filter and controlled scatter weighting.
5. Keep reconstruction gain independent of the number of levels. A reference recurrence is `U_i = lerp(D_i, Tent(U_(i+1)), scatter)`, starting from the coarsest level. If a weighted additive recurrence is used instead, normalize the accumulated weights explicitly.
6. Add `bloomStrength * bloomSceneLinear` to HDR before the existing tone curve. Do not blur or blend the complete LDR image as the bloom implementation.

The radius is screen-relative. Derive pyramid depth/support from viewport size and the desired displayed radius; a switch from native to FSR Quality must not shrink the halo proportionally to internal render size. Quality limits should change sampling cost with only a small change in the intended radius.

### 5.2 Quality levels

| Quality | Initial size | Maximum levels | Reconstruction |
| --- | --- | --- | --- |
| Low | Quarter resolution, reached through filtered reduction | 5 | Bilinear tent |
| Medium | Half resolution | 6 | Bilinear tent |
| High, default | Half resolution | 7 | High-quality final reconstruction; use bicubic at the final combine if profiling supports it |

Use the smallest valid chain when the viewport is small. Level count is a cap, not permission to change brightness. Shared source generation must still work with bloom disabled and lens flare enabled.

### 5.3 Visual acceptance

- Bright fixtures, flames, lava highlights, specular highlights, and the sun bloom naturally according to rendered brightness.
- Colored emitters retain their hue in the glow until the existing tone curve rolls off the highlight.
- Ordinary walls and dark corners do not become a uniform milky wash.
- A small moving bright emitter does not pulse at mip boundaries or form square halos.
- Disabling bloom removes its contribution completely. No old emission boost survives in the flare source.
- Switching quality or render scale does not noticeably change average bloom strength.

## 6. Damage and liquid edge aberration

### 6.1 Gameplay state

Store feedback envelopes in client/view state and update them once per client frame. Pass a final immutable snapshot to rendering. Avoid mutating event state from the render task.

Damage must originate from `V_ParseDamage`, including armor-only hits. Aggregate multiple events received between rendered frames. A hit adds/retriggers a bounded pulse with a fast attack of about 20–40 ms and a 250–700 ms release depending on damage severity. Repeated hits can maintain the pulse but must not increase its maximum intensity or indefinitely extend a single weak hit.

Liquid feedback uses actual camera/head contents, not the player's feet, low health, a water surface elsewhere on screen, or the presence of waterwarp. Define a post-FX media enum `dry / water / acid / lava`; retain existing refraction media values separately. Ramp in over about 150 ms and out over about 250 ms. While submerged, maintain a restrained steady effect. Lava is distinguishable through `rt_lavaeffects`/contents and must not be treated as ordinary water.

Combine damage and liquid amplitudes with a bounded rule such as `max(damage, liquid)`, allowing damage red tint to take priority without switching the liquid envelope off. There must be no always-on dry baseline or low-health activation.

The requested reddish fringe is the default palette for damage and all three liquid modes; liquid strength is lower. Preserve water/acid/lava identity through existing material/warp treatment. Additional media-dependent tinting is optional only if it does not replace the requested reddish edge treatment.

### 6.2 Shader

- Use displayed screen coordinates, aspect-correct distance/direction, and a smooth edge mask.
- Define a protected central rectangle `x in [0.25,0.75], y in [0.25,0.75]`; the aberration offset and damage tint are exactly zero there. Outside it, smoothly increase the mask toward the outer screen boundary, including the side midpoints, not only the corners.
- Use approximately three RGB texture samples, with a dominant red-channel radial offset and smaller green/blue offsets. Do not retain the eight-iteration spectral blur as the normal mode.
- Define peak displacement in output pixels relative to a 1080-pixel display height. Starting maximum: about 2.0 pixels for damage and 0.75 pixels for liquids at 1080p; scale with output height and bound the total displacement.
- Add a subtle red edge tint/vignette using the same mask. Channel separation alone cannot guarantee red feedback on arbitrary imagery.
- Preserve source alpha and luminance readability; clamp sampling to valid texel centers. No wraparound seams or black border sampling.
- If amplitude is zero, skip the pass or use exact identity composition. Do not run a near-zero distortion every frame.
- Damage feedback is independent of quad, invulnerability, radiation suit, and pickup feedback. Remove the old damage branch from the shared tint chain to avoid double treatment.
- Damage/liquid toggles control these layers independently. Disabling them must not restore broad damage tint or perpetual aberration.

### 6.3 Reset and time behavior

Use client/game time for gameplay envelopes. Pausing freezes a pulse; rendering the same state multiple times does not consume it multiple times. Reset feedback on disconnect, map transition, new demo/seek, save load, respawn, and editor entry. Death may finish the last damage release but must not leave a persistent tint. Suppress gameplay feedback in intermission and while the editor is active; bloom, flare, and sharpening remain available for world appearance.

Forced-underwater menu behavior (`M_ForcedUnderwater`) is not evidence of real camera submersion and must not silently activate new liquid feedback.

## 7. Bottom-only pickup pulse

- Consume the generic bonus event from `V_BonusFlash_f`/`bf`; ensure the protocol path invoking `bf` triggers the same pulse.
- Standard Quake bonus feedback does not reliably identify ammunition versus weapon pickups. Replace **all generic bonus pulses** with the bottom-only effect, ensuring ammo/weapon pickups are covered without inferring item identity from sounds, inventory deltas, or unrelated health changes.
- Keep normal `CSHIFT_BONUS` bookkeeping needed by the client, but remove its selection as the renderer's global `tint_bonus`. Recheck any additional blend consumers to avoid double feedback.
- Implement a GPU shader mask, not a CPU fullscreen colored quad.
- Define screen `y = 0` at the displayed top, `y = 1` at the bottom. Use `bottomMask = smoothstep(1-height, 1, y)` with default `height = 0.28`, optionally shaped by a broad soft horizontal falloff.
- The contribution is exactly zero above `y = 0.72` at the default height. Test this after the traced present Y flip.
- Use a warm amber/gold palette, a smooth attack of roughly 20 ms, and a total duration around 300 ms. Add a bounded warm veil preserving texture detail; do not add uncontrolled white radiance to the entire lower region.
- Default maximum blend opacity: 0.10; console-adjustable upper bound: 0.25. It is not an exposure change and is applied after sharpening/temporal reconstruction.
- Multiple pickups retrigger or boundedly accumulate the pulse. They must not create an ever-brighter or indefinite flash.
- Pickup and damage pulses may be visible simultaneously. Powerups must not swallow the pickup event because of tint priority.
- With pickup feedback disabled, there is no visual pickup pulse and no fallback fullscreen flash.

## 8. Subtle lens flare

### 8.1 Required implementation

- Create a separate flare bright pass from visible scene HDR with its own higher threshold and soft knee. Reuse the raw reduced-resolution source, not the already-thresholded or reconstructed bloom texture.
- Generate 2–3 faint ghosts along the source-to-screen-center axis and a weak halo in a quarter-resolution floating-point target. A small soft source glare is allowed within the same restrained style.
- Use aspect-correct geometry, smooth thresholding and frame-edge fades. Samples outside the source viewport return black rather than repeating a clamped edge emitter across the image.
- Tint follows the light source, with only weak spectral coloring. The ghosts should be broad, low-opacity shapes, without opaque hexagons or long dominating streaks.
- Composite the optical contribution in HDR before tone mapping, with a separate strength from bloom. `Bloom Off + Lens flare On` is a supported combination.
- Build eligibility from currently visible radiance. An occluded fixture or sun hidden by opaque geometry must not flare merely because its world position/direction exists.
- Procedural sky clouds must attenuate the visible sun flare through the same cloud-composited radiance used by the frame. A cloud-shadow value alone is not a substitute for looking through the cloud layer toward the sun.
- A scene-wide bright floor, wall, or fog sheet must not create a giant image-shaped ghost. Use local contrast/compact bright-source eligibility and area/energy limiting at extraction. Broad uniform bright input should produce negligible ghosts.
- Visible reflections of genuinely bright sources may contribute. Ordinary diffuse surfaces under normal exposure must not.
- Use filtered extraction for temporal stability. Do not add a long persistence buffer that makes a flare remain after a light goes behind a wall. If short temporal stabilization is needed, use motion-aware reprojection, visibility rejection, and camera-cut resets.
- Normal operation requires no GPU-to-CPU readback and no synchronous CPU visibility query.

### 8.2 Sun handling

The rendered sun disc must provide sufficient finite HDR radiance to pass the flare threshold while retaining its current direct appearance. Inspect the procedural sky shader and existing sky intensity first; do not generate flare from direction alone or arbitrarily multiply the entire sky.

If the sun's current visible representation cannot support reliable extraction, add a narrowly scoped GPU sun-disc source/visibility mask, using the actual view direction, sky-depth mask, and cloud transmittance. Match the displayed disc size/color and disable the source when the visible disc size or sun intensity is zero. This is a required fallback to fulfill the sun behavior, not permission to flare through roofs or from the hidden directional light.

Acceptance cases include looking toward the sun, moving it behind a wall, covering it with clouds, setting disc size to zero, disabling the sun, and moving a small bright fixture across the viewport edge. God rays and lens flare remain independently controllable.

## 9. Sharpening

- Use the vendored FidelityFX CAS filter in **sharpen-only** mode, without another scaling operation.
- Run after successful FSR output copy or actual TAAU output, before distortion, pickup overlays, and UI.
- CAS input/output here is bounded **display-linear RGB**. The vendored header explicitly requires linear `[0,1]` input and warns against perceptual/PQ input. Do not apply an extra sRGB encode before this implementation of CAS.
- Keep `FSR::Apply` internal sharpening off. Correct `IsCASInsideFSR3`, `IsDedicatedSharpeningEnabled`, and hardcoded intensity accessors so standalone CAS actually executes for FSR-selected frames.
- Expose one user strength in `[0,1]`, default 0.20. Strength zero is an actual bypass; CAS's lowest parameter alone is not guaranteed to be an identity operation.
- Replace the naive sharpen choice with CAS for normal settings. For old configs, map `rt_sharpen 1` to CAS and retain `2` as CAS; use `0` for off. Menu changes write `0` or `2`.
- Avoid bright halos, crushed dark detail, and amplification of path-tracing noise. Tune against ASVGF on/off and FSR Native AA, Quality, Performance, and TAAU fallback.
- Do not sharpen HUD text or the colored gameplay feedback layers. Add a GPU marker showing whether CAS really executed.

## 10. Menu, cvars, defaults, and migration

### 10.1 Effects page

Insert the new Options row immediately after Graphics. Remove the old standalone bloom row from Graphics. Add `m_effects` and the corresponding open/draw/key dispatch using the actual menu state definition in this checkout.

The Effects page contains these rows, in order:

```text
Bloom                    On / Off
Bloom strength           slider
Bloom quality            Low / Medium / High
Damage aberration        On / Off
Damage strength          slider
Liquid aberration        On / Off
Liquid strength          slider
Pickup feedback          On / Off
Pickup strength          slider
Lens flare               On / Off
Lens flare strength      slider
Sharpen                  On / Off
Sharpen strength         slider
Reset effects defaults   action
```

Fit the page into the existing 320-unit menu layout. Keyboard arrows, Enter/keypad Enter, mouse hover/click, controller confirm/back, and Escape must behave consistently with Graphics. Back returns to Options with Effects selected. Toggles take effect immediately and slider changes are bounded. Reset affects only this page's settings.

Advanced optical controls are console settings rather than extra crowded menu rows. All settings below are archived. Debug controls and transient pulse state are not archived.

### 10.2 Proposed cvar contract

These are required initial tuning values, not measured production optima. The implementation agent may calibrate optical defaults against captures and must record changes and the reasons; activation, localization, and off semantics are fixed requirements.

| Cvar | Initial default | Range / meaning |
| --- | --- | --- |
| `rt_bloom` | `1` | 0/1 |
| `rt_bloom_intensity` | `0.08` | 0–0.5, scene-linear additive strength after normalized reconstruction |
| `rt_bloom_quality` | `2` | 0 Low, 1 Medium, 2 High |
| `rt_bloom_threshold` | `1.0` | 0–10, exposed linear luminance |
| `rt_bloom_knee` | `0.5` | 0–1, threshold-relative soft knee |
| `rt_bloom_scatter` | `0.7` | 0–1, reconstruction distribution |
| `rt_bloom_radius` | `0.04` | 0.005–0.15, characteristic fraction of displayed image height |
| `rt_ef_damage` | `1` | damage feedback switch |
| `rt_ef_damage_strength` | `0.5` | 0–1, displacement and bounded red tint multiplier |
| `rt_ef_liquid` | `1` | submerged feedback switch |
| `rt_ef_liquid_strength` | `0.25` | 0–1 |
| `rt_ef_chraber` | `0.3` | legacy-compatible master aberration scale, clamped 0–1; never an activation trigger |
| `rt_ef_pickup` | `1` | pickup pulse switch |
| `rt_ef_pickup_strength` | `0.10` | 0–0.25, peak veil opacity |
| `rt_ef_pickup_height` | `0.28` | 0.15–0.35, fraction of displayed lower screen |
| `rt_lensflare` | `1` | 0/1 |
| `rt_lensflare_intensity` | `0.03` | 0–0.2, normalized optical strength |
| `rt_lensflare_threshold` | `4.0` | 1–32, exposed linear luminance, calibrated separately from bloom |
| `rt_sharpen` | `2` | 0 Off, 1 compatibility CAS, 2 CAS |
| `rt_sharpen_strength` | `0.20` | 0–1; zero skips CAS |

The aberration master scale uses 0.3 as the reference value for the displacement maxima in section 6; normalize the multiplier against that reference and clamp the final displacement. A legacy value of zero disables channel separation but must not accidentally enable a fallback global tint. The damage switch still controls its edge tint.

Clamp and finite-check cvar values at the client-to-renderer boundary, including console edits and config loads. Handle negative, non-integer quality, NaN, and infinite values deterministically. Store a coherent settings snapshot per frame.

### 10.3 Existing config migration

- Respect existing `rt_bloom 0`, `rt_sharpen 0`, and `rt_ef_chraber 0` preferences. Fresh defaults must not overwrite saved choices on every launch.
- Repurpose positive `rt_ef_chraber` only as a gameplay-conditioned scale; old configs must not retain constant dry aberration.
- Clamp old bloom intensity 1 into the new supported range; do not reinterpret it as an uncontrolled full-screen glow. Record this migration behavior in the release notes.
- Keep `rt_bloom_emis_mult` accepted as a deprecated no-op for the new optical stack. Do not silently pipe its legacy value 50 into radiance gain. State its new status in the settings documentation.
- Defaults reset writes the new calibrated values. It does not reset exposure, lighting, water refraction, controls, sound, or the rest of Graphics.

## 11. API and code organization

Use small RHI passes consistent with the existing frame-context model. Suggested names are implementation guidance, not a requirement to create all files if existing modules can cleanly own the behavior.

| Work | Main edit points / suggested additions |
| --- | --- |
| Client feedback state | `Quake/view.c`, `Quake/client.h`, client reset/load/demo lifecycle entry points |
| Media snapshot and draw parameters | `Quake/gl_rmain.c`, `Quake/gl_vidsdl.c` |
| Effects menu | `Quake/menu.c` and actual menu state declaration |
| Public render params | `renderer/Include/qray/qray.h` |
| Frame wiring and resources | `VulkanDevice.h`, `VulkanDevice_Init.cpp`, `VulkanDevice.cpp`, `RHI/NvrhiFrameSkeleton.h/.cpp` |
| HDR preparation/final composition | `RHI/RhiRtComposePass.h/.cpp`, `Shaders/HLSL/CmPrepareFinal.comp.hlsl`; suggested `CmPrepareHdr.comp.hlsl` |
| Shared source and bloom | suggested `RHI/RhiBloomPass.h/.cpp`, `CmBloomDownsample.comp.hlsl`, `CmBloomUpsample.comp.hlsl` |
| Lens flare | suggested `RHI/RhiLensFlarePass.h/.cpp`, `CmLensFlare.comp.hlsl` |
| Gameplay feedback | `RHI/RhiPostEffectPass.h/.cpp`; suggested `EfGameplayFeedback.comp.hlsl` |
| CAS | suggested `RHI/RhiSharpenPass.h/.cpp`, `CmSharpenCAS.comp.hlsl`; existing `Shaders/CAS/` headers |
| Render-resolution policy | `RenderResolutionHelper.h`, verify `FSR.cpp`/`RHI/RhiFsrPass.cpp` |
| Registration/build | `renderer/CMakeLists.txt`, shader blob registration/loading, `Shaders/GenerateShaders.py`, `Generated/GenerateShaderCommon.py` where needed |

Expose explicit settings and feedback data: bloom threshold/knee/scatter/radius/quality, flare enable/strength/threshold, sharpen strength, and final damage/liquid/pickup amplitudes and colors. A dedicated gameplay feedback struct/pointer in `QrDrawFramePostEffectsParams` is preferred over overloading color tint or refraction fields.

The client owns pulse timing; the renderer consumes amplitudes and does not run a second independent damage/pickup transition controller. A null feedback pointer means no feedback. Add explicit layout fields rather than hiding new data in unrelated matrices or unused enum bits.

Public structs are compiled into the executable, but all callers, default initialization, copies into the skeleton, and layout generators must be updated together. If generated C/GLSL/HLSL uniforms or framebuffer indices change, edit the generator and regenerate the outputs. Do not hand-edit generated headers alone.

### Resource and synchronization requirements

- Source and destination must not be the same UAV subresource during neighborhood filtering.
- Reuse existing output ping/pong images where legal. Resolve odd/even pass parity so UI and present always see the final output, not an earlier copy.
- Shared optical source must have a single owner; lens flare alone must not allocate or dispatch the complete bloom reconstruction chain.
- Respect per-frame-slot buffers, resize/release order, UAV barriers, sampled-image transitions, and the existing GENERAL/UnorderedAccess handoff discipline.
- Retire replaced textures, binding sets, and pipelines through the frame context; do not free in-flight resources.
- Skip dispatches for disabled/zero-strength effects and skip optical preparation if both optical effects are off. No per-frame resource allocation in steady state.
- For tiny/odd viewports, use bounded dispatch and actual texture dimensions at every level.
- Keep diagnostic lighting/depth/motion views interpretable by bypassing the new aesthetic effects in those views.
- All implementation code follows project naming/style. Do not add explanatory code comments or docstrings; describe decisions in English documentation.

## 12. Implementation plan and checkpoints

Each stage should leave a buildable engine. A stage is complete only after its relevant checks, not merely after adding cvars or shaders.

### Stage A — Baseline and contracts

1. Reconfirm the active frame path and capture dry gameplay, damage, pickup, all liquids, a visible sun, and bright fixtures.
2. Record tone/exposure, framebuffer formats, orientation, existing FSR/TAAU inputs, and resource ownership.
3. Add the settings/feedback API, client envelopes, lifecycle resets, and Effects menu.
4. Verify saved settings, immediate toggle behavior, and menu navigation.

### Stage B — Gameplay feedback

1. Implement edge aberration/red feedback and bottom pickup pulse as independent shader layers.
2. Remove old damage/pickup tint selections and constant aberration activation.
3. Verify simultaneous effects, pause/demo behavior, reset behavior, protected center, and lower-screen orientation.

### Stage C — HDR bloom

1. Split HDR preparation from final conversion and replace legacy bloom extraction.
2. Implement stable downsample/reconstruction, normalized gain, independent radius/quality, and exposure conversion.
3. Wire it into compose before tone mapping, including raster emission, sky, god rays, and source attenuation.
4. Capture native/FSR comparisons, quality changes, moving emitters, and a disabled-effects baseline.

### Stage D — Lens flare

1. Add independent extraction and compact-source eligibility.
2. Implement low-strength ghosts/halo and verify HDR composition.
3. Fulfill sun/cloud/opaque-occlusion behavior, including the explicit sun-source fallback if needed.
4. Verify bloom off + flare on, bright walls, fogged sources, reflections, and viewport-edge fade.

### Stage E — CAS and integration polish

1. Add standalone CAS and correct the old helper policy.
2. Verify actual FSR success/fallback execution and single sharpening application.
3. Tune defaults, GPU markers/timing, shader deployment, resize, and release packaging.
4. Update settings documentation and `changelog.md` only with completed behavior and measured facts.

### Stage F — Acceptance and handoff

Complete the tests below and deliver screenshots/video, GPU measurements, final calibrated cvars, implementation notes, and any genuinely unresolved failures. Do not mark the work complete while a requested effect is wired only to an inactive path.

## 13. Verification and acceptance criteria

### 13.1 Meaningful automated checks

Add a focused optional headless GPU regression target under the existing `QR_BUILD_TESTS` pattern, modeled on `renderer/Tests/CloudsSmoke.cpp`/`VulkanTestContext.h`. It should exercise the actual production passes with synthetic inputs, not copy the shader equations into an equivalent CPU test.

Required GPU invariants:

1. Black HDR input produces zero bloom and flare within FP16 tolerance.
2. Constant above-threshold input has reconstruction gain stable within 5% across quality/level counts, with flare compact-source rejection tested separately.
3. A bright impulse produces a smooth symmetric falloff without wrapping to the opposite image edge.
4. A broad uniform bright input does not generate significant image-shaped flare ghosts; a compact source does generate a finite contribution.
5. All new feedback disabled yields identity within the target format's precision; protected central pixels remain unchanged under damage.
6. Default pickup pulse changes no pixels above displayed `y = 0.72`, including the actual presentation orientation.
7. CAS off/strength zero bypasses it; a constant input remains constant when CAS is enabled.
8. Tiny and odd-sized buffers execute without out-of-bounds access or validation errors.

For client state, use deterministic event/time-sequence checks for multiple same-frame damage events, pulse decay, pause, simultaneous pickup, media transition, and lifecycle reset. Test behavioral invariants rather than mirroring the implementation line for line.

### 13.2 Manual visual matrix

| Scenario | Pass condition |
| --- | --- |
| Healthy dry player, no recent hit | Exactly no aberration, damage tint, or pickup pulse |
| One armor/blood hit | Fast reddish edge response; central protected region readable; finite release |
| Rapid hits, including during quad/suit | Bounded independent feedback; no missed events or global wash |
| Water / acid / lava camera submersion | Smooth subtle activation only for actual head/camera contents; smooth exit |
| Feet in liquid, head above surface | No submerged aberration |
| Ammo / weapon / repeated generic pickups | Only lower-screen pulse; no white/yellow full-screen flash |
| Pickup + damage together | Both visible in their own masks; no priority suppression |
| Sun + moving clouds + wall occlusion | Flare tracks visible radiance and disappears behind opaque occlusion |
| Hidden/disabled sun disc | No sun-derived flare |
| Very bright fixture and its reflection | Subtle flare and bloom when visibly bright; nothing through a wall |
| Bright diffuse wall / heavy fog | No dominant image-shaped ghosts or unattenuated flare |
| Moving tiny emissive | No distracting bloom/flare flicker or square mip patterns |
| FSR Native AA / Quality / Performance, TAAU fallback | Similar intended bloom radius; single CAS application; no feedback in history |
| 720p / 1080p / 1440p / 4K, 4:3 / 16:9 / ultrawide | Correct masks, aspect, brightness, and radius; no seams |
| Window resize / fullscreen switch / minimize-restore | Valid targets and no stale-frame flashes or resource leaks |
| Pause / save-load / respawn / demo restart / editor | Correct freeze/reset/suppression; no persistent damage tint |
| UI and HUD | Text and crosshair not bloomed, flared, sharpened, or split into RGB channels |
| Effects controls, reset, restart | Live changes work, archive round-trip works, reset is page-local |

### 13.3 Performance measurement

Use GPU timestamps/markers for HDR preparation, bloom downsample, bloom reconstruction, flare, CAS, and gameplay feedback. Record marginal cost against the same scene/settings with the new effects disabled, after a warm-up. Report GPU/driver, output/internal resolution, FSR mode, p50/p95 frame/pass time, dispatch count, and incremental resource memory per slot and total.

Initial engineering budget for the full enabled stack at High quality: approximately **1.0 ms at 1080p**, **1.6 ms at 1440p**, and **3.0 ms at 4K** on an RTX 3060/RX 6700 XT class GPU. These are profiling targets, not measured guarantees. If hardware differs, report actual results without extrapolating another GPU's timings. Optimize missed targets while retaining the fixed acceptance behavior.

Steady-state rendering must not introduce CPU/GPU synchronization or allocate resources per frame. Disabled effects must contribute no dedicated dispatches. Report memory from actual texture descriptors, including frame-slot multiplication; do not present only a single mip's size as the stack's footprint.

### 13.4 Build and regression commands

Run the project's existing build workflow after implementation:

```powershell
.\build_shaders.ps1 -GenCommon -Rebuild
.\build_win.ps1 Debug
.\build_win.ps1 Release -Parallel 4
```

With GPU test prerequisites available, configure tests and run the existing clouds regression and the newly registered post-effects test:

```powershell
cmake -S . -B build/Debug -DQR_BUILD_TESTS=ON
.\build_win.ps1 Debug
ctest --test-dir build/Debug -R "qray_(clouds|posteffects)_gpu" --output-on-failure
```

The proposed new CTest name is `qray_posteffects_gpu`; register it explicitly. Capture Vulkan validation and startup/shader-load logs. Confirm release-deployed shaders include every new blob. If GPU/game-data checks cannot run, state the specific missing prerequisite and do not claim visual acceptance.

## 14. Required implementation handoff

The implementation agent's final report must include:

- Changed modules and the real execution order, including the actual upscaler and sharpen path.
- Final settings/defaults, legacy config behavior, and any calibrated deviations from the initial values.
- Before/after stills and short movement clips for bloom, damage/liquids, bottom pickup, sun/fixture flare, and CAS.
- Independent-toggle evidence, especially bloom off + flare on and healthy dry gameplay.
- Successful shader/Debug/Release builds, focused GPU/client checks, and relevant regression results.
- GPU timing/memory data and validation status, identified by hardware and resolution.
- Any incomplete acceptance item stated explicitly, with the affected scenario.

The definition of done is a working, visually verified Effects stack in normal gameplay, not merely menu entries, parameter structs, or shader files.

## 15. Implementation status (2026-10-02)

Implemented on branch `feature/post-effects`:

- The client feedback envelopes, lifecycle resets and the `Effects` Options page.
- The HDR split (`CmPrepareHdr` -> `framebufBloomInput` 65 -> `CmPrepareFinal`), with the emission blend and the god-rays add applied once, and the optical contribution attenuated by the level-fog transmittance at composition.
- The HDR bloom pyramid, the lens-flare pass, CAS sharpening and the combined gameplay-feedback shader, all wired into the traced frame before the UI.
- The new cvars, defaults and legacy-config behavior of section 10.
- Debug build passes; all shaders compile through DXC to SPIR-V and deploy with `build_shaders.ps1`.

Not yet verified in this environment:

- The manual visual matrix of section 13.2 and the post-effects GPU regression target of section 13.1 were not run: there is no Quake game data (`id1/pak0.pak`) in the environment, so the game cannot reach a rendered world. The GPU is present (AMD Radeon RX 9070 XT).
- The GPU timing budget of section 13.3 is therefore unmeasured, and the exact visual defaults of section 10.2 are first-run engineering values pending in-game calibration.

## 16. Physical revision (2026-10-02)

After the first in-game review the optical stack was aligned with the standard post-processing practice of a path-traced renderer:

- **Bloom has two modes.** `rt_bloom_threshold > 0` (default 3.0, measured against the auto exposure) is the strict bright pass: only pixels past the threshold and its soft knee feed the normalized pyramid, and the result is added back as `scene + bloom * mix`. `rt_bloom_threshold 0` switches to the thresholdless physical mode, where the pyramid low-passes the whole denoised HDR image with normalized kernels and the host mixes it back as `scene + (blurred - scene) * mix`; both modes are attenuated by the level-fog transmittance. `rt_bloom_intensity` is the mix fraction (0-0.2, default 0.06, and 0 disables the effect). The normalized reconstruction keeps total energy (a constant image stays constant regardless of the level count), and the Karis weighting of the first reduction still suppresses isolated fireflies.
- **Chromatic aberration moved into the linear HDR stage.** The damage and liquid edge aberration is a spectral split (6-8 taps, smooth edge mask, protected central region) applied in `CmPrepareFinal` before exposure and tone mapping; the post-upscale `EfGameplayFeedback` shader keeps only the red damage tint and the bottom `#FFD47B` pickup screen pulse. AMD's guidance to run aberration after the upscaler is not adopted here: this engine tone-maps before FSR/TAAU, and pre-tonemap fringing on clipped highlights was the priority.
- **The shared downsample-13 helper is normalized** (weights sum to one), so pyramid levels no longer gain 1.25x each.
- **The flare follows the classic image-space construction (Chapman).** A quarter-res bright pass runs a hue-preserving anti-firefly clamp and Karis weighting before the subtractive threshold; the flare pass draws eight ghosts scaled through the screen centre, each with its own chromatic dispersion and coating tint, plus a halo ring with a chromatic rim; a radial lens distortion warps the sampling coordinates in aspect-corrected space, so the ghosts and the halo ring stay circular at any screen ratio, and a Gaussian-weighted 40-tap bokeh blur softens the result at half render resolution. The host strength is physical - `rt_lensflare_intensity` is a percentage with a `1`-`100` menu range and a default of `1`, the host maps it to the optical strength and the shader applies an internal 0.1 gain - so only genuinely bright sources flare.
- The essential order matches the practice: trace -> ASVGF/denoise -> linear-HDR optics (bloom, flare, CA) -> exposure and tone mapping -> FSR/TAAU -> CAS -> aesthetic overlays (damage tint, pickup pulse) -> UI, with the dither at the end of the prepare-final stage, after the denoiser.
