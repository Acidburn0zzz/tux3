/*
 * Buffer management
 */

#include "tux3.h"

#ifndef trace
#define trace trace_on
#endif

/*
 * FIXME: Setting delta is not atomic with dirty for this buffer_head,
 * so this maps the delta 0-3 to 1-4. And 0 is used to tell "delta is
 * not set yet"
 */
#define DELTA_STATES	(((BUFFER_DIRTY_STATES << 1) - 1) << BH_PrivateStart)
#define DELTA_MASK	((unsigned long)DELTA_STATES)
#define DELTA_VAL(x)	((unsigned long)((x) + 1) << BH_PrivateStart)

/*
 * FIXME: this is hack to save delta to linux buffer_head.
 * Inefficient, and this is not atomic with dirty bit change. And this
 * may not work on all arch (If set_bit() and cmpxchg() is not
 * exclusive, this has race).
 */
static void tux3_set_bufdelta(struct buffer_head *buffer, int delta)
{
	unsigned long state, old_state;

	delta = delta_when(delta);

	state = buffer->b_state;
	for (;;) {
		old_state = state;
		state = (old_state & ~DELTA_MASK) | DELTA_VAL(delta);
		state = cmpxchg(&buffer->b_state, old_state, state);
		if (state == old_state)
			break;
	}
}

static void tux3_clear_bufdelta(struct buffer_head *buffer)
{
	unsigned long state, old_state;

	state = buffer->b_state;
	for (;;) {
		old_state = state;
		state = (old_state & ~DELTA_MASK);
		state = cmpxchg(&buffer->b_state, old_state, state);
		if (state == old_state)
			break;
	}
}

static int tux3_bufdelta(struct buffer_head *buffer)
{
	int delta;

	assert(buffer_dirty(buffer));
	while (1) {
		delta = (buffer->b_state & DELTA_MASK) >> BH_PrivateStart;
		if (delta)
			return delta - 1;
		/* The delta is not yet set. Retry */
		cpu_relax();
	}
}

/* Can we modify buffer from delta */
int buffer_can_modify(struct buffer_head *buffer, unsigned delta)
{
	/* If true, buffer is still not stabilized. We can modify. */
	if (tux3_bufdelta(buffer) == delta_when(delta))
		return 1;
	/* The buffer may already be in stabilized stage for backend. */
	return 0;
}

/* FIXME: we should rewrite with own buffer management */
void tux3_set_buffer_dirty_list(struct buffer_head *buffer, int delta,
				struct list_head *head)
{
	struct inode *inode = buffer_inode(buffer);
	struct address_space *mapping = inode->i_mapping;
	struct address_space *buffer_mapping = buffer->b_page->mapping;

	mark_buffer_dirty(buffer);

	if (!mapping->assoc_mapping)
		mapping->assoc_mapping = buffer_mapping;
	else
		BUG_ON(mapping->assoc_mapping != buffer_mapping);

	if (!buffer->b_assoc_map) {
		spin_lock(&buffer_mapping->private_lock);
		list_move_tail(&buffer->b_assoc_buffers, head);
		buffer->b_assoc_map = mapping;
		/* FIXME: hack for save delta */
		tux3_set_bufdelta(buffer, delta);
		spin_unlock(&buffer_mapping->private_lock);
	}
}

/* FIXME: we should rewrite with own buffer management */
void tux3_set_buffer_dirty(struct buffer_head *buffer, int delta)
{
	struct dirty_buffers *dirty = inode_dirty_heads(buffer_inode(buffer));
	struct list_head *head = dirty_head_when(dirty, delta);
	tux3_set_buffer_dirty_list(buffer, delta, head);
}

void init_dirty_buffers(struct dirty_buffers *dirty)
{
	for (int i = 0; i < BUFFER_DIRTY_STATES; i ++)
		INIT_LIST_HEAD(&dirty->heads[i]);
}

#include "buffer_writeback.c"
#include "buffer_fork.c"