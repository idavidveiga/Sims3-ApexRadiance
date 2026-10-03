from pathlib import Path
r=Path(__file__).resolve().parents[2]
s=(r/'apex_config.cpp').read_text(encoding='utf-8')
def function(name):
 a=s.index(name); a=s.rfind('\n',0,a)+1
 i=s.index('{',a); depth=1; j=i+1
 while depth:
  depth += (s[j]=='{')-(s[j]=='}'); j+=1
 return s[a:j]
out=r.parents[1]/'outputs'/'developer-mode-check';out.mkdir(exist_ok=True)
(out/'profile_under_test.h').write_text('namespace ApexConfig {\n'+ '\n'.join(function(n) for n in ['unsigned FeaturePart(', 'const char* ProfilePartName(', 'unsigned ProfilePartsOf(', 'void KeepProfileParts('])+'\n}',encoding='utf-8')
print(out)
