"""OKLab lightness inversion, as the Invert filter does it: L becomes 1 - L, hue and chroma (a, b)
kept, chroma reduced at the same L where the result is outside sRGB. On gamma-encoded sRGB."""
import numpy as np
def to_lin(c): c=np.asarray(c,float); return np.where(c<=0.04045,c/12.92,((c+0.055)/1.055)**2.4)
def to_enc(c): c=np.clip(c,0,1); return np.where(c<=0.0031308,c*12.92,1.055*c**(1/2.4)-0.055)
M1=np.array([[0.4122214708,0.5363325363,0.0514459929],[0.2119034982,0.6806995451,0.1073969566],[0.0883024619,0.2817188376,0.6299787005]])
M2=np.array([[0.2104542553,0.7936177850,-0.0040720468],[1.9779984951,-2.4285922050,0.4505937099],[0.0259040371,0.7827717662,-0.8086757660]])
M2i=np.array([[1,0.3963377774,0.2158037573],[1,-0.1055613458,-0.0638541728],[1,-0.0894841775,-1.2914855480]])
M1i=np.array([[4.0767416621,-3.3077115913,0.2309699292],[-1.2684380046,2.6097574011,-0.3413193965],[-0.0041960863,-0.7034186147,1.7076147010]])
def lab(rgb): return np.cbrt(to_lin(rgb)@M1.T)@M2.T
def lin_of(lab): return ((lab@M2i.T)**3)@M1i.T
def ok_invert(rgb):
    L=lab(rgb); L2=L.copy(); L2[...,0]=1-L[...,0]
    lo=np.zeros(L.shape[:-1]); hi=np.ones(L.shape[:-1])
    for _ in range(16):
        k=(lo+hi)/2; t=L2.copy(); t[...,1:]*=k[...,None]; c=lin_of(t)
        ok=((c>=-1e-6)&(c<=1+1e-6)).all(-1); lo=np.where(ok,k,lo); hi=np.where(ok,hi,k)
    t=L2.copy(); t[...,1:]*=lo[...,None]; return to_enc(lin_of(t))
def hsl_invert(rgb):
    rgb=np.asarray(rgb,float); return rgb+1-rgb.max(-1,keepdims=True)-rgb.min(-1,keepdims=True)
if __name__=="__main__":
    for c in [(1,1,1),(0,0,0),(0.5,0.5,0.5),(1,0,0),(1,1,0),(0,0,1),(0.08,0.12,0.35)]:
        print(c, hsl_invert(c).round(2), ok_invert(np.array(c,float)).round(2))
