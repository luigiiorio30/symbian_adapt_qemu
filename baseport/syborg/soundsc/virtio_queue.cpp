/*
* This component and the accompanying materials are made available
* under the terms of the License "Eclipse Public License v1.0"
* which accompanies this distribution, and is available
* at the URL "http://www.eclipse.org/legal/epl-v10.html".
*
* Initial Contributors:
* Accenture Ltd
*
* Contributors:
*
* Description: This file is a part of sound driver for Syborg adaptation.
*
*/

#include "virtio_queue.h"

#define ENABLE_QEMU_AUDIO_MODEL_BUG_WORKAROUND
#define ENABLE_QEMU_VIRTIO_CLEANUP_BUG_WORKAROUND
#define PHYS_ADDR(x) Epoc::LinearToPhysical( reinterpret_cast<TUint32>(x))
	
namespace VirtIo
{


DQueue::DQueue( MIo& aVirtIo, TUint aId, TUint aCount ) 
	: iVirtIo( aVirtIo ), iId( aId ), iCount( aCount )
	{
	}
	
TInt DQueue::Construct()
	{
	TInt r = AllocQueue();
	if (r!=KErrNone)
		{ return r; }
	PreInitQueue();
	iVirtIo.SetQueueBase( iId, iDesc );
	PostInitQueue();
	return KErrNone;
	}

void DQueue::Wipe()
	{
	if ( iChunk )
		{
		iChunk->Close( NULL );
		iChunk = NULL;
		}
#ifndef ENABLE_QEMU_VIRTIO_CLEANUP_BUG_WORKAROUND		
	if ( iPhysAddr )
		{ Epoc::FreePhysicalRam( iPhysAddr ); }
#endif
	iPhysAddr = 0;
	}
	
DQueue::~DQueue()
	{
	Wipe();
	}	
	
//
// Implementation here is that descs are allocated from the one starting with lowest index, 
TInt DQueue::AddBuf( const TAddrLen aScatterList[], TUint32 aOutNum, TUint32 aInNum, Token aToken)
	{
	SYBORG_VIRTIO_DEBUG("AddBuf Q%x %x l%x %x,%x %x",
		iId, aScatterList[0].iAddr, aScatterList[0].iLen, aOutNum, aInNum, aToken );
	TUint outLeft = aOutNum;
	TUint inLeft = aInNum;
	TInt first = -1;
	TInt last = -1;
	TUint totalLen = 0;
	// @TODO maintain freedescs counter to know if the below is going to success from the outset.
	// alloc and initialise descs
	for ( TUint i = 0, j = 0; (i < iCount) && (outLeft+inLeft); ++i )
		{
		if (iDesc[i].iFlags == KFreeDescriptorMarker)
			{
			if (first<0)
				{ first = i; }
			iDesc[i].iFlags = ((outLeft)? 0 : TRingDesc::EFlagWrite);
			iDesc[i].iNext = KFreeDescriptorMarker;
			iDesc[i].iAddr = aScatterList[j].iAddr;
			iDesc[i].iLen = aScatterList[j].iLen;
			totalLen += aScatterList[j].iLen;
			iToken[i].iToken = aToken;
			iToken[i].iTotalLen = 0;
			if ( last >=0 )
				{ 
				iDesc[last].iNext = i; 
				iDesc[last].iFlags |= TRingDesc::EFlagNext;
				}
			last = i;
			if (outLeft) 
				{ --outLeft; }
			else 
				{ --inLeft; }
			j++;
			}
		}
	if (outLeft+inLeft) // rollback descs if not all could have been claimed
		{
		if (first>=0)
			{ FreeDescs(first); }
		return KErrNotReady;
		}
	iToken[first].iTotalLen = totalLen;
	
	// fill a slot in avail ring
	iAvail->iRing[Slot(iAvail->iIdx)] = first;
	++iAvail->iIdx;
	
	return KErrNone;
	}	
	
// bases on the assumption that the lowest desc index with given aToken value is used (see addBuf)
// @todo make sure the buffer is not yet posted	
TInt DQueue::DetachBuf( Token aToken )
	{
	TUint myDescId = KFreeDescriptorMarker;
	for ( TIdx i = iNextAvailToSync; i != iAvail->iIdx ; ++i )
		{
		TUint availSlot = Slot( i );
		TUint descId = iAvail->iRing[availSlot];
		if ( descId < iCount )
			{
			if (iToken[descId].iToken == aToken)
				{
				myDescId = descId;
				break;
				}
			}
		}
	if ( myDescId != KFreeDescriptorMarker )
		{ return KErrNotFound; }
	FreeDescs( myDescId );
	return KErrNone;
	}	
	
Token DQueue::GetBuf( TUint& aLen )
	{
	TIdx usedIdx = iUsed->iIdx;
	ASSERT( ((TIdx)iNextUsedToRead) <= usedIdx );
	if (usedIdx == ((TIdx)iNextUsedToRead))
		{ return 0; }
	TUint nextUsedSlot = Slot(iNextUsedToRead);
	TUint len = iUsed->iRing[nextUsedSlot].iLen;
	TUint descId = iUsed->iRing[nextUsedSlot].iId;
	ASSERT(descId<iCount);
	Token token = iToken[descId].iToken;	
	TUint orderedLen = iToken[descId].iTotalLen;	
	SYBORG_VIRTIO_DEBUG( "GetBuf Q%x %x ..%x t%x D%x L%x OL%x", iId, iNextUsedToRead, usedIdx, token, descId, len, orderedLen );

	++iNextUsedToRead;
	FreeDescs( descId );
	
#ifdef ENABLE_QEMU_AUDIO_MODEL_BUG_WORKAROUND	
	aLen = len?len:orderedLen; // @TODO kind of a hack to solve virtio-audio's failure to report len by syborg on the side of qemu
#endif
	return token;
	}	
	
TInt DQueue::Processing()
	{ return  ((TIdx)(iAvail->iIdx - iFirstEverToSync)) 
		- ((TIdx)(iUsed->iIdx - iFirstEverToRead)); }	
	
TInt DQueue::Completed()
	{ return  ((TIdx)(iUsed->iIdx - iNextUsedToRead)); }		

void DQueue::Sync()
	{
	SYBORG_VIRTIO_DEBUG("Sync Q%d, %x..%x",
		iId, iNextAvailToSync, iAvail->iIdx );	
	if ( ((TIdx)iNextAvailToSync) == iAvail->iIdx)
		{ return; }
	DumpAvailPending();

	iNextAvailToSync = iAvail->iIdx;
	iVirtIo.PostQueue( iId );
	}
	
void DQueue::DumpUsedPending()
	{
	for (TIdx i = iNextUsedToRead; i != iUsed->iIdx; ++i )
		{ DumpUsed( Slot(i) ); }
	}	
	
void DQueue::DumpUsed(TUint usedSlot)
	{
	SYBORG_VIRTIO_DEBUG("Usedslot = %x, aLen = %x, descId=%x", usedSlot, (TUint32) iUsed->iRing[usedSlot].iLen, iUsed->iRing[usedSlot].iId);
	TUint descId = iUsed->iRing[usedSlot].iId;
	DumpDescs( descId );
	}	
	
void DQueue::DumpAvailPending()
	{
	for (TIdx i = iNextAvailToSync; i != iAvail->iIdx; ++i )
		{ DumpAvail( Slot(i) ); }
	}
	
void DQueue::DumpAvail(TUint availSlot)
	{
	SYBORG_VIRTIO_DEBUG("Q%d, availslot = %x", iId, availSlot);
	TUint descId = iAvail->iRing[availSlot];
	DumpDescs( descId );
	}
	
void DQueue::DumpDescs(TUint descId )
	{
	do {
		TRingDesc& d = iDesc[descId];
		SYBORG_VIRTIO_DEBUG(" Desc %x,addr %x, len %x, flags %x, next %x",
			(TUint32)descId, (TUint32)d.iAddr, (TUint32)d.iLen, (TUint32)d.iFlags, (TUint32)d.iNext );
		if ((d.iFlags&TRingDesc::EFlagNext)==0)
			{ break; }
		descId = d.iNext;
		} while (ETrue);
	}
	
void DQueue::DumpAll()
	{
	DumpAvailPending();
	DumpUsedPending();
	}	
		
void DQueue::FreeDescs( TUint firstDescIdx )
	{
	TInt i = firstDescIdx;
	Token token = iToken[firstDescIdx].iToken;
	while (i>=0)
		{
		ASSERT( ( ((TUint)i) < iCount ) );
		TUint flags = iDesc[i].iFlags;
		ASSERT( flags != KFreeDescriptorMarker );
		iDesc[i].iFlags = KFreeDescriptorMarker;
		ASSERT( iToken[i].iToken == token );
		iToken[i].iToken = 0;
		iToken[i].iTotalLen = 0;
		i = (flags&TRingDesc::EFlagNext)
			? iDesc[i].iNext : -1;
		}
	}		
	
TUint8* DQueue::AllocMem( TUint aSize )
	{
	TInt r = KErrNone;
	
#ifdef ENABLE_QEMU_VIRTIO_CLEANUP_BUG_WORKAROUND		
	// note this is second part of workaround that deals
	// with the issue that memory once assigned to queuebase cannot be 
	// changed,
	// if queuebase != 0 this is because it was set when the driver
	// was loaded previous time

	iPhysAddr = (TUint32) iVirtIo.GetQueueBase( iId );
	iPhysMemReallyAllocated = (iPhysAddr == 0);
	if (iPhysMemReallyAllocated)
		{ r = Epoc::AllocPhysicalRam(aSize, iPhysAddr, 0 ); }

#endif

	if (r!=KErrNone ) 
		{ return NULL; }
	
	r = DPlatChunkHw::New( iChunk, iPhysAddr, aSize, 
		EMapAttrSupRw | EMapAttrL2Uncached | EMapAttrL1Uncached );

	if (r!=KErrNone ) 
		{
		if ( iPhysMemReallyAllocated )
			{ Epoc::FreePhysicalRam( iPhysAddr, aSize ); }
		iChunk->Close( NULL );
		return NULL;
		}

	ASSERT( r == KErrNone );
	return reinterpret_cast<TUint8*>( iChunk->LinearAddress() );
	}	
	
TInt DQueue::AllocQueue()
	{
	iDescSize = iCount * sizeof(TRingDesc);
	iAvailSize = sizeof(TRingAvail) + (iCount-1) * sizeof(((TRingAvail*)0)->iRing[0]);
	iTokenSize = iCount * sizeof(TTransactionInfo);
	TUint usedOffset = Align( iDescSize +  iAvailSize, KVirtIoAlignment );
	TUint iUsedSize = sizeof(TRingUsed) + (iCount-1) * sizeof(((TRingUsed*)0)->iRing[0]);
	TUint size = usedOffset + iUsedSize;
	TUint8* iMemAligned;

	iMemAligned = AllocMem( size );
	
	if (!iMemAligned)
		{ return KErrNoMemory; }
	
	iDesc = reinterpret_cast<TRingDesc*>( iMemAligned );
	iAvail = reinterpret_cast<TRingAvail*>( iMemAligned + iDescSize );
	iUsed = reinterpret_cast<TRingUsed*>( iMemAligned + usedOffset );
	iToken = reinterpret_cast<TTransactionInfo*>( Kern::Alloc( iTokenSize ) );
	SYBORG_VIRTIO_DEBUG("DQueue %d, Virt iDesc=%x,iAvail=%x,iToken=%x,iUsed=%x",
		iId, iDesc, iAvail, iToken, iUsed );
	SYBORG_VIRTIO_DEBUG("DQueue %d, Phys iDesc=%x, iUsed=%x",
		iId, PHYS_ADDR(iDesc), PHYS_ADDR(iUsed) );
	ASSERT( ((PHYS_ADDR(iUsed)-PHYS_ADDR(iDesc))) == ((TUint32)((TUint8*)iUsed-(TUint8*)iDesc)) );
	return KErrNone;
	}	
	
void DQueue::PreInitQueue()
	{
	memset(iDesc, -1, iDescSize );
	memset( ((TUint8*) iAvail) + 4, -1, iDescSize - 4 );
	memset( ((TUint8*) iUsed) + 4, -1, iDescSize - 4 );
	
	iAvail->iFlags = 0; // no notifications from control queue
	iUsed->iFlags = 0;
	if ( iPhysMemReallyAllocated )
		{
		iAvail->iIdx = 0;
		iUsed->iIdx = 0;
		}
	}	
	
void DQueue::PostInitQueue()
	{
	iFirstEverToSync = iNextAvailToSync = iAvail->iIdx;
	iFirstEverToRead = iNextUsedToRead = iUsed->iIdx;
	}		


} // namespace VirtIo
