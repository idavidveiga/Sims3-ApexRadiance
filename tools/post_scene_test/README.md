# Post-scene boundary regression check

Run in an x86 Visual Studio developer prompt, from the repository root. Choose a scratch directory for all outputs:

```bat
cl /nologo /std:c++20 /EHsc /MT /I. /Iframework /Ifeatures tools/post_scene_test/post_scene_check.cpp /Fo<scratch>/post_scene_check.obj /Fe<scratch>/post_scene_check.exe /link d3d9.lib user32.lib
<scratch>/post_scene_check.exe
```

The fixture includes the production dispatcher and creates a hidden native D3D9 device. Hook registration is inert.
It checks effect order and single execution, hidden-UI fallback, invalid depth without consuming effects, refusal to
retry over already drawn UI, recovery after real scene draws resume, short scenes, internal draws and device reset.
It does not validate the game's actual draw sequence, visual equivalence or DXVK behavior.
