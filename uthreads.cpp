//includes
#include "uthreads.h"
#include <queue>
#include <iostream>
#include <stdio.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <csetjmp>

#include <sys/time.h>
#include <stdbool.h>
#include <set>
#include <cstdint>


#ifdef __x86_64__
/* code for 64 bit Intel arch */

typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
  address_t ret;
  asm volatile("xor    %%fs:0x30,%0\n"
      "rol    $0x11,%0\n"
               : "=g" (ret)
               : "0" (addr));
  return ret;
}

#else
/* code for 32 bit Intel arch */

typedef unsigned int address_t;
#define JB_SP 4
#define JB_PC 5


/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
  address_t ret;
  asm volatile("xor    %%gs:0x18,%0\n"
               "rol    $0x9,%0\n"
  : "=g" (ret)
  : "0" (addr));
  return ret;
}
#endif




//defines
#define MICROSEC 1000000


//typdef
typedef enum {RUNNING, BLOCKED, READY, TERMINATE, SLEEPING} state;
typedef unsigned long address_t; // think to remove- Neta
typedef struct {
    int tid;
    int thread_quatum_counter;
    sigjmp_buf env;
    char* stack;
    state current_state;
    int sleeping_time = -1;
    bool blocked = false;
}thread;


//global variables
int total_quantum_counter = 0;
thread* all_threads[MAX_THREAD_NUM];
std::set<int> given_tids;
std::queue<thread*> ready_q;
std::queue<thread*> blocked_q;
thread* running_thread = nullptr;
sigset_t signal_set;
int quantum_sec;
thread* thread_to_terminate = nullptr;
bool flag = false;

//functions signatures
int get_tid();
// address_t translate_address(address_t addr);
int sigvtalrm_action();
void free_all_threads();
void switch_context(int sig = READY);
void erase_from_queue(state state_1, int tid);
void mask_signals();
void unmask_signals();
void free_all_threads_no_mask();
void reset_timer();


//API functions


int uthread_init(int quantum_usecs){
  //validation checks
  if(quantum_usecs <= 0){
    return -1;
  }

  //thread creation
  auto *thread_0 = new thread;
  thread_0->tid = 0;
  thread_0->current_state = RUNNING;
  sigsetjmp(thread_0->env,1);
  sigemptyset(&(thread_0->env->__saved_mask));

  //set an action to SIGVTALRM
  if (sigvtalrm_action() < 0)
  {
    std::cerr << "system error: sigaction failed." << std::endl;
    delete(thread_0);
    std::exit(1);
  }
  mask_signals();
  given_tids.insert (0);
  all_threads[0] = (thread_0);
  unmask_signals();
  quantum_sec = quantum_usecs;
  reset_timer();
  total_quantum_counter = 1;
  mask_signals();
  running_thread = thread_0;
  unmask_signals();
  return 0;
}

int uthread_spawn(thread_entry_point entry_point){
  //validation checks
  if(entry_point == nullptr){
    return -1;
  }
  mask_signals(); //neta add
  if(given_tids.size() == MAX_THREAD_NUM){
    unmask_signals(); //neta add
    return -1;
  }

  //creation of new thread
  auto *thread_1 = new thread;
  char* stack = new char[STACK_SIZE];
  thread_1->stack = stack;
  address_t sp = (address_t) (stack + STACK_SIZE - sizeof(address_t)); //changed arithmetic- neta
  address_t pc = (address_t)entry_point;
  sigsetjmp(thread_1->env, 1);

  (thread_1->env->__jmpbuf)[JB_SP] = translate_address(sp);
  (thread_1->env->__jmpbuf)[JB_PC] = translate_address(pc);
  sigemptyset(&(thread_1->env->__saved_mask));

  int tid = get_tid();
  thread_1->tid = tid;
  thread_1->current_state = READY;
  thread_1->thread_quatum_counter = 0;

  mask_signals();
  all_threads[tid] = (thread_1);
  given_tids.insert (tid);
  ready_q.push (thread_1);
  unmask_signals();
  return tid;
}

int uthread_terminate(int tid){
  mask_signals();
  //validation checks
  if(given_tids.count (tid) == 0){
    unmask_signals();
    return -1;
  }

  //check if the main thread in terminated
  if(tid == 0){
    free_all_threads();
    std::exit(0);
  }

  //terminates the wanted thread
  thread* cur_thread = nullptr;
  state state_1;
  for(int i=0; i<MAX_THREAD_NUM; i++){ //erase from all_threads and from
    // given_tids
    if(all_threads[i] == nullptr){
      continue;
    }
    if(all_threads[i]->tid == tid){
      state_1 = all_threads[i]->current_state;
      cur_thread = all_threads[i];
      all_threads[i] = nullptr;
      given_tids.erase (tid);
      break;
    }
  }
  if(state_1 == RUNNING){
    thread_to_terminate = running_thread;
    switch_context(TERMINATE);
  }
  erase_from_queue(state_1, tid);//erase from queue
  delete(cur_thread->stack); //deallocate memory of the thread
  delete(cur_thread);
  unmask_signals();
  return 0;
  }

int uthread_block(int tid){
  mask_signals();
  if((given_tids.count (tid) == 0)||(tid == 0)){
    unmask_signals();
    return -1;
  }
  if(all_threads[tid]->blocked == true){
    unmask_signals();
    return 0;
  }
  if(all_threads[tid]->current_state == RUNNING){
    unmask_signals();
    switch_context (BLOCKED);
  }
  all_threads[tid]->current_state = BLOCKED;
  all_threads[tid]->blocked = true;
  blocked_q.push (all_threads[tid]);
  erase_from_queue (READY, tid);
  unmask_signals();
  return 0;
}

int uthread_resume(int tid){ //we shouldnt need to consider sleeping here? - Neta
  mask_signals();
  if(all_threads[tid] == nullptr){
    unmask_signals();
    return -1;
  }
  if((all_threads[tid]->current_state != BLOCKED)||
  (all_threads[tid]->sleeping_time)>-1){
    unmask_signals();
    return 0;
  }
  ready_q.push (all_threads[tid]);
  erase_from_queue (BLOCKED, tid);
  all_threads[tid]->current_state = READY;
  all_threads[tid]->blocked = false;
  unmask_signals();
  return 0;
}

int uthread_sleep(int num_quantums){
  mask_signals();
  if(running_thread->tid == 0){
    unmask_signals();
    return -1;
  }
  if (num_quantums <= 0)
  {
    unmask_signals();
    return -1;
  }
  running_thread->sleeping_time = total_quantum_counter + num_quantums;
  blocked_q.push(running_thread);
  running_thread->current_state = BLOCKED;
  unmask_signals();
  switch_context(SLEEPING);
  return 0;
}

int uthread_get_tid(){
  return running_thread->tid;
}

int uthread_get_total_quantums(){
  return total_quantum_counter;
}

int uthread_get_quantums(int tid){
  mask_signals();
  if(all_threads[tid] == nullptr){
    unmask_signals();
    return -1;
  }
  int counter = all_threads[tid]->thread_quatum_counter;
  unmask_signals();
  return counter;
}

// //helper functions


int get_tid() {
  mask_signals();
  for(int i=0; i<MAX_THREAD_NUM; i++){
    if(given_tids.count (i) == 0){
      unmask_signals();
      return i;
    }
  }
  return -1;
}


int sigvtalrm_action(){
  struct sigaction sa = {nullptr};
  sa.sa_handler = &switch_context;
  return sigaction(SIGVTALRM, &sa, nullptr);
}

void free_all_threads(){
  mask_signals();
  for(int i=0; i<MAX_THREAD_NUM; i++){
    if(all_threads[i] == nullptr){
      continue;
    }
    delete(all_threads[i]->stack);
    delete(all_threads[i]);
  }
  unmask_signals();
}

void free_all_threads_no_mask(){
  for(int i=0; i<MAX_THREAD_NUM; i++){
    if(all_threads[i] == nullptr){
      continue;
    }
    delete(all_threads[i]->stack);
    delete(all_threads[i]);
  }
}

void switch_context(int sig){
  mask_signals();
  if (sigsetjmp(running_thread->env, 1) != 0)
  {
    if (thread_to_terminate != nullptr)
    {
      delete(thread_to_terminate->stack);
      delete(thread_to_terminate);
      thread_to_terminate = nullptr;
    }
    unmask_signals();
    return;
  }
  sigdelset(&running_thread->env->__saved_mask, SIGVTALRM);

  if(sig == SIGVTALRM){
    running_thread->current_state = READY;
    ready_q.push (running_thread);
  }

  else if(sig == BLOCKED){
    running_thread->current_state = BLOCKED;
    running_thread->blocked = true;
    blocked_q.push (running_thread);
  }

  running_thread = ready_q.front();
  ready_q.pop();
  running_thread->current_state = RUNNING;
  running_thread->thread_quatum_counter += 1;
  if(flag){
    total_quantum_counter++;
  }
  else{
    flag = true;
  }

  for(int i=0; i<MAX_THREAD_NUM; i++){
    if(all_threads[i] == nullptr){
      continue;
    }
    if((total_quantum_counter == all_threads[i]->sleeping_time)&&
       !all_threads[i]->blocked){
      all_threads[i]->sleeping_time = -1;
      all_threads[i]->current_state = READY;
      ready_q.push (all_threads[i]);
      erase_from_queue (BLOCKED, i);
    }
  }
  reset_timer();
  unmask_signals();
  siglongjmp(running_thread->env,1);
}

void erase_from_queue(state state_1, int tid){
  mask_signals();
  std::queue<thread*> temp;
  if(state_1 == BLOCKED){
    while(!blocked_q.empty()){
      if(blocked_q.front()->tid != tid){
        temp.push (blocked_q.front());
      }
      blocked_q.pop();
    }
    while(!temp.empty()){
      blocked_q.push (temp.front());
      temp.pop();
    }
    unmask_signals();
    return;
  }
  while(!ready_q.empty()){
    if(ready_q.front()->tid != tid){
      temp.push (ready_q.front());
    }
    ready_q.pop();
  }
  while(!temp.empty()){
    ready_q.push(temp.front());
    temp.pop();
  }
  unmask_signals();
}

void unmask_signals(){
  sigemptyset (&signal_set);
  sigaddset (&signal_set, SIGVTALRM);
  sigaddset (&signal_set, SIGINT);
  sigaddset (&signal_set, SIGTERM);
  if(sigprocmask (SIG_UNBLOCK, &signal_set, nullptr) == -1){
    free_all_threads_no_mask();
    std::cerr << "system error: sigpromask failed." << std::endl;
    std::exit (1);
  }
}

void mask_signals(){
  sigemptyset (&signal_set);
  sigaddset (&signal_set, SIGVTALRM);
  sigaddset (&signal_set, SIGINT);
  sigaddset (&signal_set, SIGTERM);
  if(sigprocmask (SIG_BLOCK, &signal_set, nullptr) == -1){
    free_all_threads_no_mask();
    std::cerr << "system error: sigpromask failed." << std::endl;
    std::exit (1);
  }
}

void reset_timer(){
  struct itimerval timer = {{0}};
  timer.it_value.tv_sec = quantum_sec/MICROSEC;
  timer.it_value.tv_usec = quantum_sec%MICROSEC;
  if(setitimer(ITIMER_VIRTUAL, &timer, nullptr) == -1){
    std::cerr << "system error: setitimer failed." << std::endl;
    free_all_threads();
    std::exit(1);
  }
}

