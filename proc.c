#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;
int nextpid = 1;
int nexttid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

// NxNSoft
int lockedpid = -1;

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
#ifndef MLFQ
  struct thd *t;
#endif
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

#ifdef MLFQ
  p->qlevel = 0;
  p->priority = 3; // NxNSoft
  p->tquantum = 0;
#else
  t = MAINTHD(p);
  t->state = EMBRYO;
  t->tid = nexttid++;
#endif

  release(&ptable.lock);

#ifndef MLFQ
  if(!(t->kstack = kalloc())){
    p->state = UNUSED;
    t->state = UNUSED;
    return 0;
  }
  sp = t->kstack + KSTACKSIZE;

  sp -= sizeof *(t->tf);
  t->tf = (struct trapframe *)sp;
  sp -= 4;
  *(uint *)sp = (uint)trapret;

  sp -= sizeof *(t->context);
  t->context = (struct context *)sp;
  memset(t->context, 0, sizeof *(t->context));
  t->context->eip = (uint)forkret;

  p->tid = 0;
#else
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
#endif
  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
#if defined(MLFQ)
  struct proc *target;
#else
  struct proc *p;
  struct thd *target;
#endif
  extern char _binary_initcode_start[], _binary_initcode_size[];

#ifndef MLFQ
  p = allocproc();
  target = MAINTHD(p);
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
#else
  target = allocproc();
  initproc = target;
  if((target->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(target->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  target->sz = PGSIZE;
#endif
  
  memset(target->tf, 0, sizeof(*target->tf));
  target->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  target->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  target->tf->es = target->tf->ds;
  target->tf->ss = target->tf->ds;
  target->tf->eflags = FL_IF;
  target->tf->esp = PGSIZE;
  target->tf->eip = 0;  // beginning of initcode.S

#ifndef MLFQ
  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");
#else
  safestrcpy(target->name, "initcode", sizeof(target->name));
  target->cwd = namei("/");
#endif

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

#ifndef MLFQ
  p->state = RUNNABLE;
#endif
  target->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

#ifndef MLFQ
  acquire(&ptable.lock);
#endif

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);

#ifndef MLFQ
  release(&ptable.lock);
#endif
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

#if defined(MLFQ)
  if((np = allocproc()) == 0)
    return -1;

  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

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
#else
  struct thd *main_thd;

  if((np = allocproc()) == 0)
    return -1;
  
  main_thd = MAINTHD(np);
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(main_thd->kstack);
    main_thd->kstack = 0;
    np->state = UNUSED;
    main_thd->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *(main_thd->tf) = *(CURTHD(curproc)->tf);

  main_thd->tf->eax = 0;
  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;
  main_thd->state = RUNNABLE;
  release(&ptable.lock);
#endif
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
#ifndef MLFQ
  struct thd *t;
#endif
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

  wakeup1(curproc->parent);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  curproc->state = ZOMBIE;
#ifndef MLFQ
  for(t = MAINTHD(curproc); t != THDADDR(curproc, NTHREAD); t++){
    if(t->state != UNUSED)
      t->state = ZOMBIE;
  }
#endif
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
#ifndef MLFQ
  struct thd *t;
  int i;
#endif
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
#ifndef MLFQ
        for(i = 0; i < NTHREAD; i++){
          t = THDADDR(p, i);
          t->tid = 0;
          t->state = UNUSED;
          if(t->kstack) {
            kfree(t->kstack);
            t->kstack = 0;
          }
        }
#else
        kfree(p->kstack);
        p->kstack = 0;
        p->qlevel = 0;
        p->tquantum = 0;
        p->priority = 0;
#endif
        pid = p->pid;
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
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);

#ifdef MLFQ
    // NxNSoft
    if(lockedpid != -1){
      // scheduler lock process
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->pid == lockedpid){
          if(p->state == RUNNABLE){
            c->proc = p;
            switchuvm(p);
            p->state = RUNNING;
            swtch(&(c->scheduler), p->context);
            switchkvm();
            c->proc = 0;
          }
          break;
        }
      }
      release(&ptable.lock);
      continue;
    }

    struct proc *np = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;
      if(!np){
        np = p;
      }
      else if(np->qlevel != p->qlevel){
        if(np->qlevel > p->qlevel)
          np = p;
      }
      else if(p->qlevel < MAXQUEUELEVEL && np->tquantum != p->tquantum){
        // Round-Robin
        if(np->tquantum > p->tquantum)
          np = p;
      }
      else if(p->qlevel == MAXQUEUELEVEL && np->priority != p->priority){
        // Priority Scheduling
        if(np->priority > p->priority)
          np = p;
      }
      else if(np->pid > p->pid){
        // FCFS
        np = p;
      }
    }

    if(!np)
      priority_boosting();
    else{
      c->proc = np;
      switchuvm(np);
      np->state = RUNNING;
      swtch(&(c->scheduler), np->context);
      switchkvm();
      c->proc = 0;
    }
#else
    struct thd *t;
    int infinity;

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      infinity = 0;
      if(p->state != RUNNABLE)
        continue;
      for(t = CURTHD(p); ; t++){
        if(t == THDADDR(p, NTHREAD))
          t = MAINTHD(p);
        if(t->state == RUNNABLE){
          p->tid = t - p->thds;
          c->proc = p;
          switchuvm(p);
          t->state = RUNNING;
          swtch(&(c->scheduler), t->context);
          switchkvm();
          c->proc = 0;
        }
        if (infinity && t == CURTHD(p))
          break;
        infinity = 1;
      }
    }
#endif
    release(&ptable.lock);
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
#ifndef MLFQ
  struct thd *t = CURTHD(p);

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(t->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&(t->context), mycpu()->scheduler);
  mycpu()->intena = intena;
#else
  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&(p->context), mycpu()->scheduler);
  mycpu()->intena = intena;
#endif
}

// Give up the CPU for one scheduling round.
// process wanna give up current process.
void
yield(void)
{
  struct proc *p;

  acquire(&ptable.lock);  //DOC: yieldlock
  p = myproc();
  p->state = RUNNABLE;
#ifndef MLFQ
  CURTHD(p)->state = RUNNABLE;
#endif
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
#ifndef MLFQ
  struct thd *t = CURTHD(p);
  t->chan = chan;
  t->state = SLEEPING;
  sched();
  t->chan = 0;
#else
  p->chan = chan;
  p->state = SLEEPING;
  sched();
  p->chan = 0;
#endif

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
#ifndef MLFQ
  struct thd *t;
  for(p = ptable.proc; p != &ptable.proc[NPROC]; p++) {
    if(p->state != RUNNABLE)
      continue;
    for(t = MAINTHD(p); t != THDADDR(p, NTHREAD); t++)
      if(t->state == SLEEPING && t->chan == chan)
        t->state = RUNNABLE;
  }
#else
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
#endif
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
#ifndef MLFQ
      for(struct thd* t = MAINTHD(p); t != THDADDR(p, NTHREAD); t++)
        if(t->state == SLEEPING)
          t->state = RUNNABLE;
#else
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
#endif
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
#if defined(MLFQ)
      getcallerpcs((uint*)p->context->ebp+2, pc);
#endif
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int 
getLevel(void)
{
#ifndef MLFQ
  panic("Can not call getLevel.");
#else
  if (myproc())
    return myproc()->qlevel;
#endif
  return -1;
}

int
setPriority(int pid, int priority)
{
#ifndef MLFQ
  panic("Can not call setPriority.");
#else
  struct proc *parent, *p;

  if(priority < 0 || priority > 10)
    return -2;
  
  acquire(&ptable.lock);
  parent = myproc();
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if((p->pid == pid) && (p->parent) && (p->parent == parent)){
      p->priority = priority;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
#endif
  return -1;
}

// The ptable lock must be held.
void
priority_boosting(void)
{
#ifndef MLFQ
  panic("Can not call priority_boosting.");
#else
  struct proc *p;

  if(!holding(&ptable.lock))
    panic("priority_boosting ptable.lock");

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid > 0){
      p->qlevel = 0;
      p->tquantum = 0;
      // NxNSoft
      p->priority = 3;
    }
  }
#endif
}

// NxNSoft
void
schedulerLock(int password)
{
#ifndef MLFQ
  panic("Can not call schedulerLock.");
#else
  struct proc *curproc = myproc();

  if(curproc->state != RUNNING)
    panic("schedulerLock called on a wrong process.");

  if(password != SCHED_PASSWORD){
    cprintf("%d %d %d: Wrong password for schedulerLock.\n",
            curproc->pid, curproc->tquantum, curproc->qlevel);
    exit();
  }

  acquire(&ptable.lock);
  if(lockedpid == -1){
    acquire(&tickslock);
    ticks = 0;
    release(&tickslock);
    lockedpid = curproc->pid;
  }
  release(&ptable.lock);
#endif
}

// NxNSoft
void
schedulerUnlock(int password)
{
#ifndef MLFQ
  panic("Can not call schedulerUnlock.");
#else
  struct proc *curproc = myproc();

  if(curproc->state != RUNNING)
    panic("schedulerUnlock called on a wrong process.");

  if(password != SCHED_PASSWORD){
    cprintf("%d %d %d: Wrong password for schedulerUnlock.\n",
            curproc->pid, curproc->tquantum, curproc->qlevel);
    exit();
  }

  acquire(&ptable.lock);
  if(lockedpid == curproc->pid){
    lockedpid = -1;
    curproc->qlevel = 0;
    curproc->tquantum = 0;
    curproc->priority = 3;
  }
  release(&ptable.lock);
#endif
}


inline void
acquire_ptable_lock(void)
{
  acquire(&ptable.lock);
}

inline void
release_ptable_lock(void)
{
  release(&ptable.lock);
}

int 
thread_create(thread_t *thread, void *start_routine, void *arg)
{
#ifndef MLFQ
  uint sz, sp;
  int tidx;
  struct thd *t;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(tidx = 0; tidx < NTHREAD; tidx++)
    if((t = THDADDR(curproc, tidx))->state == UNUSED)
      goto found;
  release(&ptable.lock);

  return -1;

found:
  t->state = EMBRYO;
  t->tid = nexttid++;
  *thread = t->tid;

  if ((t->kstack = kalloc()) == 0)
    goto bad;
  sp = (uint)(t->kstack + KSTACKSIZE);

  sp -= sizeof *t->tf;
  t->tf = (struct trapframe *)sp;
  *t->tf = *(CURTHD(curproc)->tf);

  sp -= 4;
  *(uint *)sp = (uint)trapret;

  sp -= sizeof *t->context;
  t->context = (struct context *)sp;
  memset(t->context, 0, sizeof *t->context);
  t->context->eip = (uint)forkret;

  sz = PGROUNDUP(curproc->sz);
  if(!(sz = allocuvm(curproc->pgdir, sz, sz + PGSIZE)))
    goto bad;
  curproc->sz = sz;
  sp = sz;
  sp -= 4;
  *(uint *)sp = (uint)arg;
  sp -= 4;
  *(uint *)sp = 0xffffffff;

  t->tf->eip = (uint)start_routine;
  t->tf->esp = (uint)sp;
  t->state = RUNNABLE;
  release(&ptable.lock);

  return 0;

bad:
  t->kstack = 0;
  t->tid = 0;
  t->state = UNUSED;
  *thread = -1;

  release(&ptable.lock);
#else
  panic("Cannot call thread_create.");
#endif
  return -1;
}

void 
thread_exit(void *retval)
{
#ifndef MLFQ
  struct proc *curproc = myproc();
  struct thd *curthd = CURTHD(curproc);

  acquire(&ptable.lock);
  wakeup1((void *)curthd->tid);
  curthd->retval = retval;
  curthd->state = ZOMBIE;
  sched();
  panic("zombie exit");
#else
  panic("Cannot call thread_exit.");
#endif
}

int 
thread_join(thread_t thread, void **retval)
{
#ifndef MLFQ
  struct proc *p;
  struct thd *t;

  acquire(&ptable.lock);
  for(p = ptable.proc; p != &ptable.proc[NPROC]; p++) {
    if(p->state != RUNNABLE)
      continue;
    for(t = MAINTHD(p); t != THDADDR(p, NTHREAD); t++)
      if(t->state != UNUSED && t->tid == thread)
        goto found;
  }
  release(&ptable.lock);
  return -1;

found:
  if(t->state != ZOMBIE){
    sleep((void *)thread, &ptable.lock);
  }

  if (retval != 0)
    *retval = t->retval;

  kfree(t->kstack);
  t->kstack = 0;
  t->retval = 0;
  t->tid = 0;
  t->state = UNUSED;

  release(&ptable.lock);

  return 0;
#else
  panic("Cannot call thread_join.");
  return -1;
#endif
}