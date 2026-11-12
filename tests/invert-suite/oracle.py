"""Ceiling: keep the output's premultiplied colour p, choose the best alpha per pixel."""
import json, sys, numpy as np
from PIL import Image
from measure import backdrops, SUITE, CATS
run=sys.argv[1]
idx=json.load(open(f"{SUITE}/index.json")); bds=list(backdrops().values())
acc={}; acc_now={}
cand=np.linspace(0,1,51)
for e in idx:
    s="holdout" if e["holdout"] else "train"
    g=np.load(f"{SUITE}/{e['name']}.npz"); o=np.asarray(Image.open(f"{run}/render/{e['name']}_0.png").convert("RGBA")).astype(np.float32)/255
    p=o[...,:3]; pmax=p.max(-1)
    ideals=[g["gt_c"]+b*(1-g["gt_a"][...,None]) for b in bds]
    best=np.full(pmax.shape,1e9); 
    for a in cand:
        aa=np.maximum(a,pmax)
        err=np.mean([np.abs(p+b*(1-aa[...,None])-I).max(-1) for b,I in zip(bds,ideals)],0)*255
        best=np.minimum(best,err)
    now=np.mean([np.abs(p+b*(1-o[...,3:])-I).max(-1) for b,I in zip(bds,ideals)],0)*255
    for c in CATS:
        m=g[c]
        if m.sum()==0: continue
        x=acc.setdefault((s,c),[0,0]); x[0]+=best[m].sum(); x[1]+=m.sum()
        y=acc_now.setdefault((s,c),[0,0]); y[0]+=now[m].sum(); y[1]+=m.sum()
for s in ["train","holdout"]:
    cn={c:acc_now[(s,c)][0]/acc_now[(s,c)][1] for c in CATS}; co={c:acc[(s,c)][0]/acc[(s,c)][1] for c in CATS}
    print(f"{s:8s} now    {np.mean(list(cn.values())):6.2f} | "+" ".join(f"{c} {v:5.1f}" for c,v in cn.items()))
    print(f"{s:8s} ceiling{np.mean(list(co.values())):6.2f} | "+" ".join(f"{c} {v:5.1f}" for c,v in co.items()))
