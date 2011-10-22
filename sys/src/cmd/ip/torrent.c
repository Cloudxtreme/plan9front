#include <u.h>
#include <libc.h>
#include <mp.h>
#include <libsec.h>

typedef struct Dict Dict;
typedef struct Piece Piece;
typedef struct File File;

struct Dict
{
	char	typ;	// i, d, s, l
	Dict	*val;
	Dict	*next;
	char	*start, *end;
	int	len;
	char	str[];
};

struct Piece
{
	uchar	*hash;
	int	len;
	int	brk;
};

struct File
{
	File	*next;
	char	*name;
	int	fd;
	vlong	off;
	vlong	len;
};

enum {
	MAXIO = 16*1024,
};

int debug, sflag, pflag, vflag;
int pidgroup = -1;
int port = 48123;
char *mntweb = "/mnt/web";
uchar infohash[20];
uchar peerid[20];
int blocksize;

int npieces;
Piece *pieces;

int nhavemap;
uchar *havemap;

File *files;

void
freedict(Dict *d)
{
	if(d){
		if(d->val != d)
			freedict(d->val);
		freedict(d->next);
		free(d);
	}
}

char*
bparse(char *s, char *e, Dict **dp)
{
	char *x, t;
	Dict *d;
	int n;

	*dp = nil;
	if(s >= e)
		return e;

	t = *s;
	switch(t){
	case 'd':
	case 'l':
		x = s++;
		d = nil;
		while(s < e){
			if(*s == 'e'){
				s++;
				break;
			}
			if(t == 'd'){
				s = bparse(s, e, dp);
				if((d = *dp) == nil)
					break;
			} else
				d = *dp = mallocz(sizeof(*d), 1);
			d->typ = t;
			d->start = x;
			if(s < e){
				s = bparse(s, e, &d->val);
				dp = &d->next;
				d->end = s;
			}
			x = s;
		}
		if(d)
			d->end = s;
		return s;
	case 'i':
		x = ++s;
		if((s = memchr(x, 'e', e - x)) == nil)
			return e;
		n = s - x;
		s++;
		break;
	default:
		if((x = memchr(s, ':', e - s)) == nil)
			return e;
		x++;
		if((n = atoi(s)) < 0)
			return e;
		s = x + n;
		if((s > e) || (s < x)){
			n = e - x;
			s = e;
		}
		t = 's';
	}
	d = mallocz(sizeof(*d) + n+1, 1);
	d->typ = t;
	memmove(d->str, x, d->len = n);
	d->str[n] = 0;
	*dp = d;
	return s;
}

char*
dstr(Dict *d)
{
	if(d && (d->typ == 's' || d->typ == 'i'))
		return d->str;
	return nil;
}

Dict*
dlook(Dict *d, char *s)
{
	for(; d && d->typ == 'd'; d = d->next)
		if(d->len && strcmp(d->str, s) == 0)
			return d->val;
	return nil;
}

int
readall(int fd, char **p)
{
	int n, r;

	n = 0;
	*p = nil;
	while(*p = realloc(*p, n+1024)){
		if((r = read(fd, *p+n, 1024)) <= 0)
			break;
		n += r;
	}
	return n;
}

int
rwpiece(int wr, int index, uchar *data, int len, int poff)
{
	vlong off;
	int n, m;
	File *f;

	if(len <= 0 || poff >= pieces[index].len)
		return 0;
	if(len+poff > pieces[index].len)
		len = pieces[index].len - poff;
	off = (vlong)index * blocksize;
	off += poff;
	for(f = files; f; f = f->next)
		if((f->off+f->len) > off)
			break;
	off -= f->off;
	n = ((off + len) > f->len) ? f->len - off : len;
	if((n = (wr ? pwrite(f->fd, data, n, off) : pread(f->fd, data, n, off))) <= 0)
		return -1;
	if((m = rwpiece(wr, index, data + n, len - n, poff + n)) < 0)
		return -1;
	return n+m;
}

int
havepiece(int x)
{
	uchar *p, m, hash[20];
	int n;

	m = 0x80>>(x&7);
	if(havemap[x>>3] & m)
		return 1;
	p = malloc(blocksize);
	n = pieces[x].len;
	if(rwpiece(0, x, p, n, 0) != n){
		free(p);
		return 0;
	}
	sha1(p, n, hash, nil);
	free(p);
	if(memcmp(hash, pieces[x].hash, 20))
		return 0;
	havemap[x>>3] |= m;
	return 1;
}

int
pickpiece(uchar *map)
{
	int i, x, r, k;
	uchar m;

	r = -1;
	k = 0;
	for(i = 0; i<nhavemap; i++){
		if(map[i] == 0)
			continue;
		for(x = i<<3, m = 0x80; m; m >>= 1, x++){
			if((~map[i] | havemap[i]) & m)
				continue;
			if(nrand(++k) == 0)
				r = x;
		}
	}
	return r;
}

int
unpack(uchar *s, int n, char *fmt, ...)
{
	va_list arg;
	uchar *b, *e;

	b = s;
	e = b + n;
	va_start(arg, fmt);
	for(; *fmt; fmt++) {
		switch(*fmt){
		case '_':
			s++;
			break;
		case 'b':
			if(s+1 > e) goto Err;
			*va_arg(arg, int*) = *s++;
			break;
		case 'l':
			if(s+4 > e) goto Err;
			*va_arg(arg, int*) = s[0]<<24 | s[1]<<16 | s[2]<<8 | s[3];
			s += 4;
			break;
		}
	}
	va_end(arg);
	return s - b;
Err:
	va_end(arg);
	return -1;
}

int
pack(uchar *s, int n, char *fmt, ...)
{
	va_list arg;
	uchar *b, *e;
	int i;

	b = s;
	e = b + n;
	va_start(arg, fmt);
	for(; *fmt; fmt++) {
		switch(*fmt){
		case '_':
			i = 0;
			if(0){
		case 'b':
			i = va_arg(arg, int);
			}
			if(s+1 > e) goto Err;
			*s++ = i & 0xFF;
			break;
		case 'l':
			i = va_arg(arg, int);
			if(s+4 > e) goto Err;
			*s++ = (i>>24) & 0xFF;
			*s++ = (i>>16) & 0xFF;
			*s++ = (i>>8) & 0xFF;
			*s++ = i & 0xFF;
			break;
		case '*':
			i = va_arg(arg, int);
			if(s+i > e) goto Err;
			memmove(s, va_arg(arg, uchar*), i);
			s += i;
			break;
		}
	}
	va_end(arg);
	return s - b;
Err:
	va_end(arg);
	return -1;
}

void
peer(char *ip, char *port)
{
	static Dict *peers;
	static QLock peerslk;

	uchar buf[64+MAXIO], *map, *told, *p, m;
	char *addr;
	int retry, i, o, l, x, n, fd;
	int mechoking, hechoking;
	int mewant, hewant;
	int workpiece;
	Dict *d;

	if(ip == nil || port == nil)
		return;

	d = mallocz(sizeof(*d) + 64, 1);
	snprint(addr = d->str, 64, "tcp!%s!%s", ip, port);
	qlock(&peerslk);
	if(dlook(peers, addr)){
		qunlock(&peerslk);
		free(d);
		return;
	}
	d->len = strlen(addr);
	d->typ = 'd';
	d->val = d;
	d->next = peers;
	peers = d;
	qunlock(&peerslk);

	if(rfork(RFFDG|RFPROC|RFMEM) <= 0)
		return;

	fd = -1;
	retry = 0;
	map = malloc(nhavemap);
	told = malloc(nhavemap);
Retry:
	if(fd >= 0){
		close(fd);
		sleep(10000 + nrand(5000));
	}
	if(++retry >= 10)
		goto Exit;

	if(debug) fprint(2, "dial %s\n", addr);
	if((fd = dial(addr, nil, nil, nil)) < 0)
		goto Retry;

	if(debug) fprint(2, "peer %s: -> handshake\n", addr);
	n = pack(buf, sizeof(buf), "*________**", 
		20, "\x13BitTorrent protocol",
		sizeof(infohash), infohash,
		sizeof(peerid), peerid);
	if(write(fd, buf, n) != n)
		goto Retry;

	if(read(fd, buf, 1) != 1)
		goto Retry;
	n = buf[0] + 8 + sizeof(infohash) + sizeof(peerid);
	if((n = readn(fd, buf+1, n)) != n)
		goto Retry;
	if(debug) fprint(2, "peer %s: <- handshake %.*s\n", addr, buf[0], (char*)buf+1);
	if(memcmp(infohash, buf + 1 + buf[0] + 8, sizeof(infohash)))
		goto Exit;

	if(debug) fprint(2, "peer %s: -> bitfield %d\n", addr, nhavemap);
	memmove(told, havemap, nhavemap);
	n = pack(buf, sizeof(buf), "lb*", nhavemap+1, 0x05, nhavemap, havemap);
	if(write(fd, buf, n) != n)
		goto Retry;

	mechoking = 1;
	hechoking = 1;
	mewant = 0;
	hewant = 0;
	workpiece = -1;
	memset(map, 0, nhavemap);
	for(;;){
		for(i=0; i<nhavemap; i++){
			if(told[i] != havemap[i]){
				for(x = i<<3, m = 0x80; m; m >>= 1, x++){
					if((~havemap[i] | told[i] | map[i]) & m)
						continue;
					told[i] |= m;
					if(debug) fprint(2, "peer %s: -> have %d\n", addr, x);
					n = pack(buf, sizeof(buf), "lbl", 1+4, 0x04, x);
					if(write(fd, buf, n) != n)
						goto Retry;
				}
			}
			if(!mewant && (map[i] & ~havemap[i])){
				mewant = 1;
				if(debug) fprint(2, "peer %s: -> interested\n", addr);
				n = pack(buf, sizeof(buf), "lb", 1, 0x02);
				if(write(fd, buf, n) != n)
					goto Retry;
			}
		}
		if(!hechoking && mewant){
			x = workpiece;
			if(x >= 0 && pieces[x].brk < pieces[x].len)
				{}
			else x = pickpiece(map);
			if(x >= 0){
				o = pieces[x].brk;
				l = pieces[x].len - o;
				if(l > MAXIO)
					l = MAXIO;
				if(debug) fprint(2, "peer %s: -> request %d %d %d\n", addr, x, o, l);
				n = pack(buf, sizeof(buf), "lblll", 1+4+4+4, 0x06, x, o, l);
				if(write(fd, buf, n) != n)
					goto Retry;
				workpiece = x;
			}
		}
		if(mechoking && hewant){
			mechoking = 0;
			if(debug) fprint(2, "peer %s: -> unchoke\n", addr);
			n = pack(buf, sizeof(buf), "lb", 1, 0x01);
			if(write(fd, buf, n) != n)
				goto Retry;
		}

		if(readn(fd, buf, 4) != 4)
			goto Retry;
		unpack(buf, 4, "l", &n);
		if(n == 0)
			continue;
		if(n < 0 || n > sizeof(buf))
			goto Retry;
		if(readn(fd, buf, n) != n)
			goto Retry;
		retry = 0;
		p = buf+1;
		n--;
		switch(*buf){
		case 0x00:	// Choke
			hechoking = 1;
			workpiece = -1;
			if(debug) fprint(2, "peer %s: <- choke\n", addr);
			break;
		case 0x01:	// Unchoke
			hechoking = 0;
			if(debug) fprint(2, "peer %s: <- unchoke\n", addr);
			break;
		case 0x02:	// Interested
			hewant = 1;
			if(debug) fprint(2, "peer %s: <- interested\n", addr);
			break;
		case 0x03:	// Notinterested
			hewant = 0;
			if(debug) fprint(2, "peer %s: <- notinterested\n", addr);
			break;
		case 0x04:	// Have <piceindex>
			if(unpack(p, n, "l", &x) < 0)
				goto Retry;
			if(debug) fprint(2, "peer %s: <- have %d\n", addr, x);
			if(x < 0 || x >= npieces)
				continue;
			map[x>>3] |= 0x80>>(x&7);
			break;
		case 0x05:	// Bitfield
			if(debug) fprint(2, "peer %s: <- bitfield %d\n", addr, n);
			if(n != nhavemap)
				continue;
			memmove(map, p, n);
			break;
		case 0x06:	// Request <index> <begin> <length>
			if(unpack(p, n, "lll", &x, &o, &l) < 0)
				goto Retry;
			if(debug) fprint(2, "peer %s: <- request %d %d %d\n", addr, x, o, l);
			if(x < 0 || x >= npieces)
				continue;
			if(!hewant || mechoking || (~havemap[x>>3]&(0x80>>(x&7))))
				continue;
			if(debug) fprint(2, "peer %s: -> piece %d %d\n", addr, x, o);
			n = 4+1+4+4;
			if(l > MAXIO)
				l = MAXIO;
			if((l = rwpiece(0, x, buf + n, l, o)) <= 0)
				continue;
			n = pack(buf, sizeof(buf), "lbll", 1+4+4+l, 0x07, x, o);
			n += l;
			if(write(fd, buf, n) != n)
				goto Retry;
			break;
		case 0x07:	// Piece <index> <begin> <block>
			if(unpack(p, n, "ll", &x, &o) != 8)
				goto Retry;
			p += 8;
			n -= 8;
			if(debug) fprint(2, "peer %s: <- piece %d %d %d\n", addr, x, o, n);
			if(x < 0 || x >= npieces)
				continue;
			if((pieces[x].brk != o) || (havemap[x>>3]&(0x80>>(x&7))))
				continue;
			if(rwpiece(1, x, p, n, o) == n){
				if((pieces[x].brk = o+n) == pieces[x].len){
					if(!havepiece(x))
						pieces[x].brk = 0;
				}
			}
			break;
		case 0x08:	// Cancel <index> <begin> <length>
			if(unpack(p, n, "lll", &x, &o, &l) < 0)
				goto Retry;
			if(debug) fprint(2, "peer %s: <- cancel %d %d %d\n", addr, x, o, l);
			break;
		case 0x09:	// Port <port>
			if(unpack(p, n, "l", &x) < 0)
				goto Retry;
			if(debug) fprint(2, "peer %s: <- port %d\n", addr, x);
			break;
		}
	}
Exit:
	free(told);
	free(map);
	exits(0);
}

int
hopen(char *url, ...)
{
	int conn, ctlfd, fd, n;
	char buf[1024+1];
	va_list arg;

	snprint(buf, sizeof buf, "%s/clone", mntweb);
	if((ctlfd = open(buf, ORDWR)) < 0)
		return -1;
	if((n = read(ctlfd, buf, sizeof buf-1)) <= 0){
		close(ctlfd);
		return -1;
	}
	buf[n] = 0;
	conn = atoi(buf);
	va_start(arg, url);
	strcpy(buf, "url ");
	n = 4+vsnprint(buf+4, sizeof(buf)-4, url, arg);
	va_end(arg);
	if(write(ctlfd, buf, n) != n){
	ErrOut:
		close(ctlfd);
		return -1;
	}
	snprint(buf, sizeof buf, "%s/%d/body", mntweb, conn);
	if((fd = open(buf, OREAD)) < 0)
		goto ErrOut;
	close(ctlfd);
	return fd;
}

void
tracker(char *url)
{
	static Dict *trackers;
	static QLock trackerslk;

	int n, fd;
	char *p;
	Dict *d, *l;

	if(url == nil)
		return;

	qlock(&trackerslk);
	if(dlook(trackers, url)){
		qunlock(&trackerslk);
		return;
	}
	n = strlen(url);
	d = mallocz(sizeof(*d) + n+1, 1);
	strcpy(d->str, url);
	d->len = n;
	d->typ = 'd';
	d->val = d;
	d->next = trackers;
	trackers = d;
	url = d->str;
	qunlock(&trackerslk);

	if(rfork(RFFDG|RFPROC|RFMEM) <= 0)
		return;

	for(;;){
		d = nil;
		if((fd = hopen("%s?info_hash=%.*H&peer_id=%.*H&port=%d&compact=1",
			url, sizeof(infohash), infohash, sizeof(peerid), peerid, port)) >= 0){
			n = readall(fd, &p);
			close(fd);
			bparse(p, p+n, &d);
			free(p);
		}
		if(l = dlook(d, "peers")){
			if(l->typ == 's'){
				uchar *b, *e;

				b = (uchar*)l->str;
				e = b + l->len;
				for(; b+6 <= e; b += 6){
					char ip[16], port[6];

					snprint(ip, sizeof(ip), "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
					snprint(port, sizeof(port), "%d", b[4]<<8 | b[5]);
					peer(ip, port);
				}
			} else for(; l && l->typ == 'l'; l = l->next)
				peer(dstr(dlook(l->val, "ip")), dstr(dlook(l->val, "port")));
		}
		n = 0;
		if(p = dstr(dlook(d, "interval")))
			n = atoi(p);
		if(n < 10 | n > 60*60)
			n = 2*60;
		freedict(d);
		sleep(n * 1000 + nrand(5000));
	}
}

int
Hfmt(Fmt *f)
{
	uchar *s, *e;
	s = va_arg(f->args, uchar*);
	if(f->flags & FmtPrec)
		e = s + f->prec;
	else
		e = s + strlen((char*)s);
	for(; s < e; s++)
		if(fmtprint(f, ((*s >= '0' && *s <= '9') || 
			(*s >= 'a' && *s <= 'z') ||
			(*s >= 'A' && *s <= 'Z') || 
			strchr(".-_~", *s)) ? "%c" : "%%%.2x", *s) < 0)
			return -1;
	return 0;
}

int
progress(void)
{
	int i, c;
	uchar m;
	c = 0;
	for(i=0; i<nhavemap; i++)
		for(m = 0x80; m; m>>=1)
			if(havemap[i] & m)
				c++;
	if(pflag)
		print("%d %d\n", c, npieces);
	return c == npieces;
}

void
killcohort(void)
{
	int i;
	for(i=0;i!=3;i++){	/* It's a long way to the kitchen */
		postnote(PNGROUP, pidgroup, "kill");
		sleep(1);
	}
}

int
catchnote(void *, char *msg)
{
	exits(msg);
	return 0;
}

void
usage(void)
{
	fprint(2, "usage: %s [ -vsdp ] [ -m mtpt ] [ torrentfile ]\n", argv0);
	exits("usage");
}

void
main(int argc, char *argv[])
{
	Dict *info, *torrent, *d;
	File **fp, *f;
	char *p, *s, *e;
	int fd, i, n;
	vlong len;

	fmtinstall('H', Hfmt);

	ARGBEGIN {
	case 'm':
		mntweb = EARGF(usage());
		break;
	case 's':
		sflag = 1;
		break;
	case 'p':
		pflag = 1;
		break;
	case 'v':
		vflag = 1;
		break;
	case 'd':
		debug++;
		break;
	default:
		usage();
	} ARGEND;

	fd = 0;
	if(*argv)
		if((fd = open(*argv, OREAD)) < 0)
			sysfatal("open torrent: %r");
	if((n = readall(fd, &p)) <= 0)
		sysfatal("read torrent: %r");
	bparse(p, p+n, &torrent);
	if((d = info = dlook(torrent, "info")) == nil)
		sysfatal("no meta info in torrent");
	for(s = e = d->start; d && d->typ == 'd'; d = d->next)
		e = d->end;
	sha1((uchar*)s, e - s, (uchar*)infohash, nil);
	free(p);

	fp = &files;
	if(d = dlook(info, "files")){		
		for(; d && d->typ == 'l'; d = d->next){
			Dict *di;

			if((s = dstr(dlook(d->val, "length"))) == nil)
				continue;
			f = mallocz(sizeof(*f), 1);
			f->len = atoll(s);
			f->name = dstr(dlook(info, "name"));
			for(di = dlook(d->val, "path"); di && di->typ == 'l'; di = di->next)
				if(s = dstr(di->val))
					f->name = f->name ? smprint("%s/%s", f->name, s) : s;
			*fp = f;
			fp = &f->next;
		}
	} else if(s = dstr(dlook(info, "length"))){
		f = mallocz(sizeof(*f), 1);
		f->len = atoll(s);
		f->name = dstr(dlook(info, "name"));
		*fp = f;
	}
	len = 0;
	for(f = files; f; f = f->next){
		if(f->name == nil || f->len <= 0)
			sysfatal("bogus file entry in meta info");
		if(vflag) fprint(pflag ? 2 : 1, "%s\n", f->name);
		if((f->fd = open(f->name, ORDWR)) < 0)
			if((f->fd = create(f->name, ORDWR, 0666)) < 0)
				sysfatal("create: %r");
		f->off = len;
		len += f->len;
	}
	if(len <= 0)
		sysfatal("no files in torrent");

	if((s = dstr(dlook(info, "piece length"))) == nil)
		sysfatal("missing piece length in meta info");
	if((blocksize = atoi(s)) <= 0)
		sysfatal("bogus piece length in meta info");
	d = dlook(info, "pieces");
	if(d == nil || d->typ != 's' || d->len <= 0 || d->len % 20)
		sysfatal("bad or no pices in meta info");
	npieces = d->len / 20;
	pieces = mallocz(sizeof(Piece) * npieces, 1);
	nhavemap = (npieces+7) / 8;
	havemap = mallocz(nhavemap, 1);
	for(i = 0; i<npieces; i++){
		pieces[i].hash = (uchar*)d->str + i*20;
		if(len < blocksize)
			pieces[i].len = len;
		else
			pieces[i].len = blocksize;
		len -= pieces[i].len;
	}
	if(len)
		sysfatal("pieces do not match file length");

	for(i = 0; i<npieces; i++)
		havepiece(i);

	switch(i = rfork(RFPROC|RFMEM|RFNOTEG|RFNAMEG)){
	case -1:
		sysfatal("fork: %r");
	case 0:
		memmove(peerid, "-NF9001-", 8);
		genrandom(peerid+8, sizeof(peerid)-8);
		tracker(dstr(dlook(torrent, "announce")));
		for(d = dlook(torrent, "announce-list"); d && d->typ == 'l'; d = d->next)
			if(d->val && d->val->typ == 'l')
				tracker(dstr(d->val->val));
		break;
	default:
		pidgroup = i;
		atexit(killcohort);
		atnotify(catchnote, 1);
		while(!progress() || sflag)
			sleep(1000);
	}
	exits(0);
}
