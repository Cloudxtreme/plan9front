u8int memread(u16int);
void memwrite(u16int, u8int);
void step(void);
void vicstep(void);
void flush(void);
u8int vmemread(u16int);
void io(void);
void memreset(void);
void vicreset(void);
void bordset(void);
void progress(int, int);