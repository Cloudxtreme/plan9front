void *emalloc(u32int);
void *emallocz(u32int);
void *erealloc(void *, u32int);
void initproc(void);
int loadtext(char *, int, char **);
Segment *newseg(u32int, u32int, int);
void *vaddr(u32int, u32int, Segment **);
void *vaddrnol(u32int, u32int);
void step(void);
void syscall(void);
void cherrstr(char *, ...);
u32int noteerr(u32int, u32int);
void freesegs(void);
Fd *newfd(void);
Fd *copyfd(Fd *);
void fddecref(Fd *);
int iscexec(Fd *, int);
void setcexec(Fd *, int, int);
void cleanup(void);
void segunlock(Segment *);
void *copyifnec(u32int, int, int *);
void *bufifnec(u32int, int, int *);
void copyback(u32int, int, void *);
void initfs(char *, char *);
void suicide(char *, ...);
void fdclear(Fd *);
void addproc(Process *);
void remproc(Process *);
Process *findproc(int);
