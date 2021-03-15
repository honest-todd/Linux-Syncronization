#include <linux/syscalls.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/stddef.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/cs1550.h>


static LIST_HEAD(sem_list); 	  	// gobal queue of semaphores.
long max_sem_id = 0;	  			// represents the max sem_id number used to define a new semaphore.
static DEFINE_RWLOCK(sem_rwlock); 	// system-wide lock on sem_list.

/**
 * Creates a new semaphore. The long integer value is used to
 * initialize the semaphore's value.
 *
 * The initial `value` must be greater than or equal to zero.
 *
 * On success, returns the identifier of the created
 * semaphore, which can be used with up() and down().
 *
 * On failure, returns -EINVAL or -ENOMEM, depending on the
 * failure condition.
 */
SYSCALL_DEFINE1(cs1550_create, long, value)
{
	struct cs1550_sem *new_sem = NULL;

	// check that a valid value was passed in
	if (value < 0){ return -EINVAL; }
	// allocate new semaphore
	new_sem = (struct cs1550_sem*)kmalloc(sizeof(struct cs1550_sem), GFP_ATOMIC);
	// ensure that memory has been properly allocated
	if (new_sem == NULL ){ return -ENOMEM; }

	// intialize a new semaphore
	spin_lock_init(&new_sem->lock);
	INIT_LIST_HEAD(&new_sem->list);
	INIT_LIST_HEAD(&new_sem->waiting_tasks);
	new_sem->value = value;					  
	new_sem->sem_id= max_sem_id++;

	// add semaphore to global queue
	write_lock(&sem_rwlock);
	list_add_tail(&new_sem->list, &sem_list); 
	write_unlock(&sem_rwlock);

	return new_sem->sem_id;
}

/**
 * Performs the down() operation on an existing semaphore
 * using the semaphore identifier obtained from a previous call
 * to cs1550_create().
 *
 * This decrements the value of the semaphore, and *may cause* the
 * calling process to sleep (if the semaphore's value goes below 0)
 * until up() is called on the semaphore by another process.
 *
 * Returns 0 when successful, or -EINVAL or -ENOMEM if an error
 * occurred.
 */
SYSCALL_DEFINE1(cs1550_down, long, sem_id)
{
	struct cs1550_sem *sem = NULL;
	struct cs1550_task *newHead = NULL;

	// search for a given semaphore
	read_lock(&sem_rwlock);
	list_for_each_entry(sem, &sem_list, list) {
		if (sem->sem_id == sem_id){ goto found; }
	}
	read_unlock(&sem_rwlock);		
	return -EINVAL; 
	
	found:
	// start critical section
	// printk(KERN_WARNING "cs1550_down syscall: pid=%d entered the critical section.\n", current->pid);
	spin_lock(&sem->lock);
	sem->value -= 1;
	if(sem->value<0){

		// allocate a new task entry
		newHead = (struct cs1550_task*)kmalloc(sizeof(struct cs1550_task), GFP_NOWAIT);

		// insert current task into waiting queue
		INIT_LIST_HEAD(&newHead->list);
		newHead->task = current;
		list_add_tail(&newHead->list, &sem->waiting_tasks);

		// put current task to sleep
		set_current_state(TASK_INTERRUPTIBLE);
		// end critical section
		// printk(KERN_WARNING "cs1550_down syscall: pid=%d leaving the critical section.\n", current->pid);
		spin_unlock(&sem->lock);
		read_unlock(&sem_rwlock);
		
		// sceduele next ready task to be run
		schedule();
		return 0; 
	}
	// end critical section
	// printk(KERN_WARNING "cs1550_down syscall: pid=%d leaving the critical section.\n", current->pid);
	spin_unlock(&sem->lock);
	read_unlock(&sem_rwlock);
	return 0;
}

/**
 * Performs the up() operation on an existing semaphore
 * using the semaphore identifier obtained from a previous call
 * to cs1550_create().
 *
 * This increments the value of the semaphore, and *may cause* the
 * calling process to wake up a process waiting on the semaphore,
 * if such a process exists in the queue.
 *
 * Returns 0 when successful, or -EINVAL if the semaphore ID is
 * invalid.
 */
SYSCALL_DEFINE1(cs1550_up, long, sem_id)
{
	struct cs1550_sem *sem = NULL;
	struct cs1550_task *head = NULL;

	// search for a given semaphore
	read_lock(&sem_rwlock);
	list_for_each_entry(sem, &sem_list, list) {
		if (sem->sem_id == sem_id){ goto found; }
	}
	read_unlock(&sem_rwlock);		
	return -EINVAL; 

	found:
	// start critical section
	// printk(KERN_WARNING "cs1550_down syscall: pid=%d entered the critical section.\n", current->pid);
	spin_lock(&sem->lock);
	sem->value += 1;
	if (sem->value <= 0){

		// ensure sempahore contains waiting task(s)
		if (list_empty(&sem->waiting_tasks)){

			// end critical section
			// printk(KERN_WARNING "cs1550_down syscall: pid=%d leaving the critical section.\n", current->pid);
			spin_unlock(&sem->lock);
			read_unlock(&sem_rwlock);
			return -EINVAL;
		}

		// remove the next task to be run from the waiting queue
		head = list_first_entry(&sem->waiting_tasks, struct cs1550_task, list);
		list_del(&head->list);

		// wake up the task
		wake_up_process(head->task);
	}
	
	// end critical section
	// printk(KERN_WARNING "cs1550_down syscall: pid=%d leaving the critical section.\n", current->pid);
	spin_unlock(&sem->lock);	
	read_unlock(&sem_rwlock);
	return 0;
}

/**
 * Removes an already-created semaphore from the system-wide
 * semaphore list using the identifier obtained from a previous
 * call to cs1550_create().
 *
 * Returns 0 when successful or -EINVAL if the semaphore ID is
 * invalid or the semaphore's process queue is not empty.
 */
SYSCALL_DEFINE1(cs1550_close, long, sem_id)
{
	struct cs1550_sem *sem = NULL;

	// search for a given semaphore
	read_lock(&sem_rwlock);
	list_for_each_entry(sem, &sem_list, list) {
		if (sem->sem_id == sem_id){ goto found; }
	}
	read_unlock(&sem_rwlock);
	return -EINVAL;

	found:
	read_unlock(&sem_rwlock);
	// start critical section
	write_lock(&sem_rwlock);	
	spin_lock(&sem->lock);

	// ensure semaphore doesn't contain waiting task(s)
	if (list_empty(&sem->waiting_tasks)){
		list_del(&sem->list);
		spin_unlock(&sem->lock);
		kfree(sem);
		write_unlock(&sem_rwlock);
		return 0;
	}

	// proccess queue not empty
	// end critical section
	spin_unlock(&sem->lock);
	write_unlock(&sem_rwlock);
	return -EINVAL;
}
