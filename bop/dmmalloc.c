/**Divide & Merge Malloc

A dual-stage malloc implementation to support safe PPR forks
Each stage (sequential/no PPR tasks running) and a PPR tasks’ design is the same, a basic size-class allocator. The complications come when PPR_begin is called:
Allocating
A PPR task is given part of the parent’s free lists to use for its memory. This ensures that there will be no ‘extra’ conflicts at commit time.
If there is not enough memory, the parent gets new memory from the system and then gives it to the PPR task (GROUP_INIT)
If a PPR task runs out of memory, it must abort speculation. Calls to the underlying malloc are not guaranteed to not conflict with other.
The under study maintains access to the entire free list. Since either the understudy or the PPR tasks will survive past the commit stage, this is still safe.
At commit time, the free lists of PPR tasks are merged along with the standard BOP changes. This allows memory not used by PPR tasks to be reclaimed and used later.
Freeing
when a PPR task frees something from the global heap (something it did not allocate, eg it was either allocated by a prior PPR task or before and PPRs were started) it marks as freed and moves to a new list. This list is parsed at commit time and is always (???) accepted. We cannot immediately move it into the free list since when allocating a new object of that size. If multiple PPR tasks do this (which is correct in sequential execution) and both allocate the new object, the merge will fail.

Large objects:
Size classes need to be finite, so there will be some sizes not handled by this method, the work around is:
    allocation: if in PPR task, abort if not use DL malloc.
    free: when one of these is freed, check the block size. if it’s too large for any size class it was allocated with dl malloc. use dl free OR if sufficiently large divide up for use in our size classes.
*/

/*
Work Notes
Set low bit in pointers for allocation in a PPR task. 

*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h> /* for memcpy */
#include <assert.h>
#include <math.h>
#include <stdbool.h> //boolean types
#include "dmmalloc.h"

//Variable bit identifier code.
//#define BIT_IDENTIFIER k uses the kth lowest bit
#define BIT_IDENTIFIER 3
#define HEADER_IDENTIFIER_MASK (1<<(BIT_IDENTIFIER-1))

#define IS_ALLOCED_BY_DM(HEADER_P) ((HEADER_P) & HEADER_IDENTIFIER_MASK)

//bit masks for marking large objects when allocated. We have 2 bits to play with
#ifndef MASK
#if __WORDSIZE == 64
#define MASK 0xfffffffffffffffc
#define ALIGNMENT 8
#elif __WORDSIZE == 32
#define mask 0xfffffffc
#define ALIGNMENT 4
#else
#error "need 32 or 64 bit word size"
#endif
#endif


//alignment macros
#define ALIGN(size) (((size) + (ALIGNMENT-1)) & ~(ALIGNMENT-1))
#define HSIZE (ALIGN((sizeof(header))))

//grow macros
#define BLKS_1 50
#define BLKS_2 50
#define BLKS_3 40
#define BLKS_4 30
#define BLKS_5 20
#define BLKS_6 10
#define BLKS_7 10
#define BLKS_8 5
#define BLKS_9 5
#define BLKS_10 5
#define BLKS_11 5
#define BLKS_12 5
#define GROW_S ((BLKS_1 * SIZE_C(1)) + (BLKS_2 * SIZE_C(2)) + \
				(BLKS_3 * SIZE_C(3)) + (BLKS_4 * SIZE_C(4)) + \
				(BLKS_5 * SIZE_C(5)) + (BLKS_6 * SIZE_C(6)) + \
				(BLKS_7 * SIZE_C(7)) + (BLKS_8 * SIZE_C(8)) + \
				(BLKS_9 * SIZE_C(9)) + (BLKS_10 * SIZE_C(10)) +\
				(BLKS_11 * SIZE_C(11)) + (BLKS_12 * SIZE_C(12)) )

//header macros
#define HEADER(vp) ((header *) (((char *) (vp)) - HSIZE))
#define CASTH(h) ((struct header *) (h))
#define CHARP(p) (((char*) (p)))
#define PAYLOAD(hp) ((header *) (((char *) (hp)) + HSIZE))
#define PTR_MATH(ptr, d) ((CHARP(ptr)) + d)

//class size macros
#define NUM_CLASSES 12
#define MAX_SIZE sizes[NUM_CLASSES - 1]
#define SIZE_C(k) ALIGN((1 << (k + 4))) //allows for recursive spliting

//BOP macros
#define SEQUENTIAL 1 //just for testing, will be replaced with actual macro



//DEBUGING

typedef struct {
    header * start[NUM_CLASSES];
    header * end[NUM_CLASSES];
} ppr_list;

ppr_list* regions = NULL;


//header info
header* headers[NUM_CLASSES]; //current heads of free lists


int sizes[NUM_CLASSES] = {SIZE_C(1), SIZE_C(2), SIZE_C(3), SIZE_C(4),
                          SIZE_C(5), SIZE_C(6), SIZE_C(7), SIZE_C(8),
                          SIZE_C(9), SIZE_C(10), SIZE_C(11), SIZE_C(12),
                         };
int counts[NUM_CLASSES];


header* allocatedList= NULL; //list of items allocated during PPR-mode
header* freedlist= NULL; //list of items freed during PPR-mode


header* ends[NUM_CLASSES]; //end of lists in PPR region TODO removable?

//helper prototypes
static inline int get_index(size_t);
static inline void grow(void);
static inline void free_now(header*);
static inline bool list_contains(header* list, header* item);
static inline void add_alloc_list(header** list, header* item);
static inline header* dm_split(size_t alloc_size, int which);
static int index_bigger(size_t);


int get_index(size_t size) {
    assert(size == ALIGN(size));
    assert(size >= HSIZE);
    //Space is too big.
    if(size > MAX_SIZE)
        return -1; //too big
    //Computations for the actual index, off set of -5 from macro & array indexing
    //if the size is not a power of two, it get rounded down so need to add one for none-powers of 2
    int index = log2(size) - 5;
    if(sizes[index] < size)
        index++;
    assert(index >= 0 && index < NUM_CLASSES);
    assert(sizes[index] >= size);
    assert(index == 0 || sizes[index - 1] < size);
    return index;
}
/**Get more space from the system*/
static void grow() {
    char * space_head = calloc(GROW_S, 1); //system malloc, use byte-sized type
    int num_blocks[] = {BLKS_1, BLKS_2, BLKS_3, BLKS_4, BLKS_5, BLKS_6,
				    	BLKS_7,	BLKS_8, BLKS_9, BLKS_10, BLKS_11, BLKS_12};
    int class_index, blocks_left, size;
    header* head;
    for(class_index = 0; class_index < NUM_CLASSES; class_index++) {
        size = sizes[class_index];
        counts[class_index] += num_blocks[class_index];
        if(headers[class_index] == NULL) {
            //list was empty
            headers[class_index] = (header *) space_head;
            space_head += size;
            num_blocks[class_index]--;
        }
        for(blocks_left = num_blocks[class_index]; blocks_left; blocks_left--) {
            ((header *) space_head)->free.next = CASTH(headers[class_index]);
            head = headers[class_index];
            head->free.prev = CASTH(space_head);
            head = (header *) space_head;
            space_head+=size;
            headers[class_index] = head;
        }
    }
}
/** Divide up the currently allocated groups into regions*/
void carve(int tasks) {
    assert(tasks >= 2);
    if(regions != NULL)
        dm_free(regions); //don't need old bounds anymore
    regions = dm_malloc(tasks * sizeof(ppr_list));
    int index, count, j, r;
    header * current_headers[NUM_CLASSES];
    header * temp = NULL;
    for(index = 0; index < NUM_CLASSES; index++)
        current_headers[index] = (header *) headers[index];
    //actually split the lists
    for(index = 0; index < NUM_CLASSES; index++) {
        count = counts[index] = counts[index] / tasks;
        for(r = 0; r < tasks; r++) {
            regions[r].start[index] = current_headers[index];
            for(j = 0; j < count && temp; j++) {
                temp = (header *) current_headers[index]->free.next;
            }
            current_headers[index] = temp;
            if(r != tasks - 1) {
                //the last task has no tail, use the same as seq. exectution
                assert(temp != NULL);
                regions[r].end[index] = (header *) temp->free.prev;
            }
        }
    }
}

/**set the range of values to be used by this PPR task*/
void initialize_group(int group_num) {
    ppr_list my_list = regions[group_num];
    int ind;
    for(ind = 0; ind < NUM_CLASSES; ind++) {
        ends[ind] = my_list.end[ind];
        headers[ind] = my_list.start[ind];
    }
}


/**size: alligned size, includes space for the header*/
static inline header * get_header(size_t size, int * which) {
    header * found = NULL;
    //requested allocation is too big
    if(size > MAX_SIZE) {
        *which = -1;
        return NULL;
    } else {
        *which = get_index(size);
        found = headers[*which];
    }
    //clean up
    if(found == NULL || (!SEQUENTIAL && CASTH(found) == ends[*which]->free.next))
        return NULL;
    return found;
}

//actual malloc implementations
void * dm_malloc(size_t size) {
    //get the right header
    size_t alloc_size = ALIGN(size + HSIZE);
    int which = -2;
    header* block = get_header(alloc_size, &which);
    assert(which != -2);
    if(block == NULL) {
        //no item in list. Either correct list is empty OR huge block
        if(SEQUENTIAL && alloc_size > MAX_SIZE) {
            //huge block always use system malloc
            block = calloc(alloc_size, 1);
            if(block == NULL){
            	printf("SYS MALLOC FAILED!!!!!!\n");
            	return NULL;
            }
            //don't need to add to free list, just set information
            block->allocated.blocksize = alloc_size;
            assert(block->allocated.blocksize != 0);
            return PAYLOAD(block);
        } else if(0 && which < NUM_CLASSES-1 && headers[which + 1] != NULL) {
            block = headers[which + 1]; //block to carve up
            header* split = (header*) (CHARP(block) + sizes[which]); //cut in half
            assert(block != split);
            //split-specific info sets
            headers[which] = split; // was null
            headers[which+1] = (header*) headers[which+1]->free.next; //remove split up block
            block->allocated.blocksize = sizes[which];
			
			block->free.next = CASTH(split); 
			split->free.next = split->free.prev = NULL;
            
            //handle book-keeping
            counts[which] = 1;
            counts[which+1]--;
            
            assert(block->allocated.blocksize != 0);
        } else if(SEQUENTIAL) {
            grow();
            block = headers[which];
            block->allocated.blocksize = sizes[which];
            assert(block != NULL);
            assert(block->allocated.blocksize != 0);
        }else{
        	//TODO bop_abort
        }
    }else
    	block->allocated.blocksize = sizes[which];
    assert(block != NULL);
	//actually allocate the block
    headers[which] = (header *) block->free.next; //remove from free list
    counts[which]--;
    if(!SEQUENTIAL)
    	add_alloc_list(&allocatedList, block);
    assert(block->allocated.blocksize != 0);
    return PAYLOAD(block);
}
//return the index of the next-largest non-null size class
static int index_bigger(size_t size){
	int which = get_index(size) + 1;
	if(which == -1)
		return which;
	while(which < NUM_CLASSES){
		if(headers[which] != NULL)
			return which;
		which++;
	}
	return -1;
}
//DON'T NEED TO KEEP ALLOCATED LIST IN PPR TASK. PPR ALLOC SET BIT 
static inline header* dm_split(size_t size, int which){
	int bigger = which + 1; index_bigger(size);
	assert(bigger > which);
	
	header* block = headers[bigger]; //block to carve up
	
	void* end = PTR_MATH(block, sizes[bigger]); //ending address
    header* split = (header*) PTR_MATH(block, sizes[which]); //cut in half
    assert(headers[which] == NULL);
    headers[which] = split;
   	header* temp = (header*) PTR_MATH(split, sizes[which]);
   	
   	for(; (void*) temp < (void*) end; temp = (header*)  PTR_MATH(temp, sizes[which])){
   		//add new item to the free list
   		temp->free.next = CASTH(headers[which]);
   		headers[which]->free.prev = CASTH(temp);
   		headers[which] = (header*) temp;
   	}
    //handle book-keeping
    counts[which] = 1;
    counts[which+1]--;
	return block;
}

void * dm_calloc(size_t n, size_t size) {
    void * allocd  = dm_malloc(size * n);
    memset(allocd, 0, size * n);
    return allocd;
}

void * dm_realloc(void * ptr, size_t gsize) {
	//use syst-realloc if possible
	header* old_head = HEADER(ptr);
	assert(old_head->allocated.blocksize != 0);
	header* new_head;
	size_t new_size = ALIGN(gsize + HSIZE);
	int new_index = get_index(new_size);
	
	void* payload; //what the programmer gets
	if(SEQUENTIAL && old_head->allocated.blocksize > MAX_SIZE && new_size > MAX_SIZE){
		//use system realloc
		new_head = realloc(old_head, new_size);
		return PAYLOAD(new_head);
	}else if(new_index != -1 && sizes[new_index] == old_head->allocated.blocksize){
		return ptr; //no need to update
	}else{
		assert(old_head->allocated.blocksize != 0);
		size_t size = old_head->allocated.blocksize;
		//we're reallocating within managed memory
		payload= dm_malloc(new_size);
		
		payload = memcpy( payload, ptr,  old_head->allocated.blocksize); //copy memory
		old_head->allocated.blocksize = size;
		//TODO re-enable once not re-writing sys mallloc info 
		dm_free(old_head);
		return payload;
	}
}
/*
 * Free a block if any of the following are true
 *	1) Any sized block running in SEQ mode
 *	2) Small block allocated and freed by this PPR task.
 *	A free is queued to be free'd at BOP commit time otherwise.
*/
void dm_free(void* ptr) {
    header * free_header = HEADER(ptr);
    if(!SEQUENTIAL) {
        //needs to be allocated in this PPR task, ie. in the freed list
        if(list_contains(allocatedList, free_header))
            free_now(free_header);
        else
        	add_alloc_list(&freedlist, free_header);
    } else {
       	free_now(free_header);
    }
}
//free a (regular or huge) block now
static inline void free_now(header* head) {
    int which = -2;
    size_t size = head->allocated.blocksize;
    assert(size == ALIGN(size));
    //test for system block
    if(size > MAX_SIZE && SEQUENTIAL) {
    	printf("SYS FREE SIZE: %d\n", size);
        free(PAYLOAD(head)); //system free, only in PPR
        return;
    }
    header* free_stack = get_header(size, &which);
    assert(sizes[which] == size); //should exactly align
	if(free_stack == NULL){
		//empty free_stack
		head->free.next = head->free.prev = NULL;
		headers[which] = head;
		return;
	}
    free_stack->free.prev = CASTH(head);
    head->free.next = CASTH(free_stack);
    head->free.prev = NULL; //debug-only remove v1
    headers[which] = head;
    counts[which]++;
}

static inline bool list_contains(header* list, header* search_value) {
	if(list == NULL || search_value == NULL)
		return false;
    header* current;
    for(current = list; current != NULL; current = ((header *) (current->free.next))){
    	if(current == search_value)
    		return true;
    }
    return false;
}
static inline void add_alloc_list(header** list, header* item){
	if(*list == NULL)
		*list = item;
	else{
		(*list)->allocated.next = CASTH(item);
		*list = item;
	}
}
