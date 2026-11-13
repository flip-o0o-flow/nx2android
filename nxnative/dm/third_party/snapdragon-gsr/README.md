# Snapdragon Game Super Resolution 1 (GSR 1)

Qualcomm's single-pass spatial upscaler, made for Adreno GPUs: from https://github.com/SnapdragonStudios/snapdragon-gsr, `sgsr/v1/include/glsl/`
(fetched 2026-11-13, branch main), unmodified here, BSD-3-Clause (`LICENSE`).

Used by `dm/android/shaders/upscale.frag` (the present's upscale, `DM_VK_UPSCALE=gsr`, `dm/android/gfx_vk_present.c`): the edge-direction variant's code,
adapted to the present pass (GLSL 450, the window image's size from `textureSize` instead of a uniform, the sharpness from a push constant).
