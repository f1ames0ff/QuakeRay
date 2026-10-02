# Plan: make ray-traced water caustics work

Status: source review and implementation instructions; implementation has not been changed by this review.

Reviewed baseline: `feature/water-caustics`, commit `30f4ef7a`, 2026-10-02. Line references below refer to this baseline. HLSL is the runtime priority.

## Goal and acceptance workflow

Produce a moving bright/dark caustic network on sunlit underwater receivers. The network must redistribute transmitted sunlight, include absorption along the light path, follow the water waves, and remain attached to world geometry when the camera moves.

The first acceptance milestone is a horizontal pool floor behind one air/water interface, viewed both from above and underwater. Subsequent iterations extend receiver orientation and transmitting geometry.

Each numbered iteration below is a separate implementation increment, build, and manual acceptance checkpoint. Record the settings and screenshots for that increment. Wait for the user's verdict before beginning the next increment.

## Current frame and lighting paths

The relevant order is:

1. Primary visibility and reflection/refraction update the checkerboard-packed G-buffer.
2. ASVGF gradient reprojection may replace selected G-buffer samples with previous-frame surfaces.
3. Direct lighting evaluates the sun and local lights.
4. Indirect lighting evaluates GI.
5. Composition denoises, reconstructs the checkerboard, and computes exposure.
6. The overlay callback traces photons and adds caustics to `framebufFinal`.
7. Tone mapping and upscaling consume that image.

Evidence: `renderer/Source/RHI/NvrhiFrameSkeleton.cpp:935-995,1108-1180`; `renderer/Source/RHI/RhiRtComposePass.cpp:1748-1785`.

The receiver medium is currently stored in `framebufQ2BounceThroughput.x` by `storeQ2GBuffer`, and direct lighting reads it when `rt_water_lightpath` is enabled. Camera-path extinction remains in `framebufThroughput.rgb` and is applied during composition.

Evidence: `renderer/Source/Shaders/RaygenPrimary.hlsli:83-107,907,945-947`; `renderer/Source/Shaders/HLSL/RtRaygenDirect.rgen.hlsl:148-158`; `renderer/Source/Shaders/HLSL/CmQ2Adapter.comp.hlsl:123-165`.

## Confirmed defects and required corrections

### D1. Photon tracing requests an arbitrary first hit instead of the closest hit

**Evidence:** `renderer/Source/Shaders/HLSL/CmCaustics.comp.hlsl:48-85` uses `RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH`. The same query is used to locate both the water boundary and the receiver (`:114-115,162-163`). `traceSunWaterFactor` repeats this flag in `renderer/Source/Shaders/RaygenCommon.hlsli:404`.

Traversal order is not distance order. A first accepted triangle can be behind a roof, water surface, or nearer receiver. First-hit termination is appropriate for boolean occlusion, but not for extracting an interface position, receiver, or absorption distance.

**Correction:** remove first-hit termination from all queries that retrieve physical hit data; finish the query and use the committed closest triangle. Retain it in genuinely boolean shadow tests. Validate with overlapping opaque blockers, water, and two receiver levels, including after a TLAS rebuild.

### D2. The new analytic water-light path uses the wrong direction and can invent total internal reflection

**Evidence:** `renderer/Source/Shaders/RaygenCommon.hlsli:396-455` traces toward the sampled sun position, then applies `refract(direction, normal, waterIOR)`. The resulting direction is only tested for zero and is never used for a subsequent ray. `renderer/Source/Shaders/HLSL/RtRaygenDirect.rgen.hlsl:140-158` still uses the original straight sun shadow and original `shade()` direction.

The sampled direction describes the sun in air. It cannot simply be interpreted as an internal water-to-air ray. For water IOR 1.33 this test can incorrectly extinguish sunlight at air incidence angles above approximately 48.75 degrees. The helper also returns full transmission if it misses the boundary or encounters glass before water (`RaygenCommon.hlsli:418-432`).

**Correction:** implement a documented flat-interface analytic fallback. Determine the pool interface plane, derive the water direction from the incident air direction using air-to-water Snell refraction, intersect the receiver-to-interface segment along that water direction, and test opaque visibility from the exit toward the sun in air. Evaluate transmission with the correct pair of media and incidence angle. Treat an unresolved boundary as an explicit fallback status, not evidence of full transmission. Traversal through glass must continue to the next boundary with a bounded interface count.

Use this analytic path for the caustics-disabled mode and areas outside photon coverage. The photon-covered mode will use the actual forward photon paths instead.

### D3. Caustics add a second sun contribution in the wrong place

**Evidence:** `CmCausticsComposite.comp.hlsl:156-158` adds `flux * coverage * albedo` to `framebufFinal`. `RtRaygenDirect.rgen.hlsl:155-158` independently adds the ordinary sun contribution, optionally scaled by the analytic water factor. Exposure is already calculated before the overlay (`RhiRtComposePass.cpp:1754-1777`).

This double-counts illumination, bypasses light-path/camera-path composition, and excludes the photon term from ASVGF and exposure metering. The late term still reaches the upscaler; it is not temporally isolated from the displayed image.

**Correction:** split the caustics pass into photon generation/binning and diagnostic display. Generate its data before direct lighting. In the direct pass, replace the underwater diffuse sun contribution with the photon estimate for covered receivers. Add it to the albedo-demodulated direct diffuse signal before `imageStoreUnfilteredDirect`. Let the existing composition apply albedo, camera throughput, split-field weighting, fog, and exposure once.

### D4. The old straight shadow and cluster-sky gate cannot gate refracted photon light

**Evidence:** `RaygenCommon.hlsli:482-501` tests the receiver normal against the unrefracted sun, checks `q2ClusterSkyVis`, and traces a straight opaque shadow. The photon pass has already tested visibility along a different, bent path (`CmCaustics.comp.hlsl:114-163`).

A receiver can be reached through water even when the old straight segment is occluded. Multiplying valid photon irradiance by `sunVis` would erase such caustics. It can also reject a receiver that did not previously see the sky directly.

**Correction:** evaluate the photon-covered branch independently of `traceSunVisibility` and its cluster gate. Photon paths determine sunlight visibility for that branch. Apply receiver orientation and surface matching using photon arrival directions and receiver geometry. Continue using the ordinary gate for dry surfaces and the appropriate segmented shadow for the analytic fallback.

### D5. Photon power is not calibrated as receiver irradiance

**Evidence:** `CmCaustics.comp.hlsl:182-190` stores `sunColor * intensity * (1 - F)`; `CmCausticsComposite.comp.hlsl:106-108,156-158` decodes it and multiplies by albedo. `RaygenCommon.hlsli:530-547` uses chroma-adjusted sun color and a Lambertian `1/pi` factor. `Light.hlsli:338-354` gives the directional light unit sampling weight.

There is no explicit launch-plane projected area, light-path extinction, receiver surface-area convention, or matching BRDF contract.

**Correction:** use the radiometric contract in the next section. Measure integrated energy on a flat receiver before enabling waves.

Do not divide accumulated power by photon count: that removes the focusing effect. Do not renormalize a zero-photon neighborhood into a bright neighborhood. Zero received flux is a legitimate dark caustic band.

### D6. Light-path absorption is missing from photons

**Evidence:** `CmCaustics.comp.hlsl:161-194` has the water-entry and receiver positions but never calls `getMediaTransmittance`. The existing extinction model is in `Media.hlsli:42-56`.

**Correction:** multiply each photon's power by water transmittance over its actual in-water path length before accumulating or gathering it. Keep the separate camera-path factor in composition. A change in depth must affect illumination even when the viewing path is held constant.

### D7. The receiver-medium channel does not follow ASVGF's surface replacement

**Evidence:** `CmQ2GradientReproject.comp.hlsl:155-181` replaces surface position, normal, material data, and view direction with history. It does not replace `Q2BounceThroughput`. That image has no history allocation (`GenerateShaderCommon.py:956`), yet the direct pass reads its current medium together with the potentially historical surface.

At water edges or when the camera moves, medium and receiver position can describe different paths.

**Correction:** make medium and validity part of the same current/previous G-buffer contract as position and material. A concrete low-descriptor-risk route is to expand the existing `MetallicRoughness` entry from RG8 to RGBA8 (`GenerateShaderCommon.py:869`), preserving its entry order and existing history allocation. Store medium as a normalized byte in `.z`, and validity/path flags as a normalized byte in `.w`; decode with rounding. Preserve `.xy`. Update all primary, refl/refr, and sky writers and current/previous readers. Restore the unused RGB fields of `Q2BounceThroughput` to their previous convention.

Check medium/path compatibility during gradient candidate matching, and copy the complete RGBA material/metadata value when replacing the surface. Do not write raw media IDs greater than one directly to an UNORM channel.

### D8. The composite reads a neighboring refraction path and paints the resolved image

**Evidence:** `CmCausticsComposite.comp.hlsl:42-44,69,156-158` reads the odd checkerboard partner regardless of the displayed pixel's path. Medium is inspected only in debug modes (`:46-67`). `CmQ2Interleave.comp.hlsl:70-93` has already mixed reflected and refracted fields before this overlay runs.

The addition is not weighted by the camera's water transmission and can contaminate reflection, dry geometry, or sky near water.

**Correction:** production caustics use the direct pass's own packed pixel, surface position, medium, and validity. Make the old composite a diagnostic-only view. The existing interleave handles the reflection/refraction split after lighting; do not manually fetch the other field for production shading.

### D9. XY bins plus one mean height cannot distinguish different receivers

**Evidence:** `CmCaustics.comp.hlsl:173-194` bins every receiver by XY and sums an encoded Z. `CmCausticsComposite.comp.hlsl:137-142` tests a single interpolated mean Z with a tolerance of at least 16 Quake units. Receiver normals and surface identity are absent.

Different floors in the same column are averaged into a fictitious intermediate surface. Vertical walls cannot be represented as ordinary XY surface area. A height-only tolerance can leak light onto unrelated geometry.

**Correction:** retain per-photon position, arrival direction, receiver geometric normal, power, and validity. Use bins to accelerate finding photons, not to discard receiver identity into a single mean. Gather only photons within a receiver-plane distance tolerance and normal-compatible surface neighborhood. Start with horizontal floors; add surface-oriented or 3D bins for walls in a later iteration.

### D10. Fixed-point depth and flux sums can wrap

**Evidence:** `CmCaustics.comp.hlsl:187-194` atomically adds 32-bit unsigned flux and biased height. At Z near zero, each encoded height is already about 524288, so about 8192 contributions exhaust a cell's 32-bit depth sum. A 512-by-512 launch grid can generate 262144 photons. The composite uses the wrapped mean for its receiver test.

**Correction:** use the per-photon record and binning design below. It stores power and geometry as floats without requiring floating-point atomics. Integer atomics count photons or allocate index slots only; those counts are bounded by the launch budget. Validate totals and buffer capacity explicitly. Report overflow instead of silently losing or wrapping energy.

### D11. Gathering has a half-cell coordinate error

**Evidence:** photons launch at `(cell + 0.5) * texelSize` (`CmCaustics.comp.hlsl:100-101`) and receivers are binned with `floor(gridCoords)` (`:173-174`). Composite bilinear interpolation directly uses `floor(gridCoords)` and `frac(gridCoords)` (`CmCausticsComposite.comp.hlsl:71-101`).

For cell-centered values, the sampling coordinate is `gridCoords - 0.5`. The current convention shifts reconstructed lighting by half a cell and drops the last row/column from the bilinear domain.

**Correction:** define one explicit cell-center convention in shared caustics parameters and gather helpers. For the new photon gather, calculate bin-neighborhood bounds from the actual spatial kernel. If a resolved cell image remains for diagnostics, sample it with the half-cell correction and explicit edge handling.

### D12. Launch and receiver coverage share an origin despite sunlight drift

**Evidence:** `NvrhiFrameSkeleton.cpp:1131-1154` offsets the grid center toward the sun at launch height, then sends a single `gridMinAndTexel` to both the launch (`CmCaustics.comp.hlsl:100-106`) and receiver binning (`:173-179`). Launch-to-pool drift can move hits out of that same receiver square. `sunZ` is clamped to 0.1 even for shallow sun directions.

**Correction:** separate launch bounds from receiver bounds. Center the receiver domain on the pool/current world window; project that domain upward along the actual sun direction to derive the launch aperture. Add a conservative refraction/kernel margin. Use a scene-based fixed launch height. Handle below-horizon and grazing sun explicitly, and count photons discarded outside the receiver domain. Validate finite positive extent and valid resolution before computing texel size.

The existing camera-window snapping already moves by whole texels. It does not, by itself, continuously change lattice phase in the overlapping interior. Preserve that property rather than treating all camera anchoring as a bug.

### D13. Fresnel and wave filtering depend on inconsistent conventions

**Evidence:** photons use F0 = 0.1 (`CmCaustics.comp.hlsl:182`). The camera water entry repeats it and the exit uses a separate heuristic (`RaygenPrimary.hlsli:734-788`). The IOR-aware helper exists in `BRDF.hlsli:68-74`. Photon wave sampling uses `rayCone.width = texelSize` (`CmCaustics.comp.hlsl:147-151`); `Water.hlsli:23-43` uses that width to select a normal-map mip.

Changing photon resolution changes the surface slopes used to bend the light, not just sampling quality. With water IOR 1.33, physical normal-incidence Fresnel is approximately 0.020 rather than 0.1.

**Correction:** define one interface/Fresnel convention and one world-space optical wave field. Use geometric surface normals consistent with `HitInfo.hlsli:193-207`. Photon optical normal LOD must be independent of camera distance and receiver-cell resolution. Camera anti-aliasing may use a larger footprint, but should filter the same underlying wave field. Change camera Fresnel in its own manual-check iteration.

### D14. Glass and alpha-tested geometry behave differently on photon paths

**Evidence:** photons stop at the first non-sky hit and require water (`CmCaustics.comp.hlsl:111-139`), so glass above a pool discards them. `RAY_FLAG_FORCE_OPAQUE` suppresses alpha rejection (`:51`). Ordinary shadows have an alpha-tested hit shader (`HLSL/RtAlphaTest.rahit.hlsl:36-61`). Water, glass, and acid share `INSTANCE_MASK_REFRACT` (`ASManager.cpp:550-574`).

**Correction:** add bounded, closest-boundary transmission through glass in the forward light path. Respect thin-glass/media-change flags instead of assuming all glass is a volume. For alpha-tested candidates, reproduce the existing alpha decision and commit only accepted candidates. Ordinary opaque blockers still stop photons. Never add the shared refract bit to a binary global shadow mask as a water-only fix.

### D15. Temporal depth matching and caustic peak filtering need separate verification

**Evidence:** gradient matching adds `motion.z` to signed Q2 depth without the negative-depth handling used by the temporal shader (`CmQ2GradientReproject.comp.hlsl:111-120`; `CmQ2Temporal.comp.hlsl:266-283`). Split paths store negative full path length (`RaygenPrimary.hlsli:945-947`). The adapter clips direct peaks against eight times neighboring luminance (`CmQ2Adapter.comp.hlsl:71-108`).

**Correction:** bring signed-depth matching into one verified convention and reject invalid/near-zero depth before division. Compare the raw caustic signal with filtered HF output. Adapt caustic filtering based on measured loss or ghosting; a fixed peak clamp is not proof that the energy estimator is correct.

### D16. GI and local lights remain media-blind

**Evidence:** the new transmission factor is used only for the sun in direct lighting (`RtRaygenDirect.rgen.hlsl:107-127,148-158`). Sky visibility still uses the ordinary binary shadow (`RaygenCommon.hlsli:504-510`) and indirect rays use world masks without refracting instances (`RaygenCommon.hlsli:140-156`).

**Correction:** isolate the direct-sun result first, then add medium-aware attenuation to local-light and sky/GI paths in a separate acceptance increment. The final pool may retain indirect illumination; dark caustic bands mean no transmitted direct sun, not necessarily zero total radiance.

## Radiometric contract

Use forward photon power, not a per-cell average of photon brightness.

Definitions:

- `D_air`: unit sun travel direction, currently `-params.sunDirection.xyz`.
- `E_perp`: the sun's irradiance on a plane perpendicular to its travel direction, using the same color convention as direct lighting.
- `A_launch`: horizontal area represented by one launch sample, divided by samples per launch cell.
- `F_entry`: air-to-water Fresnel reflectance at the sampled water normal.
- `d_water`: actual in-water photon path length in Quake units.
- `T_light`: `getMediaTransmittance(WATER, d_water)`.

For a horizontal launch plane:

`Phi_photon = E_perp * max(0, -D_air.z) * A_launch * (1 - F_entry) * T_light`.

For a horizontal receiver cell:

`E_receiver = sum(Phi_photon) / A_receiver`.

For the recommended surface-kernel gather, use a kernel normalized per true receiver surface area:

`E_receiver(x) = sum(Phi_photon * K_surface(x, photonPosition))`.

The diffuse value written to the albedo-demodulated direct channel is:

`directDiffuse_caustic = E_receiver / pi`.

Important conventions:

1. Receiver incidence is already represented by photon density per surface area. Do not multiply that irradiance by another receiver `NdotL`; on a flat surface that would darken the term twice. Arrival directions are still needed for surface validation and directional/specular response.
2. For a tilted receiver, convert projected bin area into actual surface area, or use the normalized surface kernel. XY bins alone do not define a usable area for a vertical wall.
3. Albedo, metallic diffuse weight, camera transmittance, and checkerboard split weighting belong to the existing composition, not photon power.
4. The source/receiver areas cancel when their cells have equal area and there is one sample per cell. The current missing explicit area contract is not proof of a universal resolution-squared brightness error. Verify conservation numerically.
5. `CAUSTICS_FLUX_SCALE` is currently applied at encoding and removed at decoding. It is not an extra factor making light 256 times dimmer.
6. Do not normalize the measured map to average one in the current view. That would brighten shadowed pools and make illumination depend on visible coverage.
7. `rt_caustics_intensity` is an art multiplier applied once to the transmitted photon term. Preserve coverage/validity when it is zero; otherwise the host currently disables the pass and can restore unrelated fallback sunlight (`VulkanDevice.cpp:842-844`).

## Target data flow and resource contract

The target order is:

`TLAS/vertex data ready -> primary/refl-refr -> coherent gradient reprojection -> photon trace/binning -> direct gather -> indirect -> ASVGF/composition -> exposure -> tone map -> upscale`.

Photon tracing itself does not require the screen G-buffer; direct gathering does.

Use these concrete records and passes:

1. **Photon records:** one output slot per launch sample, containing validity, world receiver position, geometric normal, arrival direction, water path length, and RGB power. A rejected sample writes an invalid record every frame.
2. **Bin counts:** integer counts for the receiver-domain bins.
3. **Prefix sum and scatter:** exclusive bin offsets and a packed list of photon indices. Capacity equals the maximum launch sample count, not an arbitrary per-cell cap.
4. **Direct gather:** use the receiver's own world position and medium to search nearby bins; reject incompatible planes/normals before evaluating the normalized kernel. Sum float power from the records. Empty evaluated regions remain dark.
5. **Diagnostics:** visualize photon counts, path rejection reasons, light-path transmittance, receiver validity, and final irradiance. Diagnostic color replaces the displayed diagnostic view rather than adding an unmetered color that looks like broken rendering.

This avoids unsupported float-atomic assumptions and removes the biased-height sum. At the current maximum resolution, the record budget is 262144 for one sample per launch cell; increasing samples requires an explicit capacity change and division of launch area by sample count.

The pass must return a same-frame output object with buffer handles, grid metadata, frame ID, generation ID, and domain validity. The direct pass binds this object even for an empty evaluated domain. Missing or stale data is distinct from a valid zero-photon result.

Mode behavior:

| Mode | Underwater diffuse sun |
|---|---|
| `rt_water_lightpath 0`, `rt_caustics 0` | Legacy baseline |
| `rt_water_lightpath 1`, `rt_caustics 0` | Corrected analytic transmitted sun |
| `rt_water_lightpath 1`, `rt_caustics 1`, evaluated photon coverage | Photon estimate replaces analytic sun |
| Photon mode outside complete coverage | Analytic fallback; controlled boundary blend |
| Evaluated coverage with zero photons | Zero transmitted direct sun, with other lighting composed normally |

For this milestone, apply photon replacement to diffuse sun. Keep any specular sunlight approximation explicit and attenuated; evaluate a photon-direction BSDF estimate separately when extending to glossy receivers.

RHI requirements:

- Maintain clear-to-count, trace-to-binning, prefix-to-scatter, and scatter-to-direct read dependencies. Use generated binding numbers and UAV/SRV state transitions on the same resource handles.
- Keep frame-slot resources alive through their consuming submission; a rotating frame slot is not automatically the immediately previous frame for temporal history.
- Resize/invalidate records and bin data together when resolution, extent, map, sun, or water parameters change.
- Replace duplicated binding-array sizes with a single count contract and compile-time checks. The previously fixed `[12]` versus 13 direct-image bug is already corrected in `RhiRtDirectPass.h:288-291`; do not report it as a current defect.
- Expand `MetallicRoughness` in place rather than inserting a new history image in the middle of the registry. New framebuffer entries change the generated sampled binding offset (`GenerateShaderCommon.py:1301`); hard-coded offsets such as the caustics pass's 124 must be audited if the registry size changes.
- Keep new global-uniform fields at the end and regenerate the host/HLSL headers and probes together.

## Ordered implementation iterations

### Iteration 0 — Reproducible baseline and diagnostic contract

**Files:** `Quake/gl_vidsdl.c`, caustics shaders, `RhiCausticsPass.*` for diagnostic extensions only.

1. Capture legacy output with both features disabled, then the current raw photon field separately.
2. Add counts for launched rays, nearest water hits, opaque blocks, refracted rays, receiver hits, and out-of-domain rejects. Count invalid normals/parameters and incomplete interface traversal.
3. Display the pixel's own medium and validity; label the current debug mode 4 as a paired-field view and mode 5 as an own-field view.
4. Make diagnostic display possible without producing sunlight; the current host gating requires a sun and positive intensity even for medium-only debug views.
5. Record sun direction, water IOR/color, wave strength, domain origin, and cell size with the capture.

**Manual gate:** scene geometry renders normally in the baseline; own-field metadata matches water/dry boundaries; a photon rejection counter explains any empty map. No conclusion about driver health is drawn from a screenshot alone.

### Iteration 1 — Closest-hit paths and corrected analytic transmission

**Files:** `CmCaustics.comp.hlsl`, `RaygenCommon.hlsli`, `RtRaygenDirect.rgen.hlsl`, shared caustics/interface helpers if introduced.

1. Correct D1 for both boundary and receiver queries.
2. Correct D2 using the flat-interface fallback contract. Report unsupported or unresolved paths explicitly. Keep the shared world shadow masks unchanged.
3. Make the opaque sun visibility use the segmented air/water path for the analytic underwater branch, not the original straight ray.
4. Test the analytical flux against the radiometric contract. For a flat floor, match the launch-plane irradiance rather than multiplying a constant beam intensity by the water-direction cosine again.
5. Issue media queries only for eligible underwater receivers and count them in diagnostics.

**Manual gate:** with `rt_caustics 0`, a flat pool receives tinted, depth-dependent light; changing sun angle across 48.75 degrees does not abruptly black out transmitted sunlight; an opaque roof blocks it; dry geometry and glass windows keep their baseline behavior. Check with `rt_denoiser 0` first, then `1`.

### Iteration 2 — Coherent receiver metadata and signed-depth matching

**Files:** `GenerateShaderCommon.py`, generated framebuffer/header/probe outputs, `RaygenPrimary.hlsli`, `Surface.hlsli`, `CmQ2GradientReproject.comp.hlsl`, relevant RHI image-format bindings, direct shader.

1. Implement D7's RGBA8 metadata route and update every writer, including early-out and sky cases.
2. Decode medium/flags consistently; check history medium/path compatibility before reusing a gradient surface.
3. Implement and verify D15's signed-depth matching correction independently of caustic amplitude.
4. Rebuild all consuming shaders and host image wrappers. Keep metadata together with the surface selected by reprojection.

**Manual gate:** move the camera through the water boundary, view the same floor from air and underwater, and inspect metadata with denoising on/off. No stale underwater labels appear on dry surfaces or reflected sky. Normal rendering remains visually equivalent when the new lighting modes are disabled.

### Iteration 3 — Energy-calibrated photons and receiver gather, with waves disabled

**Files:** `CmCaustics.comp.hlsl`, new binning kernels, shared caustics data definitions, `RhiCausticsPass.*`, diagnostic composite.

1. Implement the photon-record/count/prefix/scatter pipeline, eliminating D9/D10's mean-height and fixed-point power accumulation.
2. Apply D5/D6's launch power and Beer-Lambert contract; share the direct sun's color convention.
3. Separate source and receiver bounds (D12); use fixed scene launch height and a conservative complete aperture for the tested pool.
4. Correct D11's bin/sample coordinates; use a surface-area-normalized gather and explicit support radius.
5. Validate zero/negative/non-finite extent, sunlight below the supported horizon, and record/index capacity before dispatch.
6. Keep wave strength at zero, fixed sun, and fixed exposure during calibration.

**Manual/numeric gate:** a flat, unobstructed pool has a nearly uniform transmitted field; the integrated received flux matches launched transmitted flux after accounting for blocked/missed/outside photons. Changing launch resolution from 128 to 256 to 512 or doubling samples does not systematically change average brightness. Aim for less than 5% difference in a well-covered interior test region. A covered pool has zero transmitted direct sunlight.

### Iteration 4 — Same-frame direct integration and sun replacement

**Files:** `NvrhiFrameSkeleton.cpp`, `RhiCausticsPass.*`, `RhiRtDirectPass.*`, `RtRaygenDirect.rgen.hlsl`, diagnostic composite.

1. Move photon production before the direct pass and return the same-frame output object.
2. Bind records/bin indices/domain metadata to the direct shader; keep array counts, descriptors, and resource transitions consistent.
3. Implement D3/D4/D8: gather at the receiver's own packed pixel and replace its diffuse sun term without the straight-sun/cluster gate.
4. Implement covered-zero versus uncovered fallback semantics. Blend at incomplete domain margins, not at every dark photon bin.
5. Remove production additions to `framebufFinal`; retain the composite solely for diagnostic display.
6. Apply intensity once. Changing intensity to zero must not silently restore the ordinary underwater sun.

**Manual gate:** with waves off, analytic and photon modes approximately match for a flat pool. With photons blocked, the underwater direct channel is dark. Reflected sky and dry ledges receive no caustic overlay. Repeat from underwater. Auto-exposure responds to the photon illumination. Test a refracted path that reaches a receiver behind an obstacle to the old straight-sun path.

### Iteration 5 — Optical waves, shared Fresnel, and sampling quality

**Files:** `Water.hlsli`, interface helper, `RaygenPrimary.hlsli`, photon shader, caustics parameters/host.

1. Fix the world-space optical normal footprint (D13); decouple it from launch resolution and camera position.
2. Share IOR-derived Fresnel between photons and camera water entry/exit. Validate actual TIR for underwater camera paths without the ad hoc incidence remapping.
3. Enable waves and add deterministic stratified sample jitter keyed by world launch-cell coordinates and frame sample index. Keep the source-area weight correct for sample count.
4. Keep receiver-window lattice phase stable; invalidate newly exposed bins and count aperture misses.
5. Tune photon density and surface kernel radius based on resolved world-space detail, not intensity.

**Manual gate:** bright and dark bands move with the waves; setting `rt_water_normstren 0` removes focusing; freezing wave time freezes the network; moving the camera while time is frozen preserves the same pattern on overlapping world geometry. Doubling resolution adds detail without changing the coarse optical field or mean energy.

### Iteration 6 — Temporal behavior and performance

**Files:** Q2 adapter/temporal/atrous shaders as needed, caustics diagnostics and RHI profiling.

1. Compare raw direct caustics with ASVGF HF reconstruction and inspect the existing anti-firefly clamp.
2. Verify water/dry rejection and response to moving waves, camera cuts, and sun changes. Adjust filtering from these measurements rather than hiding faulty photon paths with a global clamp.
3. Add short-lived world-space history only if raw sampling plus ASVGF remains insufficient. If added, history must use preceding-frame world coordinates and receiver/parameter generation IDs; reset on incompatible changes.
4. Profile photon trace, binning, and gather separately at 128/256/512. Bound gather work through bin neighborhood/support radius and diagnose excessive photon density; preserve energy if a budget limit is introduced.

**Manual gate:** the network stays visible with denoising and FSR enabled, with no long trails or full-frame smearing. Frozen input converges instead of flickering. Resize, map reload, and cvar changes produce neither stale buffers nor assertions. Record GPU milliseconds for each caustics stage.

### Iteration 7 — Transmitting geometry and more receiver surfaces

**Files:** forward photon/interface helpers, binning/gather, alpha sampling helpers, relevant RHI texture bindings.

1. Implement D14's glass continuation and alpha-tested candidate acceptance.
2. Preserve absorption and interface state across every supported segment; bound recursion/interface count and report exhaustion.
3. Extend bins/gather to sloped floors and vertical walls with true surface-area normalization and per-receiver validation.
4. Add directional/specular photon response for glossy receivers using the stored arrival directions and the engine's BRDF/demodulation contract.

**Manual gate:** sunlight through a glass skylight produces pool caustics; an opaque skylight blocks them; transparent texels in a grille let them through. Stacked receiver levels do not share light, and walls have oriented caustics without floor leakage.

### Iteration 8 — GI/local-light consistency and final defaults

**Files:** `RaygenCommon.hlsli`, `RtQ2Indirect.rgen.hlsl`, direct local-light path, volumetric consumers when enabled, cvar/UI documentation.

1. Implement D16's light-path attenuation for local lights and sky/GI using medium-aware paths.
2. Verify any volumetric water path uses the same interface/transmission conventions.
3. Re-enable the complete lighting setup and tune defaults after the energy and temporal gates pass.
4. Document the production/debug modes and supported receiver/light configurations.

**Manual gate:** sunlit pools show the moving network at normal gameplay settings; roof-shadowed pools do not receive transmitted sun; any remaining illumination is explainable GI or local light. Indoor lamps and glass windows remain functional.

## Verification instructions for every implementation increment

- Build/deploy a matching host and HLSL set; use `build_shaders.ps1 -Rebuild -GenCommon` after generated format/uniform changes, followed by the normal Debug build.
- Keep the monitor connected during runtime verification. The prior `VK_ERROR_UNKNOWN` surface-capability failure cleared when the display returned; it is not evidence that caustic math caused driver corruption.
- Use the existing `quick.sav` for visual continuity, plus a controlled open pool, a roofed pool, a glass-covered pool, and separated receiver levels for path correctness. The actual sun direction must reach the test water aperture.
- Set cvars directly by name. Use short explicit manual A/B operations and `screenshot`; avoid long startup `wait` queues that postpone the user's console commands.
- Isolate with `rt_caustics 0`, `rt_water_lightpath 0/1`, `rt_denoiser 0/1`, `rt_water_normstren 0/1`, and `rt_gi_level 0/1` as appropriate. `rt_upscale_fsr31 0` disables FSR but still leaves the engine's TAAU fallback; it is not a temporally unfiltered reference.
- Use validation for new RHI descriptors/states and verify shader bindings against the generated tables. Tests should target nearest-hit ordering, energy conservation, valid-zero coverage, metadata history, and record/bin capacity.
- Save settings, raw diagnostic images, and production output. A build passing is not manual acceptance of the effect.

## Completion criteria

The feature is ready when the same scene passes all of the following:

1. Photon paths respect opaque occluders and supported transmitting boundaries.
2. Flat-water flux is calibrated and approximately resolution/sample-count independent.
3. Light-path absorption changes with depth, independently of camera absorption.
4. Photon sunlight replaces the underwater diffuse sun term and participates in composition/exposure.
5. Caustics stay on valid receiver geometry, including when the camera crosses the water boundary.
6. Waves produce moving focus/defocus bands without mean-energy drift.
7. Denoising/upscaling preserve the effect without long ghosting or full-frame artifacts.
8. Covered-zero, out-of-domain fallback, intensity zero, and disabled-feature modes have explicit, tested behavior.
9. Runtime validation, resizing, map changes, and capacity checks pass, with measured GPU cost.
10. The user has manually accepted every implementation increment.
