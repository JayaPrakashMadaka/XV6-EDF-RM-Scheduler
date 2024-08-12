#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}


int strcmp(char *str1, char *str2) {
    int i = 0;
    
    while (str1[i] == str2[i]) {
        if (str1[i] == '\0' || str2[i] == '\0') {
            break;
        }
        i++;
    }
    
    if (str1[i] > str2[i]) {
        return 1;
    } else if (str1[i] < str2[i]) {
        return -1;
    } else {
        return 0;
    }
}

int trace_on = 0;

int sys_counter[29];

int sys_toggle(void)
{
  if(trace_on ==1 ){
    trace_on = 0;
  }
  else{
    trace_on = 1;
  }
  return 0;
}


int sys_print_count(void)
{
  char *syscall_names[] = {
    "sys_fork",
    "sys_exit",
    "sys_wait",
    "sys_pipe",
    "sys_read",
    "sys_kill",
    "sys_exec",
    "sys_fstat",
    "sys_chdir",
    "sys_dup",
    "sys_getpid",
    "sys_sbrk",
    "sys_sleep",
    "sys_uptime",
    "sys_open",
    "sys_write",
    "sys_mknod",
    "sys_unlink",
    "sys_link",
    "sys_mkdir",
    "sys_close",
    "sys_toggle",
    "sys_print",
    "sys_add",
    "sys_ps",
    "sys_send",
    "sys_recv",
    "sys_send_multi"
};

  int values[29];
  for(int i=0;i<29;i++){
    values[i]=sys_counter[i];
  }

  int n = 29;

  int syscall_indexes[] = {
    24, // sys_add
    9,  // sys_chdir
    21, // sys_close
    10, // sys_dup
    7,  // sys_exec
    2,  // sys_exit
    1,  // sys_fork
    8,  // sys_fstat
    11, // sys_getpid
    6,  // sys_kill
    19, // sys_link
    20, // sys_mkdir
    17, // sys_mknod
    15, // sys_open
    4,  // sys_pipe
    23, // sys_print
    25, // sys_ps
    5,  // sys_read
    27, // sys_recv
    26, // sys_send
    28, // sys_send_multi
    13, // sys_sleep
    12, // sys_sbrk
    22, // sys_toggle
    18, // sys_unlink
    14, // sys_uptime
    3,  // sys_wait
    16  // sys_write
};

  for(int i = 0; i < n; i++) {
    if(i != 28 && i != 23 && values[syscall_indexes[i]] !=0){
    cprintf("%s %d\n", syscall_names[i], values[syscall_indexes[i]]);
  }
  }


  return 0;
}


int sys_add(void)
{
  int a,b;

  if(argint(0,&a) < 0 || argint(1,&b) < 0){
    return -1;
  }

  int x = a+b;
  return x;
}



