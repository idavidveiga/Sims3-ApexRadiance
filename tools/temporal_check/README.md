# Offline temporal candidate checks

Build temporal_check.cpp with the production features/shader_patches.cpp, D3D9/D3DCompiler and features include paths. Extract kResolveSource and Invert from patches/edge_smoothing_patch.cpp into scratch resolve_under_test.h and inverse_under_test.h before building. Pass a capture folder containing VS_2D65E890.bin (the 2026-10-02 15:40:35 pool point).

The fixture verifies that the captured pool is refused without bytecode mutation, that its existing grain variant and the unchanged pool shader are accepted by a hidden native D3D9 device, and that ordinary geometry keeps its jitter. It compiles and creates the production ps_3_0 resolve shader. 108 CPU checks validate phase displacement, unchanged depth/w and reprojection across zoom and phase transitions. No shader/game binary is included in the source archive.

This is not a gameplay replay: it does not reproduce the triangles, validate the pool fix, measure FPS, or emulate the pool renderer. The protection is a targeted candidate requiring the same camera-motion test in game.
