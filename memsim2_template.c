#include<stdlib.h>
#include<stdio.h>
#include <math.h>
#include "sim.h"

#define TRUE 1
#define FALSE 0

#define READ 1
#define WRITE 0

#define TRACE TRUE


#define MAX_NUM_THREADS 2  // Must be less than 10
#define MAX_QUEUE_SIZE 10
#define WRITE_THRESHOLD  MAX_QUEUE_SIZE - MAX_NUM_THREADS


#define CACHE_HIT_TIME 1.0
#define CACHE_MISS_TIME  50.0
#define CACHE_FLUSH_TIME 50.0
#define CPU_DELAY 0.0
#define WRITE_BUFFER_DELAY  1.0
#define FLUSH_WRITE_DELAY  0.05

#define CACHESIZE 2 // Size in Blocks
#define NUMWAYS 1
#define NUMSETS  CACHESIZE/NUMWAYS
#define BLKSIZE 5  // 2^5 = 32 bytes per block




char *filename[] = {"memtrace0", "memtrace1", "memtrace2","memtrace3", "memtrace4", "memtrace5", "memtrace6", "memtrace7", "memtrace8", "memtrace9"};
FILE *fp[MAX_NUM_THREADS];
int Hits[MAX_NUM_THREADS], Misses[MAX_NUM_THREADS];
int totalHits = 0, totalMisses = 0, totalFlushes = 0;
int totalReflushes = 0; // NEW statistic for part 2
int numrecords[MAX_NUM_THREADS];

int   numFoundInWriteQ = 0;
int   numFoundInLoadQ = 0;

void FlushDirtyBlock();
/* ****************************************** */
/* The tracefile will store one record for each memory access that is recorded.
Each record consists of the memory address being accessed, the delay (in cycles)
between the  completion of this request and the next memory access, and whether 
the access is a Load (type = READ) or Store (type = WRITE);    */

struct tracerecord {
  unsigned address;
  int delay;
  int type;
};

/* A cache block is represented by the structure below, consisting of two status 
flags Vaild (V) and Dirty (D),  a TAG field, and a data field implemented as a 
pointer to the actual data bytes in  the block */

struct cacheblock {
  int V;
  int D;
  int TAG;
  char * BLKDATA;
};
/* ******************************************** */

/* Structures used to hold memory requests sent to the memory queue */
struct memrec {
  int thread_id;
  unsigned address;
  int type;
  int cacheway; // NEW
};

// NEW
/* Structures used to hold memory requests sent to the Load  queue */
struct loadrec {
  int thread_id;
  unsigned blknum;
  unsigned  way;
};

/* Generic queue entry with a pointer to the request and a next field for chaining the queue entries together */
struct qentry {
  void * data;
  struct qentry  *next;
};


int writeQ[MAX_QUEUE_SIZE];
int writeQHead = 0;
int writeQTail = 0;
int writeQCount = 0;


// NEW for part 2
struct qentry  loadQ[MAX_QUEUE_SIZE];
int loadQHead = 0;
int loadQTail = 0;
int loadQCount = 0;


struct cacheblock CACHE[NUMSETS][NUMWAYS];  // Cache Structure

SEMAPHORE *sem_memreq,  *sem_memdone[MAX_NUM_THREADS];
SEMAPHORE *sem_cpu, *sem_memory;
SEMAPHORE *sem_writebufferfull;
SEMAPHORE *sem_loadbufferfull;  
SEMAPHORE *sem_cacheaccess;

PROCESS *proccntrl[MAX_NUM_THREADS]; // Yacsim process modeling activities of a  thread
PROCESS  *memcntrl; // Yacsim processes for memory controller 
PROCESS  *writecntrl; // Yacsim processes for flushing thread
PROCESS *memdispatcher; // Yacsim processes for low-level memory dispatcher (NEW)
 
/* ************************************************  */
int printStatistics(int normalCompletion) {
  int i;
  
  printf("\nSimulation ended  at %3.0f\n",GetSimTime());
  
  printf("Total  HITS: %d Total MISSES: %d  Time: %3.0f\n", totalHits, totalMisses, GetSimTime());
  printf("Total number of flushes of dirty blocks: %d. Number of reflushes: %d\n\n", totalFlushes, totalReflushes); 
  
  for (i=0; i < MAX_NUM_THREADS; i++) {
    printf("Number of records processed from trace file: %d\n", numrecords[i]);
    printf("Thread: %d  Hits: %d  Misses: %d   TotalfoundInWriteQueue: %d\n", i, Hits[i], Misses[i], numFoundInWriteQ);
  }
  
  if (normalCompletion){
    printf("\nAll Threads Completed reading traces\n");
    

  }
  else
    printf("Simulation ended without completing all traces\n");
  
}

/* ************************************************  */
struct tracerecord  tracerec;   // Trace record

int  getNextRef(struct tracerecord *ref, int thread_id) {
  static int numThreadsDone = 0;
  

  if (fread(ref, sizeof(struct tracerecord), 1,fp[thread_id])  == 1) {
    numrecords[thread_id]++;
    return(TRUE);
  }
  else {
    numThreadsDone++; // Number threads completed their trace
    return(FALSE); // Trace complete for this thread
  }
}

/* *****************************    MEMORY  QUEUE OPERATIONS  ************************** */

struct qentry *head = NULL, *tail = NULL;

// Insert a memory request at the tail of the memory queue 
void PutInMemQ(struct memrec *req) {
  struct qentry *recptr;
  
  recptr = malloc(sizeof(struct qentry));
  recptr->data = (void *) req;
  recptr->next = NULL;
  
  if (head == NULL) {
    head = tail = recptr; 
  }
  else {
    tail->next = recptr;;
    tail = recptr;
  }
}


// Return the memory record at the head of the memory queue; return null if queue is empty.
struct memrec * GetFromMemQ() {
  struct qentry *recptr;
  struct memrec *dataptr;
  
  if (head == NULL) 
    {
      printf("ERROR: Empty MemeQ\n"); 
      return(NULL);
    }
  else {
    recptr = head;
    head = head->next;
    if (head == NULL) tail = NULL;
    dataptr = recptr -> data;
    free(recptr);
    return (dataptr);
  } 
}


/* ***************************** WRITE QUEUE OPERATIONS ************************** */


/* Adds the memory request to the tail of the Write Queue.*/

void PutInWriteQ(int blocknum){ 
  if (writeQCount < MAX_QUEUE_SIZE) {
    writeQ[writeQTail] = blocknum;
    writeQTail = (writeQTail + 1) % MAX_QUEUE_SIZE;
    writeQCount++;
  }
  else {
    printf("ERROR: Inserting into a full Write Queue\n");
    exit(1);
  }
  if (TRACE)
    printf("Added  Write for Block Num %x in Write Queue at Time %5.2f\n", blocknum, GetSimTime());
}


/* Removes the entry at the head of the Write  Queue. Returns a NULL pointer.
 */

void  *  GetDeleteWriteQ() { 
  writeQHead = (writeQHead + 1) % MAX_QUEUE_SIZE;
  writeQCount--;
  return (void  *) NULL;
}


/* Checks if there is a pending memory write  for the block passed to the function. 
 * Searches the queue from back to front. If found it returns TRUE; else returns FALSE.                    
 */

int isInWriteQ(int blk_num) {
  int count, ptr;

  if (TRACE)
    printf("\nWrite Queue Size: %d at time %5.2f\n", writeQCount, GetSimTime());

  for (ptr = writeQTail, count = 0; count < writeQCount; count++) {
    
    if (ptr == 0) 
      ptr = MAX_QUEUE_SIZE-1;
      else 
	ptr--;
    if (TRACE)
      printf("writeQ[%d] : %x\n", ptr, writeQ[ptr]);
    
    if (writeQ[ptr] == blk_num) {
      numFoundInWriteQ++;
      return TRUE;
    }
  }

  if (TRACE)
    printf("\n");
  return FALSE;
}



/* *****************************  LOAD QUEUE OERATIONS  ************************** */


/* Checks if there is a pending memory read for the block containing the address
 * passed to the function. Searches the queue from back to front. If found it returns
 * the Load Queue index of the matching request; else returns -1.                    
 */

int isInLoadQueue(int address) {
  int count, ptr;
  int blknum = address >> BLKSIZE;

  if (TRACE)
    printf("\nLoad Queue Size: %d at time %5.2f\n", loadQCount, GetSimTime());

  for (ptr = loadQTail, count = 0; count < loadQCount; count++) {
    ptr -= 1;
    if (ptr == -1) 
      ptr += MAX_QUEUE_SIZE;
    if (TRACE)
      printf("loadQ[%d] : %x\n", ptr, (unsigned long) loadQ[ptr].data);
    
    if ( ( (unsigned long) loadQ[ptr].data) == blknum) {
      numFoundInLoadQ++;
      return ptr;
    }
  } 
  if (TRACE)
    printf("\n");
  return -1;
}


/* Adds the memory request to the tail of the Load Queue if the index parameter is -1.
 * Otherwise it adds the request to a list of pending read requests for this block.  /
 * The data field is set to the block number to facilitate easy search.              
*/

void PutInLoadQ(int index, struct memrec * memreq){
  
  struct qentry *p;
 
  p = malloc(sizeof (struct qentry));
  p-> next = NULL;
  p->data = (void *) memreq;

  if (index == -1) {
    if (TRACE) 
      printf("Appending Load request for address  %x to  tail of Load Queue at Time %5.2f\n", memreq->address, GetSimTime());

    if (loadQCount < MAX_QUEUE_SIZE) {
      loadQ[loadQTail].next = p;
      loadQ[loadQTail].data = (void *) ( (long unsigned)  (memreq->address >> BLKSIZE) );
      loadQTail = (loadQTail + 1) % MAX_QUEUE_SIZE;
      loadQCount++;
    }
    else {
      printf("ERROR: Inserting into a full Load Queue\n");
      exit(1);
    }
  }
  else   {
    if (TRACE) 
      printf("Piggybacking Load request for addressm %x to index %d  of Load Queue at Time %5.2f\n", memreq->address, index, GetSimTime());
    p->next = loadQ[index].next;
    loadQ[index].next = p;
  }
}


/* Removes the entry at the head of the Load Queue. Returns a pointer to the chain of requests for this block. 
 */
 
struct qentry *  GetDeleteLoadQueue(){
  struct qentry * p;
  
  if (TRACE) 
    printf("Deleting Block %x from LoadQ[%x] at time %5.2f. QCount: %d\n",  (unsigned long) loadQ[loadQHead].data, loadQHead, GetSimTime(), loadQCount);  

  p = loadQ[loadQHead].next;
  loadQHead = (loadQHead + 1) % MAX_QUEUE_SIZE;
  loadQCount--;


  return p;
}

/* *****************************  CACHE  ************************** */

int GetVictim(unsigned set_index) {
  int victim = 0;
  return victim;  // No choice for  Direct Mapped Cache
}


/***************************************************************** 
**  Checks for presence of specified address in the cache.   
**  Sets Dirty bit on a Write Hit. Updates  Hit/Miss count.
**  Returns success or failure.                            
*******************************************************************/ 

int LookupCache(unsigned address, unsigned type) {
  unsigned block_num, set_index, my_tag;
  unsigned way;

  SemaphoreWait(sem_cacheaccess); // 
  block_num =  address >> BLKSIZE;
  set_index = block_num% NUMSETS;
  my_tag = block_num/NUMSETS;
  

  for (way=0; way < NUMWAYS; way++)     {
	      if ( (CACHE[set_index][way].V == TRUE) && (CACHE[set_index][way].TAG == my_tag)) {
		 if (type == WRITE) 
		   CACHE[set_index][way].D = TRUE;  // Indicate DIRTY block
		 totalHits++; 
		 ProcessDelay(CACHE_HIT_TIME);	// Delay for accessing cache
		 SemaphoreSignal(sem_cacheaccess); // 
		 return(TRUE); // Cache Hit
	}
    }
  totalMisses++;
  ProcessDelay(CACHE_HIT_TIME);	// Delay for accessing cache
   SemaphoreSignal(sem_cacheaccess); // 
  return(FALSE);  // Cache Miss
}


/***************************************************************** 
**  Sets cache block status bits after a  MISS is served by memory.
********************************************************************/ 

void UpdateCache( struct memrec *req) {
  unsigned  set_index;
  int type, way, my_tag;
  char *blockdata;

  set_index = (req->address >> BLKSIZE)% NUMSETS;
 // way = req->cacheway;
  way = GetVictim(set_index); 
  my_tag = (req->address >> BLKSIZE)/NUMSETS;
  type = req->type; 
  
  CACHE[set_index][way].V = TRUE;
  CACHE[set_index][way].TAG = my_tag;
  CACHE[set_index][way].BLKDATA = blockdata;   // In the  present implementation this will be a null pointer.
}




  /* *********************************  MEMORY DISPATCHER  FUNCTIONS  *************************************** */




void DispatchMemoryRequest(){

  /* ****************************************************
   * Called to dispatch a new request to the memory unit.
   * Use the following policy to select a request:
   *         Give priority to a read request in the Load Queue unless the
   *         Write Queue has more than WRITE_THRESHOLD requests pending.  
   *
   *
   * The memory requests are to served serially and synchronously.
   * To serve a write request 
   *         Delay by  CACHE_FLUSH_TIME and delete it from the Write Queue
   * To serve a read request 
   *         Delay by  CACHE_MISS_TIME  and delete it from the Load Queue
   *         Install the new block into the cache when safe to do so and update cache
   *         Wake up all (possibly piggybacked) threads waiting for this block
   ******************************************************* */
   struct qentry q = loadQ[loadQHead];
   struct qentry *p;
   int job_num;
   
   if(writeQCount > WRITE_THRESHOLD || loadQCount <= 0){//write
        ProcessDelay( CACHE_FLUSH_TIME );
        GetDeleteWriteQ();
        SemaphoreSignal(sem_writebufferfull);
   }
   else{//read
        //Delay by  CACHE_MISS_TIME  and delete it from the Load Queue
        ProcessDelay( CACHE_MISS_TIME );
        p = GetDeleteLoadQueue();
       
        
        //Install the new block into the cache when safe to do so and update cache
        SemaphoreWait( sem_cacheaccess );
        UpdateCache(p -> data);
        SemaphoreWait( sem_cacheaccess );
        
        while(p != NULL){
             job_num = ((struct memrec *)p->data)->thread_id;
             SemaphoreSignal(sem_memdone[job_num]);
            p = p->next;
        }
        SemaphoreSignal(sem_loadbufferfull);        
   }
}



  /* *********************************  MEMORY CONTROLLER FUNCTIONS  *************************************** */


void HandleCacheMiss(struct memrec *req){
  unsigned set_index, my_tag;
  int type, way;
  char *blockdata;

  set_index = (req->address >> BLKSIZE)% NUMSETS;
  my_tag = (req->address >> BLKSIZE)/NUMSETS;
  type = req->type;  // Read (1) or Write (0)
  way = GetVictim(set_index);

  if (CACHE[set_index][way].D == TRUE) {      // This implementation does not actually write the dirty block.
    totalFlushes++;
    FlushDirtyBlock(set_index, way);
  }

  unsigned block_number;
  block_number = (req->address) >> BLKSIZE;
  
  if(isInWriteQ(block_number)){
    UpdateCache(req);
    ProcessDelay( WRITE_BUFFER_DELAY );
    SemaphoreSignal( sem_memdone[req->thread_id] );
  }
  else {
    int inload = isInLoadQueue(req->address);
    SemaphoreWait(sem_loadbufferfull);
    PutInLoadQ(inload, req);
    if(inload == -1){
        SemaphoreSignal(sem_memory);
    }
  } 

  
  if (TRACE)
    printf("In HandleCacheMiss at time %5.2f\n", GetSimTime());

}


/* ************************************************  */

void FlushDirtyBlock(int set_index, int way) {
  /*
1. Iinvalidate the cache line being flushed.

2.  Compute the block number of the block being evicted from the cache.

3. Append this request to the tail of a write buffer by calling the 
function PutInWriteQ( ) provided, if there is space in the write buffer.
if there is no space in the queue wait until there is space. If done
correctly, the memory writer process will at some point complete
a memory write from the buffer and free up space. It may be easiest 
to use a semaphore initialized to the size of the queue to control 
access to the queue, but feel free to choose your own mechanism.

4. Signal the background writing thread to indicate there is another request in the queue.
  */

  CACHE[set_index][way].V = FALSE;
  unsigned my_tag = CACHE[set_index][way].TAG;
  unsigned block_num = set_index + my_tag * NUMSETS;

  SemaphoreWait(sem_writebufferfull);
  PutInWriteQ(block_num);
  SemaphoreSignal(sem_memory);


  
     
  if (TRACE)
    printf("In function FlushDirtyBlock  at time %5.2f\n", GetSimTime());
}

int ServiceMemRequest() {

  struct memrec *memreq;
  
  memreq = GetFromMemQ();

  /* Handle the cache miss for the request (mem_req) at the head of the Memory Queue 
   * Invalidate and flush the cache block if necessary
   *                      Add write request to tail of Write Queue
   *                      Signal the Memory Dispatch process that a new request has been added.
   * If the missing block is available in the Write Queue
   *                   Copy the block  and update the cache
   *                   Delay WRITE_BUFFER_DELAY 
   *                   Signal the waiting thread (cache miss has been handled)
   * Otherwise (we need to get the block from memory)
   *                   Place a read request in the Load Queue
   *                   If there is an existing requests for this block  in the Load Queue
   *                                 Piggyback your request to the existing request
   *                   Otherwise
   *                                 Add request to tail of Load Queue
   *                                 Signal the Memory Dispatch process that a new request has been added.
   *                   Return immediately after placing request in the Load Queue 
   *                   When the memory access completes the Dispatch Process should wake up the waiting thread.
   */

 
  SemaphoreWait(sem_cacheaccess);   /*  Make sure only one entity is manipulating the cache at any time */

  HandleCacheMiss(memreq);
  
  SemaphoreSignal(sem_cacheaccess);
}



  /* *********************************  MEMORY DISPATCHER   *************************************** */

void memorydispatcher() {
  int job_num;
  job_num = ActivityArgSize(ME) - 1;
  
  if (TRACE) 
    printf("Waking up Memory Dispatcher  at time %5.2f\n", GetSimTime());
  
  while(1) {
    SemaphoreWait(sem_memory);  // Wait to be woken up by Memory Controller
                                // Signalled whenever there is a write (flush request)
                                // or a new read request to the tail of the Load Queue
                                // Piggybacked requests are not signalled
    if (TRACE)
      printf("Memory Dispatcher will dispatch a request at time %5.2f\n", GetSimTime());
    DispatchMemoryRequest();   // Choose and dispatch a request to the memory
    
  }
}

  /* *********************************  MEMORY CONTROLLER  *************************************** */

void memorycontroller() {
  int job_num;
  job_num = ActivityArgSize(ME) - 1;

  if (TRACE) 
    printf("Waking up Memory Controller  at time %5.2f\n", GetSimTime());
  
  while(1){
    SemaphoreWait(sem_memreq);  // Wait to be woken up by a Processor Thread on a cache miss
    ServiceMemRequest();  
    }
}


  /* *********************************  PROCESSOR THREAD *************************************** */

void processor() {
  struct tracerecord  *mem_ref = malloc(sizeof(struct tracerecord));  // Record from trace file
  struct memrec *mem_req = malloc(sizeof(struct memrec));    // Memory Request for Memory Controller
 
  // Yacsim specific commands
  int job_num;   // ID for this thread
  job_num = ActivityArgSize(ME) - 1;
	
  if (TRACE) 
    printf("Waking up Processor Thread %d at time %5.2f\n", job_num, GetSimTime());

  SemaphoreWait(sem_cpu); // Contend for the CPU
  if (TRACE)
    printf("Thread %d got CPU at time %5.2f\n", job_num, GetSimTime());

  while(1) {	  
    if (getNextRef(mem_ref, job_num) == FALSE)
      break; // Get next trace record from tracefile; Quit if all my records processed.
    


	  while (!LookupCache(mem_ref->address, mem_ref->type))  { // Check until successful
	    if (TRACE)
	      printf("Thread: %d  Cache MISS for address %x at time %5.2f\n", job_num, mem_ref->address, GetSimTime());          
	    Misses[job_num]++;

	   // Create a memory request record and insert into Memory Controller  queue
	    mem_req->thread_id = job_num;
	    mem_req->address = mem_ref->address; 
	    mem_req->type = mem_ref->type;
	    mem_req->cacheway= -1;    // Used to record the way of the cache 
	    PutInMemQ(mem_req);
	    SemaphoreSignal(sem_memreq);  // Wake up Memory Controller to handle this request. 
	    SemaphoreSignal(sem_cpu); // Release CPU on cache miss
	    SemaphoreWait(sem_memdone[job_num]);   // Wait for the memory request to complete.  
	    SemaphoreWait(sem_cpu); // Contend for  CPU
	  }

	  if (TRACE)
	    printf("Thread: %d  Cache HIT  for address %x at time %5.2f\n", job_num, mem_ref->address, GetSimTime());       
	  Hits[job_num]++;  // Double: counts earlier miss as a hit now
	  


	  

	  ProcessDelay( ( (double) mem_ref->delay) + CPU_DELAY);  // Delay simulating processor time between accesses
  } // end while

	if (TRACE)
	  printf("Thread %d completes at time %5.2f\n", job_num, GetSimTime());
	SemaphoreSignal(sem_cpu);  // Relinquish CPU and terminate thread
}


  /* ************************************************************************************ */
  /* ************************************************************************************ */


void UserMain(int argc, char *argv[])
{
  void memorycontroller(), processor(), memorydispatcher();
  int i;

  /* *********************************  PROCESSES *************************************** */
  // Create a process to model the activities of the processor
    for (i=0; i < MAX_NUM_THREADS; i++) {
      proccntrl[i] = NewProcess("proccntrl",processor,0);
      ActivitySetArg(proccntrl[i],NULL,i+1);
      ActivitySchedTime(proccntrl[i],0.00001,INDEPENDENT);
    }
    
    // Create a process to model the activities of the memory controller 
    memcntrl = NewProcess("memcntrl",memorycontroller,0);
    ActivitySetArg(memcntrl,NULL,1);
    ActivitySchedTime(memcntrl,0.00002,INDEPENDENT);

    // Create a process to model the activities of the low-level memory dispatcher
    memdispatcher = NewProcess("memdispatcher",memorydispatcher,0);
    ActivitySetArg(memdispatcher,NULL,1);
    ActivitySchedTime(memdispatcher,0.00002,INDEPENDENT);

    /* *********************************  SEMAPHORES  *************************************** */

  sem_cpu = NewSemaphore("semcpu", 1); // Controls access to CPU

  sem_cacheaccess = NewSemaphore("semcacheaccess", 1); // Serializes access to cache
  
  sem_writebufferfull = NewSemaphore("semwritebufferfull", MAX_QUEUE_SIZE); // Controls access to memory write queue
  sem_loadbufferfull = NewSemaphore("semloadbufferfull", MAX_QUEUE_SIZE); // Controls access to memory read queue

  sem_memreq = NewSemaphore("memreq",0); //Used by a Processor Thread to signal  Memory Controller with a new request
  
  sem_memory = NewSemaphore("semmemory", 0); // Used by Memory Controller to  signal Memory Dispatcher with a new (non-piggybacked) request  
 
  for (i=0; i < MAX_NUM_THREADS; i++) 
    sem_memdone[i] = NewSemaphore("memdone",0); // Used to signal Processor Thread that cache miss handling is complete

    /* *********************************  Global Initializations  *************************************** */
   
  totalHits = 0; 
  totalMisses = 0;
  totalFlushes = 0;
  totalReflushes = 0; 
  numFoundInWriteQ = 0;
  numFoundInLoadQ = 0;

  for (i=0; i < MAX_NUM_THREADS; i++)
    Hits[i] = Misses[i] = 0;
  
  for (i=0; i < MAX_NUM_THREADS; i++)
    fp[i] = fopen(filename[i], "r");  // Separate trace file for each thread
  
// Initialization is done, now start the simulation

    DriverRun(1000000000.0); // Maximum time of the simulation (in cycles).   
     printStatistics(TRUE); 
}


