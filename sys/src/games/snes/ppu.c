#include <u.h>
#include <libc.h>
#include <thread.h>
#include "dat.h"
#include "fns.h"

int ppux, ppuy, rx;
static u8int mode, bright, pixelpri[2];
static u32int pixelcol[2];
u16int vtime = 0x1ff, htime = 0x1ff, subcolor, mosatop;
uchar pic[256*239*2*9];
u16int m7[6], hofs[4], vofs[4];

enum { OBJ = 4, COL = 5, OBJNC = 6 };

static u16int
darken(u16int v)
{
	u8int r, g, b;
	
	r = (v >> 10) & 0x1f;
	g = (v >> 5) & 0x1f;
	b = v & 0x1f;
	r = r * bright / 15;
	g = g * bright / 15;
	b = b * bright / 15;
	return r << 10 | g << 5 | b;
}

static void
pixeldraw(int x, int y, u16int v)
{
	uchar *p;

	if(bright != 0xf)
		v = darken(v);
	p = pic + (x + y * 256) * 2;
	*p++ = v;
	*p = v >> 8;
}

static int
window(int n)
{
	int a, w1, w2;

	a = reg[0x2123 + (n >> 1)];
	if((n & 1) != 0)
		a >>= 4;
	if((a & (WIN1|WIN2)) == 0)
		return 0;
	w1 = rx >= reg[0x2126] && rx <= reg[0x2127];
	w2 = rx >= reg[0x2128] && rx <= reg[0x2129];
	if((a & INVW1) != 0)
		w1 = !w1;
	if((a & INVW2) != 0)
		w2 = !w2;
	if((a & (WIN1|WIN2)) != (WIN1|WIN2))
		return (a & WIN1) != 0 ? w1 : w2;
	a = reg[0x212a + (n >> 2)] >> ((n & 3) << 1);
	switch(a & 3){
	case 1: return w1 & w2;
	case 2: return w1 ^ w2;
	case 3: return w1 ^ w2 ^ 1;
	}
	return w1 | w2;
}

static void
pixel(int n, int v, int pri)
{
	int a;
	
	a = 1<<n;
	if((reg[TM] & a) != 0 && pri > pixelpri[0] && ((reg[TMW] & a) == 0 || !window(n))){
		pixelcol[0] = v;
		pixelpri[0] = pri;
	}
	if((reg[TS] & a) != 0 && pri > pixelpri[1] && ((reg[TSW] & a) == 0 || !window(n))){
		pixelcol[1] = v;
		pixelpri[1] = pri;
	}
}

static u16int
tile(int n, int tx, int ty)
{
	int a;
	u16int ta;
	u16int t;

	a = reg[0x2107 + n];
	ta = ((a & ~3) << 9) + ((tx & 0x1f) << 1) + ((ty & 0x1f) << 6);
	if((a & 1) != 0)
		ta += (tx & 0x20) << 6;
	if((a & 2) != 0)
		ta += (ty & 0x20) << (6 + (a & 1));
	t = vram[ta++];
	return t | vram[ta] << 8;
}

static void
chr(int n, int nb, int sz, u16int t, int x, int y, u8int c[])
{
	int i;
	u16int a;

	if(sz == 16){
		if(y >= 8){
			t += ((x >> 3 ^ t >> 14) & 1) + ((~t >> 11) & 16);
			y -= 8;
		}else
			t += ((x >> 3 ^ t >> 14) & 1) + ((t >> 11) & 16);
	}
	if((t & 0x8000) != 0)
		y = 7 - y;
	a = reg[0x210b + (n >> 1)];
	if((n & 1) != 0)
		a >>= 4;
	else
		a &= 0xf;
	a = (a << 13) + (t & 0x3ff) * 8 * nb + y * 2;
	for(i = 0; i < nb; i += 2){
		c[i] = vram[a++];
		c[i+1] = vram[a];
		a += 15;
	}
}

static int
palette(int n, int p)
{
	switch(mode){
	case 0:
		return p << 2 | n << 5;
	case 1:
		if(n >= 2)
			return p << 2;
	case 2:
	case 6:
		return p << 4;
	case 5:
		if(n == 0)
			return p << 4;
		return p << 2;
	case 3:
		if(n != 0)
			return p << 4;
	case 4:
		if(n != 0)
			return p << 2;
		if((reg[CGWSEL] & DIRCOL) != 0)
			return 0x10000;
	}
	return 0;
}

static void
shift(u8int *c, int nb, int n, int d)
{
	u8int *e;
	
	e = c + nb;
	if(d)
		while(c < e)
			*c++ >>= n;
	else
		while(c < e)
			*c++ <<= n;
}

static u8int
bgpixel(u8int *c, int nb, int d)
{
	u8int v;
	int i;
	
	v = 0;
	if(d)
		for(i = 0; i < nb; i++){
			v |= (*c & 1) << i;
			*c++ >>= 1;
		}
	else
		for(i = 0; i < nb; i++){
			v |= (*c & 0x80) >> (7 - i);
			*c++ <<= 1;
		}
	return v;
}

static void
bg(int n, int nb, int prilo, int prihi)
{
	static struct bg {
		u8int sz, szsh;
		u16int tx, ty, tnx, tny;
		u16int t;
		u8int c[8];
		int pal;
		u8int msz, mv, mx;
	} bgs[4];
	struct bg *p;
	int v, sx, sy;

	p = bgs + n;
	if(rx == 0){
		p->szsh = (reg[BGMODE] & (1<<(4+n))) != 0 ? 4 : 3;
		if(mode >= 5)
			p->szsh = 4;
		p->sz = 1<<p->szsh;
		sx = hofs[n];
		sy = vofs[n] + ppuy;
		if(reg[MOSAIC] != 0 && (reg[MOSAIC] & (1<<n)) != 0){
			p->msz = (reg[MOSAIC] >> 4) + 1;
			if(p->msz != 1){
				sx -= p->mx = sx % p->msz;
				sy -= sy % p->msz;
			}
		}else
			p->msz = 1;
	redo:
		p->tx = sx >> p->szsh;
		p->tnx = sx & (p->sz - 1);
		p->ty = sy >> p->szsh;
		p->tny = sy & (p->sz - 1);
		p->t = tile(n, p->tx, p->ty);
		chr(n, nb, p->sz, p->t, p->tnx, p->tny, p->c);
		p->pal = palette(n, p->t >> 10 & 7);
		if(p->tnx != 0)
			shift(p->c, nb, p->tnx, p->t & 0x4000);
		if(p->msz != 1 && p->mx != 0 && sx % p->msz == 0){
			p->mv = bgpixel(p->c, nb, p->t & 0x4000);
			if(p->tnx + p->mx >= 8){
				sx += p->mx;
				goto redo;
			}else if(p->mx > 1)
				shift(p->c, nb, p->mx - 1, p->t & 0x4000);
		}
	}
	v = bgpixel(p->c, nb, p->t & 0x4000);
	if(p->msz != 1)
		if(p->mx++ == 0)
			p->mv = v;
		else{
			if(p->mx == p->msz)
				p->mx = 0;
			v = p->mv;
		}
	if(v != 0)
		pixel(n, p->pal + v, (p->t & 0x2000) != 0 ? prihi : prilo);
	if(++p->tnx == p->sz){
		p->tx++;
		p->tnx = 0;
		p->t = tile(n, p->tx, p->ty);
		p->pal = palette(n, p->t >> 10 & 7);
	}
	if((p->tnx & 7) == 0)
		chr(n, nb, p->sz, p->t, p->tnx, p->tny, p->c);
}

static void
bgs(void)
{
	static int bitch[8];

	switch(mode){
	case 0:
		bg(0, 2, 0x80, 0xb0);
		bg(1, 2, 0x71, 0xa1);
		bg(2, 2, 0x22, 0x52);
		bg(3, 2, 0x13, 0x43);
		break;
	case 1:
		bg(0, 4, 0x80, 0xb0);
		bg(1, 4, 0x71, 0xa1);
		bg(2, 2, 0x12, (reg[BGMODE] & 8) != 0 ? 0xd2 : 0x42);
		break;
	default:
		if(bitch[mode]++ == 0)
			print("bg mode %d not implemented\n", mode);
	}
}

static void
sprites(void)
{
	static struct {
		short x;
		u8int y, i, c, sx, sy;
		u16int t0, t1;
	} s[32], *sp;
	static struct {
		short x;
		u8int sx, i, c, pal, pri;
		u8int *ch;
	} t[32], *tp;
	static uchar ch[34*4], *cp;
	static uchar *p, q, over;
	static int n, m;
	static int *sz;
	static int szs[] = {
		8, 8, 16, 16, 8, 8, 32, 32,
		8, 8, 64, 64, 16, 16, 32, 32,
		16, 16, 64, 64, 32, 32, 64, 64,
		16, 32, 32, 64, 16, 32, 32, 32
	};
	static u16int base[2];
	u8int dy, v, col, pri0, pri1, prio;
	u16int a;
	int i, nt, dx;

	if(rx == 0){
		n = 0;
		over = 1;
		sp = s;
		sz = szs + ((reg[OBSEL] & 0xe0) >> 3);
		base[0] = (reg[OBSEL] & 0x07) << 14;
		base[1] = base[0] + (((reg[OBSEL] & 0x18) + 8) << 10);
	}
	if((rx & 1) == 0){
		p = oam + 2 * rx;
		if(p[1] == 0xf0)
			goto nope;
		q = (oam[512 + (rx >> 3)] >> (rx & 6)) & 3;
		dy = ppuy - p[1];
		sp->sx = sz[q & 2];
		sp->sy = sz[(q & 2) + 1];
		if(dy >= sp->sy)
			goto nope;
		sp->x = p[0];
		if((q & 1) != 0)
			sp->x |= 0xff00;
		if(sp->x < -(short)sp->sx && sp->x != -256)
			goto nope;
		if(n == 32){
			over |= 0x40;
			goto nope;
		}
		sp->i = rx >> 1;
		sp->y = p[1];
		sp->c = p[3];
		sp->t0 = p[2] << 5;
		sp->t1 = base[sp->c & 1];
		sp++;
		n++;
	}
nope:
	if(ppuy != 0){
		col = 0;
		pri0 = 0;
		pri1 = 128;
		if((reg[OAMADDH] & 0x80) != 0)
			prio = oamaddr >> 2;
		else
			prio = 0;
		for(i = 0, tp = t; i < m; i++, tp++){
			dx = rx - tp->x;
			if(dx < 0 || dx >= tp->sx)
				continue;
			p = tp->ch + (dx >> 1 & 0xfc);
			if((tp->c & 0x40) != 0){
				v = p[2] & 1 | p[3] << 1 & 2 | p[0] << 2 & 4 | p[1] << 3 & 8;
				p[0] >>= 1;
				p[1] >>= 1;
				p[2] >>= 1;
				p[3] >>= 1;
			}else{
				v = p[0] >> 7 & 1 | p[1] >> 6 & 2 | p[2] >> 5 & 4 | p[3] >> 4 & 8;
				p[0] <<= 1;
				p[1] <<= 1;
				p[2] <<= 1;
				p[3] <<= 1;
			}
			nt = (tp->i - prio) & 0x7f;
			if(v != 0 && nt < pri1){
				col = tp->pal + v;
				pri0 = tp->pri;
				pri1 = nt;
			}
		}
		if(col > 0)
			pixel(OBJ, col, pri0);
	}
	if(rx == 255){
		cp = ch;
		m = n;
		for(sp = s + n - 1, tp = t + n - 1; sp >= s; sp--, tp--){
			tp->x = sp->x;
			tp->sx = 0;
			tp->c = sp->c;
			tp->pal = 0x80 | sp->c << 3 & 0x70;
			tp->pri = 3 * (0x10 + (sp->c & 0x30));
			if((tp->c & 8) != 0)
				tp->pri |= OBJ;
			else
				tp->pri |= OBJNC;
			tp->ch = cp;
			tp->i = sp->i;
			nt = sp->sx >> 2;
			dy = ppuy - sp->y;
			if((sp->c & 0x80) != 0)
				dy = sp->sy - 1 - dy;
			a = sp->t0 | (dy & 7) << 1;
			if(dy >= 8)
				a += (dy & ~7) << 6;
			if((sp->c & 0x40) != 0){
				a += sp->sx * 4;
				for(i = 0; i < nt; i++){
					if(cp < ch + sizeof(ch)){
						a -= 16;
						*(u16int*)cp = *(u16int*)&vram[sp->t1 | a & 0x1fff];
						cp += 2;
						tp->sx += 4;
					}else
						over |= 0x80;
				}
			}else
				for(i = 0; i < nt; i++){
					if(cp < ch + sizeof(ch)){
						*(u16int*)cp = *(u16int*)&vram[sp->t1 | a & 0x1fff];
						cp += 2;
						tp->sx += 4;
						a += 16;
					}else
						over |= 0x80;
				}
		}
		reg[0x213e] = over;
	}
}

static u16int
colormath(void)
{
	u16int v, w, r, g, b;
	u8int m, m2;
	int cw;
	
	m = reg[CGWSEL];
	m2 = reg[CGADSUB];
	cw = -1;
	switch(m >> 6){
	default: v = 1; break;
	case 1: v = cw = window(COL); break;
	case 2: v = !(cw = window(COL)); break;
	case 3: v = 0; break;
	}
	if(v){
		if((pixelcol[0] & 0x10000) != 0)
			v = pixelcol[0];
		else
			v = cgram[pixelcol[0] & 0xff];
	}
	if((m2 & (1 << (pixelpri[0] & 0xf))) == 0)
		return v;
	switch((m >> 4) & 3){
	case 0: break;
	case 1: if(cw < 0) cw = window(COL); if(!cw) return v; break;
	case 2: if(cw < 0) cw = window(COL); if(cw) return v; break;
	default: return v;
	}
	if((m & 2) != 0){
		if((pixelcol[1] & 0x10000) != 0)
			w = pixelcol[1];
		else
			w = cgram[pixelcol[1] & 0xff];
	}else
		w = subcolor;
	if((m2 & 0x80) != 0){
		r = (v & 0x7c00) - (w & 0x7c00);
		g = (v & 0x03e0) - (w & 0x03e0);
		b = (v & 0x001f) - (w & 0x001f);
		if((m2 & 0x40) != 0){
			r = (r >> 1) & 0xfc00;
			g = (g >> 1) & 0xffe0;
			b >>= 1;
		}
		if(r > 0x7c00) r = 0;
		if(g > 0x03e0) g = 0;
		if(b > 0x001f) b = 0;
		return r | g | b;
	}else{
		r = (v & 0x7c00) + (w & 0x7c00);
		g = (v & 0x03e0) + (w & 0x03e0);
		b = (v & 0x001f) + (w & 0x001f);
		if((m2 & 0x40) != 0){
			r = (r >> 1) & 0xfc00;
			g = (g >> 1) & 0xffe0;
			b >>= 1;
		}
		if(r > 0x7c00) r = 0x7c00;
		if(g > 0x03e0) g = 0x03e0;
		if(b > 0x001f) b = 0x001f;
		return r | g | b;
	}
}

void
ppustep(void)
{
	int yvbl;

	mode = reg[BGMODE] & 7;
	bright = reg[INIDISP] & 0xf;
	yvbl = (reg[SETINI] & OVERSCAN) != 0 ? 0xf0 : 0xe1;

	if(ppux >= XLEFT && ppux <= XRIGHT && ppuy < 0xf0){
		rx = ppux - XLEFT;
		if(ppuy < yvbl && (reg[INIDISP] & 0x80) == 0){
			pixelcol[0] = 0;
			pixelpri[0] = COL;
			pixelcol[1] = 0x10000 | subcolor;
			pixelpri[1] = COL;	
			bgs();
			sprites();
			if(ppuy != 0)
				pixeldraw(rx, ppuy - 1, colormath());
		}else if(ppuy != 0)
			pixeldraw(rx, ppuy - 1, ppuy >= yvbl ? 0x31c8 : 0);
	}

	if(ppux == 0x116 && ppuy <= yvbl)
		hdma |= reg[0x420c];
	if((reg[NMITIMEN] & HCNTIRQ) != 0 && htime == ppux && ((reg[NMITIMEN] & VCNTIRQ) == 0 || vtime == ppuy))
		irq |= IRQPPU;
	if(++ppux >= 340){
		ppux = 0;
		if(++ppuy >= 262){
			ppuy = 0;
			reg[RDNMI] &= ~VBLANK;
			hdma = reg[0x420c]<<8;
			flush();
		}
		if(ppuy == yvbl){
			reg[RDNMI] |= VBLANK;
			if((reg[NMITIMEN] & VBLANK) != 0)
				nmi = 2;
			if((reg[NMITIMEN] & AUTOJOY) != 0){
				reg[0x4218] = keys;
				reg[0x4219] = keys >> 8;
				keylatch = 0xffff;
			}
		}
		if((reg[NMITIMEN] & (HCNTIRQ|VCNTIRQ)) == VCNTIRQ && vtime == ppuy)
			irq |= IRQPPU;
	}
}