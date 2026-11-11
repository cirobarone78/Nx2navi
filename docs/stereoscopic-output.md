# Stereoscopic output (experimental)

The Screen settings offer three opt-in stereoscopic output layouts:

- **Half side-by-side:** left eye on the left, right eye on the right. Each eye occupies half the picture width.
- **Half top-and-bottom:** left eye above the right eye. Each eye occupies half the picture height.
- **Red/cyan anaglyph:** left-eye red and right-eye green/blue for red/cyan glasses. This simple channel combination changes colours; it is not colour-corrected anaglyph.

The default is **Off**, which preserves ordinary rendering. Use the matching half-width/half-height mode on a compatible display or external viewer; a normal monitor shows the packed views, not an automatically fused 3D image. The window resolution does not double. Full-resolution dual-width output, HDMI frame packing, shutter synchronization, quad-buffer stereo, headset runtimes and head tracking are not provided.

## Controls

**Separation** adjusts the depth effect from 0 to 100, default 20. This is a relative amount, not a physical IPD measurement. Per-eye displacement is bounded to three percent of the eye's image width. Start low and reduce it or disable stereo if uncomfortable.

**Convergence** chooses the zero-parallax distance from 1 to 1000 in the game's view-space units, default 100; these are not metres. A surface at this distance aligns between the eyes. Nearer and farther surfaces move in opposite directions. **Swap eyes** reverses the left/right assignment for a display or viewer that expects the opposite order. These settings apply live and are saved in the display section of `nx2navi.ini`.

HDR and frame generation are not supported together with this experimental stereo path. While either is selected or active, the ordinary monoscopic path remains in use without changing those settings. The normal spatial/temporal upscaler remains responsible for the game's original picture; stereoscopic packing is a later presentation effect.

The compute pass places each eye directly at the final picture size and copies the packed image without filtering.
The optional final FSR 1 sharpening pass is bypassed, so neither it nor a later scaler blends across the eye boundary.
This does not disable temporal scene upscaling.

## Rendering boundary

This is **single-view depth reprojection**, not two independently rendered cameras. It estimates two eyes from the already rendered colour picture and the perspective depth of the current Ocarina scene. It does not modify guest camera state or replay game commands twice. Missing, invalid or ambiguous depth makes the selected layout flat rather than reusing a previous frame's camera.

The association follows observed full-image copies, matching temporal-upscaler output, and same-size single-input
screen-space draws. The last case assumes the pass preserves screen coordinates; a shader that distorts UVs can
misalign depth even when these checks match. Multi-input or otherwise untracked passes lose depth association and
stay flat. Blended HUD/effect draws retain the underlying scene's depth, not a separate interface depth layer.

The source already contains the game HUD. HUD elements, transparency and effects can inherit the depth behind them and move incorrectly. The port's settings overlay is drawn later and remains monoscopic; close it with F1 to view the unobstructed packed image. Optional diagnostic movie overlays are not stereo-converted.

Surfaces hidden from the original camera and off-screen geometry cannot be recovered. Inverse reprojection and edge clamping can stretch or smear silhouettes and screen borders. Sky and invalid depth remain at zero parallax. Temporal jitter and differences between post-processed colour and scene depth can introduce artifacts. Stereo adds a compute pass and a full-picture scratch image; its actual gameplay cost has not been measured.

Small synthetic Vulkan images and native recording harnesses can verify layout, parallax mathematics, settings and state lifetime without starting the game. They do not establish actual-game render-pass selection, HUD behavior, viewing comfort, physical display compatibility or performance. A deliberate gameplay/display comparison is still required before treating this feature as production-ready.
