"""Reference for the Invert filter, in numpy. tone_invert is what it does: each
colour's tone (CIELAB L*, which depends only on luminance, as in Google's HCT) becomes 100 - L*,
keeping its OKLab hue and chroma; where that colour is outside sRGB, chroma is reduced at the same
tone. On gamma-encoded sRGB. ok_invert (flipping OKLab L) and hsl_invert (flipping HSL lightness,
what the filter did first) are for comparison."""
import numpy as np
def to_lin(c): c=np.clip(np.asarray(c,float),0,1); return np.where(c<=0.04045,c/12.92,((c+0.055)/1.055)**2.4)
def to_enc(c): c=np.clip(c,0,1); return np.where(c<=0.0031308,c*12.92,1.055*c**(1/2.4)-0.055)
M1=np.array([[0.4122214708,0.5363325363,0.0514459929],[0.2119034982,0.6806995451,0.1073969566],[0.0883024619,0.2817188376,0.6299787005]])
M2=np.array([[0.2104542553,0.7936177850,-0.0040720468],[1.9779984951,-2.4285922050,0.4505937099],[0.0259040371,0.7827717662,-0.8086757660]])
M2i=np.array([[1,0.3963377774,0.2158037573],[1,-0.1055613458,-0.0638541728],[1,-0.0894841775,-1.2914855480]])
M1i=np.array([[4.0767416621,-3.3077115913,0.2309699292],[-1.2684380046,2.6097574011,-0.3413193965],[-0.0041960863,-0.7034186147,1.7076147010]])
LUM=np.array([0.2126,0.7152,0.0722])
CW=LUM@M1i  # luminance from cubed LMS'
def lab(rgb): return np.cbrt(to_lin(rgb)@M1.T)@M2.T
def lin_of(lab): return ((lab@M2i.T)**3)@M1i.T
def lstar(y): return np.where(y>0.008856,1.16*np.cbrt(y)-0.16,y*9.033)          # 0..1
def lstar_inv(l): return np.where(l>0.08,((l+0.16)/1.16)**3,l/9.033)
def in_gamut(c): return (c>=-1e-4).all(-1)&(c<=1+1e-4).all(-1)
def solve(y,ab):
    """OKLab colour with chroma ab and luminance y (Newton on L)."""
    u=ab@M2i[:,1:].T                # (..., 3) offsets of l', m', s' from L
    L=np.cbrt(np.maximum(y,0))
    done=np.zeros(np.shape(L),bool)
    for _ in range(20):  # as the filter: Newton, each colour stopping once it moves less than 1e-9
        q=L[...,None]+u
        f=(q**3)@CW-y; df=3*(q**2)@CW
        nxt=np.clip(L-f/np.where(np.abs(df)>1e-4,df,1e-4),0,1)
        step=np.abs(nxt-L); L=np.where(done,L,nxt); done|=step<1e-9
        if done.all(): break
    return lin_of(np.concatenate([L[...,None],ab],-1))
def tone_invert(rgb):
    rgb=np.asarray(rgb,float); lin=to_lin(rgb); y=lin@LUM
    yt=lstar_inv(1-lstar(y)); ab=lab(rgb)[...,1:]
    c=solve(yt,ab); good=in_gamut(c)
    lo=np.zeros(y.shape); hi=np.ones(y.shape)
    for _ in range(14):
        k=(lo+hi)/2; ok=in_gamut(solve(yt,ab*k[...,None])); lo=np.where(ok,k,lo); hi=np.where(ok,hi,k)
    c=np.where(good[...,None],c,solve(yt,ab*lo[...,None]))
    return to_enc(c)
def ok_invert(rgb):
    L=lab(rgb); L2=L.copy(); L2[...,0]=1-L[...,0]
    lo=np.zeros(L.shape[:-1]); hi=np.ones(L.shape[:-1])
    for _ in range(16):
        k=(lo+hi)/2; t=L2.copy(); t[...,1:]*=k[...,None]; c=lin_of(t)
        ok=in_gamut(c); lo=np.where(ok,k,lo); hi=np.where(ok,hi,k)
    t=L2.copy(); t[...,1:]*=lo[...,None]; return to_enc(lin_of(t))
def hsl_invert(rgb):
    rgb=np.asarray(rgb,float); return rgb+1-rgb.max(-1,keepdims=True)-rgb.min(-1,keepdims=True)
if __name__=="__main__":
    print("CW",CW)
    for c in [(1,1,1),(0,0,0),(0.5,0.5,0.5),(0.8,0.1,0.15),(0,0,1),(0.08,0.12,0.35),(0.75,0.88,0.98),(1,0.85,0)]:
        t=tone_invert(np.array(c,float)); print(c, t.round(3), 'L* in', round(float(lstar(to_lin(c)@LUM))*100,1),'out',round(float(lstar(to_lin(t)@LUM))*100,1), 'back', tone_invert(t).round(3))
