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

#include "virtio_audio.h"

namespace VirtIo
{
namespace Audio
{

DControl::~DControl()
	{
	if (iCmdMem)
		{ Kern::Free( iCmdMem ); }
	}

TInt DControl::Construct()
	{
	TUint cmdSize = sizeof(iCmd[0])*KCmdMaxBufCount;
	TUint size = cmdSize + sizeof(*iBufferInfo);
	// as we are going to align the allocated memory
	// we add extra alignment size minus 1
	iCmdMem = reinterpret_cast<TUint8*>( Kern::Alloc( size + sizeof(iCmd[0])-1 ) );
	if (!iCmdMem)
		{ return KErrNoMemory; }
		
	// let us align the memory address 
	// note: sizeof(iCmd[0]) is a power of 2
	ASSERT( sizeof(iCmd[0]) == POW2ALIGN(sizeof(iCmd[0])) );
	TUint8* alignedMem = iCmdMem 
		+ ( (-(TUint32)iCmdMem)&(sizeof(iCmd[0])-1) ); // adding missing amount of bytes
	
	iCmd = reinterpret_cast<TCommandPadded*>( alignedMem );
	iBufferInfo = reinterpret_cast<TBufferInfo*>( alignedMem + cmdSize );
	
	for (TUint i = 0; i< KCmdMaxBufCount; ++i)
		{
		iCmd[i].iStream = iDataQueueId - 1;
		}	
	iCmd[0].iCommand = Audio::ECmdSetEndian;
	iCmd[1].iCommand = Audio::ECmdSetChannels;
	iCmd[2].iCommand = Audio::ECmdSetFormat;
	iCmd[3].iCommand = Audio::ECmdSetFrequency;
	iCmd[4].iCommand = Audio::ECmdInit;
	iCmd[5].iCommand = Audio::ECmdRun;
	iCmd[5].iArg = Audio::EDoRun; //start stream
	iCmd[6].iCommand = Audio::ECmdRun;
	iCmd[6].iArg = Audio::EDoStop; //stop stream		
	iCmd[7].iCommand = Audio::ECmdRun;
	iCmd[7].iArg = Audio::EDoStop; //kind of pause
	iCmd[8].iCommand = Audio::ECmdRun;
	iCmd[8].iArg = Audio::EDoRun; //kind of resume
	return KErrNone;
	}

TInt DControl::Setup( StreamDirection aDirection, TInt aChannelNum, 
	FormatId aFormat, TInt aFreq)
	{
	iCmd[1].iArg = aChannelNum;
	iCmd[2].iArg = aFormat;
	iCmd[3].iArg = aFreq;		
	iCmd[4].iArg = iDirection = aDirection;		
	AddCommand(&iCmd[0],(Token)0);
	AddCommand(&iCmd[1],(Token)1);
	AddCommand(&iCmd[2],(Token)2);
	AddCommand(&iCmd[3],(Token)3);
	AddCommand(&iCmd[4],(Token)4, iBufferInfo, sizeof(*iBufferInfo) );
	ControlQueue().Sync();
	return KErrNone;
	}
	
void DControl::AddCommand( TCommandPadded* aCmd, Token aToken )
	{
	TAddrLen list;
	list.iLen = sizeof(TCommand);
	list.iAddr = Epoc::LinearToPhysical((TUint32)aCmd);
	SYBORG_VIRTIO_DEBUG("AddCommand %x %x %x", aCmd->iCommand, aCmd->iStream, aCmd->iArg);
	ControlQueue().AddBuf(&list, 1, 0, aToken );
	}

void DControl::AddCommand( TCommandPadded* aCmd, Token aToken, 
	TAny* aMem, TUint aSize )
	{
	TAddrLen list[2];
	list[0].iLen = sizeof(TCommand);
	list[0].iAddr = Epoc::LinearToPhysical((TUint32)aCmd);
	list[1].iLen = aSize;
	list[1].iAddr = Epoc::LinearToPhysical((TUint32)aMem);
	ControlQueue().AddBuf(list, 1, 1, aToken );
	}
	
	
// Waits until device processes all pending requests
// there would be no need to have it here at all 
// if there was no bug in qemu:
// once you send stop command the buffers processing stops... but the buffers are never returned.

void DControl::WaitForCompletion()
	{
	SYBORG_VIRTIO_DEBUG("DControl::WaitForCompletion : {");

	TInt st = Kern::PollingWait( &DControl::CheckProcessing, this, 10, 100 );
	ASSERT ( (st == KErrNone) && "Polling problem" )
	
	SYBORG_VIRTIO_DEBUG("DControlWaitForCompletion : }");
	}

TBool DControl::CheckProcessing( TAny* aSelf )
	{
	DControl* self = reinterpret_cast<DControl*>( aSelf );
	return self->DataQueue().Processing() == 0;
	}
	
void DControl::AddCommand( Command aCmd )
	{
	TUint idx = aCmd;
	if (aCmd == EStop)
		{
		// due to bug on qemu's side we need to stop sending buffers
		// and wait for all pending buffers to get filled...
		WaitForCompletion();
		}
	AddCommand(&iCmd[idx], (Token)idx );
	}

TInt DControl::SendDataBuffer( TAny* virtaulAddr, TUint aSize, Token aToken )
	{
	TAddrLen sgl[KMaxSGLItemCountPerAudioBuffer];
	TUint sglCount = KMaxSGLItemCountPerAudioBuffer;
	TInt st = LinearToSGL(virtaulAddr, aSize, sgl, sglCount );
	ASSERT( st != KErrNoMemory ); // failure means our fixed lenght sgl table is too small
	if (st!=KErrNone)
		{ return st; }
	st = DataQueue().AddBuf( sgl, 
		iDirection == EDirectionPlayback ? sglCount : 0,
		iDirection == EDirectionRecord ? sglCount : 0,
		aToken );
	if (st!=KErrNone)
		{ return st; }
	DataQueue().Sync();
	return KErrNone;
	}
	

} // namespace Audio
} // namespace VirtIo
