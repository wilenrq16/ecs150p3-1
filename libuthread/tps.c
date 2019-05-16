#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "queue.h"
#include "thread.h"
#include "tps.h"

queue_t TPSqueue;

typedef struct memPage{
  int reference;
  void *page;
}memPage_t;

typedef struct tps{
  pthread_t tid;
  memPage_t *tpsPage;
}tps_t;

/*HELPER FUNCTIONS*/
int tps_find(void* tps, void* arg)
{
	
  pthread_t tid = *((pthread_t*)arg);

  if(((tps_t*)tps)->tid == tid) {
    return 1;
  }
  return 0;
}

int page_find(void* tps, void* arg)
{
	
  void* page = arg;

  if(((tps_t*)tps)->tpsPage->page == page) {
    return 1;
  }
  return 0;
}

static void segv_handler(int sig, siginfo_t *si, void *context)
{
    /*
     * Get the address corresponding to the beginning of the page where the
     * fault occurred
     */

    
    void *p_fault = (void*)((uintptr_t)si->si_addr & ~(TPS_SIZE - 1));
    
    /*
     * Iterate through all the TPS areas and find if p_fault matches one of them
     */
	 struct memPage *page = NULL;	    
	 queue_iterate(TPSqueue, page_find, p_fault, (void**)&page);
   
    /* Printf the following error message */
    if (page != NULL){
        fprintf(stderr, "TPS protection error!\n");
    }
	
    /* In any case, restore the default signal handlers */
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
    /* And transmit the signal again in order to cause the program to crash */
    raise(sig);
}



int tps_init(int segv)
{
  enter_critical_section();
  if (queue_length(TPSqueue) != -1){ //When the tps api has been initialized
    return -1;
  }

  //initializing Q
  TPSqueue = queue_create();
  exit_critical_section();

  if (segv) {
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = segv_handler;
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
  }
  return 0;
}

int tps_create(void)
{
  tps_t *newtps = NULL;
  pthread_t tidHolder = pthread_self();
	
  enter_critical_section();
  queue_iterate(TPSqueue, tps_find, (void*)tidHolder, (void**)&newtps);
  exit_critical_section();
	
  if (newtps != NULL){
	 return -1;
  }
	
  newtps = malloc(sizeof(struct tps));
  newtps->tid = tidHolder;
  newtps->tpsPage->page = mmap(NULL, TPS_SIZE ,PROT_NONE,MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  newtps->tpsPage->reference = 1;
  if (newtps == NULL || newtps->tpsPage == NULL ||  newtps->tpsPage->page == MAP_FAILED){
	  return -1;
  }
	
  enter_critical_section();	
  queue_enqueue(TPSqueue, newtps);
  exit_critical_section();

  return 0;
}

int tps_destroy(void)
{
  pthread_t tid= pthread_self();
  tps_t *tps = NULL;
	
  enter_critical_section();
  queue_iterate(TPSqueue, tps_find, (void*)&tid, (void**)&tps);
  	
  if (tps != NULL){
    exit_critical_section();
	 return -1;
  }

  munmap(tps->tpsPage->page, TPS_SIZE);
  free(tps->tpsPage);

  queue_delete(TPSqueue, tps);
  exit_critical_section();

  return 0;
}

int tps_read(size_t offset, size_t length, char *buffer)
{
	/* TODO: Phase 2 */
   tps_t *newtps = NULL;
  pthread_t tidHolder = pthread_self();
  //run without getting interrupted
  enter_critical_section();
  queue_iterate(TPSqueue, tps_find, (void*)tidHolder, (void**)&newtps);
  //lower the security to be able to read
  if (newtps == NULL || (offset + length) > TPS_SIZE || buffer == NULL) {
    exit_critical_section();
    return -1;
  }
  int error;
  error = mprotect(newtps->tpsPage->page, TPS_SIZE, PROT_READ);

  if (error != 0 ){
    exit_critical_section();
    return -1;
  }
  //transfering data
  memcpy(buffer, newtps->tpsPage->page, offset);
  //The memory cannot be accessed at all
  //mprotect(newtps->memPage, TPS_SIZE, PROT_NONE);

  error = mprotect(newtps->tpsPage->page, TPS_SIZE, PROT_NONE);
  if (error != 0 ){
    return -1;
  }
  exit_critical_section();

  return 0;
}

int tps_write(size_t offset, size_t length, char *buffer)
{
	/* TODO: Phase 2 */

  tps_t *newtps = NULL;
  pthread_t tidHolder = pthread_self();

  enter_critical_section();
  queue_iterate(TPSqueue, tps_find, (void*)tidHolder, (void**)&newtps);

  if (newtps == NULL || (offset + length) > TPS_SIZE || buffer == NULL) {
    return -1;
  }

  enter_critical_section();
  if (newtps->tpsPage->reference > 1) {
    //make space for new page
    memPage_t* newPage = malloc(sizeof(memPage_t));
    newPage->page = mmap(NULL, TPS_SIZE, PROT_WRITE, MAP_PRIVATE | MAP_ANON, 10, 0);
  
    newPage->reference= 1;
    
    mprotect(newtps->tpsPage->page, TPS_SIZE, PROT_READ);
    memcpy(newtps->tpsPage, newtps->tpsPage->page, TPS_SIZE);
    mprotect(newtps->tpsPage->page, TPS_SIZE, PROT_NONE);
    mprotect(newtps->tpsPage, TPS_SIZE, PROT_NONE);
    newtps->tpsPage->reference --;
    newtps->tpsPage = newPage;
  }
  int errorWrite;
  
  errorWrite = mprotect(newtps->tpsPage->page, TPS_SIZE, PROT_WRITE);

  if (errorWrite != 0 ){
    return -1;
  }

  memcpy(newtps->tpsPage->page, buffer, offset);

  errorWrite = mprotect(newtps->tpsPage->page, TPS_SIZE, PROT_NONE);
  if (errorWrite != 0 ){

    return -1;
  }
  exit_critical_section();

  return 0;
}

int tps_clone(pthread_t tid)
{
  tps_t* targettps = NULL;
  tps_t* clonetps = NULL;
  pthread_t clonetid = pthread_self();

  enter_critical_section();
  queue_iterate(TPSqueue, tps_find, (void*)&tid, (void**)&targettps);
  if (targettps == NULL) {
    exit_critical_section();
    return -1;
  }
  queue_iterate(TPSqueue, tps_find, (void*)&clonetid, (void**)&clonetps);
  exit_critical_section();
  if (clonetps != NULL) {
    exit_critical_section();
    return -1;
  }

  clonetps = malloc(sizeof(tps_t));  
  clonetps->tid = clonetid;
  clonetps->tpsPage = clonetps->tpsPage;
  clonetps->tpsPage->reference++;
  

  queue_enqueue(TPSqueue, (void*)clonetps);
  exit_critical_section();

return 0;
}
