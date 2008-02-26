
/*
 *	This piece is probably not GPL'd but in the public domain
 *	instead, or something. Got it from ZMailer's sources, which
 *	are.
 */

/*
 * spdaveb.c -- daveb's new splay tree functions.
 *
 * The functions in this file provide an interface that is nearly
 * the same as the hash library I swiped from mkmf, allowing
 * replacement of one by the other.  Hey, it worked for me!
 *
 * sp_lookup() -- given a key, find a node in a tree.
 * sp_install() -- install an item in the tree, overwriting existing value.
 * sp_fhead() -- fast (non-splay) find the first node in a tree.
 * sp_scan() -- forward scan tree from the head.
 * sp_fnext() -- non-splaying next.
 * sp_stats() -- make char string of stats for a tree.
 *
 * .
 */

#ifndef NULL
#define NULL	0
#endif

/* compare two objects of type spkey_t */
#define SORTED(a, b) ((a) < (b))

#define SP_ALLOC_N 5000

#include	<stdio.h>
#include	<stdlib.h>

#include	"splay.h"
#include	"hmalloc.h"
#include	"config.h"

extern struct spblk * _sp_enq __((struct spblk *, struct sptree *));
			/* insert item into the tree */
extern struct spblk * _sp_deq __((struct spblk **));
			/* return and remove lowest item in subtree */
extern void _splay __((struct spblk *, struct sptree *));
			/* reorganize tree */
extern struct spblk * sp_fhead __((struct sptree *));
			/* fast non-splaying head */
extern struct spblk * sp_fnext __((struct spblk *));
			/* fast non-splaying next */

struct spblk *sp_free_list = NULL;
long sp_allocated = 0;
long sp_entries = 0;

/*
 * Push a node to the free list
 */
 
void sp_free_blk(struct spblk *n)
{
	n->uplink = sp_free_list;
	sp_free_list = n;
	sp_entries--;
}

/*
 * Pop a node off the free list, if free list is empty, allocate some more
 */

struct spblk *sp_alloc_blk(void)
{
	struct spblk *p;
	int i;
	
	if (!sp_free_list) {
		if (dump_splay)
			fprintf(stderr, "splay: allocating %d entries for a total of %ld\n", SP_ALLOC_N, sp_allocated + SP_ALLOC_N);
		for (i = 0; i < SP_ALLOC_N; i++) {
			p = (struct spblk *) hmalloc(sizeof(*p));
			p->uplink = sp_free_list;
			sp_free_list = p;
			sp_allocated++;
		}
	}
	p = sp_free_list;
	sp_free_list = p->uplink;
	sp_entries++;
	
	return p;
}

/*
 * Free the complete free list
 */

void sp_free_freelist(void)
{
	struct spblk *p, *next = sp_free_list;
	
	while (next) {
		p = next;
		next = p->uplink;
		hfree(p);
		sp_entries--;
	}
}

/*
 * sp_lookup() -- given key, find a node in a tree.
 *
 *	Splays the found node to the root.
 */

struct spblk *sp_lookup(spkey_t key, struct sptree *q)
{
	struct spblk *n;
	int c;
	
	/* find node in the tree */
	n = q->root;
	c = ++(q->lkpcmps);
	q->lookups++;
	while(n && (key != n->key)) {
		c++;
		n = SORTED(key, n->key) ? n->leftlink : n->rightlink;
	}
	q->lkpcmps = c;
	
	/* reorganize tree around this node */
	if (n != NULL)
		_splay(n, q);

	return n;
}


/*
 * sp_install() -- install an entry in a tree, overwriting any existing node.
 *
 *	If the node already exists, replace its contents.
 *	If it does not exist, then allocate a new node and fill it in.
 */

struct spblk *sp_install(spkey_t key, struct sptree *q)
{
	struct spblk *n;

	if ((n = sp_lookup(key, q)) == NULL) {
		n = sp_alloc_blk();
		n->key = key;
		n->leftlink = NULL;
		n->rightlink = NULL;
		n->uplink = NULL;
		q->eltscnt += 1;
		(void) _sp_enq(n, q);
	}
	
	return n;
}


/*
 * sp_fhead() --	return the "lowest" element in the tree.
 *
 *	returns a reference to the head event in the event-set q.
 *	avoids splaying but just searches for and returns a pointer to
 *	the bottom of the left branch.
 */

struct spblk *sp_fhead(struct sptree *q)
{
	struct spblk *x;

	if (q == NULL)
		return NULL;
	if (NULL != (x = q->root))
		while(x->leftlink != NULL)
			x = x->leftlink;

	return x;
} /* sp_fhead */


/*
 * sp_scan() -- apply a function to nodes in ascending order.
 *
 *	if n is given, start at that node, otherwise start from
 *	the head.
 */

void sp_scan(int (*f) __((struct spblk *)), struct spblk *n, struct sptree *q)
{
	struct spblk *x, *nextx;

	for(x = n != NULL ? n : sp_fhead(q); x != NULL ; x = nextx) {
		nextx = sp_fnext(x);
		(*f)(x);
	}
}

static void _sp_chop __((struct spblk *));
static void _sp_chop(struct spblk *n)
{
	if (n->rightlink)
		_sp_chop(n->rightlink);
	if (n->leftlink)
		_sp_chop(n->leftlink);
	sp_free_blk(n);
}


/*
 * sp_null() -- bring the splay tree back to the state just after sp_init().
 *
 */

void sp_null(struct sptree *q)
{
	if (q->root == NULL)
		return;
	_sp_chop(q->root);
	q->eltscnt = 0;
	q->lookups = 0;
	q->lkpcmps = 0;
	q->enqs = 0;
	q->enqcmps = 0;
	q->splays = 0;
	q->splayloops = 0;
	q->root = NULL;
}


/*
 * sp_fnext() -- fast return next higer item in the tree, or NULL.
 *
 *	return the successor of n in q, represented as a splay tree.
 *	This is a fast (on average) version that does not splay.
 */

struct spblk *sp_fnext(struct spblk *n)
{
	struct spblk *next;
	struct spblk *x;

	/* a long version, avoids splaying for fast average,
	 * poor amortized bound
	 */

	if (n == NULL)
		return n;

	x = n->rightlink;
	if (x != NULL) {
		while(x->leftlink != NULL)
			x = x->leftlink;
		next = x;
	} else {
		x = n->uplink;
		next = NULL;
		while(x != NULL) {
			if (x->leftlink == n) {
				next = x;
				x = NULL;
			} else {
				n = x;
				x = n->uplink;
			}
		}
	}

	return next;
} /* sp_fnext */


const char *sp_stats(struct sptree *q)
{
	static char buf[ 128 ];
	float llen;
	float elen;
	float sloops;

	if (q == NULL)
		return "";

	llen = q->lookups ? (float)q->lkpcmps / q->lookups : 0;
	elen = q->enqs ? (float)q->enqcmps/q->enqs : 0;
	sloops = q->splays ? (float)q->splayloops/q->splays : 0;

	sprintf(buf, "f(%lld %4.2f) i(%lld %4.2f) s(%lld %4.2f)",
		q->lookups, llen, q->enqs, elen, q->splays, sloops);

	return buf;
}


/*
  spaux.c:  This code implements the following operations on an event-set
  or priority-queue implemented using splay trees:
  
  spdelete(n, q)		n is removed from q.
  
  In the above, n and np are pointers to single items (type
  struct spblk *); q is an event-set (type struct sptree *),
  The type definitions for these are taken
  from file sptree.h.  All of these operations rest on basic
  splay tree operations from file sptree.c.
  
  The basic splay tree algorithms were originally presented in:
  
  Self Adjusting Binary Trees,
  by D. D. Sleator and R. E. Tarjan,
  Proc. ACM SIGACT Symposium on Theory
  of Computing (Boston, Apr 1983) 235-245.
  
  The operations in this package supplement the operations from
  file splay.h to provide support for operations typically needed
  on the pending event set in discrete event simulation.  See, for
  example,
  
  Introduction to Simula 67,
  by Gunther Lamprecht, Vieweg & Sohn, Braucschweig, Wiesbaden, 1981.
  (Chapter 14 contains the relevant discussion.)
  
  Simula Begin,
  by Graham M. Birtwistle, et al, Studentlitteratur, Lund, 1979.
  (Chapter 9 contains the relevant discussion.)
  
  Many of the routines in this package use the splay procedure,
  for bottom-up splaying of the queue.  Consequently, item n in
  delete and item np in all operations listed above must be in the
  event-set prior to the call or the results will be
  unpredictable (eg:  chaos will ensue).
  
  Note that, in all cases, these operations can be replaced with
  the corresponding operations formulated for a conventional
  lexicographically ordered tree.  The versions here all use the
  splay operation to ensure the amortized bounds; this usually
  leads to a very compact formulation of the operations
  themselves, but it may slow the average performance.
  
  Alternative versions based on simple binary tree operations are
  provided (commented out) for head, next, and prev, since these
  are frequently used to traverse the entire data structure, and
  the cost of traversal is independent of the shape of the
  structure, so the extra time taken by splay in this context is
  wasted.
  
  This code was written by:
  Douglas W. Jones with assistance from Srinivas R. Sataluri
  
  Translated to C by David Brower, daveb@rtech.uucp
  
  Thu Oct  6 12:11:33 PDT 1988 (daveb) Fixed _sp_deq, which was broken
 	handling one-node trees.  I botched the pascal translation of
 	a VAR parameter.  Changed interface, so callers must also be
	corrected to pass the node by address rather than value.
  Mon Apr  3 15:18:32 PDT 1989 (daveb)
  	Apply fix supplied by Mark Moraes <moraes@csri.toronto.edu> to
	spdelete(), which dropped core when taking out the last element
	in a subtree -- that is, when the right subtree was empty and
	the leftlink was also null, it tried to take out the leftlink's
	uplink anyway.
 */

/*
 * sp_delete() -- Delete node from a tree.
 *
 *	n is deleted from q; the resulting splay tree has been splayed
 *	around its new root, which is the successor of n
 */

void sp_delete(struct spblk *n, struct sptree *q)
{
	struct spblk *x;

	_splay(n, q);
	x = _sp_deq(&q->root->rightlink);
	if (x == NULL) {	/* empty right subtree */
		q->root = q->root->leftlink;
		if (q->root) q->root->uplink = NULL;
	} else {		/* non-empty right subtree */
		x->uplink = NULL;
		x->leftlink = q->root->leftlink;
		x->rightlink = q->root->rightlink;
		if (x->leftlink != NULL)
			x->leftlink->uplink = x;
		if (x->rightlink != NULL)
			x->rightlink->uplink = x;
		q->root = x;
	}
	sp_free_blk(n);
	q->eltscnt -= 1;
} /* sp_delete */


/*
 *
 *  sptree.c:  The following code implements the basic operations on
 *  an event-set or priority-queue implemented using splay trees:
 *
 *  struct sptree *sp_init(compare)	Make a new tree
 *  struct spblk *_sp_enq(n, q)	Insert n in q after all equal keys.
 *  struct spblk *_sp_deq(np)		Return first key under *np, removing it.
 *  void _splay(n, q)		n (already in q) becomes the root.
 *
 *  In the above, n points to an struct spblk type, while q points to an
 *  struct sptree.
 *
 *  The implementation used here is based on the implementation
 *  which was used in the tests of splay trees reported in:
 *
 *    An Empirical Comparison of Priority-Queue and Event-Set Implementations,
 *	by Douglas W. Jones, Comm. ACM 29, 4 (Apr. 1986) 300-311.
 *
 *  The changes made include the addition of the enqprior
 *  operation and the addition of up-links to allow for the splay
 *  operation.  The basic splay tree algorithms were originally
 *  presented in:
 *
 *	Self Adjusting Binary Trees,
 *		by D. D. Sleator and R. E. Tarjan,
 *			Proc. ACM SIGACT Symposium on Theory
 *			of Computing (Boston, Apr 1983) 235-245.
 *
 *  The enq and enqprior routines use variations on the
 *  top-down splay operation, while the splay routine is bottom-up.
 *  All are coded for speed.
 *
 *  Written by:
 *    Douglas W. Jones
 *
 *  Translated to C by:
 *    David Brower, daveb@rtech.uucp
 *
 * Thu Oct  6 12:11:33 PDT 1988 (daveb) Fixed _sp_deq, which was broken
 *	handling one-node trees.  I botched the pascal translation of
 *	a VAR parameter.
 */

/*
 * sp_init() -- initialize an empty splay tree
 */

struct sptree *sp_init()
{
	struct sptree *q;

	q = (struct sptree *) hmalloc(sizeof(*q));

	q->symbols = NULL;
	q->eltscnt = 0;
	q->lookups = 0;
	q->lkpcmps = 0;
	q->enqs    = 0;
	q->enqcmps = 0;
	q->splays  = 0;
	q->splayloops = 0;
	q->root    = NULL;
	return q;
}

/*
 *  _sp_enq() -- insert item in a tree.
 *
 *  put n in q after all other nodes with the same key; when this is
 *  done, n will be the root of the splay tree representing q, all nodes
 *  in q with keys less than or equal to that of n will be in the
 *  left subtree, all with greater keys will be in the right subtree;
 *  the tree is split into these subtrees from the top down, with rotations
 *  performed along the way to shorten the left branch of the right subtree
 *  and the right branch of the left subtree
 */

struct spblk *_sp_enq(struct spblk *n, struct sptree *q)
{
	struct spblk
			*left,	/* the rightmost node in the left tree */
			*right,	/* the leftmost node in the right tree */
			*next,	/* the root of the unsplit part */
			*temp;

	spkey_t key;

	q->enqs++;
	n->uplink = NULL;
	next = q->root;
	q->root = n;
	if (next == NULL) {	/* trivial enq */
		n->leftlink = NULL;
		n->rightlink = NULL;
	} else {	/* difficult enq */
		key = n->key;
		left = n;
		right = n;

		/*
		 * n's left and right children will hold the right and left
		 * splayed trees resulting from splitting on n->key;
		 * note that the children will be reversed!
		 */

		q->enqcmps++;
		if (!SORTED(next->key, key))
			goto two;

one:	/* assert next->key <= key */

		do {	/* walk to the right in the left tree */
			temp = next->rightlink;
			if (temp == NULL) {
				left->rightlink = next;
				next->uplink = left;
				right->leftlink = NULL;
				goto done;  /* job done, entire tree split */
			}

			q->enqcmps++;
			if (!SORTED(temp->key, key)) {
				left->rightlink = next;
				next->uplink = left;
				left = next;
				next = temp;
				goto two;	/* change sides */
			}

			next->rightlink = temp->leftlink;
			if (temp->leftlink != NULL)
				temp->leftlink->uplink = next;
			left->rightlink = temp;
			temp->uplink = left;
			temp->leftlink = next;
			next->uplink = temp;
			left = temp;
			next = temp->rightlink;
			if (next == NULL) {
				right->leftlink = NULL;
				goto done; /* job done, entire tree split */
			}

			q->enqcmps++;
		} while(SORTED(next->key, key));	/* change sides */

two:	/* assert next->key > key */

		do {	/* walk to the left in the right tree */
			temp = next->leftlink;
			if (temp == NULL) {
				right->leftlink = next;
				next->uplink = right;
				left->rightlink = NULL;
				goto done;  /* job done, entire tree split */
			}

			q->enqcmps++;
			if (SORTED(temp->key, key)) {
				right->leftlink = next;
				next->uplink = right;
				right = next;
				next = temp;
				goto one;	/* change sides */
			}
			next->leftlink = temp->rightlink;
			if (temp->rightlink != NULL)
				temp->rightlink->uplink = next;
			right->leftlink = temp;
			temp->uplink = right;
			temp->rightlink = next;
			next->uplink = temp;
			right = temp;
			next = temp->leftlink;
			if (next == NULL) {
				left->rightlink = NULL;
				goto done;  /* job done, entire tree split */
			}

			q->enqcmps++;
		} while(!SORTED(next->key, key));	/* change sides */

		goto one;

done:	/* split is done, branches of n need reversal */

		temp = n->leftlink;
		n->leftlink = n->rightlink;
		n->rightlink = temp;
	}

	return n;

} /* _sp_enq */


/*
 *  _sp_deq() -- return and remove head node from a subtree.
 *
 *  remove and return the head node from the node set; this deletes
 *  (and returns) the leftmost node from q, replacing it with its right
 *  subtree (if there is one); on the way to the leftmost node, rotations
 *  are performed to shorten the left branch of the tree
 */

struct spblk *_sp_deq(struct spblk **np)
{
	struct spblk
			*deq,		/* one to return */
			*next,       	/* the next thing to deal with */
			*left,      	/* the left child of next */
			*farleft,		/* the left child of left */
			*farfarleft;	/* the left child of farleft */

	if (np == NULL || *np == NULL)
		return NULL;

	next = *np;
	left = next->leftlink;
	if (left == NULL) {
		deq = next;
		*np = next->rightlink;

		if (*np != NULL)
			(*np)->uplink = NULL;
		return deq;
	}

	for (;;) {
		/* next is not it, left is not NULL, might be it */
		farleft = left->leftlink;
		if (farleft == NULL) {
			deq = left;
			next->leftlink = left->rightlink;
			if (left->rightlink != NULL)
				left->rightlink->uplink = next;
			break;
		}

		/* next, left are not it, farleft is not NULL, might be it */
		farfarleft = farleft->leftlink;
		if (farfarleft == NULL) {
			deq = farleft;
			left->leftlink = farleft->rightlink;
			if (farleft->rightlink != NULL)
				farleft->rightlink->uplink = left;
			break;
		}

		/* next, left, farleft are not it, rotate */
		next->leftlink = farleft;
		farleft->uplink = next;
		left->leftlink = farleft->rightlink;
		if (farleft->rightlink != NULL)
			farleft->rightlink->uplink = left;
		farleft->rightlink = left;
		left->uplink = farleft;
		next = farleft;
		left = farfarleft;
	}

	return deq;
} /* _sp_deq */


/*
 *  _splay() -- reorganize the tree.
 *
 *  the tree is reorganized so that n is the root of the
 *  splay tree representing q; results are unpredictable if n is not
 *  in q to start with; q is split from n up to the old root, with all
 *  nodes to the left of n ending up in the left subtree, and all nodes
 *  to the right of n ending up in the right subtree; the left branch of
 *  the right subtree and the right branch of the left subtree are
 *  shortened in the process
 *
 *  this code assumes that n is not NULL and is in q; it can sometimes
 *  detect n not in q and complain
 */

void _splay(struct spblk *n, struct sptree *q)
{
	struct spblk
			*up,	/* points to the node being dealt with */
			*prev,	/* a descendent of up, already dealt with */
			*upup,	/* the parent of up */
			*upupup,	/* the grandparent of up */
			*left,	/* the top of left subtree being built */
			*right;	/* the top of right subtree being built */

	left = n->leftlink;
	right = n->rightlink;
	prev = n;
	up = prev->uplink;

	q->splays++;

	while(up != NULL) {
		q->splayloops++;

		/*
		 * walk up the tree towards the root, splaying all
		 * to the left of n into the left subtree, all to right
		 * into the right subtree
		 */

		upup = up->uplink;
		if (up->leftlink == prev) {	/* up is to the right of n */
			if (upup != NULL && upup->leftlink == up) { /* rotate */
				upupup = upup->uplink;
				upup->leftlink = up->rightlink;
				if (upup->leftlink != NULL)
					upup->leftlink->uplink = upup;
				up->rightlink = upup;
				upup->uplink = up;
				if (upupup == NULL)
					q->root = up;
				else if (upupup->leftlink == upup)
					upupup->leftlink = up;
				else
					upupup->rightlink = up;
				up->uplink = upupup;
				upup = upupup;
			}
			up->leftlink = right;
			if (right != NULL)
				right->uplink = up;
			right = up;

		} else {	/* up is to the left of n */
			if (upup != NULL && upup->rightlink == up) {/* rotate */
				upupup = upup->uplink;
				upup->rightlink = up->leftlink;
				if (upup->rightlink != NULL)
					upup->rightlink->uplink = upup;
				up->leftlink = upup;
				upup->uplink = up;
				if (upupup == NULL)
					q->root = up;
				else if (upupup->rightlink == upup)
					upupup->rightlink = up;
				else
					upupup->leftlink = up;
				up->uplink = upupup;
				upup = upupup;
			}
			up->rightlink = left;
			if (left != NULL)
				left->uplink = up;
			left = up;
		}
		prev = up;
		up = upup;
	}

#ifdef DEBUG
	if (q->root != prev) {
	  /* fprintf(stderr, " *** bug in splay: n not in q *** "); */ abort();
	}
#endif

	n->leftlink = left;
	n->rightlink = right;
	if (left != NULL)
		left->uplink = n;
	if (right != NULL)
		right->uplink = n;
	q->root = n;
	n->uplink = NULL;
} /* _splay */
