"""Locked camera overlay: zero every camera motion value on the original disc.
CAMERA-MODIFIERS (all except HEIGHT-DAMPING, which only smooths) + per-camera SWAY/TILT/LAG/ACTIVE-SCALE.
Byte-for-byte same length (digits 1-9 -> 0), written as TDOV runs like noshake.ov."""
import mmap,re,sys,struct
src,dst=sys.argv[1:3]
f=open(src,'rb'); mm=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
NL=rb'\s*\r?\n\s*'; NUM=rb'-?[0-9]+\.?[0-9]*'
def zero_vals(blk, keys, count):
    def z(m): return m.group(1)+re.sub(rb'[1-9]',b'0',m.group(2))
    total=0
    for k in keys:
        # key line, then `count` number lines
        p=rb'(\n'+re.escape(k)+NL+rb')(' + NUM + (rb'(?:'+NL+NUM+rb')')*(count[k]-1) + rb')(?=\s)'
        blk,c=re.subn(p,z,blk); total+=c
    return blk,total
out=[]; stats={'mod':0,'cam':0,'vals':0}
def emit(start, old, new):
    assert len(old)==len(new)
    a=None
    for x in range(len(old)+1):
        d = x<len(old) and old[x]!=new[x]
        if d and a is None: a=x
        if not d and a is not None: out.append((start+a,new[a:x])); a=None
MODKEYS=[b'GFORCE',b'SPEED-EFFECTS',b'ACCEL-EFFECTS',b'SURFACE-EFFECTS',b'TRACTION-EFFECTS',b'GEARCHANGE-EFFECTS',b'NITROUS-EFFECTS']
pos=0
while True:
    i=mm.find(b'CAMERA-MODIFIERS\r\n',pos) 
    if i<0: i=mm.find(b'CAMERA-MODIFIERS\n',pos)
    if i<0: break
    pos=i+16
    j=mm.find(b'GSRACECARSTUFF',i,i+20000)
    if j<0: continue
    old=mm[i-1:j]; new,c=zero_vals(old,MODKEYS,{k:1 for k in MODKEYS})
    if c: stats['mod']+=1; stats['vals']+=c; emit(i-1,old,new)
CAMKEYS={b'SWAY':3,b'SWAY-FORWARD':3,b'TILT':1,b'TILT-FORWARD':1,b'LAG':3,b'ACTIVE-SCALE':3}
for m in re.finditer(rb'\nCAMERA\r?\n(?:EXTERNAL|INTERNAL)\r?\n\{',mm):
    s=m.start(); e=s+20
    nxt=re.compile(rb'\n(?:CAMERA\r?\n(?:EXTERNAL|INTERNAL)|DRAFTING-TRAIL|GSRACECARSTUFF)').search(mm,e,s+6000)
    e=nxt.start() if nxt else s+3000
    old=mm[s:e]; new,c=zero_vals(old,list(CAMKEYS),CAMKEYS)
    if c: stats['cam']+=1; stats['vals']+=c; emit(s,old,new)
# also keep the crash/surface shake removal from noshake (subset of MODKEYS above: already covered)
out.sort()
print(src,stats,'runs',len(out),file=sys.stderr)
with open(dst,'wb') as o:
    o.write(b'TDOV'+struct.pack('<I',len(out)))
    for off,b in out: o.write(struct.pack('<QH',off,len(b))+b)
