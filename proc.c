#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

int
rand(void)
{
  return ticks*123456+ticks;
}

struct {
  struct spinlock lock;
  struct proc proc[1000];
} ptable;

struct post {
  int sender_pid;
  int recv_pid;
  int occupied;
  char message[MSGSIZE];
};

struct{
  struct spinlock lock;
  struct post posts[NPROC];
}post_box;


static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;

  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");

  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->elapsed_time = 0;
  p->sched_policy = -1;
  p->deadline = 0;
  p->rate = 0;
  if(p->pid > 2){
    p->text_offset = PGSIZE;
  }
  else{
    p->text_offset = 0;
  }
  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;


  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();

  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n,0)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n,0)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}


// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.

int edf_counter = 0;

int rms_counter = 0;

void edf_scheduler(struct cpu *c)
{
  struct proc *p;
  acquire(&ptable.lock);
  struct proc *min_p = 0;
  int min_deadline = 10000;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state != RUNNABLE || p->sched_policy != 0)
      continue;

    if(p->deadline < min_deadline) {
      min_p = p;
      min_deadline = p->deadline;
    }
    else if(p->deadline == min_deadline && min_p->pid > p->pid){
      min_p = p;
    }
  }
  if(min_p) {
    c->proc = min_p;
    switchuvm(min_p);
    min_p->state = RUNNING;
    swtch(&(c->scheduler), min_p->context);
    switchkvm();
    min_p->elapsed_time++;
    c->proc = 0;
  }
  release(&ptable.lock);
}

int my_ceil(float x) {
    if (x >= 0) {
        int i = (int) x;
        if (i == x) {
            return i;
        } else {
            return i + 1;
        }
    } else {
        return (int) x;
    }
}

void rms_scheduler(struct cpu *c)
{
  struct proc *p;
  acquire(&ptable.lock);
  struct proc *min_p = 0;
  int min_weight = 10000;
  for(p = ptable.proc ; p< &ptable.proc[NPROC];p++){
    if(p->state != RUNNABLE || p->sched_policy != 1)
      continue;

    int weight = 0;
    float x = (float)((float)(30-p->rate)/(float)29)*3;
    if(my_ceil(x) > 1){
      weight = my_ceil(x);
    }
    else{
      weight = 1;
    }
    if(weight < min_weight) {
      min_p = p;
      min_weight = weight;
    }
    if(weight == min_weight && min_p->pid > p->pid){
      min_p = p;
    }
  }
  if(min_p) {
    c->proc = min_p;
    switchuvm(min_p);
    min_p->state = RUNNING;
    swtch(&(c->scheduler), min_p->context);
    switchkvm();
    min_p->elapsed_time++;
    c->proc = 0;
  }
  release(&ptable.lock);
}

void
scheduler(void)
{

  struct cpu *c = mycpu();

  c->proc = 0;

  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.

    if(edf_counter > 0){

      edf_scheduler(c);

    }

    else if(rms_counter > 0){

      rms_scheduler(c);

    }

    else{

      struct proc *p;

      acquire(&ptable.lock);

      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){

        if(p->state != RUNNABLE || p->sched_policy != -1)
          continue;

        c->proc = p;
        switchuvm(p);

        p->state = RUNNING;

        swtch(&(c->scheduler),p->context);
        switchkvm();

        c->proc = 0;
      }

      release(&ptable.lock);

    }

  }
}


// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}


//sys_ps

void sys_ps(void)
{
  struct proc *p;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid != 0){
      cprintf("%d %s \n", p->pid, p->name);
    }
  }
}


//sys_send


int sys_send(void){

  int s_pid;

  int r_pid;

  char* msg;

  if(argint(0,&s_pid) < 0 || argint(1,&r_pid) < 0 || argptr(2,&msg,MSGSIZE) < 0 ){

    return -1;
  }

  struct post *ptr;

  acquire(&post_box.lock);

  int found = 0;

  for(ptr = post_box.posts ; ptr < &post_box.posts[NPROC] ; ptr++){
    if(ptr->occupied == 0){
        memmove(ptr->message, msg, MSGSIZE);
        ptr->sender_pid = s_pid;
        ptr->recv_pid = r_pid;
        ptr->occupied = 1;
        found = 1;
        break;
    }
  }

  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid ==r_pid){
      p->state = RUNNING;
    }
  }

  if(found==0){
    release(&post_box.lock);
    return -1;
  }

  release(&post_box.lock);

  return 0;

}


//sys_recv

int sys_recv(void){

  struct proc *curproc = myproc();

  int self_pid = curproc->pid;

  char* msg;

  struct post * ptr;

  if(argptr(0,&msg,MSGSIZE) < 0 ){

    return -1;
  }

  int found = 0;

  acquire(&post_box.lock);

  for(ptr = post_box.posts ; ptr < &post_box.posts[NPROC] ; ptr++){
    if(ptr->occupied == 1 && ptr->recv_pid == self_pid){
        memmove(msg,ptr->message, MSGSIZE);
        ptr->occupied = 0;
        found = 1;
        break;
    }
  }

  if(found==0){
    release(&post_box.lock);
    return -1;
  }

  release(&post_box.lock);

  return 0;

}

//sys_send_multi

int sys_send_multi(void)
{
  int s_pid;

  char* r_pids;

  char* msg;

  if(argint(0,&s_pid) < 0 || argptr(1,&r_pids,MSGSIZE) < 0 || argptr(2,&msg,MSGSIZE) < 0 ){

    return -1;

  }

  int * r_pid = (int*)r_pids;

  while(*r_pid >0 && *r_pid <= NPROC){

    struct post *ptr;

    acquire(&post_box.lock);

    int found = 0;

    for(ptr = post_box.posts ; ptr < &post_box.posts[NPROC] ; ptr++){
      if(ptr->occupied == 0){
          ptr->sender_pid = s_pid;
          ptr->recv_pid = *r_pid;
          memmove(ptr->message, msg, MSGSIZE);
          ptr->occupied = 1;
          found = 1;
          break;
      }
    }

    if(found==0){
      release(&post_box.lock);
      return -1;
    }

    release(&post_box.lock);

    r_pid++;

  }
  return 0;

}

double nthRoot(int n) {
    double x = 2;
    double delta = 0.00001;
    int i;

    for (i = 0; i < 1000; i++) {
        double f = 1;
        int j;
        for (j = 0; j < n; j++) {
            f *= x;
        }
        double fx = f - 2;
        double fdx = n * f / x;
        double dx = fx / fdx;
        x -= dx;
        if (dx < delta) break;
    }
    return x;
}


// sys_sched_policy


float U=0;

float U1=0;

int n=0;

int sys_sched_policy(void)
{
  int pid;
  int policy;
  if(argint(0,&pid) < 0 || argint(1,&policy) < 0){
      return -22;
  }
  struct proc * p;
  acquire(&ptable.lock);
  for(p=ptable.proc;p< &ptable.proc[NPROC];p++){
    if(p->pid == pid){
      p->sched_policy = policy;
      p->arrival_time = ticks;
      p->deadline = ticks + p->deadline;
      if(policy==0){
        float temp=((float)(p->exec_time)/(float)(p->deadline-p->arrival_time));
        if(U + temp > 1){
          release(&ptable.lock);
          kill(p->pid);
          return -22;
        }
        U+=temp;
        edf_counter++;
      }
      if(policy==1){
        float temp=((float)(p->exec_time))/((float)100/(float)(p->rate));
        if(U1 + temp > n*(nthRoot(n)-1)){
          release(&ptable.lock);
          kill(p->pid);
          return -22;
        }
        U1+=temp;
        n++;
        rms_counter++;
      }
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -22;
}

//sys_exec_time

int sys_exec_time(void)
{
  int pid;

  int exec_time;

  if(argint(0,&pid) < 0 || argint(1,&exec_time) < 0){
      return -22;
  }

  struct proc* p;

  acquire(&ptable.lock);

  for(p = ptable.proc;p<&ptable.proc[NPROC];p++){
    if(p->pid == pid && p->exec_time == 0){
      p->exec_time = exec_time;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -22;

}

//sys_deadline

int sys_deadline(void)
{
  int pid;

  int deadline;

  if(argint(0,&pid) < 0 || argint(1,&deadline) < 0){
      return -22;
  }

  struct proc* p;

  acquire(&ptable.lock);

  for(p = ptable.proc;p<&ptable.proc[NPROC];p++){
    if(p->pid == pid && p->deadline == 0){
      p->deadline = deadline;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -22;

}

int sys_rate(void)
{
  int pid;

  int rate;

  if(argint(0,&pid) < 0 || argint(1,&rate) < 0){
      return -22;
  }

  struct proc* p;

  acquire(&ptable.lock);

  for(p = ptable.proc;p<&ptable.proc[NPROC];p++){
    if(p->pid == pid && p->rate == 0){
      p->rate = rate;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -22;
}
