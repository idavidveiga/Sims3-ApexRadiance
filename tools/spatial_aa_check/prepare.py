from pathlib import Path
repo=Path(__file__).resolve().parents[2]
s=(repo/'patches/edge_smoothing_patch.cpp').read_text(encoding='utf-8')
# Exercise production resources / passes without the game or feature registration.
s=s[:s.index('class EdgeSmoothingPatch')]
out=repo.parents[1]/'outputs'/'spatial-aa-check';out.mkdir(parents=True,exist_ok=True)
(out/'aa_under_test.h').write_text(s,encoding='utf-8')
print(out)
