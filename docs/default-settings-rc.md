# Private RC default activation policy (2026-10-02)

User decision: enable normal player features by default, except Color and Ambient Occlusion.

Night Lights, Edge Smoothing, Depth Blur, Every-Story Ground Light, Banding Fix and all twelve performance features default on. Color remains off; Ambient Occlusion remains off by default. Developer mode remains opt-in with confirmation; profiler and capture actions never start automatically. Individual preferences and stored feature enabled states remain respected. This policy applies to missing feature states/new configurations, not an overwrite of existing user choices. Internal optional diagnostic switches are unchanged.

Source metadata verified; window/presentation control was removed for 2.5.5. No build, installation or publication performed for this default-only change.

## Ambient Occlusion reference

Default parameter values copied from the user's saved configuration on 2026-10-02: strength 1.6693748235702515, reach 1.3028700351715088, light protection 0.5, quality index 2 (High), map view True. AO remains disabled by default. Registered defaults and reset parameters agree; existing explicit settings remain respected (revision 5 unchanged). Enabling the feature does not overwrite a user's customized values. No shader change or build performed.

## Depth Blur reference

Default preferences copied from the player configuration: fixed focus, amount 100%, sharp area Large (auto-focus preference), focus speed 0.1 s, fixed start 0.318108052, transition 0.287828237, strength 100%, quality Medium, lamp highlights on, sky blur on, sharp map view on. Far plane 1000; legacy spread 0.800000012 retained but unused. Debug view stays off. Registered defaults, initial state and reset state share the same Params values. Existing explicit settings remain respected; enabling never forcibly reapplies defaults. Source only, no build or installation.
