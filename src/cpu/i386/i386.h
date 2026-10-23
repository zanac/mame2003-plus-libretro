/***************************************************************************

  Intel 386 emulator (Ville Linde), from MAME 0.139

***************************************************************************/

#ifndef I386INTF_H
#define I386INTF_H

#include "cpuintrf.h"

/* the A20 gate, as an input line */
#define I386_INPUT_LINE_A20		1

extern int i386_ICount;

void i386_init(void);
void i386_reset(void *param);
void i386_exit(void);
int i386_execute(int cycles);
unsigned i386_get_context(void *dst);
void i386_set_context(void *src);
unsigned i386_get_reg(int regnum);
void i386_set_reg(int regnum, unsigned val);
void i386_set_irq_line(int irqline, int state);
void i386_set_irq_callback(int (*callback)(int irqline));
const char *i386_info(void *context, int regnum);
unsigned i386_dasm(char *buffer, unsigned pc);

#endif
