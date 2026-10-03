# Persistent game Edge Smoothing compatibility notice

The menu checks the actual D3D9 backbuffer multisample description while open, after startup. A failed/unknown query shows no warning. Only enabled conflicting effects are listed: Apex Edge Smoothing, Depth Blur, Ambient Occlusion and shore Water Reflections (Night Lights on and shore strength above zero). Lamp glow alone does not trigger the water warning.

An amber card appears above Overview and the relevant effects pages. It has a Lucide warning icon and one primary action: How to turn it off. Instructions expand inline; no popup, dismissal, automated game-setting edit or persistent preference is involved. The condition is recomputed, so it survives reopening and restarting while the conflict exists, and disappears after the backbuffer becomes non-MSAA or conflicting effects are disabled. Instructions preserve Apex settings. EN/PT/ES/FR translations included.

The three old compact pause notices were replaced by this shared page notice. Startup gating follows the existing menu Loading state. No build or installation performed; gameplay first-load, device-reset and narrow layout checks remain pending.
