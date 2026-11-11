# Ambient occlusion

The Screen tab offers an additional, experimental screen-space ambient-occlusion pass. It is **off by default**. It darkens nearby creases and contacts using the visible scene depth; it does not replace the game's lighting or shadow maps.

## Settings

`ao = off|ssao|hbao` selects the method. SSAO samples a hemisphere oriented around the surface normal reconstructed from depth. HBAO searches the visible depth along screen-space directions and integrates the occlusion above the surface's tangent plane. This is not HBAO+, GTAO, or ray tracing.

`ao_strength = 0..100` controls the additional darkening, with a default of 50. Zero leaves the picture unchanged. `ao_radius = 1..100` controls the neighborhood in view-space units, with a default of 20; these units are not advertised as metres. `ao_quality = 0|1|2` selects low, medium or high sampling quality, with medium as the default. All four settings apply live. The mode must be enabled to edit the other rows.

Start with the default radius and strength. Higher quality costs more GPU time, especially at a high output resolution. A larger radius is not a draw-distance control: sampling is bounded in screen space. If the game already shades a contact, the additional AO can make it too dark; lower strength or turn the feature off.

## Rendering boundary

`runtime/vk_ao.c` captures Ocarina's scene projection from the submission snapshot used by its geometry draws. The accepted scene has a single light-format color target and a matching, single-sample D32F depth target, a full viewport, and a supported perspective projection. Smaller effect targets do not replace the current scene. The most frequently observed camera on that depth target supplies the projection.

The spatial path applies AO to the decoded RGBA16F picture in `runtime/vk_tonemap.c`. A frame with an active temporal upscaler instead applies it to the output of `runtime/vk_dlaa.c:evaluate`, before the blit and HUD composition. The pass modifies only each invocation's own color pixel and samples depth read-only. The HUD-less frame-generation picture is taken after this point. The feature needs neither additional guest patches nor the old contact-shadow G-buffer layout.

Image retirement invalidates the cached depth view under the texture lock, and the replay-order present boundary clears camera state. Missing snapshots, unsupported projections, incompatible depths or a failed compute pipeline leave the picture unchanged. Only an eligible frame allocates the compute pipeline; AO needs no persistent intermediate image or temporal history of its own.

## Limits

Temporal output must match the captured depth image exactly. If multiple equally sized scene depths make the spatial
output ambiguous, AO skips that frame rather than sampling a different camera's depth. If an active temporal upscaler
misses its evaluation point, the frame also remains unoccluded; no late fallback is applied over the completed HUD.

This is a post-process darkening approximation, not physically separate indirect lighting. It has no off-screen or hidden geometry. Thin objects and silhouettes can still show screen-space artifacts. Transparent surfaces generally do not contribute depth and can inherit the occlusion of the opaque surface behind them. There is no temporal denoiser, so fine shading can shimmer in motion. Menus and movies without an eligible scene are left alone.

The source integration and synthetic depth/color cases can be checked without a game. Those checks do not establish the final appearance, every scene's render-pass selection, camera transitions, driver compatibility, or gameplay performance. These require a deliberate gameplay comparison before treating the feature as production-ready.
