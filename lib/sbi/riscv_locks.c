/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 * Copyright (c) 2021 Christoph Müllner <cmuellner@linux.com>
 */

#include <sbi/riscv_barrier.h>
#include <sbi/riscv_locks.h>

static inline bool spin_lock_unlocked(spinlock_t lock)
{
	return lock.owner == lock.next;
}

bool spin_lock_check(spinlock_t *lock)
{
	RISCV_FENCE(r, rw);
	return !spin_lock_unlocked(*lock);
}

bool spin_trylock(spinlock_t *lock)
{
	unsigned long inc = 1u << TICKET_SHIFT;
	unsigned long mask = 0xffffu << TICKET_SHIFT;
	u32 l0, tmp1, tmp2;

	__asm__ __volatile__(				/*使用next与owner的自旋锁就是排队自旋锁，使用next去排队，当owner到达next时，就获取锁，使用owner去释放锁 */
							/*获取锁时next+1，释放时owner+1，释放代码可见把spin_unlock即可*/
		/* Get the current lock counters. */	/*一般来说，按照小端模式，owner在低位，next在高位*/
		"1:	lr.w.aq	%0, %3\n"		/*把锁的值加载到l0中*/
		"	slli	%2, %0, %6\n"		/*把锁的值左移16位，放在tmp2中，也就是把owner的值放在高位去，准备比较*/
		"	and	%2, %2, %5\n"		/*拿上一步的owner的值去与mask，个人理解这一步只有64位才有意义，32位是个多余操作。因为trylock函数没有被调用过，理解无法证实*/
		"	and	%1, %0, %5\n"		/*lock值与mask，也就是直接拿到next的值，并且它任然还在高16位*/
		/* Is the lock free right now? */
		"	bne	%1, %2, 2f\n"		/*判断能否进行lock，owner与next相等才能lock，所以不相等直接退出*/
		"	add	%0, %0, %4\n"		/*相等则next加1，*/
		/* Acquire the lock. */
		"	sc.w.rl	%0, %0, %3\n"		/*把修改后的锁值写会对应变量。
		"	bnez	%0, 1b\n"		/*判断写是否成功，否则重新执行trylock流程*/
		"2:"
		: "=&r"(l0), "=&r"(tmp1), "=&r"(tmp2), "+A"(*lock)
		: "r"(inc), "r"(mask), "I"(TICKET_SHIFT)
		: "memory");

	return l0 == 0;					/*返回点有两个，一个是按流程执行下来，%0在40行赋值为0，表示成功，否则是在36行返回，它一定不为0，表示失败*/
}

void spin_lock(spinlock_t *lock)
{
	unsigned long inc = 1u << TICKET_SHIFT;
	unsigned long mask = 0xffffu;
	u32 l0, tmp1, tmp2;

	__asm__ __volatile__(
		/* Atomically increment the next ticket. */
		"	amoadd.w.aqrl	%0, %4, %3\n"		/*高16位+1，也就是next+1 ，表示持有这个锁，这里需要注意两点 */
								/*1，%3=%4+%3是一个原则操作;也就是lock变量已经被修改了*/
								/*2, %0保存的是%3之前的值，这也是为啥65行汇编是相等的原因*/

		/* Did we get the lock? */
		"	srli	%1, %0, %6\n"			/*立即数右移16位，也就是获取next，得到一个register 的next，其他spin lock可能会更新此变量 */
		"	and	%1, %1, %5\n"			/*next与mask，和上面理解一样，觉得是个多余操作*/
		"1:	and	%2, %0, %5\n"			/*锁与mask，获取低16位值，也就是owner的值 ，owner值会不断从变量中更新，知道二者相等*/
		"	beq	%1, %2, 2f\n"			/*判断next与owner是否相等，相等则获取到锁*/

		/* If not, then spin on the lock. */
		"	lw	%0, %3\n"			/*无法获取锁时，重新load %3也就是lock，然后进行跳转，直到lock的变化到可以lock的状态*/
		RISCV_ACQUIRE_BARRIER
		"	j	1b\n"
		"2:"
		: "=&r"(l0), "=&r"(tmp1), "=&r"(tmp2), "+A"(*lock)
		: "r"(inc), "r"(mask), "I"(TICKET_SHIFT)
		: "memory");
}

void spin_unlock(spinlock_t *lock)
{
	__smp_store_release(&lock->owner, lock->owner + 1);
}
