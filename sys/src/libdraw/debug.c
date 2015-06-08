#include <u.h>
#include <libc.h>
#include <draw.h>

void
drawsetdebug(int v)
{
	uchar *a;
	a = bufimage(display, 1+1);
	if(a == nil){
		fprint(2, "drawsetdebug: %r\n");
		return;
	}
	a[0] = 'D';
	a[1] = v;
}
