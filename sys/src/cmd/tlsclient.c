#include <u.h>
#include <libc.h>
#include <mp.h>
#include <libsec.h>
#include <auth.h>

int debug, auth;
char *keyspec = "";
char *servername, *file, *filex, *ccert;

void
usage(void)
{
	fprint(2, "usage: tlsclient [-D] [-a [-k keyspec] ] [-c lib/tls/clientcert] [-t /sys/lib/tls/xxx] [-x /sys/lib/tls/xxx.exclude] [-n servername] dialstring [cmd [args...]]\n");
	exits("usage");
}

void
xfer(int from, int to)
{
	char buf[12*1024];
	int n;

	while((n = read(from, buf, sizeof buf)) > 0)
		if(write(to, buf, n) < 0)
			break;
}

static int
reporter(char *fmt, ...)
{
	va_list ap;
	
	va_start(ap, fmt);
	fprint(2, "%s:  tls reports ", argv0);
	vfprint(2, fmt, ap);
	fprint(2, "\n");

	va_end(ap);
	return 0;
}

void
main(int argc, char **argv)
{
	int fd;
	char *addr;
	TLSconn *conn;
	Thumbprint *thumb;

	fmtinstall('H', encodefmt);

	ARGBEGIN{
	case 'D':
		debug++;
		break;
	case 'a':
		auth++;
		break;
	case 'k':
		keyspec = EARGF(usage());
		break;
	case 't':
		file = EARGF(usage());
		break;
	case 'x':
		filex = EARGF(usage());
		break;
	case 'c':
		ccert = EARGF(usage());
		break;
	case 'n':
		servername = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND

	if(argc < 1)
		usage();

	if(filex && !file)	
		sysfatal("specifying -x without -t is useless");

	if(file){
		thumb = initThumbprints(file, filex);
		if(thumb == nil)
			sysfatal("initThumbprints: %r");
	} else
		thumb = nil;

	addr = *argv++;
	if((fd = dial(addr, 0, 0, 0)) < 0)
		sysfatal("dial %s: %r", addr);

	conn = (TLSconn*)mallocz(sizeof *conn, 1);
	conn->serverName = servername;
	if(ccert){
		conn->cert = readcert(ccert, &conn->certlen);
		if(conn->cert == nil)
			sysfatal("readcert: %r");
	}

	if(auth){
		AuthInfo *ai;

		ai = auth_proxy(fd, auth_getkey, "proto=p9any role=client %s", keyspec);
		if(ai == nil)
			sysfatal("auth_proxy: %r");

		conn->pskID = "p9secret";
		conn->psk = ai->secret;
		conn->psklen = ai->nsecret;
	}

	if(debug)
		conn->trace = reporter;

	fd = tlsClient(fd, conn);
	if(fd < 0)
		sysfatal("tlsclient: %r");

	if(thumb){
		uchar digest[20];

		if(conn->cert==nil || conn->certlen<=0)
			sysfatal("server did not provide TLS certificate");
		sha1(conn->cert, conn->certlen, digest, nil);
		if(!okThumbprint(digest, thumb))
			sysfatal("server certificate %.*H not recognized", SHA1dlen, digest);
	}

	if(*argv){
		dup(fd, 0);
		dup(fd, 1);
		if(fd > 1)
			close(fd);
		exec(*argv, argv);
		sysfatal("exec: %r");
	}

	rfork(RFNOTEG);
	switch(fork()){
	case -1:
		sysfatal("fork: %r");
	case 0:
		xfer(0, fd);
		break;
	default:
		xfer(fd, 1);
		break;
	}
	postnote(PNGROUP, getpid(), "die yankee pig dog");
	exits(0);
}
