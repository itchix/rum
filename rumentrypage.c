/*-------------------------------------------------------------------------
 *
 * rumentrypage.c
 *	  page utilities routines for the postgres inverted index access method.
 *
 *
 * Portions Copyright (c) 2015-2016, Postgres Professional
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/rel.h"
#include "utils/datum.h"

#include "rum.h"

/*
 * Read item pointers with additional information from leaf data page.
 * Information is stored in the same manner as in leaf data pages.
 */
void
rumReadTuple(RumState *rumstate, OffsetNumber attnum,
	IndexTuple itup, ItemPointerData *ipd, Datum *addInfo, bool *addInfoIsNull)
{
	Pointer ptr;
	int nipd = RumGetNPosting(itup), i;
	ItemPointerData ip = {{0,0},0};

	ptr = RumGetPosting(itup);

	if (addInfo && addInfoIsNull)
	{
		for (i = 0; i < nipd; i++)
		{
			ptr = rumDataPageLeafRead(ptr, attnum, &ip, &addInfo[i],
												&addInfoIsNull[i], rumstate);
			ipd[i] = ip;
		}
	}
	else
	{
		for (i = 0; i < nipd; i++)
		{
			ptr = rumDataPageLeafRead(ptr, attnum, &ip, NULL, NULL, rumstate);
			ipd[i] = ip;
		}
	}
}

/*
 * Form a non-leaf entry tuple by copying the key data from the given tuple,
 * which can be either a leaf or non-leaf entry tuple.
 *
 * Any posting list in the source tuple is not copied.  The specified child
 * block number is inserted into t_tid.
 */
static IndexTuple
RumFormInteriorTuple(IndexTuple itup, Page page, BlockNumber childblk)
{
	IndexTuple	nitup;

	if (RumPageIsLeaf(page) && !RumIsPostingTree(itup))
	{
		/* Tuple contains a posting list, just copy stuff before that */
		uint32		origsize = RumGetPostingOffset(itup);

		origsize = MAXALIGN(origsize);
		nitup = (IndexTuple) palloc(origsize);
		memcpy(nitup, itup, origsize);
		/* ... be sure to fix the size header field ... */
		nitup->t_info &= ~INDEX_SIZE_MASK;
		nitup->t_info |= origsize;
	}
	else
	{
		/* Copy the tuple as-is */
		nitup = (IndexTuple) palloc(IndexTupleSize(itup));
		memcpy(nitup, itup, IndexTupleSize(itup));
	}

	/* Now insert the correct downlink */
	RumSetDownlink(nitup, childblk);

	return nitup;
}

/*
 * Entry tree is a "static", ie tuple never deletes from it,
 * so we don't use right bound, we use rightmost key instead.
 */
static IndexTuple
getRightMostTuple(Page page)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

	return (IndexTuple) PageGetItem(page, PageGetItemId(page, maxoff));
}

static bool
entryIsMoveRight(RumBtree btree, Page page)
{
	IndexTuple	itup;
	OffsetNumber attnum;
	Datum		key;
	RumNullCategory category;

	if (RumPageRightMost(page))
		return FALSE;

	itup = getRightMostTuple(page);
	attnum = rumtuple_get_attrnum(btree->rumstate, itup);
	key = rumtuple_get_key(btree->rumstate, itup, &category);

	if (rumCompareAttEntries(btree->rumstate,
				   btree->entryAttnum, btree->entryKey, btree->entryCategory,
							 attnum, key, category) > 0)
		return TRUE;

	return FALSE;
}

/*
 * Find correct tuple in non-leaf page. It supposed that
 * page correctly chosen and searching value SHOULD be on page
 */
static BlockNumber
entryLocateEntry(RumBtree btree, RumBtreeStack *stack)
{
	OffsetNumber low,
				high,
				maxoff;
	IndexTuple	itup = NULL;
	int			result;
	Page		page = BufferGetPage(stack->buffer, NULL, NULL,
									 BGP_NO_SNAPSHOT_TEST);

	Assert(!RumPageIsLeaf(page));
	Assert(!RumPageIsData(page));

	if (btree->fullScan)
	{
		stack->off = FirstOffsetNumber;
		stack->predictNumber *= PageGetMaxOffsetNumber(page);
		return btree->getLeftMostPage(btree, page);
	}

	low = FirstOffsetNumber;
	maxoff = high = PageGetMaxOffsetNumber(page);
	Assert(high >= low);

	high++;

	while (high > low)
	{
		OffsetNumber mid = low + ((high - low) / 2);

		if (mid == maxoff && RumPageRightMost(page))
		{
			/* Right infinity */
			result = -1;
		}
		else
		{
			OffsetNumber attnum;
			Datum		key;
			RumNullCategory category;

			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, mid));
			attnum = rumtuple_get_attrnum(btree->rumstate, itup);
			key = rumtuple_get_key(btree->rumstate, itup, &category);
			result = rumCompareAttEntries(btree->rumstate,
										  btree->entryAttnum,
										  btree->entryKey,
										  btree->entryCategory,
										  attnum, key, category);
		}

		if (result == 0)
		{
			stack->off = mid;
			Assert(RumGetDownlink(itup) != RUM_ROOT_BLKNO);
			return RumGetDownlink(itup);
		}
		else if (result > 0)
			low = mid + 1;
		else
			high = mid;
	}

	Assert(high >= FirstOffsetNumber && high <= maxoff);

	stack->off = high;
	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, high));
	Assert(RumGetDownlink(itup) != RUM_ROOT_BLKNO);
	return RumGetDownlink(itup);
}

/*
 * Searches correct position for value on leaf page.
 * Page should be correctly chosen.
 * Returns true if value found on page.
 */
static bool
entryLocateLeafEntry(RumBtree btree, RumBtreeStack *stack)
{
	Page		page = BufferGetPage(stack->buffer, NULL, NULL,
									 BGP_NO_SNAPSHOT_TEST);
	OffsetNumber low,
				high;

	Assert(RumPageIsLeaf(page));
	Assert(!RumPageIsData(page));

	if (btree->fullScan)
	{
		stack->off = FirstOffsetNumber;
		return TRUE;
	}

	low = FirstOffsetNumber;
	high = PageGetMaxOffsetNumber(page);

	if (high < low)
	{
		stack->off = FirstOffsetNumber;
		return false;
	}

	high++;

	while (high > low)
	{
		OffsetNumber mid = low + ((high - low) / 2);
		IndexTuple	itup;
		OffsetNumber attnum;
		Datum		key;
		RumNullCategory category;
		int			result;

		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, mid));
		attnum = rumtuple_get_attrnum(btree->rumstate, itup);
		key = rumtuple_get_key(btree->rumstate, itup, &category);
		result = rumCompareAttEntries(btree->rumstate,
									  btree->entryAttnum,
									  btree->entryKey,
									  btree->entryCategory,
									  attnum, key, category);
		if (result == 0)
		{
			stack->off = mid;
			return true;
		}
		else if (result > 0)
			low = mid + 1;
		else
			high = mid;
	}

	stack->off = high;
	return false;
}

static OffsetNumber
entryFindChildPtr(RumBtree btree, Page page, BlockNumber blkno, OffsetNumber storedOff)
{
	OffsetNumber i,
				maxoff = PageGetMaxOffsetNumber(page);
	IndexTuple	itup;

	Assert(!RumPageIsLeaf(page));
	Assert(!RumPageIsData(page));

	/* if page isn't changed, we returns storedOff */
	if (storedOff >= FirstOffsetNumber && storedOff <= maxoff)
	{
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, storedOff));
		if (RumGetDownlink(itup) == blkno)
			return storedOff;

		/*
		 * we hope, that needed pointer goes to right. It's true if there
		 * wasn't a deletion
		 */
		for (i = storedOff + 1; i <= maxoff; i++)
		{
			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, i));
			if (RumGetDownlink(itup) == blkno)
				return i;
		}
		maxoff = storedOff - 1;
	}

	/* last chance */
	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, i));
		if (RumGetDownlink(itup) == blkno)
			return i;
	}

	return InvalidOffsetNumber;
}

static BlockNumber
entryGetLeftMostPage(RumBtree btree, Page page)
{
	IndexTuple	itup;

	Assert(!RumPageIsLeaf(page));
	Assert(!RumPageIsData(page));
	Assert(PageGetMaxOffsetNumber(page) >= FirstOffsetNumber);

	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, FirstOffsetNumber));
	return RumGetDownlink(itup);
}

static bool
entryIsEnoughSpace(RumBtree btree, Buffer buf, OffsetNumber off)
{
	Size		itupsz = 0;
	Page		page = BufferGetPage(buf, NULL, NULL, BGP_NO_SNAPSHOT_TEST);

	Assert(btree->entry);
	Assert(!RumPageIsData(page));

	if (btree->isDelete)
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));

		itupsz = MAXALIGN(IndexTupleSize(itup)) + sizeof(ItemIdData);
	}

	if (PageGetFreeSpace(page) + itupsz >= MAXALIGN(IndexTupleSize(btree->entry)) + sizeof(ItemIdData))
		return true;

	return false;
}

/*
 * Delete tuple on leaf page if tuples existed and we
 * should update it, update old child blkno to new right page
 * if child split occurred
 */
static BlockNumber
entryPreparePage(RumBtree btree, Page page, OffsetNumber off)
{
	BlockNumber ret = InvalidBlockNumber;

	Assert(btree->entry);
	Assert(!RumPageIsData(page));

	if (btree->isDelete)
	{
		Assert(RumPageIsLeaf(page));
		PageIndexTupleDelete(page, off);
	}

	if (!RumPageIsLeaf(page) && btree->rightblkno != InvalidBlockNumber)
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));

		RumSetDownlink(itup, btree->rightblkno);
		ret = btree->rightblkno;
	}

	btree->rightblkno = InvalidBlockNumber;

	return ret;
}

/*
 * Place tuple on page and fills WAL record
 */
static void
entryPlaceToPage(RumBtree btree, Buffer buf, OffsetNumber off, XLogRecData **prdata)
{
	Page		page = BufferGetPage(buf, NULL, NULL, BGP_NO_SNAPSHOT_TEST);
	OffsetNumber placed;

	/* these must be static so they can be returned to caller */
	static XLogRecData rdata[3];
	static rumxlogInsert data;

	*prdata = rdata;
	data.updateBlkno = entryPreparePage(btree, page, off);

	placed = PageAddItem(page, (Item) btree->entry, IndexTupleSize(btree->entry), off, false, false);
	if (placed != off)
		elog(ERROR, "failed to add item to index page in \"%s\"",
			 RelationGetRelationName(btree->index));

	data.node = btree->index->rd_node;
	data.blkno = BufferGetBlockNumber(buf);
	data.offset = off;
	data.nitem = 1;
	data.isDelete = btree->isDelete;
	data.isData = false;
	data.isLeaf = RumPageIsLeaf(page) ? TRUE : FALSE;

	/*
	 * For incomplete-split tracking, we need updateBlkno information and the
	 * inserted item even when we make a full page image of the page, so put
	 * the buffer reference in a separate XLogRecData entry.
	 */
	rdata[0].buffer = buf;
	rdata[0].buffer_std = TRUE;
	rdata[0].data = NULL;
	rdata[0].len = 0;
	rdata[0].next = &rdata[1];

	rdata[1].buffer = InvalidBuffer;
	rdata[1].data = (char *) &data;
	rdata[1].len = sizeof(rumxlogInsert);
	rdata[1].next = &rdata[2];

	rdata[2].buffer = InvalidBuffer;
	rdata[2].data = (char *) btree->entry;
	rdata[2].len = IndexTupleSize(btree->entry);
	rdata[2].next = NULL;

	btree->entry = NULL;
}

/*
 * Place tuple and split page, original buffer(lbuf) leaves untouched,
 * returns shadow page of lbuf filled new data.
 * Tuples are distributed between pages by equal size on its, not
 * an equal number!
 */
static Page
entrySplitPage(RumBtree btree, Buffer lbuf, Buffer rbuf, OffsetNumber off, XLogRecData **prdata)
{
	OffsetNumber i,
				maxoff,
				separator = InvalidOffsetNumber;
	Size		totalsize = 0;
	Size		lsize = 0,
				size;
	char	   *ptr;
	IndexTuple	itup,
				leftrightmost = NULL;
	Page		page;
	Page		lpage = PageGetTempPageCopy(BufferGetPage(lbuf, NULL, NULL,
														 BGP_NO_SNAPSHOT_TEST));
	Page		rpage = BufferGetPage(rbuf, NULL, NULL, BGP_NO_SNAPSHOT_TEST);
	Size		pageSize = PageGetPageSize(lpage);

	/* these must be static so they can be returned to caller */
	static XLogRecData rdata[2];
	static rumxlogSplit data;
	static char tupstore[2 * BLCKSZ];

	*prdata = rdata;
	data.leftChildBlkno = (RumPageIsLeaf(lpage)) ?
		InvalidOffsetNumber : RumGetDownlink(btree->entry);
	data.updateBlkno = entryPreparePage(btree, lpage, off);

	maxoff = PageGetMaxOffsetNumber(lpage);
	ptr = tupstore;

	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		if (i == off)
		{
			size = MAXALIGN(IndexTupleSize(btree->entry));
			memcpy(ptr, btree->entry, size);
			ptr += size;
			totalsize += size + sizeof(ItemIdData);
		}

		itup = (IndexTuple) PageGetItem(lpage, PageGetItemId(lpage, i));
		size = MAXALIGN(IndexTupleSize(itup));
		memcpy(ptr, itup, size);
		ptr += size;
		totalsize += size + sizeof(ItemIdData);
	}

	if (off == maxoff + 1)
	{
		size = MAXALIGN(IndexTupleSize(btree->entry));
		memcpy(ptr, btree->entry, size);
		ptr += size;
		totalsize += size + sizeof(ItemIdData);
	}

	RumInitPage(rpage, RumPageGetOpaque(lpage)->flags, pageSize);
	RumInitPage(lpage, RumPageGetOpaque(rpage)->flags, pageSize);

	ptr = tupstore;
	maxoff++;
	lsize = 0;

	page = lpage;
	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		itup = (IndexTuple) ptr;

		if (lsize > totalsize / 2)
		{
			if (separator == InvalidOffsetNumber)
				separator = i - 1;
			page = rpage;
		}
		else
		{
			leftrightmost = itup;
			lsize += MAXALIGN(IndexTupleSize(itup)) + sizeof(ItemIdData);
		}

		if (PageAddItem(page, (Item) itup, IndexTupleSize(itup), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
			elog(ERROR, "failed to add item to index page in \"%s\"",
				 RelationGetRelationName(btree->index));
		ptr += MAXALIGN(IndexTupleSize(itup));
	}

	btree->entry = RumFormInteriorTuple(leftrightmost, lpage,
										BufferGetBlockNumber(lbuf));

	btree->rightblkno = BufferGetBlockNumber(rbuf);

	data.node = btree->index->rd_node;
	data.rootBlkno = InvalidBlockNumber;
	data.lblkno = BufferGetBlockNumber(lbuf);
	data.rblkno = BufferGetBlockNumber(rbuf);
	data.separator = separator;
	data.nitem = maxoff;
	data.isData = FALSE;
	data.isLeaf = RumPageIsLeaf(lpage) ? TRUE : FALSE;
	data.isRootSplit = FALSE;

	rdata[0].buffer = InvalidBuffer;
	rdata[0].data = (char *) &data;
	rdata[0].len = sizeof(rumxlogSplit);
	rdata[0].next = &rdata[1];

	rdata[1].buffer = InvalidBuffer;
	rdata[1].data = tupstore;
	rdata[1].len = MAXALIGN(totalsize);
	rdata[1].next = NULL;

	return lpage;
}

/*
 * return newly allocated rightmost tuple
 */
IndexTuple
rumPageGetLinkItup(Buffer buf)
{
	IndexTuple	itup,
				nitup;
	Page		page = BufferGetPage(buf, NULL, NULL, BGP_NO_SNAPSHOT_TEST);

	itup = getRightMostTuple(page);
	nitup = RumFormInteriorTuple(itup, page, BufferGetBlockNumber(buf));

	return nitup;
}

/*
 * Fills new root by rightest values from child.
 * Also called from rumxlog, should not use btree
 */
void
rumEntryFillRoot(RumBtree btree, Buffer root, Buffer lbuf, Buffer rbuf)
{
	Page		page;
	IndexTuple	itup;

	page = BufferGetPage(root, NULL, NULL, BGP_NO_SNAPSHOT_TEST);

	itup = rumPageGetLinkItup(lbuf);
	if (PageAddItem(page, (Item) itup, IndexTupleSize(itup), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
		elog(ERROR, "failed to add item to index root page");
	pfree(itup);

	itup = rumPageGetLinkItup(rbuf);
	if (PageAddItem(page, (Item) itup, IndexTupleSize(itup), InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
		elog(ERROR, "failed to add item to index root page");
	pfree(itup);
}

/*
 * Set up RumBtree for entry page access
 *
 * Note: during WAL recovery, there may be no valid data in rumstate
 * other than a faked-up Relation pointer; the key datum is bogus too.
 */
void
rumPrepareEntryScan(RumBtree btree, OffsetNumber attnum,
					Datum key, RumNullCategory category,
					RumState *rumstate)
{
	memset(btree, 0, sizeof(RumBtreeData));

	btree->index = rumstate->index;
	btree->rumstate = rumstate;

	btree->findChildPage = entryLocateEntry;
	btree->isMoveRight = entryIsMoveRight;
	btree->findItem = entryLocateLeafEntry;
	btree->findChildPtr = entryFindChildPtr;
	btree->getLeftMostPage = entryGetLeftMostPage;
	btree->isEnoughSpace = entryIsEnoughSpace;
	btree->placeToPage = entryPlaceToPage;
	btree->splitPage = entrySplitPage;
	btree->fillRoot = rumEntryFillRoot;

	btree->isData = FALSE;
	btree->searchMode = FALSE;
	btree->fullScan = FALSE;
	btree->isBuild = FALSE;

	btree->entryAttnum = attnum;
	btree->entryKey = key;
	btree->entryCategory = category;
	btree->isDelete = FALSE;
}