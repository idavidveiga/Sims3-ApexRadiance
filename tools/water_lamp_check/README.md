# Offline water lamp check

Build `check.cpp` with the x86 MSVC compiler, the shaders include directory, and d3d9.lib, d3dcompiler.lib, user32.lib. Run the executable with the path to the pre-change water_lamps_ps.hlsl as its only argument.

Uses a hidden D3D9 window and 64x64 readback. Checks shader compilation/creation, legacy equivalence within 1 LSB, flat-normal filter identity, zero lights, alpha and analytical highlight compression/color ratios. Assertions must remain enabled. It does not substitute for moving-camera or gameplay performance tests.
